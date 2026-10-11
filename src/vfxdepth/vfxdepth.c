/*
 * vfxdepth.c -- FastDepth in plain C, and the fake 3D steps after it
 * (include/vfx/vfxdepth.h).
 */
#include "vfx/vfxdepth.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { L_PAD = 1, L_CONV = 2, L_DW = 3, L_RESIZE = 4, L_ADD = 5 };

typedef struct Layer {
    uint32_t type, in, in2, out, k, stride, pad[4];
    float clamp;
    float *w, *b;
} Layer;

typedef struct Shape { uint32_t h, w, c; } Shape;

struct VfxDepthNet {
    uint32_t tensors, layers;
    Shape *shape;
    Layer *layer;
    float **t;                      /* every tensor, allocated once */
};

/* ------------------------------------------------------------- loading */

static int read_u32(FILE *f, uint32_t *v)
{
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return 0;
    *v = (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
    return 1;
}

static float *read_floats(FILE *f, size_t n)
{
    float *p = (float *)malloc(n * sizeof *p);
    if (!p || fread(p, sizeof *p, n, f) != n) { free(p); return NULL; }
    return p;                       /* the file is little-endian, as both hosts are */
}

void vfxdepth_free(VfxDepthNet *net)
{
    uint32_t i;
    if (!net) return;
    if (net->layer)
        for (i = 0; i < net->layers; i++) { free(net->layer[i].w); free(net->layer[i].b); }
    if (net->t)
        for (i = 0; i < net->tensors; i++) free(net->t[i]);
    free(net->t);
    free(net->layer);
    free(net->shape);
    free(net);
}

VfxDepthNet *vfxdepth_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    VfxDepthNet *net;
    char magic[4];
    uint32_t version, i, j;
    if (!f) return NULL;
    net = (VfxDepthNet *)calloc(1, sizeof *net);
    if (!net || fread(magic, 1, 4, f) != 4 || memcmp(magic, "VFDN", 4) || !read_u32(f, &version) || version != 1 ||
        !read_u32(f, &net->tensors) || !read_u32(f, &net->layers) || net->tensors < 2 || net->tensors > 1024 ||
        net->layers < 1 || net->layers > 1024)
        goto fail;
    net->shape = (Shape *)calloc(net->tensors, sizeof *net->shape);
    net->layer = (Layer *)calloc(net->layers, sizeof *net->layer);
    net->t = (float **)calloc(net->tensors, sizeof *net->t);
    if (!net->shape || !net->layer || !net->t) goto fail;
    for (i = 0; i < net->tensors; i++) {
        Shape *s = &net->shape[i];
        if (!read_u32(f, &s->h) || !read_u32(f, &s->w) || !read_u32(f, &s->c) || s->h > 4096 || s->w > 4096 ||
            s->c > 4096)
            goto fail;
        net->t[i] = (float *)malloc((size_t)s->h * s->w * s->c * sizeof(float));
        if (!net->t[i]) goto fail;
    }
    if (net->shape[0].h != VFXDEPTH_SIZE || net->shape[0].w != VFXDEPTH_SIZE || net->shape[0].c != 3) goto fail;
    for (i = 0; i < net->layers; i++) {
        Layer *l = &net->layer[i];
        uint32_t *fields[10] = { &l->type, &l->in, &l->in2, &l->out, &l->k, &l->stride,
                                 &l->pad[0], &l->pad[1], &l->pad[2], &l->pad[3] };
        for (j = 0; j < 10; j++)
            if (!read_u32(f, fields[j])) goto fail;
        if (fread(&l->clamp, sizeof l->clamp, 1, f) != 1) goto fail;
        if (l->in >= net->tensors || l->in2 >= net->tensors || l->out >= net->tensors) goto fail;
        if (l->type == L_CONV || l->type == L_DW) {
            const Shape *si = &net->shape[l->in], *so = &net->shape[l->out];
            size_t nw = l->type == L_CONV ? (size_t)so->c * l->k * l->k * si->c : (size_t)l->k * l->k * si->c;
            if (l->k == 0 || l->k > 7 || l->stride == 0 || l->stride > 2) goto fail;
            if (!(l->w = read_floats(f, nw)) || !(l->b = read_floats(f, so->c))) goto fail;
            if (l->type == L_CONV) {
                /* (out, ky, kx, in) -> (ky, kx, in, out): the inner loop then
                 * runs along the outputs, which compilers vectorise */
                float *t = (float *)malloc(nw * sizeof *t);
                size_t o, q, kk = (size_t)l->k * l->k, ci = si->c, co = so->c;
                if (!t) goto fail;
                for (o = 0; o < co; o++)
                    for (q = 0; q < kk * ci; q++) t[q * co + o] = l->w[o * kk * ci + q];
                free(l->w);
                l->w = t;
            }
        } else if (l->type < L_PAD || l->type > L_ADD) {
            goto fail;
        }
    }
    fclose(f);
    return net;
fail:
    fclose(f);
    vfxdepth_free(net);
    return NULL;
}

/* ------------------------------------------------------------- layers */

static float clampf(float v, float hi)
{
    return v < 0 ? 0 : v > hi ? hi : v;
}

static void pad(const float *x, const Shape *si, float *y, const Shape *so, const uint32_t p[4])
{
    size_t row = (size_t)si->w * si->c;
    uint32_t r;
    memset(y, 0, (size_t)so->h * so->w * so->c * sizeof *y);
    for (r = 0; r < si->h; r++)
        memcpy(y + ((size_t)(r + p[0]) * so->w + p[2]) * so->c, x + (size_t)r * row, row * sizeof *y);
}

static void conv(const Layer *l, const float *x, const Shape *si, float *y, const Shape *so)
{
    uint32_t oy, ox, o, ky, kx, i, k = l->k, s = l->stride, ci = si->c, co = so->c;
    for (oy = 0; oy < so->h; oy++)
        for (ox = 0; ox < so->w; ox++) {
            float *out = y + ((size_t)oy * so->w + ox) * co;
            memcpy(out, l->b, co * sizeof *out);
            for (ky = 0; ky < k; ky++)
                for (kx = 0; kx < k; kx++) {
                    const float *in = x + ((size_t)(oy * s + ky) * si->w + ox * s + kx) * ci;
                    const float *w = l->w + ((size_t)ky * k + kx) * ci * co;
                    for (i = 0; i < ci; i++, w += co) {
                        float v = in[i];
                        for (o = 0; o < co; o++) out[o] += v * w[o];
                    }
                }
            for (o = 0; o < co; o++) out[o] = clampf(out[o], l->clamp);
        }
}

static void depthwise(const Layer *l, const float *x, const Shape *si, float *y, const Shape *so)
{
    uint32_t oy, ox, ky, kx, c, k = l->k, s = l->stride, n = so->c;
    for (oy = 0; oy < so->h; oy++)
        for (ox = 0; ox < so->w; ox++) {
            float *out = y + ((size_t)oy * so->w + ox) * n;
            memcpy(out, l->b, n * sizeof *out);
            for (ky = 0; ky < k; ky++)
                for (kx = 0; kx < k; kx++) {
                    const float *in = x + ((size_t)(oy * s + ky) * si->w + ox * s + kx) * n;
                    const float *w = l->w + ((size_t)ky * k + kx) * n;
                    for (c = 0; c < n; c++) out[c] += in[c] * w[c];
                }
            for (c = 0; c < n; c++) out[c] = clampf(out[c], l->clamp);
        }
}

static void resize2(const float *x, const Shape *si, float *y, const Shape *so)
{
    uint32_t oy, ox;
    size_t c = si->c;
    for (oy = 0; oy < so->h; oy++)
        for (ox = 0; ox < so->w; ox++)
            memcpy(y + ((size_t)oy * so->w + ox) * c, x + ((size_t)(oy / 2) * si->w + ox / 2) * c, c * sizeof *y);
}

int vfxdepth_run(VfxDepthNet *net, const uint8_t *rgb, float *out)
{
    size_t i, n0 = (size_t)VFXDEPTH_SIZE * VFXDEPTH_SIZE * 3;
    uint32_t li;
    const Layer *last;
    if (!net || !rgb || !out) return -1;
    for (i = 0; i < n0; i++) net->t[0][i] = (float)rgb[i] * (1.0f / 255.0f);
    for (li = 0; li < net->layers; li++) {
        const Layer *l = &net->layer[li];
        const Shape *si = &net->shape[l->in], *so = &net->shape[l->out];
        float *x = net->t[l->in], *y = net->t[l->out];
        switch (l->type) {
        case L_PAD: pad(x, si, y, so, l->pad); break;
        case L_CONV: conv(l, x, si, y, so); break;
        case L_DW: depthwise(l, x, si, y, so); break;
        case L_RESIZE: resize2(x, si, y, so); break;
        default: {
            const float *x2 = net->t[l->in2];
            size_t n = (size_t)so->h * so->w * so->c;
            for (i = 0; i < n; i++) y[i] = clampf(x[i] + x2[i], l->clamp);
        }
        }
    }
    last = &net->layer[net->layers - 1];
    memcpy(out, net->t[last->out], (size_t)VFXDEPTH_SIZE * VFXDEPTH_SIZE * sizeof *out);
    return 0;
}

void vfxdepth_resize_rgb(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh)
{
    int x, y, c;
    for (y = 0; y < dh; y++) {
        double fy = (y + 0.5) * sh / dh - 0.5;
        int y0 = fy < 0 ? 0 : (int)fy, y1 = y0 + 1 < sh ? y0 + 1 : sh - 1;
        double ty = fy < 0 ? 0 : fy - y0;
        for (x = 0; x < dw; x++) {
            double fx = (x + 0.5) * sw / dw - 0.5;
            int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < sw ? x0 + 1 : sw - 1;
            double tx = fx < 0 ? 0 : fx - x0;
            for (c = 0; c < 3; c++) {
                double a = src[((size_t)y0 * sw + x0) * 3 + c] * (1 - tx) + src[((size_t)y0 * sw + x1) * 3 + c] * tx;
                double b = src[((size_t)y1 * sw + x0) * 3 + c] * (1 - tx) + src[((size_t)y1 * sw + x1) * 3 + c] * tx;
                dst[((size_t)y * dw + x) * 3 + c] = (uint8_t)(a * (1 - ty) + b * ty + 0.5);
            }
        }
    }
}

/* ------------------------------------------------------------- fake 3D */

#define RANGE_KEEP 0.9f             /* share of the previous range kept per depth map */
#define TIME_KEEP 0.6f              /* share of the previous nearness kept per depth map */
#define GUIDE_RADIUS 4              /* guided filter window at 320 wide */
#define GUIDE_EPS 2e-3f             /* guided filter smoothing, (0..1 luma)^2 */

void vfxfake3d_init(VfxFake3D *f)
{
    memset(f, 0, sizeof *f);
}

void vfxfake3d_free(VfxFake3D *f)
{
    free(f->nearness);
    free(f->work);
    vfxfake3d_init(f);
}

void vfxfake3d_reset(VfxFake3D *f)
{
    f->have_range = 0;
    f->have_near = 0;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

void vfxfake3d_add_depth(VfxFake3D *f, const float *depth)
{
    enum { N = VFXDEPTH_SIZE * VFXDEPTH_SIZE, SAMPLES = 4096 };
    static float sample[SAMPLES];
    float lo, hi, span;
    size_t i;
    if (!f->nearness && !(f->nearness = (float *)malloc(N * sizeof *f->nearness))) return;
    /* the range of inverse depth, 2nd to 98th percentile, from a sample */
    for (i = 0; i < SAMPLES; i++) sample[i] = 1.0f / fmaxf(depth[i * (N / SAMPLES)], 1e-3f);
    qsort(sample, SAMPLES, sizeof *sample, cmp_float);
    lo = sample[SAMPLES * 2 / 100];
    hi = sample[SAMPLES * 98 / 100];
    if (f->have_range) {
        lo = RANGE_KEEP * f->lo + (1 - RANGE_KEEP) * lo;
        hi = RANGE_KEEP * f->hi + (1 - RANGE_KEEP) * hi;
    }
    f->lo = lo;
    f->hi = hi;
    f->have_range = 1;
    span = hi - lo;
    for (i = 0; i < N; i++) {
        /* no depth range (a blank screen): all at the screen */
        float n = span > 1e-3f * fabsf(hi) ? (1.0f / fmaxf(depth[i], 1e-3f) - lo) / span : 0.5f;
        n = n < 0 ? 0 : n > 1 ? 1 : n;
        f->nearness[i] = f->have_near ? TIME_KEEP * f->nearness[i] + (1 - TIME_KEEP) * n : n;
    }
    f->have_near = 1;
}

/* Box mean of a w x h float image, radius r, edges clamped. */
static void box_mean(const float *src, float *dst, float *tmp, int w, int h, int r)
{
    int x, y;
    for (y = 0; y < h; y++) {                       /* rows */
        const float *s = src + (size_t)y * w;
        float *t = tmp + (size_t)y * w;
        double sum = 0;
        for (x = -r; x <= r; x++) sum += s[x < 0 ? 0 : x >= w ? w - 1 : x];
        for (x = 0; x < w; x++) {
            int add = x + r + 1, sub = x - r;
            t[x] = (float)(sum / (2 * r + 1));
            sum += s[add >= w ? w - 1 : add] - s[sub < 0 ? 0 : sub];
        }
    }
    for (x = 0; x < w; x++) {                       /* columns */
        double sum = 0;
        for (y = -r; y <= r; y++) sum += tmp[(size_t)(y < 0 ? 0 : y >= h ? h - 1 : y) * w + x];
        for (y = 0; y < h; y++) {
            int add = y + r + 1, sub = y - r;
            dst[(size_t)y * w + x] = (float)(sum / (2 * r + 1));
            sum += tmp[(size_t)(add >= h ? h - 1 : add) * w + x] - tmp[(size_t)(sub < 0 ? 0 : sub) * w + x];
        }
    }
}

int vfxfake3d_eyes(VfxFake3D *f, const uint8_t *rgb, int w, int h, double disparity, uint8_t *eyes)
{
    size_t n = (size_t)w * h, need = n * 9 * sizeof(float), i;
    float *I, *P, *mI, *mP, *IP, *II, *a, *b, *tmp;
    int x, y, r;
    double scale = disparity * w / 320.0;
    if (!f->have_near || w < 8 || h < 8) return -1;
    if (need > f->work_size) {
        float *p = (float *)realloc(f->work, need);
        if (!p) return -1;
        f->work = p;
        f->work_size = need;
    }
    I = f->work; P = I + n; mI = P + n; mP = mI + n; IP = mP + n; II = IP + n; a = II + n; b = a + n; tmp = b + n;
    /* the frame's luma, and the nearness scaled up to it */
    for (y = 0; y < h; y++) {
        double fy = (y + 0.5) * VFXDEPTH_SIZE / h - 0.5;
        int y0 = fy < 0 ? 0 : (int)fy, y1 = y0 + 1 < VFXDEPTH_SIZE ? y0 + 1 : VFXDEPTH_SIZE - 1;
        float ty = fy < 0 ? 0 : (float)(fy - y0);
        for (x = 0; x < w; x++) {
            double fx = (x + 0.5) * VFXDEPTH_SIZE / w - 0.5;
            int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < VFXDEPTH_SIZE ? x0 + 1 : VFXDEPTH_SIZE - 1;
            float tx = fx < 0 ? 0 : (float)(fx - x0);
            const float *nr = f->nearness;
            float top = nr[y0 * VFXDEPTH_SIZE + x0] * (1 - tx) + nr[y0 * VFXDEPTH_SIZE + x1] * tx;
            float bot = nr[y1 * VFXDEPTH_SIZE + x0] * (1 - tx) + nr[y1 * VFXDEPTH_SIZE + x1] * tx;
            const uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            i = (size_t)y * w + x;
            P[i] = top * (1 - ty) + bot * ty;
            I[i] = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) * (1.0f / 255.0f);
        }
    }
    /* He's guided filter: the nearness, with its edges taken from the luma */
    r = (int)lround(GUIDE_RADIUS * w / 320.0);
    if (r < 1) r = 1;
    for (i = 0; i < n; i++) { IP[i] = I[i] * P[i]; II[i] = I[i] * I[i]; }
    box_mean(I, mI, tmp, w, h, r);
    box_mean(P, mP, tmp, w, h, r);
    box_mean(IP, IP, tmp, w, h, r);
    box_mean(II, II, tmp, w, h, r);
    for (i = 0; i < n; i++) {
        float cov = IP[i] - mI[i] * mP[i], var = II[i] - mI[i] * mI[i];
        a[i] = cov / (var + GUIDE_EPS);
        b[i] = mP[i] - a[i] * mI[i];
    }
    box_mean(a, a, tmp, w, h, r);
    box_mean(b, b, tmp, w, h, r);
    /* each eye samples the frame half the disparity sideways */
    for (y = 0; y < h; y++) {
        const uint8_t *row = rgb + (size_t)y * w * 3;
        uint8_t *le = eyes + (size_t)y * w * 6, *re = le + (size_t)w * 3;
        for (x = 0; x < w; x++) {
            float q;
            double d, sx[2];
            int e, c;
            i = (size_t)y * w + x;
            q = a[i] * I[i] + b[i];
            q = q < 0 ? 0 : q > 1 ? 1 : q;
            d = (q - 0.5) * scale;                 /* + is nearer: out of the screen */
            sx[0] = x + d / 2;
            sx[1] = x - d / 2;
            for (e = 0; e < 2; e++) {
                double s = sx[e] < 0 ? 0 : sx[e] > w - 1 ? w - 1 : sx[e];
                int x0 = (int)s, x1 = x0 + 1 < w ? x0 + 1 : w - 1;
                double t = s - x0;
                uint8_t *o = (e ? re : le) + (size_t)x * 3;
                for (c = 0; c < 3; c++)
                    o[c] = (uint8_t)(row[(size_t)x0 * 3 + c] * (1 - t) + row[(size_t)x1 * 3 + c] * t + 0.5);
            }
        }
    }
    return 0;
}
