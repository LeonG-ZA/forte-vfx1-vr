/*
 * xrgl_win.c -- loads the OpenGL past 1.1 that app/xr/xrgl.h names.
 */
#include "xrgl.h"

#define XRGL_DEFINE(type, name) type xrgl_##name;
XRGL_FUNCS(XRGL_DEFINE)
#undef XRGL_DEFINE

int xrgl_load(void)
{
    int ok = 1;
#define XRGL_LOAD(type, name) \
    xrgl_##name = (type)(void *)wglGetProcAddress(#name); \
    if (!xrgl_##name) ok = 0;
    XRGL_FUNCS(XRGL_LOAD)
#undef XRGL_LOAD
    return ok;
}
