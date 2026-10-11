/*
 * End-to-end contract test: stub core -> VfxCoreApi -> libvfxbridge -> eyes.
 *
 * This proves the API contract works before any DOS
 * hardware emulation exists. Everything here runs on a virtual clock, so it is
 * deterministic and has no wall-clock dependency.
 */
#include "vfxtest.h"
#include "vfx/vfxbridge.h"
#include "core_stub/stub_core.h"
#include "../src/bridge/vfxmath.h"
#include <string.h>

static VfxPose pose_at_time(double t, uint64_t now_ns)
{
    VfxPose p;
    memset(&p, 0, sizeof(p));
    p.orientation = vfx_quat_from_yaw_pitch_roll((float)(t * 0.8), 0.0f, 0.0f);
    p.timestamp_ns = now_ns;
    return p;
}

static int buffers_differ(const uint8_t *a, const uint8_t *b, size_t n)
{
    return memcmp(a, b, n) != 0;
}

int main(void)
{
    /* ---- the bridge refuses a core it cannot trust ---- */
    {
        VfxCoreApi core;
        VfxStubCoreConfig ccfg;
        vfx_stub_core_config_defaults(&ccfg);
        ccfg.eye_width = 64u; ccfg.eye_height = 48u;
        CHECK(vfx_stub_core_create(&ccfg, &core) == 1);

        CHECK(vfx_bridge_create(NULL, NULL) == NULL);
        {
            VfxCoreApi bad = core;
            bad.api_version = VFX_CORE_API_VERSION + 1u;
            CHECK(vfx_bridge_create(&bad, NULL) == NULL);
        }
        {
            VfxCoreApi bad = core;
            bad.get_frame = NULL;
            CHECK(vfx_bridge_create(&bad, NULL) == NULL);
        }
        core.destroy(core.handle);
    }

    /* ---- pose really reaches the core, not just the bridge ---- */
    {
        VfxCoreApi core;
        VfxStubCoreConfig ccfg;
        VfxBridge *b;
        VfxPose sent, seen;
        VfxControllerState puck, seen_puck;

        vfx_stub_core_config_defaults(&ccfg);
        ccfg.eye_width = 64u; ccfg.eye_height = 48u;
        CHECK(vfx_stub_core_create(&ccfg, &core) == 1);
        b = vfx_bridge_create(&core, NULL);
        CHECK(b != NULL);

        sent = pose_at_time(0.5, 500000000ull);
        memset(&puck, 0, sizeof(puck));
        puck.tilt_x = 0.25f; puck.tilt_y = -0.5f; puck.buttons = 0x5u;
        vfx_bridge_submit_pose(b, &sent, &puck);

        seen = vfx_stub_core_last_pose(core.handle);
        seen_puck = vfx_stub_core_last_controller(core.handle);
        CHECK_NEAR(seen.orientation.y, sent.orientation.y, 1e-6);
        CHECK(seen.timestamp_ns == sent.timestamp_ns);
        CHECK_NEAR(seen_puck.tilt_x, 0.25, 1e-6);
        CHECK_NEAR(seen_puck.tilt_y, -0.5, 1e-6);
        CHECK(seen_puck.buttons == 0x5u);

        vfx_bridge_destroy(b);
        core.destroy(core.handle);
    }

    /* ---- nothing to serve before the core has run ---- */
    {
        VfxCoreApi core;
        VfxStubCoreConfig ccfg;
        VfxBridge *b;
        VfxEyeFrames ef;

        vfx_stub_core_config_defaults(&ccfg);
        ccfg.eye_width = 64u; ccfg.eye_height = 48u;
        CHECK(vfx_stub_core_create(&ccfg, &core) == 1);
        b = vfx_bridge_create(&core, NULL);
        CHECK(vfx_bridge_acquire_eyes(b, 0u, &ef) == 0);
        CHECK(vfx_bridge_last_error(b) != NULL);
        vfx_bridge_destroy(b);
        core.destroy(core.handle);
    }

    /* ---- timing arbitration: the DOS clock and the HMD clock are separate,
     *      and the emulator advances at its own configured rate ---- */
    {
        VfxCoreApi core;
        VfxStubCoreConfig ccfg;
        VfxBridgeConfig bcfg;
        VfxBridge *b;
        uint64_t i, ran = 0;

        vfx_stub_core_config_defaults(&ccfg);
        ccfg.eye_width = 32u; ccfg.eye_height = 24u;
        CHECK(vfx_stub_core_create(&ccfg, &core) == 1);

        vfx_bridge_config_defaults(&bcfg);
        bcfg.core_hz = 20.0;
        b = vfx_bridge_create(&core, &bcfg);
        CHECK(b != NULL);

        /* One virtual second at 90 Hz should advance a 20 Hz core 20 times. */
        for (i = 0; i < 90u; ++i)
            ran += vfx_bridge_pump(b, (uint64_t)(i * (1000000000ull / 90ull)));
        CHECK(ran == 20u);
        CHECK(vfx_stub_core_frame_count(core.handle) == 20u);

        vfx_bridge_destroy(b);
        core.destroy(core.handle);
    }

    /* ---- a stalled host cannot make the emulator sprint ---- */
    {
        VfxCoreApi core;
        VfxStubCoreConfig ccfg;
        VfxBridgeConfig bcfg;
        VfxBridge *b;
        uint32_t ran;

        vfx_stub_core_config_defaults(&ccfg);
        ccfg.eye_width = 32u; ccfg.eye_height = 24u;
        CHECK(vfx_stub_core_create(&ccfg, &core) == 1);

        vfx_bridge_config_defaults(&bcfg);
        bcfg.core_hz = 30.0;
        bcfg.max_catchup_frames = 3u;
        b = vfx_bridge_create(&core, &bcfg);

        vfx_bridge_pump(b, 0u);
        ran = vfx_bridge_pump(b, 10000000000ull); /* ten seconds later */
        CHECK(ran == 3u);

        vfx_bridge_destroy(b);
        core.destroy(core.handle);
    }

    /* ---- host-driven mode: core_hz == 0 means one frame per pump ---- */
    {
        VfxCoreApi core;
        VfxStubCoreConfig ccfg;
        VfxBridgeConfig bcfg;
        VfxBridge *b;
        int i;

        vfx_stub_core_config_defaults(&ccfg);
        ccfg.eye_width = 32u; ccfg.eye_height = 24u;
        CHECK(vfx_stub_core_create(&ccfg, &core) == 1);
        vfx_bridge_config_defaults(&bcfg);
        bcfg.core_hz = 0.0;
        b = vfx_bridge_create(&core, &bcfg);
        for (i = 0; i < 7; ++i)
            CHECK(vfx_bridge_pump(b, 0u) == 1u);
        CHECK(vfx_stub_core_frame_count(core.handle) == 7u);
        vfx_bridge_destroy(b);
        core.destroy(core.handle);
    }

    /* ---- the full loop: 90 Hz headset over a 20 Hz game ---- */
    {
        VfxCoreApi core;
        VfxStubCoreConfig ccfg;
        VfxBridgeConfig bcfg;
        VfxBridge *b;
        VfxEyeFrames ef;
        uint64_t i, served = 0, fresh = 0, stale = 0, reproj = 0;
        size_t eye_bytes;

        vfx_stub_core_config_defaults(&ccfg);
        ccfg.eye_width = 96u; ccfg.eye_height = 72u;
        ccfg.format = VFX_STEREO_SIDE_BY_SIDE;
        CHECK(vfx_stub_core_create(&ccfg, &core) == 1);
        eye_bytes = (size_t)ccfg.eye_width * ccfg.eye_height * 4u;

        vfx_bridge_config_defaults(&bcfg);
        bcfg.core_hz = 20.0;
        bcfg.source_fov_h_rad = ccfg.fov_h_rad;
        bcfg.source_fov_v_rad = ccfg.fov_v_rad;
        b = vfx_bridge_create(&core, &bcfg);
        CHECK(b != NULL);

        for (i = 0; i < 90u; ++i) {
            double t = (double)i / 90.0;
            uint64_t now = (uint64_t)(t * 1e9);
            VfxPose p = pose_at_time(t, now);
            vfx_bridge_submit_pose(b, &p, NULL);
            vfx_bridge_pump(b, now);
            if (!vfx_bridge_acquire_eyes(b, now, &ef))
                continue;
            ++served;
            if (ef.is_stale) ++stale; else ++fresh;
            if (ef.reprojected) ++reproj;
            CHECK(ef.width == ccfg.eye_width);
            CHECK(ef.height == ccfg.eye_height);
            CHECK(ef.rgba[VFX_EYE_LEFT] != NULL);
            CHECK(ef.rgba[VFX_EYE_RIGHT] != NULL);
            CHECK(ef.source_format == VFX_STEREO_SIDE_BY_SIDE);
        }

        CHECK(served == 90u);
        CHECK(fresh == 20u);            /* one per emulated DOS frame */
        CHECK(stale == 70u);            /* the rest are reprojected holds */
        CHECK(reproj >= 70u);           /* the comfort layer did its job */

        /* Side-by-side really did split into two different views: the stub
         * renders genuine parallax from a 64 mm eye separation. */
        CHECK(buffers_differ(ef.rgba[VFX_EYE_LEFT], ef.rgba[VFX_EYE_RIGHT],
                             eye_bytes));

        vfx_bridge_destroy(b);
        core.destroy(core.handle);
    }

    /* ---- VFX_STEREO_NONE duplicates the mono frame to both eyes ---- */
    {
        VfxCoreApi core;
        VfxStubCoreConfig ccfg;
        VfxBridgeConfig bcfg;
        VfxBridge *b;
        VfxEyeFrames ef;
        VfxPose p;

        vfx_stub_core_config_defaults(&ccfg);
        ccfg.eye_width = 48u; ccfg.eye_height = 36u;
        ccfg.format = VFX_STEREO_NONE;
        CHECK(vfx_stub_core_create(&ccfg, &core) == 1);

        vfx_bridge_config_defaults(&bcfg);
        bcfg.core_hz = 0.0;
        bcfg.reprojection_enabled = 0;
        b = vfx_bridge_create(&core, &bcfg);

        p = pose_at_time(0.0, 0u);
        vfx_bridge_submit_pose(b, &p, NULL);
        vfx_bridge_pump(b, 0u);
        CHECK(vfx_bridge_acquire_eyes(b, 0u, &ef) == 1);
        CHECK(ef.source_format == VFX_STEREO_NONE);
        CHECK(memcmp(ef.rgba[0], ef.rgba[1],
                     (size_t)ef.width * ef.height * 4u) == 0);

        vfx_bridge_destroy(b);
        core.destroy(core.handle);
    }

    /* ---- line-sequential, both candidate interpretations (open question Q3) --- */
    {
        int by_frame;
        for (by_frame = 0; by_frame <= 1; ++by_frame) {
            VfxCoreApi core;
            VfxStubCoreConfig ccfg;
            VfxBridgeConfig bcfg;
            VfxBridge *b;
            VfxEyeFrames ef;
            int i;

            vfx_stub_core_config_defaults(&ccfg);
            ccfg.eye_width = 48u; ccfg.eye_height = 36u;
            ccfg.format = VFX_STEREO_LINE_SEQUENTIAL;
            ccfg.line_sequential_by_frame = by_frame;
            CHECK(vfx_stub_core_create(&ccfg, &core) == 1);

            vfx_bridge_config_defaults(&bcfg);
            bcfg.core_hz = 0.0;
            bcfg.reprojection_enabled = 0;
            bcfg.line_sequential_mode = by_frame ? VFX_LINESEQ_FRAME
                                                 : VFX_LINESEQ_FIELD;
            b = vfx_bridge_create(&core, &bcfg);

            /* Two core frames, so frame-alternating mode has seen both eyes. */
            for (i = 0; i < 2; ++i) {
                VfxPose p = pose_at_time(0.0, (uint64_t)i * 50000000ull);
                vfx_bridge_submit_pose(b, &p, NULL);
                vfx_bridge_pump(b, (uint64_t)i * 50000000ull);
                CHECK(vfx_bridge_acquire_eyes(b, (uint64_t)i * 50000000ull, &ef) == 1);
            }
            CHECK(ef.width == ccfg.eye_width);
            CHECK(ef.height == ccfg.eye_height);
            CHECK(ef.source_format == VFX_STEREO_LINE_SEQUENTIAL);
            CHECK(buffers_differ(ef.rgba[0], ef.rgba[1],
                                 (size_t)ef.width * ef.height * 4u));

            vfx_bridge_destroy(b);
            core.destroy(core.handle);
        }
    }

    /* ---- a core that hides render_pose still works, via the estimate ---- */
    {
        VfxCoreApi core;
        VfxStubCoreConfig ccfg;
        VfxBridgeConfig bcfg;
        VfxBridge *b;
        VfxEyeFrames ef;
        uint64_t i, reproj = 0;

        vfx_stub_core_config_defaults(&ccfg);
        ccfg.eye_width = 48u; ccfg.eye_height = 36u;
        ccfg.report_render_pose = 0;
        CHECK(vfx_stub_core_create(&ccfg, &core) == 1);

        vfx_bridge_config_defaults(&bcfg);
        bcfg.core_hz = 20.0;
        b = vfx_bridge_create(&core, &bcfg);

        for (i = 0; i < 90u; ++i) {
            double t = (double)i / 90.0;
            uint64_t now = (uint64_t)(t * 1e9);
            VfxPose p = pose_at_time(t, now);
            vfx_bridge_submit_pose(b, &p, NULL);
            vfx_bridge_pump(b, now);
            if (vfx_bridge_acquire_eyes(b, now, &ef) && ef.reprojected)
                ++reproj;
        }
        CHECK(reproj > 0u);

        vfx_bridge_destroy(b);
        core.destroy(core.handle);
    }

    /* ---- a delta too large to re-warp honestly is passed through ---- */
    {
        VfxCoreApi core;
        VfxStubCoreConfig ccfg;
        VfxBridgeConfig bcfg;
        VfxBridge *b;
        VfxEyeFrames ef;
        VfxPose p;

        vfx_stub_core_config_defaults(&ccfg);
        ccfg.eye_width = 48u; ccfg.eye_height = 36u;
        CHECK(vfx_stub_core_create(&ccfg, &core) == 1);

        vfx_bridge_config_defaults(&bcfg);
        bcfg.core_hz = 0.0;
        bcfg.max_reprojection_rad = 0.05f;
        b = vfx_bridge_create(&core, &bcfg);

        p = pose_at_time(0.0, 0u);
        vfx_bridge_submit_pose(b, &p, NULL);
        vfx_bridge_pump(b, 0u);
        CHECK(vfx_bridge_acquire_eyes(b, 0u, &ef) == 1);

        /* Snap the head 90 degrees without giving the core a new frame. */
        p.orientation = vfx_quat_from_yaw_pitch_roll(1.5707963f, 0.0f, 0.0f);
        p.timestamp_ns = 11000000ull;
        vfx_bridge_submit_pose(b, &p, NULL);
        CHECK(vfx_bridge_acquire_eyes(b, 11000000ull, &ef) == 1);
        CHECK(ef.reprojected == 0);
        CHECK(ef.reprojection_angle_rad > 1.0f);
        CHECK(ef.is_stale == 1);

        vfx_bridge_destroy(b);
        core.destroy(core.handle);
    }

    /* ---- a core that pre-splits its eyes takes the fast path ---- */
    {
        VfxCoreApi core;
        VfxStubCoreConfig ccfg;
        VfxBridgeConfig bcfg;
        VfxBridge *b;
        VfxEyeFrames ef;
        VfxPose p;

        vfx_stub_core_config_defaults(&ccfg);
        ccfg.eye_width = 48u; ccfg.eye_height = 36u;
        ccfg.provide_split = 1;
        CHECK(vfx_stub_core_create(&ccfg, &core) == 1);

        vfx_bridge_config_defaults(&bcfg);
        bcfg.core_hz = 0.0;
        bcfg.reprojection_enabled = 0;
        b = vfx_bridge_create(&core, &bcfg);

        p = pose_at_time(0.0, 0u);
        vfx_bridge_submit_pose(b, &p, NULL);
        vfx_bridge_pump(b, 0u);
        CHECK(vfx_bridge_acquire_eyes(b, 0u, &ef) == 1);
        CHECK(buffers_differ(ef.rgba[0], ef.rgba[1],
                             (size_t)ef.width * ef.height * 4u));

        vfx_bridge_destroy(b);
        core.destroy(core.handle);
    }

    /* ---- destroying null is a no-op, as adapters will rely on ---- */
    vfx_bridge_destroy(NULL);
    CHECK(vfx_bridge_last_error(NULL) != NULL);

    TEST_MAIN_END();
}
