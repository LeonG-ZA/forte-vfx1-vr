/*
 * vrtrack.h -- serial head trackers of the VFX1's rivals: Virtual I/O's
 * i-glasses! and VictorMaxx's CyberMaxx.
 *
 * Both sat on a COM port and answered a request byte with a packet of
 * angles. The protocols here are the ones Descent 2 speaks, from Parallax's
 * released source (main/iglasses.c in Descent 2, main/victor.c in Descent):
 *
 *   i-glasses!  9600 8N1. Commands "!...\r" are answered 'O'. In mode 1
 *               ("!M1,P,B", Descent 2) each 'S' is answered with 12 bytes: a
 *               header, big-endian x, y, z of a direction vector (16384 =
 *               1), pitch and roll (16384 = pi radians), and a checksum, the
 *               low byte of the sum of the first eleven.
 *
 *               Z.A.R. (Maddox), read from its code: "!R" and "!M2,P,B" are
 *               answered 'O'; "!V" with 60 bytes of version text, which it
 *               checks for 'M' at 0, 'P' at 17, 'F' at 51, three digits at
 *               52, '.' at 55, three digits at 56 and the 'O' at 59. In mode
 *               2 each 'S' is answered with 8 bytes: FFh, big-endian yaw and
 *               pitch (32768 to a turn, within +-16384), two bytes it
 *               ignores, and the low byte of the sum of the first seven.
 *   CyberMaxx   19200 8N1. 'A', 'C', 'F', 'G' set it up and are not
 *               answered. Each 'S' is answered with 6 bytes: big-endian yaw,
 *               pitch and roll, 65536 to a turn, pitch and roll offset by
 *               16384.
 *
 * The real devices' angle units are not documented anywhere found; the
 * scales here make Descent 2 turn exactly as far as it does for the VFX1
 * (docs/FINDINGS.md, Q14).
 *
 * Plain C, no emulator types: the DOSBox glue feeds it the bytes the DOS
 * program sends and drains the bytes it answers.
 */
#ifndef VFX_VRTRACK_H
#define VFX_VRTRACK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum VrTrackKind { VRTRACK_IGLASSES, VRTRACK_CYBERMAXX } VrTrackKind;

#define VRTRACK_OUT 64

typedef struct VrTrack {
    VrTrackKind kind;
    char line[32];              /* an i-glasses "!..." command being received */
    int line_len, in_line;
    int mode;                   /* i-glasses packet mode: 1, or 2 after "!M2" */
    uint8_t out[VRTRACK_OUT];
    int out_head, out_count;
    uint32_t commands, packets; /* for diagnostics */
} VrTrack;

void vrtrack_init(VrTrack *t, VrTrackKind kind);

/* One byte from the DOS program. `head` is the head orientation in VFX1
 * units (65536 to a turn): yaw positive left, pitch positive up, roll
 * positive tilting right, as vfx1_set_head takes it. */
void vrtrack_write(VrTrack *t, uint8_t byte, const int16_t head[3]);

/* Next byte for the DOS program; returns 0 when there is none. */
int vrtrack_read(VrTrack *t, uint8_t *byte);

/* The head scaled by `gain` for a title that turns by how far the head moved
 * (Descent 2 does): yaw wraps, pitch and roll stop at +-170 degrees so they
 * never jump from one end to the other. */
void vrtrack_scale_head(const int16_t head[3], double gain, int16_t out[3]);

/*
 * Frame-rate matching for titles that steer by head movement.
 *
 * Descent and Descent 2 turn the ship by each frame's change in head angle
 * times that frame's duration, so the same head turn steers less the faster
 * the game runs: close to one to one at the 30 frames a second of a 1995 PC,
 * a fraction of that in an emulator running it at a hundred or more. Called
 * on every poll, this scales each head movement by how often the game polls,
 * as if it ran at 30 a second. Off, it passes the head through.
 */
typedef struct VrSteer {
    int primed;
    int16_t last[3];            /* raw head at the previous poll */
    double out[3];              /* the head the title sees */
    double last_ms, interval_ms;/* poll timing, smoothed */
} VrSteer;

void vrsteer_reset(VrSteer *s);
/* `now_ms`: emulated time. Returns the head the title should see. */
void vrsteer_update(VrSteer *s, const int16_t raw[3], double now_ms, int on, int16_t out[3]);

/* The baud rate the device runs at, for byte timing. */
unsigned vrtrack_baud(VrTrackKind kind);

#ifdef __cplusplus
}
#endif

#endif
