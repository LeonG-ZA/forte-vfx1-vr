/* test_vfxdepth.c -- the depth network runner on a small network written
 * here, and the fake 3D steps. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vfxtest.h"
#include "vfx/vfxdepth.h"

#define S VFXDEPTH_SIZE

static void put_u32(FILE *f, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}

static void put_layer(FILE *f, uint32_t type, uint32_t in, uint32_t in2, uint32_t out, uint32_t k, uint32_t stride,
                      uint32_t pad, float clamp)
{
    uint32_t v[10] = { type, in, in2, out, k, stride, pad, pad, pad, pad };
    int i;
    for (i = 0; i < 10; i++) put_u32(f, v[i]);
    fwrite(&clamp, sizeof clamp, 1, f);
}

/* input -> pad 1 -> 3x3 conv, stride 2, one output (the patch mean + 0.5)
 * -> resize x2 -> add itself: depth = 2 * (mean + 0.5) */
static int write_net(const char *path)
{
    FILE *f = fopen(path, "wb");
    float w = 1.0f / 27, b = 0.5f;
    int i;
    if (!f) return 0;
    fwrite("VFDN", 1, 4, f);
    put_u32(f, 1); put_u32(f, 5); put_u32(f, 4);
    put_u32(f, S); put_u32(f, S); put_u32(f, 3);
    put_u32(f, S + 2); put_u32(f, S + 2); put_u32(f, 3);
    put_u32(f, S / 2); put_u32(f, S / 2); put_u32(f, 1);
    put_u32(f, S); put_u32(f, S); put_u32(f, 1);
    put_u32(f, S); put_u32(f, S); put_u32(f, 1);
    put_layer(f, 1, 0, 0, 1, 0, 1, 1, 10);
    put_layer(f, 2, 1, 0, 2, 3, 2, 0, 10);
    for (i = 0; i < 27; i++) fwrite(&w, sizeof w, 1, f);
    fwrite(&b, sizeof b, 1, f);
    put_layer(f, 4, 2, 0, 3, 0, 1, 0, 10);
    put_layer(f, 5, 3, 3, 4, 0, 1, 0, 10);
    fclose(f);
    return 1;
}

static void test_net(void)
{
    static uint8_t rgb[S * S * 3];
    static float depth[S * S];
    const char *path = "build/test_vfxdepth.vfd";
    VfxDepthNet *net;
    CHECK(write_net(path));
    CHECK(vfxdepth_load("build/no_such_file.vfd") == NULL);
    net = vfxdepth_load(path);
    CHECK(net != NULL);
    if (!net) return;
    memset(rgb, 255, sizeof rgb);                   /* every channel 1.0 */
    CHECK(vfxdepth_run(net, rgb, depth) == 0);
    CHECK_NEAR(depth[100 * S + 100], 2 * (1.0 + 0.5), 1e-4);   /* inside: the full patch */
    CHECK_NEAR(depth[0], 2 * (4.0 / 9 + 0.5), 1e-4);           /* the corner: 4 of 9 inside */
    vfxdepth_free(net);
    remove(path);

    {
        static uint8_t big[64 * 40 * 3], small[16 * 10 * 3];
        memset(big, 77, sizeof big);
        vfxdepth_resize_rgb(big, 64, 40, small, 16, 10);
        CHECK(small[0] == 77 && small[sizeof small - 1] == 77);
    }
}

static void test_fake3d(void)
{
    enum { W = 64, H = 40 };
    static float depth[S * S];
    static uint8_t frame[W * H * 3], eyes[2 * W * H * 3];
    VfxFake3D f;
    int x, y, i;
    vfxfake3d_init(&f);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++)
            for (i = 0; i < 3; i++) frame[(y * W + x) * 3 + i] = (uint8_t)(x * 4);
    CHECK(vfxfake3d_eyes(&f, frame, W, H, 8, eyes) == -1);   /* no depth yet */

    /* one depth everywhere: no disparity, both eyes are the frame */
    for (i = 0; i < S * S; i++) depth[i] = 2.0f;
    vfxfake3d_add_depth(&f, depth);
    CHECK(vfxfake3d_eyes(&f, frame, W, H, 8, eyes) == 0);
    {
        int same = 1;
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++)
                if (abs(eyes[(y * 2 * W + x) * 3] - frame[(y * W + x) * 3]) > 1 ||
                    abs(eyes[(y * 2 * W + W + x) * 3] - frame[(y * W + x) * 3]) > 1)
                    same = 0;
        CHECK(same);
    }

    /* near on the left, far on the right: on the left the left eye looks
     * further right than the right eye (crossed: in front of the screen) */
    vfxfake3d_reset(&f);
    for (y = 0; y < S; y++)
        for (x = 0; x < S; x++) depth[y * S + x] = x < S / 2 ? 0.5f : 5.0f;
    vfxfake3d_add_depth(&f, depth);
    CHECK(vfxfake3d_eyes(&f, frame, W, H, 64, eyes) == 0);
    y = H / 2;
    CHECK(eyes[(y * 2 * W + 8) * 3] > eyes[(y * 2 * W + W + 8) * 3]);
    CHECK(eyes[(y * 2 * W + 56) * 3] < eyes[(y * 2 * W + W + 56) * 3]);

    /* time smoothing: a sudden new depth moves the nearness only partly */
    {
        float before = f.nearness[S * S / 2 + 10];
        for (i = 0; i < S * S; i++) depth[i] = 5.0f;
        vfxfake3d_add_depth(&f, depth);
        CHECK(f.nearness[S * S / 2 + 10] < before && f.nearness[S * S / 2 + 10] > 0.0f);
    }
    vfxfake3d_free(&f);
}

int main(void)
{
    test_net();
    test_fake3d();
    TEST_MAIN_END();
}
