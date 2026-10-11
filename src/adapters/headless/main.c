/*
 * main.c -- headless adapter.
 *
 * This is the third kind of adapter the architecture allows, alongside the
 * WebXR and OpenXR ones sketched in section 6: it feeds the bridge a scripted
 * head pose on a virtual clock and writes eye images to disk. It exists to
 * prove the contract end to end with no XR runtime, no GPU and no DOS game,
 * and it doubles as the reference for what a real adapter has to do.
 *
 * Everything platform-specific lives here. The bridge below it never learns
 * where the pose came from or where the pixels went.
 */
#include "vfx/vfxbridge.h"
#include "vfx/vfxmouse.h"
#include "core_stub/stub_core.h"
#include "png_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
    const char     *out_dir;
    double          hmd_hz;
    double          core_hz;
    double          seconds;
    VfxStereoFormat format;
    int             lineseq_by_frame;
    int             provide_split;
    int             reproject;
    int             report_render_pose;
    int             dump_every;   /* HMD frames between PNG dumps; 0 = none */
    uint32_t        eye_w, eye_h;
    int             mouse_trace;  /* also run the INT 33h mouse path */
} Options;

static void usage(const char *argv0)
{
    printf(
"usage: %s [options]\n"
"\n"
"  --out DIR            write eye PNGs here (default: no image output)\n"
"  --dump-every N       dump a stereo pair every N HMD frames (default 30)\n"
"  --format F           sbs | lineseq-field | lineseq-frame | none\n"
"                       (default sbs)\n"
"  --core-hz H          emulated DOS frame rate (default 20)\n"
"  --hmd-hz H           headset frame rate (default 90)\n"
"  --seconds S          length of the scripted run (default 2)\n"
"  --eye WxH            per-eye resolution (default 263x230, the real panels)\n"
"  --split              core also hands over pre-split eye buffers\n"
"  --no-reproject       disable the reprojection layer\n"
"  --no-render-pose     core hides render_pose, forcing the bridge estimate\n"
"  --mouse-trace        also run the INT 33h mouse mapping over the\n"
"                       same head motion, and report the counts it would\n"
"                       inject into DOSBox-X\n"
"  --help\n", argv0);
}

static int parse_format(const char *s, VfxStereoFormat *fmt, int *by_frame)
{
    if (!strcmp(s, "sbs"))            { *fmt = VFX_STEREO_SIDE_BY_SIDE;    *by_frame = 0; return 1; }
    if (!strcmp(s, "lineseq-field"))  { *fmt = VFX_STEREO_LINE_SEQUENTIAL; *by_frame = 0; return 1; }
    if (!strcmp(s, "lineseq-frame"))  { *fmt = VFX_STEREO_LINE_SEQUENTIAL; *by_frame = 1; return 1; }
    if (!strcmp(s, "none"))           { *fmt = VFX_STEREO_NONE;            *by_frame = 0; return 1; }
    return 0;
}

static const char *format_name(VfxStereoFormat f)
{
    switch (f) {
    case VFX_STEREO_SIDE_BY_SIDE:    return "side-by-side";
    case VFX_STEREO_LINE_SEQUENTIAL: return "line-sequential";
    default:                         return "none (mono)";
    }
}

/*
 * Scripted head motion standing in for a real tracker: a steady yaw sweep with
 * a slower pitch nod on top. Deliberately fast enough that the gap between two
 * DOS frames is visible, which is what makes the reprojection layer testable.
 */
static VfxQuat scripted_pose(double t_seconds)
{
    float yaw   = (float)(sin(t_seconds * 0.9) * 0.9);   /* +-51 deg */
    float pitch = (float)(sin(t_seconds * 0.37) * 0.30); /* +-17 deg */
    float cy = cosf(yaw * 0.5f),   sy = sinf(yaw * 0.5f);
    float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
    VfxQuat q;
    /* qy * qp, matching vfx_quat_from_yaw_pitch_roll with roll = 0. */
    q.w = cy * cp;
    q.x = cy * sp;
    q.y = sy * cp;
    q.z = -sy * sp;
    return q;
}

static int dump_pair(const Options *o, const VfxEyeFrames *ef, uint32_t index)
{
    char path[512];
    int ok = 1;
    if (!o->out_dir)
        return 1;
    snprintf(path, sizeof(path), "%s/frame%04u_left.png", o->out_dir, index);
    ok &= vfx_png_write_rgba(path, ef->rgba[VFX_EYE_LEFT], ef->width, ef->height);
    snprintf(path, sizeof(path), "%s/frame%04u_right.png", o->out_dir, index);
    ok &= vfx_png_write_rgba(path, ef->rgba[VFX_EYE_RIGHT], ef->width, ef->height);
    return ok;
}

int main(int argc, char **argv)
{
    Options o;
    VfxCoreApi        core;
    VfxStubCoreConfig ccfg;
    VfxBridgeConfig   bcfg;
    VfxBridge        *bridge;

    VfxMouseMap       mmap;
    VfxMouseMapConfig mcfg;
    long     mouse_dx_total = 0, mouse_dy_total = 0;
    int32_t  mouse_dx_peak = 0;
    uint64_t mouse_nonzero = 0;

    uint64_t hmd_frames = 0, stale = 0, reprojected = 0, dumped = 0;
    uint64_t core_frames_run = 0;
    double   coverage_sum = 0.0, max_angle = 0.0;
    uint64_t total_hmd;
    uint64_t i;
    int      exit_code = 0;

    memset(&o, 0, sizeof(o));
    o.hmd_hz = 90.0;
    o.core_hz = 20.0;
    o.seconds = 2.0;
    o.format = VFX_STEREO_SIDE_BY_SIDE;
    o.reproject = 1;
    o.report_render_pose = 1;
    o.dump_every = 30;
    o.eye_w = 263u;
    o.eye_h = 230u;

    for (i = 1; i < (uint64_t)argc; ++i) {
        const char *a = argv[i];
        int has_next = ((int)i + 1) < argc;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(argv[0]); return 0; }
        else if (!strcmp(a, "--out") && has_next)        o.out_dir = argv[++i];
        else if (!strcmp(a, "--dump-every") && has_next) o.dump_every = atoi(argv[++i]);
        else if (!strcmp(a, "--core-hz") && has_next)    o.core_hz = atof(argv[++i]);
        else if (!strcmp(a, "--hmd-hz") && has_next)     o.hmd_hz = atof(argv[++i]);
        else if (!strcmp(a, "--seconds") && has_next)    o.seconds = atof(argv[++i]);
        else if (!strcmp(a, "--split"))                  o.provide_split = 1;
        else if (!strcmp(a, "--no-reproject"))           o.reproject = 0;
        else if (!strcmp(a, "--no-render-pose"))         o.report_render_pose = 0;
        else if (!strcmp(a, "--mouse-trace"))            o.mouse_trace = 1;
        else if (!strcmp(a, "--eye") && has_next) {
            unsigned w = 0, h = 0;
            if (sscanf(argv[++i], "%ux%u", &w, &h) != 2 || !w || !h) {
                fprintf(stderr, "bad --eye value, expected WxH\n");
                return 2;
            }
            o.eye_w = w; o.eye_h = h;
        }
        else if (!strcmp(a, "--format") && has_next) {
            if (!parse_format(argv[++i], &o.format, &o.lineseq_by_frame)) {
                fprintf(stderr, "unknown --format value\n");
                return 2;
            }
        }
        else { fprintf(stderr, "unknown option: %s\n", a); usage(argv[0]); return 2; }
    }

    if (o.hmd_hz <= 0.0 || o.core_hz <= 0.0 || o.seconds <= 0.0) {
        fprintf(stderr, "rates and duration must be positive\n");
        return 2;
    }

    vfx_stub_core_config_defaults(&ccfg);
    ccfg.eye_width  = o.eye_w;
    ccfg.eye_height = o.eye_h;
    ccfg.format     = o.format;
    ccfg.line_sequential_by_frame = o.lineseq_by_frame;
    ccfg.provide_split      = o.provide_split;
    ccfg.report_render_pose = o.report_render_pose;

    if (!vfx_stub_core_create(&ccfg, &core)) {
        fprintf(stderr, "failed to create stub core\n");
        return 1;
    }

    vfx_bridge_config_defaults(&bcfg);
    bcfg.core_hz = o.core_hz;
    bcfg.reprojection_enabled = o.reproject;
    bcfg.line_sequential_mode = o.lineseq_by_frame ? VFX_LINESEQ_FRAME
                                                   : VFX_LINESEQ_FIELD;
    bcfg.source_fov_h_rad = ccfg.fov_h_rad;
    bcfg.source_fov_v_rad = ccfg.fov_v_rad;

    bridge = vfx_bridge_create(&core, &bcfg);
    if (!bridge) {
        fprintf(stderr, "failed to create bridge\n");
        core.destroy(core.handle);
        return 1;
    }

    printf("stereo format      : %s%s\n", format_name(o.format),
           (o.format == VFX_STEREO_LINE_SEQUENTIAL)
               ? (o.lineseq_by_frame ? " (frame-alternating)" : " (field/scanline)")
               : "");
    printf("per-eye resolution : %ux%u\n", o.eye_w, o.eye_h);
    printf("core / hmd rate    : %.1f Hz / %.1f Hz\n", o.core_hz, o.hmd_hz);
    printf("reprojection       : %s\n", o.reproject ? "on" : "off");
    printf("render_pose from core: %s\n",
           o.report_render_pose ? "reported" : "hidden (bridge estimates)");

    /* The mouse path is independent of the frame path: it converts the same
     * head pose into the relative motion DOSBox-X injects through INT 33h. */
    vfx_mousemap_config_defaults(&mcfg);
    vfx_mousemap_init(&mmap, &mcfg);
    if (o.mouse_trace)
        printf("int 33h mouse path : on, %.0f counts per radian\n",
               (double)mcfg.counts_per_radian_yaw);

    total_hmd = (uint64_t)(o.seconds * o.hmd_hz);

    for (i = 0; i < total_hmd; ++i) {
        double   t   = (double)i / o.hmd_hz;
        uint64_t now = (uint64_t)(t * 1e9);
        VfxPose  head;
        VfxControllerState puck;
        VfxEyeFrames ef;

        memset(&head, 0, sizeof(head));
        head.orientation  = scripted_pose(t);
        head.timestamp_ns = now;

        memset(&puck, 0, sizeof(puck));
        puck.tilt_x  = (float)sin(t * 1.7);
        puck.tilt_y  = (float)cos(t * 1.1);
        puck.buttons = (uint8_t)(((uint64_t)(t * 3.0)) & 0x7u);

        vfx_bridge_submit_pose(bridge, &head, &puck);

        if (o.mouse_trace) {
            /* Exactly what the DOSBox-X module hands to Mouse_CursorMoved. */
            VfxMouseDelta md = vfx_mousemap_update(&mmap, &head);
            mouse_dx_total += md.dx;
            mouse_dy_total += md.dy;
            if (md.dx || md.dy) ++mouse_nonzero;
            if (md.dx >  mouse_dx_peak) mouse_dx_peak =  md.dx;
            if (-md.dx > mouse_dx_peak) mouse_dx_peak = -md.dx;
        }

        core_frames_run += vfx_bridge_pump(bridge, now);

        if (!vfx_bridge_acquire_eyes(bridge, now, &ef)) {
            /* Expected only for the first HMD frames, before the core has
             * produced anything at all. */
            continue;
        }

        ++hmd_frames;
        if (ef.is_stale)    ++stale;
        if (ef.reprojected) ++reprojected;
        coverage_sum += 0.5 * ((double)ef.coverage[0] + (double)ef.coverage[1]);
        if ((double)ef.reprojection_angle_rad > max_angle)
            max_angle = (double)ef.reprojection_angle_rad;

        if (o.out_dir && o.dump_every > 0 && (i % (uint64_t)o.dump_every) == 0) {
            if (!dump_pair(&o, &ef, (uint32_t)dumped)) {
                fprintf(stderr, "failed writing PNGs to %s\n", o.out_dir);
                exit_code = 1;
                break;
            }
            ++dumped;
        }
    }

    printf("\n");
    printf("hmd frames served  : %llu\n", (unsigned long long)hmd_frames);
    printf("core frames run    : %llu\n", (unsigned long long)core_frames_run);
    printf("  reported by core : %llu\n",
           (unsigned long long)vfx_stub_core_frame_count(core.handle));
    printf("served from a stale core frame : %llu (%.1f%%)\n",
           (unsigned long long)stale,
           hmd_frames ? 100.0 * (double)stale / (double)hmd_frames : 0.0);
    printf("reprojected        : %llu (%.1f%%)\n",
           (unsigned long long)reprojected,
           hmd_frames ? 100.0 * (double)reprojected / (double)hmd_frames : 0.0);
    printf("mean eye coverage  : %.3f\n",
           hmd_frames ? coverage_sum / (double)hmd_frames : 0.0);
    printf("max reproject angle: %.2f deg\n", max_angle * 57.2957795);
    if (o.out_dir)
        printf("stereo pairs written: %llu -> %s\n",
               (unsigned long long)dumped, o.out_dir);

    if (o.mouse_trace) {
        float ty = 0.0f, tp = 0.0f;
        vfx_mousemap_tracked(&mmap, &ty, &tp);
        printf("\nint 33h mouse path\n");
        printf("  updates emitting motion : %llu of %llu\n",
               (unsigned long long)mouse_nonzero, (unsigned long long)total_hmd);
        printf("  net counts              : %ld x, %ld y\n",
               mouse_dx_total, mouse_dy_total);
        printf("  largest single step     : %d counts\n", (int)mouse_dx_peak);
        printf("  clamped by the limiter  : %llu\n",
               (unsigned long long)mmap.clamped);
        printf("  head now at             : yaw %.1f deg, pitch %.1f deg\n",
               (double)ty * 57.2957795, (double)tp * 57.2957795);
    }

    /* Prove the pose really reached the core, not just the bridge. */
    {
        VfxControllerState c = vfx_stub_core_last_controller(core.handle);
        VfxPose p = vfx_stub_core_last_pose(core.handle);
        printf("core last saw quat (%.3f %.3f %.3f %.3f), puck tilt (%.2f %.2f) buttons %u\n",
               p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z,
               c.tilt_x, c.tilt_y, (unsigned)c.buttons);
    }

    vfx_bridge_destroy(bridge);
    core.destroy(core.handle);
    return exit_code;
}
