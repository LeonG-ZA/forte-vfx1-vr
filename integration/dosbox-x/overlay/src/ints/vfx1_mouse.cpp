/*
 *  VFX1 head tracking for DOSBox-X -- INT 33h mouse path. Licensed GPL v2 or later, matching DOSBox-X.
 *
 *  The Forte VFX1 shipped a TSR called VRMOUSE that presented head orientation
 *  to DOS software as an ordinary INT 33h mouse, and a good number of titles
 *  used that rather than talking to the VIP card. Reproducing it needs no
 *  knowledge of the VIP register map, which is the one part of this project
 *  nobody has reverse engineered yet, so it is the first thing that can
 *  actually work.
 *
 *  Everything interesting about the conversion -- absolute orientation into
 *  relative mickeys, yaw wraparound, sub-count accumulation, pitch limits --
 *  lives in libvfxbridge and is unit tested there. This file is the plumbing:
 *  read config, poll a pose source on the millisecond tick, hand the pose to
 *  the mapper, push the resulting counts into Mouse_CursorMoved().
 *
 *  This file must stay transport-neutral. It names no XR API, no graphics API
 *  and no socket; each vfx1_source_*.cpp may name exactly one.
 *  tools/check_layering.sh enforces that.
 */
#include <string.h>
#include <math.h>

#include "dosbox.h"
#include "logging.h"
#include "mouse.h"
#include "pic.h"
#include "timer.h"
#include "mapper.h"
#include "setup.h"
#include "control.h"
#include "vfx1.h"

extern "C" {
#include "vfx/vfxmouse.h"
}

/*
 * DOSBox-X gates its INT 33h relative-motion path on the host cursor being
 * captured, in two places: whether mickeys accumulate at all, and whether
 * INT 33h function 0Bh hands them back. Head tracking is not the host cursor
 * and has to work whether or not the user has clicked into the window, so
 * apply.sh widens both gates to `|| VFX1_Mouse_Active()`. That is what makes
 * VFX1_Mouse_Active the switch for the whole path, and why it must stay cheap.
 */

/* ------------------------------------------------------------------ */
/* Built-in pose sources                                               */
/* ------------------------------------------------------------------ */

/* --- "none": tracking off ------------------------------------------ */

static bool src_none_open(void) { return false; }
static void src_none_close(void) { }
static bool src_none_poll(VfxPose *) { return false; }

const VfxPoseSource *VFX1_Source_None(void)
{
    static const VfxPoseSource s = {
        "none", src_none_open, src_none_close, src_none_poll
    };
    return &s;
}

/* --- "script": a deterministic sweep, for testing without a headset -- */

static double script_yaw_amplitude   = 0.9;   /* radians */
static double script_pitch_amplitude = 0.25;
static double script_yaw_period      = 8.0;   /* seconds */
static double script_pitch_period    = 5.0;
static double script_t0_ms           = 0.0;

static bool src_script_open(void)
{
    script_t0_ms = PIC_FullIndex();
    return true;
}

static void src_script_close(void) { }

static bool src_script_poll(VfxPose *out)
{
    const double two_pi = 6.283185307179586;
    double t = (PIC_FullIndex() - script_t0_ms) / 1000.0;
    double yaw   = script_yaw_amplitude   * sin(two_pi * t / script_yaw_period);
    double pitch = script_pitch_amplitude * sin(two_pi * t / script_pitch_period);

    /* Yaw about +Y then pitch about +X, which is the axis order the VFX1
     * sensor reported and the order libvfxbridge expects. */
    float cy = (float)cos(yaw * 0.5),   sy = (float)sin(yaw * 0.5);
    float cp = (float)cos(pitch * 0.5), sp = (float)sin(pitch * 0.5);

    memset(out, 0, sizeof(*out));
    out->orientation.w =  cy * cp;
    out->orientation.x =  cy * sp;
    out->orientation.y =  sy * cp;
    out->orientation.z = -sy * sp;
    out->timestamp_ns  = (uint64_t)(PIC_FullIndex() * 1.0e6);
    return true;
}

const VfxPoseSource *VFX1_Source_Script(void)
{
    static const VfxPoseSource s = {
        "script", src_script_open, src_script_close, src_script_poll
    };
    return &s;
}

/* ------------------------------------------------------------------ */
/* Module state                                                        */
/* ------------------------------------------------------------------ */

static const VfxPoseSource *vfx_source = NULL;
static VfxMouseMap          vfx_map;
static bool                 vfx_enabled = false;
static bool                 vfx_tick_registered = false;
static unsigned int         vfx_poll_interval_ms = 4u;   /* ~250 Hz */
static double               vfx_last_poll_ms = 0.0;

/* Diagnostics, reported once a second at debug level. */
static double   vfx_stat_window_ms = 0.0;
static uint64_t vfx_stat_polls = 0;
static long     vfx_stat_dx = 0, vfx_stat_dy = 0;

bool VFX1_Mouse_Active(void)
{
    return vfx_enabled && vfx_source != NULL &&
           vfx_source != VFX1_Source_None();
}

void VFX1_Mouse_Recenter(void)
{
    vfx_mousemap_recenter(&vfx_map);
    LOG_MSG("VFX1: head tracking recentred");
}

/* ------------------------------------------------------------------ */
/* The tick                                                            */
/* ------------------------------------------------------------------ */

static void VFX1_Mouse_Tick(void)
{
    VfxPose       pose;
    VfxMouseDelta d;
    double        now;

    if (!VFX1_Mouse_Active())
        return;

    /* TIMER_AddTickHandler fires every millisecond, which is far more often
     * than any tracker updates. Decimate to the configured poll rate rather
     * than resampling the same pose over and over. */
    now = PIC_FullIndex();
    if ((now - vfx_last_poll_ms) < (double)vfx_poll_interval_ms)
        return;
    vfx_last_poll_ms = now;

    if (!vfx_source->poll(&pose))
        return;

    ++vfx_stat_polls;
    d = vfx_mousemap_update(&vfx_map, &pose);
    if (d.dx == 0 && d.dy == 0)
        return;

    vfx_stat_dx += d.dx;
    vfx_stat_dy += d.dy;

    /* emulate = true selects the relative path, which is the one that feeds
     * the mickey counters a DOS game reads through INT 33h. */
    Mouse_CursorMoved((float)d.dx, (float)d.dy, 0.0f, 0.0f, true);

    if ((now - vfx_stat_window_ms) >= 1000.0) {
        LOG(LOG_MOUSE, LOG_DEBUG)("VFX1: %llu polls/s, %ld,%ld counts, %llu clamped",
                                  (unsigned long long)vfx_stat_polls,
                                  vfx_stat_dx, vfx_stat_dy,
                                  (unsigned long long)vfx_map.clamped);
        vfx_stat_window_ms = now;
        vfx_stat_polls = 0;
        vfx_stat_dx = vfx_stat_dy = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

static void VFX1_Recenter_Mapper(bool pressed)
{
    if (pressed)
        VFX1_Mouse_Recenter();
}

static const VfxPoseSource *find_source(const char *name)
{
    const VfxPoseSource *all[] = {
        VFX1_Source_None(), VFX1_Source_Script(), VFX1_Source_Net()
    };
    size_t i;
    if (name != NULL)
        for (i = 0; i < sizeof(all) / sizeof(all[0]); ++i)
            if (all[i] != NULL && strcmp(name, all[i]->name) == 0)
                return all[i];
    return VFX1_Source_None();
}

void VFX1_Init(void)
{
    Section_prop *sec = static_cast<Section_prop *>(control->GetSection("vfx1"));
    VfxMouseMapConfig mcfg;

    vfx_mousemap_config_defaults(&mcfg);
    vfx_mousemap_init(&vfx_map, &mcfg);
    vfx_source = VFX1_Source_None();

    if (sec == NULL) {
        /* Section missing means an old config file; stay silent and off. */
        vfx_enabled = false;
        return;
    }

    vfx_enabled = sec->Get_bool("headtracking");

    mcfg.enable_yaw   = sec->Get_bool("track yaw")   ? 1 : 0;
    mcfg.enable_pitch = sec->Get_bool("track pitch") ? 1 : 0;
    mcfg.invert_yaw   = sec->Get_bool("invert yaw")   ? 1 : 0;
    mcfg.invert_pitch = sec->Get_bool("invert pitch") ? 1 : 0;
    mcfg.counts_per_radian_yaw   = (float)sec->Get_double("yaw sensitivity");
    mcfg.counts_per_radian_pitch = (float)sec->Get_double("pitch sensitivity");
    mcfg.pitch_limit_rad =
        (float)(sec->Get_double("pitch limit") * 3.14159265358979 / 180.0);
    mcfg.max_counts_per_update = (int32_t)sec->Get_int("max counts per update");
    vfx_mousemap_init(&vfx_map, &mcfg);

    {
        int poll = sec->Get_int("poll rate");
        if (poll < 1)    poll = 1;
        if (poll > 1000) poll = 1000;
        vfx_poll_interval_ms = (unsigned int)(1000 / poll);
        if (vfx_poll_interval_ms < 1u)
            vfx_poll_interval_ms = 1u;
    }

    vfx_source = find_source(sec->Get_string("posesource"));

    if (!vfx_enabled) {
        LOG_MSG("VFX1: head tracking disabled");
        return;
    }

    if (!vfx_source->open()) {
        LOG_MSG("VFX1: pose source '%s' unavailable, head tracking off",
                vfx_source->name);
        vfx_source = VFX1_Source_None();
        vfx_enabled = false;
        return;
    }

    if (!vfx_tick_registered) {
        TIMER_AddTickHandler(&VFX1_Mouse_Tick);
        MAPPER_AddHandler(VFX1_Recenter_Mapper, MK_nothing, 0,
                          "vfx1recenter", "VFX1 recentre head");
        vfx_tick_registered = true;
    }

    vfx_last_poll_ms   = PIC_FullIndex();
    vfx_stat_window_ms = vfx_last_poll_ms;

    LOG_MSG("VFX1: head tracking on, source '%s', %u ms poll, "
            "yaw %s %.0f counts/rad, pitch %s %.0f counts/rad",
            vfx_source->name, vfx_poll_interval_ms,
            mcfg.enable_yaw ? "on" : "off",
            (double)mcfg.counts_per_radian_yaw,
            mcfg.enable_pitch ? "on" : "off",
            (double)mcfg.counts_per_radian_pitch);
}
