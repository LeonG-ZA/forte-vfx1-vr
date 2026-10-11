/*
 * vfx1dev.h -- an emulated Forte VFX1: the VFX1.COM "Sense" interface on
 * INT 33h and the VIP card's I/O registers.
 *
 * Native VFX1 games do not read the head tracker from hardware. They call
 * Forte's driver through INT 33h, which it shares with the mouse, using an
 * AH value from 60h to 70h that the mouse never uses. They also program the
 * VIP ISA card directly to flip stereo pages and download the palette. This
 * library answers both, so an emulator only has to route INT 33h calls and
 * port I/O here and feed in head orientation.
 *
 * Interface as documented by Forte's VFX1 SDK 2.13 (VFX1.H, VFX1REGS.C,
 * EXAMPLE1.C) and as used by Descent (Parallax's released source). See
 * docs/FINDINGS.md Q1, Q2 and Q13.
 *
 * Plain C99, no emulator types, no allocation. Not thread safe: call from the
 * emulation thread.
 */
#ifndef VFX_VFX1DEV_H
#define VFX_VFX1DEV_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The AH prefix this driver answers on. Games scan 60h..70h for it. */
#define VFX1_SENSE_MASK        0x60u
#define VFX1_SENSE_MIN         0x60u
#define VFX1_SENSE_MAX         0x70u

/* Sense functions, the AL value. */
#define VFX1_RESET_DEVICE      0x00u
#define VFX1_GET_DATA_SIZES    0x01u
#define VFX1_SET_DATA_SIZE     0x02u
#define VFX1_GET_CONFIGURATION 0x03u
#define VFX1_SET_CONFIGURATION 0x04u
#define VFX1_REPORT            0x05u
#define VFX1_CONTINUE          0x06u
#define VFX1_RESET_ZERO        0x07u
#define VFX1_SET_ZERO          0x08u
#define VFX1_TEST_DEVICES      0x09u
#define VFX1_GET_DEVICE_DATA   0x70u
#define VFX1_SET_DEVICE_DATA   0x71u
#define VFX1_DRIVER_STATUS     0x7Fu
#define VFX1_SET_VIDEO_PAGE    0x80u
#define VFX1_SET_CALLBACK      0x90u

/* Device classes, BH in device calls. */
#define VFX1_CLASS_HMS         2u   /* head-mounted system: the display */
#define VFX1_CLASS_HTD         3u   /* head tracker */
#define VFX1_CLASS_HPD         4u   /* hand-held pointer: the CyberPuck */

/* Configuration packet types. */
#define VFX1_PKT_NULL          0u
#define VFX1_PKT_SYSI          1u
#define VFX1_PKT_VNDI          2u
#define VFX1_PKT_SYSC          3u
#define VFX1_PKT_HMS           4u
#define VFX1_PKT_SVM           5u
#define VFX1_PKT_HTD           6u
#define VFX1_PKT_HPD           7u

/* VIP card: port offsets from the base, and indexed registers. */
#define VFX1_VIP_AIR           0x0u
#define VFX1_VIP_CSR           0x1u
#define VFX1_VIP_PORTS         16u
#define VFX1_VIP_PSR           4u
#define VFX1_VIP_VRR           5u
#define VFX1_VIP_REGS          16u

/* Angles are 16-bit, 65536 to a full turn. Very likely what the real driver
 * reported (Descent treats them so; Forte's Win95 SDK says the same range
 * spans -179..180 degrees), not confirmed against hardware. */
#define VFX1_UNITS_PER_TURN    65536.0

typedef struct VfxSenseRegs {
    uint16_t ax, bx, cx, dx;
    uint16_t es;
} VfxSenseRegs;

/* Access to emulated memory at a 20-bit real-mode address. */
typedef struct VfxSenseMem {
    void *user;
    void (*read)(void *user, uint32_t addr, uint8_t *dst, size_t n);
    void (*write)(void *user, uint32_t addr, const uint8_t *src, size_t n);
} VfxSenseMem;

typedef struct Vfx1Device {
    /* head tracker, raw orientation and the zero set by SET_ZERO */
    int16_t head[3];            /* yaw, pitch, roll */
    int16_t head_zero[3];
    /* CyberPuck: pitch, roll and buttons; its yaw is inactive */
    int16_t puck[3];
    uint8_t puck_buttons;
    uint8_t htd_enabled, hpd_enabled;  /* any of the device's data enabled */
    /* each axis's data-enable bit, as SET_CONFIGURATION last set it: head
     * yaw, pitch, roll, puck yaw, pitch, roll, puck buttons */
    uint8_t axis_enabled[7];

    uint16_t video_mode;        /* SET_DEVICE_DATA: 0 mono, 1 multi-buffer, 2 line-seq */
    uint8_t hms_flag7;          /* HMS flags bit 7, from an SVM packet */
    uint16_t video_page;        /* SET_VIDEO_PAGE */

    /* VIP card */
    uint8_t vip_air;
    uint8_t vip_reg[VFX1_VIP_REGS];
    uint8_t vip_port[VFX1_VIP_PORTS];   /* last byte written to each port */
    uint32_t psr_writes;
    uint32_t vip_writes;        /* any port */

    /* how often each Sense function was called, for diagnostics */
    uint32_t calls[256];
    uint32_t pose_updates;
} Vfx1Device;

void vfx1_init(Vfx1Device *dev);

/* Head orientation, in VFX1 units. Positive yaw is left, pitch up, roll
 * tilting right; rotations applied yaw, then pitch, then roll. */
void vfx1_set_head(Vfx1Device *dev, int16_t yaw, int16_t pitch, int16_t roll);
void vfx1_set_puck(Vfx1Device *dev, int16_t pitch, int16_t roll, uint8_t buttons);

/* Radians to VFX1 units, wrapping into int16. */
int16_t vfx1_angle_from_radians(double radians);

/* Handles an INT 33h call. Returns 1 if it was a Sense call and the registers
 * now hold the result, 0 if it belongs to the mouse driver. `mem` may be NULL
 * if the caller cannot provide memory; buffer calls then fail. */
int vfx1_int33(Vfx1Device *dev, VfxSenseRegs *regs, const VfxSenseMem *mem);

/* The configuration packets GET_CONFIGURATION returns, ending in a NULL
 * packet. Returns the byte count, or 0 if `cap` is too small. */
size_t vfx1_build_config(const Vfx1Device *dev, uint8_t *buf, size_t cap);

/* Report data for the enabled devices, head tracker first: 3 shorts, then
 * CyberPuck: 3 shorts and a button byte. Returns the byte count. */
size_t vfx1_build_report(const Vfx1Device *dev, uint8_t *buf, size_t cap);

/* VIP card port I/O. `offset` is the port minus the base, 0..15. */
void vfx1_vip_write(Vfx1Device *dev, unsigned offset, uint8_t value);
uint8_t vfx1_vip_read(const Vfx1Device *dev, unsigned offset);

/* Pages the card shows, from PSR: left eye in the high nibble. */
unsigned vfx1_left_page(const Vfx1Device *dev);
unsigned vfx1_right_page(const Vfx1Device *dev);

#ifdef __cplusplus
}
#endif

#endif /* VFX_VFX1DEV_H */
