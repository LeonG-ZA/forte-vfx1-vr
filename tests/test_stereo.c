/* Stereo extraction
 *
 * Every pixel is stamped with an eye tag and its own coordinates, so a wrong
 * split shows up as a mismatched tag rather than as a plausible-looking image.
 */
#include "vfxtest.h"
#include "vfx/vfxbridge.h"
#include <string.h>
#include <stdlib.h>

#define W 8u
#define H 6u

/* R = eye tag (10 left, 200 right), G = x, B = y. */
static void stamp(uint8_t *px, uint8_t tag, uint32_t x, uint32_t y)
{
    px[0] = tag;
    px[1] = (uint8_t)x;
    px[2] = (uint8_t)y;
    px[3] = 255u;
}

static int matches(const uint8_t *buf, uint8_t tag, uint32_t w, uint32_t h)
{
    uint32_t x, y;
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            const uint8_t *p = buf + ((size_t)y * w + x) * 4u;
            if (p[0] != tag || p[1] != (uint8_t)x || p[2] != (uint8_t)y)
                return 0;
        }
    return 1;
}

int main(void)
{
    VfxBridgeConfig cfg;
    uint8_t *left  = (uint8_t *)malloc(W * H * 4u);
    uint8_t *right = (uint8_t *)malloc(W * H * 4u);
    uint32_t ow = 0, oh = 0, x, y;

    vfx_bridge_config_defaults(&cfg);
    CHECK(left && right);
    if (!left || !right)
        return 1;

    /* ---- side by side: 2W x H, left half then right half ---- */
    {
        uint8_t comb[2u * W * H * 4u];
        VfxFrame f;
        memset(&f, 0, sizeof(f));
        for (y = 0; y < H; ++y)
            for (x = 0; x < W; ++x) {
                stamp(comb + ((size_t)y * (2u * W) + x) * 4u, 10u, x, y);
                stamp(comb + ((size_t)y * (2u * W) + W + x) * 4u, 200u, x, y);
            }
        f.format = VFX_STEREO_SIDE_BY_SIDE;
        f.width = W; f.height = H;
        f.combined_rgba = comb;
        f.combined_width = 2u * W; f.combined_height = H;

        CHECK(vfx_stereo_extract(&f, &cfg, left, right, &ow, &oh) == 1);
        CHECK(ow == W && oh == H);
        CHECK(matches(left, 10u, W, H));
        CHECK(matches(right, 200u, W, H));
    }

    /* ---- line sequential, field mode: W x 2H, even rows left ---- */
    {
        uint8_t comb[W * 2u * H * 4u];
        VfxFrame f;
        memset(&f, 0, sizeof(f));
        for (y = 0; y < H; ++y)
            for (x = 0; x < W; ++x) {
                stamp(comb + ((size_t)(y * 2u) * W + x) * 4u, 10u, x, y);
                stamp(comb + ((size_t)(y * 2u + 1u) * W + x) * 4u, 200u, x, y);
            }
        f.format = VFX_STEREO_LINE_SEQUENTIAL;
        f.width = W; f.height = H;
        f.combined_rgba = comb;
        f.combined_width = W; f.combined_height = 2u * H;

        cfg.line_sequential_mode = VFX_LINESEQ_FIELD;
        cfg.lineseq_left_is_first = 1;
        CHECK(vfx_stereo_extract(&f, &cfg, left, right, &ow, &oh) == 1);
        CHECK(matches(left, 10u, W, H));
        CHECK(matches(right, 200u, W, H));

        /* Open question Q3b: nobody has confirmed which field is the left
         * eye, so flipping the config must genuinely swap the eyes. */
        cfg.lineseq_left_is_first = 0;
        CHECK(vfx_stereo_extract(&f, &cfg, left, right, &ow, &oh) == 1);
        CHECK(matches(left, 200u, W, H));
        CHECK(matches(right, 10u, W, H));
        cfg.lineseq_left_is_first = 1;
    }

    /* ---- VFX_STEREO_NONE: mono duplicated so menus stay usable ---- */
    {
        uint8_t comb[W * H * 4u];
        VfxFrame f;
        memset(&f, 0, sizeof(f));
        for (y = 0; y < H; ++y)
            for (x = 0; x < W; ++x)
                stamp(comb + ((size_t)y * W + x) * 4u, 77u, x, y);
        f.format = VFX_STEREO_NONE;
        f.width = W; f.height = H;
        f.combined_rgba = comb;
        f.combined_width = W; f.combined_height = H;

        CHECK(vfx_stereo_extract(&f, &cfg, left, right, &ow, &oh) == 1);
        CHECK(matches(left, 77u, W, H));
        CHECK(matches(right, 77u, W, H));
        CHECK(memcmp(left, right, W * H * 4u) == 0);
    }

    /* ---- a core that pre-splits wins over any packing guess ---- */
    {
        uint8_t l[W * H * 4u], r[W * H * 4u];
        VfxFrame f;
        memset(&f, 0, sizeof(f));
        for (y = 0; y < H; ++y)
            for (x = 0; x < W; ++x) {
                stamp(l + ((size_t)y * W + x) * 4u, 33u, x, y);
                stamp(r + ((size_t)y * W + x) * 4u, 99u, x, y);
            }
        f.format = VFX_STEREO_SIDE_BY_SIDE;
        f.width = W; f.height = H;
        f.left_rgba = l; f.right_rgba = r;

        CHECK(vfx_stereo_extract(&f, &cfg, left, right, &ow, &oh) == 1);
        CHECK(matches(left, 33u, W, H));
        CHECK(matches(right, 99u, W, H));
    }

    /* ---- format override beats what the core claims ---- */
    {
        uint8_t comb[2u * W * H * 4u];
        VfxFrame f;
        memset(&f, 0, sizeof(f));
        for (y = 0; y < H; ++y)
            for (x = 0; x < W; ++x) {
                stamp(comb + ((size_t)y * (2u * W) + x) * 4u, 10u, x, y);
                stamp(comb + ((size_t)y * (2u * W) + W + x) * 4u, 200u, x, y);
            }
        f.format = VFX_STEREO_NONE;      /* core got it wrong */
        f.width = W; f.height = H;
        f.combined_rgba = comb;
        f.combined_width = 2u * W; f.combined_height = H;

        cfg.format_override_enabled = 1;
        cfg.format_override = VFX_STEREO_SIDE_BY_SIDE;
        CHECK(vfx_stereo_extract(&f, &cfg, left, right, &ow, &oh) == 1);
        CHECK(matches(left, 10u, W, H));
        CHECK(matches(right, 200u, W, H));
        cfg.format_override_enabled = 0;
    }

    /* ---- refuse a combined buffer too small for the claimed layout ---- */
    {
        uint8_t comb[W * H * 4u];
        VfxFrame f;
        memset(comb, 0, sizeof(comb));
        memset(&f, 0, sizeof(f));
        f.format = VFX_STEREO_SIDE_BY_SIDE;
        f.width = W; f.height = H;
        f.combined_rgba = comb;
        f.combined_width = W;   /* should be 2W */
        f.combined_height = H;
        CHECK(vfx_stereo_extract(&f, &cfg, left, right, &ow, &oh) == 0);
    }

    /* ---- bad arguments ---- */
    {
        VfxFrame f;
        memset(&f, 0, sizeof(f));
        CHECK(vfx_stereo_extract(NULL, &cfg, left, right, &ow, &oh) == 0);
        CHECK(vfx_stereo_extract(&f, &cfg, NULL, right, &ow, &oh) == 0);
        CHECK(vfx_stereo_extract(&f, &cfg, left, right, &ow, &oh) == 0);
    }

    free(left);
    free(right);
    TEST_MAIN_END();
}
