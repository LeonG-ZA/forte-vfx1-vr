/*
 * vfxbridge.h -- public API of libvfxbridge.
 *
 * The bridge sits between an emulator core (VfxCoreApi, see vfxcore_api.h) and
 * an XR adapter. It owns stereo extraction, reprojection and timing
 * arbitration. It knows nothing about WebXR, OpenXR, GL, SDL, browsers or
 * windowing systems, and tools/check_layering.sh fails the build if that ever
 * stops being true.
 *
 * Threading: a VfxBridge is not internally synchronised. Drive one bridge from
 * one thread, or serialise access yourself.
 */
#ifndef VFX_VFXBRIDGE_H
#define VFX_VFXBRIDGE_H

#include "vfx/vfxcore_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VfxBridge VfxBridge;

typedef enum {
    VFX_EYE_LEFT  = 0,
    VFX_EYE_RIGHT = 1,
    VFX_EYE_COUNT = 2
} VfxEye;

/*
 * Nobody has confirmed whether
 * line-sequential titles alternate eyes by scanline within one frame or by
 * whole frame. Both are implemented and both are tested; the mode is
 * configuration, never a silent assumption. See docs/FINDINGS.md, Q3.
 */
typedef enum {
    VFX_LINESEQ_FIELD = 0, /* even/odd scanlines of one frame are L/R */
    VFX_LINESEQ_FRAME = 1  /* whole frames alternate L, R, L, R */
} VfxLineSeqMode;

typedef struct {
    /* --- stereo extraction --- */
    VfxLineSeqMode  line_sequential_mode;
    int             lineseq_left_is_first;  /* 1: field 0 / even frame_id is the
                                             * left eye. Unverified, see Q3b. */
    int             format_override_enabled; /* ignore the core's reported format */
    VfxStereoFormat format_override;

    /* --- reprojection (section 5.3) --- */
    int   reprojection_enabled;
    float source_fov_h_rad;      /* horizontal FOV one eye image covers */
    float source_fov_v_rad;      /* vertical FOV one eye image covers */
    float max_reprojection_rad;  /* beyond this the re-warp is skipped as
                                  * untrustworthy and the frame is passed
                                  * through unwarped */
    int   reprojection_bilinear; /* 1: bilinear sample, 0: nearest */

    /* --- timing arbitration (section 5.4) --- */
    double   core_hz;            /* target emulator frame rate; 0 = the host
                                  * decides when to advance the core */
    uint32_t max_catchup_frames; /* cap on core frames run in one pump, so a
                                  * stalled host cannot spiral */
} VfxBridgeConfig;

/* VFX1 optics per eye: 35.5 deg horizontal, 26.4 vertical, 45 diagonal, from
 * Forte's official VFX1 FAQ -- see docs/FINDINGS.md, Q4. */
#define VFX_DEFAULT_FOV_H_RAD 0.6195918845f /* 35.5 deg */
#define VFX_DEFAULT_FOV_V_RAD 0.4607669225f /* 26.4 deg */

void vfx_bridge_config_defaults(VfxBridgeConfig *cfg);

/* Takes a copy of *core; the caller keeps ownership of the core itself and
 * must destroy the bridge before the core. Returns NULL on bad arguments or
 * on a VFX_CORE_API_VERSION mismatch. */
VfxBridge *vfx_bridge_create(const VfxCoreApi *core, const VfxBridgeConfig *cfg);
void       vfx_bridge_destroy(VfxBridge *b);

/* Adapter -> bridge, once per HMD frame or faster. Forwarded straight to the
 * core and also cached for reprojection. cyberpuck may be NULL. */
void vfx_bridge_submit_pose(VfxBridge *b, const VfxPose *head,
                            const VfxControllerState *cyberpuck);

/* Timing arbitration. Advances the emulator core as far as now_ns says it
 * should have got to, and returns how many core frames were run. With
 * core_hz == 0 this runs exactly one core frame per call. */
uint32_t vfx_bridge_pump(VfxBridge *b, uint64_t now_ns);

typedef struct {
    uint32_t       width, height;    /* per-eye, always equal for both eyes */
    const uint8_t *rgba[VFX_EYE_COUNT];
    uint64_t       source_frame_id;
    uint64_t       source_timestamp_ns;
    int            reprojected;            /* 1 if the eyes were re-warped */
    float          reprojection_angle_rad; /* magnitude of the applied delta */
    float          coverage[VFX_EYE_COUNT];/* 0..1 fraction of output pixels
                                            * that had real source data */
    int            is_stale;               /* 1 if no new core frame since the
                                            * previous acquire */
    VfxStereoFormat source_format;
} VfxEyeFrames;

/*
 * Bridge -> adapter. Produces two eye images valid for the pose most recently
 * submitted. Buffers are owned by the bridge and stay valid until the next
 * acquire or destroy. Returns 1 on success, 0 if no frame is available yet
 * (call vfx_bridge_last_error for why).
 */
int vfx_bridge_acquire_eyes(VfxBridge *b, uint64_t now_ns, VfxEyeFrames *out);

const char *vfx_bridge_last_error(const VfxBridge *b);

/* --- pieces exposed for testing and for adapters that want them directly --- */

/* Split/deinterlace a captured frame into two eye buffers. dst_left and
 * dst_right must each hold out_w*out_h*4 bytes. Returns 1 on success. */
int vfx_stereo_extract(const VfxFrame *frame, const VfxBridgeConfig *cfg,
                       uint8_t *dst_left, uint8_t *dst_right,
                       uint32_t *out_w, uint32_t *out_h);

/* Per-eye dimensions vfx_stereo_extract will produce for this frame. */
int vfx_stereo_eye_size(const VfxFrame *frame, const VfxBridgeConfig *cfg,
                        uint32_t *out_w, uint32_t *out_h);

/*
 * Orientation-only re-warp. Treats src as a pinhole image covering
 * fov_h x fov_v taken with orientation render_q, and resamples it as if taken
 * with current_q. Returns the fraction of destination pixels that landed
 * inside the source image.
 */
float vfx_reproject_orientation(const uint8_t *src, uint32_t sw, uint32_t sh,
                                uint8_t *dst, uint32_t dw, uint32_t dh,
                                VfxQuat render_q, VfxQuat current_q,
                                float fov_h, float fov_v, int bilinear);

#ifdef __cplusplus
}
#endif

#endif /* VFX_VFXBRIDGE_H */
