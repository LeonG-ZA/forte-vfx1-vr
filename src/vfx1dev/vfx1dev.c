/*
 * vfx1dev.c -- see include/vfx/vfx1dev.h.
 */
#include "vfx/vfx1dev.h"

#include <math.h>
#include <string.h>

#define MAX_CONFIG 256u
#define MAX_REPORT 64u

/* Sense error codes are not documented in the SDK beyond "nonzero". */
#define ERR_OK        0u
#define ERR_FAILED    1u

void vfx1_init(Vfx1Device *dev)
{
    memset(dev, 0, sizeof *dev);
    /* Enabled from the start: the SDK example enables them explicitly, but a
     * title that skips SET_CONFIGURATION should still get data. */
    dev->htd_enabled = 1;
    dev->hpd_enabled = 1;
    /* everything the packets offer, the puck's yaw aside, which it has not */
    dev->axis_enabled[0] = dev->axis_enabled[1] = dev->axis_enabled[2] = 1;
    dev->axis_enabled[3] = 0;
    dev->axis_enabled[4] = dev->axis_enabled[5] = dev->axis_enabled[6] = 1;
    dev->vip_reg[VFX1_VIP_VRR] = 0x01;   /* VIPR: processor ready */
}

void vfx1_set_head(Vfx1Device *dev, int16_t yaw, int16_t pitch, int16_t roll)
{
    dev->head[0] = yaw;
    dev->head[1] = pitch;
    dev->head[2] = roll;
    dev->pose_updates++;
}

void vfx1_set_puck(Vfx1Device *dev, int16_t pitch, int16_t roll, uint8_t buttons)
{
    dev->puck[0] = 0;
    dev->puck[1] = pitch;
    dev->puck[2] = roll;
    dev->puck_buttons = buttons;
}

int16_t vfx1_angle_from_radians(double radians)
{
    double units = floor(radians / (2.0 * 3.14159265358979323846) * VFX1_UNITS_PER_TURN + 0.5);
    long v = (long)fmod(units, VFX1_UNITS_PER_TURN);
    if (v < 0) v += 65536L;
    if (v >= 32768L) v -= 65536L;
    return (int16_t)v;
}

static int16_t wrap16(int32_t v)
{
    uint16_t u = (uint16_t)(v & 0xFFFF);
    return (int16_t)(u >= 0x8000u ? (int32_t)u - 65536 : (int32_t)u);
}

static int16_t head_axis(const Vfx1Device *dev, unsigned i)
{
    return wrap16((int32_t)dev->head[i] - (int32_t)dev->head_zero[i]);
}

/* ---------------------------------------------------------------- packets */

typedef struct Out {
    uint8_t *buf;
    size_t cap, n;
    int overflow;
} Out;

static void put8(Out *o, unsigned v)
{
    if (o->n < o->cap) o->buf[o->n] = (uint8_t)(v & 0xFFu);
    else o->overflow = 1;
    o->n++;
}

static void put16(Out *o, int32_t v)
{
    put8(o, (unsigned)(v & 0xFF));
    put8(o, (unsigned)((v >> 8) & 0xFF));
}

static void putstr(Out *o, const char *s, size_t field)
{
    size_t len = strlen(s), i;
    for (i = 0; i < field; i++) put8(o, i < len ? (unsigned char)s[i] : 0u);
}

/* Rotation coordinate descriptor: max, min, steps, flags, pad. Flags: wrp
 * 02h (wraps), rdg 08h (data generated), den 80h (data enabled). */
static void rtcd(Out *o, int32_t maxv, int32_t minv, int32_t steps, unsigned flags, int enabled)
{
    put16(o, maxv);
    put16(o, minv);
    put16(o, steps);
    put8(o, flags | (enabled ? 0x80u : 0u));
    put8(o, 0);
}

/*
 * The packets as Forte's VFX1.COM 2.14 builds them (its routine at 36F9h, and
 * the order its GET_CONFIGURATION handler at 154Bh copies them in): SYSI,
 * VNDI, HMS, HTD, HPD, NULL. Titles check more of this than the SDK example
 * does: Z.A.R. wants the system name "SENS" and devices numbered 1, so every
 * byte the driver sets is copied here. Only the enable bits and the current
 * video mode change at run time; the real driver also starts with the
 * trackers disabled, where this one starts them enabled so a title that skips
 * SET_CONFIGURATION still gets data.
 */
size_t vfx1_build_config(const Vfx1Device *dev, uint8_t *buf, size_t cap)
{
    Out o = { buf, cap, 0, 0 };
    int i;

    /* SYSI, 15 bytes: system name "SENS"; the other values' meanings are not known */
    put8(&o, 15); put8(&o, VFX1_PKT_SYSI);
    put8(&o, 0); put8(&o, 0);               /* dcls, dnum */
    put8(&o, 0);
    put8(&o, 1); put8(&o, 0xFFu);
    putstr(&o, "SENS", 4);
    put16(&o, 0x100);
    put16(&o, 0);

    /* VNDI, 98 bytes. The SDK example checks the product name to decide
     * whether to enable the tracker, so it must read exactly "VFX1 CyberPuck". */
    put8(&o, 98); put8(&o, VFX1_PKT_VNDI);
    putstr(&o, "Forte Technologies, Inc.", 32);
    putstr(&o, "VFX1 CyberPuck", 32);
    putstr(&o, "0", 16);                    /* a number the driver formats, 0 at load */
    putstr(&o, "000B", 16);

    /* HMS, the display, 44 bytes */
    put8(&o, 44); put8(&o, VFX1_PKT_HMS);
    put8(&o, 0); put8(&o, 1);               /* dcls (the driver leaves it 0), dnum */
    /* hmsf: the driver sets bit 0 from the hardware's status, and Z.A.R.
     * wants it ("attached and working"); bit 7 comes from an SVM packet */
    put8(&o, 0x01u | (dev->hms_flag7 ? 0x80u : 0u));
    put8(&o, 0); put8(&o, 0); put8(&o, 0);
    put8(&o, 0x3Du);                        /* audio out, stereo audio, colour, stereo image, tracker */
    put8(&o, 1);                            /* dtyp */
    put16(&o, 0x2FC3);                      /* vmag */
    put16(&o, 157);                         /* hfov */
    put16(&o, 123);                         /* vfov */
    put8(&o, 124);                          /* ipd */
    put8(&o, 0); put8(&o, 0);               /* hoao, voao */
    put8(&o, 0xD8u);                        /* oar */
    put8(&o, 60);                           /* mrr */
    put8(&o, dev->video_mode);              /* cvm */
    put8(&o, 0);
    put8(&o, 2);                            /* vmsp */
    put16(&o, 0x1300);
    put16(&o, 0x0101);
    for (i = 0; i < 16; i++) put8(&o, 0);

    /* HTD, the head tracker, 32 bytes. Yaw wraps; pitch and roll span about
     * 70 degrees each way at 65536 units a turn. */
    put8(&o, 32); put8(&o, VFX1_PKT_HTD);
    put8(&o, VFX1_CLASS_HTD); put8(&o, 1);
    put8(&o, dev->htd_enabled ? 0u : 0x80u); /* htf: ddsbl */
    put8(&o, 0); put8(&o, 0); put8(&o, 0);
    rtcd(&o, 0x7FFF, -0x7FFE, 0xFFFE, 0x0Au, dev->axis_enabled[0]);
    rtcd(&o, 0x31CE, -0x31CE, 0x639D, 0x08u, dev->axis_enabled[1]);
    rtcd(&o, 0x31CE, -0x31CE, 0x639D, 0x08u, dev->axis_enabled[2]);

    /* HPD, the CyberPuck, 34 bytes: no yaw, pitch and roll, three buttons */
    put8(&o, 34); put8(&o, VFX1_PKT_HPD);
    put8(&o, VFX1_CLASS_HPD); put8(&o, 1);
    put8(&o, dev->hpd_enabled ? 0u : 0x40u); /* hhpf: ddsbl */
    put8(&o, 0); put8(&o, 0); put8(&o, 0);
    rtcd(&o, 0, 0, 0, 0, dev->axis_enabled[3]);
    rtcd(&o, 0x31CE, -0x31CE, 0x1FF, 0x08u, dev->axis_enabled[4]);
    rtcd(&o, 0x31CE, -0x31CE, 0x1FF, 0x08u, dev->axis_enabled[5]);
    put8(&o, 3);                            /* nbtn */
    put8(&o, dev->axis_enabled[6] ? 0x80u : 0u);    /* btcd: den */

    put8(&o, 2); put8(&o, VFX1_PKT_NULL);

    return o.overflow ? 0 : o.n;
}

/* As VFX1.COM 2.14's REPORT (13E8h): a 16-bit word for each enabled axis,
 * head yaw, pitch and roll, then the puck's yaw, pitch and roll, then the
 * buttons as a word, each in or out by its own enable bit. A title works out
 * where each value is from the bits it set (Z.A.R. leaves the puck's yaw
 * out). */
size_t vfx1_build_report(const Vfx1Device *dev, uint8_t *buf, size_t cap)
{
    Out o = { buf, cap, 0, 0 };
    int16_t value[7];
    int i;
    value[0] = head_axis(dev, 0);
    value[1] = head_axis(dev, 1);
    value[2] = head_axis(dev, 2);
    value[3] = dev->puck[0];
    value[4] = dev->puck[1];
    value[5] = dev->puck[2];
    value[6] = (int16_t)dev->puck_buttons;
    for (i = 0; i < 7; i++)
        if (dev->axis_enabled[i]) put16(&o, value[i]);
    return o.overflow ? 0 : o.n;
}

/* SET_CONFIGURATION: the title hands the packets back with data-enable bits
 * set or cleared. Only the enable bits are honoured. */
static void apply_config(Vfx1Device *dev, const uint8_t *buf, size_t n)
{
    size_t i = 0;
    while (i + 2 <= n) {
        unsigned size = buf[i], type = buf[i + 1];
        if (type == VFX1_PKT_NULL || size < 2 || i + size > n) break;
        if (type == VFX1_PKT_HTD && size >= 32) {
            /* den in each descriptor's flags: offsets 14, 22, 30; ddsbl in htf */
            int off = (buf[i + 4] & 0x80u) != 0;
            dev->axis_enabled[0] = (uint8_t)(!off && (buf[i + 14] & 0x80u));
            dev->axis_enabled[1] = (uint8_t)(!off && (buf[i + 22] & 0x80u));
            dev->axis_enabled[2] = (uint8_t)(!off && (buf[i + 30] & 0x80u));
            dev->htd_enabled = (uint8_t)(dev->axis_enabled[0] | dev->axis_enabled[1] | dev->axis_enabled[2]);
        } else if (type == VFX1_PKT_SVM && size >= 6) {
            /* set video mode, as VFX1.COM 2.14 reads it (1745h): byte 4 bit 7
             * into the HMS flags, byte 5 the current video mode */
            dev->hms_flag7 = (uint8_t)((buf[i + 4] & 0x80u) != 0);
            if (buf[i + 5] <= 2u) dev->video_mode = buf[i + 5];
        } else if (type == VFX1_PKT_HPD && size >= 34) {
            int off = (buf[i + 4] & 0x40u) != 0;
            dev->axis_enabled[3] = (uint8_t)(!off && (buf[i + 14] & 0x80u));
            dev->axis_enabled[4] = (uint8_t)(!off && (buf[i + 22] & 0x80u));
            dev->axis_enabled[5] = (uint8_t)(!off && (buf[i + 30] & 0x80u));
            dev->axis_enabled[6] = (uint8_t)(!off && (buf[i + 33] & 0x80u));
            dev->hpd_enabled = (uint8_t)(dev->axis_enabled[3] | dev->axis_enabled[4] | dev->axis_enabled[5] |
                                         dev->axis_enabled[6]);
        }
        i += size;
    }
}

/* ---------------------------------------------------------------- INT 33h */

static uint32_t es_dx(const VfxSenseRegs *r)
{
    return ((uint32_t)r->es << 4) + r->dx;
}

int vfx1_int33(Vfx1Device *dev, VfxSenseRegs *r, const VfxSenseMem *mem)
{
    unsigned ah = (unsigned)(r->ax >> 8), al = (unsigned)(r->ax & 0xFFu);
    unsigned cls = (unsigned)(r->bx >> 8);

    if (ah < VFX1_SENSE_MIN || ah > VFX1_SENSE_MAX) return 0;
    if (ah != VFX1_SENSE_MASK) {
        /* Another prefix in the Sense range: not us. Leaving AX unchanged is
         * what tells a scanning title to keep looking. */
        return 1;
    }
    dev->calls[al]++;

    switch (al) {
    case VFX1_DRIVER_STATUS:
        /* As VFX1.COM 2.14 answers (its handler at 1986h): the prefix in AX,
         * BX = 0, and CX + 1 with bit 8 set, a handshake Z.A.R. checks. The
         * real driver answers only when DX is 0 or a value it sets at load
         * time, and then returns DX unchanged; this one answers any DX. */
        r->ax = (uint16_t)((VFX1_DRIVER_STATUS << 8) | VFX1_SENSE_MASK);
        r->bx = 0;
        r->cx = (uint16_t)(((r->cx + 1u) | 0x100u) & 0x1FFu);
        return 1;

    case VFX1_GET_DEVICE_DATA:
        if (cls == VFX1_CLASS_HTD) {
            r->bx = (uint16_t)head_axis(dev, 0);
            r->cx = (uint16_t)head_axis(dev, 1);
            r->dx = (uint16_t)head_axis(dev, 2);
            r->ax = 0;
        } else if (cls == VFX1_CLASS_HPD) {
            r->bx = (uint16_t)dev->puck[0];
            r->cx = (uint16_t)dev->puck[1];
            r->dx = (uint16_t)dev->puck[2];
            r->ax = dev->puck_buttons;
        } else {
            r->ax = (uint16_t)(ERR_FAILED << 8);
        }
        return 1;

    case VFX1_SET_ZERO:
        if (cls == VFX1_CLASS_HTD || cls == 0xFFu) {
            memcpy(dev->head_zero, dev->head, sizeof dev->head);
            r->ax = ERR_OK;
        } else {
            r->ax = ERR_FAILED;
        }
        return 1;

    case VFX1_RESET_ZERO:
        if (cls == VFX1_CLASS_HTD || cls == 0xFFu) {
            memset(dev->head_zero, 0, sizeof dev->head_zero);
            r->ax = ERR_OK;
        } else {
            r->ax = ERR_FAILED;
        }
        return 1;

    case VFX1_SET_DEVICE_DATA:
        if (cls == VFX1_CLASS_HMS && r->cx <= 2u) {
            dev->video_mode = r->cx;
            r->ax = ERR_OK;
        } else {
            r->ax = ERR_FAILED;
        }
        return 1;

    case VFX1_SET_VIDEO_PAGE:
        /* VFX1.COM 2.14 (1A07h, 12B8h) writes CX to the VIP's page select
         * register: the left eye's page in the high nibble, the right's in
         * the low. Z.A.R. sets 01h once and then draws the eyes on
         * alternate frames. */
        dev->video_page = r->cx;
        dev->vip_reg[VFX1_VIP_PSR] = (uint8_t)r->cx;
        dev->psr_writes++;
        r->ax = ERR_OK;
        return 1;

    case VFX1_GET_CONFIGURATION: {
        uint8_t buf[MAX_CONFIG];
        size_t n = vfx1_build_config(dev, buf, sizeof buf);
        if (!mem || !mem->write || n == 0) { r->ax = ERR_FAILED; return 1; }
        mem->write(mem->user, es_dx(r), buf, n);
        r->cx = (uint16_t)n;
        r->ax = ERR_OK;
        return 1;
    }

    case VFX1_SET_CONFIGURATION: {
        uint8_t buf[MAX_CONFIG];
        if (!mem || !mem->read) { r->ax = ERR_FAILED; return 1; }
        mem->read(mem->user, es_dx(r), buf, sizeof buf);
        apply_config(dev, buf, sizeof buf);
        r->ax = ERR_OK;
        return 1;
    }

    case VFX1_REPORT: {
        uint8_t buf[MAX_REPORT];
        size_t n = vfx1_build_report(dev, buf, sizeof buf);
        if (!mem || !mem->write) { r->ax = ERR_FAILED; return 1; }
        if (n) mem->write(mem->user, es_dx(r), buf, n);
        r->cx = (uint16_t)n;
        r->ax = ERR_OK;
        return 1;
    }

    case VFX1_GET_DATA_SIZES: {
        uint8_t buf[MAX_CONFIG];
        r->cx = (uint16_t)vfx1_build_config(dev, buf, sizeof buf);
        r->dx = (uint16_t)vfx1_build_report(dev, buf, sizeof buf);
        r->ax = ERR_OK;
        return 1;
    }

    case VFX1_RESET_DEVICE:
    case VFX1_SET_DATA_SIZE:
    case VFX1_CONTINUE:
    case VFX1_TEST_DEVICES:
        r->ax = ERR_OK;
        return 1;

    case VFX1_SET_CALLBACK:     /* not emulated: titles must poll */
    default:
        r->ax = ERR_FAILED;
        return 1;
    }
}

/* ---------------------------------------------------------------- VIP card */

void vfx1_vip_write(Vfx1Device *dev, unsigned offset, uint8_t value)
{
    offset &= VFX1_VIP_PORTS - 1u;
    dev->vip_port[offset] = value;
    dev->vip_writes++;
    if (offset == VFX1_VIP_AIR) {
        dev->vip_air = value;
    } else if (offset == VFX1_VIP_CSR) {
        unsigned idx = dev->vip_air & (VFX1_VIP_REGS - 1u);
        if (idx == VFX1_VIP_VRR) return;          /* read-only */
        dev->vip_reg[idx] = value;
        if (idx == VFX1_VIP_PSR) dev->psr_writes++;
    }
}

uint8_t vfx1_vip_read(const Vfx1Device *dev, unsigned offset)
{
    offset &= VFX1_VIP_PORTS - 1u;
    if (offset == VFX1_VIP_AIR) return dev->vip_air;
    if (offset == VFX1_VIP_CSR) return dev->vip_reg[dev->vip_air & (VFX1_VIP_REGS - 1u)];
    return dev->vip_port[offset];
}

unsigned vfx1_left_page(const Vfx1Device *dev)
{
    return (unsigned)(dev->vip_reg[VFX1_VIP_PSR] >> 4);
}

unsigned vfx1_right_page(const Vfx1Device *dev)
{
    return (unsigned)(dev->vip_reg[VFX1_VIP_PSR] & 0x0Fu);
}
