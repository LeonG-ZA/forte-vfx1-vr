/* vfxmath.h -- quaternion / vector helpers. Internal to libvfxbridge. */
#ifndef VFX_VFXMATH_H
#define VFX_VFXMATH_H

#include "vfx/vfxcore_api.h"

#ifdef __cplusplus
extern "C" {
#endif

VfxQuat vfx_quat_identity(void);
VfxQuat vfx_quat_normalize(VfxQuat q);
VfxQuat vfx_quat_conjugate(VfxQuat q);
/* Hamilton product: applying a then b to a vector is vfx_quat_mul(b, a). */
VfxQuat vfx_quat_mul(VfxQuat a, VfxQuat b);
VfxVec3 vfx_quat_rotate(VfxQuat q, VfxVec3 v);
float   vfx_quat_dot(VfxQuat a, VfxQuat b);
/* Shortest-arc slerp, used for pose extrapolation between DOS frames. */
VfxQuat vfx_quat_slerp(VfxQuat a, VfxQuat b, float t);

/* Intrinsic yaw(Y) -> pitch(X) -> roll(Z) in radians, matching the axis order
 * the VFX1 sensor reported (yaw 360 deg, pitch +-70 deg, roll +-70 deg). */
VfxQuat vfx_quat_from_yaw_pitch_roll(float yaw, float pitch, float roll);
void    vfx_quat_to_yaw_pitch_roll(VfxQuat q, float *yaw, float *pitch, float *roll);

VfxVec3 vfx_vec3_normalize(VfxVec3 v);

#ifdef __cplusplus
}
#endif

#endif /* VFX_VFXMATH_H */
