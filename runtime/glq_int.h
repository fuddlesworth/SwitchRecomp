/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* internal glq stuff, see glq.c */
#ifndef SWR_GLQ_INT_H
#define SWR_GLQ_INT_H
#include "rt.h"
#define SWR_GL_NOMACROS
#include "glfn.h"
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
static inline size_t q_al16(size_t n) { return (n + 15) & ~(size_t)15; }
#endif
