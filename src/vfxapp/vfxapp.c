/*
 * vfxapp.c -- see include/vfx/vfxapp.h.
 */
#include "vfx/vfxapp.h"
#include "vfx/applog.h"
#include "libretro.h"
#include "fetch.h"

#include <math.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TAG "VFX1"

static void depth_stop(VfxApp *a);

/* ------------------------------------------------------------ audio */

/* One writer (the emulator) and one reader (the audio thread): each index is
 * written by one side only. */
static void audio_push(void *user, const int16_t *frames, size_t count)
{
    VfxApp *a = (VfxApp *)user;
    unsigned r = __atomic_load_n(&a->audio_read, __ATOMIC_ACQUIRE), w = a->audio_write;
    size_t i;
    for (i = 0; i < count; i++) {
        unsigned next = (w + 1) & (VFXAPP_AUDIO_RING - 1);
        if (next == r) break;                   /* full: drop the rest */
        a->audio_ring[w * 2] = frames[i * 2];
        a->audio_ring[w * 2 + 1] = frames[i * 2 + 1];
        w = next;
    }
    __atomic_store_n(&a->audio_write, w, __ATOMIC_RELEASE);
}

void vfxapp_audio_pull(VfxApp *a, int16_t *out, size_t frames)
{
    unsigned r = a->audio_read, w = __atomic_load_n(&a->audio_write, __ATOMIC_ACQUIRE);
    size_t i;
    for (i = 0; i < frames; i++) {
        if (r == w) { out[i * 2] = out[i * 2 + 1] = 0; continue; }
        out[i * 2] = a->audio_ring[r * 2];
        out[i * 2 + 1] = a->audio_ring[r * 2 + 1];
        r = (r + 1) & (VFXAPP_AUDIO_RING - 1);
    }
    __atomic_store_n(&a->audio_read, r, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------ setup */

static const unsigned key_map[VFXKEY_COUNT] = {
    [VFXKEY_CTRL] = RETROK_LCTRL, [VFXKEY_SPACE] = RETROK_SPACE, [VFXKEY_A] = RETROK_a, [VFXKEY_Z] = RETROK_z,
    [VFXKEY_UP] = RETROK_UP, [VFXKEY_DOWN] = RETROK_DOWN, [VFXKEY_LEFT] = RETROK_LEFT, [VFXKEY_RIGHT] = RETROK_RIGHT,
    [VFXKEY_ENTER] = RETROK_RETURN, [VFXKEY_ESCAPE] = RETROK_ESCAPE,
};

static void core_log(void *user, int level, const char *line)
{
    (void)user;
    applog(level >= 3 ? APPLOG_ERROR : level == 2 ? APPLOG_WARN : APPLOG_INFO, "DOSBoxPure", "%s", line);
}

static void reset_stereo(VfxApp *a)
{
    vfxhost_pageflip_free(&a->pageflip);
    vfxhost_pageflip_init(&a->pageflip, 8);
    /* A game set to interlaced stereo starts as stereo: its eyes may be so
     * alike (Quake's lcd_x 1 scores about 1.0) that only a flat screen gives
     * clear evidence either way. */
    a->lines_gate = (VfxStereoGate){ 1, 0, 8, 8 };   /* frames */
    a->pages_gate = (VfxStereoGate){ 0, 0, 2, 20 };  /* completed pages */
    a->side_gate = (VfxStereoGate){ 0, 0, 4, 20 };   /* frames */
    a->stereo_live = 0;
    a->last_serial = 0;
}

int vfxapp_init(VfxApp *a, const char *core, const char *files, const char *temp, const char *server)
{
    char err[512];
    VfxHostControlsConfig cc;

    fetch_set_temp_dir(temp);
    /* Saves next to the content, where adb can read them (e.g. a game's own logs). */
    a->host = retrohost_open(core, files, files, err, sizeof err);
    if (!a->host) { applog(APPLOG_ERROR, TAG, "core: %s", err); return 0; }
    retrohost_set_log(a->host, core_log, NULL);
    retrohost_set_option(a->host, "dosbox_pure_conf", "inside");
    retrohost_set_option(a->host, "dosbox_pure_cpu_core", "dynamic");
    retrohost_set_option(a->host, "dosbox_pure_cycles", "max");
    /* Android builds default to touchpad mode, which ignores relative mouse
     * movement; the controllers send relative movement, like a real mouse. */
    retrohost_set_option(a->host, "dosbox_pure_mouse_input", "true");
    retrohost_set_audio(a->host, audio_push, a);

    reset_stereo(a);
    vfxhost_controls_default_config(&cc);
    vfxhost_controls_init(&a->controls, &cc);
    a->last_frame_time = -1;
    a->last_flip_time = -1e9;

    launcher_init(&a->launcher, a->host, files, server);
    if (launcher_autostart(&a->launcher)) applog(APPLOG_INFO, TAG, "resumed %s", a->launcher.loaded);
    applog(APPLOG_INFO, TAG, "core ready, %d options, VFX1 %s, games in %s", retrohost_option_count(a->host),
           retrohost_has_vfx1(a->host) ? "present" : "missing", files);
    return 1;
}

void vfxapp_close(VfxApp *a)
{
    if (a->host) retrohost_close(a->host);
    a->host = NULL;
    vfxhost_pageflip_free(&a->pageflip);
    free(a->lines_eyes);
    a->lines_eyes = NULL;
    free(a->label_image);
    a->label_image = NULL;
    a->base_image = a->image = NULL;
    depth_stop(a);
    a->depth_tried = 0;
    vfxfake3d_free(&a->fake3d);
    free(a->fake_eyes);
    a->fake_eyes = NULL;
    a->fake_eyes_size = 0;
}

int vfxapp_game_visible(const VfxApp *a)
{
    return a->host && retrohost_loaded(a->host);
}

void vfxapp_add_keyboard_mouse(const VfxApp *a, VfxAppInput *in, const uint8_t *keys, int dx, int dy,
                               unsigned buttons)
{
    in->menu_button |= keys[RETROK_F11];
    if (a->launcher.open) {
        if (keys[RETROK_RIGHT]) in->right.stick_x = 1;
        if (keys[RETROK_LEFT]) in->right.stick_x = -1;
        if (keys[RETROK_DOWN]) in->right.stick_y = 1;
        if (keys[RETROK_UP]) in->right.stick_y = -1;
        in->right.primary |= keys[RETROK_RETURN] || keys[RETROK_KP_ENTER];
        in->right.secondary |= keys[RETROK_BACKSPACE] || keys[RETROK_ESCAPE];
        if (keys[RETROK_RIGHT] || keys[RETROK_LEFT] || keys[RETROK_DOWN] || keys[RETROK_UP] ||
            keys[RETROK_RETURN] || keys[RETROK_KP_ENTER] || keys[RETROK_BACKSPACE] || keys[RETROK_ESCAPE])
            in->right.present = 1;
    } else {
        in->dos_keys = keys;
        in->mouse_dx += dx;
        in->mouse_dy += dy;
        in->mouse_buttons |= buttons;
    }
}

/* ------------------------------------------------------------ menu */

static void release_game_input(VfxApp *a)
{
    unsigned k;
    for (k = 0; k < RETROK_LAST; k++) retrohost_key(a->host, k, 0);
    retrohost_mouse(a->host, 0, 0, 0);
}

/* The launcher menu: either stick moves, A or trigger selects, B goes back. */
static void menu_frame(VfxApp *a, const VfxHostHand *left, const VfxHostHand *right, double dt)
{
    VfxMenuInput in;
    double sy = right->stick_y, sx = right->stick_x;
    int activate, back;
    if (fabs(left->stick_y) > fabs(sy)) sy = left->stick_y;
    if (fabs(left->stick_x) > fabs(sx)) sx = left->stick_x;
    memset(&in, 0, sizeof in);
    in.up = vfxmenu_repeat(&a->rep_up, sy < -0.6, dt, 0.35, 0.09);
    in.down = vfxmenu_repeat(&a->rep_down, sy > 0.6, dt, 0.35, 0.09);
    in.left = vfxmenu_repeat(&a->rep_left, sx < -0.6, dt, 0.35, 0.12);
    in.right = vfxmenu_repeat(&a->rep_right, sx > 0.6, dt, 0.35, 0.12);
    activate = right->primary || right->trigger;
    back = right->secondary;
    in.activate = activate && !a->activate_held;
    in.back = back && !a->back_held;
    a->activate_held = activate;
    a->back_held = back;

    launcher_update(&a->launcher, &in);
    if (a->launcher.just_loaded) {
        reset_stereo(a);
        vfxhost_head_recenter(&a->head);
        a->activate_held = 1;          /* the A press that loaded must not reach the game */
    }
    if (a->launcher.quit_requested) a->exit_requested = 1;

    vfxmenu_draw(&a->launcher.menu, a->menu_rgba);
    a->menu_changed = 1;
}

/* ------------------------------------------------------------ frame */

void vfxapp_set_frame_hook(VfxApp *a, VfxAppFrameHook hook, void *user)
{
    a->frame_hook = hook;
    a->frame_hook_user = user;
}

static void call_frame_hook(VfxApp *a)
{
    const uint8_t *rgb;
    int w, h;
    uint64_t serial;
    if (!a->frame_hook) return;
    rgb = retrohost_frame(a->host, &w, &h, &serial);
    if (!rgb || serial == a->hook_serial) return;
    a->hook_serial = serial;
    a->frame_hook(a->frame_hook_user, rgb, w, h, serial);
}

static void show(VfxApp *a, const uint8_t *rgb, int w, int h, int stereo)
{
    /* every switch between stereo and flat, for chasing wrong ones */
    if (stereo != a->stereo_live)
        applog(APPLOG_INFO, TAG, "%s: %dx%d frame, line score %.2f, alike %.2f", stereo ? "stereo" : "flat",
               w, h, a->lines_score, a->lines_alike);
    a->image = a->base_image = rgb;
    a->image_w = a->base_w = w;
    a->image_h = a->base_h = h;
    a->image_changed = 1;
    a->stereo_live = a->base_stereo = stereo;
}

/* ------------------------------------------------------------- fake 3D */

/* The depth network on a thread of its own: the frame thread hands it the
 * newest flat frame, scaled to the network's input, whenever it is free, and
 * takes the depth map when it is done, so the game never waits for it. */
typedef struct VfxDepthWorker {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    VfxDepthNet *net;
    int quit, job, done;
    uint8_t in[VFXDEPTH_SIZE * VFXDEPTH_SIZE * 3];
    float out[VFXDEPTH_SIZE * VFXDEPTH_SIZE];
    double ms;                          /* the last run's time */
} VfxDepthWorker;

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static void *depth_thread(void *arg)
{
    VfxDepthWorker *w = (VfxDepthWorker *)arg;
    static float out[VFXDEPTH_SIZE * VFXDEPTH_SIZE];
    for (;;) {
        double t0;
        pthread_mutex_lock(&w->lock);
        while (!w->quit && !w->job) pthread_cond_wait(&w->cond, &w->lock);
        if (w->quit) { pthread_mutex_unlock(&w->lock); break; }
        pthread_mutex_unlock(&w->lock);
        t0 = now_ms();                  /* `in` is the thread's while job is set */
        vfxdepth_run(w->net, w->in, out);
        pthread_mutex_lock(&w->lock);
        memcpy(w->out, out, sizeof out);
        w->ms = now_ms() - t0;
        w->done = 1;
        w->job = 0;
        pthread_mutex_unlock(&w->lock);
    }
    return NULL;
}

static void depth_stop(VfxApp *a)
{
    VfxDepthWorker *w = a->depth;
    if (!w) return;
    pthread_mutex_lock(&w->lock);
    w->quit = 1;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
    pthread_join(w->thread, NULL);
    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->cond);
    vfxdepth_free(w->net);
    free(w);
    a->depth = NULL;
}

/* The network from <files>/fastdepth.vfd (tools/depthtest/export_fastdepth.py),
 * loaded the first time fake 3D is wanted. */
static int depth_start(VfxApp *a)
{
    char path[600];
    VfxDepthWorker *w;
    if (a->depth) return 1;
    if (a->depth_tried) return 0;
    a->depth_tried = 1;
    snprintf(path, sizeof path, "%s/fastdepth.vfd", a->launcher.files_dir);
    w = (VfxDepthWorker *)calloc(1, sizeof *w);
    if (!w) return 0;
    w->net = vfxdepth_load(path);
    if (!w->net) {
        applog(APPLOG_INFO, TAG, "fake 3D: no depth network at %s", path);
        free(w);
        return 0;
    }
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->cond, NULL);
    if (pthread_create(&w->thread, NULL, depth_thread, w)) {
        vfxdepth_free(w->net);
        free(w);
        return 0;
    }
    a->depth = w;
    applog(APPLOG_INFO, TAG, "fake 3D: depth network loaded from %s", path);
    return 1;
}

/* A screen to show flat: as fake 3D eyes if the setting asks and depth is
 * there, else flat. */
static void show_flat(VfxApp *a, const uint8_t *rgb, int w, int h)
{
    static const double DISPARITY[FAKE3D_LEVELS] = { 0, 3, 5, 8 };
    int level = a->launcher.settings.fake3d;
    size_t need = (size_t)w * 2 * h * 3;
    if (level <= FAKE3D_OFF || level >= FAKE3D_LEVELS || w < 64 || h < 64 || !depth_start(a)) {
        show(a, rgb, w, h, 0);
        return;
    }
    pthread_mutex_lock(&a->depth->lock);
    if (a->depth->done) {
        vfxfake3d_add_depth(&a->fake3d, a->depth->out);
        a->depth->done = 0;
    }
    if (!a->depth->job) {
        vfxdepth_resize_rgb(rgb, w, h, a->depth->in, VFXDEPTH_SIZE, VFXDEPTH_SIZE);
        a->depth->job = 1;
        pthread_cond_signal(&a->depth->cond);
    }
    pthread_mutex_unlock(&a->depth->lock);
    if (need > a->fake_eyes_size) {
        uint8_t *p = (uint8_t *)realloc(a->fake_eyes, need);
        if (!p) { show(a, rgb, w, h, 0); return; }
        a->fake_eyes = p;
        a->fake_eyes_size = need;
    }
    if (vfxfake3d_eyes(&a->fake3d, rgb, w, h, DISPARITY[level], a->fake_eyes) == 0)
        show(a, a->fake_eyes, w * 2, h, 1);
    else
        show(a, rgb, w, h, 0);
}

extern const uint8_t vfxmenu_font16[256 * 16];

/* text in the font of the menu, `scale` pixels a dot, on a dark box */
static void draw_text(uint8_t *rgb, int w, int h, int x0, int y0, const char *s, int scale)
{
    int n = (int)strlen(s), bw = (n * 8 + 4) * scale, bh = 20 * scale, x, y;
    for (y = 0; y < bh; y++)
        for (x = 0; x < bw; x++) {
            int px = x0 + x, py = y0 + y, cx = x / scale - 2, cy = y / scale - 2, on = 0;
            uint8_t *p;
            if (px < 0 || py < 0 || px >= w || py >= h) continue;
            if (cx >= 0 && cx < n * 8 && cy >= 0 && cy < 16)
                on = (vfxmenu_font16[(unsigned char)s[cx / 8] * 16 + cy] & (0x80 >> (cx % 8))) != 0;
            p = rgb + ((size_t)py * (size_t)w + (size_t)px) * 3u;
            p[0] = p[1] = p[2] = on ? 255 : 24;
        }
}

/* The mode key's label over the picture for a moment: in the top right of
 * each eye's half when the picture is a stereo pair. */
static void draw_view_label(VfxApp *a, double t)
{
    int active = a->view_label && t - a->view_label_time < 1.5;
    if (!a->base_image) return;
    if (active && (a->image_changed || !a->label_drawn)) {
        int w = a->base_w, h = a->base_h, halves = a->base_stereo ? 2 : 1, i;
        int scale = h >= 400 ? 2 : 1;
        int tw = ((int)strlen(a->view_label) * 8 + 4) * scale;
        size_t need = (size_t)w * (size_t)h * 3u;
        if (need > a->label_image_size) {
            uint8_t *p = (uint8_t *)realloc(a->label_image, need);
            if (!p) return;
            a->label_image = p;
            a->label_image_size = need;
        }
        memcpy(a->label_image, a->base_image, need);
        for (i = 0; i < halves; i++)
            draw_text(a->label_image, w, h, (i + 1) * w / halves - tw - 4 * scale, 4 * scale, a->view_label, scale);
        a->image = a->label_image;
        a->image_changed = 1;
        a->label_drawn = 1;
    } else if (!active && a->label_drawn) {
        a->image = a->base_image;
        a->image_changed = 1;
        a->label_drawn = 0;
        a->view_label = NULL;
    }
}

/* The mode key, from LauncherSettings.mode_key: held now? `key` gets the
 * keyboard key it uses, kept from the game. */
static int mode_key_down(const VfxAppInput *in, int mode_key, unsigned *key)
{
    int pad = 0;
    *key = 0;
    if (mode_key == MODEKEY_F9_OR_Y) { *key = RETROK_F9; pad = in->left.present && in->left.secondary; }
    else if (mode_key == MODEKEY_SCROLL_OR_STICK) { *key = RETROK_SCROLLOCK; pad = in->left.present && in->left.stick_press; }
    else if (mode_key == MODEKEY_PAUSE) *key = RETROK_PAUSE;
    return pad || (*key && in->dos_keys && in->dos_keys[*key]);
}

void vfxapp_frame(VfxApp *a, const VfxAppInput *in)
{
    double t = in->now_s, dt, fps;
    VfxHostControlsOut out;
    int flying, i, pressed;
    int16_t yaw = 0, pitch = 0, roll = 0;
    const uint8_t *rgb;
    int w, h;
    uint64_t serial;

    a->image_changed = 0;
    a->menu_changed = 0;
    if (!a->host) return;
    if (a->last_frame_time < 0) a->last_frame_time = t;
    dt = t - a->last_frame_time;
    fps = retrohost_fps(a->host);
    a->last_frame_time = t;
    flying = a->stereo_live && (t - a->last_flip_time) < 0.5;

    pressed = in->menu_button && !a->menu_button_held;
    a->menu_button_held = in->menu_button;
    if (pressed) {
        int open = !a->launcher.open || !retrohost_loaded(a->host);
        launcher_set_open(&a->launcher, open);
        if (open) release_game_input(a);
    }
    if (!retrohost_loaded(a->host)) a->launcher.open = 1;
    if (a->launcher.open) {
        menu_frame(a, &in->left, &in->right, dt);
        if (!a->launcher.open) {           /* resumed or loaded: start clean */
            a->emu_accumulator = 0;
            a->activate_held = 1;
            vfxhost_controls_rest(&a->controls);   /* take the puck and pointer as held now */
        }
        return;
    }

    /* settings the launcher controls */
    {
        const LauncherSettings *s = &a->launcher.settings;
        VfxHostControlsConfig *cc = &a->controls.config;
        cc->mouse = s->mouse == MOUSE_PUCK ? VFXMOUSE_PUCK
                  : s->mouse == MOUSE_POINT_RIGHT || s->mouse == MOUSE_POINT_LEFT ? VFXMOUSE_POINT : VFXMOUSE_OFF;
        cc->mouse_hand = s->mouse == MOUSE_POINT_LEFT ? VFXPUCK_LEFT : VFXPUCK_RIGHT;
        cc->puck_rate = 700.0 * s->mouse_speed;
        cc->point_scale = 1000.0 * s->mouse_speed;
        cc->puck_hand = s->puck_left ? VFXPUCK_LEFT : VFXPUCK_RIGHT;
        retrohost_vfx1_tracker_gain(a->host, s->tracker_gain);
        retrohost_vfx1_steer(a->host, s->steer);
    }
    vfxhost_controls_update(&a->controls, &in->left, &in->right, flying, dt, &out);
    {
        /* the layout's keys and any real keyboard's; retrohost passes on changes only */
        uint8_t down[RETROK_LAST];
        unsigned k, mode_key;
        int held = mode_key_down(in, a->launcher.settings.mode_key, &mode_key);
        if (held && !a->mode_key_held) {
            static const char *const LABELS[3] = { "Auto", "Stereo", "Mono" };
            a->view_label = LABELS[launcher_step_view_mode(&a->launcher)];
            a->view_label_time = t;
            a->label_drawn = 0;
        }
        a->mode_key_held = held;
        if (in->dos_keys) memcpy(down, in->dos_keys, sizeof down);
        else memset(down, 0, sizeof down);
        if (mode_key) down[mode_key] = 0;
        for (i = 0; i < VFXKEY_COUNT; i++) down[key_map[i]] |= out.keys[i];
        for (k = 0; k < RETROK_LAST; k++) retrohost_key(a->host, k, down[k]);
    }
    retrohost_mouse(a->host, out.mouse_dx + in->mouse_dx, out.mouse_dy + in->mouse_dy,
                    out.mouse_buttons | in->mouse_buttons);
    {
        /* mouse telemetry, every two seconds while the controller mouse is on */
        const VfxHostHand *mh = a->controls.config.mouse_hand == VFXPUCK_LEFT ? &in->left : &in->right;
        a->mouse_sum_x += out.mouse_dx;
        a->mouse_sum_y += out.mouse_dy;
        a->mouse_buttons_seen |= out.mouse_buttons;
        if (a->controls.config.mouse != VFXMOUSE_OFF && t - a->mouse_log_time >= 2.0) {
            applog(APPLOG_INFO, TAG, "mouse: mode %d hand %s present %d orientation %d moved %d,%d buttons %d puck tilt %.2f,%.2f",
                   (int)a->controls.config.mouse, a->controls.config.mouse_hand == VFXPUCK_LEFT ? "left" : "right",
                   mh->present, mh->has_orientation, a->mouse_sum_x, a->mouse_sum_y, a->mouse_buttons_seen,
                   out.puck_pitch, out.puck_roll);
            a->mouse_sum_x = a->mouse_sum_y = 0;
            a->mouse_buttons_seen = 0;
            a->mouse_log_time = t;
        }
    }
    if (out.recenter) vfxhost_head_recenter(&a->head);

    /* head */
    if (in->head_valid) vfxhost_head_update(&a->head, in->head, &yaw, &pitch, &roll);
    retrohost_vfx1_pose(a->host, yaw, pitch, roll, vfxhost_sense_units(out.puck_pitch),
                        vfxhost_sense_units(out.puck_roll), out.puck_buttons);

    /* a resumed game that has run ten seconds did not crash at start-up */
    if (a->launcher.starting && a->frames_run > 700) launcher_started(&a->launcher);

    /* run the core at its own rate against the headset's */
    a->emu_accumulator += dt;
    if (a->emu_accumulator > 0.1) a->emu_accumulator = 0.1;
    while (a->emu_accumulator >= 1.0 / fps) {
        retrohost_run(a->host);
        a->frames_run++;
        a->emu_accumulator -= 1.0 / fps;
        call_frame_hook(a);
    }

    /* newest frame: stereo the way the running game draws it */
    rgb = retrohost_frame(a->host, &w, &h, &serial);
    if (rgb && serial != a->last_serial) {
        const LauncherSettings *s = &a->launcher.settings;
        /* stereo told from flat screens by the picture, or fixed in the settings */
        int detect = s->stereo_auto;
        double strictness = 1.0 + 0.1 * (s->stereo_threshold - 3);
        int lines = 0;
        a->lines_score = a->lines_alike = -1;
        if (s->stereo == STEREO_LINES && h >= 4)
            lines = !detect ? s->stereo_force
                  : vfxhost_stereo_gate(&a->lines_gate,
                                        vfxhost_lines_look_stereo(rgb, w, h, a->lines_gate.stereo, strictness,
                                                                  &a->lines_score, &a->lines_alike));
        int pages_frame = w == 640 && h == 480 && s->stereo == STEREO_PAGES;
        int page_done = pages_frame ? vfxhost_pageflip_feed(&a->pageflip, rgb, w, h) == 1 : 0;
        /* A VFX1 title in its side-by-side mode (video mode 1 set through the
         * driver, as Gridfighter 3D's SETMODE does) draws both eyes in one
         * frame, left eye on the left, in whatever resolution it likes. */
        const uint32_t *vst = retrohost_vfx1_stats(a->host);
        /* What the game tells the VFX1, as the real headset was told. A game
         * driving the VIP's page select in the last 0.6 seconds (MadSpace,
         * menus included) is showing its VFX1 pages. One reading the head
         * tracker (Descent in flight, but also Magic Carpet's flat views) is
         * in its 3D view, stereo unless the picture plainly is one picture.
         * Without either (MechWarrior 2's menus), the picture decides. */
        int paged = 0, tracked = 0;
        if (vst) {
            uint32_t polls = vst[4] + vst[5];                   /* dataCalls, reportCalls */
            if (vst[1] != a->vfx1_psr_count) { a->vfx1_psr_count = vst[1]; a->vfx1_psr_time = t; }
            if (polls != a->vfx1_poll_count) { a->vfx1_poll_count = polls; a->vfx1_poll_time = t; }
            paged = a->vfx1_psr_time > 0 && t - a->vfx1_psr_time < 0.6;
            tracked = a->vfx1_poll_time > 0 && t - a->vfx1_poll_time < 0.6;
        }
        int side = s->stereo == STEREO_PAGES && !pages_frame && vst && vst[7] == 1 && w >= 64 &&
                   (!detect ? s->stereo_force
                            : paged ? (a->side_gate.stereo = 1)
                            : vfxhost_stereo_gate(&a->side_gate, tracked ? vfxhost_pages_look_stereo_tracked(rgb, w, h)
                                                                         : vfxhost_pages_look_stereo_at(rgb, w, h, strictness)));
        a->last_serial = serial;
        if (page_done && detect && paged)
            a->pages_gate.stereo = 1, a->pages_gate.run = 0;
        else if (page_done && detect) {
            int look = tracked ? vfxhost_pages_look_stereo_tracked(a->pageflip.eyes, a->pageflip.eyes_w, a->pageflip.eyes_h)
                               : vfxhost_pages_look_stereo_at(a->pageflip.eyes, a->pageflip.eyes_w,
                                                              a->pageflip.eyes_h, strictness);
            /* Not flipping the VIP's pages, two flat looks end stereo: a game
             * drawing one 640x480 picture (Magic Carpet after R) completes
             * few pages, and 20 of them kept it split for seconds. */
            if (vst && !look && a->pages_gate.stereo) {
                if (++a->pages_gate.run >= 2) a->pages_gate.stereo = 0, a->pages_gate.run = 0;
            } else {
                vfxhost_stereo_gate(&a->pages_gate, look);
            }
        }
        if (lines) {
            size_t need = (size_t)w * (size_t)(h / 2) * 6u;
            if (need > a->lines_eyes_size) {
                uint8_t *p = (uint8_t *)realloc(a->lines_eyes, need);
                if (p) { a->lines_eyes = p; a->lines_eyes_size = need; }
            }
            if (a->lines_eyes_size >= need) {
                vfxhost_interlace_split(rgb, w, h, a->lines_eyes);
                show(a, a->lines_eyes, w * 2, h / 2, 1);
            }
        } else if (side) {
            show(a, rgb, w, h, 1);
        } else if (pages_frame && (detect ? a->pages_gate.stereo : s->stereo_force)) {
            if (page_done) {
                show(a, a->pageflip.eyes, a->pageflip.eyes_w, a->pageflip.eyes_h, 1);
                a->last_flip_time = t;
            }
        } else {
            show_flat(a, rgb, w, h);
        }
    }
    draw_view_label(a, t);
}
