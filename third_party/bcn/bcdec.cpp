// SwitchRecomp - decodificacion BCn en la CPU para GPUs sin soporte (Mali/Adreno en Android).
// Usa los decodificadores de texture2ddecoder (MIT, ver LICENSE_texture2ddecoder.txt).
#include "bcn.h"
#include <string.h>
#include <stdint.h>

static void bc2_block(const uint8_t* b, uint32_t* o){
  uint16_t q0=b[8]|(b[9]<<8), q1=b[10]|(b[11]<<8);
  uint32_t r0=(q0>>11)&31, g0=(q0>>5)&63, b0=q0&31, r1=(q1>>11)&31, g1=(q1>>5)&63, b1=q1&31;
  r0=r0*255/31; g0=g0*255/63; b0=b0*255/31; r1=r1*255/31; g1=g1*255/63; b1=b1*255/31;
  uint32_t c[4][3]={{r0,g0,b0},{r1,g1,b1},{(2*r0+r1)/3,(2*g0+g1)/3,(2*b0+b1)/3},{(r0+2*r1)/3,(g0+2*g1)/3,(b0+2*b1)/3}};
  uint32_t idx=b[12]|(b[13]<<8)|(b[14]<<16)|((uint32_t)b[15]<<24);
  for(int i=0;i<16;i++){ uint32_t a=(b[i>>1]>>((i&1)*4))&15; a*=17; uint32_t k=(idx>>(i*2))&3;
    o[i]=c[k][0]|(c[k][1]<<8)|(c[k][2]<<16)|(a<<24); } }

extern "C" int swr_bc_decode(int kind, const uint8_t* src, uint32_t w, uint32_t h, uint32_t* out){
  switch(kind){
    case 2: decode_bc1(src,(long)w,(long)h,out); break;
    case 3: { uint32_t bx=(w+3)/4, by=(h+3)/4, blk[16];
      for(uint32_t y=0;y<by;y++) for(uint32_t x=0;x<bx;x++){ bc2_block(src+(y*bx+x)*16,blk);
        for(uint32_t j=0;j<4;j++){ uint32_t yy=y*4+j; if(yy>=h) break; for(uint32_t i=0;i<4;i++){ uint32_t xx=x*4+i; if(xx<w) out[yy*w+xx]=blk[j*4+i]; } } }
      return 1; }   /* ya esta en RGBA */
    case 4: decode_bc3(src,(long)w,(long)h,out); break;
    case 5: decode_bc4(src,w,h,out); break;
    case 6: decode_bc5(src,w,h,out); break;
    case 7: decode_bc7(src,w,h,out); break;
    case 8: decode_bc6(src,w,h,out); break;
    default: memset(out,0,(size_t)w*h*4); return 0;
  }
  /* la biblioteca entrega BGRA: pasar a RGBA */
  size_t n=(size_t)w*h; for(size_t i=0;i<n;i++){ uint32_t v=out[i]; out[i]=(v&0xFF00FF00u)|((v&0xFF)<<16)|((v>>16)&0xFF); }
  return 1; }
