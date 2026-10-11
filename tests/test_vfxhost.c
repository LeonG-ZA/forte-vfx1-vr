/* test_vfxhost.c -- the shared headset-side logic, with the checks that
 * web/test-stereo.html and web/test-reproject.html ran on the JavaScript it
 * replaces, plus the controller layout the user settled on for Descent. */
#include "vfxtest.h"
#include "vfx/vfxhost.h"

#include <string.h>

#define PI 3.14159265358979323846
#define DEG (PI / 180.0)

static VfxHostQuat axis_q(double deg, double x, double y, double z)
{
    VfxHostQuat q;
    double h = deg * DEG / 2;
    q.w = cos(h); q.x = x * sin(h); q.y = y * sin(h); q.z = z * sin(h);
    return q;
}
static VfxHostQuat yaw_q(double d) { return axis_q(d, 0, 1, 0); }
static VfxHostQuat pitch_q(double d) { return axis_q(d, 1, 0, 0); }
static VfxHostQuat roll_q(double d) { return axis_q(-d, 0, 0, 1); }    /* tilt right */

static void test_angles(void)
{
    double y, p, r;
    vfxhost_sense_angles(yaw_q(30), &y, &p, &r);
    CHECK_NEAR(y / DEG, 30, 1e-6); CHECK_NEAR(p, 0, 1e-9); CHECK_NEAR(r, 0, 1e-9);
    vfxhost_sense_angles(pitch_q(20), &y, &p, &r);
    CHECK_NEAR(p / DEG, 20, 1e-6); CHECK_NEAR(y, 0, 1e-9);
    vfxhost_sense_angles(roll_q(15), &y, &p, &r);
    CHECK_NEAR(r / DEG, 15, 1e-6);
    vfxhost_sense_angles(vfxhost_quat_mul(vfxhost_quat_mul(yaw_q(-120), pitch_q(-35)), roll_q(25)), &y, &p, &r);
    CHECK_NEAR(y / DEG, -120, 1e-4); CHECK_NEAR(p / DEG, -35, 1e-4); CHECK_NEAR(r / DEG, 25, 1e-4);

    CHECK(vfxhost_sense_units(PI / 2) == 16384);
    CHECK(vfxhost_sense_units(-PI / 2) == -16384);
    CHECK(vfxhost_sense_units(PI) == -32768);
}

static void test_tilt(void)
{
    double p, r, pp = 0, pr = 0, worst = 0;
    int d, first = 1;
    vfxhost_tilt_angles(pitch_q(30), &p, &r);
    CHECK_NEAR(p / DEG, 30, 1e-6); CHECK_NEAR(r, 0, 1e-9);
    vfxhost_tilt_angles(roll_q(20), &p, &r);
    CHECK_NEAR(r / DEG, 20, 1e-6); CHECK_NEAR(p, 0, 1e-9);
    vfxhost_tilt_angles(vfxhost_quat_mul(yaw_q(137), pitch_q(-25)), &p, &r);
    CHECK_NEAR(p / DEG, -25, 1e-6); CHECK_NEAR(r, 0, 1e-9);
    /* No jumps sweeping through pointing straight down while tilted. */
    for (d = -60; d >= -120; d--) {
        vfxhost_tilt_angles(vfxhost_quat_mul(pitch_q(d), roll_q(15)), &p, &r);
        if (!first) {
            if (fabs(p - pp) / DEG > worst) worst = fabs(p - pp) / DEG;
            if (fabs(r - pr) / DEG > worst) worst = fabs(r - pr) / DEG;
        }
        pp = p; pr = r; first = 0;
    }
    CHECK(worst < 2.0);
}

static void test_head(void)
{
    VfxHostHead h;
    int16_t y, p, r;
    memset(&h, 0, sizeof h);
    vfxhost_head_update(&h, yaw_q(40), &y, &p, &r);
    CHECK(y == 0 && p == 0 && r == 0);                  /* first pose is the reference */
    vfxhost_head_update(&h, yaw_q(40 + 90), &y, &p, &r);
    CHECK(y == 16384);
    vfxhost_head_recenter(&h);
    vfxhost_head_update(&h, yaw_q(10), &y, &p, &r);
    CHECK(y == 0);
}

/* ---- page flip: synthetic frames, as web/test-stereo.html ---- */

enum { W = 640, H = 480 };
static uint8_t frame[W * H * 3];

static void draw_page(int parity, int id, int from, int to)
{
    int py, x;
    for (py = from; py < to; py++) {
        int y = 2 * py + parity;
        for (x = 0; x < W; x++) {
            uint8_t *p = frame + (y * W + x) * 3;
            p[0] = (uint8_t)id; p[1] = x < W / 2 ? 10 : 20; p[2] = (uint8_t)py;
        }
    }
}

/* The single game frame id the eyes show, or -1 if they are mixed. */
static int shown_id(const VfxPageFlip *pf)
{
    int y, id = pf->eyes[5 * 3];
    for (y = 0; y < H / 2; y += 7)
        if (pf->eyes[(y * W + 5) * 3] != id) return -1;
    return id;
}

static void test_pageflip(void)
{
    VfxPageFlip pf;
    int id, n = 0, ok = 1;

    memset(frame, 0, sizeof frame);
    vfxhost_pageflip_init(&pf, 8);
    draw_page(0, 1, 0, H / 2); draw_page(1, 1, 0, H / 2);
    CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 1);
    CHECK(pf.eyes_w == W && pf.eyes_h == H / 2);
    CHECK(pf.eyes[(10 * W + 5) * 3 + 1] == 10 && pf.eyes[(10 * W + W - 5) * 3 + 1] == 20);

    /* one page per delivery: each shown one delivery later, whole */
    for (id = 2; id < 12; id++) {
        draw_page(id & 1, id, 0, H / 2);
        if (vfxhost_pageflip_feed(&pf, frame, W, H) == 1) {
            if (shown_id(&pf) != id - 1) ok = 0;
            n++;
        }
    }
    CHECK(n == 9 && ok);          /* the first new page only becomes pending */

    /* a page drawn across two deliveries is never shown half drawn */
    draw_page(0, 30, 0, H / 2); draw_page(1, 30, 0, H / 2);
    vfxhost_pageflip_feed(&pf, frame, W, H);
    vfxhost_pageflip_feed(&pf, frame, W, H);
    draw_page(1, 31, 0, 120);
    CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 0);
    draw_page(1, 31, 120, 240);
    CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 0);
    draw_page(0, 32, 0, H / 2);
    CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 1 && pf.shown == 1 && shown_id(&pf) == 31);

    /* an unchanged delivery completes the pending page */
    CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 1 && shown_id(&pf) == 32);
    CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 0);

    /* the left eye drawn, a pause, then the right (Descent): the page waits
     * for both */
    {
        int py, x;
        for (py = 0; py < H / 2; py++)
            for (x = 0; x < W / 2; x++) frame[((2 * py) * W + x) * 3] = 40;
        CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 0);
        CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 0);      /* the pause */
        for (py = 0; py < H / 2; py++)
            for (x = W / 2; x < W; x++) frame[((2 * py) * W + x) * 3] = 40;
        CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 0);
        CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 1 && pf.shown == 0 && shown_id(&pf) == 40);
    }

    /* a mouse cursor drawn into the other parity is not a page (System
     * Shock): a few pixels there leave the page shown as it was */
    {
        int k;
        CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 0);
        for (k = 0; k < 16; k++) frame[((2 * 50 + 1) * W + 40 + k) * 3] ^= 0x80;
        CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 0);
        CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 0 && pf.shown == 0);
    }

    /* a new palette changes both parities alike: not a page (Descent 2) */
    draw_page(1, 33, 0, H / 2);
    CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 0);
    {
        int i;
        for (i = 0; i < W * H; i++) frame[i * 3 + 1] ^= 1;
    }
    CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 0);
    CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 1 && pf.shown == 1 && shown_id(&pf) == 33);
    CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 0);

    /* both parities redrawn alike is drawing, not a palette: a page still
     * completes (Descent froze while this waited) */
    {
        int i;
        for (i = 0; i < W * H; i++) frame[i * 3] = (uint8_t)(50 + (i % W) * 7);
    }
    CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 0);
    CHECK(vfxhost_pageflip_feed(&pf, frame, W, H) == 1);

    vfxhost_pageflip_free(&pf);
}

/* ---- controls ---- */

static VfxHostHand hand(void)
{
    VfxHostHand h;
    memset(&h, 0, sizeof h);
    h.present = 1;
    h.has_orientation = 1;
    h.orientation.w = 1;
    return h;
}

static void test_controls_layout(void)
{
    VfxHostControls c;
    VfxHostControlsOut o;
    VfxHostHand l = hand(), r = hand();
    vfxhost_controls_init(&c, NULL);

    r.trigger = 1; r.grip = 1; r.stick_press = 1; r.secondary = 1;
    r.stick_x = -0.9; r.stick_y = 0.9;
    vfxhost_controls_update(&c, &l, &r, 1, 0.014, &o);
    CHECK(o.keys[VFXKEY_CTRL] && o.keys[VFXKEY_A] && o.keys[VFXKEY_SPACE] && o.keys[VFXKEY_ESCAPE]);
    CHECK(o.keys[VFXKEY_LEFT] && o.keys[VFXKEY_DOWN] && !o.keys[VFXKEY_RIGHT] && !o.keys[VFXKEY_UP]);
    CHECK(o.mouse_dx == 0 && o.mouse_dy == 0 && o.puck_buttons == 3);

    /* A is Enter, and the puck's third button */
    r = hand(); r.primary = 1;
    vfxhost_controls_update(&c, &l, &r, 1, 0.014, &o);
    CHECK(o.keys[VFXKEY_ENTER] && o.puck_buttons == 4);
    vfxhost_controls_update(&c, &l, &r, 0, 0.014, &o);
    CHECK(o.keys[VFXKEY_ENTER]);

    /* left: trigger secondary, stick forward accelerate, back reverse, sideways turn */
    r = hand(); l = hand();
    l.trigger = 1; l.stick_y = -0.8; l.stick_x = 0.8;
    vfxhost_controls_update(&c, &l, &r, 1, 0.014, &o);
    CHECK(o.keys[VFXKEY_SPACE] && o.keys[VFXKEY_A] && o.keys[VFXKEY_RIGHT] && !o.keys[VFXKEY_Z]);
    l = hand(); l.stick_y = 0.8;
    vfxhost_controls_update(&c, &l, &r, 1, 0.014, &o);
    CHECK(o.keys[VFXKEY_Z] && !o.keys[VFXKEY_A]);

    /* X recentres once per press */
    l = hand(); l.primary = 1;
    vfxhost_controls_update(&c, &l, &r, 1, 0.014, &o);
    CHECK(o.recenter == 1);
    vfxhost_controls_update(&c, &l, &r, 1, 0.014, &o);
    CHECK(o.recenter == 0);
}

static void test_controls_puck(void)
{
    VfxHostControls c;
    VfxHostControlsConfig cfg;
    VfxHostControlsOut o;
    VfxHostHand l = hand(), r = hand();
    int i, sum_y = 0, sum_x = 0;

    vfxhost_controls_default_config(&cfg);
    cfg.mouse = VFXMOUSE_PUCK;
    vfxhost_controls_init(&c, &cfg);

    r.orientation = pitch_q(10);                         /* rest pose */
    vfxhost_controls_update(&c, &l, &r, 1, 0.014, &o);
    CHECK_NEAR(o.puck_pitch, 0, 1e-9);

    r.orientation = pitch_q(10 - 20);                   /* tilted forward 20 degrees */
    vfxhost_controls_update(&c, &l, &r, 1, 0.014, &o);
    CHECK_NEAR(o.puck_pitch / DEG, -20, 1e-6);

    /* held with its front steeply down (the Quest's grip pose): tipping it
     * forward and back from there give opposite tilts */
    {
        VfxHostControls c2;
        VfxHostControlsOut o2;
        VfxHostHand r2 = hand();
        vfxhost_controls_init(&c2, &cfg);
        r2.orientation = pitch_q(-80);
        vfxhost_controls_update(&c2, &l, &r2, 1, 0.014, &o2);
        r2.orientation = vfxhost_quat_mul(pitch_q(-20), pitch_q(-80));
        vfxhost_controls_update(&c2, &l, &r2, 1, 0.014, &o2);
        CHECK_NEAR(o2.puck_pitch / DEG, -20, 1e-6);
        r2.orientation = vfxhost_quat_mul(pitch_q(20), pitch_q(-80));
        vfxhost_controls_update(&c2, &l, &r2, 1, 0.014, &o2);
        CHECK_NEAR(o2.puck_pitch / DEG, 20, 1e-6);
    }

    /* inside the 12 degree dead zone: still */
    r.orientation = pitch_q(10 - 8);
    for (i = 0; i < 72; i++) { vfxhost_controls_update(&c, &l, &r, 1, 1.0 / 72, &o); sum_y += o.mouse_dy; }
    CHECK(sum_y == 0);

    /* full forward tilt for a second: about puck_rate counts up (negative) */
    r.orientation = pitch_q(10 - 60);
    sum_y = 0;
    for (i = 0; i < 72; i++) { vfxhost_controls_update(&c, &l, &r, 1, 1.0 / 72, &o); sum_y += o.mouse_dy; }
    CHECK(sum_y <= -690 && sum_y >= -700);

    /* trigger is a mouse button, not Ctrl, when the puck is the mouse */
    r.trigger = 1;
    vfxhost_controls_update(&c, &l, &r, 1, 0.014, &o);
    CHECK(o.mouse_buttons == 1 && !o.keys[VFXKEY_CTRL]);

    /* in menus only the stronger axis moves */
    r.trigger = 0;
    r.orientation = vfxhost_quat_mul(pitch_q(10 - 20), roll_q(40));
    sum_x = sum_y = 0;
    for (i = 0; i < 72; i++) { vfxhost_controls_update(&c, &l, &r, 0, 1.0 / 72, &o); sum_x += o.mouse_dx; sum_y += o.mouse_dy; }
    CHECK(sum_x > 500 && sum_y == 0);
}

static void test_interlace(void)
{
    /* 2x4 frame: row r is filled with value 10*r */
    uint8_t rgb[2 * 4 * 3], eyes[2 * 2 * 2 * 3];
    int r, i;
    for (r = 0; r < 4; r++) for (i = 0; i < 2 * 3; i++) rgb[r * 6 + i] = (uint8_t)(10 * r);
    vfxhost_interlace_split(rgb, 2, 4, eyes);
    /* eyes row 0: left = frame row 0, right = frame row 1; row 1: rows 2 and 3 */
    CHECK(eyes[0] == 0 && eyes[5] == 0 && eyes[6] == 10 && eyes[11] == 10);
    CHECK(eyes[12] == 20 && eyes[17] == 20 && eyes[18] == 30 && eyes[23] == 30);
}

static uint8_t texture(int x, int y) { return (uint8_t)((x * 37 + y * 91 + ((x * y) % 13) * 17) & 0xFF); }

static void test_stereo_detect(void)
{
    enum { SW = 320, SH = 200 };
    static uint8_t img[SW * SH * 3], eyes[640 * 240 * 3];
    VfxStereoGate g = { 0, 0, 3, 3 };
    int x, y, i;

    /* flat: a smooth picture; neighbouring lines alike */
    for (y = 0; y < SH; y++) for (x = 0; x < SW; x++) for (i = 0; i < 3; i++) img[(y * SW + x) * 3 + i] = (uint8_t)(x / 2 + y);
    CHECK(vfxhost_lines_stereo_score(img, SW, SH) < 1.0);
    CHECK(vfxhost_lines_alike_score(img, SW, SH) > 1.05);
    CHECK(vfxhost_lines_look_stereo(img, SW, SH, 1, 1.0, NULL, NULL) == 0);
    /* the eyes the same picture (Z.A.R.): each right line copies the left
     * line beside it. Stereo, whatever the line score says. */
    for (y = 0; y < SH; y++) for (x = 0; x < SW; x++) for (i = 0; i < 3; i++)
        img[(y * SW + x) * 3 + i] = (uint8_t)(x / 2 + (y & ~1) + ((y & 1) && (x % 5 == 0)));
    CHECK(vfxhost_lines_alike_score(img, SW, SH) < 0.9);
    CHECK(vfxhost_lines_look_stereo(img, SW, SH, 0, 1.0, NULL, NULL) == 1);
    /* interlaced: even lines one picture, odd lines another */
    for (y = 0; y < SH; y++) for (x = 0; x < SW; x++) for (i = 0; i < 3; i++)
        img[(y * SW + x) * 3 + i] = (y & 1) ? texture(x + 7, y / 2) : (uint8_t)(x / 2 + y);
    CHECK(vfxhost_lines_stereo_score(img, SW, SH) > 1.0);
    CHECK(vfxhost_lines_look_stereo(img, SW, SH, 0, 1.0, NULL, NULL) == 1);

    /* in between (eyes nearly alike, as Z.A.R. draws them): keep the current
     * state. Lines two apart differ by 10, neighbours by 9: a score of 0.9
     * (50 lines, to stay within a byte). */
    for (y = 0; y < 50; y++) for (x = 0; x < SW; x++) for (i = 0; i < 3; i++)
        img[(y * SW + x) * 3 + i] = (uint8_t)((y / 2) * 10 + (y & 1) * 9);
    CHECK_NEAR(vfxhost_lines_stereo_score(img, SW, 50), 0.9, 0.05);
    CHECK(vfxhost_lines_look_stereo(img, SW, 50, 1, 1.0, NULL, NULL) == 1);
    CHECK(vfxhost_lines_look_stereo(img, SW, 50, 0, 1.0, NULL, NULL) == 0);
    /* a higher flat-screen threshold calls that score flat (0.9 < 0.8 x 1.2) */
    CHECK(vfxhost_lines_look_stereo(img, SW, 50, 1, 1.2, NULL, NULL) == 0);

    /* pages: one detailed wide picture continuing across the middle is flat... */
    {
        uint32_t r = 2463534242u;                    /* xorshift noise */
        for (y = 0; y < 240; y++) for (x = 0; x < 640; x++) {
            r ^= r << 13; r ^= r >> 17; r ^= r << 5;
            for (i = 0; i < 3; i++) eyes[(y * 640 + x) * 3 + i] = (uint8_t)(r >> 24);
        }
    }
    CHECK(!vfxhost_pages_look_stereo(eyes, 640, 240));
    /* ...the same view in both eyes, offset a little, is stereo */
    for (y = 0; y < 240; y++) for (x = 0; x < 640; x++) for (i = 0; i < 3; i++)
        eyes[(y * 640 + x) * 3 + i] = (uint8_t)(((x % 320) + (x >= 320 ? 6 : 0)) / 2 + ((((x % 320) + (x >= 320 ? 6 : 0)) / 40 + y / 30) & 1) * 80);
    CHECK(vfxhost_pages_look_stereo(eyes, 640, 240));
    /* ...and so is a dark screen with too little detail to tell */
    memset(eyes, 3, sizeof eyes);
    CHECK(vfxhost_pages_look_stereo(eyes, 640, 240));

    /* the gate switches only after three agreeing frames */
    CHECK(vfxhost_stereo_gate(&g, 1) == 0 && vfxhost_stereo_gate(&g, 1) == 0 && vfxhost_stereo_gate(&g, 1) == 1);
    CHECK(vfxhost_stereo_gate(&g, 0) == 1 && vfxhost_stereo_gate(&g, 1) == 1);
}

static void test_controls_point(void)
{
    VfxHostControls c;
    VfxHostControlsConfig cfg;
    VfxHostControlsOut o;
    VfxHostHand l = hand(), r = hand();
    double yaw, pitch;
    int i, sum_x = 0, sum_y = 0;

    vfxhost_point_angles(vfxhost_quat_mul(yaw_q(30), pitch_q(-20)), &yaw, &pitch);
    CHECK_NEAR(yaw / DEG, 30, 1e-6);
    CHECK_NEAR(pitch / DEG, -20, 1e-6);

    vfxhost_controls_default_config(&cfg);
    cfg.mouse = VFXMOUSE_POINT;
    cfg.mouse_hand = VFXPUCK_LEFT;
    vfxhost_controls_init(&c, &cfg);

    /* the first update only takes the reference */
    l.orientation = yaw_q(10);
    vfxhost_controls_update(&c, &l, &r, 0, 0.014, &o);
    CHECK(o.mouse_dx == 0 && o.mouse_dy == 0);

    /* turning right 0.1 rad in small steps: +100 counts; pointing down: +dy */
    for (i = 1; i <= 50; i++) {
        l.orientation = vfxhost_quat_mul(yaw_q(10 - (0.1 / DEG) * i / 50), pitch_q(-(0.05 / DEG) * i / 50));
        vfxhost_controls_update(&c, &l, &r, 0, 0.014, &o);
        sum_x += o.mouse_dx; sum_y += o.mouse_dy;
    }
    CHECK(sum_x >= 99 && sum_x <= 101);
    CHECK(sum_y >= 49 && sum_y <= 51);

    /* left trigger is the left button, not Space; the right hand keeps Ctrl */
    l.trigger = 1; r.trigger = 1;
    vfxhost_controls_update(&c, &l, &r, 0, 0.014, &o);
    CHECK(o.mouse_buttons == 1 && !o.keys[VFXKEY_SPACE] && o.keys[VFXKEY_CTRL]);

    /* after a rest, a big jump in pose does not move the mouse */
    vfxhost_controls_rest(&c);
    l.orientation = yaw_q(-80);
    vfxhost_controls_update(&c, &l, &r, 0, 0.014, &o);
    CHECK(o.mouse_dx == 0 && o.mouse_dy == 0);
}

int main(void)
{
    test_angles();
    test_tilt();
    test_head();
    test_pageflip();
    test_controls_layout();
    test_controls_puck();
    test_controls_point();
    test_interlace();
    test_stereo_detect();
    TEST_MAIN_END();
}
