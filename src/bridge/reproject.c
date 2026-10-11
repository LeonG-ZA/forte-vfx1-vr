/*
 * reproject.c -- orientation-only reprojection.
 *
 * A DOS game hands us a new frame 15-35 times a second; the headset wants one
 * 72-120 times a second. Between real frames we re-warp the last stereo pair
 * so it at least tracks where the head is now.
 *
 * The frame is treated as a pinhole image covering fov_h x fov_v, taken with
 * orientation render_q. For each destination pixel we build the view ray in
 * the *current* camera frame, rotate it into the *render* camera frame by
 * delta = conj(render_q) * current_q, and project it back into the source
 * image. Camera convention matches the header: right-handed, +Y up, camera
 * looks down -Z.
 *
 * This corrects rotation only. Anything parallax-dependent is wrong by
 * construction, which is fine here: real VFX1 hardware was 3DOF, so the
 * original games never rendered parallax from head translation anyway.
 */
#include "vfx/vfxbridge.h"
#include "vfxmath.h"
#include <math.h>
#include <string.h>

static int clampi(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void sample_nearest(const uint8_t *src, uint32_t sw, uint32_t sh,
                           float fx, float fy, uint8_t *out)
{
    int x = (int)floorf(fx + 0.5f);
    int y = (int)floorf(fy + 0.5f);
    x = clampi(x, 0, (int)sw - 1);
    y = clampi(y, 0, (int)sh - 1);
    memcpy(out, src + ((size_t)y * sw + (size_t)x) * 4u, 4u);
}

static void sample_bilinear(const uint8_t *src, uint32_t sw, uint32_t sh,
                            float fx, float fy, uint8_t *out)
{
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float tx = fx - (float)x0, ty = fy - (float)y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    int c;
    x0 = clampi(x0, 0, (int)sw - 1);
    x1 = clampi(x1, 0, (int)sw - 1);
    y0 = clampi(y0, 0, (int)sh - 1);
    y1 = clampi(y1, 0, (int)sh - 1);
    {
        const uint8_t *p00 = src + ((size_t)y0 * sw + (size_t)x0) * 4u;
        const uint8_t *p10 = src + ((size_t)y0 * sw + (size_t)x1) * 4u;
        const uint8_t *p01 = src + ((size_t)y1 * sw + (size_t)x0) * 4u;
        const uint8_t *p11 = src + ((size_t)y1 * sw + (size_t)x1) * 4u;
        for (c = 0; c < 4; ++c) {
            float top = (float)p00[c] + tx * ((float)p10[c] - (float)p00[c]);
            float bot = (float)p01[c] + tx * ((float)p11[c] - (float)p01[c]);
            float v = top + ty * (bot - top);
            if (v < 0.0f) v = 0.0f;
            if (v > 255.0f) v = 255.0f;
            out[c] = (uint8_t)(v + 0.5f);
        }
    }
}

float vfx_reproject_orientation(const uint8_t *src, uint32_t sw, uint32_t sh,
                                uint8_t *dst, uint32_t dw, uint32_t dh,
                                VfxQuat render_q, VfxQuat current_q,
                                float fov_h, float fov_v, int bilinear)
{
    VfxQuat delta;
    float tan_x, tan_y;
    uint32_t x, y;
    uint64_t covered = 0, total;

    if (!src || !dst || !sw || !sh || !dw || !dh)
        return 0.0f;

    total = (uint64_t)dw * dh;

    /* delta takes a ray expressed in the current camera frame and re-expresses
     * it in the frame the source image was rendered from. */
    delta = vfx_quat_normalize(
        vfx_quat_mul(vfx_quat_conjugate(vfx_quat_normalize(render_q)),
                     vfx_quat_normalize(current_q)));

    tan_x = tanf(fov_h * 0.5f);
    tan_y = tanf(fov_v * 0.5f);

    for (y = 0; y < dh; ++y) {
        float ndc_y = 1.0f - 2.0f * ((float)y + 0.5f) / (float)dh;
        for (x = 0; x < dw; ++x) {
            float ndc_x = 2.0f * ((float)x + 0.5f) / (float)dw - 1.0f;
            VfxVec3 ray, r;
            uint8_t *o = dst + ((size_t)y * dw + (size_t)x) * 4u;

            ray.x = ndc_x * tan_x;
            ray.y = ndc_y * tan_y;
            ray.z = -1.0f;               /* camera looks down -Z */
            r = vfx_quat_rotate(delta, ray);

            if (r.z >= -1e-6f) {
                /* Behind the source camera. Nothing sane to sample: leave the
                 * pixel black rather than smearing an edge across the view. */
                o[0] = o[1] = o[2] = 0u;
                o[3] = 255u;
                continue;
            }
            {
                float u = (r.x / -r.z) / tan_x;   /* -1..1 across the source */
                float v = (r.y / -r.z) / tan_y;
                float sx = (u + 1.0f) * 0.5f * (float)sw - 0.5f;
                float sy = (1.0f - v) * 0.5f * (float)sh - 0.5f;
                int inside = (sx >= -0.5f && sx <= (float)sw - 0.5f &&
                              sy >= -0.5f && sy <= (float)sh - 0.5f);
                if (inside)
                    ++covered;
                /* Outside the source we clamp to the edge. Black borders that
                 * swim with head motion read worse than a smeared edge. */
                if (bilinear)
                    sample_bilinear(src, sw, sh, sx, sy, o);
                else
                    sample_nearest(src, sw, sh, sx, sy, o);
            }
        }
    }

    return total ? (float)((double)covered / (double)total) : 0.0f;
}
