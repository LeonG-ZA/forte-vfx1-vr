/*
 * xrapp.h -- the OpenXR app shared by the Quest (Android) and Windows builds.
 *
 * xrapp.c holds the OpenXR session, controllers, swapchains and drawing, over
 * vfxapp (the session logic every route shares). A platform file supplies
 * only what OpenXR and OpenGL need from the OS: the GL context and its
 * OpenXR graphics binding, sound output, log output, and the main loop.
 *
 *   app/android/src/main_android.c   NativeActivity, EGL, AAudio, logcat
 *   app/windows/main_windows.c       a console program, WGL, waveOut
 */
#ifndef VFX_XRAPP_H
#define VFX_XRAPP_H

#include <openxr/openxr.h>
#include "vfx/vfxapp.h"

typedef struct XrAppPlatform {
    const char *const *extensions;      /* the graphics binding's and the platform's */
    uint32_t extension_count;
    const void *instance_next;          /* chained into XrInstanceCreateInfo */
    /* After the system is chosen and before the session: query the graphics
     * requirements and return the graphics binding to chain into
     * XrSessionCreateInfo. */
    const void *(*graphics_binding)(XrInstance instance, XrSystemId system);
    XrStructureType swapchain_image_type;   /* the ..._OPENGL_ES_KHR or _OPENGL_KHR image type */
    /* Optional: each frame, after the controllers, add the platform's own
     * input (a Bluetooth keyboard and mouse: vfxapp_add_keyboard_mouse). */
    void (*add_input)(const VfxApp *vfx, VfxAppInput *in);
} XrAppPlatform;

typedef struct XrApp XrApp;

/* With the GL context current. NULL if OpenXR could not start. */
XrApp *xrapp_create(const XrAppPlatform *platform, VfxApp *vfx);
/* Handle runtime events; returns 0 once the app should leave. */
int xrapp_poll(XrApp *x);
/* 1 while the session runs and frames should be drawn. */
int xrapp_running(const XrApp *x);
/* Wait for, emulate and draw one headset frame. */
void xrapp_frame(XrApp *x);
void xrapp_destroy(XrApp *x);

#endif
