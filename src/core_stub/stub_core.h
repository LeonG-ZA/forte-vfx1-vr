/*
 * stub_core.h -- synthetic emulator core.
 *
 * Stands in for the future DOSBox-X core so the VfxCoreApi contract, the
 * stereo extraction paths and the reprojection layer can all be validated
 * before any DOS hardware emulation exists. It renders a small raycast scene
 * from the latched head pose, at whatever cadence the caller pumps it, and
 * packs the result into the stereo format under test.
 */
#ifndef VFX_STUB_CORE_H
#define VFX_STUB_CORE_H

#include "vfx/vfxcore_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t        eye_width;    /* per-eye; VFX1 panels were 263x230 */
    uint32_t        eye_height;
    VfxStereoFormat format;
    int             line_sequential_by_frame; /* 1: whole frames alternate eyes */
    int             lineseq_left_is_first;
    int             provide_split;  /* also fill left_rgba/right_rgba */
    float           ipd_meters;     /* eye separation used to render parallax */
    float           fov_h_rad;
    float           fov_v_rad;
    int             report_render_pose; /* fill VfxFrame.render_pose */
} VfxStubCoreConfig;

void vfx_stub_core_config_defaults(VfxStubCoreConfig *cfg);

/* Fills *out with a ready-to-use VfxCoreApi. Returns 1 on success. Destroy
 * through out->destroy(out->handle). */
int vfx_stub_core_create(const VfxStubCoreConfig *cfg, VfxCoreApi *out);

/* Test hooks: what the core last received and how far it has run. */
VfxPose  vfx_stub_core_last_pose(void *handle);
VfxControllerState vfx_stub_core_last_controller(void *handle);
uint64_t vfx_stub_core_frame_count(void *handle);

#ifdef __cplusplus
}
#endif

#endif /* VFX_STUB_CORE_H */
