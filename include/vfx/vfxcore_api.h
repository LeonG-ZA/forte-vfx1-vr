/*
 * vfxcore_api.h -- Emulator-to-bridge contract.
 *
 * HARD REQUIREMENT: nothing in this header may refer to WebXR, OpenXR, GL,
 * WebGL, SDL, Emscripten, or any windowing system. It is plain C99 plus
 * <stdint.h>/<stddef.h> so it compiles unchanged for a native target or for
 * WASM. tools/check_layering.sh mechanically enforces this.
 */
#ifndef VFX_VFXCORE_API_H
#define VFX_VFXCORE_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever the layout of any struct below changes. A core and a bridge
 * built from different headers must refuse to talk to each other. */
#define VFX_CORE_API_VERSION 1u

/* Orientation only -- position tracking did not exist on real VFX1 hardware,
 * but there is room for 6DOF because a modern reimplementation may want
 * positional data for comfort/reprojection even though the original games
 * never asked for it. */
typedef struct {
    float w, x, y, z;      /* quaternion, right-handed, +Y up, +Z toward viewer */
} VfxQuat;

typedef struct {
    float x, y, z;         /* meters; may be all-zero if unused */
} VfxVec3;

typedef struct {
    VfxQuat  orientation;
    VfxVec3  position;     /* optional; core may ignore */
    uint64_t timestamp_ns; /* monotonic clock, for extrapolation */
} VfxPose;

typedef struct {
    float   tilt_x, tilt_y; /* CyberPuck 2DOF tilt, normalized -1..1 */
    uint8_t buttons;        /* bitmask, 3 buttons used historically */
} VfxControllerState;

typedef enum {
    VFX_STEREO_NONE = 0,        /* game not currently in VFX1-aware mode */
    VFX_STEREO_SIDE_BY_SIDE = 1,
    VFX_STEREO_LINE_SEQUENTIAL = 2
} VfxStereoFormat;

/*
 * The pose a frame was rendered with.
 *
 * Reprojection needs the head pose a frame was rendered against: the
 * re-warp is driven by the delta between "pose the frame was rendered with"
 * and "pose right now". Without it the bridge has to guess, and the guess is
 * systematically stale by one DOS frame (28-65 ms at the 15-35 Hz DOS games
 * ran at), which is the exact discomfort the layer exists to remove.
 *
 * render_pose is therefore optional-but-preferred: a core that latches the
 * pose at the start of its frame sets render_pose_valid = 1. A core that
 * cannot sets it to 0 and the bridge falls back to its own estimate. Adding a
 * field keeps every existing member at its original offset.
 */
typedef struct {
    VfxStereoFormat format;
    uint32_t        width, height;    /* per-eye dimensions */
    const uint8_t  *left_rgba;        /* may be NULL if format doesn't split cleanly */
    const uint8_t  *right_rgba;
    const uint8_t  *combined_rgba;    /* raw captured frame, for NONE/line-sequential */
    uint64_t        frame_id;

    VfxPose  render_pose;             /* pose latched when this frame started */
    uint32_t render_pose_valid;       /* 0 = render_pose is meaningless */
    uint32_t combined_width;          /* dimensions of combined_rgba, which are */
    uint32_t combined_height;         /* NOT per-eye for SBS / line-sequential */
} VfxFrame;

/*
 * Buffer ownership, which section 4 left open:
 *   - The core owns every pixel buffer it hands out.
 *   - Pointers stay valid until the next VfxRunFrameFn or VfxDestroyFn call on
 *     the same handle. A caller that needs them longer must copy.
 *   - The core must not mutate a buffer it has already handed out during that
 *     window.
 */

/* Core -> caller: pull the most recent frame (non-blocking, returns last
 * available; do not assume this changes every call). */
typedef VfxFrame (*VfxGetFrameFn)(void *core_handle);

/* Caller -> core: push a fresh head pose + controller state. Call this as
 * often as new tracking data is available (e.g. once per HMD frame), NOT tied
 * to the DOS game's own internal frame rate. The core caches the latest pose
 * and applies it to whatever internal register the DOS driver reads next. */
typedef void (*VfxSubmitPoseFn)(void *core_handle, const VfxPose *head,
                                const VfxControllerState *cyberpuck);

/* Lifecycle. */
typedef void *(*VfxCreateFn)(const char *dos_game_path, const char *config_path);
typedef void  (*VfxDestroyFn)(void *core_handle);
typedef void  (*VfxRunFrameFn)(void *core_handle); /* advances emulation */

/*
 * The typedefs above are the contract verbatim. VfxCoreApi bundles them so a
 * core can be swapped at link time or load time without the bridge knowing
 * which one it got -- the stub core in src/core_stub and a future DOSBox-X
 * core both fill in this struct.
 */
typedef struct {
    uint32_t        api_version;   /* must equal VFX_CORE_API_VERSION */
    void           *handle;        /* opaque core instance */
    VfxGetFrameFn   get_frame;
    VfxSubmitPoseFn submit_pose;
    VfxDestroyFn    destroy;
    VfxRunFrameFn   run_frame;
} VfxCoreApi;

#ifdef __cplusplus
}
#endif

#endif /* VFX_VFXCORE_API_H */
