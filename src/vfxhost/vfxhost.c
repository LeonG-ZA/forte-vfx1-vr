/*
 * vfxhost.c -- see include/vfx/vfxhost.h. Ported from web/vfx-reproject.js,
 * web/vfx-stereo.js and web/vfx1.html, which the headset sessions validated.
 */
#include "vfx/vfxhost.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265358979323846

/* ------------------------------------------------------------ angles */

static VfxHostQuat qnorm(VfxHostQuat q)
{
    double n = sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    if (n <= 0.0) { q.w = 1.0; q.x = q.y = q.z = 0.0; return q; }
    q.w /= n; q.x /= n; q.y /= n; q.z /= n;
    return q;
}

VfxHostQuat vfxhost_quat_mul(VfxHostQuat a, VfxHostQuat b)
{
    VfxHostQuat r;
    r.w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z;
    r.x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y;
    r.y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x;
    r.z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w;
    return r;
}

VfxHostQuat vfxhost_quat_conj(VfxHostQuat q)
{
    q.x = -q.x; q.y = -q.y; q.z = -q.z;
    return q;
}

static double clamp1(double v) { return v < -1.0 ? -1.0 : v > 1.0 ? 1.0 : v; }

/* q = Ry(yaw) Rx(pitch) Rz(-roll) */
void vfxhost_sense_angles(VfxHostQuat q, double *yaw, double *pitch, double *roll)
{
    double m02, m22, m12, m10, m11;
    q = qnorm(q);
    m02 = 2 * (q.x * q.z + q.w * q.y);
    m22 = 1 - 2 * (q.x * q.x + q.y * q.y);
    m12 = 2 * (q.y * q.z - q.w * q.x);
    m10 = 2 * (q.x * q.y + q.w * q.z);
    m11 = 1 - 2 * (q.x * q.x + q.z * q.z);
    *pitch = asin(clamp1(-m12));
    *yaw = atan2(m02, m22);
    *roll = -atan2(m10, m11);
}

void vfxhost_tilt_angles(VfxHostQuat q, double *pitch, double *roll)
{
    double forward_y, right_y;
    q = qnorm(q);
    forward_y = 2 * (q.w * q.x - q.y * q.z);    /* world Y of the local -Z axis */
    right_y = 2 * (q.x * q.y + q.w * q.z);      /* world Y of the local +X axis */
    *pitch = asin(clamp1(forward_y));
    *roll = -asin(clamp1(right_y));
}

int16_t vfxhost_sense_units(double radians)
{
    double units = floor(radians / (2 * PI) * 65536.0 + 0.5);
    long v = (long)fmod(units, 65536.0);
    if (v < 0) v += 65536L;
    if (v >= 32768L) v -= 65536L;
    return (int16_t)v;
}

void vfxhost_head_recenter(VfxHostHead *h)
{
    h->have_ref = 0;
}

void vfxhost_head_update(VfxHostHead *h, VfxHostQuat q, int16_t *yaw, int16_t *pitch, int16_t *roll)
{
    double y, p, r;
    if (!h->have_ref) { h->ref = q; h->have_ref = 1; }
    vfxhost_sense_angles(vfxhost_quat_mul(vfxhost_quat_conj(h->ref), q), &y, &p, &r);
    *yaw = vfxhost_sense_units(y);
    *pitch = vfxhost_sense_units(p);
    *roll = vfxhost_sense_units(r);
}

/* ------------------------------------------------------------ page flip */

void vfxhost_pageflip_init(VfxPageFlip *pf, int step)
{
    memset(pf, 0, sizeof *pf);
    pf->step = step > 0 ? step : 8;
    pf->pending = -1;
    pf->shown = -1;
}

void vfxhost_pageflip_free(VfxPageFlip *pf)
{
    free(pf->prev);
    free(pf->eyes);
    vfxhost_pageflip_init(pf, pf->step);
}

void vfxhost_changed_per_parity(const uint8_t *cur, const uint8_t *prev, int w, int h,
                                int step, uint32_t changed[2])
{
    int x, y;
    changed[0] = changed[1] = 0;
    for (y = 0; y < h; y++) {
        size_t row = (size_t)y * (size_t)w * 3u;
        for (x = 0; x < w; x += step) {
            size_t i = row + (size_t)x * 3u;
            if (cur[i] != prev[i] || cur[i + 1] != prev[i + 1] || cur[i + 2] != prev[i + 2])
                changed[y & 1]++;
        }
    }
}

static int copy_parity(VfxPageFlip *pf, const uint8_t *rgb, int w, int h, int parity)
{
    int hh = h / 2, y;
    size_t row = (size_t)w * 3u, need = row * (size_t)hh;
    if (pf->eyes_size != need) {
        uint8_t *e = (uint8_t *)realloc(pf->eyes, need);
        if (!e) return -1;
        pf->eyes = e;
        pf->eyes_size = need;
    }
    pf->eyes_w = w;
    pf->eyes_h = hh;
    for (y = 0; y < hh; y++)
        memcpy(pf->eyes + (size_t)y * row, rgb + (size_t)(2 * y + parity) * row, row);
    return 1;
}

/* Whether a frame's changes are a new palette: every sampled pixel of one
 * old colour turned the same new colour (at least 95% of them), as no
 * drawing does. */
static int palette_change(const uint8_t *cur, const uint8_t *prev, int w, int h, int step)
{
    enum { SLOTS = 4096 };
    static uint32_t from[SLOTS], to[SLOTS];
    static uint8_t used[SLOTS];
    uint32_t changed = 0, conflicts = 0, colours = 0;
    int x, y;
    memset(used, 0, sizeof used);
    for (y = 0; y < h; y++) {
        size_t row = (size_t)y * (size_t)w * 3u;
        for (x = 0; x < w; x += step) {
            size_t i = row + (size_t)x * 3u;
            uint32_t a = ((uint32_t)prev[i] << 16) | ((uint32_t)prev[i + 1] << 8) | prev[i + 2];
            uint32_t b = ((uint32_t)cur[i] << 16) | ((uint32_t)cur[i + 1] << 8) | cur[i + 2];
            unsigned s = (unsigned)((a * 2654435761u) >> 20) & (SLOTS - 1);
            if (a == b) continue;
            changed++;
            while (used[s] && from[s] != a) s = (s + 1) & (SLOTS - 1);
            if (!used[s]) {
                if (++colours > SLOTS / 2) return 0;    /* not a 256-colour picture */
                used[s] = 1; from[s] = a; to[s] = b;
            }
            else if (to[s] != b) conflicts++;
        }
    }
    return changed > 0 && conflicts * 20u < changed;
}

/* Which halves of a parity's lines changed: bit 0 left, bit 1 right. */
static int changed_halves(const uint8_t *cur, const uint8_t *prev, int w, int h, int step, int parity)
{
    int x, y, halves = 0;
    for (y = parity; y < h && halves != 3; y += 2) {
        size_t row = (size_t)y * (size_t)w * 3u;
        for (x = 0; x < w; x += step) {
            size_t i = row + (size_t)x * 3u;
            if (cur[i] != prev[i] || cur[i + 1] != prev[i + 1] || cur[i + 2] != prev[i + 2])
                halves |= x < w / 2 ? 1 : 2;
        }
    }
    return halves;
}

int vfxhost_pageflip_feed(VfxPageFlip *pf, const uint8_t *rgb, int w, int h)
{
    size_t size = (size_t)w * (size_t)h * 3u;
    uint32_t c[2];
    int drawing, done = -1, halves = 0;

    if (!pf->prev || pf->prev_size != size) {
        uint8_t *p = (uint8_t *)realloc(pf->prev, size);
        if (!p) return -1;
        pf->prev = p;
        pf->prev_size = size;
        memcpy(pf->prev, rgb, size);
        pf->pending = -1;
        pf->shown = 0;
        return copy_parity(pf, rgb, w, h, 0);
    }
    vfxhost_changed_per_parity(rgb, pf->prev, w, h, pf->step, c);
    {
        /* A mouse cursor is not a page being drawn: System Shock draws its
         * pages on the even lines and its cursor into the unused odd ones,
         * about 0.2% of their samples, and showing that page for it flashed
         * a black page with a red cursor. Drawing changes far more. */
        uint32_t small = (uint32_t)(h / 2) * (uint32_t)((w + pf->step - 1) / pf->step) / 200u;
        if (c[0] <= small) c[0] = 0;
        if (c[1] <= small) c[1] = 0;
    }
    drawing = (c[0] == 0 && c[1] == 0) ? -1 : (c[1] > c[0] ? 1 : 0);
    /* A new palette changes every pixel of both parities a little and is
     * not drawing: Descent 2 sets one each game frame, between drawing its
     * pages, and taken for a page it showed some pages early, others late.
     * Telling it by both parities changing alike also caught Descent
     * redrawing both, and froze the picture. */
    if (c[0] && c[1] && palette_change(rgb, pf->prev, w, h, pf->step))
        drawing = pf->pending;
    else if (drawing != -1)
        halves = changed_halves(rgb, pf->prev, w, h, pf->step, drawing);
    memcpy(pf->prev, rgb, size);

    /* A pause completes the page only once both eyes are in it: Descent
     * draws the left eye, waits a frame or two, then the right, and showing
     * the page at the wait paired a new left eye with a right two pages old.
     * Drawing moving to the other parity always completes it. */
    if (pf->pending != -1 && drawing != pf->pending && (drawing != -1 || pf->pending_halves == 3))
        done = pf->pending;
    if (drawing != -1) {
        if (drawing != pf->pending) pf->pending_halves = 0;
        pf->pending = drawing;
        pf->pending_halves |= halves;
    } else if (done != -1) {
        pf->pending = -1;
    }
    if (done == -1) return 0;

    /* The finished page is untouched in this frame, so it is copied from it. */
    pf->shown = done;
    pf->flips++;
    return copy_parity(pf, rgb, w, h, done);
}

void vfxhost_interlace_split(const uint8_t *rgb, int w, int h, uint8_t *eyes)
{
    size_t row = (size_t)w * 3u;
    int y;
    for (y = 0; y < h / 2; y++) {
        memcpy(eyes + (size_t)y * row * 2u, rgb + (size_t)(2 * y) * row, row);
        memcpy(eyes + (size_t)y * row * 2u + row, rgb + (size_t)(2 * y + 1) * row, row);
    }
}

static int luma(const uint8_t *p)
{
    return (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8;
}

double vfxhost_lines_stereo_score(const uint8_t *rgb, int w, int h)
{
    double near = 0, far = 0;
    int y, x;
    for (y = 0; y + 2 < h; y += 2)
        for (x = 0; x < w; x += 2) {
            int a = luma(rgb + ((size_t)y * w + x) * 3);
            int b = luma(rgb + ((size_t)(y + 1) * w + x) * 3);
            int c = luma(rgb + ((size_t)(y + 2) * w + x) * 3);
            near += abs(a - b);
            far += abs(a - c);
        }
    return far > 0 ? near / far : 0;
}

double vfxhost_lines_alike_score(const uint8_t *rgb, int w, int h)
{
    double same = 0, between = 0;
    int y, x;
    for (y = 0; y + 2 < h; y += 2)
        for (x = 0; x < w; x += 2) {
            int a = luma(rgb + ((size_t)y * w + x) * 3);           /* left eye */
            int b = luma(rgb + ((size_t)(y + 1) * w + x) * 3);     /* right eye, the same row */
            int c = luma(rgb + ((size_t)(y + 2) * w + x) * 3);     /* left eye, the next row */
            same += abs(b - a);
            between += abs(2 * b - a - c) * 0.5;
        }
    return between > 0 ? same / between : 0;
}

int vfxhost_lines_look_stereo(const uint8_t *rgb, int w, int h, int current, double strictness,
                              double *score_out, double *alike_out)
{
    double score = vfxhost_lines_stereo_score(rgb, w, h);
    double alike = vfxhost_lines_alike_score(rgb, w, h);
    if (score_out) *score_out = score;
    if (alike_out) *alike_out = alike;
    if (score > 1.0 || alike < 0.9) return 1;
    if (strictness <= 0) strictness = 1;
    if (score < 0.8 * strictness && alike > 1.05 / strictness) return 0;
    return current;
}

/* 4x4 box-averaged luma of one eye, for comparing the eyes */
static void eye_thumb(const uint8_t *eyes, int w, int h, int right, float *out, int tw, int th)
{
    int ew = w / 2, tx, ty, x, y;
    for (ty = 0; ty < th; ty++)
        for (tx = 0; tx < tw; tx++) {
            int sum = 0;
            for (y = 0; y < 4; y++)
                for (x = 0; x < 4; x++)
                    sum += luma(eyes + ((size_t)(ty * 4 + y) * w + (size_t)right * ew + tx * 4 + x) * 3);
            out[ty * tw + tx] = (float)sum / 16.0f;
        }
    (void)h;
}

int vfxhost_pages_look_stereo(const uint8_t *eyes, int w, int h)
{
    return vfxhost_pages_look_stereo_at(eyes, w, h, 1.0);
}

static int pages_judge(const uint8_t *eyes, int w, int h, double strictness, int tracked);

int vfxhost_pages_look_stereo_at(const uint8_t *eyes, int w, int h, double strictness)
{
    return pages_judge(eyes, w, h, strictness, 0);
}

int vfxhost_pages_look_stereo_tracked(const uint8_t *eyes, int w, int h)
{
    return pages_judge(eyes, w, h, 1.0, 1);
}

static int pages_judge(const uint8_t *eyes, int w, int h, double strictness, int tracked)
{
    enum { TW = 80, TH = 120 };
    static float l[TW * TH], r[TW * TH];
    double seam = 0, cols = 0, best = -1, half[2] = { 0, 0 };
    int seam_rows = 0, low_detail, ew = w / 2, x, y, s, tw = ew / 4, th = h / 4;
    if (w < 64 || h < 16 || tw > TW || th > TH) return 1;

    /* the jump between the eyes against the usual column-to-column change */
    for (y = 0; y < h; y++) {
        const uint8_t *row = eyes + (size_t)y * w * 3;
        int js = abs(luma(row + (ew - 1) * 3) - luma(row + ew * 3)), jn = 4, k;
        static const int near[4] = { -5, -3, 1, 3 };
        seam += js;
        /* a row whose jump at the seam stands out from the columns beside it */
        for (k = 0; k < 4; k++) {
            int d = abs(luma(row + (ew + near[k]) * 3) - luma(row + (ew + near[k] + 1) * 3));
            if (d > jn) jn = d;
        }
        seam_rows += js > 3 * jn;
        for (x = 8; x < w - 9; x += 7)
            if (x != ew - 1) {
                int d = abs(luma(row + x * 3) - luma(row + (x + 1) * 3));
                cols += d;
                half[x >= ew] += d;
            }
    }
    cols /= (double)((w - 17) / 7);
    /* one half with a picture and the other all but empty: a flat screen
     * drawn on one side, such as a text menu */
    if (half[0] + half[1] > 2.0 * h && (half[0] < 0.08 * half[1] || half[1] < 0.08 * half[0])) return 0;
    /* too little detail to judge (a dark corridor): stereo, unless (below)
     * one picture runs on smoothly across the middle */
    low_detail = cols < 2.0 * h;
    if (strictness <= 0) strictness = 1;

    /* how alike the halves are, at a small offset */
    eye_thumb(eyes, w, h, 0, l, tw, th);
    eye_thumb(eyes, w, h, 1, r, tw, th);
    for (s = -4; s <= 4; s++) {
        double ml = 0, mr = 0, lr = 0, ll = 0, rr = 0;
        int n = 0;
        for (y = 0; y < th; y++)
            for (x = 4; x < tw - 4; x++) { ml += l[y * tw + x]; mr += r[y * tw + x + s]; n++; }
        ml /= n; mr /= n;
        for (y = 0; y < th; y++)
            for (x = 4; x < tw - 4; x++) {
                double a = l[y * tw + x] - ml, b = r[y * tw + x + s] - mr;
                lr += a * b; ll += a * a; rr += b * b;
            }
        if (ll * rr > 0 && lr / sqrt(ll * rr) > best) best = lr / sqrt(ll * rr);
        if (ll * rr == 0) best = 1;
    }
    /* A title reading the head tracker is in its 3D view, and so is a screen
     * too plain to judge: stereo, unless one picture runs on smoothly across
     * the middle: few rows jumping at the seam and the halves not near
     * copies. Magic Carpet's flat 640x480 view has at most 4% of rows
     * jumping, even with something drawn across the middle, and halves alike
     * at up to 0.91 over sky and water; VFX1 pages 12 to 68% of rows, or
     * halves alike at 1.0. Descent's detailed pages can have as few as 2% of
     * rows jumping (a dark corridor) but a seam of 2.2 columns' change or
     * more, or halves alike at 0.93 or more; Magic Carpet's detailed flat
     * views stay under 1.8 and 0.57. A plain
     * picture's seam is one edge or two (Magic Carpet: 3.4), so there only
     * the rows count. */
    if (low_detail) return !(seam_rows < 0.08 * h && best < 0.97);
    if (tracked) return !(seam_rows < 0.08 * h && seam < 1.8 * cols && best < 0.9);
    /* the eyes nearly the same picture: stereo */
    if (best > 0.6 + (strictness - 1.0)) return 1;
    /* a jump at the seam between two pictures, if they are at all alike. A
     * sparse flat screen (MechWarrior 2's combat variables: halves alike at
     * -0.06) jumps there too, wherever its text crosses the middle. */
    return seam > 1.8 * strictness * cols && best > 0.2;
}

int vfxhost_stereo_gate(VfxStereoGate *g, int looks_stereo)
{
    looks_stereo = !!looks_stereo;
    if (looks_stereo == g->stereo) g->run = 0;
    else if (++g->run >= (looks_stereo ? (g->hold > 0 ? g->hold : 1) : (g->hold_flat > 0 ? g->hold_flat : 1))) {
        g->stereo = looks_stereo;
        g->run = 0;
    }
    return g->stereo;
}

/* ------------------------------------------------------------ controls */

#define PUCK_DEAD (12.0 * PI / 180.0)
#define PUCK_FULL (40.0 * PI / 180.0)

void vfxhost_controls_default_config(VfxHostControlsConfig *c)
{
    c->puck_hand = VFXPUCK_RIGHT;
    c->mouse = VFXMOUSE_OFF;
    c->mouse_hand = VFXPUCK_RIGHT;
    c->puck_rate = 700.0;
    c->point_scale = 1000.0;
}

void vfxhost_controls_init(VfxHostControls *c, const VfxHostControlsConfig *config)
{
    memset(c, 0, sizeof *c);
    if (config) c->config = *config;
    else vfxhost_controls_default_config(&c->config);
}

void vfxhost_controls_recenter(VfxHostControls *c)
{
    c->puck_rest = c->puck_now;
}

void vfxhost_controls_rest(VfxHostControls *c)
{
    c->puck_rest_set = 0;
    c->point_set = 0;
    c->carry_x = c->carry_y = 0;
}

void vfxhost_point_angles(VfxHostQuat q, double *yaw, double *pitch)
{
    double fx, fy, fz;
    q = qnorm(q);
    fx = -2 * (q.x * q.z + q.w * q.y);          /* world direction of the local -Z axis */
    fy = 2 * (q.w * q.x - q.y * q.z);
    fz = -(1 - 2 * (q.x * q.x + q.y * q.y));
    *yaw = atan2(-fx, -fz);
    *pitch = asin(clamp1(fy));
}

static double wrap_pi(double a)
{
    while (a > PI) a -= 2 * PI;
    while (a < -PI) a += 2 * PI;
    return a;
}

static double puck_axis(double angle)
{
    double a = fabs(angle);
    if (a <= PUCK_DEAD) return 0.0;
    a = (a - PUCK_DEAD) / (PUCK_FULL - PUCK_DEAD);
    if (a > 1.0) a = 1.0;
    return angle < 0 ? -a : a;
}

static double limit_quarter(double v)
{
    return v < -PI / 2 ? -PI / 2 : v > PI / 2 ? PI / 2 : v;
}

void vfxhost_controls_update(VfxHostControls *c, const VfxHostHand *left, const VfxHostHand *right,
                             int flying, double dt, VfxHostControlsOut *out)
{
    const VfxHostHand *puck = c->config.puck_hand == VFXPUCK_LEFT ? left
                            : c->config.puck_hand == VFXPUCK_RIGHT ? right : NULL;
    int puck_mouse = puck && puck->present && c->config.mouse == VFXMOUSE_PUCK;
    const VfxHostHand *pointer = c->config.mouse != VFXMOUSE_POINT ? NULL
                               : c->config.mouse_hand == VFXPUCK_LEFT ? left
                               : c->config.mouse_hand == VFXPUCK_RIGHT ? right : NULL;
    const VfxHostHand *mouse_hand = puck_mouse ? puck : pointer && pointer->present ? pointer : NULL;
    int recenter_now;

    memset(out, 0, sizeof *out);

    /* CyberPuck: tilt from rest and three buttons. */
    if (puck && puck->present) {
        if (puck->has_orientation) {
            c->puck_now = puck->orientation;
            if (!c->puck_rest_set) {
                c->puck_rest = c->puck_now;
                c->puck_rest_set = 1;
            }
        }
        if (c->puck_rest_set) {
            /* The turn from rest, in the room: its pitch and roll are the
             * tilt. Subtracting the pitch at rest from the pitch now went
             * wrong for a controller held with its front steeply up or down,
             * where tipping it either way lowered the pitch. */
            double pitch, roll;
            vfxhost_tilt_angles(vfxhost_quat_mul(c->puck_now, vfxhost_quat_conj(c->puck_rest)), &pitch, &roll);
            out->puck_pitch = limit_quarter(pitch);
            out->puck_roll = limit_quarter(roll);
        }
        out->puck_buttons = (uint8_t)((puck->trigger ? 1 : 0) | (puck->grip ? 2 : 0) | (puck->primary ? 4 : 0));
    }

    /* VRMOUSE: tilt as a mouse rate, puck buttons as mouse buttons. Outside
     * stereo only the stronger axis moves, which keeps menu pointers that
     * follow one axis at a time from jumping. */
    if (puck_mouse) {
        double ax = puck_axis(out->puck_roll), ay = puck_axis(out->puck_pitch);
        int dx, dy;
        if (dt > 0.1) dt = 0.1;
        if (!flying) {
            if (fabs(ax) >= fabs(ay)) { ay = 0; c->carry_y = 0; }
            else { ax = 0; c->carry_x = 0; }
        }
        c->carry_x += ax * c->config.puck_rate * dt;
        c->carry_y += ay * c->config.puck_rate * dt;
        dx = (int)c->carry_x;
        dy = (int)c->carry_y;
        c->carry_x -= dx;
        c->carry_y -= dy;
        out->mouse_dx = dx;
        out->mouse_dy = dy;
        out->mouse_buttons = out->puck_buttons;
    }

    /* Pointing: how far the controller turned since the last update, as mouse
     * counts; trigger and grip are the left and right buttons. */
    if (pointer && pointer->present) {
        if (pointer->has_orientation) {
            double yaw, pitch, dyaw, dpitch;
            vfxhost_point_angles(pointer->orientation, &yaw, &pitch);
            if (c->point_set) {
                dyaw = wrap_pi(yaw - c->point_yaw);
                dpitch = pitch - c->point_pitch;
                /* pointing near straight up or down swings yaw wildly */
                if (fabs(pitch) > 1.3 || fabs(dyaw) > 0.5) dyaw = 0;
                if (fabs(dpitch) > 0.5) dpitch = 0;
                c->carry_x += -dyaw * c->config.point_scale;
                c->carry_y += -dpitch * c->config.point_scale;
                out->mouse_dx = (int)c->carry_x;
                out->mouse_dy = (int)c->carry_y;
                c->carry_x -= out->mouse_dx;
                c->carry_y -= out->mouse_dy;
            }
            c->point_yaw = yaw;
            c->point_pitch = pitch;
            c->point_set = 1;
        }
        out->mouse_buttons = (uint8_t)((pointer->trigger ? 1 : 0) | (pointer->grip ? 2 : 0));
    }

    /* The default layout, everything on the right hand, the left optional.
     * Keys wanted by either hand are held. */
    if (right && right->present) {
        if (mouse_hand != right) {
            out->keys[VFXKEY_CTRL] |= (uint8_t)!!right->trigger;
            out->keys[VFXKEY_A] |= (uint8_t)!!right->grip;
        }
        out->keys[VFXKEY_SPACE] |= (uint8_t)!!right->stick_press;
        out->keys[VFXKEY_ENTER] |= (uint8_t)!!right->primary;
        out->keys[VFXKEY_ESCAPE] |= (uint8_t)!!right->secondary;
        out->keys[VFXKEY_UP] |= (uint8_t)(right->stick_y < -0.6);
        out->keys[VFXKEY_DOWN] |= (uint8_t)(right->stick_y > 0.6);
        out->keys[VFXKEY_LEFT] |= (uint8_t)(right->stick_x < -0.6);
        out->keys[VFXKEY_RIGHT] |= (uint8_t)(right->stick_x > 0.6);
    }
    if (left && left->present) {
        if (mouse_hand != left) out->keys[VFXKEY_SPACE] |= (uint8_t)!!left->trigger;
        out->keys[VFXKEY_A] |= (uint8_t)(left->stick_y < -0.5);
        out->keys[VFXKEY_Z] |= (uint8_t)(left->stick_y > 0.5);
        out->keys[VFXKEY_LEFT] |= (uint8_t)(left->stick_x < -0.6);
        out->keys[VFXKEY_RIGHT] |= (uint8_t)(left->stick_x > 0.6);
    }

    recenter_now = left && left->present && left->primary;
    out->recenter = recenter_now && !c->recenter_held;
    c->recenter_held = recenter_now;
    if (out->recenter) vfxhost_controls_recenter(c);
}
