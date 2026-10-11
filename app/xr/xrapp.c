/*
 * xrapp.c -- VFX1 in OpenXR: the app shared by the Quest and Windows builds.
 *
 * DOSBox Pure (with the emulated Forte VFX1 from integration/dosbox-pure)
 * runs in vfxapp, through retrohost, our libretro frontend. Each headset
 * frame:
 *
 *   OpenXR head pose  -> vfxhost head angles -> the core's vfx1Pose()
 *   controllers       -> vfxhost layout      -> DOS keys, mouse, CyberPuck
 *   core frame        -> vfxhost stereo      -> one eye image per eye
 *
 * and the image is drawn head-locked at the real VFX1's size, 35.5 degrees
 * wide at 3.35 m. Everything between runtime and emulator is shared C
 * (docs/ARCHITECTURE.md); this file binds it to OpenXR and OpenGL (ES), and
 * the platform files (xrapp.h) bind those to the OS.
 *
 * The left controller's menu button opens and closes the launcher menu.
 */
#include "xrapp.h"
#include "xrgl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "vfx/applog.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TAG "VFX1XR"
#define LOGI(...) applog(APPLOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) applog(APPLOG_ERROR, TAG, __VA_ARGS__)

/* ------------------------------------------------------------ small maths */

typedef struct { float m[16]; } Mat4;   /* column-major */

static Mat4 mat_mul(Mat4 a, Mat4 b)
{
    Mat4 o;
    int c, r, k;
    for (c = 0; c < 4; c++)
        for (r = 0; r < 4; r++) {
            float s = 0;
            for (k = 0; k < 4; k++) s += a.m[k * 4 + r] * b.m[c * 4 + k];
            o.m[c * 4 + r] = s;
        }
    return o;
}

static Mat4 mat_pose(XrPosef p)
{
    Mat4 o;
    float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
    memset(&o, 0, sizeof o);
    o.m[0] = 1 - 2 * (y * y + z * z); o.m[1] = 2 * (x * y + w * z);     o.m[2] = 2 * (x * z - w * y);
    o.m[4] = 2 * (x * y - w * z);     o.m[5] = 1 - 2 * (x * x + z * z); o.m[6] = 2 * (y * z + w * x);
    o.m[8] = 2 * (x * z + w * y);     o.m[9] = 2 * (y * z - w * x);     o.m[10] = 1 - 2 * (x * x + y * y);
    o.m[12] = p.position.x; o.m[13] = p.position.y; o.m[14] = p.position.z; o.m[15] = 1;
    return o;
}

static Mat4 mat_rigid_inverse(Mat4 a)
{
    Mat4 o;
    memset(&o, 0, sizeof o);
    /* transpose rotation, rotate the negated translation */
    o.m[0] = a.m[0]; o.m[1] = a.m[4]; o.m[2] = a.m[8];
    o.m[4] = a.m[1]; o.m[5] = a.m[5]; o.m[6] = a.m[9];
    o.m[8] = a.m[2]; o.m[9] = a.m[6]; o.m[10] = a.m[10];
    o.m[12] = -(o.m[0] * a.m[12] + o.m[4] * a.m[13] + o.m[8] * a.m[14]);
    o.m[13] = -(o.m[1] * a.m[12] + o.m[5] * a.m[13] + o.m[9] * a.m[14]);
    o.m[14] = -(o.m[2] * a.m[12] + o.m[6] * a.m[13] + o.m[10] * a.m[14]);
    o.m[15] = 1;
    return o;
}

static Mat4 mat_projection(XrFovf fov, float nearz, float farz)
{
    Mat4 o;
    float l = tanf(fov.angleLeft), r = tanf(fov.angleRight);
    float u = tanf(fov.angleUp), d = tanf(fov.angleDown);
    float w = r - l, h = u - d;
    memset(&o, 0, sizeof o);
    o.m[0] = 2 / w;
    o.m[5] = 2 / h;
    o.m[8] = (r + l) / w;
    o.m[9] = (u + d) / h;
    o.m[10] = -(farz + nearz) / (farz - nearz);
    o.m[11] = -1;
    o.m[14] = -2 * farz * nearz / (farz - nearz);
    return o;
}

/* ------------------------------------------------------------ app state */

/* XrSwapchainImageOpenGLESKHR and XrSwapchainImageOpenGLKHR alike */
typedef struct GlImage {
    XrStructureType type;
    void *next;
    uint32_t image;
} GlImage;

typedef struct Swapchain {
    XrSwapchain handle;
    int32_t width, height;
    uint32_t count;
    GlImage *images;
    GLuint fbo;
} Swapchain;

struct XrApp {
    XrAppPlatform platform;
    VfxApp *vfx;
    int session_running, exit_requested;

    XrInstance instance;
    XrSystemId system;
    XrSession session;
    XrSessionState state;
    XrSpace local, view;
    XrViewConfigurationView config_views[2];
    Swapchain swapchains[2];
    XrView views[2];

    XrActionSet actions;
    XrAction trigger, squeeze, stick, stick_click, primary, secondary, grip_pose, menu;
    XrPath hand_paths[2];
    XrSpace grip_spaces[2];

    GLuint program, vao, texture, menu_texture;
    GLint u_mvp, u_uv, u_lcd;
};

static double now_s(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

static int xr_ok(XrResult r, const char *what)
{
    if (XR_SUCCEEDED(r)) return 1;
    LOGE("%s failed: %d", what, (int)r);
    return 0;
}

/* ------------------------------------------------------------ GL */

static GLuint compile(GLenum type, const char *src)
{
    /* the same shaders in both: GLSL ES 3.00 and desktop GLSL 3.30 */
    const char *srcs[2] = { XRGL_DESKTOP ? "#version 330 core\n" : "#version 300 es\n", src };
    GLuint s = glCreateShader(type);
    GLint ok;
    glShaderSource(s, 2, srcs, NULL);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        LOGE("shader: %s", log);
    }
    return s;
}

static void gl_init(XrApp *app)
{
    static const char *vs =
        "layout(location=0) in vec2 pos;\n"
        "uniform mat4 mvp; uniform vec2 uvx;\n"
        "out vec2 uv; out vec2 eye_uv;\n"
        "void main() { eye_uv = vec2((pos.x + 1.0) * 0.5, (1.0 - pos.y) * 0.5);\n"
        "  uv = vec2(uvx.x + eye_uv.x * uvx.y, eye_uv.y);\n"
        "  gl_Position = mvp * vec4(pos, 0.0, 1.0); }\n";
    static const char *fs =
        "precision mediump float;\n"
        "in vec2 uv; in vec2 eye_uv; uniform sampler2D tex;\n"
        /* the same precision as in the vertex shader, or the program will not link */
        "uniform highp vec2 uvx;\n"
        /* lcd: panel cells across and down one eye; zero for a plain screen */
        "uniform vec2 lcd; out vec4 color;\n"
        "void main() {\n"
        "  if (lcd.x <= 0.0) { color = vec4(texture(tex, uv).rgb, 1.0); return; }\n"
        /* The VFX1's LCDs, as lens photos show them: round dots on rows
         * offset by half a dot, dark between, the colour triads faint. */
        "  vec2 p = eye_uv * lcd;\n"
        "  float row = floor(p.y);\n"
        "  float shift = mod(row, 2.0) * 0.5;\n"
        "  vec2 cell = vec2(floor(p.x - shift) + 0.5 + shift, row + 0.5);\n"
        "  vec3 c = texture(tex, vec2(uvx.x + clamp(cell.x / lcd.x, 0.0, 1.0) * uvx.y, cell.y / lcd.y)).rgb;\n"
        "  vec2 d = (p - cell) * vec2(1.0, 1.05);\n"
        "  float edge = max(fwidth(p.x) * 0.9, 0.06);\n"
        "  float dot_ = 1.0 - smoothstep(0.47 - edge, 0.47 + edge, length(d));\n"
        "  float t = clamp((p.x - cell.x) * 1.8 + 0.5, 0.0, 1.0);\n"
        "  vec3 triad = vec3(1.0 - t, 1.0 - abs(t - 0.5) * 2.0, t);\n"
        "  vec3 tint = mix(vec3(1.0), triad * 1.6 + 0.2, 0.18 * (1.0 - smoothstep(0.35, 0.9, edge * 4.0)));\n"
        "  color = vec4(c * tint * dot_ * 1.25 + c * 0.05, 1.0);\n"
        "}\n";
    static const float quad[] = { -1, -1, 1, -1, -1, 1, -1, 1, 1, -1, 1, 1 };
    static const uint8_t black[3] = { 0, 0, 0 };
    GLuint vbo;

    app->program = glCreateProgram();
    glAttachShader(app->program, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(app->program, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(app->program);
    {
        GLint linked;
        glGetProgramiv(app->program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[512];
            glGetProgramInfoLog(app->program, sizeof log, NULL, log);
            LOGE("shader link: %s", log);
        }
    }
    app->u_mvp = glGetUniformLocation(app->program, "mvp");
    app->u_uv = glGetUniformLocation(app->program, "uvx");
    app->u_lcd = glGetUniformLocation(app->program, "lcd");

    glGenVertexArrays(1, &app->vao);
    glBindVertexArray(app->vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

    glGenTextures(1, &app->texture);
    glBindTexture(GL_TEXTURE_2D, app->texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8, 1, 1, 0, GL_RGB, GL_UNSIGNED_BYTE, black);

    glGenTextures(1, &app->menu_texture);
    glBindTexture(GL_TEXTURE_2D, app->menu_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, VFXMENU_WIDTH, VFXMENU_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
#if XRGL_DESKTOP
    /* OpenGL ES encodes to an sRGB target by itself; desktop GL only when asked */
    glEnable(GL_FRAMEBUFFER_SRGB);
#endif
}

/* ------------------------------------------------------------ OpenXR */

static XrPath path(XrApp *app, const char *s)
{
    XrPath p = XR_NULL_PATH;
    xrStringToPath(app->instance, s, &p);
    return p;
}

static XrAction make_action(XrApp *app, XrActionType type, const char *name, const char *label)
{
    XrActionCreateInfo ci = { XR_TYPE_ACTION_CREATE_INFO };
    XrAction a = XR_NULL_HANDLE;
    ci.actionType = type;
    snprintf(ci.actionName, sizeof ci.actionName, "%s", name);
    snprintf(ci.localizedActionName, sizeof ci.localizedActionName, "%s", label);
    ci.countSubactionPaths = 2;
    ci.subactionPaths = app->hand_paths;
    xr_ok(xrCreateAction(app->actions, &ci, &a), name);
    return a;
}

static void actions_init(XrApp *app)
{
    XrActionSetCreateInfo asci = { XR_TYPE_ACTION_SET_CREATE_INFO };
    XrInteractionProfileSuggestedBinding sb = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
    XrActionSuggestedBinding b[20];
    XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
    int n = 0, i;

    snprintf(asci.actionSetName, sizeof asci.actionSetName, "gameplay");
    snprintf(asci.localizedActionSetName, sizeof asci.localizedActionSetName, "Gameplay");
    xr_ok(xrCreateActionSet(app->instance, &asci, &app->actions), "xrCreateActionSet");
    app->hand_paths[0] = path(app, "/user/hand/left");
    app->hand_paths[1] = path(app, "/user/hand/right");

    app->trigger = make_action(app, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger");
    app->squeeze = make_action(app, XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", "Grip");
    app->stick = make_action(app, XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Thumbstick");
    app->stick_click = make_action(app, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Thumbstick press");
    app->primary = make_action(app, XR_ACTION_TYPE_BOOLEAN_INPUT, "primary", "A or X");
    app->secondary = make_action(app, XR_ACTION_TYPE_BOOLEAN_INPUT, "secondary", "B or Y");
    app->grip_pose = make_action(app, XR_ACTION_TYPE_POSE_INPUT, "grip_pose", "Grip pose");
    app->menu = make_action(app, XR_ACTION_TYPE_BOOLEAN_INPUT, "menu", "Menu");

#define BIND(act, str) do { b[n].action = act; b[n].binding = path(app, str); n++; } while (0)
    BIND(app->trigger, "/user/hand/left/input/trigger/value");
    BIND(app->trigger, "/user/hand/right/input/trigger/value");
    BIND(app->squeeze, "/user/hand/left/input/squeeze/value");
    BIND(app->squeeze, "/user/hand/right/input/squeeze/value");
    BIND(app->stick, "/user/hand/left/input/thumbstick");
    BIND(app->stick, "/user/hand/right/input/thumbstick");
    BIND(app->stick_click, "/user/hand/left/input/thumbstick/click");
    BIND(app->stick_click, "/user/hand/right/input/thumbstick/click");
    BIND(app->primary, "/user/hand/left/input/x/click");
    BIND(app->primary, "/user/hand/right/input/a/click");
    BIND(app->secondary, "/user/hand/left/input/y/click");
    BIND(app->secondary, "/user/hand/right/input/b/click");
    BIND(app->grip_pose, "/user/hand/left/input/grip/pose");
    BIND(app->grip_pose, "/user/hand/right/input/grip/pose");
    BIND(app->menu, "/user/hand/left/input/menu/click");
#undef BIND
    sb.interactionProfile = path(app, "/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = b;
    sb.countSuggestedBindings = (uint32_t)n;
    xr_ok(xrSuggestInteractionProfileBindings(app->instance, &sb), "xrSuggestInteractionProfileBindings");

    for (i = 0; i < 2; i++) {
        XrActionSpaceCreateInfo sci = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
        sci.action = app->grip_pose;
        sci.subactionPath = app->hand_paths[i];
        sci.poseInActionSpace.orientation.w = 1;
        xr_ok(xrCreateActionSpace(app->session, &sci, &app->grip_spaces[i]), "xrCreateActionSpace");
    }
    attach.countActionSets = 1;
    attach.actionSets = &app->actions;
    xr_ok(xrAttachSessionActionSets(app->session, &attach), "xrAttachSessionActionSets");
}

static int xr_init(XrApp *app)
{
    XrInstanceCreateInfo ci = { XR_TYPE_INSTANCE_CREATE_INFO };
    XrSystemGetInfo sgi = { XR_TYPE_SYSTEM_GET_INFO };
    XrSessionCreateInfo sci = { XR_TYPE_SESSION_CREATE_INFO };
    XrReferenceSpaceCreateInfo rsci = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
    uint32_t count = 0, i;

    ci.next = app->platform.instance_next;
    snprintf(ci.applicationInfo.applicationName, sizeof ci.applicationInfo.applicationName, "VFX1 XR");
    ci.applicationInfo.applicationVersion = 1;
    snprintf(ci.applicationInfo.engineName, sizeof ci.applicationInfo.engineName, "vfx1xr");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ci.enabledExtensionCount = app->platform.extension_count;
    ci.enabledExtensionNames = app->platform.extensions;
    LOGI("creating OpenXR instance");
    if (!xr_ok(xrCreateInstance(&ci, &app->instance), "xrCreateInstance")) return 0;
    {
        XrInstanceProperties props = { XR_TYPE_INSTANCE_PROPERTIES };
        if (XR_SUCCEEDED(xrGetInstanceProperties(app->instance, &props)))
            LOGI("OpenXR runtime: %s", props.runtimeName);
    }

    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    {
        /* On a PC the headset may not be streaming yet: wait for it. */
        XrResult r;
        int waited = 0;
        while ((r = xrGetSystem(app->instance, &sgi, &app->system)) == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
            if (!waited++) LOGI("waiting for the headset (connect it in Virtual Desktop or SteamVR)");
#ifdef _WIN32
            Sleep(1000);
#else
            { struct timespec ts = { 1, 0 }; nanosleep(&ts, NULL); }
#endif
        }
        if (!xr_ok(r, "xrGetSystem")) return 0;
    }

    sci.next = app->platform.graphics_binding(app->instance, app->system);
    sci.systemId = app->system;
    if (!xr_ok(xrCreateSession(app->instance, &sci, &app->session), "xrCreateSession")) return 0;
    LOGI("OpenXR session created");

    rsci.poseInReferenceSpace.orientation.w = 1;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    xr_ok(xrCreateReferenceSpace(app->session, &rsci, &app->local), "local space");
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    xr_ok(xrCreateReferenceSpace(app->session, &rsci, &app->view), "view space");

    xrEnumerateViewConfigurationViews(app->instance, app->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, NULL);
    if (count != 2) { LOGE("expected 2 views, got %u", count); return 0; }
    for (i = 0; i < 2; i++) app->config_views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    xrEnumerateViewConfigurationViews(app->instance, app->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, app->config_views);

    for (i = 0; i < 2; i++) {
        Swapchain *sc = &app->swapchains[i];
        XrSwapchainCreateInfo swci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
        uint32_t j;
        swci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        /* sRGB swapchain: GL encodes linear values to sRGB on output. The game
         * texture is therefore GL_SRGB8, decoded to linear when sampled, so
         * DOS's sRGB palette comes out unchanged instead of encoded twice. */
        swci.format = GL_SRGB8_ALPHA8;
        swci.sampleCount = 1;
        swci.width = app->config_views[i].recommendedImageRectWidth;
        swci.height = app->config_views[i].recommendedImageRectHeight;
        swci.faceCount = 1;
        swci.arraySize = 1;
        swci.mipCount = 1;
        if (!xr_ok(xrCreateSwapchain(app->session, &swci, &sc->handle), "xrCreateSwapchain")) return 0;
        sc->width = (int32_t)swci.width;
        sc->height = (int32_t)swci.height;
        xrEnumerateSwapchainImages(sc->handle, 0, &sc->count, NULL);
        sc->images = calloc(sc->count, sizeof *sc->images);
        for (j = 0; j < sc->count; j++) sc->images[j].type = app->platform.swapchain_image_type;
        xrEnumerateSwapchainImages(sc->handle, sc->count, &sc->count, (XrSwapchainImageBaseHeader *)sc->images);
        glGenFramebuffers(1, &sc->fbo);
    }
    for (i = 0; i < 2; i++) app->views[i].type = XR_TYPE_VIEW;

    actions_init(app);
    LOGI("OpenXR ready: %dx%d per eye", app->swapchains[0].width, app->swapchains[0].height);
    return 1;
}

int xrapp_poll(XrApp *app)
{
    XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(app->instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const XrEventDataSessionStateChanged *sc = (const XrEventDataSessionStateChanged *)&ev;
            app->state = sc->state;
            LOGI("session state %d", (int)sc->state);
            if (sc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = { XR_TYPE_SESSION_BEGIN_INFO };
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (xr_ok(xrBeginSession(app->session, &bi), "xrBeginSession")) app->session_running = 1;
            } else if (sc->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(app->session);
                app->session_running = 0;
            } else if (sc->state == XR_SESSION_STATE_EXITING || sc->state == XR_SESSION_STATE_LOSS_PENDING) {
                app->exit_requested = 1;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            app->exit_requested = 1;
        }
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
    if (app->vfx->exit_requested && !app->exit_requested) {
        app->exit_requested = 1;
        if (app->session_running) xrRequestExitSession(app->session);
    }
    return !app->exit_requested;
}

int xrapp_running(const XrApp *app)
{
    return app->session_running;
}

/* ------------------------------------------------------------ input */

static void read_hand(XrApp *app, int i, XrTime time, VfxHostHand *h)
{
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateFloat f = { XR_TYPE_ACTION_STATE_FLOAT };
    XrActionStateBoolean b = { XR_TYPE_ACTION_STATE_BOOLEAN };
    XrActionStateVector2f v = { XR_TYPE_ACTION_STATE_VECTOR2F };
    XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };

    memset(h, 0, sizeof *h);
    gi.subactionPath = app->hand_paths[i];

    gi.action = app->trigger;  xrGetActionStateFloat(app->session, &gi, &f);   h->trigger = f.isActive && f.currentState > 0.5f;
    gi.action = app->squeeze;  xrGetActionStateFloat(app->session, &gi, &f);   h->grip = f.isActive && f.currentState > 0.5f;
    h->present = f.isActive;
    gi.action = app->stick;    xrGetActionStateVector2f(app->session, &gi, &v);
    /* OpenXR's thumbstick y is positive forward; the layout wants the
     * gamepad convention, negative forward, as WebXR reports it. */
    h->stick_x = v.currentState.x; h->stick_y = -v.currentState.y;
    gi.action = app->stick_click; xrGetActionStateBoolean(app->session, &gi, &b); h->stick_press = b.isActive && b.currentState;
    gi.action = app->primary;     xrGetActionStateBoolean(app->session, &gi, &b); h->primary = b.isActive && b.currentState;
    gi.action = app->secondary;   xrGetActionStateBoolean(app->session, &gi, &b); h->secondary = b.isActive && b.currentState;
    h->present = h->present || v.isActive;

    if (XR_SUCCEEDED(xrLocateSpace(app->grip_spaces[i], app->local, time, &loc)) &&
        (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        h->has_orientation = 1;
        h->orientation.w = loc.pose.orientation.w; h->orientation.x = loc.pose.orientation.x;
        h->orientation.y = loc.pose.orientation.y; h->orientation.z = loc.pose.orientation.z;
    }
}

static void read_input(XrApp *app, XrTime time, VfxAppInput *in)
{
    XrActiveActionSet active = { app->actions, XR_NULL_PATH };
    XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
    XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateBoolean b = { XR_TYPE_ACTION_STATE_BOOLEAN };
    XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };

    memset(in, 0, sizeof *in);
    in->now_s = now_s();
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    xrSyncActions(app->session, &sync);
    read_hand(app, 0, time, &in->left);
    read_hand(app, 1, time, &in->right);

    gi.action = app->menu;
    gi.subactionPath = app->hand_paths[0];
    xrGetActionStateBoolean(app->session, &gi, &b);
    in->menu_button = b.isActive && b.currentState;

    if (XR_SUCCEEDED(xrLocateSpace(app->view, app->local, time, &head)) &&
        (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        VfxHostQuat q = { head.pose.orientation.w, head.pose.orientation.x, head.pose.orientation.y, head.pose.orientation.z };
        in->head_valid = 1;
        in->head = q;
    }
}

/* ------------------------------------------------------------ frame */

static void upload(XrApp *app)
{
    VfxApp *v = app->vfx;
    if (v->image_changed && v->image) {
        glBindTexture(GL_TEXTURE_2D, app->texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8, v->image_w, v->image_h, 0, GL_RGB, GL_UNSIGNED_BYTE, v->image);
    }
    if (v->menu_changed) {
        glBindTexture(GL_TEXTURE_2D, app->menu_texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, VFXMENU_WIDTH, VFXMENU_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE, v->menu_rgba);
    }
}

void xrapp_frame(XrApp *app)
{
    XrFrameWaitInfo fwi = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState fs = { XR_TYPE_FRAME_STATE };
    XrFrameBeginInfo fbi = { XR_TYPE_FRAME_BEGIN_INFO };
    XrFrameEndInfo fei = { XR_TYPE_FRAME_END_INFO };
    XrViewLocateInfo vli = { XR_TYPE_VIEW_LOCATE_INFO };
    XrViewState vs = { XR_TYPE_VIEW_STATE };
    XrCompositionLayerProjection layer = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
    XrCompositionLayerProjectionView pviews[2];
    const XrCompositionLayerBaseHeader *layers[1];
    XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
    VfxApp *v = app->vfx;
    const LauncherSettings *s = &v->launcher.settings;
    uint32_t count = 0;
    int i, rendered = 0;

    if (!xr_ok(xrWaitFrame(app->session, &fwi, &fs), "xrWaitFrame")) return;
    xrBeginFrame(app->session, &fbi);

    if (fs.shouldRender && v->host) {
        VfxAppInput in;
        read_input(app, fs.predictedDisplayTime, &in);
        if (app->platform.add_input) app->platform.add_input(v, &in);
        vfxapp_frame(v, &in);
        upload(app);

        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = app->local;
        xrLocateViews(app->session, &vli, &vs, 2, &count, app->views);
        xrLocateSpace(app->view, app->local, fs.predictedDisplayTime, &head);

        if (count == 2 && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            /* Head-locked display: the real VFX1's image size and distance. */
            float dist = (float)s->distance_m;
            float half_w = dist * tanf((float)(s->fov_deg * M_PI / 360.0)), half_h = half_w * 0.75f;
            Mat4 model = mat_pose(head.pose), scale, menu_model;
            /* the menu: a panel 50 degrees wide at 1.4 m, in front of the game */
            float menu_dist = 1.4f, menu_half_w = menu_dist * tanf((float)(50.0 * M_PI / 360.0));
            memset(&scale, 0, sizeof scale);
            scale.m[0] = half_w; scale.m[5] = half_h; scale.m[10] = 1; scale.m[15] = 1; scale.m[14] = -dist;
            memset(&menu_model, 0, sizeof menu_model);
            menu_model.m[0] = menu_half_w; menu_model.m[5] = menu_half_w * VFXMENU_HEIGHT / VFXMENU_WIDTH;
            menu_model.m[10] = 1; menu_model.m[15] = 1; menu_model.m[14] = -menu_dist;
            menu_model = mat_mul(model, menu_model);
            model = mat_mul(model, scale);

            for (i = 0; i < 2; i++) {
                Swapchain *sc = &app->swapchains[i];
                XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
                XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
                uint32_t index = 0;
                Mat4 mvp;
                float uvx[2];

                xrAcquireSwapchainImage(sc->handle, &ai, &index);
                wi.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(sc->handle, &wi);

                glBindFramebuffer(GL_FRAMEBUFFER, sc->fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc->images[index].image, 0);
                glViewport(0, 0, sc->width, sc->height);
                glClearColor(0, 0, 0, 1);
                glClear(GL_COLOR_BUFFER_BIT);

                mvp = mat_mul(mat_projection(app->views[i].fov, 0.05f, 100.0f),
                              mat_mul(mat_rigid_inverse(mat_pose(app->views[i].pose)), model));
                uvx[0] = v->stereo_live ? (i == 0 ? 0.0f : 0.5f) : 0.0f;
                uvx[1] = v->stereo_live ? 0.5f : 1.0f;
                glUseProgram(app->program);
                glBindVertexArray(app->vao);
                if (vfxapp_game_visible(v)) {
                    glUniformMatrix4fv(app->u_mvp, 1, GL_FALSE, mvp.m);
                    glUniform2fv(app->u_uv, 1, uvx);
                    /* The VFX1 panel: 263 x 230 dots an eye (789 x 230 colour cells) */
                    glUniform2f(app->u_lcd, s->lcd_look ? 263.0f : 0.0f, 230.0f);
                    glBindTexture(GL_TEXTURE_2D, app->texture);
                    glDrawArrays(GL_TRIANGLES, 0, 6);
                }
                if (v->launcher.open) {
                    const float full[2] = { 0.0f, 1.0f };
                    mvp = mat_mul(mat_projection(app->views[i].fov, 0.05f, 100.0f),
                                  mat_mul(mat_rigid_inverse(mat_pose(app->views[i].pose)), menu_model));
                    glUniformMatrix4fv(app->u_mvp, 1, GL_FALSE, mvp.m);
                    glUniform2fv(app->u_uv, 1, full);
                    glUniform2f(app->u_lcd, 0.0f, 0.0f);
                    glBindTexture(GL_TEXTURE_2D, app->menu_texture);
                    glDrawArrays(GL_TRIANGLES, 0, 6);
                }
                glBindFramebuffer(GL_FRAMEBUFFER, 0);

                xrReleaseSwapchainImage(sc->handle, &ri);

                memset(&pviews[i], 0, sizeof pviews[i]);
                pviews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                pviews[i].pose = app->views[i].pose;
                pviews[i].fov = app->views[i].fov;
                pviews[i].subImage.swapchain = sc->handle;
                pviews[i].subImage.imageRect.extent.width = sc->width;
                pviews[i].subImage.imageRect.extent.height = sc->height;
            }
            layer.space = app->local;
            layer.viewCount = 2;
            layer.views = pviews;
            layers[0] = (const XrCompositionLayerBaseHeader *)&layer;
            rendered = 1;
        }
    }

    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fei.layerCount = rendered ? 1 : 0;
    fei.layers = rendered ? layers : NULL;
    xrEndFrame(app->session, &fei);
}

/* ------------------------------------------------------------ public */

XrApp *xrapp_create(const XrAppPlatform *platform, VfxApp *vfx)
{
    XrApp *app = calloc(1, sizeof *app);
    if (!app) return NULL;
    app->platform = *platform;
    app->vfx = vfx;
    gl_init(app);
    if (!xr_init(app)) { xrapp_destroy(app); return NULL; }
    return app;
}

void xrapp_destroy(XrApp *app)
{
    int i;
    if (!app) return;
    for (i = 0; i < 2; i++) free(app->swapchains[i].images);
    if (app->session != XR_NULL_HANDLE) xrDestroySession(app->session);
    if (app->instance != XR_NULL_HANDLE) xrDestroyInstance(app->instance);
    free(app);
}
