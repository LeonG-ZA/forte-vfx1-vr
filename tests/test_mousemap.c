/*
 * Head pose to INT 33h relative mouse motion.
 *
 * The properties that matter are conservation and continuity: a head that
 * turns N radians and stops must have emitted exactly N radians' worth of
 * counts, and no single step across the yaw wrap may emit a large one.
 */
#include "vfxtest.h"
#include "vfx/vfxmouse.h"
#include "../src/bridge/vfxmath.h"
#include <string.h>

static VfxPose pose_ypr(float yaw, float pitch, float roll)
{
    VfxPose p;
    memset(&p, 0, sizeof(p));
    p.orientation = vfx_quat_from_yaw_pitch_roll(yaw, pitch, roll);
    return p;
}

/* Feed a yaw sweep in n steps and return the total emitted X counts. */
static long sweep_yaw(VfxMouseMap *m, float from, float to, int n, long *sum_y)
{
    long sx = 0, sy = 0;
    int i;
    for (i = 0; i <= n; ++i) {
        float a = from + (to - from) * ((float)i / (float)n);
        VfxPose p = pose_ypr(a, 0.0f, 0.0f);
        VfxMouseDelta d = vfx_mousemap_update(m, &p);
        sx += d.dx;
        sy += d.dy;
    }
    if (sum_y)
        *sum_y = sy;
    return sx;
}

int main(void)
{
    VfxMouseMapConfig cfg;
    VfxMouseMap m;

    vfx_mousemap_config_defaults(&cfg);

    /* The first pose only establishes an origin; it must not jerk the view. */
    {
        VfxPose p = pose_ypr(1.0f, 0.5f, 0.0f);
        VfxMouseDelta d;
        vfx_mousemap_init(&m, &cfg);
        d = vfx_mousemap_update(&m, &p);
        CHECK(d.dx == 0 && d.dy == 0);
        /* A repeat of the same pose is still no motion. */
        d = vfx_mousemap_update(&m, &p);
        CHECK(d.dx == 0 && d.dy == 0);
    }

    /* Conservation: a 1.0 rad turn emits counts_per_radian counts, whatever
     * step size it was delivered in. */
    {
        long y = 0, total_fine, total_coarse;
        vfx_mousemap_init(&m, &cfg);
        total_fine = sweep_yaw(&m, 0.0f, 1.0f, 500, &y);
        CHECK_NEAR((double)total_fine, -600.0, 2.0);
        CHECK(y == 0);           /* a pure yaw sweep must not move Y at all */

        vfx_mousemap_init(&m, &cfg);
        total_coarse = sweep_yaw(&m, 0.0f, 1.0f, 5, &y);
        CHECK_NEAR((double)total_coarse, -600.0, 2.0);
    }

    /* Sub-count accumulation: motion far slower than one count per update must
     * still add up, not truncate to nothing forever. */
    {
        long total = 0;
        int i;
        vfx_mousemap_init(&m, &cfg);
        for (i = 0; i <= 6000; ++i) {
            /* 0.1 rad over 6000 updates is 0.01 counts per update. */
            VfxPose p = pose_ypr(0.1f * ((float)i / 6000.0f), 0.0f, 0.0f);
            VfxMouseDelta d = vfx_mousemap_update(&m, &p);
            total += d.dx;
        }
        CHECK_NEAR((double)total, -60.0, 2.0);
        CHECK(total != 0);
    }

    /* Direction: head left is view left, which is negative mouse X. */
    {
        long y;
        vfx_mousemap_init(&m, &cfg);
        CHECK(sweep_yaw(&m, 0.0f, 0.5f, 50, &y) < 0);
        vfx_mousemap_init(&m, &cfg);
        CHECK(sweep_yaw(&m, 0.0f, -0.5f, 50, &y) > 0);
    }

    /* Inversion flips it and nothing else. */
    {
        long y, normal, inverted;
        vfx_mousemap_init(&m, &cfg);
        normal = sweep_yaw(&m, 0.0f, 0.5f, 50, &y);
        cfg.invert_yaw = 1;
        vfx_mousemap_init(&m, &cfg);
        inverted = sweep_yaw(&m, 0.0f, 0.5f, 50, &y);
        CHECK(normal == -inverted);
        cfg.invert_yaw = 0;
    }

    /* Yaw wraparound: walking the head all the way round past the +-pi seam
     * must never emit a single huge step. */
    {
        int i;
        int32_t worst = 0;
        long total = 0;
        vfx_mousemap_init(&m, &cfg);
        for (i = 0; i <= 720; ++i) {
            /* A full turn in half-degree steps, straight through the seam. */
            VfxPose p = pose_ypr((float)i * 0.0087266463f, 0.0f, 0.0f);
            VfxMouseDelta d = vfx_mousemap_update(&m, &p);
            if (d.dx > worst)  worst = d.dx;
            if (-d.dx > worst) worst = -d.dx;
            total += d.dx;
        }
        /* Half a degree is 0.00873 rad, about 5.2 counts. Nothing should be
         * anywhere near the 200-count clamp, let alone a full-turn jump. */
        CHECK(worst < 20);
        CHECK(m.clamped == 0u);
        /* 720 half-degrees is one full turn plus none: 360 deg = 2pi rad. */
        CHECK_NEAR((double)total, -600.0 * 6.2831853, 20.0);
    }

    /* Pitch clamps at the hardware limit, and coming back down does not have
     * to unwind travel that was never emitted. */
    {
        VfxPose p;
        VfxMouseDelta d;
        long up = 0, down = 0;
        int i;
        vfx_mousemap_init(&m, &cfg);
        p = pose_ypr(0.0f, 0.0f, 0.0f);
        vfx_mousemap_update(&m, &p);

        for (i = 1; i <= 200; ++i) {         /* look up to 100 degrees */
            p = pose_ypr(0.0f, (float)i * 0.0087266463f, 0.0f);
            d = vfx_mousemap_update(&m, &p);
            up += d.dy;
        }
        /* Only the first 70 degrees can have counted. */
        CHECK_NEAR((double)up, -600.0 * 1.2217305, 3.0);
        {
            float ty, tp;
            vfx_mousemap_tracked(&m, &ty, &tp);
            CHECK_NEAR(tp, 1.2217305, 1e-4);
        }

        for (i = 199; i >= 0; --i) {         /* and back to level */
            p = pose_ypr(0.0f, (float)i * 0.0087266463f, 0.0f);
            d = vfx_mousemap_update(&m, &p);
            down += d.dy;
        }
        CHECK_NEAR((double)(up + down), 0.0, 3.0); /* returns to where it began */
    }

    /* Disabling an axis, as VRMOUSE's -XX and -XY did. */
    {
        long y;
        cfg.enable_yaw = 0;
        vfx_mousemap_init(&m, &cfg);
        CHECK(sweep_yaw(&m, 0.0f, 1.0f, 100, &y) == 0);
        cfg.enable_yaw = 1;

        cfg.enable_pitch = 0;
        vfx_mousemap_init(&m, &cfg);
        {
            int i;
            long sy = 0;
            for (i = 0; i <= 100; ++i) {
                VfxPose p = pose_ypr(0.0f, (float)i * 0.005f, 0.0f);
                sy += vfx_mousemap_update(&m, &p).dy;
            }
            CHECK(sy == 0);
        }
        cfg.enable_pitch = 1;
    }

    /* Roll is ignored: a mouse has two axes. */
    {
        VfxPose p;
        VfxMouseDelta d;
        vfx_mousemap_init(&m, &cfg);
        p = pose_ypr(0.2f, 0.1f, 0.0f);
        vfx_mousemap_update(&m, &p);
        p = pose_ypr(0.2f, 0.1f, 0.9f);
        d = vfx_mousemap_update(&m, &p);
        CHECK(d.dx == 0 && d.dy == 0);
    }

    /* A tracking glitch is swallowed, not deferred: the clamp discards the
     * excess and says so. */
    {
        VfxPose p;
        VfxMouseDelta d;
        cfg.max_counts_per_update = 50;
        vfx_mousemap_init(&m, &cfg);
        p = pose_ypr(0.0f, 0.0f, 0.0f);
        vfx_mousemap_update(&m, &p);
        p = pose_ypr(1.5f, 0.0f, 0.0f);       /* 86 degrees in one update */
        d = vfx_mousemap_update(&m, &p);
        CHECK(d.dx == -50);
        CHECK(m.clamped == 1u);
        /* And the discarded part does not arrive later. */
        d = vfx_mousemap_update(&m, &p);
        CHECK(d.dx == 0);
        cfg.max_counts_per_update = 200;
    }

    /* Recentre emits nothing for the pose it re-zeros on, then tracks from
     * there. */
    {
        VfxPose p;
        VfxMouseDelta d;
        vfx_mousemap_init(&m, &cfg);
        p = pose_ypr(0.0f, 0.0f, 0.0f);
        vfx_mousemap_update(&m, &p);
        p = pose_ypr(0.5f, 0.0f, 0.0f);
        vfx_mousemap_update(&m, &p);

        vfx_mousemap_recenter(&m);
        d = vfx_mousemap_update(&m, &p);
        CHECK(d.dx == 0 && d.dy == 0);
        p = pose_ypr(0.6f, 0.0f, 0.0f);
        d = vfx_mousemap_update(&m, &p);
        CHECK(d.dx < 0);                       /* tracks from the new origin */
        CHECK_NEAR((double)d.dx, -60.0, 2.0);
    }

    /* Null arguments are refused rather than crashing. */
    {
        VfxPose p = pose_ypr(0.0f, 0.0f, 0.0f);
        VfxMouseDelta d = vfx_mousemap_update(NULL, &p);
        CHECK(d.dx == 0 && d.dy == 0);
        d = vfx_mousemap_update(&m, NULL);
        CHECK(d.dx == 0 && d.dy == 0);
        vfx_mousemap_init(NULL, &cfg);
        vfx_mousemap_recenter(NULL);
        vfx_mousemap_tracked(NULL, NULL, NULL);
    }

    TEST_MAIN_END();
}
