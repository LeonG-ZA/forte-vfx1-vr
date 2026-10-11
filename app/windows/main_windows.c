/*
 * main_windows.c -- the Windows edges of the OpenXR app (app/xr/xrapp.h):
 * a console program with a small window that holds the OpenGL context and,
 * while it is in front, takes the PC's keyboard and mouse for the game.
 * Paths, the log, sound and the key table are winplat.c, shared with the
 * desktop debug app.
 *
 * It runs on whatever OpenXR runtime is active: Virtual Desktop's VDXR for a
 * Quest streamed over Wi-Fi, SteamVR, or Meta's Quest Link.
 *
 *   vfx1xr.exe [games folder]
 *
 * The games folder defaults to games\ next to the program; DOSBox Pure,
 * dosbox_pure_libretro.dll, sits next to the program too.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>             /* openxr_platform.h names IUnknown */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xrgl.h"

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "libretro.h"
#include "vfx/applog.h"
#include "vfx/vfxapp.h"
#include "winplat.h"
#include "xrapp.h"

/* Where the launcher lists and downloads games from, as on the Quest; the
 * VFX1_SERVER environment variable overrides it. */
#ifndef VFX1_SERVER_DIR
#define VFX1_SERVER_DIR ""
#endif

#define TAG "VFX1Win"
#define LOGI(...) applog(APPLOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) applog(APPLOG_ERROR, TAG, __VA_ARGS__)

static HWND g_window;
static HDC g_dc;
static HGLRC g_glrc;

/* ------------------------------------------------------------ keyboard and mouse */

/* The PC's keyboard and mouse, read as raw input while the window is in
 * front: held now, or pressed and released since the last frame (latched
 * so a quick tap still reaches DOS). While in front the mouse is held in
 * the window and hidden, so clicks cannot land on the desktop; F12 lets it
 * go and a click on the window takes it back, as in VFX1 Desk. */
static struct {
    uint8_t held[RETROK_LAST], latched[RETROK_LAST], keys[RETROK_LAST];
    long dx, dy;
    unsigned buttons, buttons_latched;
    int captured, released;
} g_in;

static void capture(int on)
{
    RECT r;
    if (on && !g_in.captured && GetForegroundWindow() == g_window && GetClientRect(g_window, &r)) {
        MapWindowPoints(g_window, NULL, (POINT *)&r, 2);
        ClipCursor(&r);
        ShowCursor(FALSE);
        g_in.captured = 1;
    } else if (!on && g_in.captured) {
        ClipCursor(NULL);
        ShowCursor(TRUE);
        g_in.captured = 0;
    }
}

static void raw_input(HRAWINPUT h)
{
    RAWINPUT ri;
    UINT size = sizeof ri;
    if (GetRawInputData(h, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
    if (ri.header.dwType == RIM_TYPEKEYBOARD) {
        const RAWKEYBOARD *k = &ri.data.keyboard;
        int down = !(k->Flags & RI_KEY_BREAK);
        unsigned key;
        if (k->VKey == VK_F12) {                    /* let the mouse go */
            if (down) { capture(0); g_in.released = 1; }
            return;
        }
        key = k->VKey == VK_F11 ? RETROK_F11
            : winplat_retrok_from_vk(k->VKey, (LPARAM)((k->Flags & RI_KEY_E0 ? 1L : 0L) << 24 |
                                                       (long)(k->MakeCode & 0xFF) << 16));
        if (key == RETROK_UNKNOWN || key >= RETROK_LAST) return;
        g_in.held[key] = (uint8_t)down;
        if (down) g_in.latched[key] = 1;
    } else if (ri.header.dwType == RIM_TYPEMOUSE && g_in.captured) {
        const RAWMOUSE *m = &ri.data.mouse;
        USHORT f = m->usButtonFlags;
        if (!(m->usFlags & MOUSE_MOVE_ABSOLUTE)) { g_in.dx += m->lLastX; g_in.dy += m->lLastY; }
        if (f & RI_MOUSE_LEFT_BUTTON_DOWN) g_in.buttons |= 1;
        if (f & RI_MOUSE_LEFT_BUTTON_UP) g_in.buttons &= ~1u;
        if (f & RI_MOUSE_RIGHT_BUTTON_DOWN) g_in.buttons |= 2;
        if (f & RI_MOUSE_RIGHT_BUTTON_UP) g_in.buttons &= ~2u;
        if (f & RI_MOUSE_MIDDLE_BUTTON_DOWN) g_in.buttons |= 4;
        if (f & RI_MOUSE_MIDDLE_BUTTON_UP) g_in.buttons &= ~4u;
        g_in.buttons_latched |= g_in.buttons;
    }
}

static LRESULT CALLBACK window_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INPUT:
        raw_input((HRAWINPUT)lp);
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            capture(0);
            memset(g_in.held, 0, sizeof g_in.held);   /* keys let go elsewhere never come back up */
            g_in.buttons = 0;
        } else if (!g_in.released) {
            capture(1);
        }
        return 0;
    case WM_LBUTTONDOWN:                        /* a click takes the mouse back */
        g_in.released = 0;
        capture(1);
        return 0;
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_SYSCHAR: case WM_CHAR:
        /* raw input has them; Alt and F10 would otherwise open the window menu
         * and stall the frame loop, so only Alt+F4 goes on to Windows */
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;
        return 0;
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(w, msg, wp, lp);
}

/* Something else came to the front while the game had the keyboard and
 * mouse (a notification, another program): log what, and take the front
 * back once a second, unless F12 let the mouse go. Windows lets a program
 * take the front only from the one in front, so the app briefly joins that
 * one's input. */
static void keep_front(double now)
{
    static double last_try;
    static HWND last_logged;
    HWND fg = GetForegroundWindow();
    DWORD fg_thread, me = GetCurrentThreadId();
    if (fg == g_window || g_in.released || now - last_try < 1.0) return;
    last_try = now;
    if (fg && fg != last_logged) {
        char title[128] = "", exe[MAX_PATH] = "?";
        DWORD pid = 0;
        HANDLE proc;
        GetWindowTextA(fg, title, sizeof title);
        GetWindowThreadProcessId(fg, &pid);
        if ((proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))) {
            DWORD n = sizeof exe;
            QueryFullProcessImageNameA(proc, 0, exe, &n);
            CloseHandle(proc);
        }
        LOGI("keyboard and mouse: \"%s\" (%s) came to the front; taking it back", title, exe);
        last_logged = fg;
    }
    fg_thread = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    if (fg_thread && fg_thread != me) AttachThreadInput(me, fg_thread, TRUE);
    BringWindowToTop(g_window);
    SetForegroundWindow(g_window);
    if (fg_thread && fg_thread != me) AttachThreadInput(me, fg_thread, FALSE);
    if (GetForegroundWindow() == g_window) capture(1);
}

/* Each frame: the keyboard and mouse, after the controllers. */
static void add_input(const VfxApp *vfx, VfxAppInput *in)
{
    int i;
    for (i = 0; i < RETROK_LAST; i++) g_in.keys[i] = g_in.held[i] | g_in.latched[i];
    memset(g_in.latched, 0, sizeof g_in.latched);
    vfxapp_add_keyboard_mouse(vfx, in, g_in.keys, (int)g_in.dx, (int)g_in.dy, g_in.buttons | g_in.buttons_latched);
    g_in.dx = g_in.dy = 0;
    g_in.buttons_latched = 0;
}

static int gl_init(void)
{
    WNDCLASSA wc;
    PIXELFORMATDESCRIPTOR pfd;
    int format;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = window_proc;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "vfx1xr";
    RegisterClassA(&wc);
    /* The headset gets the frames; the window holds the OpenGL context and,
     * in front, the keyboard and mouse. */
    g_window = CreateWindowA("vfx1xr", "VFX1 XR - keyboard and mouse go to the game (F12 frees the mouse)",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 200, NULL, NULL, wc.hInstance,
                             NULL);
    if (!g_window) return 0;
    {
        RAWINPUTDEVICE rid[2];
        rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x06;   /* keyboard */
        rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x02;   /* mouse */
        rid[0].dwFlags = rid[1].dwFlags = 0;                /* only while in front */
        rid[0].hwndTarget = rid[1].hwndTarget = g_window;
        if (!RegisterRawInputDevices(rid, 2, sizeof rid[0])) LOGE("keyboard and mouse: raw input not available");
    }
    ShowWindow(g_window, SW_SHOWNORMAL);
    SetForegroundWindow(g_window);
    g_dc = GetDC(g_window);
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    format = ChoosePixelFormat(g_dc, &pfd);
    if (!format || !SetPixelFormat(g_dc, format, &pfd)) return 0;
    g_glrc = wglCreateContext(g_dc);
    if (!g_glrc || !wglMakeCurrent(g_dc, g_glrc)) return 0;
    LOGI("OpenGL %s on %s", (const char *)glGetString(GL_VERSION), (const char *)glGetString(GL_RENDERER));
    return xrgl_load();
}

static const void *graphics_binding(XrInstance instance, XrSystemId system)
{
    static XrGraphicsBindingOpenGLWin32KHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR };
    XrGraphicsRequirementsOpenGLKHR req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR };
    PFN_xrVoidFunction fn = NULL;
    if (XR_SUCCEEDED(xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR", &fn)) && fn)
        ((PFN_xrGetOpenGLGraphicsRequirementsKHR)fn)(instance, system, &req);
    binding.hDC = g_dc;
    binding.hGLRC = g_glrc;
    return &binding;
}

int main(int argc, char **argv)
{
    static const char *const exts[] = { XR_KHR_OPENGL_ENABLE_EXTENSION_NAME };
    WinPaths paths;
    XrAppPlatform platform;
    VfxApp *vfx;
    XrApp *xr;
    int audio = 0;

    winplat_start(&paths, argc, argv, "vfx1xr.log");
    if (!gl_init()) { LOGE("OpenGL 3.3 not available"); return 1; }
    memset(&platform, 0, sizeof platform);
    platform.extensions = exts;
    platform.extension_count = 1;
    platform.graphics_binding = graphics_binding;
    platform.swapchain_image_type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR;
    platform.add_input = add_input;

    vfx = calloc(1, sizeof *vfx);
    xr = xrapp_create(&platform, vfx);
    if (!xr) {
        LOGE("OpenXR did not start: is a headset connected, and Virtual Desktop's VDXR or SteamVR "
             "the active OpenXR runtime?");
        return 1;
    }
    if (!vfxapp_init(vfx, paths.core, paths.games, paths.temp, winplat_server(VFX1_SERVER_DIR)))
        LOGE("emulator not started; showing a blank display");
    else { winplat_audio_start(vfx, 48000); audio = 1; }

    for (;;) {
        MSG msg;
        int quit = 0;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = 1;
            DispatchMessageA(&msg);
        }
        if (quit) break;
        if (xrapp_running(xr)) keep_front(winplat_now());
        if (!xrapp_poll(xr)) break;
        if (xrapp_running(xr)) xrapp_frame(xr);
        else Sleep(20);
    }

    LOGI("leaving");
    capture(0);
    if (audio) winplat_audio_stop();
    vfxapp_close(vfx);
    xrapp_destroy(xr);
    winplat_stop();
    return 0;
}
