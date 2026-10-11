/* test_vrtrack.c -- the i-glasses and CyberMaxx trackers, decoded the way
 * Descent 2's released source decodes them (main/iglasses.c, main/victor.c),
 * must turn the ship as far as the VFX1 path does: its angle, negated, in
 * turns. */
#include "vfxtest.h"
#include "vfx/vrtrack.h"

#include <string.h>

#define PI 3.14159265358979323846

static void send(VrTrack *t, const char *s, const int16_t head[3])
{
    while (*s) vrtrack_write(t, (uint8_t)*s++, head);
}

static int drain(VrTrack *t, uint8_t *buf, int cap)
{
    int n = 0;
    while (n < cap && vrtrack_read(t, &buf[n])) n++;
    return n;
}

static int16_t units(double deg) { return (int16_t)floor(deg / 360.0 * 65536.0 + 0.5); }

/* Descent 2's iglasses_read_headset, without its filter and basis. Angles in
 * turns. */
static int iglasses_decode(const uint8_t *b, double *yaw, double *pitch, double *roll)
{
    uint8_t sum = 0;
    int i;
    double x, y, z, p, r, ry, rz, rx;
    for (i = 0; i < 11; i++) sum = (uint8_t)(sum + b[i]);
    if (sum != b[11]) return 0;
    x = (short)((b[1] << 8) | b[2]) / 16384.0;
    y = (short)((b[3] << 8) | b[4]) / 16384.0;
    z = (short)((b[5] << 8) | b[6]) / 16384.0;
    p = (short)((b[7] << 8) | b[8]) * PI / 16384.0;
    r = (short)((b[9] << 8) | b[10]) * PI / 16384.0;
    ry = cos(p) * y - sin(p) * z;
    rz = sin(p) * y + cos(p) * z;
    rx = cos(r) * x - sin(r) * ry;
    *yaw = -atan2(rz, rx) * PI / 2.0;
    *pitch = -p * PI / 2.0;
    *roll = r * PI / 2.0;
    return 1;
}

static void test_iglasses(void)
{
    VrTrack t;
    uint8_t buf[64];
    int16_t head[3] = { 0, 0, 0 };
    double y, p, r;
    int n;

    vrtrack_init(&t, VRTRACK_IGLASSES);
    CHECK(vrtrack_baud(VRTRACK_IGLASSES) == 9600);

    /* the handshake Descent 2 sends */
    send(&t, "!\r", head);
    n = drain(&t, buf, 64);
    CHECK(n == 1 && buf[0] == 'O');
    send(&t, "!M1,P,B\r", head);
    n = drain(&t, buf, 64);
    CHECK(n == 1 && buf[0] == 'O');

    /* head turned 30 left, 10 up, rolled 5 right */
    head[0] = units(30); head[1] = units(10); head[2] = units(5);
    send(&t, "S", head);
    n = drain(&t, buf, 64);
    CHECK(n == 12);
    CHECK(iglasses_decode(buf, &y, &p, &r));
    /* six times the VFX1's, which Descent 2's filter divides back */
    CHECK_NEAR(y / 6, -30.0 / 360.0, 0.002);
    CHECK_NEAR(p / 6, -10.0 / 360.0, 0.002);
    CHECK_NEAR(r / 6, -5.0 / 360.0, 0.002);

    /* right and down the other way */
    head[0] = units(-20); head[1] = units(-20); head[2] = units(-15);
    send(&t, "S", head);
    CHECK(drain(&t, buf, 64) == 12 && iglasses_decode(buf, &y, &p, &r));
    CHECK_NEAR(y / 6, 20.0 / 360.0, 0.003);
    CHECK_NEAR(p / 6, 20.0 / 360.0, 0.002);
    CHECK_NEAR(r / 6, 15.0 / 360.0, 0.002);
    CHECK(t.packets == 2 && t.commands == 2);
}

/* Z.A.R.'s handshake and mode 2, as its code reads them */
static void test_iglasses_zar(void)
{
    VrTrack t;
    uint8_t buf[64];
    int16_t head[3] = { 0, 0, 0 };
    uint8_t sum = 0;
    int n, i;

    vrtrack_init(&t, VRTRACK_IGLASSES);
    send(&t, "!R\r", head);
    CHECK(drain(&t, buf, 64) == 1 && buf[0] == 'O');
    send(&t, "!V\r", head);
    n = drain(&t, buf, 64);
    CHECK(n == 60);
    CHECK(buf[0] == 'M' && buf[17] == 'P' && buf[51] == 'F' && buf[55] == '.' && buf[59] == 'O');
    CHECK(buf[52] >= '0' && buf[52] <= '9' && buf[54] == '1' && buf[56] == '0' && buf[58] == '0');
    send(&t, "!M2,P,B\r", head);
    CHECK(drain(&t, buf, 64) == 1 && buf[0] == 'O');

    /* 30 degrees left, 10 up: half the VFX1's units */
    head[0] = units(30); head[1] = units(10);
    vrtrack_write(&t, 'S', head);
    n = drain(&t, buf, 64);
    CHECK(n == 8 && buf[0] == 0xFF);
    for (i = 0; i < 7; i++) sum = (uint8_t)(sum + buf[i]);
    CHECK(sum == buf[7]);
    CHECK((short)((buf[1] << 8) | buf[2]) == units(30) / 2);
    CHECK((short)((buf[3] << 8) | buf[4]) == units(10) / 2);
}

static void test_cybermaxx(void)
{
    VrTrack t;
    uint8_t buf[64];
    int16_t head[3] = { 0, 0, 0 };
    int y, p, r;

    vrtrack_init(&t, VRTRACK_CYBERMAXX);
    CHECK(vrtrack_baud(VRTRACK_CYBERMAXX) == 19200);
    send(&t, "ACFG", head);
    CHECK(drain(&t, buf, 64) == 0);

    head[0] = units(30); head[1] = units(10); head[2] = units(5);
    send(&t, "S", head);
    CHECK(drain(&t, buf, 64) == 6);
    /* victor_read_headset_raw */
    y = (short)((buf[0] << 8) | buf[1]);
    p = -1 * ((short)((buf[2] << 8) | buf[3]) - 16384);
    r = -1 * ((short)((buf[4] << 8) | buf[5]) - 16384);
    CHECK(y == -units(30));
    CHECK(p == -units(10));
    CHECK(r == -units(5));

    /* a request mid-packet replaces the unfinished one, so packets stay whole */
    send(&t, "S", head);
    CHECK(vrtrack_read(&t, &buf[0]) && vrtrack_read(&t, &buf[1]));
    send(&t, "S", head);
    CHECK(drain(&t, buf, 64) == 6 && (short)((buf[0] << 8) | buf[1]) == -units(30));
}

static void test_steer(void)
{
    VrSteer s;
    int16_t head[3] = { 0, 0, 0 }, out[3];
    double t = 0;
    int i;

    /* off: the head passes through */
    vrsteer_reset(&s);
    head[0] = units(10);
    vrsteer_update(&s, head, 0, 0, out);
    CHECK(out[0] == units(10));

    /* polled 90 times a second: a 10 degree turn reaches the title as 30 */
    vrsteer_reset(&s);
    head[0] = 0;
    for (i = 0; i < 200; i++, t += 1000.0 / 90) vrsteer_update(&s, head, t, 1, out);
    head[0] = units(10);
    vrsteer_update(&s, head, t, 1, out);
    CHECK_NEAR(out[0], units(30), units(1));
    /* holding still changes nothing; turning back returns */
    for (i = 0; i < 20; i++, t += 1000.0 / 90) vrsteer_update(&s, head, t, 1, out);
    CHECK_NEAR(out[0], units(30), units(1));
    head[0] = 0;
    vrsteer_update(&s, head, t, 1, out);
    CHECK_NEAR(out[0], 0, units(1));

    /* at 30 a second or slower, one to one */
    vrsteer_reset(&s);
    head[1] = 0;
    for (i = 0; i < 200; i++, t += 1000.0 / 20) vrsteer_update(&s, head, t, 1, out);
    head[1] = units(10);
    vrsteer_update(&s, head, t, 1, out);
    CHECK_NEAR(out[1], units(10), 2);
}

static void test_scale(void)
{
    int16_t head[3] = { 0, 0, 0 }, out[3];
    head[0] = units(10); head[1] = units(10); head[2] = units(-10);
    vrtrack_scale_head(head, 8, out);
    CHECK_NEAR(out[0], units(80), 8); CHECK_NEAR(out[1], units(80), 8); CHECK_NEAR(out[2], units(-80), 8);
    head[0] = units(30); head[1] = units(30);
    vrtrack_scale_head(head, 8, out);                 /* 240 wraps to -120; pitch stops */
    CHECK_NEAR(out[0], units(-120), 8); CHECK_NEAR(out[1], units(170), 8);
}

int main(void)
{
    test_iglasses();
    test_cybermaxx();
    test_iglasses_zar();
    test_scale();
    test_steer();
    TEST_MAIN_END();
}
