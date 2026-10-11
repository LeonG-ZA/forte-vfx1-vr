/*
 * vfx-retro-headless -- run a libretro DOS core with the VFX1, no window.
 *
 * The test bench for the native route: the same frontend (retrohost) and the
 * same headset logic (vfxhost) the OpenXR app uses, driven by a scripted head
 * sweep and scripted keys instead of a headset, in WSL or any Linux.
 *
 *   vfx-retro-headless --core dosbox_pure_libretro.so --content SENSE.COM \
 *       [--seconds 12] [--sweep] [--option key=value]... \
 *       [--key SECONDS,RETROK,HOLD]... [--mouse SECONDS,DX,DY,BUTTONS,HOLD]...
 *       [--shot SECONDS,file.png]... [--png last.png] [--eyes eyes.png]
 *
 * Prints one JSON line: frames run, the last frame's size, stereo pages the
 * page-flip extractor completed, emulation speed, and the VFX1 device's
 * counters. Exit status 0 if the core loaded and ran.
 */
#define _POSIX_C_SOURCE 200809L
#include "vfx/retrohost.h"
#include "vfx/vfxhost.h"
#include "../headless/png_write.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#endif

#define MAX_KEYS 64

typedef struct KeyEvent { double at, hold; unsigned key; int state; } KeyEvent;
/* A mouse move of DX,DY at SECONDS, with BUTTONS (1 left, 2 right) held for HOLD. */
typedef struct MouseEvent { double at, hold; int dx, dy, buttons, state; } MouseEvent;
typedef struct ShotEvent { double at; char path[256]; int done; } ShotEvent;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void quiet_log(void *user, int level, const char *line)
{
    (void)user;
    if (level >= 2 || strstr(line, "VFX1")) fprintf(stderr, "core: %s", line);    /* warnings, errors, VFX1 */
}

static int write_rgb_png(const char *path, const uint8_t *rgb, int w, int h)
{
    size_t n = (size_t)w * (size_t)h, i;
    uint8_t *rgba = (uint8_t *)malloc(n * 4);
    int ok;
    if (!rgba) return 0;
    for (i = 0; i < n; i++) {
        rgba[i * 4] = rgb[i * 3]; rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2]; rgba[i * 4 + 3] = 255;
    }
    ok = vfx_png_write_rgba(path, rgba, (uint32_t)w, (uint32_t)h);   /* 1 on success */
    free(rgba);
    return ok;
}

int main(int argc, char **argv)
{
    const char *core = NULL, *content = NULL, *png = NULL, *eyes_png = NULL;
    double seconds = 12.0;
    int sweep = 0, i, frames, f, last_w = 0, last_h = 0, list_options = 0, switched = 0;
    const char *then = NULL;              /* CONTENT@SECONDS: switch content mid-run */
    KeyEvent keys[MAX_KEYS];
    int key_count = 0;
    MouseEvent mice[MAX_KEYS];
    int mouse_count = 0;
    ShotEvent shots[MAX_KEYS];
    int shot_count = 0;
    char err[512];
    RetroHost *host;
    VfxPageFlip pf;
    VfxHostHead head;
    uint64_t last_serial = 0;
    uint32_t new_frames = 0;
    const uint32_t *st;
    double t0, wall, fps;

    vfxhost_pageflip_init(&pf, 8);
    memset(&head, 0, sizeof head);

    /* Options first, so they exist when the core reads them on load. */
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--core") && i + 1 < argc) core = argv[++i];
        else if (!strcmp(argv[i], "--content") && i + 1 < argc) content = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sweep")) sweep = 1;
        else if (!strcmp(argv[i], "--options")) list_options = 1;
        else if (!strcmp(argv[i], "--then") && i + 1 < argc) then = argv[++i];
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(argv[i], "--eyes") && i + 1 < argc) eyes_png = argv[++i];
        else if (!strcmp(argv[i], "--option") && i + 1 < argc) i++;
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
            KeyEvent *k = &keys[key_count];
            if (key_count < MAX_KEYS && sscanf(argv[++i], "%lf,%u,%lf", &k->at, &k->key, &k->hold) == 3) {
                k->state = 0;
                key_count++;
            }
        } else if (!strcmp(argv[i], "--mouse") && i + 1 < argc) {
            MouseEvent *m = &mice[mouse_count];
            if (mouse_count < MAX_KEYS && sscanf(argv[++i], "%lf,%d,%d,%d,%lf", &m->at, &m->dx, &m->dy, &m->buttons, &m->hold) == 5) {
                m->state = 0;
                mouse_count++;
            }
        } else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            ShotEvent *sh = &shots[shot_count];
            if (shot_count < MAX_KEYS && sscanf(argv[++i], "%lf,%255s", &sh->at, sh->path) == 2) {
                sh->done = 0;
                shot_count++;
            }
        } else {
            fprintf(stderr, "unknown or incomplete argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!core || !content) {
        fprintf(stderr, "usage: %s --core CORE.so --content FILE [--seconds N] [--sweep] "
                        "[--option k=v]... [--key SECONDS,RETROK,HOLD]... [--mouse SECONDS,DX,DY,BUTTONS,HOLD]... "
                        "[--shot SECONDS,F]... [--png F] [--eyes F]\n", argv[0]);
        return 2;
    }

    mkdir("build/retro-system", 0755);
    mkdir("build/retro-save", 0755);
    host = retrohost_open(core, "build/retro-system", "build/retro-save", err, sizeof err);
    if (!host) { fprintf(stderr, "%s\n", err); return 1; }
    retrohost_set_log(host, quiet_log, NULL);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--option") && i + 1 < argc) {
            char kv[400], *eq;
            snprintf(kv, sizeof kv, "%s", argv[++i]);
            eq = strchr(kv, '=');
            if (eq) { *eq = 0; retrohost_set_option(host, kv, eq + 1); }
        }
    }
    {
        /* HOST_TRACKER=PORT,KIND: plug an emulated serial head tracker in, as
         * the app's per-game setting does (KIND 0 i-glasses, 1 CyberMaxx) */
        const char *ht = getenv("HOST_TRACKER");
        int port, kind;
        if (ht && sscanf(ht, "%d,%d", &port, &kind) == 2) retrohost_vfx1_serial_tracker(host, port, kind);
        /* HOST_GAIN=N: the app's head tracker gain */
        if (getenv("HOST_GAIN")) retrohost_vfx1_tracker_gain(host, atof(getenv("HOST_GAIN")));
        /* HOST_STEER=1: frame-rate matched steering, as the app sets per game */
        if (getenv("HOST_STEER")) retrohost_vfx1_steer(host, atoi(getenv("HOST_STEER")));
    }
    if (!retrohost_load(host, content)) {
        fprintf(stderr, "core refused %s\n", content);
        retrohost_close(host);
        return 1;
    }

    if (list_options) {
        for (i = 0; i < retrohost_option_count(host); i++) {
            int v;
            fprintf(stderr, "option %s [%s] =", retrohost_option_key(host, i), retrohost_option_label(host, i));
            for (v = 0; v < retrohost_option_value_count(host, i) && v < 6; v++)
                fprintf(stderr, " %s", retrohost_option_value(host, i, v));
            fprintf(stderr, "%s\n", retrohost_option_value_count(host, i) > 6 ? " ..." : "");
        }
        fprintf(stderr, "%d options\n", retrohost_option_count(host));
    }

    fps = retrohost_fps(host);
    if (fps <= 0) fps = 60.0;
    frames = (int)(seconds * fps);
    t0 = now_s();
    for (f = 0; f < frames; f++) {
        double t = f / fps;
        const uint8_t *rgb;
        uint64_t serial;

        for (i = 0; i < key_count; i++) {
            KeyEvent *k = &keys[i];
            if (k->state == 0 && t >= k->at) { retrohost_key(host, k->key, 1); k->state = 1; }
            if (k->state == 1 && t >= k->at + k->hold) { retrohost_key(host, k->key, 0); k->state = 2; }
        }
        {
            int dx = 0, dy = 0, buttons = 0, active = 0;
            for (i = 0; i < mouse_count; i++) {
                MouseEvent *m = &mice[i];
                if (m->state == 0 && t >= m->at) { dx += m->dx; dy += m->dy; m->state = 1; active = 1; }
                if (m->state == 1) {
                    if (t < m->at + m->hold) buttons |= m->buttons;
                    else { m->state = 2; active = 1; }
                }
                if (m->state == 1) active = 1;
            }
            if (active) retrohost_mouse(host, dx, dy, buttons);
        }
        if (sweep) {
            /* The sweep web/vfx1.html's pose=fake uses, through vfxhost. */
            double yaw = 0.9 * sin(2 * 3.14159265358979 * t / 8), pitch = 0.25 * sin(2 * 3.14159265358979 * t / 5);
            /* SWEEP_AXES=y or p sweeps only yaw or only pitch */
            const char *axes = getenv("SWEEP_AXES");
            if (axes && !strchr(axes, 'y')) yaw = 0;
            if (axes && !strchr(axes, 'p')) pitch = 0;
            VfxHostQuat qy, qp;
            int16_t y, p, r;
            qy.w = cos(yaw / 2); qy.x = 0; qy.y = sin(yaw / 2); qy.z = 0;
            qp.w = cos(pitch / 2); qp.x = sin(pitch / 2); qp.y = 0; qp.z = 0;
            vfxhost_head_update(&head, vfxhost_quat_mul(qy, qp), &y, &p, &r);
            {
                /* CONST_POSE=Y,P,R,T: hold this head pose (degrees) instead of the
                 * sweep, from T seconds on; zero before */
                const char *cp = getenv("CONST_POSE");
                double cy, cpi, cr, ct;
                if (cp && sscanf(cp, "%lf,%lf,%lf,%lf", &cy, &cpi, &cr, &ct) == 4) {
                    int on = t >= ct;
                    y = vfxhost_sense_units(on ? cy * 3.14159265358979 / 180 : 0);
                    p = vfxhost_sense_units(on ? cpi * 3.14159265358979 / 180 : 0);
                    r = vfxhost_sense_units(on ? cr * 3.14159265358979 / 180 : 0);
                }
            }
            {
                /* CONST_PUCK=PITCH,ROLL,T: hold the CyberPuck tilted (degrees) from T on */
                const char *cp = getenv("CONST_PUCK");
                double pp, pr, pt;
                if (cp && sscanf(cp, "%lf,%lf,%lf", &pp, &pr, &pt) == 3) {
                    int on = t >= pt;
                    retrohost_vfx1_pose(host, y, p, r, vfxhost_sense_units(on ? pp * 3.14159265358979 / 180 : 0),
                                        vfxhost_sense_units(on ? pr * 3.14159265358979 / 180 : 0), 0);
                } else {
                    retrohost_vfx1_pose(host, y, p, r, vfxhost_sense_units(0.5 * sin(t)), 0, (int)t % 8);
                }
            }
        }

        if (then && !switched) {
            const char *at = strrchr(then, '@');
            if (at && t >= atof(at + 1)) {
                char path[1024];
                snprintf(path, sizeof path, "%.*s", (int)(at - then), then);
                retrohost_unload(host);
                if (!retrohost_load(host, path)) { fprintf(stderr, "core refused %s\n", path); break; }
                fprintf(stderr, "switched to %s at %.1f s\n", path, t);
                switched = 1;
            }
        }

        retrohost_run(host);

        rgb = retrohost_frame(host, &last_w, &last_h, &serial);
        if (rgb && serial != last_serial) {
            last_serial = serial;
            new_frames++;
            if (last_w == 640 && last_h == 480) vfxhost_pageflip_feed(&pf, rgb, last_w, last_h);
        }
        {
            /* STATS_EVERY=S: every S seconds, what the title told the VFX1 in
             * that time (page select writes, video mode) and the frame size;
             * STATS_SHOTS=DIR also saves the frame then, as DIR/tNNN.png */
            static double next_stats = -1, every = 0;
            static uint32_t last_psr_writes, last_data, last_reports;
            const char *se = getenv("STATS_EVERY");
            if (next_stats < 0) { every = se ? atof(se) : 0; next_stats = every; }
            if (every > 0 && t >= next_stats) {
                const uint32_t *vs = retrohost_vfx1_stats(host);
                const char *dir = getenv("STATS_SHOTS");
                if (vs) {
                    fprintf(stderr, "stats t=%5.1f psrWrites +%u psr %02X mode %u polls +%u size %dx%d\n", t,
                            vs[1] - last_psr_writes, vs[2], vs[7], (vs[4] - last_data) + (vs[5] - last_reports),
                            last_w, last_h);
                    last_psr_writes = vs[1]; last_data = vs[4]; last_reports = vs[5];
                }
                if (dir) {
                    char path[1024];
                    const uint8_t *img = retrohost_frame(host, &last_w, &last_h, NULL);
                    snprintf(path, sizeof path, "%s/t%03d.png", dir, (int)(t + 0.5));
                    if (img) write_rgb_png(path, img, last_w, last_h);
                }
                next_stats += every;
            }
        }
        for (i = 0; i < shot_count; i++) {
            ShotEvent *sh = &shots[i];
            if (!sh->done && t >= sh->at) {
                const uint8_t *img = retrohost_frame(host, &last_w, &last_h, NULL);
                if (img && !write_rgb_png(sh->path, img, last_w, last_h)) fprintf(stderr, "could not write %s\n", sh->path);
                sh->done = 1;
            }
        }
    }
    wall = now_s() - t0;

    st = retrohost_vfx1_stats(host);
    printf("{\"frames\":%d,\"newFrames\":%u,\"width\":%d,\"height\":%d,\"stereoFlips\":%u,"
           "\"emulatedSeconds\":%.2f,\"wallSeconds\":%.2f,\"vfx1\":",
           frames, new_frames, last_w, last_h, pf.flips, frames / fps, wall);
    if (st)
        printf("{\"poses\":%u,\"psrWrites\":%u,\"psr\":%u,\"statusCalls\":%u,\"dataCalls\":%u,"
               "\"reportCalls\":%u,\"zeroCalls\":%u,\"videoMode\":%u,\"vipWrites\":%u}}\n",
               st[0], st[1], st[2], st[3], st[4], st[5], st[6], st[7], st[8]);
    else
        printf("null}\n");

    if (png) {
        const uint8_t *rgb = retrohost_frame(host, &last_w, &last_h, NULL);
        if (rgb && !write_rgb_png(png, rgb, last_w, last_h)) fprintf(stderr, "could not write %s\n", png);
    }
    if (eyes_png && pf.eyes) {
        if (!write_rgb_png(eyes_png, pf.eyes, pf.eyes_w, pf.eyes_h)) fprintf(stderr, "could not write %s\n", eyes_png);
    }

    vfxhost_pageflip_free(&pf);
    retrohost_close(host);
    return 0;
}
