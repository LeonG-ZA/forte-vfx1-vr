#include "vfxmath.h"
#include <math.h>

VfxQuat vfx_quat_identity(void)
{
    VfxQuat q = { 1.0f, 0.0f, 0.0f, 0.0f };
    return q;
}

VfxQuat vfx_quat_normalize(VfxQuat q)
{
    float n = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (n < 1e-12f)
        return vfx_quat_identity();
    n = 1.0f / n;
    q.w *= n; q.x *= n; q.y *= n; q.z *= n;
    return q;
}

VfxQuat vfx_quat_conjugate(VfxQuat q)
{
    VfxQuat r = { q.w, -q.x, -q.y, -q.z };
    return r;
}

VfxQuat vfx_quat_mul(VfxQuat a, VfxQuat b)
{
    VfxQuat r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

VfxVec3 vfx_quat_rotate(VfxQuat q, VfxVec3 v)
{
    /* v + 2w(q_v x v) + 2 q_v x (q_v x v) */
    float tx = 2.0f * (q.y * v.z - q.z * v.y);
    float ty = 2.0f * (q.z * v.x - q.x * v.z);
    float tz = 2.0f * (q.x * v.y - q.y * v.x);
    VfxVec3 r;
    r.x = v.x + q.w * tx + (q.y * tz - q.z * ty);
    r.y = v.y + q.w * ty + (q.z * tx - q.x * tz);
    r.z = v.z + q.w * tz + (q.x * ty - q.y * tx);
    return r;
}

float vfx_quat_dot(VfxQuat a, VfxQuat b)
{
    return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
}

VfxQuat vfx_quat_slerp(VfxQuat a, VfxQuat b, float t)
{
    float d = vfx_quat_dot(a, b);
    if (d < 0.0f) { /* take the shortest arc */
        b.w = -b.w; b.x = -b.x; b.y = -b.y; b.z = -b.z;
        d = -d;
    }
    if (d > 0.9995f) { /* nearly parallel: lerp, which stays stable */
        VfxQuat r;
        r.w = a.w + t * (b.w - a.w);
        r.x = a.x + t * (b.x - a.x);
        r.y = a.y + t * (b.y - a.y);
        r.z = a.z + t * (b.z - a.z);
        return vfx_quat_normalize(r);
    }
    {
        float theta = acosf(d);
        float s = sinf(theta);
        float wa = sinf((1.0f - t) * theta) / s;
        float wb = sinf(t * theta) / s;
        VfxQuat r;
        r.w = wa * a.w + wb * b.w;
        r.x = wa * a.x + wb * b.x;
        r.y = wa * a.y + wb * b.y;
        r.z = wa * a.z + wb * b.z;
        return vfx_quat_normalize(r);
    }
}

VfxQuat vfx_quat_from_yaw_pitch_roll(float yaw, float pitch, float roll)
{
    float cy = cosf(yaw * 0.5f),   sy = sinf(yaw * 0.5f);
    float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
    float cr = cosf(roll * 0.5f),  sr = sinf(roll * 0.5f);
    VfxQuat qy = { cy, 0.0f, sy, 0.0f };
    VfxQuat qp = { cp, sp, 0.0f, 0.0f };
    VfxQuat qr = { cr, 0.0f, 0.0f, sr };
    return vfx_quat_mul(vfx_quat_mul(qy, qp), qr);
}

void vfx_quat_to_yaw_pitch_roll(VfxQuat q, float *yaw, float *pitch, float *roll)
{
    /* Inverse of R = Ry(yaw) * Rx(pitch) * Rz(roll), read off that product:
     *   sin(pitch) = -R[1][2],  yaw = atan2(R[0][2], R[2][2]),
     *   roll = atan2(R[1][0], R[1][1]). */
    float sp, xx, yy, zz;
    q = vfx_quat_normalize(q);
    xx = q.x * q.x; yy = q.y * q.y; zz = q.z * q.z;

    sp = 2.0f * (q.w * q.x - q.y * q.z);
    if (sp >  1.0f) sp =  1.0f;
    if (sp < -1.0f) sp = -1.0f;
    if (pitch) *pitch = asinf(sp);

    if (sp > 0.99999f || sp < -0.99999f) {
        /* Gimbal lock: looking straight up or down makes yaw and roll
         * degenerate. Fold the whole rotation into yaw and zero the roll. */
        if (yaw)  *yaw  = atan2f(2.0f * (q.w * q.y - q.x * q.z),
                                 1.0f - 2.0f * (yy + zz));
        if (roll) *roll = 0.0f;
    } else {
        if (yaw)  *yaw  = atan2f(2.0f * (q.x * q.z + q.w * q.y),
                                 1.0f - 2.0f * (xx + yy));
        if (roll) *roll = atan2f(2.0f * (q.x * q.y + q.w * q.z),
                                 1.0f - 2.0f * (xx + zz));
    }
}

VfxVec3 vfx_vec3_normalize(VfxVec3 v)
{
    float n = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    if (n < 1e-12f) { VfxVec3 z = { 0.0f, 0.0f, 0.0f }; return z; }
    n = 1.0f / n;
    v.x *= n; v.y *= n; v.z *= n;
    return v;
}
