/*
 * stub_core.c -- synthetic emulator core. See stub_core.h.
 *
 * The scene is deliberately simple but genuinely three-dimensional: a checker
 * ground plane, a sky gradient, and a ring of coloured pillars with a distinct
 * marker due north. That is enough to make three things visible at a glance:
 * yaw and pitch actually move the view, the two eyes really do differ by
 * parallax, and a reprojected frame lines up with a freshly rendered one.
 */
#include "stub_core.h"
#include "../bridge/vfxmath.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PILLAR_COUNT 8

typedef struct {
    VfxStubCoreConfig cfg;

    VfxPose            pose;       /* latest submitted */
    VfxControllerState puck;
    VfxPose            latched;    /* pose in force for the frame being built */

    uint8_t *eye[2];      /* per-eye render targets */
    uint8_t *combined;    /* packed to cfg.format */
    uint32_t comb_w, comb_h;

    uint64_t frame_id;
    uint64_t frames_run;
    int      have_frame;
} StubCore;

static const uint8_t pillar_rgb[PILLAR_COUNT][3] = {
    { 235,  64,  52 }, { 235, 156,  52 }, { 214, 219,  60 }, {  92, 214,  76 },
    {  60, 214, 197 }, {  66, 133, 244 }, { 145,  70, 235 }, { 235,  70, 175 }
};

void vfx_stub_core_config_defaults(VfxStubCoreConfig *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    /* The real VFX1 LCDs were 263x230 per eye. */
    cfg->eye_width  = 263u;
    cfg->eye_height = 230u;
    cfg->format     = VFX_STEREO_SIDE_BY_SIDE;
    cfg->line_sequential_by_frame = 0;
    cfg->lineseq_left_is_first    = 1;
    cfg->provide_split      = 0;
    cfg->ipd_meters         = 0.064f;
    cfg->fov_h_rad          = 0.7853981634f; /* 45 deg */
    cfg->fov_v_rad          = 0.6108652382f; /* 35 deg */
    cfg->report_render_pose = 1;
}

/* ---------------------------- rendering ---------------------------- */

static void shade_sky(VfxVec3 d, uint8_t *px)
{
    float t = d.y * 0.5f + 0.5f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    px[0] = (uint8_t)(20.0f  + t * 40.0f);
    px[1] = (uint8_t)(30.0f  + t * 90.0f);
    px[2] = (uint8_t)(60.0f  + t * 150.0f);
    px[3] = 255u;
}

static void shade_ground(float wx, float wz, float dist, uint8_t *px)
{
    int cx = (int)floorf(wx);
    int cz = (int)floorf(wz);
    int checker = ((cx + cz) & 1);
    float fade = 1.0f / (1.0f + dist * 0.035f);
    float base = checker ? 190.0f : 70.0f;
    float v = base * fade + 25.0f * (1.0f - fade);
    px[0] = (uint8_t)(v * 0.85f);
    px[1] = (uint8_t)(v * 0.95f);
    px[2] = (uint8_t)(v * 0.80f);
    px[3] = 255u;
}

/* Nearest hit against an infinite vertical cylinder clipped to [y0, y1]. */
static int hit_pillar(VfxVec3 o, VfxVec3 d, float cx, float cz, float r,
                      float y0, float y1, float *t_out)
{
    float ex = o.x - cx, ez = o.z - cz;
    float a = d.x * d.x + d.z * d.z;
    float b = 2.0f * (d.x * ex + d.z * ez);
    float c = ex * ex + ez * ez - r * r;
    float disc, sq, t;

    if (a < 1e-9f)
        return 0;
    disc = b * b - 4.0f * a * c;
    if (disc < 0.0f)
        return 0;
    sq = sqrtf(disc);
    t = (-b - sq) / (2.0f * a);
    if (t < 1e-3f)
        t = (-b + sq) / (2.0f * a);
    if (t < 1e-3f)
        return 0;
    {
        float y = o.y + t * d.y;
        if (y < y0 || y > y1)
            return 0;
    }
    *t_out = t;
    return 1;
}

static void render_eye(const StubCore *s, VfxQuat orient, VfxVec3 eye_pos,
                       uint8_t *dst)
{
    const uint32_t w = s->cfg.eye_width, h = s->cfg.eye_height;
    const float tan_x = tanf(s->cfg.fov_h_rad * 0.5f);
    const float tan_y = tanf(s->cfg.fov_v_rad * 0.5f);
    const float ground_y = 0.0f;
    uint32_t x, y;
    int i;
    float px_c[PILLAR_COUNT], pz_c[PILLAR_COUNT];

    for (i = 0; i < PILLAR_COUNT; ++i) {
        float ang = (float)i * 6.2831853f / (float)PILLAR_COUNT;
        px_c[i] = sinf(ang) * 6.0f;
        pz_c[i] = -cosf(ang) * 6.0f; /* i == 0 sits due north, at -Z */
    }

    for (y = 0; y < h; ++y) {
        float ndc_y = 1.0f - 2.0f * ((float)y + 0.5f) / (float)h;
        for (x = 0; x < w; ++x) {
            float ndc_x = 2.0f * ((float)x + 0.5f) / (float)w - 1.0f;
            uint8_t *px = dst + ((size_t)y * w + (size_t)x) * 4u;
            VfxVec3 ray, d;
            float best_t = 1e30f;
            int best_pillar = -1;

            ray.x = ndc_x * tan_x;
            ray.y = ndc_y * tan_y;
            ray.z = -1.0f;
            d = vfx_vec3_normalize(vfx_quat_rotate(orient, ray));

            for (i = 0; i < PILLAR_COUNT; ++i) {
                float t;
                float radius = (i == 0) ? 0.45f : 0.30f;
                float top    = (i == 0) ? 4.0f  : 2.6f;
                if (hit_pillar(eye_pos, d, px_c[i], pz_c[i], radius,
                               ground_y, top, &t) && t < best_t) {
                    best_t = t;
                    best_pillar = i;
                }
            }

            if (d.y < -1e-6f) {
                float t = (ground_y - eye_pos.y) / d.y;
                if (t > 1e-3f && t < best_t) {
                    shade_ground(eye_pos.x + d.x * t, eye_pos.z + d.z * t, t, px);
                    continue;
                }
            }

            if (best_pillar >= 0) {
                const uint8_t *c = pillar_rgb[best_pillar];
                float fade = 1.0f / (1.0f + best_t * 0.06f);
                px[0] = (uint8_t)((float)c[0] * fade);
                px[1] = (uint8_t)((float)c[1] * fade);
                px[2] = (uint8_t)((float)c[2] * fade);
                px[3] = 255u;
                continue;
            }

            shade_sky(d, px);
        }
    }
}

/* ---------------------------- packing ------------------------------ */

static void pack_frame(StubCore *s)
{
    const uint32_t w = s->cfg.eye_width, h = s->cfg.eye_height;
    const size_t row = (size_t)w * 4u;
    uint32_t y;

    switch (s->cfg.format) {
    case VFX_STEREO_SIDE_BY_SIDE:
        for (y = 0; y < h; ++y) {
            uint8_t *o = s->combined + (size_t)y * (size_t)s->comb_w * 4u;
            memcpy(o,       s->eye[0] + (size_t)y * row, row);
            memcpy(o + row, s->eye[1] + (size_t)y * row, row);
        }
        break;

    case VFX_STEREO_LINE_SEQUENTIAL:
        if (s->cfg.line_sequential_by_frame) {
            /* Whole frames alternate; emit only this frame's eye. */
            int even = (s->frame_id % 2u) == 0u;
            int eye = s->cfg.lineseq_left_is_first ? (even ? 0 : 1)
                                                   : (even ? 1 : 0);
            memcpy(s->combined, s->eye[eye], (size_t)w * h * 4u);
        } else {
            int first = s->cfg.lineseq_left_is_first ? 0 : 1;
            for (y = 0; y < h; ++y) {
                uint8_t *o = s->combined + (size_t)(y * 2u) * row;
                memcpy(o,       s->eye[first]     + (size_t)y * row, row);
                memcpy(o + row, s->eye[first ^ 1] + (size_t)y * row, row);
            }
        }
        break;

    case VFX_STEREO_NONE:
    default:
        /* Not in VFX1 mode: one mono image, which is what a DOS menu screen
         * would actually be putting on the feature connector. */
        memcpy(s->combined, s->eye[0], (size_t)w * h * 4u);
        break;
    }
}

static void combined_dims(const VfxStubCoreConfig *cfg, uint32_t *cw, uint32_t *ch)
{
    switch (cfg->format) {
    case VFX_STEREO_SIDE_BY_SIDE:
        *cw = cfg->eye_width * 2u; *ch = cfg->eye_height;
        break;
    case VFX_STEREO_LINE_SEQUENTIAL:
        if (cfg->line_sequential_by_frame) {
            *cw = cfg->eye_width; *ch = cfg->eye_height;
        } else {
            *cw = cfg->eye_width; *ch = cfg->eye_height * 2u;
        }
        break;
    default:
        *cw = cfg->eye_width; *ch = cfg->eye_height;
        break;
    }
}

/* ---------------------------- VfxCoreApi ---------------------------- */

static void stub_submit_pose(void *handle, const VfxPose *head,
                             const VfxControllerState *puck)
{
    StubCore *s = (StubCore *)handle;
    if (!s || !head)
        return;
    /* Cache only. The DOS side reads this whenever it next polls, exactly as
     * the real core will latch it into the emulated VIP registers. */
    s->pose = *head;
    s->pose.orientation = vfx_quat_normalize(s->pose.orientation);
    if (puck)
        s->puck = *puck;
}

static void stub_run_frame(void *handle)
{
    StubCore *s = (StubCore *)handle;
    VfxQuat q;
    VfxVec3 right, base, l, r;
    float half;

    if (!s)
        return;

    /* Latch at the start of the frame, which is what gives render_pose its
     * meaning for the bridge reprojection layer. */
    s->latched = s->pose;
    q = vfx_quat_normalize(s->latched.orientation);

    base.x = s->latched.position.x;
    base.y = 1.6f + s->latched.position.y; /* standing eye height */
    base.z = s->latched.position.z;

    { VfxVec3 rx = { 1.0f, 0.0f, 0.0f }; right = vfx_quat_rotate(q, rx); }
    half = s->cfg.ipd_meters * 0.5f;

    l.x = base.x - right.x * half;
    l.y = base.y - right.y * half;
    l.z = base.z - right.z * half;
    r.x = base.x + right.x * half;
    r.y = base.y + right.y * half;
    r.z = base.z + right.z * half;

    render_eye(s, q, l, s->eye[0]);
    render_eye(s, q, r, s->eye[1]);

    pack_frame(s);

    ++s->frame_id;
    ++s->frames_run;
    s->have_frame = 1;
}

static VfxFrame stub_get_frame(void *handle)
{
    StubCore *s = (StubCore *)handle;
    VfxFrame f;

    memset(&f, 0, sizeof(f));
    if (!s || !s->have_frame)
        return f;

    f.format          = s->cfg.format;
    f.width           = s->cfg.eye_width;
    f.height          = s->cfg.eye_height;
    f.combined_rgba   = s->combined;
    f.combined_width  = s->comb_w;
    f.combined_height = s->comb_h;
    f.frame_id        = s->frame_id;

    if (s->cfg.provide_split) {
        f.left_rgba  = s->eye[0];
        f.right_rgba = s->eye[1];
    }
    if (s->cfg.report_render_pose) {
        f.render_pose       = s->latched;
        f.render_pose_valid = 1u;
    }
    return f;
}

static void stub_destroy(void *handle)
{
    StubCore *s = (StubCore *)handle;
    if (!s)
        return;
    free(s->eye[0]);
    free(s->eye[1]);
    free(s->combined);
    free(s);
}

int vfx_stub_core_create(const VfxStubCoreConfig *cfg, VfxCoreApi *out)
{
    StubCore *s;
    VfxStubCoreConfig c;

    if (!out)
        return 0;
    if (cfg)
        c = *cfg;
    else
        vfx_stub_core_config_defaults(&c);
    if (!c.eye_width || !c.eye_height)
        return 0;

    s = (StubCore *)calloc(1, sizeof(*s));
    if (!s)
        return 0;
    s->cfg = c;
    s->pose.orientation    = vfx_quat_identity();
    s->latched.orientation = vfx_quat_identity();

    combined_dims(&c, &s->comb_w, &s->comb_h);
    s->eye[0]   = (uint8_t *)calloc((size_t)c.eye_width * c.eye_height, 4u);
    s->eye[1]   = (uint8_t *)calloc((size_t)c.eye_width * c.eye_height, 4u);
    s->combined = (uint8_t *)calloc((size_t)s->comb_w * s->comb_h, 4u);
    if (!s->eye[0] || !s->eye[1] || !s->combined) {
        stub_destroy(s);
        return 0;
    }

    memset(out, 0, sizeof(*out));
    out->api_version = VFX_CORE_API_VERSION;
    out->handle      = s;
    out->get_frame   = stub_get_frame;
    out->submit_pose = stub_submit_pose;
    out->destroy     = stub_destroy;
    out->run_frame   = stub_run_frame;
    return 1;
}

VfxPose vfx_stub_core_last_pose(void *handle)
{
    StubCore *s = (StubCore *)handle;
    if (s)
        return s->pose;
    { VfxPose p; memset(&p, 0, sizeof(p)); p.orientation = vfx_quat_identity(); return p; }
}

VfxControllerState vfx_stub_core_last_controller(void *handle)
{
    StubCore *s = (StubCore *)handle;
    if (s)
        return s->puck;
    { VfxControllerState c; memset(&c, 0, sizeof(c)); return c; }
}

uint64_t vfx_stub_core_frame_count(void *handle)
{
    StubCore *s = (StubCore *)handle;
    return s ? s->frames_run : 0u;
}
