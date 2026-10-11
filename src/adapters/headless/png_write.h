/* png_write.h -- minimal RGBA PNG writer, adapter-side only.
 *
 * Lives in the headless adapter, not the bridge: the bridge deals in raw
 * buffers and must not grow file-format opinions. Returns 1 on success. */
#ifndef VFX_PNG_WRITE_H
#define VFX_PNG_WRITE_H

#include <stdint.h>

int vfx_png_write_rgba(const char *path, const uint8_t *rgba,
                       uint32_t w, uint32_t h);

#endif /* VFX_PNG_WRITE_H */
