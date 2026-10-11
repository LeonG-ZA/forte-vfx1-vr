/*
 * vfxhost.h -- the headset side of a VFX1 emulation, shared by every host.
 *
 * Everything between the XR runtime and the emulator that is neither: VFX1
 * head angles from a pose, CyberPuck tilt, recovering the two eye images from
 * a page-flipped stereo frame, and the controller layout. The WebXR page
 * (as WebAssembly) and the native OpenXR app (as a static library) both use
 * it, so the two routes behave the same. See docs/ARCHITECTURE.md.
 *
 * Poses use the head frame OpenXR and WebXR share: +X right, +Y up, -Z
 * forward. Plain C99; the page-flip extractor allocates with malloc.
 */
#ifndef VFX_VFXHOST_H
#define VFX_VFXHOST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ angles */

typedef struct VfxHostQuat { double w, x, y, z; } VfxHostQuat;

/* The real VFX1's image per eye (Forte's FAQ): 35.5 x 26.4 degrees, shown as
 * a 120 inch screen at 11 feet. */
#define VFXHOST_DISPLAY_FOV_H_DEG 35.5
#define VFXHOST_DISPLAY_DISTANCE_M 3.35

VfxHostQuat vfxhost_quat_mul(VfxHostQuat a, VfxHostQuat b);
VfxHostQuat vfxhost_quat_conj(VfxHostQuat q);

/* VFX1 head tracker angles in radians: positive yaw turns left, pitch looks
 * up, roll tilts right, applied yaw then pitch then roll. */
void vfxhost_sense_angles(VfxHostQuat q, double *yaw, double *pitch, double *roll);

/* Tilt as the CyberPuck's two inclinometers read it, each -pi/2..pi/2 against
 * gravity and independent of yaw: pitch is the forward axis rising, roll the
 * right axis dropping. No 180 degree flips through vertical. */
void vfxhost_tilt_angles(VfxHostQuat q, double *pitch, double *roll);

/* Radians to VFX1 units, 65536 to a turn, wrapped to int16. */
int16_t vfxhost_sense_units(double radians);

/* Head angles relative to the orientation at the last recentre. The first
 * pose becomes the reference. */
typedef struct VfxHostHead { VfxHostQuat ref; int have_ref; } VfxHostHead;
void vfxhost_head_recenter(VfxHostHead *h);
void vfxhost_head_update(VfxHostHead *h, VfxHostQuat q,
                         int16_t *yaw, int16_t *pitch, int16_t *roll);

/* ------------------------------------------------------------ page flip */

/*
 * A VFX1 title draws a 640x480 frame holding four 320x240 pages: left and
 * right halves, on even and on odd scanlines, and flips between the parities.
 * Feed every delivered RGB frame; a parity is shown once it stops changing,
 * so a half-drawn page never is. Eyes are w x h/2 RGB, left eye left.
 */
typedef struct VfxPageFlip {
    int step;               /* sample every step pixels per row */
    uint8_t *prev;
    size_t prev_size;
    int pending, shown;
    int pending_halves;     /* bit 0 left, bit 1 right: drawn in the pending page */
    uint8_t *eyes;
    size_t eyes_size;
    int eyes_w, eyes_h;
    uint32_t flips;
} VfxPageFlip;

void vfxhost_pageflip_init(VfxPageFlip *pf, int step);
void vfxhost_pageflip_free(VfxPageFlip *pf);
/* Returns 1 when a newly completed page was copied into pf->eyes, 0 if not,
 * -1 on allocation failure. */
int vfxhost_pageflip_feed(VfxPageFlip *pf, const uint8_t *rgb, int w, int h);
void vfxhost_changed_per_parity(const uint8_t *cur, const uint8_t *prev, int w, int h,
                                int step, uint32_t changed[2]);

/* Line-interlaced stereo, what the i-glasses!, the CyberMaxx and Quake's
 * lcd_x draw: even scanlines are the left eye, odd the right. Writes the
 * eyes side by side, 2w x h/2 RGB, left eye left, the layout VfxPageFlip
 * gives. `eyes` holds at least 2 * w * (h / 2) * 3 bytes. */
void vfxhost_interlace_split(const uint8_t *rgb, int w, int h, uint8_t *eyes);

/*
 * Whether a frame really holds two eyes, so menus and other flat screens are
 * shown whole instead of being split between the eyes.
 *
 * Interlaced, two measures:
 *
 *   score   neighbouring lines against lines two apart: lines of one eye
 *           resemble each other more than neighbours do when the eyes differ
 *           (Descent 2 in flight 1.2 to 5.5); a flat screen is the other way
 *           round (Descent 2's menus and briefings 0.59 to 0.73).
 *   alike   each right-eye line against the left-eye line beside it, divided
 *           by it against the halfway point of the left-eye lines around it.
 *           Below 1 when the eyes are nearly the same picture, as in Z.A.R.,
 *           whose score then falls to 0.73 in play (alike 0.71 to 0.82); above
 *           1 when the lines are rows of one picture (flat screens 1.11 to
 *           1.66; Descent 2's stereo 1.02 to 1.08).
 *
 * vfxhost_lines_look_stereo: stereo when score is above 1 or alike below 0.9;
 * flat only when score is below 0.8 and alike above 1.05; otherwise what the
 * last frame looked (`current`). A line-doubled flat screen counts as stereo,
 * which shows it the same to both eyes.
 *
 * `strictness` moves the flat cut-offs, 1 as above: higher calls a frame flat
 * sooner (lines: score below 0.8 x strictness, alike above 1.05 / strictness;
 * pages: the eyes must match better, and the seam stand out more, to count as
 * stereo). The launcher's flat-screen threshold, 1 to 5, gives 0.8 to 1.2.
 *
 * VFX1 pages, given pf->eyes (two eyes side by side): flat only on clear
 * evidence, in a frame with enough detail to tell: one half all but empty, or
 * the two halves different pictures (alike below 0.6) without a jump at the
 * seam between two somewhat alike pictures (alike above 0.2). Stereo frames
 * measure 0.57 to 1.00 alike; MechWarrior 2's combat variables -0.06.
 */
double vfxhost_lines_stereo_score(const uint8_t *rgb, int w, int h);
double vfxhost_lines_alike_score(const uint8_t *rgb, int w, int h);
/* `score` and `alike`, if not NULL, get the two measures. */
int vfxhost_lines_look_stereo(const uint8_t *rgb, int w, int h, int current, double strictness,
                              double *score, double *alike);
int vfxhost_pages_look_stereo(const uint8_t *eyes, int w, int h);
int vfxhost_pages_look_stereo_at(const uint8_t *eyes, int w, int h, double strictness);
/* For a title reading the head tracker: stereo unless one picture clearly
 * runs on across the middle. */
int vfxhost_pages_look_stereo_tracked(const uint8_t *eyes, int w, int h);

/* Switches to stereo after `hold` frames in a row look stereo, and back to
 * flat only after `hold_flat` look flat, so a stereo scene that briefly
 * looks flat (a dark corridor) never splits into two side-by-side views. */
typedef struct VfxStereoGate { int stereo, run, hold, hold_flat; } VfxStereoGate;
int vfxhost_stereo_gate(VfxStereoGate *g, int looks_stereo);


/* ------------------------------------------------------------ controls */

/* One controller, as both runtimes report it. */
typedef struct VfxHostHand {
    int present;
    int trigger, grip, stick_press, primary, secondary;   /* A/X and B/Y */
    double stick_x, stick_y;        /* -1..1, y negative is forward */
    int has_orientation;
    VfxHostQuat orientation;        /* grip orientation, level reference space */
} VfxHostHand;

/* DOS keys the layout uses; hosts translate to their own key codes. */
typedef enum VfxHostKey {
    VFXKEY_CTRL,        /* primary fire */
    VFXKEY_SPACE,       /* secondary fire */
    VFXKEY_A,           /* accelerate */
    VFXKEY_Z,           /* reverse */
    VFXKEY_UP, VFXKEY_DOWN, VFXKEY_LEFT, VFXKEY_RIGHT,
    VFXKEY_ENTER, VFXKEY_ESCAPE,
    VFXKEY_COUNT
} VfxHostKey;

typedef enum VfxHostPuckHand { VFXPUCK_NONE, VFXPUCK_LEFT, VFXPUCK_RIGHT } VfxHostPuckHand;

/* What drives the DOS mouse. */
typedef enum VfxHostMouseMode {
    VFXMOUSE_OFF,
    VFXMOUSE_PUCK,              /* VRMOUSE: the CyberPuck's tilt is a mouse rate */
    VFXMOUSE_POINT              /* a controller's pointing moves the mouse, like an air mouse */
} VfxHostMouseMode;

typedef struct VfxHostControlsConfig {
    VfxHostPuckHand puck_hand;
    VfxHostMouseMode mouse;
    VfxHostPuckHand mouse_hand; /* VFXMOUSE_POINT: which controller points */
    double puck_rate;           /* mouse counts per second at full tilt */
    double point_scale;         /* mouse counts per radian of pointing */
} VfxHostControlsConfig;

void vfxhost_controls_default_config(VfxHostControlsConfig *c);

typedef struct VfxHostControls {
    VfxHostControlsConfig config;
    int puck_rest_set;
    VfxHostQuat puck_rest, puck_now;    /* the controller at rest and now */
    double carry_x, carry_y;
    int point_set;
    double point_yaw, point_pitch;
    int recenter_held;
} VfxHostControls;

typedef struct VfxHostControlsOut {
    uint8_t keys[VFXKEY_COUNT];
    int mouse_dx, mouse_dy;
    uint8_t mouse_buttons;          /* bit 0 left, 1 right, 2 middle */
    double puck_pitch, puck_roll;   /* radians from rest, clamped to +-pi/2 */
    uint8_t puck_buttons;           /* bit 0 trigger, 1 grip, 2 A or X */
    int recenter;                   /* 1 on the frame X was pressed */
} VfxHostControlsOut;

void vfxhost_controls_init(VfxHostControls *c, const VfxHostControlsConfig *config);
void vfxhost_controls_recenter(VfxHostControls *c);
/* Forget the CyberPuck's rest pose and the pointing reference, so the next
 * update takes them afresh; for when play resumes after a menu. */
void vfxhost_controls_rest(VfxHostControls *c);
/* Yaw (positive left) and pitch (positive up) of where a controller points. */
void vfxhost_point_angles(VfxHostQuat q, double *yaw, double *pitch);
/*
 * One update per headset frame. flying: the title is in flight, not a menu
 * (for Descent's native stereo: new stereo pages arrived in the last half
 * second). dt: seconds since the previous update.
 */
void vfxhost_controls_update(VfxHostControls *c, const VfxHostHand *left, const VfxHostHand *right,
                             int flying, double dt, VfxHostControlsOut *out);

#ifdef __cplusplus
}
#endif

#endif /* VFX_VFXHOST_H */
