/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* GL function pointer loading */
#define SWR_GL_NOMACROS
#include "glfn.h"
#include <stddef.h>
#define X(T,n) T p_##n;
SWR_GL_FUNCS(X)
#undef X
PFNGLCLIPCONTROLPROC_ p_glClipControl;
PFNGLPROGRAMBINARYPROC p_glProgramBinary; PFNGLGETPROGRAMBINARYPROC p_glGetProgramBinary; PFNGLPROGRAMPARAMETERIPROC p_glProgramParameteri;
/* ---------- GLES 3.2: map desktop-only calls to something equivalent */
#include <string.h>
#include <stdio.h>
static int g_is_gles;
static PFNGLVIEWPORTPROC o_vp; static PFNGLSCISSORPROC o_sc; static void (APIENTRY*o_dr)(GLfloat,GLfloat); static void (APIENTRY*o_cd)(GLfloat);
static PFNGLDRAWBUFFERSPROC o_dbs; static void (APIENTRY*o_deibv)(GLenum,GLsizei,GLenum,const void*,GLsizei,GLint); static void (APIENTRY*o_dai)(GLenum,GLint,GLsizei,GLsizei);
static PFNGLENABLEPROC o_en; static PFNGLDISABLEPROC o_dis; static PFNGLTEXPARAMETERIPROC o_tpi; static PFNGLTEXPARAMETERIVPROC o_tpiv;
static void APIENTRY f_vpi(GLuint i, GLfloat x, GLfloat y, GLfloat w, GLfloat h){ if(!i) o_vp((GLint)x,(GLint)y,(GLsizei)w,(GLsizei)h); }
static void APIENTRY f_sci(GLuint i, GLint x, GLint y, GLsizei w, GLsizei h){ if(!i) o_sc(x,y,w,h); }
static void APIENTRY f_dri(GLuint i, GLdouble n, GLdouble f){ if(!i && o_dr) o_dr((GLfloat)n,(GLfloat)f); }
static void APIENTRY f_cd(GLdouble d){ if(o_cd) o_cd((GLfloat)d); }
static void APIENTRY f_nop_ee(GLenum a, GLenum b){ (void)a;(void)b; }
static void APIENTRY f_nop_f(GLfloat a){ (void)a; }
static void APIENTRY f_nop_u(GLuint a){ (void)a; }
static void APIENTRY f_nop_e(GLenum a){ (void)a; }
static void APIENTRY f_nop_uuu(GLuint a, GLuint b, GLuint c){ (void)a;(void)b;(void)c; }
static void APIENTRY f_gti(GLenum t, GLint l, GLenum f, GLenum ty, void* p){ (void)t;(void)l;(void)f;(void)ty;(void)p; }
static void APIENTRY f_gbs(GLenum t, GLintptr o, GLsizeiptr n, void* p){ (void)t;(void)o; memset(p,0,(size_t)n); }
static void APIENTRY f_db(GLenum b){ o_dbs(1,&b); }
static void APIENTRY f_deibvbi(GLenum m, GLsizei c, GLenum t, const void* i, GLsizei n, GLint bv, GLuint bi){ (void)bi; o_deibv(m,c,t,i,n,bv); }
static void APIENTRY f_daibi(GLenum m, GLint f, GLsizei c, GLsizei n, GLuint bi){ (void)bi; o_dai(m,f,c,n); }
static void APIENTRY f_lop(GLenum a){ (void)a; }
static int gles_desktop_only(const char* n){
  static const char* l[]={"glViewportIndexedf","glScissorIndexed","glDepthRangeIndexed","glClearDepth","glDrawBuffer","glPolygonMode","glPointSize","glPrimitiveRestartIndex",
    "glGetTexImage","glGetBufferSubData","glProvokingVertex","glShaderStorageBlockBinding","glLogicOp","glDrawElementsInstancedBaseVertexBaseInstance","glDrawArraysInstancedBaseInstance",NULL};
  for(int i=0;l[i];i++) if(!strcmp(l[i],n)) return 1; return 0; }
static int gles_fallback(const char* n, void** p, void* (*gp)(const char*)){
  if(!g_is_gles) return 0;
  *p=NULL;
  o_vp=(PFNGLVIEWPORTPROC)gp("glViewport"); o_sc=(PFNGLSCISSORPROC)gp("glScissor"); o_dr=(void (APIENTRY*)(GLfloat,GLfloat))gp("glDepthRangef"); o_cd=(void (APIENTRY*)(GLfloat))gp("glClearDepthf");
  o_dbs=(PFNGLDRAWBUFFERSPROC)gp("glDrawBuffers"); o_deibv=(void (APIENTRY*)(GLenum,GLsizei,GLenum,const void*,GLsizei,GLint))gp("glDrawElementsInstancedBaseVertex");
  o_dai=(void (APIENTRY*)(GLenum,GLint,GLsizei,GLsizei))gp("glDrawArraysInstanced");
  if(!strcmp(n,"glViewportIndexedf")) *p=(void*)f_vpi; else if(!strcmp(n,"glScissorIndexed")) *p=(void*)f_sci;
  else if(!strcmp(n,"glDepthRangeIndexed")) *p=(void*)f_dri; else if(!strcmp(n,"glClearDepth")) *p=(void*)f_cd;
  else if(!strcmp(n,"glPolygonMode")) *p=(void*)f_nop_ee; else if(!strcmp(n,"glPointSize")) *p=(void*)f_nop_f;
  else if(!strcmp(n,"glPrimitiveRestartIndex")) *p=(void*)f_nop_u; else if(!strcmp(n,"glLogicOp")) *p=(void*)f_lop;
  else if(!strcmp(n,"glGetTexImage")) *p=(void*)f_gti; else if(!strcmp(n,"glGetBufferSubData")) *p=(void*)f_gbs;
  else if(!strcmp(n,"glDrawBuffer")) *p=(void*)f_db; else if(!strcmp(n,"glProvokingVertex")) *p=(void*)f_nop_e;
  else if(!strcmp(n,"glShaderStorageBlockBinding")) *p=(void*)f_nop_uuu;
  else if(!strcmp(n,"glDrawElementsInstancedBaseVertexBaseInstance") && o_deibv) *p=(void*)f_deibvbi;
  else if(!strcmp(n,"glDrawArraysInstancedBaseInstance") && o_dai) *p=(void*)f_daibi;
  return *p!=NULL; }
/* GLES always has fixed-index primitive restart; drop caps it doesn't know (FRAMEBUFFER_SRGB etc) */
static void APIENTRY w_en(GLenum c){ if(c==0x8F9D) c=0x8D69; else if(c==0x8DB9 || c==0x8642 || c==0x809D || c==0x2A02 || c==0x92E0 || c==0x0BF2) return; o_en(c); }
static void APIENTRY w_dis(GLenum c){ if(c==0x8F9D) c=0x8D69; else if(c==0x8DB9 || c==0x8642 || c==0x809D || c==0x2A02 || c==0x92E0 || c==0x0BF2) return; o_dis(c); }
static void APIENTRY w_tpiv(GLenum t, GLenum pn, const GLint* v){
  if(pn==0x8E46){ o_tpi(t,0x8E42,v[0]); o_tpi(t,0x8E43,v[1]); o_tpi(t,0x8E44,v[2]); o_tpi(t,0x8E45,v[3]); return; }   /* SWIZZLE_RGBA */
  o_tpiv(t,pn,v); }
static void gles_wrap(void* (*gp)(const char*)){ (void)gp;
  o_en=p_glEnable; p_glEnable=w_en; o_dis=p_glDisable; p_glDisable=w_dis;
  o_tpi=p_glTexParameteri; o_tpiv=p_glTexParameteriv; p_glTexParameteriv=w_tpiv; }
int swr_gl_is_gles(void){ return g_is_gles; }

int swr_gl_load(void* (*gp)(const char*)){
  int ok=1;
#if defined(_WIN32)
  HMODULE gl=LoadLibraryA("opengl32.dll");
#define GETP(n) (gp(n)? gp(n) : (void*)GetProcAddress(gl,n))
#else
#define GETP(n) gp(n)
#endif
#define X(T,n) p_##n=(T)GETP(#n); if(g_is_gles && gles_desktop_only(#n)) p_##n=NULL; if(!p_##n){ if(!gles_fallback(#n,(void**)&p_##n,gp)) ok=0; }
  { const char* v=(const char*)(p_glGetString=(PFNGLGETSTRINGPROC)GETP("glGetString"))? (const char*)p_glGetString(GL_VERSION) : NULL; g_is_gles= v && strstr(v,"OpenGL ES"); }
  SWR_GL_FUNCS(X)
#undef X
  if(g_is_gles) gles_wrap(gp);
  p_glClipControl=(PFNGLCLIPCONTROLPROC_)GETP("glClipControl");
  p_glProgramBinary=(PFNGLPROGRAMBINARYPROC)GETP("glProgramBinary"); p_glGetProgramBinary=(PFNGLGETPROGRAMBINARYPROC)GETP("glGetProgramBinary"); p_glProgramParameteri=(PFNGLPROGRAMPARAMETERIPROC)GETP("glProgramParameteri");
  return ok;
}
