/* test_vfx1dev.c -- the emulated VFX1 driver and card, against the calling
 * conventions in Forte's SDK (VFX1REGS.C, EXAMPLE1.C) and Descent's use. */
#include "vfxtest.h"
#include "vfx/vfx1dev.h"

#include <string.h>

static uint8_t ram[0x100000];

static void ram_read(void *user, uint32_t addr, uint8_t *dst, size_t n)
{
    (void)user;
    memcpy(dst, ram + addr, n);
}

static void ram_write(void *user, uint32_t addr, const uint8_t *src, size_t n)
{
    (void)user;
    memcpy(ram + addr, src, n);
}

static const VfxSenseMem MEM = { NULL, ram_read, ram_write };

static VfxSenseRegs call(Vfx1Device *d, unsigned ax, unsigned bx, unsigned cx, int *handled)
{
    VfxSenseRegs r;
    memset(&r, 0, sizeof r);
    r.ax = (uint16_t)ax; r.bx = (uint16_t)bx; r.cx = (uint16_t)cx;
    r.es = 0x2000; r.dx = 0x0100;                 /* buffer at 20100h */
    *handled = vfx1_int33(d, &r, &MEM);
    return r;
}

static int16_t s16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }

/* SenseStatus from VFX1REGS.C, verbatim in logic. */
static unsigned sense_status(Vfx1Device *d)
{
    unsigned i;
    for (i = VFX1_SENSE_MIN; i <= VFX1_SENSE_MAX; i++) {
        int h;
        unsigned result = i | (VFX1_DRIVER_STATUS << 8);
        VfxSenseRegs r = call(d, VFX1_DRIVER_STATUS | (i << 8), 0, 0, &h);
        if (r.ax == result) return (VFX1_DRIVER_STATUS | (i << 8)) & 0xFF00u;
    }
    return 0;
}

static void test_detection(void)
{
    Vfx1Device d;
    int h;
    vfx1_init(&d);
    CHECK(sense_status(&d) == (VFX1_SENSE_MASK << 8));

    /* The handshake VFX1.COM 2.14 gives with the status: CX + 1, bit 8 set. */
    {
        VfxSenseRegs r = call(&d, 0x607F, 0x1234, 0, &h);
        CHECK(r.ax == 0x7F60 && r.bx == 0 && r.cx == 0x101);
        r = call(&d, 0x607F, 0, 0x1FF, &h);
        CHECK(r.cx == 0x100);
    }
    /* Set video page writes the VIP's page select register, as VFX1.COM does. */
    {
        call(&d, 0x6080, 0x0201, 0x01, &h);
        CHECK(vfx1_left_page(&d) == 0 && vfx1_right_page(&d) == 1);
    }

    /* Mouse calls pass through untouched. */
    {
        VfxSenseRegs r = call(&d, 0x0003, 0, 0, &h);
        CHECK(h == 0 && r.ax == 0x0003);
        r = call(&d, 0x000B, 0, 0, &h);
        CHECK(h == 0);
    }
    /* Other prefixes in the Sense range are swallowed without answering. */
    {
        VfxSenseRegs r = call(&d, 0x657F, 0, 0, &h);
        CHECK(h == 1 && r.ax == 0x657F);
    }
}

static void test_device_data(void)
{
    Vfx1Device d;
    int h;
    VfxSenseRegs r;
    vfx1_init(&d);
    vfx1_set_head(&d, 1000, -2000, 300);

    r = call(&d, 0x6070, 1 | (VFX1_CLASS_HTD << 8), 0, &h);
    CHECK(h == 1);
    CHECK((int16_t)r.bx == 1000 && (int16_t)r.cx == -2000 && (int16_t)r.dx == 300);
    CHECK((r.ax >> 8) == 0);                    /* AH: no error, what Descent checks */

    vfx1_set_puck(&d, 111, -222, 0x05);
    r = call(&d, 0x6070, 1 | (VFX1_CLASS_HPD << 8), 0, &h);
    CHECK((int16_t)r.cx == 111 && (int16_t)r.dx == -222 && (r.ax & 0xFF) == 0x05 && (r.ax >> 8) == 0);

    r = call(&d, 0x6070, 1 | (VFX1_CLASS_HMS << 8), 0, &h);
    CHECK((r.ax >> 8) != 0);                    /* the display has no angles */
}

static void test_zero(void)
{
    Vfx1Device d;
    int h;
    VfxSenseRegs r;
    vfx1_init(&d);
    vfx1_set_head(&d, 30000, 100, -50);
    r = call(&d, 0x6008, 1 | (VFX1_CLASS_HTD << 8), 0, &h);
    CHECK(r.ax == 0);
    r = call(&d, 0x6070, 1 | (VFX1_CLASS_HTD << 8), 0, &h);
    CHECK((int16_t)r.bx == 0 && (int16_t)r.cx == 0 && (int16_t)r.dx == 0);

    /* After zeroing near +180 degrees, turning further wraps, not saturates. */
    vfx1_set_head(&d, -30000, 100, -50);
    r = call(&d, 0x6070, 1 | (VFX1_CLASS_HTD << 8), 0, &h);
    CHECK((int16_t)r.bx == 5536);

    r = call(&d, 0x6007, 1 | (VFX1_CLASS_HTD << 8), 0, &h);
    r = call(&d, 0x6070, 1 | (VFX1_CLASS_HTD << 8), 0, &h);
    CHECK((int16_t)r.bx == -30000);
}

static void test_configuration_walk(void)
{
    Vfx1Device d;
    int h, htd = 0, hpd = 0, hms = 0, puck_named = 0, ended = 0;
    unsigned i, count;
    VfxSenseRegs r;
    const uint8_t *buf = ram + 0x20100;

    vfx1_init(&d);
    memset(ram, 0xEE, sizeof ram);
    r = call(&d, 0x6003, 0, 0, &h);
    CHECK(r.ax == 0);
    count = r.cx;
    CHECK(count > 0 && count <= 256);            /* EXAMPLE1's buffer is 256 bytes */

    /* EXAMPLE1.C's loop over the packets. */
    for (i = 0; i < count; i += buf[i]) {
        if (buf[i] == 0) break;
        switch (buf[i + 1]) {
        case VFX1_PKT_NULL: ended = 1; i = count; break;
        case VFX1_PKT_VNDI:
            CHECK(buf[i] == 98);
            puck_named = strcmp((const char *)buf + i + 2 + 32, "VFX1 CyberPuck") == 0;
            CHECK(strcmp((const char *)buf + i + 2, "Forte Technologies, Inc.") == 0);
            break;
        case VFX1_PKT_HMS:
            hms = 1;
            /* as VFX1.COM 2.14: class left 0, device 1, attached */
            CHECK(buf[i] == 44 && buf[i + 2] == 0 && buf[i + 3] == 1 && (buf[i + 4] & 1));
            break;
        case VFX1_PKT_HTD:
            htd = 1;
            CHECK(buf[i] == 32 && buf[i + 2] == VFX1_CLASS_HTD);
            CHECK(buf[i + 3] == 1);
            CHECK(s16(buf + i + 8) == 32767 && s16(buf + i + 10) == -32766);
            break;
        case VFX1_PKT_HPD:
            hpd = 1;
            CHECK(buf[i] == 34 && buf[i + 2] == VFX1_CLASS_HPD);
            CHECK(buf[i + 32] == 3);
            break;
        default: break;
        }
        if (ended) break;
    }
    CHECK(ended && htd && hpd && hms && puck_named);
    CHECK(buf[count] == 0xEE);                   /* wrote exactly count bytes */
}

static void test_report_and_enable(void)
{
    Vfx1Device d;
    int h;
    VfxSenseRegs r;
    uint8_t *cfg = ram + 0x20100;
    unsigned i, count;

    vfx1_init(&d);
    vfx1_set_head(&d, -5, 6, -7);
    vfx1_set_puck(&d, 8, -9, 2);

    /* a word an enabled axis, as VFX1.COM: head yaw, pitch, roll, puck
     * pitch, roll (its yaw is off), buttons */
    r = call(&d, 0x6005, 0, 0, &h);
    CHECK(r.ax == 0 && r.cx == 12);
    CHECK(s16(ram + 0x20100) == -5 && s16(ram + 0x20102) == 6 && s16(ram + 0x20104) == -7);
    CHECK(s16(ram + 0x20106) == 8 && s16(ram + 0x20108) == -9 && s16(ram + 0x2010A) == 2);

    /* Clear the tracker's enable bits and hand the packets back. */
    r = call(&d, 0x6003, 0, 0, &h);
    count = r.cx;
    for (i = 0; i < count && cfg[i]; i += cfg[i]) {
        if (cfg[i + 1] == VFX1_PKT_HTD) {
            cfg[i + 14] &= 0x7F; cfg[i + 22] &= 0x7F; cfg[i + 30] &= 0x7F;
        }
    }
    r = call(&d, 0x6004, 0, 0, &h);
    CHECK(r.ax == 0 && d.htd_enabled == 0 && d.hpd_enabled == 1);
    r = call(&d, 0x6005, 0, 0, &h);
    CHECK(r.cx == 6 && s16(ram + 0x20100) == 8 && s16(ram + 0x20102) == -9);
}

static void test_video(void)
{
    Vfx1Device d;
    int h;
    VfxSenseRegs r;
    vfx1_init(&d);
    r = call(&d, 0x6071, 1 | (VFX1_CLASS_HMS << 8), 2, &h);
    CHECK(r.ax == 0 && d.video_mode == 2);
    r = call(&d, 0x6071, 1 | (VFX1_CLASS_HMS << 8), 9, &h);
    CHECK(r.ax != 0 && d.video_mode == 2);
    r = call(&d, 0x6080, 1 | (VFX1_CLASS_HMS << 8), 1, &h);
    CHECK(r.ax == 0 && d.video_page == 1);
}

static void test_vip(void)
{
    Vfx1Device d;
    vfx1_init(&d);
    /* Descent's vfx_set_page(1): left pages 2, right 3. */
    vfx1_vip_write(&d, VFX1_VIP_AIR, VFX1_VIP_PSR);
    vfx1_vip_write(&d, VFX1_VIP_CSR, 0x23);
    CHECK(vfx1_left_page(&d) == 2 && vfx1_right_page(&d) == 3 && d.psr_writes == 1 && d.vip_writes == 2);
    CHECK(vfx1_vip_read(&d, VFX1_VIP_CSR) == 0x23);

    /* VRR reads ready and ignores writes. */
    vfx1_vip_write(&d, VFX1_VIP_AIR, VFX1_VIP_VRR);
    vfx1_vip_write(&d, VFX1_VIP_CSR, 0x00);
    CHECK((vfx1_vip_read(&d, VFX1_VIP_CSR) & 1) == 1);

    /* Descent's palette download: 768 bytes to index 10 do not disturb PSR. */
    {
        int i;
        vfx1_vip_write(&d, VFX1_VIP_AIR, 9);
        vfx1_vip_write(&d, VFX1_VIP_CSR, 0);
        for (i = 0; i < 768; i++) {
            vfx1_vip_write(&d, VFX1_VIP_AIR, 10);
            vfx1_vip_write(&d, VFX1_VIP_CSR, (uint8_t)i);
        }
        CHECK(vfx1_left_page(&d) == 2 && d.psr_writes == 1);
    }
}

static void test_angles(void)
{
    const double pi = 3.14159265358979323846;
    CHECK(vfx1_angle_from_radians(0.0) == 0);
    CHECK(vfx1_angle_from_radians(pi / 2) == 16384);
    CHECK(vfx1_angle_from_radians(-pi / 2) == -16384);
    CHECK(vfx1_angle_from_radians(pi) == -32768);
    CHECK(vfx1_angle_from_radians(2 * pi + 0.001) == vfx1_angle_from_radians(0.001));
    CHECK(vfx1_angle_from_radians(-3 * pi / 2) == 16384);
}

int main(void)
{
    test_detection();
    test_device_data();
    test_zero();
    test_configuration_walk();
    test_report_and_enable();
    test_video();
    test_vip();
    test_angles();
    TEST_MAIN_END();
}
