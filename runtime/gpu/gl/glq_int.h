/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* internal glq stuff, see glq.c */
#ifndef SWR_GLQ_INT_H
#define SWR_GLQ_INT_H
#include "abi/cpu.h"
#define SWR_GL_NOMACROS
#include "gpu/gl/glfn.h"
#include <string.h>
#include <stdlib.h>
extern __thread int
    t_glq_direct; /* set on threads that talk to GL directly (GL thread, shader workers, or with the queue disabled) */
typedef struct {
    const char* name;
    void** real;
    void* q;
    void** pvar;
} GlqEnt;
extern const GlqEnt g_glq_tab[];
void* glq_alloc(size_t sz, size_t extra, void (*ex)(void*));
void glq_commit(void);
void glq_sync(void (*ex)(void*), void* arg);
size_t q_img_size(GLsizei w, GLsizei h, GLsizei d, GLenum f, GLenum t);
size_t q_texel_size(GLenum f, GLenum t);
int q_unpack_pbo(void);
/* the wrappers glq.c writes by hand, and the real entry points they call (glq_gen.c owns the pointers) */
extern void(APIENTRY* r_glPixelStorei)(GLenum, GLint);
extern void(APIENTRY* r_glBindBuffer)(GLenum, GLuint);
extern GLsync(APIENTRY* r_glFenceSync)(GLenum, GLbitfield);
extern GLenum(APIENTRY* r_glClientWaitSync)(GLsync, GLbitfield, GLuint64);
extern void(APIENTRY* r_glDeleteSync)(GLsync);
void APIENTRY q_glPixelStorei(GLenum pname, GLint param);
void APIENTRY q_glBindBuffer(GLenum target, GLuint buffer);
GLsync APIENTRY q_glFenceSync(GLenum condition, GLbitfield flags);
GLenum APIENTRY q_glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
void APIENTRY q_glDeleteSync(GLsync sync);
static inline size_t q_al16(size_t n) { return (n + 15) & ~(size_t)15; }
#endif
