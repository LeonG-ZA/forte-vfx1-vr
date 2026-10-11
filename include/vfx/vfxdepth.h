/*
 * vfxdepth.h -- an optional "fake 3D" for titles that draw no stereo: depth
 * from one frame by a small neural network (FastDepth, MIT's MobileNet depth
 * model, 256x256 input), then a left and a right eye made by shifting the
 * frame by the disparity that depth implies.
 *
 * The network is the int8 TFLite model from STMicroelectronics' model zoo,
 * exported to a plain file of float layers by tools/depthtest/
 * export_fastdepth.py (the format is described there). It is not shipped:
 * the host loads it from its files folder if it is there.
 *
 * Plain C, no libraries: the same code in the PC and Quest builds. The
 * prototype, scores and timings are in tools/depthtest/fake3d.py.
 */
#ifndef VFX_VFXDEPTH_H
#define VFX_VFXDEPTH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VFXDEPTH_SIZE 256           /* the network's square input and output */

typedef struct VfxDepthNet VfxDepthNet;

/* NULL if the file is missing or not an export of this network. */
VfxDepthNet *vfxdepth_load(const char *path);
void vfxdepth_free(VfxDepthNet *net);

/* Depth for a VFXDEPTH_SIZE square RGB image: out gets SIZE*SIZE floats,
 * larger farther, in the network's own units. Returns 0 on success. */
int vfxdepth_run(VfxDepthNet *net, const uint8_t *rgb, float *out);

/* Bilinear resize of an RGB image, for the network's input. */
void vfxdepth_resize_rgb(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh);

/* The steps between depth and eyes, with their state between frames. */
typedef struct VfxFake3D {
    float lo, hi;                   /* the inverse depth range, following the scene slowly */
    int have_range;
    float *nearness;                /* 0..1, 1 nearest, VFXDEPTH_SIZE square, time smoothed */
    int have_near;
    float *work;                    /* scratch for the frame-sized steps */
    size_t work_size;
} VfxFake3D;

void vfxfake3d_init(VfxFake3D *f);
void vfxfake3d_free(VfxFake3D *f);
/* Forget the scene: the next depth map starts the range and smoothing anew. */
void vfxfake3d_reset(VfxFake3D *f);

/* Folds a new depth map (VFXDEPTH_SIZE square, from vfxdepth_run) into the
 * smoothed nearness. */
void vfxfake3d_add_depth(VfxFake3D *f, const float *depth);

/* The eyes side by side, left eye on the left (2w x h RGB), from a w x h
 * frame and the current nearness, its edges snapped to the frame's by a
 * guided filter. disparity: pixels between the eyes for the nearest point
 * at 320 wide; mid depth stays at the screen. Returns 0, or -1 with no depth
 * yet or out of memory. */
int vfxfake3d_eyes(VfxFake3D *f, const uint8_t *rgb, int w, int h, double disparity, uint8_t *eyes);

#ifdef __cplusplus
}
#endif

#endif
