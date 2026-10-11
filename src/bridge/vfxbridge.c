/*
 * vfxbridge.c -- libvfxbridge core.
 *
 * Owns pose ingestion, stereo extraction glue, reprojection, and the timing
 * arbitration that decouples the DOS game frame clock from the headset one.
 * Contains no reference to any XR API by design.
 */
#include "vfx/vfxbridge.h"
#include "vfxmath.h"
#include "stereo_internal.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define VFX_POSE_HISTORY 64u

typedef struct {
    uint64_t t_ns;
    VfxQuat  q;
} PoseSample;

struct VfxBridge {
    VfxCoreApi      core;
    VfxBridgeConfig cfg;

    /* latest tracking state from the adapter */
    VfxPose            head;
    VfxControllerState puck;
    int                have_pose;

    PoseSample hist[VFX_POSE_HISTORY];
    uint32_t   hist_count;
    uint32_t   hist_head;

    /* timing arbitration */
    uint64_t next_core_frame_ns;
    int      timing_primed;

    /* extracted eyes at source resolution */
    uint8_t *eye_src[VFX_EYE_COUNT];
    /* reprojected eyes; only allocated when reprojection is on */
    uint8_t *eye_out[VFX_EYE_COUNT];
    size_t   eye_bytes;
    uint32_t eye_w, eye_h;

    /* which eye of a FRAME-mode line-sequential pair we have seen */
    int lineseq_have[VFX_EYE_COUNT];

    uint64_t        last_frame_id;
    uint64_t        last_frame_ns;
    uint64_t        prev_frame_ns;
    int             have_frame;
    VfxQuat         frame_render_q;
    VfxStereoFormat frame_format;

    const char *err;
};

/* ------------------------------------------------------------------ */

void vfx_bridge_config_defaults(VfxBridgeConfig *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->line_sequential_mode  = VFX_LINESEQ_FIELD;
    cfg->lineseq_left_is_first = 1;
    cfg->reprojection_enabled  = 1;
    cfg->source_fov_h_rad      = VFX_DEFAULT_FOV_H_RAD;
    cfg->source_fov_v_rad      = VFX_DEFAULT_FOV_V_RAD;
    /* Past 45 degrees the re-warp is mostly invented, so stop pretending. */
    cfg->max_reprojection_rad  = 0.7853981634f;
    cfg->reprojection_bilinear = 1;
    cfg->core_hz               = 0.0;
    cfg->max_catchup_frames    = 4u;
}

VfxBridge *vfx_bridge_create(const VfxCoreApi *core, const VfxBridgeConfig *cfg)
{
    VfxBridge *b;

    if (!core || !core->get_frame || !core->submit_pose || !core->run_frame)
        return NULL;
    if (core->api_version != VFX_CORE_API_VERSION)
        return NULL;

    b = (VfxBridge *)calloc(1, sizeof(*b));
    if (!b)
        return NULL;

    b->core = *core;
    if (cfg)
        b->cfg = *cfg;
    else
        vfx_bridge_config_defaults(&b->cfg);

    b->head.orientation = vfx_quat_identity();
    b->frame_render_q   = vfx_quat_identity();
    b->err              = "no frame acquired yet";
    return b;
}

void vfx_bridge_destroy(VfxBridge *b)
{
    int e;
    if (!b)
        return;
    for (e = 0; e < VFX_EYE_COUNT; ++e) {
        free(b->eye_src[e]);
        free(b->eye_out[e]);
    }
    free(b);
}

const char *vfx_bridge_last_error(const VfxBridge *b)
{
    return b ? b->err : "null bridge";
}

/* ------------------------------------------------------------------ */

void vfx_bridge_submit_pose(VfxBridge *b, const VfxPose *head,
                            const VfxControllerState *puck)
{
    if (!b || !head)
        return;

    b->head = *head;
    b->head.orientation = vfx_quat_normalize(b->head.orientation);
    b->have_pose = 1;

    if (puck)
        b->puck = *puck;

    /* Keep a short history so we can reconstruct which pose a frame was most
     * likely rendered against, for cores that do not report render_pose. */
    b->hist[b->hist_head].t_ns = head->timestamp_ns;
    b->hist[b->hist_head].q    = b->head.orientation;
    b->hist_head = (b->hist_head + 1u) % VFX_POSE_HISTORY;
    if (b->hist_count < VFX_POSE_HISTORY)
        ++b->hist_count;

    b->core.submit_pose(b->core.handle, &b->head, puck ? puck : &b->puck);
}

/* Interpolate the head orientation we had at t_ns. Falls back to the oldest or
 * newest sample outside the recorded window. */
static VfxQuat pose_at(const VfxBridge *b, uint64_t t_ns)
{
    uint32_t i, n = b->hist_count;
    const PoseSample *newer = NULL, *older = NULL;

    if (!n)
        return vfx_quat_identity();

    /* Walk newest to oldest. */
    for (i = 0; i < n; ++i) {
        uint32_t idx = (b->hist_head + VFX_POSE_HISTORY - 1u - i) % VFX_POSE_HISTORY;
        const PoseSample *s = &b->hist[idx];
        if (s->t_ns <= t_ns) { older = s; break; }
        newer = s;
    }
    if (!older)
        return newer ? newer->q : vfx_quat_identity();
    if (!newer || newer->t_ns <= older->t_ns)
        return older->q;
    {
        float span = (float)(newer->t_ns - older->t_ns);
        float t = span > 0.0f ? (float)(t_ns - older->t_ns) / span : 0.0f;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        return vfx_quat_slerp(older->q, newer->q, t);
    }
}

/* ------------------------------------------------------------------ */

uint32_t vfx_bridge_pump(VfxBridge *b, uint64_t now_ns)
{
    uint32_t ran = 0;

    if (!b)
        return 0;

    if (b->cfg.core_hz <= 0.0) {
        /* Host-driven: exactly one emulator frame per pump. */
        b->core.run_frame(b->core.handle);
        return 1;
    }

    {
        uint64_t period = (uint64_t)(1e9 / b->cfg.core_hz);
        uint32_t cap = b->cfg.max_catchup_frames ? b->cfg.max_catchup_frames : 1u;

        if (!period)
            period = 1u;
        if (!b->timing_primed) {
            b->next_core_frame_ns = now_ns;
            b->timing_primed = 1;
        }
        while (now_ns >= b->next_core_frame_ns && ran < cap) {
            b->core.run_frame(b->core.handle);
            b->next_core_frame_ns += period;
            ++ran;
        }
        if (ran == cap && now_ns >= b->next_core_frame_ns) {
            /* The host fell far behind. Resync rather than accumulate debt,
             * which would make the emulator sprint to catch up. */
            b->next_core_frame_ns = now_ns + period;
        }
    }
    return ran;
}

/* ------------------------------------------------------------------ */

static int ensure_buffers(VfxBridge *b, uint32_t w, uint32_t h)
{
    size_t need = (size_t)w * h * 4u;
    int e;

    if (b->eye_w == w && b->eye_h == h && b->eye_src[0])
        return 1;

    for (e = 0; e < VFX_EYE_COUNT; ++e) {
        uint8_t *p = (uint8_t *)realloc(b->eye_src[e], need);
        if (!p) { b->err = "out of memory allocating eye buffers"; return 0; }
        memset(p, 0, need);
        b->eye_src[e] = p;

        if (b->cfg.reprojection_enabled) {
            uint8_t *q = (uint8_t *)realloc(b->eye_out[e], need);
            if (!q) { b->err = "out of memory allocating reprojection buffers"; return 0; }
            memset(q, 0, need);
            b->eye_out[e] = q;
        }
    }
    b->eye_bytes = need;
    b->eye_w = w;
    b->eye_h = h;
    b->lineseq_have[0] = b->lineseq_have[1] = 0;
    return 1;
}

/* Best guess at the orientation a frame was rendered against when the core
 * does not report one: the pose current roughly one emulator frame before the
 * frame surfaced. */
static VfxQuat estimate_render_pose(const VfxBridge *b, uint64_t frame_ns)
{
    uint64_t period_ns;

    if (b->cfg.core_hz > 0.0)
        period_ns = (uint64_t)(1e9 / b->cfg.core_hz);
    else if (b->prev_frame_ns && frame_ns > b->prev_frame_ns)
        period_ns = frame_ns - b->prev_frame_ns;
    else
        period_ns = 33333333ull; /* 30 Hz, mid-range for the era */

    if (frame_ns <= period_ns)
        return b->hist_count ? pose_at(b, 0) : vfx_quat_identity();
    return pose_at(b, frame_ns - period_ns);
}

int vfx_bridge_acquire_eyes(VfxBridge *b, uint64_t now_ns, VfxEyeFrames *out)
{
    VfxFrame f;
    uint32_t w = 0, h = 0;
    int fresh, e;
    VfxStereoFormat fmt;

    if (!b || !out)
        return 0;
    memset(out, 0, sizeof(*out));

    f = b->core.get_frame(b->core.handle);
    fmt = b->cfg.format_override_enabled ? b->cfg.format_override : f.format;

    if (!f.width || !f.height ||
        (!f.combined_rgba && !(f.left_rgba && f.right_rgba))) {
        if (!b->have_frame) {
            b->err = "core has produced no frame yet";
            return 0;
        }
        f.frame_id = b->last_frame_id; /* keep serving the last good pair */
        fresh = 0;
    } else {
        fresh = !b->have_frame || f.frame_id != b->last_frame_id;
    }

    if (fresh) {
        if (!vfx_stereo_eye_size(&f, &b->cfg, &w, &h)) {
            b->err = "frame has no usable dimensions";
            return 0;
        }
        if (!ensure_buffers(b, w, h))
            return 0;

        if (fmt == VFX_STEREO_LINE_SEQUENTIAL &&
            b->cfg.line_sequential_mode == VFX_LINESEQ_FRAME) {
            /* This capture is one eye only; the other keeps its previous
             * content until its own turn comes round. */
            int eye = vfx_stereo_frame_mode_eye(f.frame_id,
                                                b->cfg.lineseq_left_is_first);
            const uint8_t *srcbuf = f.combined_rgba
                                  ? f.combined_rgba
                                  : (eye == VFX_EYE_LEFT ? f.left_rgba : f.right_rgba);
            if (!srcbuf) {
                b->err = "line-sequential frame has no pixels";
                return 0;
            }
            memcpy(b->eye_src[eye], srcbuf, b->eye_bytes);
            if (!b->lineseq_have[eye ^ 1]) {
                /* First half of the very first pair: duplicate it so the user
                 * is not shown one black eye. */
                memcpy(b->eye_src[eye ^ 1], srcbuf, b->eye_bytes);
            }
            b->lineseq_have[eye] = 1;
        } else {
            if (!vfx_stereo_extract(&f, &b->cfg,
                                    b->eye_src[0], b->eye_src[1], &w, &h)) {
                b->err = "stereo extraction failed";
                return 0;
            }
            b->lineseq_have[0] = b->lineseq_have[1] = 1;
        }

        b->prev_frame_ns  = b->last_frame_ns;
        b->last_frame_ns  = now_ns;
        b->last_frame_id  = f.frame_id;
        b->frame_format   = fmt;
        b->have_frame     = 1;
        b->frame_render_q = f.render_pose_valid
                          ? vfx_quat_normalize(f.render_pose.orientation)
                          : estimate_render_pose(b, now_ns);
    }

    w = b->eye_w;
    h = b->eye_h;

    out->width               = w;
    out->height              = h;
    out->source_frame_id     = b->last_frame_id;
    out->source_timestamp_ns = b->last_frame_ns;
    out->is_stale            = !fresh;
    out->source_format       = b->frame_format;

    if (b->cfg.reprojection_enabled && b->have_pose) {
        VfxQuat cur   = b->head.orientation;
        VfxQuat delta = vfx_quat_mul(vfx_quat_conjugate(b->frame_render_q), cur);
        float cw = fabsf(delta.w);
        float angle;
        if (cw > 1.0f) cw = 1.0f;
        angle = 2.0f * acosf(cw);

        if (angle <= b->cfg.max_reprojection_rad) {
            for (e = 0; e < VFX_EYE_COUNT; ++e)
                out->coverage[e] = vfx_reproject_orientation(
                    b->eye_src[e], w, h, b->eye_out[e], w, h,
                    b->frame_render_q, cur,
                    b->cfg.source_fov_h_rad, b->cfg.source_fov_v_rad,
                    b->cfg.reprojection_bilinear);
            out->rgba[0] = b->eye_out[0];
            out->rgba[1] = b->eye_out[1];
            out->reprojected = 1;
            out->reprojection_angle_rad = angle;
            b->err = "ok";
            return 1;
        }
        /* Too large a delta to re-warp honestly: show the real frame. */
        out->reprojection_angle_rad = angle;
    }

    out->rgba[0] = b->eye_src[0];
    out->rgba[1] = b->eye_src[1];
    out->coverage[0] = out->coverage[1] = 1.0f;
    b->err = "ok";
    return 1;
}
