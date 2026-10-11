/*
 * vfxmouse.h -- head pose to relative mouse motion.
 *
 * The VFX1 shipped a TSR called VRMOUSE that presented head orientation to DOS
 * software as an ordinary INT 33h mouse. Many titles used that instead of
 * talking to the VIP card, which makes it the one integration path that needs
 * no reverse engineering at all. This is the arithmetic for it.
 *
 * The job is harder than it sounds in one specific way: head tracking is
 * absolute and a mouse is relative. Everything below exists to convert the one
 * into the other without drifting, dithering, or flinging the view when the
 * yaw angle wraps.
 *
 * Lives in libvfxbridge because it is register emulation glue:
 * platform-neutral, testable on its own, and used identically by
 * a DOSBox-X core and by anything else that needs it.
 */
#ifndef VFX_VFXMOUSE_H
#define VFX_VFXMOUSE_H

#include "vfx/vfxcore_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* VRMOUSE had -XX and -XY flags to disable an axis. Same idea. */
    int enable_yaw;
    int enable_pitch;

    /*
     * Sign conventions, which are defaults rather than truths:
     *   yaw   -- turning the head left (yaw positive) emits negative mouse X,
     *            because moving a mouse right turns a view right.
     *   pitch -- looking up (pitch positive) emits negative mouse Y, because
     *            screen Y grows downward.
     * Whether a given DOS title agrees is per-title. See FINDINGS Q7.
     */
    int invert_yaw;
    int invert_pitch;

    /* Mouse counts emitted per radian of head rotation. There is no correct
     * value: it depends on the title's own mouse sensitivity. See Q8. */
    float counts_per_radian_yaw;
    float counts_per_radian_pitch;

    /* Tracked pitch is clamped to this, so motion past the limit emits nothing
     * and coming back does not have to unwind phantom travel. Defaults to the
     * real hardware's +-70 degrees. 0 disables the clamp. */
    float pitch_limit_rad;

    /* Upper bound on counts emitted in one update, discarding the excess. A
     * tracker glitch or a recentre should not fling the view across the map.
     * 0 disables the clamp. */
    int32_t max_counts_per_update;
} VfxMouseMapConfig;

typedef struct {
    int32_t dx, dy;
} VfxMouseDelta;

/*
 * Plain value type, no allocation, so an emulator core can embed one. Treat
 * the fields as private; they are visible only to keep this malloc-free.
 */
typedef struct {
    VfxMouseMapConfig cfg;
    int      primed;        /* 0 until the first pose establishes a reference */
    float    ref_yaw;       /* last absolute angles converted to motion */
    float    ref_pitch;
    float    accum_x;       /* sub-count residue, so slow motion still moves */
    float    accum_y;
    uint64_t clamped;       /* updates that hit max_counts_per_update */
} VfxMouseMap;

void vfx_mousemap_config_defaults(VfxMouseMapConfig *cfg);

/* cfg may be NULL for the defaults. */
void vfx_mousemap_init(VfxMouseMap *m, const VfxMouseMapConfig *cfg);

/* Re-zero against whatever pose arrives next, emitting no motion for it. Use
 * when the user wants "forward" to mean where they are looking now. */
void vfx_mousemap_recenter(VfxMouseMap *m);

/* Convert one absolute head pose into the relative motion to inject. Returns
 * {0,0} for the first pose after init or recenter, and whenever the residue
 * has not yet accumulated a whole count. */
VfxMouseDelta vfx_mousemap_update(VfxMouseMap *m, const VfxPose *head);

/* Tracked angles in radians, after clamping. For diagnostics and overlays. */
void vfx_mousemap_tracked(const VfxMouseMap *m, float *yaw, float *pitch);

#ifdef __cplusplus
}
#endif

#endif /* VFX_VFXMOUSE_H */
