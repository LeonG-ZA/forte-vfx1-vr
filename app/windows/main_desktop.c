/*
 * main_desktop.c -- VFX1 Desk: the shared session (vfxapp) in a plain
 * window, for debugging without a headset.
 *
 * The game is drawn as a red/cyan anaglyph when it draws stereo, or in the
 * other views the panel offers. Two pads on the right stand in for the
 * hardware:
 *
 *   Head        drag: yaw and pitch; the slider or the mouse wheel: roll
 *   CyberPuck   drag: tilt; Trigger, Grip and A buttons (right-click latches)
 *
 * Right-click a pad to centre it. The keyboard goes to DOS. Click the game
 * to give it the mouse; F12 takes the mouse back. F11 opens the launcher
 * menu, which the arrows, Enter and Backspace drive.
 *
 *   vfx1desk.exe [games folder]
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libretro.h"
#include "vfx/applog.h"
#include "vfx/vfxapp.h"
#include "winplat.h"
#include "../../src/adapters/headless/png_write.h"

#ifndef VFX1_SERVER_DIR
#define VFX1_SERVER_DIR ""
#endif

#define TAG "VFX1Desk"
#define PI 3.14159265358979323846
#define PANEL_W 260
#define PAD 180

/* ------------------------------------------------------------ state */

enum { VIEW_ANAGLYPH, VIEW_HALF_COLOR, VIEW_GRAY, VIEW_SIDE, VIEW_LEFT, VIEW_RIGHT, VIEW_COUNT };
static const char *const VIEW_NAMES[VIEW_COUNT] = {
    "anaglyph", "half-colour", "grey anaglyph", "side by side", "left eye", "right eye" };

enum { DRAG_NONE, DRAG_HEAD, DRAG_ROLL, DRAG_PUCK };
enum { BTN_TRIGGER, BTN_GRIP, BTN_A, BTN_MENU, BTN_RECENTER, BTN_VIEW, BTN_SWAP, BTN_SAVE, BTN_COUNT };

typedef struct Layout {
    RECT game;                          /* the 4:3 picture */
    RECT head, roll, puck;
    RECT button[BTN_COUNT];
    int text_x, head_text_y, status_y;
} Layout;

static struct {
    HWND window;
    VfxApp *vfx;
    int running;

    double yaw, pitch, roll;            /* head, degrees: yaw left, pitch up, roll right */
    double puck_pitch, puck_roll;       /* degrees: pitch nose up, roll right side down */
    int puck_held[3], puck_latched[3];  /* trigger, grip, A */
    int drag, pressed_button;

    /* A press is latched until a frame has seen it, so a key or button let
     * go before the next frame still reaches DOS. */
    uint8_t key_held[RETROK_LAST], key_latched[RETROK_LAST];
    uint8_t dos_keys[RETROK_LAST];      /* what this frame sends */
    int menu_key, menu_latched, menu_pulse, recenter_pulse;
    int view, swap;

    int captured;                       /* the DOS mouse has the real mouse */
    POINT capture_centre;
    int mouse_dx, mouse_dy;
    unsigned mouse_buttons, mouse_latched;

    uint8_t *rgb;                       /* the last image, kept: vfxapp's is only valid for a frame */
    int rgb_w, rgb_h, rgb_size, stereo;
    uint32_t *view_px;                  /* what is drawn, BGRA */
    int view_size;
    uint32_t menu_px[VFXMENU_WIDTH * VFXMENU_HEIGHT];

    HDC back_dc;
    HBITMAP back_bmp;
    int back_w, back_h;
    HFONT font;
    double fps, fps_time;
    int fps_frames;

    int save_now;                       /* 1: write the ring of recent frames out */
    struct RingFrame { uint8_t *rgb; int w, h, stereo; uint64_t serial; double score, alike; } ring[90];
    int ring_next, ring_count;
    char save_dir[MAX_PATH], saved_note[48];
} g;

#define SAVE_FRAMES 90

/* ------------------------------------------------------------ angles */

static VfxHostQuat q_axis(double x, double y, double z, double deg)
{
    double h = deg * PI / 360.0, s = sin(h);
    VfxHostQuat q = { cos(h), x * s, y * s, z * s };
    return q;
}

/* yaw about +Y, then pitch about +X, then roll (tilting right) about -Z, as
 * vfxhost_sense_angles reads them back */
static VfxHostQuat head_quat(void)
{
    return vfxhost_quat_mul(vfxhost_quat_mul(q_axis(0, 1, 0, g.yaw), q_axis(1, 0, 0, g.pitch)),
                            q_axis(0, 0, -1, g.roll));
}

/* the puck's forward axis rising, then its right side dropping, as
 * vfxhost_tilt_angles reads them */
static VfxHostQuat puck_quat(void)
{
    return vfxhost_quat_mul(q_axis(1, 0, 0, g.puck_pitch), q_axis(0, 0, -1, g.puck_roll));
}

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ------------------------------------------------------------ layout */

static void set_rect(RECT *r, int x, int y, int w, int h) { SetRect(r, x, y, x + w, y + h); }

static Layout layout(void)
{
    Layout l;
    RECT c;
    int gw, gh, x, y, bw = (PAD - 10) / 3, bw2 = (PAD - 5) / 2;
    GetClientRect(g.window, &c);
    gw = c.right - PANEL_W;
    gh = c.bottom;
    if (gw < 64) gw = 64;
    if (gw * 3 > gh * 4) { int w = gh * 4 / 3; set_rect(&l.game, (gw - w) / 2, 0, w, gh); }
    else { int h = gw * 3 / 4; set_rect(&l.game, 0, (gh - h) / 2, gw, h); }

    x = c.right - PANEL_W + (PANEL_W - PAD) / 2;
    l.text_x = x;
    y = 26;
    set_rect(&l.head, x, y, PAD, PAD);              y += PAD + 6;
    set_rect(&l.roll, x, y, PAD, 18);               y += 24;
    l.head_text_y = y;                              y += 44;
    set_rect(&l.puck, x, y, PAD, PAD);              y += PAD + 6;
    set_rect(&l.button[BTN_TRIGGER], x, y, bw, 26);
    set_rect(&l.button[BTN_GRIP], x + bw + 5, y, bw, 26);
    set_rect(&l.button[BTN_A], x + 2 * (bw + 5), y, bw, 26);   y += 34;
    set_rect(&l.button[BTN_MENU], x, y, bw2, 26);
    set_rect(&l.button[BTN_RECENTER], x + bw2 + 5, y, bw2, 26); y += 32;
    set_rect(&l.button[BTN_VIEW], x, y, bw2, 26);
    set_rect(&l.button[BTN_SWAP], x + bw2 + 5, y, bw2, 26);   y += 32;
    set_rect(&l.button[BTN_SAVE], x, y, PAD, 26);             y += 36;
    l.status_y = y;
    return l;
}

/* ------------------------------------------------------------ keyboard */

static void release_mouse(void)
{
    if (!g.captured) return;
    g.captured = 0;
    g.mouse_buttons = 0;
    ClipCursor(NULL);
    ShowCursor(TRUE);
}

static void capture_mouse(const Layout *l)
{
    RECT r = l->game;
    if (g.captured) return;
    g.captured = 1;
    MapWindowPoints(g.window, NULL, (POINT *)&r, 2);
    ClipCursor(&r);
    ShowCursor(FALSE);
    g.capture_centre.x = (r.left + r.right) / 2;
    g.capture_centre.y = (r.top + r.bottom) / 2;
    SetCursorPos(g.capture_centre.x, g.capture_centre.y);
}

/* ------------------------------------------------------------ pads */

static void pad_drag(int which, int mx, int my, const Layout *l)
{
    if (which == DRAG_HEAD) {
        double nx = clampd((mx - l->head.left) / (double)PAD * 2 - 1, -1, 1);
        double ny = clampd((my - l->head.top) / (double)PAD * 2 - 1, -1, 1);
        g.yaw = -nx * 90;               /* drag right: turn right */
        g.pitch = -ny * 60;             /* drag up: look up */
    } else if (which == DRAG_ROLL) {
        g.roll = clampd((mx - l->roll.left) / (double)PAD * 2 - 1, -1, 1) * 45;
    } else if (which == DRAG_PUCK) {
        double nx = clampd((mx - l->puck.left) / (double)PAD * 2 - 1, -1, 1);
        double ny = clampd((my - l->puck.top) / (double)PAD * 2 - 1, -1, 1);
        g.puck_roll = nx * 90;          /* drag right: right side down */
        g.puck_pitch = ny * 90;         /* drag up: tip it forward, nose down, like a joystick */
    }
}

static int in_rect(const RECT *r, int x, int y) { POINT p = { x, y }; return PtInRect(r, p); }

static void click_button(int b, int right_click)
{
    if (b <= BTN_A) {
        if (right_click) g.puck_latched[b] = !g.puck_latched[b];
        else g.puck_held[b] = 1;
    } else if (b == BTN_MENU) {
        g.menu_pulse = 1;
    } else if (b == BTN_RECENTER) {
        g.yaw = g.pitch = g.roll = 0;
        g.puck_pitch = g.puck_roll = 0;
        g.recenter_pulse = 1;
    } else if (b == BTN_VIEW) {
        g.view = (g.view + (right_click ? VIEW_COUNT - 1 : 1)) % VIEW_COUNT;
    } else if (b == BTN_SWAP) {
        g.swap = !g.swap;
    } else if (b == BTN_SAVE) {
        g.save_now = 1;
        g.saved_note[0] = 0;
    }
}

/* ------------------------------------------------------------ window */

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Layout l;
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), i;
    if (!g.window) return DefWindowProcA(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CLOSE:
        g.running = 0;
        return 0;
    case WM_KILLFOCUS:
        memset(g.key_held, 0, sizeof g.key_held);
        g.menu_key = 0;
        release_mouse();
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        unsigned k;
        if (wp == VK_F4 && (GetKeyState(VK_MENU) & 0x8000)) return DefWindowProcA(hwnd, msg, wp, lp);
        if (wp == VK_F11) { g.menu_key = down; g.menu_latched |= down; return 0; }
        if (wp == VK_F12) { if (down) release_mouse(); return 0; }
        k = winplat_retrok_from_vk(wp, lp);
        if (k != RETROK_UNKNOWN && k < RETROK_LAST) {
            g.key_held[k] = (uint8_t)down;
            g.key_latched[k] |= (uint8_t)down;
        }
        return 0;                       /* no beeps or menu bar for Alt and F10 */
    }
    case WM_SYSCHAR: case WM_CHAR:
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
        unsigned bit = msg == WM_LBUTTONDOWN ? 1 : msg == WM_RBUTTONDOWN ? 2 : 4;
        int right = msg == WM_RBUTTONDOWN;
        if (g.captured) { g.mouse_buttons |= bit; g.mouse_latched |= bit; return 0; }
        l = layout();
        if (msg == WM_MBUTTONDOWN) return 0;
        if (in_rect(&l.head, x, y)) {
            if (right) { g.yaw = g.pitch = 0; return 0; }
            g.drag = DRAG_HEAD;
        } else if (in_rect(&l.roll, x, y)) {
            if (right) { g.roll = 0; return 0; }
            g.drag = DRAG_ROLL;
        } else if (in_rect(&l.puck, x, y)) {
            if (right) { g.puck_pitch = g.puck_roll = 0; return 0; }
            g.drag = DRAG_PUCK;
        } else {
            for (i = 0; i < BTN_COUNT; i++)
                if (in_rect(&l.button[i], x, y)) { click_button(i, right); g.pressed_button = i; SetCapture(hwnd); return 0; }
            if (!right && in_rect(&l.game, x, y) && !g.vfx->launcher.open) capture_mouse(&l);
            return 0;
        }
        SetCapture(hwnd);
        pad_drag(g.drag, x, y, &l);
        return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        unsigned bit = msg == WM_LBUTTONUP ? 1 : msg == WM_RBUTTONUP ? 2 : 4;
        if (g.captured) { g.mouse_buttons &= ~bit; return 0; }
        g.drag = DRAG_NONE;
        if (g.pressed_button >= 0 && g.pressed_button <= BTN_A) g.puck_held[g.pressed_button] = 0;
        g.pressed_button = -1;
        ReleaseCapture();
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g.captured) {
            POINT p;
            GetCursorPos(&p);
            if (p.x != g.capture_centre.x || p.y != g.capture_centre.y) {
                g.mouse_dx += p.x - g.capture_centre.x;
                g.mouse_dy += p.y - g.capture_centre.y;
                SetCursorPos(g.capture_centre.x, g.capture_centre.y);
            }
            return 0;
        }
        if (g.drag) { l = layout(); pad_drag(g.drag, x, y, &l); }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT p = { x, y };
        ScreenToClient(hwnd, &p);
        l = layout();
        if (in_rect(&l.head, p.x, p.y) || in_rect(&l.roll, p.x, p.y))
            g.roll = clampd(g.roll + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 5 : -5), -45, 45);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------ drawing */

static void keep_image(void)
{
    VfxApp *v = g.vfx;
    int size;
    if (!v->image_changed || !v->image) return;
    size = v->image_w * v->image_h * 3;
    if (size > g.rgb_size) { free(g.rgb); g.rgb = malloc((size_t)size); g.rgb_size = g.rgb ? size : 0; }
    if (!g.rgb) return;
    memcpy(g.rgb, v->image, (size_t)size);
    g.rgb_w = v->image_w;
    g.rgb_h = v->image_h;
    g.stereo = v->stereo_live;
}

static uint32_t bgra(int r, int gg, int b) { return (uint32_t)b | ((uint32_t)gg << 8) | ((uint32_t)r << 16); }
static int luma(const uint8_t *p) { return (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8; }

/* The image to draw, in g.view_px; returns its size. */
static void compose(int *out_w, int *out_h)
{
    int w = g.rgb_w, h = g.rgb_h, ew = w / 2, x, y, vw, need;
    int stereo = g.stereo && ew > 0;
    int mode = stereo ? g.view : VIEW_SIDE;     /* a flat image: as it is */
    vw = stereo && mode != VIEW_SIDE ? ew : w;
    need = vw * h;
    if (need > g.view_size) { free(g.view_px); g.view_px = malloc((size_t)need * 4); g.view_size = g.view_px ? need : 0; }
    if (!g.view_px || !g.rgb) { *out_w = *out_h = 0; return; }
    for (y = 0; y < h; y++) {
        const uint8_t *row = g.rgb + (size_t)y * w * 3;
        uint32_t *o = g.view_px + (size_t)y * vw;
        if (mode == VIEW_SIDE) {
            for (x = 0; x < w; x++) {
                int sx = stereo && g.swap ? (x + ew) % (2 * ew) : x;
                const uint8_t *p = row + sx * 3;
                o[x] = bgra(p[0], p[1], p[2]);
            }
            continue;
        }
        for (x = 0; x < ew; x++) {
            const uint8_t *L = row + (g.swap ? x + ew : x) * 3, *R = row + (g.swap ? x : x + ew) * 3;
            switch (mode) {
            case VIEW_ANAGLYPH:   o[x] = bgra(L[0], R[1], R[2]); break;
            case VIEW_HALF_COLOR: o[x] = bgra(luma(L), R[1], R[2]); break;
            case VIEW_GRAY:       o[x] = bgra(luma(L), luma(R), luma(R)); break;
            case VIEW_LEFT:       o[x] = bgra(L[0], L[1], L[2]); break;
            default:              o[x] = bgra(R[0], R[1], R[2]); break;
            }
        }
    }
    *out_w = vw;
    *out_h = h;
}

static void blit(HDC dc, const RECT *r, const uint32_t *px, int w, int h)
{
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;         /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(dc, r->left, r->top, r->right - r->left, r->bottom - r->top, 0, 0, w, h, px, &bi,
                  DIB_RGB_COLORS, SRCCOPY);
}

static void fill(HDC dc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, r, b);
    DeleteObject(b);
}

static void text(HDC dc, int x, int y, COLORREF c, const char *fmt, ...)
{
    char s[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    SetTextColor(dc, c);
    TextOutA(dc, x, y, s, (int)strlen(s));
}

static void dot(HDC dc, int x, int y, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c), old = SelectObject(dc, b);
    HPEN pen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, x - 7, y - 7, x + 8, y + 8);
    SelectObject(dc, old);
    SelectObject(dc, pen);
    DeleteObject(b);
}

static void pad(HDC dc, const RECT *r, double nx, double ny, COLORREF c)
{
    RECT line;
    int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2;
    fill(dc, r, RGB(32, 36, 44));
    SetRect(&line, cx, r->top, cx + 1, r->bottom);   fill(dc, &line, RGB(60, 66, 78));
    SetRect(&line, r->left, cy, r->right, cy + 1);   fill(dc, &line, RGB(60, 66, 78));
    dot(dc, cx + (int)(nx * (PAD / 2 - 8)), cy + (int)(ny * (PAD / 2 - 8)), c);
}

static void button(HDC dc, const RECT *r, const char *label, int on)
{
    SIZE sz;
    fill(dc, r, on ? RGB(214, 120, 40) : RGB(52, 58, 70));
    GetTextExtentPoint32A(dc, label, (int)strlen(label), &sz);
    text(dc, (r->left + r->right - sz.cx) / 2, (r->top + r->bottom - sz.cy) / 2, RGB(235, 235, 235), "%s", label);
}

static void draw(void)
{
    VfxApp *v = g.vfx;
    Layout l = layout();
    RECT c, panel;
    HDC dc;
    int w, h, i;
    char label[48];

    GetClientRect(g.window, &c);
    if (c.right <= 0 || c.bottom <= 0) return;
    dc = GetDC(g.window);
    if (!g.back_dc || g.back_w != c.right || g.back_h != c.bottom) {
        if (g.back_dc) { DeleteObject(g.back_bmp); DeleteDC(g.back_dc); }
        g.back_dc = CreateCompatibleDC(dc);
        g.back_bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
        SelectObject(g.back_dc, g.back_bmp);
        SelectObject(g.back_dc, g.font);
        SetBkMode(g.back_dc, TRANSPARENT);
        g.back_w = c.right;
        g.back_h = c.bottom;
    }
    fill(g.back_dc, &c, RGB(0, 0, 0));
    SetStretchBltMode(g.back_dc, COLORONCOLOR);

    /* the game, or the launcher menu over it */
    if (v->launcher.open) {
        RECT m = l.game;
        int mh = (m.right - m.left) * VFXMENU_HEIGHT / VFXMENU_WIDTH;
        if (v->menu_changed)
            for (i = 0; i < VFXMENU_WIDTH * VFXMENU_HEIGHT; i++)
                g.menu_px[i] = bgra(v->menu_rgba[i * 4], v->menu_rgba[i * 4 + 1], v->menu_rgba[i * 4 + 2]);
        m.top = (l.game.top + l.game.bottom - mh) / 2;
        m.bottom = m.top + mh;
        blit(g.back_dc, &m, g.menu_px, VFXMENU_WIDTH, VFXMENU_HEIGHT);
    } else if (vfxapp_game_visible(v) && g.rgb) {
        RECT r = l.game;
        compose(&w, &h);
        if (g.stereo && g.view == VIEW_SIDE) {        /* two 4:3 eyes side by side */
            int hh = (r.right - r.left) * 3 / 8;
            r.top = (l.game.top + l.game.bottom - hh) / 2;
            r.bottom = r.top + hh;
        }
        if (w > 0) blit(g.back_dc, &r, g.view_px, w, h);
    }

    /* the panel */
    SetRect(&panel, c.right - PANEL_W, 0, c.right, c.bottom);
    fill(g.back_dc, &panel, RGB(22, 24, 30));
    text(g.back_dc, l.text_x, 6, RGB(160, 200, 255), "HEAD  drag, wheel: roll");
    pad(g.back_dc, &l.head, -g.yaw / 90, -g.pitch / 60, RGB(90, 170, 255));
    fill(g.back_dc, &l.roll, RGB(32, 36, 44));
    {
        RECT knob;
        int kx = l.roll.left + (int)((g.roll / 45 + 1) / 2 * (PAD - 10));
        SetRect(&knob, kx, l.roll.top, kx + 10, l.roll.bottom);
        fill(g.back_dc, &knob, RGB(90, 170, 255));
    }
    text(g.back_dc, l.text_x, l.head_text_y, RGB(200, 200, 200), "yaw %+.0f  pitch %+.0f  roll %+.0f", g.yaw, g.pitch, g.roll);
    text(g.back_dc, l.text_x, l.head_text_y + 22, RGB(255, 200, 140), "CYBERPUCK  drag to tilt");
    pad(g.back_dc, &l.puck, g.puck_roll / 90, g.puck_pitch / 90, RGB(255, 160, 70));
    button(g.back_dc, &l.button[BTN_TRIGGER], "Trigger", g.puck_held[0] || g.puck_latched[0]);
    button(g.back_dc, &l.button[BTN_GRIP], "Grip", g.puck_held[1] || g.puck_latched[1]);
    button(g.back_dc, &l.button[BTN_A], "A", g.puck_held[2] || g.puck_latched[2]);
    button(g.back_dc, &l.button[BTN_MENU], "Menu  F11", v->launcher.open);
    button(g.back_dc, &l.button[BTN_RECENTER], "Recentre", 0);
    snprintf(label, sizeof label, "%s", VIEW_NAMES[g.view]);
    button(g.back_dc, &l.button[BTN_VIEW], label, 0);
    button(g.back_dc, &l.button[BTN_SWAP], "Swap eyes", g.swap);
    button(g.back_dc, &l.button[BTN_SAVE],
           g.saved_note[0] ? g.saved_note : "Save last 90 frames", 0);

    {
        int y = l.status_y;
        const uint32_t *st = v->host ? retrohost_vfx1_stats(v->host) : NULL;
        static const char *const stereo_names[] = { "off", "VFX1 pages", "lines" };
        COLORREF dim = RGB(150, 150, 160);
        text(g.back_dc, l.text_x, y, dim, "%s", v->launcher.loaded[0] ? v->launcher.loaded : "(no game)"); y += 16;
        text(g.back_dc, l.text_x, y, dim, "%dx%d  %s  %.0f fps", g.rgb_w, g.rgb_h,
             g.stereo ? "STEREO" : "flat", g.fps); y += 16;
        text(g.back_dc, l.text_x, y, dim, "stereo mode: %s",
             stereo_names[v->launcher.settings.stereo % 3]); y += 16;
        if (st) { text(g.back_dc, l.text_x, y, dim, "VFX1 polls %u/%u  video %u", st[4], st[5], st[7]); y += 16; }
        text(g.back_dc, l.text_x, y, dim, g.captured ? "mouse: DOS has it, F12 frees" : "mouse: click the game"); y += 16;
        (void)y;
    }

    BitBlt(dc, 0, 0, c.right, c.bottom, g.back_dc, 0, 0, SRCCOPY);
    ReleaseDC(g.window, dc);
}

/* ------------------------------------------------------------ input */

static void build_input(VfxAppInput *in)
{
    VfxApp *v = g.vfx;
    VfxHostHand *puck = v->launcher.settings.puck_left ? &in->left : &in->right;
    int i;

    memset(in, 0, sizeof *in);
    in->now_s = winplat_now();
    in->head_valid = 1;
    in->head = head_quat();
    in->left.present = in->right.present = 1;
    in->left.has_orientation = in->right.has_orientation = 1;
    in->left.orientation.w = in->right.orientation.w = 1;
    puck->orientation = puck_quat();
    puck->trigger = g.puck_held[0] || g.puck_latched[0];
    puck->grip = g.puck_held[1] || g.puck_latched[1];
    puck->primary = g.puck_held[2] || g.puck_latched[2];
    for (i = 0; i < RETROK_LAST; i++) g.dos_keys[i] = g.key_held[i] | g.key_latched[i];
    memset(g.key_latched, 0, sizeof g.key_latched);
    in->menu_button = g.menu_key || g.menu_latched || g.menu_pulse;
    g.menu_latched = g.menu_pulse = 0;
    if (g.recenter_pulse) { in->left.primary = 1; g.recenter_pulse = 0; }   /* X recentres */

    vfxapp_add_keyboard_mouse(v, in, g.dos_keys, g.mouse_dx, g.mouse_dy, g.mouse_buttons | g.mouse_latched);
    g.mouse_dx = g.mouse_dy = 0;
    g.mouse_latched = 0;
}

/* ------------------------------------------------------------ frame dumps */

/* Every frame the core draws is kept, as it drew it, in a ring of the last
 * SAVE_FRAMES, so a glitch can be saved after it is seen. */
static void keep_raw_frame(void *user, const uint8_t *rgb, int w, int h, uint64_t serial)
{
    struct RingFrame *f = &g.ring[g.ring_next];
    size_t size = (size_t)w * h * 3;
    (void)user;
    if (w <= 0 || h <= 0) return;
    if (!f->rgb || (size_t)f->w * f->h * 3 < size) {
        free(f->rgb);
        if (!(f->rgb = malloc(size))) { f->w = f->h = 0; return; }
    }
    memcpy(f->rgb, rgb, size);
    f->w = w;
    f->h = h;
    f->serial = serial;
    f->stereo = g.vfx->stereo_live;
    f->score = g.vfx->lines_score;
    f->alike = g.vfx->lines_alike;
    g.ring_next = (g.ring_next + 1) % SAVE_FRAMES;
    if (g.ring_count < SAVE_FRAMES) g.ring_count++;
}

/* The ring to frames\: raw0.png the oldest, and frames.csv with the shown
 * stereo state and the interlaced-stereo score at each. For working out how
 * a title lays out its eyes, and why the stereo detection decided as it did. */
static void save_ring(void)
{
    char path[MAX_PATH + 32];
    FILE *csv;
    int n, i;
    g.save_now = 0;
    CreateDirectoryA(g.save_dir, NULL);
    snprintf(path, sizeof path, "%s\\frames.csv", g.save_dir);
    csv = fopen(path, "w");
    if (csv) fprintf(csv, "frame,serial,width,height,shown_stereo,line_score,alike\n");
    for (n = 0; n < g.ring_count; n++) {
        struct RingFrame *f = &g.ring[(g.ring_next - g.ring_count + n + SAVE_FRAMES) % SAVE_FRAMES];
        uint8_t *rgba = malloc((size_t)f->w * f->h * 4);
        if (csv) fprintf(csv, "%d,%llu,%d,%d,%d,%.3f,%.3f\n", n, (unsigned long long)f->serial, f->w, f->h, f->stereo,
                         f->score, f->alike);
        if (!rgba) continue;
        for (i = 0; i < f->w * f->h; i++) {
            rgba[i * 4] = f->rgb[i * 3];
            rgba[i * 4 + 1] = f->rgb[i * 3 + 1];
            rgba[i * 4 + 2] = f->rgb[i * 3 + 2];
            rgba[i * 4 + 3] = 255;
        }
        snprintf(path, sizeof path, "%s\\raw%d.png", g.save_dir, n);
        vfx_png_write_rgba(path, rgba, (uint32_t)f->w, (uint32_t)f->h);
        free(rgba);
    }
    if (csv) fclose(csv);
    snprintf(g.saved_note, sizeof g.saved_note, "Saved %d to frames\\", g.ring_count);
    applog(APPLOG_INFO, TAG, "saved the last %d frames to %s", g.ring_count, g.save_dir);
}

/* ------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    WinPaths paths;
    WNDCLASSA wc;
    RECT r = { 0, 0, 960 + PANEL_W, 720 };
    double next;
    int audio = 0;

    winplat_start(&paths, argc, argv, "vfx1desk.log");
    snprintf(g.save_dir, sizeof g.save_dir, "%s\\frames", paths.dir);
    g.pressed_button = -1;

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "vfx1desk";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g.window = CreateWindowA("vfx1desk", "VFX1 Desk", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    g.font = CreateFontA(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    ShowWindow(g.window, SW_SHOW);

    g.vfx = calloc(1, sizeof *g.vfx);
    vfxapp_set_frame_hook(g.vfx, keep_raw_frame, NULL);
    if (!vfxapp_init(g.vfx, paths.core, paths.games, paths.temp, winplat_server(VFX1_SERVER_DIR)))
        applog(APPLOG_ERROR, TAG, "emulator not started");
    else { winplat_audio_start(g.vfx, 48000); audio = 1; }

    g.running = 1;
    next = winplat_now();
    g.fps_time = next;
    while (g.running && !g.vfx->exit_requested) {
        MSG msg;
        VfxAppInput in;
        double now, fps;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        build_input(&in);
        vfxapp_frame(g.vfx, &in);
        if (g.save_now) save_ring();
        keep_image();
        draw();

        g.fps_frames++;
        now = winplat_now();
        if (now - g.fps_time >= 1) { g.fps = g.fps_frames / (now - g.fps_time); g.fps_frames = 0; g.fps_time = now; }
        /* one frame per emulated frame, so the page-flip detector sees every one */
        fps = g.vfx->host ? retrohost_fps(g.vfx->host) : 60;
        next += 1.0 / clampd(fps, 30, 120);
        if (next < now - 0.1) next = now;
        if (next > now) Sleep((DWORD)((next - now) * 1000));
    }

    release_mouse();
    if (audio) winplat_audio_stop();
    vfxapp_close(g.vfx);
    winplat_stop();
    return 0;
}
