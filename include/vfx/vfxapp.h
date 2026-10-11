/*
 * vfxapp.h -- one VFX1 session: the core, the launcher menu, the controller
 * layout, the head, and the stereo the running game draws.
 *
 * Everything the headset apps do between the XR runtime and the emulator,
 * with no XR, graphics or OS API: the host feeds it the controllers and the
 * head each frame, then shows `image` (one eye on each half when
 * `stereo_live`) and, while the menu is open, `menu_rgba`. The Quest and
 * Windows OpenXR apps share it through app/xr/xrapp.c; a WebXR page can call
 * it compiled to WebAssembly.
 */
#ifndef VFX_VFXAPP_H
#define VFX_VFXAPP_H

#include <stddef.h>
#include <stdint.h>
#include "vfx/launcher.h"
#include "vfx/retrohost.h"
#include "vfx/vfxdepth.h"
#include "vfx/vfxhost.h"
#include "vfx/vfxmenu.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VFXAPP_AUDIO_RING (1 << 16)     /* stereo frames */

typedef struct VfxAppInput {
    VfxHostHand left, right;
    int menu_button;                    /* held: opens and closes the menu */
    int head_valid;
    VfxHostQuat head;                   /* the headset's orientation */
    double now_s;                       /* a monotonic clock */
    /* Optional, for a desktop host: a real keyboard and mouse, added to what
     * the controllers do. dos_keys holds RETROK_LAST entries, 1 while held. */
    const uint8_t *dos_keys;
    int mouse_dx, mouse_dy;
    unsigned mouse_buttons;             /* bit 0 left, 1 right, 2 middle */
} VfxAppInput;

/* Optional: called with every new frame the core draws, before any stereo
 * extraction, for a debugging host to record. */
typedef void (*VfxAppFrameHook)(void *user, const uint8_t *rgb, int w, int h, uint64_t serial);

typedef struct VfxApp {
    RetroHost *host;
    Launcher launcher;
    int exit_requested;

    /* what to show; the pointer stays valid until the next vfxapp_frame */
    const uint8_t *image;               /* RGB */
    int image_w, image_h;
    int image_changed;                  /* 1: upload image this frame */
    int stereo_live;                    /* 1: left eye on the left half, right on the right */
    double lines_score, lines_alike;    /* the interlaced-stereo measures of the last frame, -1 none */
    uint8_t menu_rgba[VFXMENU_WIDTH * VFXMENU_HEIGHT * 4];
    int menu_changed;

    /* private */
    VfxPageFlip pageflip;
    VfxStereoGate lines_gate, pages_gate, side_gate;
    uint8_t *lines_eyes;
    size_t lines_eyes_size;
    VfxHostHead head;
    VfxHostControls controls;
    uint64_t last_serial;
    double last_flip_time, emu_accumulator, last_frame_time;
    double mouse_log_time;
    uint32_t frames_run;                /* core frames since start-up */
    uint32_t vfx1_psr_count, vfx1_poll_count;   /* page select writes and head reads, last seen */
    double vfx1_psr_time, vfx1_poll_time;       /* when each last went up */
    int mouse_sum_x, mouse_sum_y, mouse_buttons_seen;
    VfxMenuRepeat rep_up, rep_down, rep_left, rep_right;
    int menu_button_held, activate_held, back_held;
    /* the mode key's label, shown a moment in the top right corner */
    int mode_key_held;
    const char *view_label;
    double view_label_time;
    int label_drawn;
    const uint8_t *base_image;          /* what show() chose, without the label */
    int base_w, base_h, base_stereo;
    uint8_t *label_image;
    size_t label_image_size;
    /* fake 3D (vfxdepth) for screens shown flat: the depth network on its
     * own thread, fed the newest flat frame whenever it is free */
    VfxFake3D fake3d;
    struct VfxDepthWorker *depth;
    int depth_tried;                    /* 1 once loading the network was tried */
    uint8_t *fake_eyes;
    size_t fake_eyes_size;
    VfxAppFrameHook frame_hook;
    void *frame_hook_user;
    uint64_t hook_serial;
    int16_t audio_ring[VFXAPP_AUDIO_RING * 2];
    volatile unsigned audio_read, audio_write;
} VfxApp;

void vfxapp_set_frame_hook(VfxApp *a, VfxAppFrameHook hook, void *user);

/* `core`: the DOSBox Pure library; `files`: game zips, saves and settings;
 * `temp`: scratch files; `server`: the dev PC's game listing, or "". */
int vfxapp_init(VfxApp *a, const char *core, const char *files, const char *temp, const char *server);
void vfxapp_frame(VfxApp *a, const VfxAppInput *in);
/* 1 while a game is loaded and its image should be drawn */
int vfxapp_game_visible(const VfxApp *a);

/* A real keyboard and mouse (VFX1 Desk's, a Quest's Bluetooth ones) added
 * to the controllers' input: with the menu open the arrows, Enter and
 * Backspace or Escape drive it, otherwise the keys go to DOS and the mouse
 * is the DOS mouse; F11 opens and closes the menu. `keys` holds RETROK_LAST
 * entries, 1 while held, and must last until vfxapp_frame. */
void vfxapp_add_keyboard_mouse(const VfxApp *a, VfxAppInput *in, const uint8_t *keys, int dx, int dy,
                               unsigned buttons);
/* Up to `frames` stereo frames of sound, from the audio thread; the rest of
 * `out` is filled with silence. */
void vfxapp_audio_pull(VfxApp *a, int16_t *out, size_t frames);
void vfxapp_close(VfxApp *a);

#ifdef __cplusplus
}
#endif

#endif
