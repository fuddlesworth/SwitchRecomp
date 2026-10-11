/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.txt and NOTICE.txt;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs. */
/* GL 4.3 entry points, loaded at runtime so we don't need any loader lib */
#ifndef SWR_GLFN_H
#define SWR_GLFN_H
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#define GL_GLEXT_PROTOTYPES_NOT
#include <GL/glcorearb.h>

#define SWR_GL_FUNCS(X) \
 X(PFNGLGETSTRINGPROC,glGetString) X(PFNGLGETINTEGERVPROC,glGetIntegerv) X(PFNGLGETERRORPROC,glGetError) \
 X(PFNGLENABLEPROC,glEnable) X(PFNGLDISABLEPROC,glDisable) X(PFNGLENABLEIPROC,glEnablei) X(PFNGLDISABLEIPROC,glDisablei) \
 X(PFNGLVIEWPORTPROC,glViewport) X(PFNGLVIEWPORTINDEXEDFPROC,glViewportIndexedf) X(PFNGLSCISSORPROC,glScissor) X(PFNGLSCISSORINDEXEDPROC,glScissorIndexed) \
 X(PFNGLDEPTHRANGEINDEXEDPROC,glDepthRangeIndexed) \
 X(PFNGLCLEARPROC,glClear) X(PFNGLCLEARCOLORPROC,glClearColor) X(PFNGLCLEARDEPTHPROC,glClearDepth) X(PFNGLCLEARSTENCILPROC,glClearStencil) \
 X(PFNGLCLEARBUFFERFVPROC,glClearBufferfv) X(PFNGLCLEARBUFFERIVPROC,glClearBufferiv) X(PFNGLCLEARBUFFERUIVPROC,glClearBufferuiv) X(PFNGLCLEARBUFFERFIPROC,glClearBufferfi) \
 X(PFNGLCOLORMASKIPROC,glColorMaski) X(PFNGLCOLORMASKPROC,glColorMask) X(PFNGLDEPTHMASKPROC,glDepthMask) X(PFNGLSTENCILMASKSEPARATEPROC,glStencilMaskSeparate) \
 X(PFNGLDEPTHFUNCPROC,glDepthFunc) X(PFNGLSTENCILFUNCSEPARATEPROC,glStencilFuncSeparate) X(PFNGLSTENCILOPSEPARATEPROC,glStencilOpSeparate) \
 X(PFNGLBLENDFUNCSEPARATEIPROC,glBlendFuncSeparatei) X(PFNGLBLENDEQUATIONSEPARATEIPROC,glBlendEquationSeparatei) X(PFNGLBLENDCOLORPROC,glBlendColor) \
 X(PFNGLCULLFACEPROC,glCullFace) X(PFNGLFRONTFACEPROC,glFrontFace) X(PFNGLPOLYGONMODEPROC,glPolygonMode) X(PFNGLPOLYGONOFFSETPROC,glPolygonOffset) \
 X(PFNGLLINEWIDTHPROC,glLineWidth) X(PFNGLPOINTSIZEPROC,glPointSize) X(PFNGLPRIMITIVERESTARTINDEXPROC,glPrimitiveRestartIndex) X(PFNGLLOGICOPPROC,glLogicOp) \
 X(PFNGLGENTEXTURESPROC,glGenTextures) X(PFNGLDELETETEXTURESPROC,glDeleteTextures) X(PFNGLBINDTEXTUREPROC,glBindTexture) X(PFNGLACTIVETEXTUREPROC,glActiveTexture) \
 X(PFNGLTEXIMAGE2DPROC,glTexImage2D) X(PFNGLTEXSUBIMAGE2DPROC,glTexSubImage2D) X(PFNGLTEXSUBIMAGE3DPROC,glTexSubImage3D) \
 X(PFNGLTEXSTORAGE2DPROC,glTexStorage2D) X(PFNGLTEXSTORAGE3DPROC,glTexStorage3D) X(PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC,glCompressedTexSubImage2D) \
 X(PFNGLCOMPRESSEDTEXSUBIMAGE3DPROC,glCompressedTexSubImage3D) X(PFNGLTEXPARAMETERIPROC,glTexParameteri) X(PFNGLTEXPARAMETERIVPROC,glTexParameteriv) X(PFNGLPIXELSTOREIPROC,glPixelStorei) \
 X(PFNGLGENSAMPLERSPROC,glGenSamplers) X(PFNGLBINDSAMPLERPROC,glBindSampler) X(PFNGLSAMPLERPARAMETERIPROC,glSamplerParameteri) \
 X(PFNGLSAMPLERPARAMETERFPROC,glSamplerParameterf) X(PFNGLSAMPLERPARAMETERFVPROC,glSamplerParameterfv) \
 X(PFNGLGENBUFFERSPROC,glGenBuffers) X(PFNGLDELETEBUFFERSPROC,glDeleteBuffers) X(PFNGLBINDBUFFERPROC,glBindBuffer) X(PFNGLBUFFERDATAPROC,glBufferData) \
 X(PFNGLBUFFERSUBDATAPROC,glBufferSubData) X(PFNGLBINDBUFFERRANGEPROC,glBindBufferRange) X(PFNGLBINDBUFFERBASEPROC,glBindBufferBase) \
 X(PFNGLGENVERTEXARRAYSPROC,glGenVertexArrays) X(PFNGLBINDVERTEXARRAYPROC,glBindVertexArray) \
 X(PFNGLENABLEVERTEXATTRIBARRAYPROC,glEnableVertexAttribArray) X(PFNGLDISABLEVERTEXATTRIBARRAYPROC,glDisableVertexAttribArray) \
 X(PFNGLVERTEXATTRIBPOINTERPROC,glVertexAttribPointer) X(PFNGLVERTEXATTRIBIPOINTERPROC,glVertexAttribIPointer) X(PFNGLVERTEXATTRIBDIVISORPROC,glVertexAttribDivisor) \
 X(PFNGLVERTEXATTRIB4FPROC,glVertexAttrib4f) \
 X(PFNGLDRAWARRAYSINSTANCEDBASEINSTANCEPROC,glDrawArraysInstancedBaseInstance) X(PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXBASEINSTANCEPROC,glDrawElementsInstancedBaseVertexBaseInstance) \
 X(PFNGLDRAWARRAYSPROC,glDrawArrays) \
 X(PFNGLCREATESHADERPROC,glCreateShader) X(PFNGLSHADERSOURCEPROC,glShaderSource) X(PFNGLCOMPILESHADERPROC,glCompileShader) X(PFNGLGETSHADERIVPROC,glGetShaderiv) \
 X(PFNGLGETSHADERINFOLOGPROC,glGetShaderInfoLog) X(PFNGLCREATEPROGRAMPROC,glCreateProgram) X(PFNGLATTACHSHADERPROC,glAttachShader) X(PFNGLLINKPROGRAMPROC,glLinkProgram) \
 X(PFNGLGETPROGRAMIVPROC,glGetProgramiv) X(PFNGLGETPROGRAMINFOLOGPROC,glGetProgramInfoLog) X(PFNGLUSEPROGRAMPROC,glUseProgram) X(PFNGLDELETESHADERPROC,glDeleteShader) \
 X(PFNGLGETUNIFORMLOCATIONPROC,glGetUniformLocation) X(PFNGLUNIFORM1IPROC,glUniform1i) X(PFNGLUNIFORM4FVPROC,glUniform4fv) X(PFNGLUNIFORM1FPROC,glUniform1f) X(PFNGLUNIFORM4IVPROC,glUniform4iv) X(PFNGLUNIFORM1IVPROC,glUniform1iv) \
 X(PFNGLGETUNIFORMBLOCKINDEXPROC,glGetUniformBlockIndex) X(PFNGLUNIFORMBLOCKBINDINGPROC,glUniformBlockBinding) \
 X(PFNGLGENFRAMEBUFFERSPROC,glGenFramebuffers) X(PFNGLDELETEFRAMEBUFFERSPROC,glDeleteFramebuffers) X(PFNGLBINDFRAMEBUFFERPROC,glBindFramebuffer) \
 X(PFNGLFRAMEBUFFERTEXTURE2DPROC,glFramebufferTexture2D) X(PFNGLFRAMEBUFFERTEXTURELAYERPROC,glFramebufferTextureLayer) X(PFNGLFRAMEBUFFERTEXTUREPROC,glFramebufferTexture) X(PFNGLCHECKFRAMEBUFFERSTATUSPROC,glCheckFramebufferStatus) \
 X(PFNGLDRAWBUFFERSPROC,glDrawBuffers) X(PFNGLREADBUFFERPROC,glReadBuffer) X(PFNGLBLITFRAMEBUFFERPROC,glBlitFramebuffer) X(PFNGLREADPIXELSPROC,glReadPixels) \
 X(PFNGLFINISHPROC,glFinish) X(PFNGLFLUSHPROC,glFlush) X(PFNGLGETTEXIMAGEPROC,glGetTexImage) X(PFNGLCOPYIMAGESUBDATAPROC,glCopyImageSubData) \
 X(PFNGLDISPATCHCOMPUTEPROC,glDispatchCompute) X(PFNGLMEMORYBARRIERPROC,glMemoryBarrier) X(PFNGLBINDIMAGETEXTUREPROC,glBindImageTexture) \
 X(PFNGLGETPROGRAMRESOURCEINDEXPROC,glGetProgramResourceIndex) X(PFNGLSHADERSTORAGEBLOCKBINDINGPROC,glShaderStorageBlockBinding) \
 X(PFNGLDRAWBUFFERPROC,glDrawBuffer) X(PFNGLTEXPARAMETERFPROC,glTexParameterf) X(PFNGLGETBUFFERSUBDATAPROC,glGetBufferSubData) \
 X(PFNGLPROVOKINGVERTEXPROC,glProvokingVertex) X(PFNGLSAMPLECOVERAGEPROC,glSampleCoverage) X(PFNGLDELETEPROGRAMPROC,glDeleteProgram) \
 X(PFNGLCOPYBUFFERSUBDATAPROC,glCopyBufferSubData) X(PFNGLPROGRAMUNIFORM4FVPROC,glProgramUniform4fv) X(PFNGLISENABLEDPROC,glIsEnabled) X(PFNGLGETBOOLEANVPROC,glGetBooleanv) X(PFNGLGENERATEMIPMAPPROC,glGenerateMipmap)

#define X(T,n) extern T p_##n;
SWR_GL_FUNCS(X)
#undef X
typedef void (APIENTRYP PFNGLCLIPCONTROLPROC_)(GLenum origin, GLenum depth);
extern PFNGLCLIPCONTROLPROC_ p_glClipControl;
extern PFNGLPROGRAMBINARYPROC p_glProgramBinary; extern PFNGLGETPROGRAMBINARYPROC p_glGetProgramBinary; extern PFNGLPROGRAMPARAMETERIPROC p_glProgramParameteri;
#ifndef SWR_GL_NOMACROS
#define glGetString p_glGetString
#define glGetIntegerv p_glGetIntegerv
#define glGetError p_glGetError
#define glEnable p_glEnable
#define glDisable p_glDisable
#define glEnablei p_glEnablei
#define glDisablei p_glDisablei
#define glViewport p_glViewport
#define glViewportIndexedf p_glViewportIndexedf
#define glScissor p_glScissor
#define glScissorIndexed p_glScissorIndexed
#define glDepthRangeIndexed p_glDepthRangeIndexed
#define glClear p_glClear
#define glClearColor p_glClearColor
#define glClearDepth p_glClearDepth
#define glClearStencil p_glClearStencil
#define glClearBufferfv p_glClearBufferfv
#define glClearBufferiv p_glClearBufferiv
#define glClearBufferuiv p_glClearBufferuiv
#define glClearBufferfi p_glClearBufferfi
#define glColorMaski p_glColorMaski
#define glColorMask p_glColorMask
#define glDepthMask p_glDepthMask
#define glStencilMaskSeparate p_glStencilMaskSeparate
#define glDepthFunc p_glDepthFunc
#define glStencilFuncSeparate p_glStencilFuncSeparate
#define glStencilOpSeparate p_glStencilOpSeparate
#define glBlendFuncSeparatei p_glBlendFuncSeparatei
#define glBlendEquationSeparatei p_glBlendEquationSeparatei
#define glBlendColor p_glBlendColor
#define glCullFace p_glCullFace
#define glFrontFace p_glFrontFace
#define glPolygonMode p_glPolygonMode
#define glPolygonOffset p_glPolygonOffset
#define glLineWidth p_glLineWidth
#define glPointSize p_glPointSize
#define glPrimitiveRestartIndex p_glPrimitiveRestartIndex
#define glLogicOp p_glLogicOp
#define glGenTextures p_glGenTextures
#define glDeleteTextures p_glDeleteTextures
#define glBindTexture p_glBindTexture
#define glActiveTexture p_glActiveTexture
#define glTexImage2D p_glTexImage2D
#define glTexSubImage2D p_glTexSubImage2D
#define glTexSubImage3D p_glTexSubImage3D
#define glTexStorage2D p_glTexStorage2D
#define glTexStorage3D p_glTexStorage3D
#define glCompressedTexSubImage2D p_glCompressedTexSubImage2D
#define glCompressedTexSubImage3D p_glCompressedTexSubImage3D
#define glTexParameteri p_glTexParameteri
#define glTexParameteriv p_glTexParameteriv
#define glTexParameterf p_glTexParameterf
#define glPixelStorei p_glPixelStorei
#define glGenSamplers p_glGenSamplers
#define glBindSampler p_glBindSampler
#define glSamplerParameteri p_glSamplerParameteri
#define glSamplerParameterf p_glSamplerParameterf
#define glSamplerParameterfv p_glSamplerParameterfv
#define glGenBuffers p_glGenBuffers
#define glDeleteBuffers p_glDeleteBuffers
#define glBindBuffer p_glBindBuffer
#define glBufferData p_glBufferData
#define glBufferSubData p_glBufferSubData
#define glBindBufferRange p_glBindBufferRange
#define glBindBufferBase p_glBindBufferBase
#define glGenVertexArrays p_glGenVertexArrays
#define glBindVertexArray p_glBindVertexArray
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glDisableVertexAttribArray p_glDisableVertexAttribArray
#define glVertexAttribPointer p_glVertexAttribPointer
#define glVertexAttribIPointer p_glVertexAttribIPointer
#define glVertexAttribDivisor p_glVertexAttribDivisor
#define glVertexAttrib4f p_glVertexAttrib4f
#define glDrawArraysInstancedBaseInstance p_glDrawArraysInstancedBaseInstance
#define glDrawElementsInstancedBaseVertexBaseInstance p_glDrawElementsInstancedBaseVertexBaseInstance
#define glDrawArrays p_glDrawArrays
#define glCreateShader p_glCreateShader
#define glShaderSource p_glShaderSource
#define glCompileShader p_glCompileShader
#define glGetShaderiv p_glGetShaderiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glCreateProgram p_glCreateProgram
#define glAttachShader p_glAttachShader
#define glLinkProgram p_glLinkProgram
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glUseProgram p_glUseProgram
#define glDeleteShader p_glDeleteShader
#define glDeleteProgram p_glDeleteProgram
#define glGetUniformLocation p_glGetUniformLocation
#define glUniform1i p_glUniform1i
#define glUniform4fv p_glUniform4fv
#define glUniform4iv p_glUniform4iv
#define glUniform1iv p_glUniform1iv
#define glUniform1f p_glUniform1f
#define glGetUniformBlockIndex p_glGetUniformBlockIndex
#define glUniformBlockBinding p_glUniformBlockBinding
#define glGenFramebuffers p_glGenFramebuffers
#define glDeleteFramebuffers p_glDeleteFramebuffers
#define glBindFramebuffer p_glBindFramebuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glFramebufferTextureLayer p_glFramebufferTextureLayer
#define glFramebufferTexture p_glFramebufferTexture
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glDrawBuffers p_glDrawBuffers
#define glDrawBuffer p_glDrawBuffer
#define glReadBuffer p_glReadBuffer
#define glBlitFramebuffer p_glBlitFramebuffer
#define glGenerateMipmap p_glGenerateMipmap
#define glReadPixels p_glReadPixels
#define glFinish p_glFinish
#define glFlush p_glFlush
#define glGetTexImage p_glGetTexImage
#define glCopyImageSubData p_glCopyImageSubData
#define glDispatchCompute p_glDispatchCompute
#define glMemoryBarrier p_glMemoryBarrier
#define glBindImageTexture p_glBindImageTexture
#define glGetProgramResourceIndex p_glGetProgramResourceIndex
#define glShaderStorageBlockBinding p_glShaderStorageBlockBinding
#define glGetBufferSubData p_glGetBufferSubData
#define glProvokingVertex p_glProvokingVertex
#define glSampleCoverage p_glSampleCoverage
#define glCopyBufferSubData p_glCopyBufferSubData
#define glProgramUniform4fv p_glProgramUniform4fv
#define glIsEnabled p_glIsEnabled
#define glGetBooleanv p_glGetBooleanv
#define glClipControl p_glClipControl
#endif
int swr_gl_load(void* (*getproc)(const char*));
int swr_gl_is_gles(void);
#endif
