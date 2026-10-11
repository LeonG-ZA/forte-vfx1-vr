/*
 * mousemap.c -- head pose to relative mouse motion. See vfxmouse.h.
 *
 * Four things here are load-bearing, and each of them is a bug if omitted:
 *
 *   1. Yaw differences are wrapped into (-pi, pi]. Without it, a head that
 *      turns past due south emits a delta of nearly two pi and the view spins.
 *   2. Fractional counts accumulate. Without it, any head motion slower than
 *      one count per update truncates to zero and the view never moves at all.
 *   3. Tracked pitch is clamped before the difference is taken, not after, so
 *      looking past the limit banks no phantom travel to unwind on the way
 *      back down.
 *   4. Excess beyond max_counts_per_update is discarded rather than banked,
 *      because the point of the clamp is to swallow a glitch, not defer it.
 */
#include "vfx/vfxmouse.h"
#include "vfxmath.h"

#include <math.h>
#include <string.h>

#define VFX_PI  3.14159265358979323846f
#define VFX_TAU 6.28318530717958647692f

void vfx_mousemap_config_defaults(VfxMouseMapConfig *cfg)
{
    if (!cfg)
        return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->enable_yaw   = 1;
    cfg->enable_pitch = 1;
    cfg->invert_yaw   = 0;
    cfg->invert_pitch = 0;
    /* No correct value exists; this one makes a 90 degree head turn about 940
     * counts, which is a brisk but not absurd mouse sweep. Tune per title. */
    cfg->counts_per_radian_yaw   = 600.0f;
    cfg->counts_per_radian_pitch = 600.0f;
    cfg->pitch_limit_rad         = 1.2217304764f; /* 70 deg, as the hardware */
    cfg->max_counts_per_update   = 200;
}

void vfx_mousemap_init(VfxMouseMap *m, const VfxMouseMapConfig *cfg)
{
    if (!m)
        return;
    memset(m, 0, sizeof(*m));
    if (cfg)
        m->cfg = *cfg;
    else
        vfx_mousemap_config_defaults(&m->cfg);
}

void vfx_mousemap_recenter(VfxMouseMap *m)
{
    if (!m)
        return;
    m->primed  = 0;
    m->accum_x = 0.0f;
    m->accum_y = 0.0f;
}

/* Shortest signed angular difference, in (-pi, pi]. */
static float wrap_pi(float a)
{
    while (a > VFX_PI)   a -= VFX_TAU;
    while (a <= -VFX_PI) a += VFX_TAU;
    return a;
}

static int32_t take_whole(float *accum, int32_t limit, uint64_t *clamped)
{
    float t = truncf(*accum);      /* toward zero, so signs stay symmetric */
    int32_t out = (int32_t)t;
    *accum -= t;
    if (limit > 0) {
        if (out >  limit) { out =  limit; ++*clamped; }
        if (out < -limit) { out = -limit; ++*clamped; }
    }
    return out;
}

VfxMouseDelta vfx_mousemap_update(VfxMouseMap *m, const VfxPose *head)
{
    VfxMouseDelta out;
    float yaw = 0.0f, pitch = 0.0f;
    float dyaw, dpitch;

    out.dx = 0;
    out.dy = 0;
    if (!m || !head)
        return out;

    vfx_quat_to_yaw_pitch_roll(head->orientation, &yaw, &pitch, NULL);
    /* Roll is dropped on purpose: a mouse has two axes, and VRMOUSE exposed
     * two. Titles that wanted roll used the native VIP path. */

    if (m->cfg.pitch_limit_rad > 0.0f) {
        if (pitch >  m->cfg.pitch_limit_rad) pitch =  m->cfg.pitch_limit_rad;
        if (pitch < -m->cfg.pitch_limit_rad) pitch = -m->cfg.pitch_limit_rad;
    }

    if (!m->primed) {
        m->ref_yaw   = yaw;
        m->ref_pitch = pitch;
        m->primed    = 1;
        return out;                 /* first pose only establishes the origin */
    }

    dyaw   = wrap_pi(yaw - m->ref_yaw);
    dpitch = pitch - m->ref_pitch;  /* asin output, so it cannot wrap */

    m->ref_yaw   = yaw;
    m->ref_pitch = pitch;

    if (!m->cfg.enable_yaw)   dyaw   = 0.0f;
    if (!m->cfg.enable_pitch) dpitch = 0.0f;

    /* Negated because turning the head left must move the view left, and a
     * mouse moves a view left by travelling in negative X. Same for pitch,
     * where screen Y grows downward. */
    m->accum_x += -dyaw   * m->cfg.counts_per_radian_yaw
                * (m->cfg.invert_yaw   ? -1.0f : 1.0f);
    m->accum_y += -dpitch * m->cfg.counts_per_radian_pitch
                * (m->cfg.invert_pitch ? -1.0f : 1.0f);

    out.dx = take_whole(&m->accum_x, m->cfg.max_counts_per_update, &m->clamped);
    out.dy = take_whole(&m->accum_y, m->cfg.max_counts_per_update, &m->clamped);
    return out;
}

void vfx_mousemap_tracked(const VfxMouseMap *m, float *yaw, float *pitch)
{
    if (!m)
        return;
    if (yaw)   *yaw   = m->ref_yaw;
    if (pitch) *pitch = m->ref_pitch;
}
