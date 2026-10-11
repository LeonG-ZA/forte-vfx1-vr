/* Quaternion helpers. The VFX1 sensor reported yaw/pitch/roll, so the
 * conversion in both directions has to be exact or every downstream angle is
 * quietly wrong. */
#include "vfxtest.h"
#include "../src/bridge/vfxmath.h"

static void check_ypr_roundtrip(float yaw, float pitch, float roll)
{
    VfxQuat q = vfx_quat_from_yaw_pitch_roll(yaw, pitch, roll);
    float y2, p2, r2;
    vfx_quat_to_yaw_pitch_roll(q, &y2, &p2, &r2);
    CHECK_NEAR(y2, yaw, 1e-4);
    CHECK_NEAR(p2, pitch, 1e-4);
    CHECK_NEAR(r2, roll, 1e-4);
}

int main(void)
{
    const float deg = 3.14159265f / 180.0f;

    /* Identity behaves. */
    {
        VfxQuat i = vfx_quat_identity();
        VfxVec3 v = { 1.0f, 2.0f, 3.0f };
        VfxVec3 r = vfx_quat_rotate(i, v);
        CHECK_NEAR(r.x, 1.0, 1e-6);
        CHECK_NEAR(r.y, 2.0, 1e-6);
        CHECK_NEAR(r.z, 3.0, 1e-6);
    }

    /* Yaw is a rotation about +Y. With +Z toward the viewer, a 90 degree yaw
     * takes the forward axis -Z onto -X. */
    {
        VfxQuat q = vfx_quat_from_yaw_pitch_roll(90.0f * deg, 0.0f, 0.0f);
        VfxVec3 fwd = { 0.0f, 0.0f, -1.0f };
        VfxVec3 r = vfx_quat_rotate(q, fwd);
        CHECK_NEAR(r.x, -1.0, 1e-5);
        CHECK_NEAR(r.y,  0.0, 1e-5);
        CHECK_NEAR(r.z,  0.0, 1e-5);
    }

    /* Pitch is a rotation about +X: looking up tilts forward toward +Y. */
    {
        VfxQuat q = vfx_quat_from_yaw_pitch_roll(0.0f, 30.0f * deg, 0.0f);
        VfxVec3 fwd = { 0.0f, 0.0f, -1.0f };
        VfxVec3 r = vfx_quat_rotate(q, fwd);
        CHECK_NEAR(r.y, sin(30.0 * 3.14159265 / 180.0), 1e-5);
        CHECK(r.z < 0.0f);
    }

    /* Round-trips across the ranges the real hardware reported:
     * yaw 360 deg, pitch +-70 deg, roll +-70 deg. */
    check_ypr_roundtrip(0.0f, 0.0f, 0.0f);
    check_ypr_roundtrip(30.0f * deg, 20.0f * deg, -15.0f * deg);
    check_ypr_roundtrip(-170.0f * deg, -69.0f * deg, 69.0f * deg);
    check_ypr_roundtrip(179.0f * deg, 5.0f * deg, -60.0f * deg);
    check_ypr_roundtrip(-45.0f * deg, 70.0f * deg, 0.0f);

    /* conj(q) * q is the identity. */
    {
        VfxQuat q = vfx_quat_from_yaw_pitch_roll(1.1f, -0.4f, 0.7f);
        VfxQuat e = vfx_quat_mul(vfx_quat_conjugate(q), q);
        CHECK_NEAR(fabs(e.w), 1.0, 1e-5);
        CHECK_NEAR(e.x, 0.0, 1e-5);
        CHECK_NEAR(e.y, 0.0, 1e-5);
        CHECK_NEAR(e.z, 0.0, 1e-5);
    }

    /* Composition order: rotating by a then b equals rotating by mul(b, a). */
    {
        VfxQuat a = vfx_quat_from_yaw_pitch_roll(0.3f, 0.0f, 0.0f);
        VfxQuat b = vfx_quat_from_yaw_pitch_roll(0.0f, 0.2f, 0.0f);
        VfxVec3 v = { 0.3f, -0.5f, -1.0f };
        VfxVec3 step = vfx_quat_rotate(b, vfx_quat_rotate(a, v));
        VfxVec3 once = vfx_quat_rotate(vfx_quat_mul(b, a), v);
        CHECK_NEAR(step.x, once.x, 1e-5);
        CHECK_NEAR(step.y, once.y, 1e-5);
        CHECK_NEAR(step.z, once.z, 1e-5);
    }

    /* Slerp endpoints and midpoint. */
    {
        VfxQuat a = vfx_quat_from_yaw_pitch_roll(0.0f, 0.0f, 0.0f);
        VfxQuat b = vfx_quat_from_yaw_pitch_roll(1.0f, 0.0f, 0.0f);
        VfxQuat m = vfx_quat_slerp(a, b, 0.5f);
        float yaw;
        vfx_quat_to_yaw_pitch_roll(vfx_quat_slerp(a, b, 0.0f), &yaw, NULL, NULL);
        CHECK_NEAR(yaw, 0.0, 1e-5);
        vfx_quat_to_yaw_pitch_roll(vfx_quat_slerp(a, b, 1.0f), &yaw, NULL, NULL);
        CHECK_NEAR(yaw, 1.0, 1e-5);
        vfx_quat_to_yaw_pitch_roll(m, &yaw, NULL, NULL);
        CHECK_NEAR(yaw, 0.5, 1e-5);
    }

    /* Shortest arc: slerping to the negated (identical) rotation must not
     * take the long way round. */
    {
        VfxQuat a = vfx_quat_from_yaw_pitch_roll(0.2f, 0.0f, 0.0f);
        VfxQuat b = vfx_quat_from_yaw_pitch_roll(0.4f, 0.0f, 0.0f);
        VfxQuat bneg;
        float yaw;
        bneg.w = -b.w; bneg.x = -b.x; bneg.y = -b.y; bneg.z = -b.z;
        vfx_quat_to_yaw_pitch_roll(vfx_quat_slerp(a, bneg, 0.5f), &yaw, NULL, NULL);
        CHECK_NEAR(yaw, 0.3, 1e-4);
    }

    /* Degenerate input must not produce NaNs. */
    {
        VfxQuat z = { 0.0f, 0.0f, 0.0f, 0.0f };
        VfxQuat n = vfx_quat_normalize(z);
        CHECK_NEAR(n.w, 1.0, 1e-6);
    }

    TEST_MAIN_END();
}
