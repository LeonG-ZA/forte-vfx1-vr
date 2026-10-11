/*
 * vfxweb.c -- libvfxbridge's head-to-mouse mapping, exported to JavaScript.
 *
 * The WebXR adapter (web/vfx1.html) reads the headset pose every XR frame and
 * needs it turned into relative mouse counts for js-dos. That conversion is
 * already written and unit tested in src/bridge/mousemap.c: yaw wraparound,
 * sub-count accumulation, pitch limits, glitch clamping. Rather than port it to
 * JavaScript and maintain two copies, this file compiles the C unchanged to
 * WebAssembly and exposes a flat, allocation-free API that is trivial to call
 * from JS. That the bridge compiles to WASM unchanged was a design goal; this is
 * the first place it is exercised.
 *
 * Adapter code: it may name Emscripten. The bridge underneath may not, and
 * tools/check_layering.sh checks that.
 *
 * Build: web/build-bridge.sh
 */
#include "vfx/vfxmouse.h"
#include "../../bridge/vfxmath.h"

#ifdef __EMSCRIPTEN__
# include <emscripten/emscripten.h>
# define VFXWEB_EXPORT EMSCRIPTEN_KEEPALIVE
#else
# define VFXWEB_EXPORT
#endif

static VfxMouseMap map;
static int initialised = 0;
static int32_t last_dx = 0, last_dy = 0;

static void ensure_init(void)
{
    if (!initialised) {
        vfx_mousemap_init(&map, NULL);
        initialised = 1;
    }
}

/* Replace the whole configuration. Angles in degrees for JS convenience. */
VFXWEB_EXPORT
void vfxweb_configure(double counts_per_rad_yaw, double counts_per_rad_pitch,
                      double pitch_limit_deg, int invert_yaw, int invert_pitch,
                      int enable_yaw, int enable_pitch, int max_counts);
void vfxweb_configure(double counts_per_rad_yaw, double counts_per_rad_pitch,
                      double pitch_limit_deg, int invert_yaw, int invert_pitch,
                      int enable_yaw, int enable_pitch, int max_counts)
{
    VfxMouseMapConfig cfg;
    vfx_mousemap_config_defaults(&cfg);
    cfg.counts_per_radian_yaw   = (float)counts_per_rad_yaw;
    cfg.counts_per_radian_pitch = (float)counts_per_rad_pitch;
    cfg.pitch_limit_rad         = (float)(pitch_limit_deg * 3.14159265358979 / 180.0);
    cfg.invert_yaw              = invert_yaw ? 1 : 0;
    cfg.invert_pitch            = invert_pitch ? 1 : 0;
    cfg.enable_yaw              = enable_yaw ? 1 : 0;
    cfg.enable_pitch            = enable_pitch ? 1 : 0;
    cfg.max_counts_per_update   = (int32_t)max_counts;
    vfx_mousemap_init(&map, &cfg);
    initialised = 1;
}

VFXWEB_EXPORT void vfxweb_recenter(void);
void vfxweb_recenter(void)
{
    ensure_init();
    vfx_mousemap_recenter(&map);
}

/*
 * Feed one head orientation (a WebXR quaternion passes straight through: the
 * conventions match). Returns 1 if there is motion to send, after which
 * vfxweb_dx / vfxweb_dy hold the counts.
 */
VFXWEB_EXPORT int vfxweb_update(double w, double x, double y, double z);
int vfxweb_update(double w, double x, double y, double z)
{
    VfxPose pose;
    VfxMouseDelta d;

    ensure_init();
    pose.orientation.w = (float)w;
    pose.orientation.x = (float)x;
    pose.orientation.y = (float)y;
    pose.orientation.z = (float)z;
    pose.position.x = pose.position.y = pose.position.z = 0.0f;
    pose.timestamp_ns = 0u;

    d = vfx_mousemap_update(&map, &pose);
    last_dx = d.dx;
    last_dy = d.dy;
    return (d.dx != 0 || d.dy != 0) ? 1 : 0;
}

VFXWEB_EXPORT int vfxweb_dx(void);
int vfxweb_dx(void) { return last_dx; }

VFXWEB_EXPORT int vfxweb_dy(void);
int vfxweb_dy(void) { return last_dy; }

VFXWEB_EXPORT double vfxweb_tracked_yaw_deg(void);
double vfxweb_tracked_yaw_deg(void)
{
    float yaw = 0.0f, pitch = 0.0f;
    ensure_init();
    vfx_mousemap_tracked(&map, &yaw, &pitch);
    return (double)yaw * 180.0 / 3.14159265358979;
}

VFXWEB_EXPORT double vfxweb_tracked_pitch_deg(void);
double vfxweb_tracked_pitch_deg(void)
{
    float yaw = 0.0f, pitch = 0.0f;
    ensure_init();
    vfx_mousemap_tracked(&map, &yaw, &pitch);
    return (double)pitch * 180.0 / 3.14159265358979;
}

VFXWEB_EXPORT double vfxweb_clamped_count(void);
double vfxweb_clamped_count(void)
{
    ensure_init();
    return (double)map.clamped;
}
