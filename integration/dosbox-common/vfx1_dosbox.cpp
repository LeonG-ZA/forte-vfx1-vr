/*
 * vfx1_dosbox.cpp -- a Forte VFX1 in any DOSBox derived from DOSBox 0.74.
 *
 * Glue only, shared by every emulator route (docs/ARCHITECTURE.md): js-dos's
 * classic DOSBox for the WebXR page (integration/jsdos/apply.sh) and DOSBox
 * Pure as a libretro core for the native app (integration/dosbox-pure). The
 * driver and card logic is libvfx1dev (src/vfx1dev), copied next to this file
 * as vfx1/vfx1dev.cpp by the apply scripts; this file routes DOSBox's INT 33h
 * calls and port I/O to it and takes head orientation from the host.
 *
 * The host calls vfx1Pose() with VFX1 angles: the WebXR page through a js-dos
 * backend event, the native app by looking the symbol up in the loaded core.
 */
#include "dosbox.h"
#include "inout.h"
#include "mem.h"
#include "regs.h"

#ifdef EMSCRIPTEN
#include <emscripten.h>
#define VFX1_EXPORT EMSCRIPTEN_KEEPALIVE
#elif defined(_WIN32)
#define VFX1_EXPORT __declspec(dllexport)
#else
#define VFX1_EXPORT __attribute__((visibility("default")))
#endif

#include "vfx1/vfx/vfx1dev.h"
#include "vfx1/vfx/vrtrack.h"
#include "pic.h"

/* Forte's installer offered 260h, 280h, 2A0h and 2C0h; games take the port
 * from SET VIPPORT, so that line must name this one. Titles from before it
 * write the card at 300h whatever VIPPORT says (System Shock's CD version;
 * Magic Carpet without VIPPORT), so the card answers there too. */
#define VFX1_VIP_BASE 0x260
#define VFX1_VIP_EARLY_BASE 0x300

static Vfx1Device vfx1;
static bool vfx1_ready = false;
static bool vfx1_logged[256];
static IO_ReadHandleObject vfx1_read_handler;
static IO_WriteHandleObject vfx1_write_handler;
static IO_ReadHandleObject vfx1_early_read_handler;
static IO_WriteHandleObject vfx1_early_write_handler;

static void vfx1_mem_read(void *, uint32_t addr, uint8_t *dst, size_t n) {
	for (size_t i = 0; i < n; i++) dst[i] = mem_readb((PhysPt)(addr + i));
}

static void vfx1_mem_write(void *, uint32_t addr, const uint8_t *src, size_t n) {
	for (size_t i = 0; i < n; i++) mem_writeb((PhysPt)(addr + i), src[i]);
}

static const VfxSenseMem vfx1_mem = { nullptr, vfx1_mem_read, vfx1_mem_write };

static unsigned vfx1_vip_offset(Bitu port) {
	return (unsigned)(port - (port >= VFX1_VIP_EARLY_BASE ? VFX1_VIP_EARLY_BASE : VFX1_VIP_BASE));
}

static Bitu vfx1_vip_in(Bitu port, Bitu /*iolen*/) {
	return vfx1_vip_read(&vfx1, vfx1_vip_offset(port));
}

/* Page select changes, the first few: how a title arranges its eyes. */
static void vfx1_log_psr(Bit8u before, const char *how) {
	static unsigned logged;
	if (vfx1.vip_reg[VFX1_VIP_PSR] != before && logged < 24) {
		logged++;
		LOG_MSG("VFX1: page select %02Xh -> %02Xh (%s)", before, vfx1.vip_reg[VFX1_VIP_PSR], how);
	}
}

static void vfx1_vip_out(Bitu port, Bitu val, Bitu /*iolen*/) {
	Bit8u psr = vfx1.vip_reg[VFX1_VIP_PSR];
	if (vfx1.vip_writes < 16) LOG_MSG("VFX1: VIP port %03Xh <- %02Xh", (unsigned)port, (unsigned)val);
	vfx1_vip_write(&vfx1, vfx1_vip_offset(port), (uint8_t)val);
	vfx1_log_psr(psr, "card");
}

void VFX1_Init(void) {
	/* DOSBox Pure restarts the machine, running MOUSE_Init again, when it loads
	 * a dosbox.conf; installing a port handler twice is fatal there. */
	if (vfx1_ready) {
		vfx1_read_handler.Uninstall();
		vfx1_write_handler.Uninstall();
		vfx1_early_read_handler.Uninstall();
		vfx1_early_write_handler.Uninstall();
	}
	vfx1_init(&vfx1);
	vfx1_read_handler.Install(VFX1_VIP_BASE, vfx1_vip_in, IO_MB, VFX1_VIP_PORTS);
	vfx1_write_handler.Install(VFX1_VIP_BASE, vfx1_vip_out, IO_MB, VFX1_VIP_PORTS);
	vfx1_early_read_handler.Install(VFX1_VIP_EARLY_BASE, vfx1_vip_in, IO_MB, VFX1_VIP_PORTS);
	vfx1_early_write_handler.Install(VFX1_VIP_EARLY_BASE, vfx1_vip_out, IO_MB, VFX1_VIP_PORTS);
	vfx1_ready = true;
	LOG_MSG("VFX1: driver on INT 33h prefix %02Xh, VIP card at %03Xh and %03Xh", VFX1_SENSE_MASK, VFX1_VIP_BASE,
	        VFX1_VIP_EARLY_BASE);
}

/* The head orientation the host last passed, for the serial trackers
 * (vrtrack_serial.cpp): yaw, pitch, roll in VFX1 units. */
static double vfx1_tracker_gain = 1.0;

/* Gain for the rival headsets' serial trackers (vrtrack_serial.cpp), which
 * titles such as Descent 2 turn by only a nudge per head movement. */
extern "C" VFX1_EXPORT void vfx1TrackerGain(double gain) {
	vfx1_tracker_gain = gain > 0 ? gain : 1.0;
}

/* A serial head tracker the host plugs in, whatever the title's dosbox.conf
 * says: per COM port, -1 none, 0 i-glasses, 1 CyberMaxx. Read when the
 * machine starts, so set it before loading content. */
static int vfx1_serial_tracker[4] = { -1, -1, -1, -1 };

extern "C" VFX1_EXPORT void vfx1SerialTracker(int port, int kind) {
	if (port >= 1 && port <= 4) vfx1_serial_tracker[port - 1] = kind >= 0 && kind <= 1 ? kind : -1;
}

int VFX1_SerialTracker(unsigned index) {
	return index < 4 ? vfx1_serial_tracker[index] : -1;
}

double VFX1_TrackerGain(void) {
	return vfx1_tracker_gain;
}

/* Frame-rate matching for titles that steer by head movement (vrtrack.h),
 * set per title by the host. The VFX1 driver and the serial trackers each
 * keep their own, since they are polled at their own rates. */
static int vfx1_steer_on = 0;
static int16_t vfx1_raw_head[3];
static VrSteer vfx1_steer, vfx1_serial_steer;

extern "C" VFX1_EXPORT void vfx1Steer(int on) {
	if (on != vfx1_steer_on) { vrsteer_reset(&vfx1_steer); vrsteer_reset(&vfx1_serial_steer); }
	vfx1_steer_on = on;
}

void VFX1_GetHead(Bit16s head[3]) {
	int16_t out[3];
	vrsteer_update(&vfx1_serial_steer, vfx1_raw_head, PIC_FullIndex(), vfx1_steer_on, out);
	head[0] = out[0];
	head[1] = out[1];
	head[2] = out[2];
}

/* Called first thing in INT33_Handler. True if the call was for the VFX1. */
bool VFX1_Int33(void) {
	if (!vfx1_ready) return false;
	VfxSenseRegs r;
	r.ax = reg_ax; r.bx = reg_bx; r.cx = reg_cx; r.dx = reg_dx;
	r.es = SegValue(es);
	if (reg_ah == VFX1_SENSE_MASK && ((reg_al == VFX1_GET_DEVICE_DATA) || (reg_al == VFX1_REPORT))) {
		int16_t out[3];                     /* a head poll: the head the title should see */
		vrsteer_update(&vfx1_steer, vfx1_raw_head, PIC_FullIndex(), vfx1_steer_on, out);
		Bit32u updates = vfx1.pose_updates;
		vfx1_set_head(&vfx1, out[0], out[1], out[2]);
		vfx1.pose_updates = updates;
	}
	Bit16u mode_before = vfx1.video_mode;
	Bit8u psr_before = vfx1.vip_reg[VFX1_VIP_PSR];
	if (!vfx1_int33(&vfx1, &r, &vfx1_mem)) return false;
	unsigned fn = reg_ax & 0xFF;
	if (reg_ah == VFX1_SENSE_MASK && fn == VFX1_SET_VIDEO_PAGE)
		LOG_MSG("VFX1: set video page BX=%04X CX=%04X", reg_bx, reg_cx);
	vfx1_log_psr(psr_before, "driver");
	/* every change of the headset's video mode: how a title asks for stereo */
	if (reg_ah == VFX1_SENSE_MASK && fn == VFX1_SET_DEVICE_DATA && vfx1.video_mode != mode_before)
		LOG_MSG("VFX1: video mode %u -> %u (BX=%04X CX=%04X)", mode_before, vfx1.video_mode, reg_bx, reg_cx);
	if ((reg_ah == VFX1_SENSE_MASK) && !vfx1_logged[fn]) {
		vfx1_logged[fn] = true;
		LOG_MSG("VFX1: first Sense call %02Xh (BX=%04X CX=%04X) -> AX=%04X", fn, reg_bx, reg_cx, r.ax);
	}
	reg_ax = r.ax; reg_bx = r.bx; reg_cx = r.cx; reg_dx = r.dx;
	return true;
}

extern "C" VFX1_EXPORT void vfx1Pose(int yaw, int pitch, int roll,
                                              int puckPitch, int puckRoll, int buttons) {
	vfx1_raw_head[0] = (int16_t)yaw;
	vfx1_raw_head[1] = (int16_t)pitch;
	vfx1_raw_head[2] = (int16_t)roll;
	if (!vfx1_steer_on) vfx1_set_head(&vfx1, (int16_t)yaw, (int16_t)pitch, (int16_t)roll);
	else vfx1.pose_updates++;
	vfx1_set_puck(&vfx1, (int16_t)puckPitch, (int16_t)puckRoll, (uint8_t)buttons);
}

/* Diagnostics for the page: packed as a small array in wasm memory. */
extern "C" VFX1_EXPORT uint32_t* vfx1Stats(void) {
	static uint32_t s[9];
	s[0] = vfx1.pose_updates;
	s[1] = vfx1.psr_writes;
	s[2] = vfx1.vip_reg[VFX1_VIP_PSR];
	s[3] = vfx1.calls[VFX1_DRIVER_STATUS];
	s[4] = vfx1.calls[VFX1_GET_DEVICE_DATA];
	s[5] = vfx1.calls[VFX1_REPORT];
	s[6] = vfx1.calls[VFX1_SET_ZERO];
	s[7] = vfx1.video_mode;
	s[8] = vfx1.vip_writes;
	return s;
}
