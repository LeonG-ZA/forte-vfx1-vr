/* stereo_internal.h -- bridge-private helpers from stereo.c. */
#ifndef VFX_STEREO_INTERNAL_H
#define VFX_STEREO_INTERNAL_H

#include <stdint.h>

/* Which eye a FRAME-mode line-sequential capture belongs to, given its
 * frame_id. See docs/FINDINGS.md, Q3. */
int vfx_stereo_frame_mode_eye(uint64_t frame_id, int left_is_first);

#endif /* VFX_STEREO_INTERNAL_H */
