/*
 * xrgl.h -- the OpenGL the XR apps draw with: OpenGL ES 3 on Android,
 * desktop OpenGL 3.3 on Windows, where everything past OpenGL 1.1 is loaded
 * at run time (xrgl_load, app/windows/xrgl_win.c).
 */
#ifndef VFX_XRGL_H
#define VFX_XRGL_H

#ifdef __ANDROID__
#include <GLES3/gl3.h>
#define XRGL_DESKTOP 0
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <GL/gl.h>
#include <GL/glext.h>
#define XRGL_DESKTOP 1

#define XRGL_FUNCS(X) \
    X(PFNGLCREATESHADERPROC, glCreateShader) \
    X(PFNGLSHADERSOURCEPROC, glShaderSource) \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) \
    X(PFNGLGETSHADERIVPROC, glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC, glGetShaderInfoLog) \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) \
    X(PFNGLATTACHSHADERPROC, glAttachShader) \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC, glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC, glGetProgramInfoLog) \
    X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation) \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) \
    X(PFNGLUNIFORMMATRIX4FVPROC, glUniformMatrix4fv) \
    X(PFNGLUNIFORM2FVPROC, glUniform2fv) \
    X(PFNGLUNIFORM2FPROC, glUniform2f) \
    X(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray) \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) \
    X(PFNGLBINDBUFFERPROC, glBindBuffer) \
    X(PFNGLBUFFERDATAPROC, glBufferData) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC, glEnableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC, glVertexAttribPointer) \
    X(PFNGLGENFRAMEBUFFERSPROC, glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC, glFramebufferTexture2D)

#define XRGL_DECLARE(type, name) extern type xrgl_##name;
XRGL_FUNCS(XRGL_DECLARE)
#undef XRGL_DECLARE
#define glCreateShader xrgl_glCreateShader
#define glShaderSource xrgl_glShaderSource
#define glCompileShader xrgl_glCompileShader
#define glGetShaderiv xrgl_glGetShaderiv
#define glGetShaderInfoLog xrgl_glGetShaderInfoLog
#define glCreateProgram xrgl_glCreateProgram
#define glAttachShader xrgl_glAttachShader
#define glLinkProgram xrgl_glLinkProgram
#define glGetProgramiv xrgl_glGetProgramiv
#define glGetProgramInfoLog xrgl_glGetProgramInfoLog
#define glGetUniformLocation xrgl_glGetUniformLocation
#define glUseProgram xrgl_glUseProgram
#define glUniformMatrix4fv xrgl_glUniformMatrix4fv
#define glUniform2fv xrgl_glUniform2fv
#define glUniform2f xrgl_glUniform2f
#define glGenVertexArrays xrgl_glGenVertexArrays
#define glBindVertexArray xrgl_glBindVertexArray
#define glGenBuffers xrgl_glGenBuffers
#define glBindBuffer xrgl_glBindBuffer
#define glBufferData xrgl_glBufferData
#define glEnableVertexAttribArray xrgl_glEnableVertexAttribArray
#define glVertexAttribPointer xrgl_glVertexAttribPointer
#define glGenFramebuffers xrgl_glGenFramebuffers
#define glBindFramebuffer xrgl_glBindFramebuffer
#define glFramebufferTexture2D xrgl_glFramebufferTexture2D

/* 1 when every function loaded; needs a current context. */
int xrgl_load(void);
#endif

#endif
