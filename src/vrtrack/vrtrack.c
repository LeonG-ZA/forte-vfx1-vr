/*
 * vrtrack.c -- see include/vfx/vrtrack.h.
 */
#include "vfx/vrtrack.h"

#include <math.h>
#include <string.h>

#define PI 3.14159265358979323846
#define UNITS_TO_RAD (2.0 * PI / 65536.0)

void vrtrack_init(VrTrack *t, VrTrackKind kind)
{
    memset(t, 0, sizeof *t);
    t->kind = kind;
    t->mode = 1;
}

void vrtrack_scale_head(const int16_t head[3], double gain, int16_t out[3])
{
    const double limit = 170.0 / 360.0 * 65536.0;
    double y = head[0] * gain, p = head[1] * gain, r = head[2] * gain;
    long yi = (long)floor(y + 0.5) & 0xFFFF;
    out[0] = (int16_t)(yi >= 32768 ? yi - 65536 : yi);
    out[1] = (int16_t)(p > limit ? limit : p < -limit ? -limit : p);
    out[2] = (int16_t)(r > limit ? limit : r < -limit ? -limit : r);
}

void vrsteer_reset(VrSteer *s)
{
    memset(s, 0, sizeof *s);
}

void vrsteer_update(VrSteer *s, const int16_t raw[3], double now_ms, int on, int16_t out[3])
{
    const double limit = 170.0 / 360.0 * 65536.0;
    double k = 1.0;
    int i;

    if (!on || !s->primed) {
        for (i = 0; i < 3; i++) { s->out[i] = raw[i]; s->last[i] = raw[i]; out[i] = raw[i]; }
        s->last_ms = now_ms;
        s->interval_ms = 1000.0 / 30.0;
        s->primed = on;
        return;
    }
    {
        double dt = now_ms - s->last_ms;
        if (dt > 0 && dt < 500) s->interval_ms += (dt - s->interval_ms) * 0.1;
        s->last_ms = now_ms;
    }
    k = (1000.0 / 30.0) / (s->interval_ms > 1 ? s->interval_ms : 1);
    if (k < 1) k = 1;
    if (k > 10) k = 10;
    for (i = 0; i < 3; i++) {
        int16_t d = (int16_t)(raw[i] - s->last[i]);        /* wraps like the angle */
        s->out[i] += d * k;
        s->last[i] = raw[i];
    }
    /* yaw wraps; pitch and roll stop short of the ends so they never jump */
    while (s->out[0] >= 32768.0) s->out[0] -= 65536.0;
    while (s->out[0] < -32768.0) s->out[0] += 65536.0;
    for (i = 1; i < 3; i++) s->out[i] = s->out[i] > limit ? limit : s->out[i] < -limit ? -limit : s->out[i];
    for (i = 0; i < 3; i++) out[i] = (int16_t)floor(s->out[i] + 0.5);
}

unsigned vrtrack_baud(VrTrackKind kind)
{
    return kind == VRTRACK_CYBERMAXX ? 19200u : 9600u;
}

static void put(VrTrack *t, uint8_t b)
{
    if (t->out_count >= VRTRACK_OUT) return;       /* the program stopped reading */
    t->out[(t->out_head + t->out_count) % VRTRACK_OUT] = b;
    t->out_count++;
}

static void put16(VrTrack *t, long v)
{
    uint16_t u = (uint16_t)(v & 0xFFFF);
    put(t, (uint8_t)(u >> 8));
    put(t, (uint8_t)(u & 0xFF));
}

static long clamp16(double v)
{
    v = floor(v + 0.5);
    return v < -32768.0 ? -32768 : v > 32767.0 ? 32767 : (long)v;
}

/* A request that comes while the last answer is still going out replaces
 * it: Descent 2 gives up on a packet after 10 ms and asks again, and
 * without this the late bytes would shift every packet after. */
static void restart_output(VrTrack *t)
{
    t->out_head = 0;
    t->out_count = 0;
}

/*
 * Descent 2 turns the i-glasses' readings into its own angles (a full turn
 * is 1.0) as -atan2(z', x') * pi/2, -pitch * pi/2 and roll * pi/2, the
 * primes being the vector rotated back by pitch and roll. For the VFX1 it
 * uses the negated angle in turns. Reporting every angle divided by pi^2
 * makes the two agree.
 */
static void iglasses_packet(VrTrack *t, const int16_t head[3])
{
    /* Descent 2 smooths these with a six-sample average whose weights already
     * sum to one and then divides by six again (iglasses.c, filterFIR), so
     * every reading reaches it at a sixth: sent six times larger. */
    const double scale = 6.0 / (PI * PI);
    double yaw = head[0] * UNITS_TO_RAD * scale;          /* the a that Descent 2 recovers */
    double pitch = head[1] * UNITS_TO_RAD * scale;
    double roll = -head[2] * UNITS_TO_RAD * scale;
    double sp = sin(pitch), cp = cos(pitch), cr = cos(roll);
    double fx = cos(yaw) / cr, fy = sp * sin(yaw), fz = cp * sin(yaw);
    uint8_t sum = 0;
    int i, start;

    restart_output(t);
    start = t->out_count;
    put(t, 0x81);                                          /* header; its meaning is unknown */
    put16(t, clamp16(fx * 16384.0));
    put16(t, clamp16(fy * 16384.0));
    put16(t, clamp16(fz * 16384.0));
    put16(t, clamp16(pitch * 16384.0 / PI));
    put16(t, clamp16(roll * 16384.0 / PI));
    for (i = start; i < t->out_count; i++) sum = (uint8_t)(sum + t->out[(t->out_head + i) % VRTRACK_OUT]);
    put(t, sum);
    t->packets++;
}

/* Z.A.R.'s mode 2 (vrtrack.h). Yaw and pitch at 32768 to a turn, half the
 * VFX1's units, signed and within half a turn either way. */
static void iglasses_m2_packet(VrTrack *t, const int16_t head[3])
{
    long yaw = head[0] / 2, pitch = head[1] / 2;
    uint8_t sum = 0;
    int i;
    yaw = yaw > 16384 ? 16384 : yaw < -16384 ? -16384 : yaw;
    pitch = pitch > 16384 ? 16384 : pitch < -16384 ? -16384 : pitch;
    restart_output(t);
    put(t, 0xFF);
    put16(t, yaw);
    put16(t, pitch);
    put16(t, 0);
    for (i = 0; i < 7; i++) sum = (uint8_t)(sum + t->out[(t->out_head + i) % VRTRACK_OUT]);
    put(t, sum);
    t->packets++;
}

/* The answer to "!V", laid out as Z.A.R. checks it (vrtrack.h); the last
 * byte is the command's 'O'. */
static void iglasses_version(VrTrack *t)
{
    static const char text[61] =
        "Mfr: Virtual i-O"                      /* 0 */
        " "
        "Product: i-glasses! head tracker  "    /* 17 */
        "F001.000O";                            /* 51 */
    int i;
    for (i = 0; i < 60; i++) put(t, (uint8_t)text[i]);
}

/*
 * Descent 2 reads yaw as is and pitch and roll as 16384 minus the value,
 * all in its own angle units, 65536 to a turn; for the VFX1 it negates the
 * driver's angles. So yaw is sent negated and pitch and roll offset.
 */
static void cybermaxx_packet(VrTrack *t, const int16_t head[3])
{
    restart_output(t);
    put16(t, -(long)head[0]);
    put16(t, 16384L + head[1]);
    put16(t, 16384L + head[2]);
    t->packets++;
}

void vrtrack_write(VrTrack *t, uint8_t byte, const int16_t head[3])
{
    if (t->kind == VRTRACK_CYBERMAXX) {
        if (byte == 'S') cybermaxx_packet(t, head);
        else t->commands++;                                /* A, C, F, G: modes, no answer */
        return;
    }

    if (t->in_line) {
        if (byte == '\r' || byte == '\n') {
            t->line[t->line_len] = 0;
            t->in_line = 0;
            t->commands++;
            if (t->line[0] == 'M' && (t->line[1] == '1' || t->line[1] == '2')) t->mode = t->line[1] - '0';
            if (t->line[0] == 'V' && t->line[1] == 0) iglasses_version(t);
            else put(t, 'O');                              /* every command is acknowledged */
        } else if (t->line_len < (int)sizeof t->line - 1) {
            t->line[t->line_len++] = (char)byte;
        }
        return;
    }
    if (byte == '!') {
        t->in_line = 1;
        t->line_len = 0;
    } else if (byte == 'S') {
        if (t->mode == 2) iglasses_m2_packet(t, head);
        else iglasses_packet(t, head);
    }
}

int vrtrack_read(VrTrack *t, uint8_t *byte)
{
    if (t->out_count == 0) return 0;
    *byte = t->out[t->out_head];
    t->out_head = (t->out_head + 1) % VRTRACK_OUT;
    t->out_count--;
    return 1;
}
