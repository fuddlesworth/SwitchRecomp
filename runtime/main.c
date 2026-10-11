/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
#define _GNU_SOURCE
/* startup: load GAME_code.dll/.so, set up memory + services + main thread, then run the frontend
   loop (window, input, vsync). */
#include "rt.h"
#include <sys/stat.h>

const SwrModule* g_mod;
GuestFn* g_disp;
GuestFn swr_interp_entry;
char g_datadir[1024], g_exename[256], g_exedir[1024];
volatile int g_quit;
int g_frames;
int g_opt_diag=0;
int g_opt_docked=0, g_opt_lang=1, g_opt_res=1, g_opt_fps=1, g_opt_vsync=0, g_opt_drs=0;
float g_opt_resf=1.0f; int g_opt_shadows=1; int g_opt_cmp_low=1; int g_opt_resdyn=0; int g_opt_prof=0; float g_opt_resmin=0.6f;
static char g_hints_path[1200];

/* ============================================================ exports for the generated code */
static PMutex g_hint_lock; static u64* g_hints; static int g_nhints, g_caphints, g_hints_dirty;
static void rt_note_hint(u64 pc){
  pm_lock(&g_hint_lock);
  for(int i=g_nhints-1;i>=0 && i>=g_nhints-64;i--) if(g_hints[i]==pc){ pm_unlock(&g_hint_lock); return; }
  int found=0; for(int i=0;i<g_nhints;i++) if(g_hints[i]==pc){ found=1; break; }
  if(!found){ if(g_nhints==g_caphints){ g_caphints=g_caphints?g_caphints*2:256; g_hints=realloc(g_hints,8*g_caphints); } g_hints[g_nhints++]=pc; g_hints_dirty=1;
    LOGV("interpreter: entering %llx (not recompiled)\n",(unsigned long long)pc); }
  pm_unlock(&g_hint_lock); }
void save_hints(void){
  if(!g_hints_dirty) return; FILE* f=fopen(g_hints_path,"w"); if(!f) return;
  pm_lock(&g_hint_lock); for(int i=0;i<g_nhints;i++) fprintf(f,"%llx\n",(unsigned long long)g_hints[i]); g_hints_dirty=0; pm_unlock(&g_hint_lock); fclose(f); }
static void load_hints(void){ FILE* f=fopen(g_hints_path,"r"); if(!f) return; char ln[64]; while(fgets(ln,sizeof ln,f)){ u64 v=strtoull(ln,NULL,16); if(v){ if(g_nhints==g_caphints){ g_caphints=g_caphints?g_caphints*2:256; g_hints=realloc(g_hints,8*g_caphints);} g_hints[g_nhints++]=v; } } fclose(f); }

void kernel_dump_regs(CPU* c);
static void rt_undef(CPU* c, u64 pc, u32 w){
  WARN("\nUnsupported instruction %08x at %llx\n",w,(unsigned long long)pc); c->pc=pc; kernel_dump_regs(c);
  save_hints(); fatal("Unsupported instruction %08x at %llx",w,(unsigned long long)pc); }
static void rt_brk(CPU* c, u64 pc, u32 imm){ WARN("\nBRK #%u at %llx\n",imm,(unsigned long long)pc); c->pc=pc; kernel_dump_regs(c); save_hints(); fatal("The game executed BRK at %llx",(unsigned long long)pc); }
static u64 g_t0;
u64 g_t0_ns(void){ return g_t0; }
/* guest clock. Normally == real time. In smooth recording mode (F7) it runs slower than real time so that
   every frame spans ~1/recording_fps of guest time, which gives a steady video even when the PC can't keep up.
   It never stops, so guest timeouts still fire. vsync follows it too. */
volatile int g_vt_on, g_rec_active; int g_opt_rec_smooth=1, g_opt_rec_fps=60;
static PMutex g_vt_m; static int g_vt_mi;
static u64 g_vt_ar, g_vt_av, g_vt_lastfr; static double g_vt_s=1.0, g_vt_fr=0;   /* anchor + rate */
static volatile u64 g_vt_max;
static u64 guest_ns_raw(u64 r){ return g_vt_av+(u64)((double)(r-g_vt_ar)*g_vt_s); }
u64 guest_ns(void){
  u64 r=plat_ns()-g_t0, v;
  if(!g_vt_mi){ v=r; } else { pm_lock(&g_vt_m); v=guest_ns_raw(r); pm_unlock(&g_vt_m); }
  u64 m=__atomic_load_n(&g_vt_max,__ATOMIC_RELAXED);
  while(v>m){ if(__atomic_compare_exchange_n(&g_vt_max,&m,v,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) return v; }
  return m; }
static void vt_rebase(u64 r, double s){ g_vt_av=guest_ns_raw(r); g_vt_ar=r; g_vt_s=s; }
static void vt_frame(void){
  if(!g_vt_mi) return;
  u64 r=plat_ns()-g_t0; pm_lock(&g_vt_m);
  /* cost per frame = GPU thread busy time. don't use full frame time, the game's own waits
     would feed back and slow the clock further */
  { extern u64 gpu_busy_ns(void); u64 b=gpu_busy_ns(); static u64 lb;
    if(g_vt_lastfr){ double d=(double)(b-lb); if(d>2e9) d=2e9;
      /* attack fast, release slow */
      if(g_vt_fr<=0 || d>g_vt_fr) g_vt_fr=d; else g_vt_fr=g_vt_fr*0.92+d*0.08; } lb=b; }
  g_vt_lastfr=r;
  double s=1.0; if(g_vt_on && g_vt_fr>0){ s=(1e9/(double)g_opt_rec_fps)/(g_vt_fr*1.25); if(s>1) s=1; if(s<0.02) s=0.02; }
  { static double fs=-1; if(fs<0){ const char* e=getenv("SWR_VT_SCALE"); fs= e? atof(e) : 0; } if(g_vt_on && fs>0) s=fs; }   /* test hook: force the clock rate */
  vt_rebase(r,s); pm_unlock(&g_vt_m); }
void vt_set(int on){
  if(!g_vt_mi){ pm_init(&g_vt_m); u64 r=plat_ns()-g_t0; g_vt_ar=r; g_vt_av=r; g_vt_s=1.0; g_vt_mi=1; }
  pm_lock(&g_vt_m); vt_rebase(plat_ns()-g_t0,1.0); g_vt_fr=0; g_vt_lastfr=0; pm_unlock(&g_vt_m);
  g_vt_on= on && g_opt_rec_smooth; }
/* vsync ticks on guest time */
static void vsync_tick_vt(void){
  static u64 next; if(!g_vt_on){ next=0; vi_vsync_tick(); return; }
  u64 v=guest_ns(); if(!next) next=v;
  if(v>=next){ vi_vsync_tick(); next+=16666667ull; if(v>next+50000000ull) next=v; } }
static u64 rt_ticks(void){ return (u64)((double)guest_ns()*0.0192); }
void swr_fpcr_apply(CPU* c){
#if defined(__aarch64__)
  /* arm64 host: just load the guest FPCR (FZ, rounding, DN, AHP) */
  u64 v=(u64)(c->fpcr&0x07C00000u); __asm__ __volatile__("msr fpcr, %0"::"r"(v));
#elif defined(__x86_64__) || defined(_M_X64)
  u32 mx; __asm__ __volatile__("stmxcsr %0":"=m"(mx));
  mx&=~(0x8040u|0x6000u);
  if(c->fpcr&(1u<<24)) mx|=0x8040;                        /* FZ -> FTZ+DAZ */
  static const u32 rm[4]={0,0x4000,0x2000,0x6000}; mx|=rm[(c->fpcr>>22)&3];
  __asm__ __volatile__("ldmxcsr %0"::"m"(mx));
#else
  (void)c;
#endif
}
static void rt_fpcr(CPU* c){ swr_fpcr_apply(c); }
static void rt_sev(void){}
static void rt_wfe(CPU* c){ (void)c; plat_yield(); }
const RtAPI g_rtapi={swr_svc,rt_undef,rt_note_hint,rt_ticks,rt_brk,rt_fpcr,rt_sev,rt_wfe};

/* ============================================================ system errors */
void crash_report(void* host_pc, void* fault, int sig){
  KThread* t=kthread_cur();
  u64 ga=(u64)((u8*)fault-g_mem);
  logf_(0,"\n*** Crash (signal %d) at host pc=%p, access to %p (game address %llx) ***\n",sig,host_pc,fault,(unsigned long long)ga);
  { extern void mem_report(char*,int); char mr[200]; mem_report(mr,sizeof mr); logf_(0,"  memory: %s\n",mr);
#ifdef _WIN32
    { extern char __ImageBase; u64 b=(u64)(size_t)&__ImageBase, p=(u64)(size_t)host_pc; if(p>=b && p<b+0x10000000ull) logf_(0,"  in the runtime itself (exe offset +%llx), thread %s\n",(unsigned long long)(p-b), kthread_cur()? "of the game" : "of the runtime (GPU, audio or window)"); }
#endif
    if(ga && ga<0x1000000ull) logf_(0,"  the address is a null pointer plus an offset: the game probably got NULL from its allocator (out of memory?). Try memory_gb=6 in the .ini\n"); }
  if(g_mod){ /* closest recompiled fn below host pc */
    u64 best=0, ga2=0; for(u32 i=0;i<g_mod->nentries;i++){ u64 f=(u64)(size_t)g_mod->entry_fn[i]; if(f<=(u64)(size_t)host_pc && f>best){ best=f; ga2=g_mod->entry_addr[i]; } }
    if(best && (u64)(size_t)host_pc-best<0x100000) logf_(0,"  inside the recompiled code of game function %llx (+%llx on the host)\n",(unsigned long long)ga2,(unsigned long long)((u64)(size_t)host_pc-best)); }
  if(t){ logf_(0,"  thread %llu (%s)\n",(unsigned long long)t->id,t->name); kernel_dump_regs(&t->cpu); }
  save_hints();
  fatal("The game crashed (invalid memory access). See the .log.");
}

/* ============================================================ options */
/* legacy (spanish) key names -> current ones */
static const char* const INI_ALIAS[][2]={
  {"modo_tv","docked"},{"idioma","language"},{"resolucion","resolution"},{"resolucion_dinamica","dynamic_resolution"},
  {"resolucion_minima","dynamic_resolution_min"},{"resolucion_dinamica_del_juego","game_dynamic_resolution"},{"sombras","shadows"},
  {"perfil","profile"},{"compilador_prioridad_baja","low_priority_shader_compiler"},{"vsync_forzado","forced_vsync"},
  {"grabacion_fluida","smooth_recording"},{"grabacion_fps","recording_fps"},{"lsfg_objetivo","lsfg_target"},
  {"lsfg_multiplicador","lsfg_multiplier"},{"lsfg_flujo","lsfg_flow"},{"lsfg_resolucion","lsfg_resolution"},
  {"lossless_ruta","lossless_path"},{"controles","touch_controls"},{"controles_opacidad","touch_controls_opacity"},{NULL,NULL}};
static const char* ini_norm(const char* k){ for(int i=0;INI_ALIAS[i][0];i++) if(!strcmp(k,INI_ALIAS[i][0])) return INI_ALIAS[i][1]; return k; }
/* everything we read, so the rewrite keeps unknown keys */
#define INI_MAXK 128
static char g_ini_k[INI_MAXK][64], g_ini_v[INI_MAXK][1024]; static int g_ini_n;
static void ini_store(const char* k, const char* v){ for(int i=0;i<g_ini_n;i++) if(!strcmp(g_ini_k[i],k)){ strncpy(g_ini_v[i],v,1023); return; }
  if(g_ini_n<INI_MAXK){ strncpy(g_ini_k[g_ini_n],k,63); strncpy(g_ini_v[g_ini_n],v,1023); g_ini_n++; } }
static const char* ini_get(const char* k, const char* def){ for(int i=0;i<g_ini_n;i++) if(!strcmp(g_ini_k[i],k)) return g_ini_v[i]; return def; }
static void ini_apply(const char* k, const char* v){
    if(!strcmp(k,"docked")) g_opt_docked=atoi(v);
    else if(!strcmp(k,"language")) g_opt_lang=atoi(v);
    else if(!strcmp(k,"log")) g_verbose=atoi(v);
    else if(!strcmp(k,"diag")) g_opt_diag=atoi(v);
    else if(!strcmp(k,"smooth_recording")) g_opt_rec_smooth=atoi(v);
    else if(!strcmp(k,"recording_fps")){ g_opt_rec_fps=atoi(v); if(g_opt_rec_fps<15) g_opt_rec_fps=15; if(g_opt_rec_fps>60) g_opt_rec_fps=60; }
    else if(!strcmp(k,"fps_limit")) g_opt_fps=atoi(v);
    else if(!strcmp(k,"resolution")){ g_opt_resf=(float)atof(v); if(g_opt_resf<0.25f) g_opt_resf=0.25f; if(g_opt_resf>4) g_opt_resf=4; }
    else if(!strcmp(k,"shadows")) g_opt_shadows=atoi(v);
    else if(!strcmp(k,"profile")) g_opt_prof=atoi(v);
    else if(!strcmp(k,"low_priority_shader_compiler")) g_opt_cmp_low=atoi(v);
    else if(!strcmp(k,"dynamic_resolution")) g_opt_resdyn=atoi(v);
    else if(!strcmp(k,"dynamic_resolution_min")){ g_opt_resmin=(float)atof(v); if(g_opt_resmin<0.25f) g_opt_resmin=0.25f; if(g_opt_resmin>1) g_opt_resmin=1; }
    else if(!strcmp(k,"forced_vsync")) g_opt_vsync=atoi(v);
    else if(!strcmp(k,"game_dynamic_resolution")) g_opt_drs=atoi(v);   /* 1 = let the game's own DRS work */
    else if(!strcmp(k,"memory_gb")){ extern int g_opt_mem_gb; g_opt_mem_gb=atoi(v); }
    else if(!strcmp(k,"heap_max_mb")){ extern u64 g_heap_max; g_heap_max=(u64)atoi(v)<<20; }
    { extern void lsfg_ini(const char*, const char*); lsfg_ini(k,v); }
    backend_ini(k,v); }
void ini_load(const char* path){
  FILE* f=fopen(path,"r"); if(!f) return; char ln[1200];
  while(fgets(ln,sizeof ln,f)){ char* eq=strchr(ln,'='); if(!eq||ln[0]==';'||ln[0]=='#'||ln[0]=='[') continue; *eq=0; char* v=eq+1; char* nl=strpbrk(v,"\r\n"); if(nl) *nl=0;
    char* k=ln; while(*k==' '||*k=='\t') k++; char* ke=k+strlen(k); while(ke>k && (ke[-1]==' '||ke[-1]=='\t')) *--ke=0; while(*v==' '||*v=='\t') v++;
    { char* ve=v+strlen(v); while(ve>v && (ve[-1]==' '||ve[-1]=='\t')) *--ve=0; }
    if(!strcmp(k,"vsync")) continue;   /* removed option */
    const char* kn=ini_norm(k); ini_store(kn,v); ini_apply(kn,v); }
  fclose(f); }
/* rewrite the ini on every start: sorted, all keys, with help text */
typedef struct { const char* key; const char* def; const char* help; } IniOpt;
typedef struct { const char* title; const IniOpt* o; } IniSec;
static const IniOpt INI_DISPLAY[]={
  {"docked","0","1 = TV mode (1920x1080 / 1600x900 games), 0 = handheld mode (1280x720)"},
  {"fullscreen","1","1 = start in fullscreen (F11 or Alt+Enter toggles it in game)"},
  {"language","1","Game language: 0 Japanese, 1 English (US), 2 French, 3 German, 4 Italian, 5 Spanish,\n;   6 Chinese, 7 Korean, 8 Dutch, 9 Portuguese, 10 Russian, 11 Taiwanese, 12 English (UK),\n;   13 French (Canada), 14 Spanish (Latin America)"},
  {NULL,NULL,NULL}};
static const IniOpt INI_GRAPHICS[]={
  {"resolution","1","Internal 3D resolution: 1 = native, 2 = double, 0.75 / 0.5 = faster (applies live)"},
  {"dynamic_resolution","0","1 = lower the resolution automatically when the GPU can't keep up (down to dynamic_resolution_min)"},
  {"dynamic_resolution_min","0.6","Lowest internal resolution allowed by dynamic_resolution"},
  {"game_dynamic_resolution","0","1 = let the game lower its own resolution like on the console (0 = always full resolution)"},
  {"shadows","1","0 = disable shadow maps (more FPS)"},
  {NULL,NULL,NULL}};
static const IniOpt INI_PERF[]={
  {"fps_limit","1","1 = 60 FPS (the speed of a Switch), 0 = unlimited (the game runs faster). Values under 60 are treated as 60: the game advances one step per image, so 30 meant half speed. F4 changes it in game"},
  {"forced_vsync","0","1 = always wait for vsync (can lower FPS a lot on slow PCs)"},
  {"memory_gb","0","Memory the game sees: 0 = automatic (3.2 GB like the console, more for games that need it,\n;   e.g. Tears of the Kingdom), 4 = console (3.2 GB), 6 or 8 = extended (like yuzu's extended memory layout)"},
  {"low_priority_shader_compiler","1","1 = compile shaders at low priority (less stutter on weak CPUs)"},
  {NULL,NULL,NULL}};
static const IniOpt INI_FG[]={
  {"lsfg","1","In-game frame generation (LSFG). Uses the Lossless.dll from YOUR Lossless Scaling\n;   (copy it into this folder or set lsfg_dll). Only generates frames when the game is below lsfg_target"},
  {"lsfg_target","60","Target FPS for frame generation"},
  {"lsfg_multiplier","2","2 = x2, 3 = x3, 4 = x4"},
  {"lsfg_flow","0.5","Flow scale: 1 = best quality, 0.5 = faster (recommended on integrated GPUs)"},
  {"lsfg_resolution","720","Resolution (height) used for frame generation"},
  {"lsfg_dll","","Path to Lossless.dll (empty = search automatically)"},
  {"lossless_scaling","0","1 = also launch the Lossless Scaling app with the game (not needed when using lsfg)"},
  {"lossless_path","","Path to LosslessScaling.exe (empty = search in Steam)"},
  {NULL,NULL,NULL}};
static const IniOpt INI_REC[]={
  {"smooth_recording","1","1 = F7 recordings are always smooth: the game runs in slow motion while recording\n;   if the PC can't keep up, but the exported video plays at normal speed with no stutter"},
  {"recording_fps","60","FPS of the exported video (constant)"},
  {NULL,NULL,NULL}};
static const IniOpt INI_DBG[]={
  {"log","1","Log detail: 1 = normal, 2 = everything (slow)"},
  {"diag","0","1 = automatic diagnostic dumps (F5 always works)"},
  {"profile","0","1 = write profile.txt every 10 seconds (CPU profiler)"},
  {NULL,NULL,NULL}};
static const IniSec INI_SECS[]={{"Display",INI_DISPLAY},{"Graphics",INI_GRAPHICS},{"Performance",INI_PERF},{"Frame generation",INI_FG},{"Recording",INI_REC},{"Debug",INI_DBG},{NULL,NULL}};
static char g_ini_path[1400]; static u64 g_ini_mtime;
static u64 file_mtime(const char* p){ struct stat st; if(stat(p,&st)) return 0; return (u64)st.st_mtime; }
static int ini_known(const char* k){ for(int s=0;INI_SECS[s].title;s++) for(const IniOpt* o=INI_SECS[s].o;o->key;o++) if(!strcmp(o->key,k)) return 1; return 0; }
void ini_write(void){
  if(!g_ini_path[0]) return;
  char tmp[1450]; snprintf(tmp,sizeof tmp,"%s.tmp",g_ini_path); FILE* f=fopen(tmp,"w"); if(!f) return;
  fprintf(f,"; =====================================================================\n");
  fprintf(f,";  %s - SwitchRecomp settings\n",g_mod && g_mod->title? g_mod->title : g_exename);
  fprintf(f,";  Edit a value and save: most settings apply live while the game runs\n");
  fprintf(f,";  (docked, language, fullscreen and frame generation apply on restart).\n");
  fprintf(f,";  Keys in game: F1 help, F3 screenshot, F4 FPS limit, F5 trace,\n");
  fprintf(f,";  F7 record video, F8 full-res screenshot, F11 fullscreen.\n");
  fprintf(f,"; =====================================================================\n");
  for(int s=0;INI_SECS[s].title;s++){
    fprintf(f,"\n[%s]\n",INI_SECS[s].title);
    for(const IniOpt* o=INI_SECS[s].o;o->key;o++) fprintf(f,"; %s\n%s=%s\n",o->help,o->key,ini_get(o->key,o->def)); }
  int other=0; for(int i=0;i<g_ini_n;i++) if(!ini_known(g_ini_k[i])){ if(!other){ fprintf(f,"\n[Other]\n"); other=1; } fprintf(f,"%s=%s\n",g_ini_k[i],g_ini_v[i]); }
  fclose(f);
  remove(g_ini_path); if(rename(tmp,g_ini_path)) { remove(tmp); return; }
  g_ini_mtime=file_mtime(g_ini_path); }
/* in-game toggles (F4) get persisted */
volatile int g_ini_dirty;
void ini_set(const char* k, const char* v){ ini_store(k,v); g_ini_dirty=1; }
void ini_open(const char* path){ strncpy(g_ini_path,path,sizeof g_ini_path-1); ini_load(path); ini_write(); }
/* ini edited while running: reload and apply what we can live */
void ini_poll(void){
  if(!g_ini_path[0]) return; if(g_ini_dirty){ g_ini_dirty=0; ini_write(); return; } u64 m=file_mtime(g_ini_path); if(!m || m==g_ini_mtime) return;
  g_ini_mtime=m; g_ini_n=0; ini_load(g_ini_path); LOG("settings: %s reloaded\n",g_ini_path); ini_write(); }

/* ============================================================ present */
static u32* g_frame; static int g_fw, g_fh; static PMutex g_flock;
static u64 g_last_present;
void vi_convert_cpu(u64 a, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, u32* out){
  u32 bpp= (fmt==4||fmt==6||fmt==7)? 2 : 4;
  u8* lin=malloc((size_t)w*h*bpp);
  if(layout==3 || layout==2){
    extern void deswizzle(u8* dst, u32 dst_pitch, const u8* src, u32 w_bytes, u32 h, u32 bh_log2);
    deswizzle(lin,w*bpp,GP(a),w*bpp,h,bhl);
  } else { for(u32 y=0;y<h;y++) memcpy(lin+(size_t)y*w*bpp,GP(a+(u64)y*pitch),w*bpp); }
  if(bpp==4){ if(fmt==5){ for(size_t i=0;i<(size_t)w*h;i++){ u32 p=((u32*)lin)[i]; out[i]=(p&0xFF00FF00u)|((p>>16)&0xFF)|((p&0xFF)<<16); } } else memcpy(out,lin,(size_t)w*h*4); }
  else { u16* s=(u16*)lin; for(size_t i=0;i<(size_t)w*h;i++){ u32 p=s[i], r,g,b;
      if(fmt==4){ r=(p>>11)&31; g=(p>>5)&63; b=p&31; r=(r<<3)|(r>>2); g=(g<<2)|(g>>4); b=(b<<3)|(b>>2); }
      else if(fmt==6){ r=(p>>11)&31; g=(p>>6)&31; b=(p>>1)&31; r=(r<<3)|(r>>2); g=(g<<3)|(g>>2); b=(b<<3)|(b>>2); }
      else { r=((p>>12)&15)*17; g=((p>>8)&15)*17; b=((p>>4)&15)*17; }
      out[i]=r|(g<<8)|(b<<16)|0xFF000000u; } }
  free(lin);
}
void vi_show_buffer(u32 nvmap_id, u32 offset, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt){
  extern u64 nvmap_addr_by_id(u32 id); extern int g_gl;
  u64 a=nvmap_addr_by_id(nvmap_id); if(!a){ WARN("vi: buffer without memory (nvmap %u)\n",nvmap_id); return; }
  a+=offset;
  if(g_gl){ extern void gpu_gl_present_buffer(u64,u32,u32,u32,u32,u32,u32); gpu_gl_present_buffer(a,w,h,pitch,layout,bhl,fmt); }
  else {
    pm_lock(&g_flock);
    if(g_fw!=(int)w || g_fh!=(int)h){ free(g_frame); g_frame=malloc((size_t)w*h*4); g_fw=(int)w; g_fh=(int)h; }
    vi_convert_cpu(a,w,h,pitch,layout,bhl,fmt,g_frame);
    video_present_rgba(g_frame,(int)w,(int)h,(int)w);
    pm_unlock(&g_flock);
  }
  { static int once; if(!once){ once=1; if(getenv("SWR_VT")) vt_set(1); } }   /* test smooth clock w/o recording */
  g_frames++; vt_frame();
  /* fps_limit: 1 = 60, otherwise N fps */
  /* in smooth recording the limiter has to count guest time, not real time.
     games that tick once per frame would otherwise run sped up in the video */
  if(g_vt_on){ u64 per= g_opt_fps>=60 && g_opt_fps<=240? 1000000000ull/(u64)g_opt_fps : 16666667ull;
    static u64 last_g; u64 g=guest_ns(), next=last_g+per, t0=plat_ns();
    if(last_g && g<next && next-g<per*2){ while(g<next && plat_ns()-t0<1000000000ull){ plat_sleep_ns(500000); g=guest_ns(); } }
    g=guest_ns(); last_g= (last_g && g-last_g<2*per)? next : g; g_last_present=plat_ns(); return; }
  /* while recording keep at least the 60 cap, uncapped + per-frame logic = sped up video */
  if(g_opt_fps || g_rec_active){ u64 per= g_opt_fps>=60 && g_opt_fps<=240? 1000000000ull/(u64)g_opt_fps : 16666667ull;
    if(g_opt_fps>=10 && g_opt_fps<60){ static int said; if(!said){ said=1; WARN("fps_limit=%d would run the game at %d%% speed (one game step per image): using 60\n",g_opt_fps,g_opt_fps*100/60); } }
    u64 now=plat_ns(); u64 next=g_last_present+per;
    if(g_last_present && now<next) plat_sleep_ns((i64)(next-now));
    now=plat_ns(); g_last_present= (g_last_present && now-g_last_present<2*per)? next : now; }
}

/* ============================================================ code module */
typedef const SwrModule* (*ModFn)(void);
#if !defined(_WIN32)
__attribute__((weak)) const SwrModule* swr_module_static(void);
#else
const SwrModule* swr_module_static(void) __attribute__((weak));
#endif
int g_code_tcc;
static int load_code(void){
  if(swr_module_static){ g_mod=swr_module_static(); return g_mod? 0 : -1; }
  char p[1200];
#if defined(_WIN32)
  snprintf(p,sizeof p,"%s%s_code.dll",g_exedir,g_exename);
#elif defined(__ANDROID__)
  snprintf(p,sizeof p,"lib%s_code.so",g_exename);   /* lives in the apk's lib dir */
#else
  snprintf(p,sizeof p,"%s%s_code.so",g_exedir,g_exename);
#endif
  void* h=plat_dl_open(p);
  if(!h){
#if !defined(_WIN32)
    extern char* dlerror(void); WARN("dlopen: %s\n",dlerror());
#endif
    WARN("Cannot find the recompiled code: %s\n",p); return -1; }
  ModFn f=(ModFn)plat_dl_sym(h,"swr_module"); if(!f){ WARN("%s is not a SwitchRecomp module\n",p); return -1; }
#if defined(_WIN32)
  { /* tell gcc from tcc builds by the PE linker version (ld 2.x vs tcc 6.0). tcc code is a lot slower */
    const u8* b=(const u8*)h; u32 lf; memcpy(&lf,b+0x3C,4); u8 mj=b[lf+24+2], mn=b[lf+24+3];
    g_code_tcc= mj==6 && mn==0;
    if(g_code_tcc) LOG("code: %s_code.dll was compiled with TCC. It runs several times slower than with GCC: recompile with GCC (SwitchRecomp, compiler GCC) for much better performance\n",g_exename);
    else LOG("code: %s_code.dll compiled with GCC (linker %u.%u)\n",g_exename,mj,mn); }
#endif
  g_mod=f(); return 0; }

static void build_dispatch(void){
  g_code_lo=g_mod->code_lo; g_code_hi=g_mod->code_hi;
  u64 n=(g_code_hi-g_code_lo)/4;
  g_disp=calloc(n?n:1,sizeof(GuestFn));
  for(u32 i=0;i<g_mod->nentries;i++){ u64 a=g_mod->entry_addr[i]; if(a>=g_code_lo && a<g_code_hi) g_disp[(a-g_code_lo)/4]=g_mod->entry_fn[i]; }
  swr_interp_entry=g_mod->interp;
}

void services_init(void){
  extern void svc_core_init(void), svc_hid_init(void), svc_fs_init(void), svc_vi_init(void), svc_pl_init(void);
  svc_core_init(); svc_hid_init(); svc_fs_init(); svc_vi_init(); nv_init(); svc_pl_init();
}

static void split_path(const char* argv0){
  char p[1024]; strncpy(p,argv0?argv0:"",1023); p[1023]=0;
  char* s1=strrchr(p,'/'), *s2=strrchr(p,'\\'), *s=s1>s2? s1 : s2;
  const char* base= s? s+1 : p;
  strncpy(g_exename,base,255); char* dot=strrchr(g_exename,'.'); if(dot && (!strcmp(dot,".exe")||!strcmp(dot,".EXE"))) *dot=0;
  if(s){ s[1]=0; strncpy(g_exedir,p,1023); } else g_exedir[0]=0;
}

int swr_main(int argc, char** argv){
  split_path(argv[0]);
  char ini[1400];
  snprintf(ini,sizeof ini,"%s%s.ini",g_exedir,g_exename); ini_open(ini);
  for(int i=1;i<argc;i++){ if(!strcmp(argv[i],"-v")) g_verbose=2; else if(!strncmp(argv[i],"--",2) && strchr(argv[i],'=')){ char k[256]; strncpy(k,argv[i]+2,255); char* e=strchr(k,'='); *e=0; backend_ini(k,e+1); if(!strcmp(k,"docked")) g_opt_docked=atoi(e+1); if(!strcmp(k,"fps_limit")) g_opt_fps=atoi(e+1); } }
  snprintf(g_datadir,sizeof g_datadir,"%s%s_data/",g_exedir,g_exename);
  snprintf(g_hints_path,sizeof g_hints_path,"%s%s.hints",g_exedir,g_exename);
  pm_init(&g_hint_lock); pm_init(&g_flock);
  plat_install_crash_handler();
  g_t0=plat_ns();
  LOG("SwitchRecomp - %s\n",g_exename);
  if(load_code()) fatal("Could not load the game's recompiled code.");
  if(g_mod->abi!=1) fatal("The recompiled code is from another SwitchRecomp version.");
  g_title_id=g_mod->title_id;
  mem_init(); kernel_init(); build_dispatch();
  if(g_mod->init) g_mod->init(&g_rtapi);
  load_hints();
  if(loader_load()) fatal("Could not load the game executables from %sexefs/",g_datadir);
  { /* npdm SystemResourceSize; newer SDKs only init their VM allocator when > 0 */
    extern u64 g_sysres_size; char q[1200]; snprintf(q,sizeof q,"%sexefs/main.npdm",g_datadir); FILE* f=fopen(q,"rb");
    if(f){ u8 h[0x20]; if(fread(h,1,0x20,f)==0x20 && !memcmp(h,"META",4)){ u32 v; memcpy(&v,h+0x14,4); g_sysres_size=v; } fclose(f); }
    LOG("npdm: system resource %.1f MB\n",g_sysres_size/1048576.0); }
  services_init(); romfs_init(); gpu_init();
  video_init(g_mod->title? g_mod->title : g_exename);
  { extern void plat_prof_start(int); if(g_opt_prof){ plat_prof_start(0); LOG("profile: active (profile.txt every 10 s)\n"); } }
  kernel_run_main(g_mod->mods[0].base+g_mod->mods[0].text_off,g_mod->main_stack?g_mod->main_stack:0x100000,(int)g_mod->main_prio,(int)g_mod->main_core);
  /* 60Hz frontend loop */
  u64 next=plat_ns(); int tick=0;
  while(!g_quit){
    video_poll(); hid_update(); vsync_tick_vt();
    if(tick%60==0) ini_poll();
    { extern void kernel_heal_mutexes(void); kernel_heal_mutexes(); }
    /* bail out of the slow clock if nothing was presented for 3s */
    { static int lf=-1, still; if(g_vt_on){ if(g_frames!=lf){ lf=g_frames; still=0; } else if(++still==180){ vt_set(0); WARN("smooth recording: the game stopped presenting images; back to the normal clock (recording continues)\n"); } } else { lf=-1; still=0; } }
    frontend_frame();
    if(++tick%300==0) save_hints();
    /* dump thread states to threads.txt every 15s, useful when something hangs */
    if(tick%900==0){ extern FILE* volatile g_log_redirect; extern void kernel_dump_threads(void); char p[1200]; snprintf(p,sizeof p,"%sthreads.txt",g_exedir);
      FILE* f=fopen(p,"w"); if(f){ fprintf(f,"second %d, frames %d\n",tick/60,g_frames); g_log_redirect=f; kernel_dump_threads(); g_log_redirect=NULL; fclose(f); } }
    /* auto diag only with diag=1, F5 always works */
    { extern int g_opt_diag; if(g_opt_diag && (tick==60*20 || tick==60*40)){ extern void gpu_gl_dump_rts(void); gpu_gl_dump_rts(); } }
    { static int last_frames=-1, still=0, dumps=0; if(g_frames!=last_frames){ last_frames=g_frames; still=0; }
      else if(++still==60*15 && dumps<4){ dumps++; still=0; extern void kernel_dump_threads(void);
        LOG("warning: %d s without a new image (frames=%d, GPU submits=%llu)\n",15,g_frames,(unsigned long long)g_gpu_submits); kernel_dump_threads(); } }
    next+=16666667ull; u64 now=plat_ns(); if(next>now) plat_sleep_ns((i64)(next-now)); else if(now-next>100000000ull) next=now;
  }
  save_hints(); { extern void gpu_gl_rec_finish(void); gpu_gl_rec_finish(); } LOG("Exit.\n"); fflush(NULL);
#if defined(_WIN32)
  TerminateProcess(GetCurrentProcess(),0);   /* don't join the guest threads */
#endif
  _exit(0);
}

#if !defined(_WIN32)
#include <dlfcn.h>
int dladdr_name(void* pc, char* out, int n){ Dl_info i; if(dladdr(pc,&i) && i.dli_sname){ snprintf(out,n,"%s+%lx",i.dli_sname,(unsigned long)((u8*)pc-(u8*)i.dli_saddr)); return 1; } return 0; }
#endif

/* ============================================================ audio (muted) */
/* audren RequestUpdate, answered in the real format but no sound is produced */
/* RequestUpdate layout as in yuzu's InfoUpdater.
   header (in and out): revision, behaviour, memory_pools, voices, voice_resources, effects, mixes, sinks,
   performance, splitter, render_info, pad[4], total. */
int audren_update(SvcObj* o, IpcCtx* c){
  u64 ia,is,oa,os; buf_in(c,0,&ia,&is); buf_out(c,0,&oa,&os);
  if(!ia || !oa || is<0x40) return 0;
  u8* in=GP(ia); u8* out=GP(oa);
  u32 ih[16]; memcpy(ih,in,64);
  u32 rev=ih[0]; int revn=(int)((rev>>24)&0xFF)-'0'; if(revn<0) revn=0;
  u32 behav=ih[1], mp=ih[2], vo=ih[3], vr=ih[4], ef=ih[5];
  u32 npools=mp/0x20, nvoices=vo/0x170, neff=ef/0xC0, nsinks=ih[7]/0x140;
  u8* p=in+0x40+behav;
  u8* pools_in=p; p+=mp; p+=vr; u8* voices_in=p; p+=vo; u8* eff_in=p;
  int eff_v2 = revn>=9;
  u32 o_mp=npools*0x10, o_vo=nvoices*0x10, o_ef=neff*(eff_v2? 0x90 : 0x10), o_sk=nsinks*0x20, o_pf=0x10, o_bh=0xB0, o_ri= revn>=5? 0x10 : 0;
  u32 oh[16]; memset(oh,0,sizeof oh);
  oh[0]=rev; oh[1]=o_bh; oh[2]=o_mp; oh[3]=o_vo; oh[4]=0; oh[5]=o_ef; oh[6]=0; oh[7]=o_sk; oh[8]=o_pf; oh[9]=0; oh[10]=o_ri;
  oh[15]=0x40+o_mp+o_vo+o_ef+o_sk+o_pf+o_bh+o_ri;
  if(oh[15]>os){ WARN("audren: output buffer too small (%u > %llu)\n",oh[15],(unsigned long long)os); return RC(153,4); }
  static int said; if(!said){ said=1; LOG("audren: REV%d, %u pools, %u voices, %u effects, %u sinks\n",revn,npools,nvoices,neff,nsinks); }
  memset(out,0,oh[15]); memcpy(out,oh,64);
  u8* q=out+0x40;
  /* pools: RequestAttach(4)->Attached(5), RequestDetach(2)->Detached(3) */
  for(u32 i=0;i<npools;i++){ u32 st; memcpy(&st,pools_in+i*0x20+0x10,4);
    u32 ns= st==4? 5 : st==2? 3 : st; memcpy(q+i*0x10,&ns,4); }
  q+=o_mp;
  /* consume voice buffers at real playback speed: 240 samples per 5ms update at 48k, scaled by the
     voice's rate and pitch. don't consume instantly, streaming players wait for buffers to pile up */
  typedef struct { u64 played; u32 consumed, qn; i64 pos; i64 q[8]; u8 loop[8]; } VSt;
  if(!o->u[4]){ o->u[4]=(u64)(size_t)calloc(4096,sizeof(VSt)); }
  VSt* vst=(VSt*)(size_t)o->u[4];
  for(u32 i=0;i<nvoices;i++){ u8* vi=voices_in+i*0x170; u8* vout=q+i*0x10;
    u32 id; memcpy(&id,vi,4); id&=4095; u8 in_use=vi[9], is_new=vi[8], play=vi[0xA];
    VSt* v=&vst[id];
    if(is_new) memset(v,0,sizeof *v);
    if(in_use){
      /* queue new buffers (sent_to_DSP==0) */
      for(int w=0;w<4;w++){ u8* wb=vi+0x60+w*0x38; u64 addr, size; i32 st, en; memcpy(&addr,wb,8); memcpy(&size,wb+8,8); memcpy(&st,wb+0x10,4); memcpy(&en,wb+0x14,4);
        if(addr && size && !wb[0x1A]){ i64 n=(i64)en-(i64)st; if(n<=0) n=1;
          if(v->qn<8){ v->q[v->qn]=n; v->loop[v->qn]=wb[0x18]; v->qn++; } else v->consumed++; } }
      if(play==0 && v->qn){ /* Started */
        u32 rate; float pitch; memcpy(&rate,vi+0x18,4); memcpy(&pitch,vi+0x1C,4);
        if(!rate || rate>192000) rate=48000; if(!(pitch>0.0f) || pitch>8.0f) pitch=1.0f;
        i64 adv=(i64)(240.0*(double)rate*(double)pitch/48000.0+0.5); if(adv<1) adv=1;
        v->played+=(u64)adv;
        while(adv>0 && v->qn){ i64 left=v->q[0]-v->pos;
          if(adv<left){ v->pos+=adv; adv=0; break; }
          adv-=left; v->pos=0;
          if(v->loop[0]) { if(v->q[0]<=0) break; continue; }   /* looping buffers never finish */
          v->consumed++; memmove(v->q,v->q+1,(v->qn-1)*sizeof(i64)); memmove(v->loop,v->loop+1,v->qn-1); v->qn--; } } }
    memcpy(vout,&v->played,8); memcpy(vout+8,&v->consumed,4); }
  q+=o_vo;
  /* effects: Enabled(3) / Disabled(4) */
  for(u32 i=0;i<neff;i++){ u8 en=eff_in[i*0xC0+2]; q[i*(eff_v2?0x90:0x10)]= en? 3 : 4; }
  q+=o_ef; q+=o_sk; q+=o_pf; q+=o_bh;
  if(o_ri){ u64 frames=o->u[5]++; memcpy(q,&frames,8); }
  return 0;
}

/* pl:u shared fonts. We don't ship Nintendo's: use GAME_data/fonts/<Name>.bfttf|.ttf if there,
   else a system ttf (Arial, Segoe UI, DejaVu) wrapped in BFTTF. */
static const char* g_font_names[6]={"FontStandard","FontChineseSimplified","FontExtendedChineseSimplified","FontChineseTraditional","FontKorean","FontNintendoExtended"};
static u32 g_font_off[6], g_font_size[6]; static int g_fonts_ready;
static u8* read_file_all(const char* path, u64* size){
  FILE* f=fopen(path,"rb"); if(!f) return NULL; fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
  if(n<=0){ fclose(f); return NULL; } u8* d=malloc((size_t)n+4); memset(d+n,0,4); if(fread(d,1,(size_t)n,f)!=(size_t)n){ fclose(f); free(d); return NULL; } fclose(f); *size=(u64)n; return d; }
static u32 bswap32_(u32 v){ return (v>>24)|((v>>8)&0xFF00)|((v<<8)&0xFF0000)|(v<<24); }
static u8* font_fallback(u64* size){
  const char* cands[8]; int n=0; char w1[512], w2[512], w3[512];
#if defined(_WIN32)
  const char* wd=getenv("WINDIR"); if(!wd) wd="C:\\Windows";
  snprintf(w1,sizeof w1,"%s\\Fonts\\arial.ttf",wd); snprintf(w2,sizeof w2,"%s\\Fonts\\segoeui.ttf",wd); snprintf(w3,sizeof w3,"%s\\Fonts\\tahoma.ttf",wd);
  cands[n++]=w1; cands[n++]=w2; cands[n++]=w3;
#else
  (void)w1; (void)w2; (void)w3;
  cands[n++]="/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"; cands[n++]="/usr/share/fonts/TTF/DejaVuSans.ttf"; cands[n++]="/usr/share/fonts/truetype/freefont/FreeSans.ttf";
#endif
  for(int i=0;i<n;i++){ u8* d=read_file_all(cands[i],size); if(d){ LOG("pl: system font -> %s\n",cands[i]); return d; } }
  return NULL; }
static void fonts_fill(u8* shm, u64 cap){
  if(g_fonts_ready) return; g_fonts_ready=1;
  const u32 RESULT=0x7f9a0218u, MAGIC=0x36f81a1eu; u32 key=bswap32_(RESULT^MAGIC);
  u64 off=0; u8* fb=NULL; u64 fbs=0; u32 fb_off=0, fb_size=0;
  for(int i=0;i<6;i++){
    char p[1200]; u64 sz=0; u8* d=NULL; int is_bfttf=0;
    snprintf(p,sizeof p,"%sfonts/%s.bfttf",g_datadir,g_font_names[i]); d=read_file_all(p,&sz); if(d) is_bfttf=1;
    if(!d){ snprintf(p,sizeof p,"%sfonts/%s.ttf",g_datadir,g_font_names[i]); d=read_file_all(p,&sz); }
    if(!d){ /* one fallback font for every slot */
      if(fb_size){ g_font_off[i]=fb_off; g_font_size[i]=fb_size; continue; }
      if(!fb) fb=font_fallback(&fbs); if(!fb){ WARN("pl: no TTF font found; system text will not be shown\n"); return; }
      d=fb; sz=fbs; }
    else LOG("pl: %s <- %s\n",g_font_names[i],p);
    u64 words=(sz+3)/4;
    if(off+8+words*4>cap){ WARN("pl: font %s does not fit\n",g_font_names[i]); if(d!=fb) free(d); continue; }
    u32 h0=bswap32_(MAGIC), h1=bswap32_((u32)(words*4))^key; memcpy(shm+off,&h0,4); memcpy(shm+off+4,&h1,4);
    if(is_bfttf){ /* bfttf -> decrypt, shmem holds plain ttf */
      u32 w0, w1; memcpy(&w0,d,4); memcpy(&w1,d+4,4); u32 k2=w0^RESULT; u32 tsz=bswap32_(w1^k2); if(tsz>sz-8) tsz=(u32)(sz-8);
      for(u64 k=0;k+8<sz;k+=4){ u32 w; memcpy(&w,d+8+k,4); w=bswap32_(w^k2); memcpy(shm+off+8+k,&w,4); }
      g_font_off[i]=(u32)off+8; g_font_size[i]=tsz; }
    else { memcpy(shm+off+8,d,sz); g_font_off[i]=(u32)off+8; g_font_size[i]=(u32)sz; }
    if(d==fb){ fb_off=g_font_off[i]; fb_size=g_font_size[i]; }
    off=(off+8+words*4+0xF)&~0xFull;
    if(d!=fb) free(d);
  }
  if(fb) free(fb);
}
static int pl_disp(SvcObj* o, IpcCtx* c){
  static KShmem* shm; extern KShmem* kshmem_new(u64,const char*); extern u8* kshmem_ptr(KShmem*);
  if(!shm){ shm=kshmem_new(0x1100000,"font"); fonts_fill(kshmem_ptr(shm),0x1100000); }
  u32 id=in32(c,0); if(id>5) id=0;
  switch(c->cmd){
    case 0: return 0;                                                       /* RequestLoad */
    case 1: out32(c,1); return 0;                                           /* GetLoadState: loaded */
    case 2: out32(c,g_font_size[id]); return 0;                             /* GetSize */
    case 3: out32(c,g_font_off[id]); return 0;                              /* GetSharedMemoryAddressOffset */
    case 4: out_copy_h(c,h_new(&shm->o)); return 0;
    case 5: case 6: {                                                       /* GetSharedFontInOrderOfPriority */
      u64 a0,s0,a1,s1,a2,s2; buf_out(c,0,&a0,&s0); buf_out(c,1,&a1,&s1); buf_out(c,2,&a2,&s2);
      u32 n=6; for(u32 i=0;i<n;i++){ if(a0 && (i+1)*4<=s0) gw32(a0+i*4,i); if(a1 && (i+1)*4<=s1) gw32(a1+i*4,g_font_off[i]); if(a2 && (i+1)*4<=s2) gw32(a2+i*4,g_font_size[i]); }
      out8(c,1); c->out_size=4; out32(c,n); return 0; }
  }
  return stub_ok(o,c); }
static SvcObj* new_pl(void){ return svcobj_new("pl:u",pl_disp,0); }
void svc_pl_init(void){ svc_register("pl:u",new_pl); svc_register("pl:s",new_pl); }
