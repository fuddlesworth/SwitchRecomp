/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* nvhost-nvdec / nvhost-vic (host1x channels).
   We parse the host1x stream, decode NVDEC H.264 jobs with OpenH264 (SPS/PPS get rebuilt from the
   picture setup the game fills in) and VIC jobs copy the last decoded picture into the output surface.
   Struct layouts / method offsets come from NVIDIA's open-gpu-doc (nvdec_drv.h, clc5b0.h, VIC). */
#include "rt.h"
#ifndef SWR_NO_H264
#include "wels/codec_api.h"
#endif

u64 nvmap_addr(u32 h); u64 nvmap_addr_by_id(u32 id);
u32 syncpt_read(u32 id); void syncpt_incr(u32 id);

/* ------------------------------------------------------------ IOVA (what the video engines see through the SMMU) */
typedef struct { u32 iova, size; u64 addr; u32 h; } IoMap;
static IoMap g_iom[1024]; static int g_niom; static u32 g_iova_next=0x10000000u; static PMutex g_hlk; static int g_hinit;
static void h_init(void){ if(!g_hinit){ pm_init(&g_hlk); g_hinit=1; } }
static u32 iova_map(u32 h){
  u64 a=nvmap_addr(h); if(!a) a=nvmap_addr_by_id(h); if(!a) return 0;
  for(int i=0;i<g_niom;i++) if(g_iom[i].h==h && g_iom[i].addr==a) return g_iom[i].iova;
  extern u32 nvmap_size(u32 h); u32 sz=nvmap_size(h); if(!sz) sz=0x100000;
  if(g_niom>=1024) return 0;
  u32 base=g_iova_next; g_iova_next+=(sz+0xFFFFu)&~0xFFFFu; g_iova_next+=0x10000u;
  g_iom[g_niom].iova=base; g_iom[g_niom].size=sz; g_iom[g_niom].addr=a; g_iom[g_niom].h=h; g_niom++;
  return base; }
/* 40-bit iova (engines get it >>8) -> host pointer */
static u8* iova_ptr(u64 iova, u64 len){
  for(int i=0;i<g_niom;i++){ IoMap* m=&g_iom[i]; if(iova>=m->iova && iova+len<=(u64)m->iova+m->size) return GP(m->addr+(iova-m->iova)); }
  return NULL; }

/* ------------------------------------------------------------ H.264: SPS/PPS rebuilt from nvdec_h264_pic_s */
typedef struct { u8* p; u32 cap, bytes; u32 acc; int nb; } BW;
static void bw_bit(BW* w, u32 b){ w->acc=(w->acc<<1)|(b&1); if(++w->nb==8){ if(w->bytes<w->cap) w->p[w->bytes++]=(u8)w->acc; w->acc=0; w->nb=0; } }
static void bw_u(BW* w, u32 v, int n){ for(int i=n-1;i>=0;i--) bw_bit(w,(v>>i)&1); }
static void bw_ue(BW* w, u32 v){ u32 x=v+1; int n=0; while((x>>n)>1) n++; for(int i=0;i<n;i++) bw_bit(w,0); bw_u(w,x,n+1); }
static void bw_se(BW* w, i32 v){ bw_ue(w, v>0? (u32)(2*v-1) : (u32)(-2*v)); }
static void bw_trailing(BW* w){ bw_bit(w,1); while(w->nb) bw_bit(w,0); }
/* start code + emulation prevention bytes */
static u32 nal_out(u8* dst, u32 cap, u8 hdr, const u8* rbsp, u32 n){
  u32 o=0; if(cap<5) return 0; dst[o++]=0; dst[o++]=0; dst[o++]=0; dst[o++]=1; dst[o++]=hdr; int z=0;
  for(u32 i=0;i<n && o+2<cap;i++){ if(z>=2 && rbsp[i]<=3){ dst[o++]=3; z=0; } dst[o++]=rbsp[i]; z= rbsp[i]==0? z+1 : 0; }
  return o; }
static u32 rd32(const u8* p, u32 off){ u32 v; memcpy(&v,p+off,4); return v; }
static i32 sext(u32 v, int bits){ return (i32)(v<<(32-bits))>>(32-bits); }
static const u8 zz4[16]={0,1,4,8,5,2,3,6,9,12,13,10,7,11,14,15};
static const u8 zz8[64]={0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
  35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};
static void scaling_list(BW* w, const u8* raster, int n, const u8* zz){
  int last=8; for(int j=0;j<n;j++){ int v=raster[zz[j]]; int d=v-last; if(d>127) d-=256; if(d<-128) d+=256; bw_se(w,d); last=v; } }
static u32 build_headers(const u8* ps, u8* out, u32 cap){
  u8 rb[1024]; BW w; u32 o=0;
  u32 log2_poc=rd32(ps,0x58), dpoc0=rd32(ps,0x5C), fmo=rd32(ps,0x60), wmb=rd32(ps,0x64), hmb=rd32(ps,0x68);
  u32 ecm=rd32(ps,0x70), pop=rd32(ps,0x74), l0=rd32(ps,0x78), l1=rd32(ps,0x7C), dfc=rd32(ps,0x80), rpc=rd32(ps,0x84), t8=rd32(ps,0x88);
  u32 b0=rd32(ps,0xB0), b1=rd32(ps,0xB4), bl=rd32(ps,0x2D0);
  u32 mbaff=b0&1, d8=(b0>>1)&1, wp=(b0>>2)&1, cip=(b0>>3)&1, l2fn=(b0>>8)&15, cfi=(b0>>12)&3, poct=(b0>>14)&3;
  i32 qp26=sext((b0>>16)&63,6), cqo=sext((b0>>22)&31,5), cqo2=sext((b0>>27)&31,5);
  u32 wbi=b1&3, qpz=(bl>>1)&1;
  /* SPS, High profile so the PPS may use 8x8 transform / scaling lists */
  memset(&w,0,sizeof w); w.p=rb; w.cap=sizeof rb;
  bw_u(&w,100,8); bw_u(&w,0,8); bw_u(&w,51,8); bw_ue(&w,0);
  bw_ue(&w,cfi? cfi : 1); bw_ue(&w,0); bw_ue(&w,0); bw_u(&w,qpz,1); bw_u(&w,0,1);
  bw_ue(&w,l2fn); bw_ue(&w,poct);
  if(poct==0) bw_ue(&w,log2_poc);
  else if(poct==1){ bw_u(&w,dpoc0,1); bw_se(&w,0); bw_se(&w,0); bw_ue(&w,0); }
  bw_ue(&w,16); bw_u(&w,0,1);
  bw_ue(&w,wmb? wmb-1 : 0); bw_ue(&w, fmo? (hmb? hmb-1 : 0) : (hmb/2? hmb/2-1 : 0));
  bw_u(&w,fmo,1); if(!fmo) bw_u(&w,mbaff,1);
  bw_u(&w,d8,1); bw_u(&w,0,1); bw_u(&w,0,1); bw_trailing(&w);
  o+=nal_out(out+o,cap-o,0x67,rb,w.bytes);
  /* PPS, scaling matrices taken from the pic setup */
  memset(&w,0,sizeof w); w.p=rb; w.cap=sizeof rb;
  bw_ue(&w,0); bw_ue(&w,0); bw_u(&w,ecm,1); bw_u(&w,pop,1); bw_ue(&w,0);
  bw_ue(&w,l0); bw_ue(&w,l1); bw_u(&w,wp,1); bw_u(&w,wbi,2); bw_se(&w,qp26); bw_se(&w,0); bw_se(&w,cqo);
  bw_u(&w,dfc,1); bw_u(&w,cip,1); bw_u(&w,rpc,1);
  bw_u(&w,t8,1); bw_u(&w,1,1);
  for(int i=0;i<6;i++){ bw_u(&w,1,1); scaling_list(&w,ps+0x1C0+i*16,16,zz4); }
  if(t8) for(int i=0;i<2;i++){ bw_u(&w,1,1); scaling_list(&w,ps+0x220+i*64,64,zz8); }
  bw_se(&w,cqo2); bw_trailing(&w);
  o+=nal_out(out+o,cap-o,0x68,rb,w.bytes);
  return o; }

/* ------------------------------------------------------------ decoder and the last picture */
#ifndef SWR_NO_H264
static ISVCDecoder* g_dec;
#endif
static int g_dec_fail;
static u8* g_fy; static u8* g_fuv; static u32 g_fw, g_fh, g_fcap; static u32 g_vframes;
static u8 g_last_hdr[512]; static u32 g_last_hdr_n;
static int dec_open(void){
#ifdef SWR_NO_H264
  if(!g_dec_fail){ g_dec_fail=1; LOG("video: this build has no H.264 decoder (the game's videos stay black)\n"); } return 0;
#else
  if(g_dec) return 1; if(g_dec_fail) return 0;
  if(WelsCreateDecoder(&g_dec) || !g_dec){ g_dec_fail=1; LOG("video: OpenH264 decoder could not be created\n"); return 0; }
  SDecodingParam p; memset(&p,0,sizeof p); p.sVideoProperty.eVideoBsType=VIDEO_BITSTREAM_AVC; p.eEcActiveIdc=ERROR_CON_SLICE_COPY;
  if((*g_dec)->Initialize(g_dec,&p)){ g_dec_fail=1; LOG("video: OpenH264 decoder initialize failed\n"); return 0; }
  LOG("video: H.264 decoder ready (OpenH264, NVDEC jobs are decoded on the CPU)\n"); return 1;
#endif
}
#ifndef SWR_NO_H264
static void keep_frame(unsigned char** pl, SBufferInfo* bi){
  u32 w=(u32)bi->UsrData.sSystemBuffer.iWidth, h=(u32)bi->UsrData.sSystemBuffer.iHeight; int sy=bi->UsrData.sSystemBuffer.iStride[0], sc=bi->UsrData.sSystemBuffer.iStride[1];
  if(!w || !h || w>4096 || h>4096) return;
  u32 need=w*h; if(g_fcap<need){ free(g_fy); free(g_fuv); g_fy=malloc(need); g_fuv=malloc(need/2+w); g_fcap=need; }
  for(u32 y=0;y<h;y++) memcpy(g_fy+(u64)y*w,pl[0]+(u64)y*sy,w);
  for(u32 y=0;y<h/2;y++){ u8* d=g_fuv+(u64)y*w; const u8* u=pl[1]+(u64)y*sc; const u8* v=pl[2]+(u64)y*sc; for(u32 x=0;x<w/2;x++){ d[2*x]=u[x]; d[2*x+1]=v[x]; } }
  g_fw=w; g_fh=h; g_vframes++; }
#endif
static void nvdec_execute(u32* r){
  if(!dec_open()) return;
#ifndef SWR_NO_H264
  u64 ps_iova=(u64)r[0x404/4]<<8, bs_iova=(u64)r[0x408/4]<<8;
  u8* ps=iova_ptr(ps_iova,0x300); if(!ps){ static int said; if(said<4){ said++; LOG("video: NVDEC picture setup at iova %llx not mapped\n",(unsigned long long)ps_iova); } return; }
  u32 slen=rd32(ps,0x48); if(!slen || slen>(16u<<20)) return;
  u8* bs=iova_ptr(bs_iova,slen); if(!bs){ static int said; if(said<4){ said++; LOG("video: NVDEC bitstream at iova %llx (%u bytes) not mapped\n",(unsigned long long)bs_iova,slen); } return; }
  u8 hdr[512]; u32 hn=build_headers(ps,hdr,sizeof hdr);
  static u8* buf; static u32 bcap; u32 need=hn+slen+8; if(bcap<need){ buf=realloc(buf,need); bcap=need; }
  u32 o=0;
  if(hn!=g_last_hdr_n || memcmp(hdr,g_last_hdr,hn)){ memcpy(buf,hdr,hn); o=hn; memcpy(g_last_hdr,hdr,hn); g_last_hdr_n=hn; }
  if(bs[0]==0 && bs[1]==0 && (bs[2]==1 || (bs[2]==0 && bs[3]==1))){ memcpy(buf+o,bs,slen); o+=slen; }
  else {
    /* mp4-style 4 byte BE lengths -> start codes; otherwise just prepend one */
    u32 p=0, ok=1; while(p+4<=slen){ u32 L=((u32)bs[p]<<24)|((u32)bs[p+1]<<16)|((u32)bs[p+2]<<8)|bs[p+3]; if(!L || p+4+L>slen || (bs[p+4]&0x80)){ ok=0; break; } p+=4+L; }
    if(ok && p==slen){ static u8* t; static u32 tc; if(tc<slen+64){ t=realloc(t,slen+64); tc=slen+64; } (void)t;
      if(bcap<o+slen+slen/4+16){ bcap=o+slen+slen/4+16; buf=realloc(buf,bcap); }
      for(p=0;p<slen;){ u32 L=((u32)bs[p]<<24)|((u32)bs[p+1]<<16)|((u32)bs[p+2]<<8)|bs[p+3]; buf[o++]=0; buf[o++]=0; buf[o++]=0; buf[o++]=1; memcpy(buf+o,bs+p+4,L); o+=L; p+=4+L; } }
    else { buf[o++]=0; buf[o++]=0; buf[o++]=1; memcpy(buf+o,bs,slen); o+=slen; } }
  unsigned char* pl[3]={0,0,0}; SBufferInfo bi; memset(&bi,0,sizeof bi);
  DECODING_STATE st=(*g_dec)->DecodeFrameNoDelay(g_dec,buf,(int)o,pl,&bi);
  if(bi.iBufferStatus==1 && pl[0]) keep_frame(pl,&bi);
  /* nvdec_status_s at method 0x424. players check it, an all-zero status means the frame failed */
  { u64 st_iova=(u64)r[0x424/4]<<8; u8* sp= st_iova? iova_ptr(st_iova,16) : NULL;
    if(sp){ u32 mbs=rd32(ps,0x64)*rd32(ps,0x68); u32 v[4]={ st? 0 : mbs, st? mbs : 0, 100000, st? 1 : 0 }; memcpy(sp,v,16); } }
  { static u32 njobs; njobs++; if(njobs%120==0) LOG("video: %u NVDEC jobs so far, %u pictures decoded\n",njobs,g_vframes); }
  { static int said; if(said<3 || (st && said<8)){ said++; LOG("video: NVDEC H.264 job: %ux%u MBs, %u bytes, decoder state %x, picture %s (%ux%u)\n",rd32(ps,0x64),rd32(ps,0x68),slen,(unsigned)st,bi.iBufferStatus==1? "out" : "pending",g_fw,g_fh); } }
#endif
}
/* VIC output surface config, u64 at config(0x708)+0x20:
   0-6 format (0x1F A8B8G8R8, 0x20 A8R8G8B8, 0x44 NV12), 11-14 bl kind, 15-18 block height log2,
   32-45 width-1, 46-59 height-1 */
u64 bl_offset(u32 x_bytes, u32 y, u32 z, u32 width_bytes, u32 height, u32 bh_log2, u32 bd_log2);
static u8 clamp8(int v){ return v<0? 0 : v>255? 255 : (u8)v; }
static void vic_put(u8* base, u64 off, const u8* src, u32 n, u64 lim){ if(off+n<=lim) memcpy(base+off,src,n); }
static void vic_execute(u32* r){
  if(!g_vframes) return;
  { static u32 nv; nv++; if(nv%120==0) LOG("video: %u VIC jobs so far\n",nv); }
  u64 ly=(u64)r[0x720/4]<<8, lc=(u64)r[0x724/4]<<8, cfg_iova=(u64)r[0x708/4]<<8;
  u8* cs=iova_ptr(cfg_iova,0x28); u64 cfg=0; if(cs) memcpy(&cfg,cs+0x20,8);
  u32 fmt=(u32)(cfg&0x7F), blk=(u32)((cfg>>11)&15), bh=(u32)((cfg>>15)&15);
  u32 sw=(u32)((cfg>>32)&0x3FFF)+1, sh=(u32)((cfg>>46)&0x3FFF)+1;
  if(!cs){ fmt=0x44; sw=g_fw; sh=g_fh; }
  if(getenv("SWR_VIC_FMT")) fmt=(u32)strtoul(getenv("SWR_VIC_FMT"),0,0);
  u32 w=g_fw<sw? g_fw : sw, h=g_fh<sh? g_fh : sh;
  { static int said; if(said<3){ said++; LOG("video: VIC config %016llx: format %x, %s (block height %u), surface %ux%u, picture %ux%u\n",(unsigned long long)cfg,fmt,blk? "block linear" : "pitch linear",1u<<bh,sw,sh,g_fw,g_fh); } }
  if(fmt==0x1F || fmt==0x20){
    u32 pitch=sw*4; u64 lim= blk? (u64)pitch*(((sh+(8u<<bh)-1)/(8u<<bh))*(8u<<bh)) : (u64)pitch*sh;
    u8* py=iova_ptr(ly,lim); if(!py){ py=iova_ptr(ly,(u64)pitch*h); lim=(u64)pitch*h; }
    if(!py){ static int said; if(said<4){ said++; LOG("video: VIC output at iova %llx not mapped\n",(unsigned long long)ly); } return; }
    static u8* row; static u32 rcap; if(rcap<pitch){ row=realloc(row,pitch); rcap=pitch; }
    int ri= fmt==0x1F? 0 : 2, bi= fmt==0x1F? 2 : 0;
    for(u32 y=0;y<h;y++){
      const u8* Y=g_fy+(u64)y*g_fw; const u8* UV=g_fuv+(u64)(y/2)*g_fw;
      for(u32 x=0;x<w;x++){
        /* BT.601, limited range */
        int c=((int)Y[x]-16)*298, d=(int)UV[x&~1u]-128, e=(int)UV[(x&~1u)+1]-128; u8* o=row+x*4;
        o[ri]=clamp8((c+409*e+128)>>8); o[1]=clamp8((c-100*d-208*e+128)>>8); o[bi]=clamp8((c+516*d+128)>>8); o[3]=255; }
      if(!blk) vic_put(py,(u64)y*pitch,row,w*4,lim);
      else for(u32 xb=0;xb<w*4;xb+=16){ u32 n=w*4-xb<16? w*4-xb : 16; vic_put(py,bl_offset(xb,y,0,pitch,sh,bh,0),row+xb,n,lim); } }
    { static int said; if(said<2){ said++; LOG("video: VIC wrote a %ux%u RGBA picture at iova %llx\n",w,h,(unsigned long long)ly); } }
    return; }
  /* NV12. pitch linear rows are 256 aligned */
  u32 pitch= blk? sw : (sw+255)&~255u;
  u64 ylim=(u64)pitch*sh, clim=(u64)pitch*((sh+1)/2);
  if(blk){ u32 gy=8u<<bh; ylim=(u64)pitch*((sh+gy-1)/gy*gy); clim=(u64)pitch*(((sh+1)/2+gy-1)/gy*gy); }
  u8* py=iova_ptr(ly,ylim); u8* pc= lc? iova_ptr(lc,clim) : NULL;
  if(!py){ static int said; if(said<4){ said++; LOG("video: VIC output luma at iova %llx not mapped\n",(unsigned long long)ly); } return; }
  for(u32 y=0;y<h;y++){ const u8* s=g_fy+(u64)y*g_fw;
    if(!blk) vic_put(py,(u64)y*pitch,s,w,ylim);
    else for(u32 x=0;x<w;x+=16){ u32 n=w-x<16? w-x : 16; vic_put(py,bl_offset(x,y,0,pitch,sh,bh,0),s+x,n,ylim); } }
  if(pc) for(u32 y=0;y<h/2;y++){ const u8* s=g_fuv+(u64)y*g_fw;
    if(!blk) vic_put(pc,(u64)y*pitch,s,w,clim);
    else for(u32 x=0;x<w;x+=16){ u32 n=w-x<16? w-x : 16; vic_put(pc,bl_offset(x,y,0,pitch,(sh+1)/2,bh,0),s+x,n,clim); } }
  { static int said; if(said<2){ said++; LOG("video: VIC wrote a %ux%u NV12 picture (luma iova %llx, chroma %llx, pitch %u)\n",w,h,(unsigned long long)ly,(unsigned long long)lc,pitch); } } }

/* ------------------------------------------------------------ host1x command stream */
typedef struct { int is_vic; u32 syncpt; u32 regs[0x1000/4]; u32 method; } H1Ch;
static void dev_method(H1Ch* ch, u32 m, u32 v){
  u32 off=m<<2; if(off<0x1000) ch->regs[off/4]=v;
  if(off==0x300){ if(ch->is_vic) vic_execute(ch->regs); else nvdec_execute(ch->regs); } }
static void reg_write(H1Ch* ch, u32 cls, u32 off, u32 v){
  if(cls==0x1) return;                       /* host1x class itself, incrs are already counted from the submit */
  if(off==0x10) ch->method=v; else if(off==0x11) dev_method(ch,ch->method,v); }
static void run_words(H1Ch* ch, const u32* w, u32 n){
  u32 cls=ch->is_vic? 0x5D : 0xF0;
  for(u32 i=0;i<n;){
    u32 x=w[i++], op=x>>28;
    switch(op){
      case 0: { cls=(x>>6)&0x3FF; u32 off=(x>>16)&0xFFF, mask=x&0x3F; for(int b=0;b<6;b++) if(mask&(1u<<b)){ if(i>=n) return; reg_write(ch,cls,off+b,w[i++]); } break; }
      case 1: { u32 off=(x>>16)&0xFFF, c=x&0xFFFF; for(u32 k=0;k<c && i<n;k++) reg_write(ch,cls,off+k,w[i++]); break; }
      case 2: { u32 off=(x>>16)&0xFFF, c=x&0xFFFF; for(u32 k=0;k<c && i<n;k++) reg_write(ch,cls,off,w[i++]); break; }
      case 3: { u32 off=(x>>16)&0xFFF, mask=x&0xFFFF; for(int b=0;b<16;b++) if(mask&(1u<<b)){ if(i>=n) return; reg_write(ch,cls,off+b,w[i++]); } break; }
      case 4: reg_write(ch,cls,(x>>16)&0xFFF,x&0xFFFF); break;
      default: { static int said; if(said<6){ said++; LOG("video: host1x opcode %u not handled (word %08x)\n",op,x); } return; }
    } } }

/* ------------------------------------------------------------ channel ioctls */
static H1Ch g_ch[2]; static u32 g_ch_syncpt_next=200;
u32 host1x_ioctl(int is_vic, u32 cmd, u8* d, u32 n){
  h_init(); pm_lock(&g_hlk);
  H1Ch* ch=&g_ch[is_vic? 1 : 0]; ch->is_vic=is_vic; if(!ch->syncpt) ch->syncpt=g_ch_syncpt_next++;
  u32 r=0, grp=(cmd>>8)&0xFF, nr=cmd&0xFF;
  if(grp==0x48){ /* SET_NVMAP_FD etc */ }
  else if(grp==0) switch(nr){
    case 0x01: { /* SUBMIT: counts, cmdbufs {memid, offset, words}, relocs, reloc shifts, syncpt incrs {id, n, ?x3}, fences {id, value} */
      u32 ncb=rd32(d,0), nrel=rd32(d,4), nsp=rd32(d,8), nfe=rd32(d,12); u32 o=16;
      u32 cbo=o; o+=ncb*12; o+=nrel*16; o+=nrel*4; u32 spo=o; o+=nsp*20; u32 feo=o;
      if(o>n){ r=1; break; }
      for(u32 i=0;i<ncb;i++){ u32 mem=rd32(d,cbo+i*12), off=rd32(d,cbo+i*12+4), wc=rd32(d,cbo+i*12+8);
        u64 a=nvmap_addr(mem); if(!a) a=nvmap_addr_by_id(mem); if(!a || !wc || wc>0x100000) continue;
        run_words(ch,(const u32*)GP(a+off),wc); }
      for(u32 i=0;i<nsp;i++){ u32 id=rd32(d,spo+i*20), inc=rd32(d,spo+i*20+4); if(!id) id=ch->syncpt;
        for(u32 k=0;k<inc && k<64;k++) syncpt_incr(id);
        if(i<nfe && feo+i*4+4<=n){ u32 v=syncpt_read(id); memcpy(d+feo+i*4,&v,4); } }   /* fence thresholds, u32 each */
      { static int said; if(said<3){ said++; LOG("video: %s submit: %u command buffers, %u relocs, %u syncpoint increments, %u fences (%u bytes)\n",is_vic? "vic" : "nvdec",ncb,nrel,nsp,nfe,n); } }
      break; }
    case 0x02: { u32 v=ch->syncpt; memcpy(d+4,&v,4); break; }          /* GET_SYNCPOINT */
    case 0x03: { u32 v=0; memcpy(d+4,&v,4); break; }                    /* GET_WAITBASE */
    case 0x07: break;                                                   /* SET_SUBMIT_TIMEOUT */
    case 0x09: { u32 ne=rd32(d,0); for(u32 i=0;i<ne && 12+i*8+8<=n;i++){ u32 h=rd32(d,12+i*8); u32 io=iova_map(h); memcpy(d+12+i*8+4,&io,4); } break; }   /* MAP_BUFFER */
    case 0x0A: break;                                                   /* UNMAP_BUFFER: we leave the iova mapped */
    default: { static int said; if(said<8){ said++; LOG("video: %s ioctl %08x not handled\n",is_vic? "vic" : "nvdec",cmd); } break; }
  }
  else { static int said; if(said<8){ said++; LOG("video: %s ioctl %08x not handled\n",is_vic? "vic" : "nvdec",cmd); } }
  pm_unlock(&g_hlk); return r; }
