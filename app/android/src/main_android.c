/*
 * main_android.c -- the Quest edges of the OpenXR app (app/xr/xrapp.h):
 * NativeActivity, EGL, AAudio, logcat and crash reports.
 *
 * Games are zip files in the app's external files directory,
 * /sdcard/Android/data/com.fortevfx.vfx1quest/files/, chosen, configured and
 * downloaded from the dev PC in the launcher menu.
 */
#define _GNU_SOURCE
#include <EGL/egl.h>
#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif
#include <GLES3/gl3.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android_native_app_glue.h>
#include <dlfcn.h>
#include <jni.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ucontext.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "libretro.h"
#include "vfx/applog.h"
#include "vfx/vfxapp.h"
#include "xrapp.h"

/* Log lines and crash reports go to the dev PC too when set (build.sh). */
#ifndef VFX1_LOG_URL
#define VFX1_LOG_URL ""
#endif

/* Where the launcher lists and downloads games from: build.sh sets it from
 * VFX1_SERVER_DIR, normally the dev PC's web/serve.py serving build/web/native/. */
#ifndef VFX1_SERVER_DIR
#define VFX1_SERVER_DIR ""
#endif

#define TAG "VFX1Quest"
#define LOGI(...) applog(APPLOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) applog(APPLOG_ERROR, TAG, __VA_ARGS__)

typedef struct Platform {
    struct android_app *android;
    int resumed;
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface surface;
    AAudioStream *audio;
    VfxApp *vfx;
    /* a Bluetooth keyboard and mouse: held now, or pressed and released
     * since the last frame (latched so a quick tap still reaches DOS) */
    uint8_t key_held[RETROK_LAST], key_latched[RETROK_LAST], dos_keys[RETROK_LAST];
    float mouse_x, mouse_y;             /* movement since the last frame */
    unsigned mouse_held, mouse_latched;
    int logged_key, logged_mouse;
} Platform;

static Platform g;

/* ------------------------------------------------------------ log */

static void logcat(int prio, const char *tag, const char *line)
{
    __android_log_write(prio, tag, line);
}

static void on_crash(int sig, siginfo_t *info, void *ctx)
{
    ucontext_t *uc = (ucontext_t *)ctx;
    uintptr_t pc = (uintptr_t)uc->uc_mcontext.pc, lr = (uintptr_t)uc->uc_mcontext.regs[30];
    Dl_info di, dl;
    char msg[800];
    int have_pc = dladdr((void *)pc, &di), have_lr = dladdr((void *)lr, &dl);

    snprintf(msg, sizeof msg,
             "E crash: signal %d at address %p; pc %s+0x%lx (%s); lr %s+0x%lx (%s)",
             sig, info ? info->si_addr : NULL,
             have_pc && di.dli_fname ? di.dli_fname : "?", have_pc ? (unsigned long)(pc - (uintptr_t)di.dli_saddr) : (unsigned long)pc,
             have_pc && di.dli_sname ? di.dli_sname : "?",
             have_lr && dl.dli_fname ? dl.dli_fname : "?", have_lr ? (unsigned long)(lr - (uintptr_t)dl.dli_saddr) : (unsigned long)lr,
             have_lr && dl.dli_sname ? dl.dli_sname : "?");
    __android_log_write(ANDROID_LOG_FATAL, TAG, msg);
    applog_post_now(msg);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void catch_crashes(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

/* ------------------------------------------------------------ audio */

static aaudio_data_callback_result_t audio_pull(AAudioStream *s, void *user, void *data, int32_t frames)
{
    (void)s;
    vfxapp_audio_pull((VfxApp *)user, (int16_t *)data, (size_t)frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void audio_start(VfxApp *vfx, int sample_rate)
{
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, sample_rate);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, audio_pull, vfx);
    if (AAudioStreamBuilder_openStream(b, &g.audio) == AAUDIO_OK) AAudioStream_requestStart(g.audio);
    else g.audio = NULL;
    AAudioStreamBuilder_delete(b);
}

/* ------------------------------------------------------------ EGL and OpenXR */

static int egl_init(void)
{
    const EGLint attribs[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                               EGL_DEPTH_SIZE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                               EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    const EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    const EGLint pb_attribs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLint n = 0;
    g.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(g.display, NULL, NULL)) return 0;
    if (!eglChooseConfig(g.display, attribs, &g.config, 1, &n) || n < 1) return 0;
    g.context = eglCreateContext(g.display, g.config, EGL_NO_CONTEXT, ctx_attribs);
    g.surface = eglCreatePbufferSurface(g.display, g.config, pb_attribs);
    if (g.context == EGL_NO_CONTEXT || g.surface == EGL_NO_SURFACE) return 0;
    return eglMakeCurrent(g.display, g.surface, g.surface, g.context);
}

/* The Khronos loader does not export xrInitializeLoaderKHR; it is only
 * reachable through xrGetInstanceProcAddr. Calling it directly links but
 * fails when Android loads the library. */
static void loader_init(void)
{
    XrLoaderInitInfoAndroidKHR loader = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
    PFN_xrVoidFunction fn = NULL;
    if (XR_SUCCEEDED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &fn)) && fn) {
        loader.applicationVM = g.android->activity->vm;
        loader.applicationContext = g.android->activity->clazz;
        ((PFN_xrInitializeLoaderKHR)fn)((const XrLoaderInitInfoBaseHeaderKHR *)&loader);
    } else {
        LOGE("xrInitializeLoaderKHR not found in the loader");
    }
}

static const void *graphics_binding(XrInstance instance, XrSystemId system)
{
    static XrGraphicsBindingOpenGLESAndroidKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
    XrGraphicsRequirementsOpenGLESKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
    /* Extension functions come from the runtime through the loader's
     * dispatch, not as exported symbols. */
    PFN_xrVoidFunction fn = NULL;
    if (XR_SUCCEEDED(xrGetInstanceProcAddr(instance, "xrGetOpenGLESGraphicsRequirementsKHR", &fn)) && fn)
        ((PFN_xrGetOpenGLESGraphicsRequirementsKHR)fn)(instance, system, &req);
    binding.display = g.display;
    binding.config = g.config;
    binding.context = g.context;
    return &binding;
}

/* ------------------------------------------------------------ Android */

/* ------------------------------------------------------------ keyboard and mouse */

static unsigned retrok_from_android(int32_t k)
{
    if (k >= AKEYCODE_A && k <= AKEYCODE_Z) return RETROK_a + (unsigned)(k - AKEYCODE_A);
    if (k >= AKEYCODE_0 && k <= AKEYCODE_9) return RETROK_0 + (unsigned)(k - AKEYCODE_0);
    if (k >= AKEYCODE_F1 && k <= AKEYCODE_F12) return RETROK_F1 + (unsigned)(k - AKEYCODE_F1);
    if (k >= AKEYCODE_NUMPAD_0 && k <= AKEYCODE_NUMPAD_9) return RETROK_KP0 + (unsigned)(k - AKEYCODE_NUMPAD_0);
    switch (k) {
    case AKEYCODE_ENTER: return RETROK_RETURN;
    case AKEYCODE_NUMPAD_ENTER: return RETROK_KP_ENTER;
    case AKEYCODE_ESCAPE: return RETROK_ESCAPE;
    case AKEYCODE_SPACE: return RETROK_SPACE;
    case AKEYCODE_TAB: return RETROK_TAB;
    case AKEYCODE_DEL: return RETROK_BACKSPACE;             /* Android's "delete" is backspace */
    case AKEYCODE_FORWARD_DEL: return RETROK_DELETE;
    case AKEYCODE_SHIFT_LEFT: return RETROK_LSHIFT;
    case AKEYCODE_SHIFT_RIGHT: return RETROK_RSHIFT;
    case AKEYCODE_CTRL_LEFT: return RETROK_LCTRL;
    case AKEYCODE_CTRL_RIGHT: return RETROK_RCTRL;
    case AKEYCODE_ALT_LEFT: return RETROK_LALT;
    case AKEYCODE_ALT_RIGHT: return RETROK_RALT;
    case AKEYCODE_DPAD_UP: return RETROK_UP;
    case AKEYCODE_DPAD_DOWN: return RETROK_DOWN;
    case AKEYCODE_DPAD_LEFT: return RETROK_LEFT;
    case AKEYCODE_DPAD_RIGHT: return RETROK_RIGHT;
    case AKEYCODE_INSERT: return RETROK_INSERT;
    case AKEYCODE_MOVE_HOME: return RETROK_HOME;
    case AKEYCODE_MOVE_END: return RETROK_END;
    case AKEYCODE_PAGE_UP: return RETROK_PAGEUP;
    case AKEYCODE_PAGE_DOWN: return RETROK_PAGEDOWN;
    case AKEYCODE_NUMPAD_DOT: return RETROK_KP_PERIOD;
    case AKEYCODE_NUMPAD_ADD: return RETROK_KP_PLUS;
    case AKEYCODE_NUMPAD_SUBTRACT: return RETROK_KP_MINUS;
    case AKEYCODE_NUMPAD_MULTIPLY: return RETROK_KP_MULTIPLY;
    case AKEYCODE_NUMPAD_DIVIDE: return RETROK_KP_DIVIDE;
    case AKEYCODE_MINUS: return RETROK_MINUS;
    case AKEYCODE_EQUALS: return RETROK_EQUALS;
    case AKEYCODE_LEFT_BRACKET: return RETROK_LEFTBRACKET;
    case AKEYCODE_RIGHT_BRACKET: return RETROK_RIGHTBRACKET;
    case AKEYCODE_SEMICOLON: return RETROK_SEMICOLON;
    case AKEYCODE_APOSTROPHE: return RETROK_QUOTE;
    case AKEYCODE_COMMA: return RETROK_COMMA;
    case AKEYCODE_PERIOD: return RETROK_PERIOD;
    case AKEYCODE_SLASH: return RETROK_SLASH;
    case AKEYCODE_BACKSLASH: return RETROK_BACKSLASH;
    case AKEYCODE_GRAVE: return RETROK_BACKQUOTE;
    case AKEYCODE_CAPS_LOCK: return RETROK_CAPSLOCK;
    case AKEYCODE_NUM_LOCK: return RETROK_NUMLOCK;
    case AKEYCODE_SCROLL_LOCK: return RETROK_SCROLLOCK;
    case AKEYCODE_BREAK: return RETROK_PAUSE;
    default: return RETROK_UNKNOWN;
    }
}

static int32_t on_input(struct android_app *android, AInputEvent *e)
{
    int32_t type = AInputEvent_getType(e), source = AInputEvent_getSource(e);
    (void)android;
    if (type == AINPUT_EVENT_TYPE_KEY && (source & AINPUT_SOURCE_KEYBOARD) == AINPUT_SOURCE_KEYBOARD) {
        int32_t code = AKeyEvent_getKeyCode(e), action = AKeyEvent_getAction(e);
        unsigned k = retrok_from_android(code);
        if (!g.logged_key) { g.logged_key = 1; LOGI("keyboard: first key %d", code); }
        if (k == RETROK_UNKNOWN || k >= RETROK_LAST) return 0;
        if (action == AKEY_EVENT_ACTION_DOWN) { g.key_held[k] = 1; g.key_latched[k] = 1; }
        else if (action == AKEY_EVENT_ACTION_UP) g.key_held[k] = 0;
        return 1;
    }
    if (type == AINPUT_EVENT_TYPE_MOTION && (source & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE) {
        size_t i, hist = AMotionEvent_getHistorySize(e);
        float dx = 0, dy = 0;
        unsigned b = 0;
        int32_t state = AMotionEvent_getButtonState(e);
        for (i = 0; i < hist; i++) {
            dx += AMotionEvent_getHistoricalAxisValue(e, AMOTION_EVENT_AXIS_RELATIVE_X, 0, i);
            dy += AMotionEvent_getHistoricalAxisValue(e, AMOTION_EVENT_AXIS_RELATIVE_Y, 0, i);
        }
        dx += AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RELATIVE_X, 0);
        dy += AMotionEvent_getAxisValue(e, AMOTION_EVENT_AXIS_RELATIVE_Y, 0);
        g.mouse_x += dx;
        g.mouse_y += dy;
        if (state & AMOTION_EVENT_BUTTON_PRIMARY) b |= 1;
        if (state & AMOTION_EVENT_BUTTON_SECONDARY) b |= 2;
        if (state & AMOTION_EVENT_BUTTON_TERTIARY) b |= 4;
        g.mouse_held = b;
        g.mouse_latched |= b;
        if (!g.logged_mouse) { g.logged_mouse = 1; LOGI("mouse: first event, moved %.1f,%.1f buttons %u", dx, dy, b); }
        return 1;
    }
    return 0;
}

/* Each frame: the keyboard and mouse, after the controllers. */
static void add_input(const VfxApp *vfx, VfxAppInput *in)
{
    int i, dx = (int)g.mouse_x, dy = (int)g.mouse_y;
    for (i = 0; i < RETROK_LAST; i++) g.dos_keys[i] = g.key_held[i] | g.key_latched[i];
    memset(g.key_latched, 0, sizeof g.key_latched);
    g.mouse_x -= (float)dx;                 /* keep the fraction for the next frame */
    g.mouse_y -= (float)dy;
    vfxapp_add_keyboard_mouse(vfx, in, g.dos_keys, dx, dy, g.mouse_held | g.mouse_latched);
    g.mouse_latched = 0;
}

static void on_cmd(struct android_app *android, int32_t cmd)
{
    (void)android;
    if (cmd == APP_CMD_RESUME) g.resumed = 1;
    else if (cmd == APP_CMD_PAUSE) g.resumed = 0;
}

void android_main(struct android_app *android)
{
    static const char *const exts[] = { XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME };
    XrInstanceCreateInfoAndroidKHR android_ci = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
    XrAppPlatform platform;
    const char *files = android->activity->externalDataPath;
    XrApp *xr;

    g.android = android;
    android->onAppCmd = on_cmd;
    android->onInputEvent = on_input;

    applog_set_sink(logcat);
    applog_init(VFX1_LOG_URL);
    catch_crashes();
    LOGI("starting: files %s", files ? files : "(none)");

    if (!egl_init()) { LOGE("EGL init failed: 0x%x", eglGetError()); ANativeActivity_finish(android->activity); return; }
    loader_init();
    android_ci.applicationVM = android->activity->vm;
    android_ci.applicationActivity = android->activity->clazz;
    memset(&platform, 0, sizeof platform);
    platform.extensions = exts;
    platform.extension_count = 2;
    platform.instance_next = &android_ci;
    platform.graphics_binding = graphics_binding;
    platform.swapchain_image_type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    platform.add_input = add_input;

    g.vfx = calloc(1, sizeof *g.vfx);
    xr = xrapp_create(&platform, g.vfx);
    if (!xr) { LOGE("OpenXR init failed"); ANativeActivity_finish(android->activity); return; }
    LOGI("starting the emulator");
    if (!files || !vfxapp_init(g.vfx, "libdosbox_pure_libretro.so", files, android->activity->internalDataPath,
                               VFX1_SERVER_DIR))
        LOGE("emulator not started; showing a blank display");
    else
        audio_start(g.vfx, 48000);

    while (!android->destroyRequested) {
        struct android_poll_source *source;
        int timeout = (g.resumed || xrapp_running(xr)) ? 0 : -1;
        while (ALooper_pollOnce(timeout, NULL, NULL, (void **)&source) >= 0) {
            if (source) source->process(android, source);
            if (android->destroyRequested) break;
            timeout = (g.resumed || xrapp_running(xr)) ? 0 : -1;
        }
        if (!xrapp_poll(xr)) break;
        if (xrapp_running(xr)) xrapp_frame(xr);
    }

    LOGI("leaving: destroyRequested %d", android->destroyRequested);
    if (g.audio) { AAudioStream_requestStop(g.audio); AAudioStream_close(g.audio); }
    vfxapp_close(g.vfx);
    xrapp_destroy(xr);
    ANativeActivity_finish(android->activity);
}
