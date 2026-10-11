/* Orientation reprojection
 *
 * The interesting property is not "it produces pixels" but "it produces them
 * in the analytically correct place". A source feature at image centre must
 * land at ndc_x = tan(yaw)/tan(fov_h/2) after a yaw of that size, which is
 * checked here against the measured brightest column.
 */
#include "vfxtest.h"
#include "vfx/vfxbridge.h"
#include "../src/bridge/vfxmath.h"
#include <string.h>
#include <stdlib.h>

#define SW 256u
#define SH 192u

static const float FOV_H = 0.7853981634f; /* 45 deg */
static const float FOV_V = 0.6108652382f; /* 35 deg */

/* Dark field with one bright column and one bright row through the centre. */
static void make_crosshair(uint8_t *buf, uint32_t w, uint32_t h)
{
    uint32_t x, y;
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            uint8_t *p = buf + ((size_t)y * w + x) * 4u;
            int on_col = (x == w / 2u);
            int on_row = (y == h / 2u);
            uint8_t v = (uint8_t)((on_col || on_row) ? 255u : 20u);
            p[0] = p[1] = p[2] = v;
            p[3] = 255u;
        }
}

static uint32_t brightest_column(const uint8_t *buf, uint32_t w, uint32_t h)
{
    uint32_t x, y, best = 0;
    double best_sum = -1.0;
    for (x = 0; x < w; ++x) {
        double s = 0.0;
        for (y = 0; y < h; ++y)
            s += buf[((size_t)y * w + x) * 4u];
        if (s > best_sum) { best_sum = s; best = x; }
    }
    return best;
}

static uint32_t brightest_row(const uint8_t *buf, uint32_t w, uint32_t h)
{
    uint32_t x, y, best = 0;
    double best_sum = -1.0;
    for (y = 0; y < h; ++y) {
        double s = 0.0;
        for (x = 0; x < w; ++x)
            s += buf[((size_t)y * w + x) * 4u];
        if (s > best_sum) { best_sum = s; best = y; }
    }
    return best;
}

int main(void)
{
    uint8_t *src = (uint8_t *)malloc(SW * SH * 4u);
    uint8_t *dst = (uint8_t *)malloc(SW * SH * 4u);
    VfxQuat id = vfx_quat_identity();

    CHECK(src && dst);
    if (!src || !dst)
        return 1;
    make_crosshair(src, SW, SH);

    /* Identity delta reproduces the source and covers every pixel. */
    {
        float cov = vfx_reproject_orientation(src, SW, SH, dst, SW, SH,
                                              id, id, FOV_H, FOV_V, 1);
        CHECK_NEAR(cov, 1.0, 1e-6);
        CHECK(memcmp(src, dst, SW * SH * 4u) == 0);
    }

    /* Nearest sampling must agree with bilinear on an identity delta. */
    {
        vfx_reproject_orientation(src, SW, SH, dst, SW, SH,
                                  id, id, FOV_H, FOV_V, 0);
        CHECK(memcmp(src, dst, SW * SH * 4u) == 0);
    }

    /* Yaw: the head turns left, so world content slides right by exactly
     * tan(yaw)/tan(fov_h/2) in normalised device coordinates. */
    {
        const double yaw = 5.0 * 3.14159265358979 / 180.0;
        VfxQuat cur = vfx_quat_from_yaw_pitch_roll((float)yaw, 0.0f, 0.0f);
        double u_d = tan(yaw) / tan((double)FOV_H * 0.5);
        double expect_x = (u_d + 1.0) * 0.5 * (double)SW - 0.5;
        uint32_t got;
        float cov = vfx_reproject_orientation(src, SW, SH, dst, SW, SH,
                                              id, cur, FOV_H, FOV_V, 0);
        got = brightest_column(dst, SW, SH);
        CHECK_NEAR((double)got, expect_x, 1.5);
        CHECK(got > SW / 2u);          /* moved right, not left */
        CHECK(cov > 0.7f && cov < 1.0f); /* some of the view is now off-source */
    }

    /* The mirrored yaw must move it the same distance the other way. */
    {
        const double yaw = -5.0 * 3.14159265358979 / 180.0;
        VfxQuat cur = vfx_quat_from_yaw_pitch_roll((float)yaw, 0.0f, 0.0f);
        double u_d = tan(yaw) / tan((double)FOV_H * 0.5);
        double expect_x = (u_d + 1.0) * 0.5 * (double)SW - 0.5;
        uint32_t got;
        vfx_reproject_orientation(src, SW, SH, dst, SW, SH,
                                  id, cur, FOV_H, FOV_V, 0);
        got = brightest_column(dst, SW, SH);
        CHECK_NEAR((double)got, expect_x, 1.5);
        CHECK(got < SW / 2u);
    }

    /* Pitch: looking up pushes the world down the image, so the source centre
     * row lands at ndc_y = -tan(pitch)/tan(fov_v/2), below centre. */
    {
        const double pitch = 4.0 * 3.14159265358979 / 180.0;
        VfxQuat cur = vfx_quat_from_yaw_pitch_roll(0.0f, (float)pitch, 0.0f);
        double v_d = -tan(pitch) / tan((double)FOV_V * 0.5);
        double expect_y = (1.0 - v_d) * 0.5 * (double)SH - 0.5;
        uint32_t got;
        vfx_reproject_orientation(src, SW, SH, dst, SW, SH,
                                  id, cur, FOV_H, FOV_V, 0);
        got = brightest_row(dst, SW, SH);
        CHECK_NEAR((double)got, expect_y, 1.5);
        CHECK(got > SH / 2u);          /* below centre: the head looked up */
    }

    /* Looking down is the mirror of that. */
    {
        const double pitch = -4.0 * 3.14159265358979 / 180.0;
        VfxQuat cur = vfx_quat_from_yaw_pitch_roll(0.0f, (float)pitch, 0.0f);
        double v_d = -tan(pitch) / tan((double)FOV_V * 0.5);
        double expect_y = (1.0 - v_d) * 0.5 * (double)SH - 0.5;
        uint32_t got;
        vfx_reproject_orientation(src, SW, SH, dst, SW, SH,
                                  id, cur, FOV_H, FOV_V, 0);
        got = brightest_row(dst, SW, SH);
        CHECK_NEAR((double)got, expect_y, 1.5);
        CHECK(got < SH / 2u);
    }

    /* Reprojecting the render pose itself is always the identity, whatever
     * that pose happens to be. */
    {
        VfxQuat q = vfx_quat_from_yaw_pitch_roll(1.2f, -0.5f, 0.3f);
        float cov = vfx_reproject_orientation(src, SW, SH, dst, SW, SH,
                                              q, q, FOV_H, FOV_V, 1);
        CHECK_NEAR(cov, 1.0, 1e-6);
        CHECK(memcmp(src, dst, SW * SH * 4u) == 0);
    }

    /* Coverage falls monotonically as the head moves further from the frame. */
    {
        float prev = 1.1f;
        int i;
        for (i = 1; i <= 5; ++i) {
            VfxQuat cur = vfx_quat_from_yaw_pitch_roll((float)i * 0.08f, 0.0f, 0.0f);
            float cov = vfx_reproject_orientation(src, SW, SH, dst, SW, SH,
                                                  id, cur, FOV_H, FOV_V, 1);
            CHECK(cov <= prev + 1e-4f);
            prev = cov;
        }
        CHECK(prev < 0.95f);
    }

    /* Looking clean away from the frame leaves nothing to sample, and the
     * result must be black rather than a smeared edge. */
    {
        VfxQuat cur = vfx_quat_from_yaw_pitch_roll(3.14159265f, 0.0f, 0.0f);
        float cov = vfx_reproject_orientation(src, SW, SH, dst, SW, SH,
                                              id, cur, FOV_H, FOV_V, 1);
        uint32_t i, nonblack = 0;
        CHECK_NEAR(cov, 0.0, 1e-6);
        for (i = 0; i < SW * SH; ++i)
            if (dst[i * 4u] != 0u)
                ++nonblack;
        CHECK(nonblack == 0u);
    }

    /* Bad arguments are refused rather than crashing. */
    {
        CHECK_NEAR(vfx_reproject_orientation(NULL, SW, SH, dst, SW, SH,
                                             id, id, FOV_H, FOV_V, 1), 0.0, 1e-9);
        CHECK_NEAR(vfx_reproject_orientation(src, 0u, SH, dst, SW, SH,
                                             id, id, FOV_H, FOV_V, 1), 0.0, 1e-9);
    }

    free(src);
    free(dst);
    TEST_MAIN_END();
}
