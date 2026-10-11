/*
 * stereo.c -- stereo extraction.
 *
 * Turns whatever the core captured into two equally sized eye buffers:
 *   SIDE_BY_SIDE      -- left half / right half of the combined frame
 *   LINE_SEQUENTIAL   -- either even/odd scanlines (FIELD) or alternating
 *                        whole frames (FRAME); see docs/FINDINGS.md Q3
 *   NONE              -- the game is not in VFX1 mode (menus, boot), so the
 *                        mono frame is duplicated to both eyes rather than
 *                        breaking the experience
 * If the core already hands over split left_rgba/right_rgba, those win.
 */
#include "vfx/vfxbridge.h"
#include "stereo_internal.h"
#include <string.h>

static VfxStereoFormat effective_format(const VfxFrame *f,
                                        const VfxBridgeConfig *cfg)
{
    if (cfg && cfg->format_override_enabled)
        return cfg->format_override;
    return f->format;
}

/* Dimensions of the combined buffer, falling back to a per-format guess when
 * the core did not fill in the explicit fields. */
static void combined_size(const VfxFrame *f, VfxStereoFormat fmt,
                          const VfxBridgeConfig *cfg,
                          uint32_t *cw, uint32_t *ch)
{
    if (f->combined_width && f->combined_height) {
        *cw = f->combined_width;
        *ch = f->combined_height;
        return;
    }
    switch (fmt) {
    case VFX_STEREO_SIDE_BY_SIDE:
        *cw = f->width * 2u; *ch = f->height;
        break;
    case VFX_STEREO_LINE_SEQUENTIAL:
        if (cfg && cfg->line_sequential_mode == VFX_LINESEQ_FRAME) {
            *cw = f->width; *ch = f->height;
        } else {
            *cw = f->width; *ch = f->height * 2u;
        }
        break;
    default:
        *cw = f->width; *ch = f->height;
        break;
    }
}

int vfx_stereo_eye_size(const VfxFrame *frame, const VfxBridgeConfig *cfg,
                        uint32_t *out_w, uint32_t *out_h)
{
    (void)cfg; /* per-eye size is carried by the frame itself today; the
                * parameter stays for formats that may need a hint later. */
    if (!frame || !out_w || !out_h)
        return 0;
    if (!frame->width || !frame->height)
        return 0;
    *out_w = frame->width;
    *out_h = frame->height;
    return 1;
}

static void copy_rows(uint8_t *dst, const uint8_t *src, uint32_t w, uint32_t h,
                      size_t src_stride, uint32_t src_row0, uint32_t src_row_step)
{
    uint32_t y;
    for (y = 0; y < h; ++y)
        memcpy(dst + (size_t)y * w * 4u,
               src + (size_t)(src_row0 + y * src_row_step) * src_stride,
               (size_t)w * 4u);
}

int vfx_stereo_extract(const VfxFrame *frame, const VfxBridgeConfig *cfg,
                       uint8_t *dst_left, uint8_t *dst_right,
                       uint32_t *out_w, uint32_t *out_h)
{
    VfxStereoFormat fmt;
    uint32_t w, h, cw, ch;

    if (!frame || !dst_left || !dst_right)
        return 0;
    if (!vfx_stereo_eye_size(frame, cfg, &w, &h))
        return 0;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;

    fmt = effective_format(frame, cfg);

    /* A core that already did the split saves us the work. */
    if (frame->left_rgba && frame->right_rgba) {
        memcpy(dst_left,  frame->left_rgba,  (size_t)w * h * 4u);
        memcpy(dst_right, frame->right_rgba, (size_t)w * h * 4u);
        return 1;
    }
    if (!frame->combined_rgba)
        return 0;

    combined_size(frame, fmt, cfg, &cw, &ch);

    switch (fmt) {
    case VFX_STEREO_SIDE_BY_SIDE: {
        uint32_t y;
        size_t stride = (size_t)cw * 4u;
        if (cw < w * 2u || ch < h)
            return 0;
        for (y = 0; y < h; ++y) {
            const uint8_t *row = frame->combined_rgba + (size_t)y * stride;
            memcpy(dst_left  + (size_t)y * w * 4u, row,                    (size_t)w * 4u);
            memcpy(dst_right + (size_t)y * w * 4u, row + (size_t)w * 4u,   (size_t)w * 4u);
        }
        return 1;
    }

    case VFX_STEREO_LINE_SEQUENTIAL: {
        int left_first = cfg ? cfg->lineseq_left_is_first : 1;

        if (cfg && cfg->line_sequential_mode == VFX_LINESEQ_FRAME) {
            /* One whole captured frame belongs to one eye. The caller (the
             * bridge) is responsible for holding the other eye's previous
             * frame; on its own this function can only fill the eye it has,
             * so it duplicates into both and reports success. The bridge
             * overwrites the stale eye afterwards. */
            size_t bytes = (size_t)w * h * 4u;
            if (cw < w || ch < h)
                return 0;
            memcpy(dst_left,  frame->combined_rgba, bytes);
            memcpy(dst_right, frame->combined_rgba, bytes);
            return 1;
        }

        /* FIELD mode: even scanlines to one eye, odd to the other. */
        if (cw < w || ch < h * 2u)
            return 0;
        {
            size_t stride = (size_t)cw * 4u;
            uint8_t *a = left_first ? dst_left  : dst_right;
            uint8_t *b = left_first ? dst_right : dst_left;
            copy_rows(a, frame->combined_rgba, w, h, stride, 0u, 2u);
            copy_rows(b, frame->combined_rgba, w, h, stride, 1u, 2u);
        }
        return 1;
    }

    case VFX_STEREO_NONE:
    default: {
        /* Not in VFX1 mode: show the same mono image to both eyes so menus and
         * boot screens stay usable instead of going black. */
        uint32_t y;
        size_t stride = (size_t)cw * 4u;
        if (cw < w || ch < h)
            return 0;
        for (y = 0; y < h; ++y) {
            const uint8_t *row = frame->combined_rgba + (size_t)y * stride;
            memcpy(dst_left  + (size_t)y * w * 4u, row, (size_t)w * 4u);
            memcpy(dst_right + (size_t)y * w * 4u, row, (size_t)w * 4u);
        }
        return 1;
    }
    }
}

/* Which eye a FRAME-mode line-sequential capture belongs to. */
int vfx_stereo_frame_mode_eye(uint64_t frame_id, int left_is_first)
{
    int even = (frame_id % 2u) == 0u;
    if (left_is_first)
        return even ? (int)VFX_EYE_LEFT : (int)VFX_EYE_RIGHT;
    return even ? (int)VFX_EYE_RIGHT : (int)VFX_EYE_LEFT;
}
