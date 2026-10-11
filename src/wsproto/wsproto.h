/*
 * wsproto.h -- the wire between a browser and the emulator.
 *
 * A WebXR page cannot open a raw socket and the emulator cannot call WebXR, so
 * the two meet over a WebSocket on localhost. This module is the part of that
 * with no I/O in it: the handshake key derivation, RFC 6455 frame coding, and
 * the one-line pose format carried inside the frames.
 *
 * Deliberately NOT part of libvfxbridge. The bridge is transport-neutral by
 * requirement, and WebSocket framing is a transport. This is a utility a pose
 * source or an adapter may link; nothing below the bridge line references it,
 * and tools/check_layering.sh checks that.
 *
 * Pure C99 over <stdint.h>/<stddef.h>: no sockets, no allocation, no globals.
 * All of it is unit tested in tests/test_wsproto.c against RFC test vectors.
 */
#ifndef VFX_WSPROTO_H
#define VFX_WSPROTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- */
/* Handshake                                                        */
/* ---------------------------------------------------------------- */

/* Length of a Sec-WebSocket-Accept value plus its terminator. */
#define VFX_WS_ACCEPT_LEN 29

/*
 * Derive Sec-WebSocket-Accept from the client's Sec-WebSocket-Key, per
 * RFC 6455 section 4.2.2: base64(sha1(key + magic GUID)).
 * Returns 1 on success, 0 if outcap is too small or client_key is NULL.
 */
int vfx_ws_accept_key(const char *client_key, char *out, size_t outcap);

/*
 * Case-insensitively find a header in an HTTP request and copy its value,
 * trimmed. Returns 1 if found. `req` must be NUL-terminated; `name` is given
 * without the colon.
 */
int vfx_http_header(const char *req, const char *name, char *out, size_t outcap);

/*
 * Copy the request target of an HTTP request line ("GET /pose HTTP/1.1") into
 * out. Returns 1 if the request line parsed and the method matched `method`.
 */
int vfx_http_request_target(const char *req, const char *method,
                            char *out, size_t outcap);

/* True once the request headers are complete, i.e. CRLFCRLF is present. */
int vfx_http_headers_complete(const char *buf, size_t len);

/* ---------------------------------------------------------------- */
/* Framing                                                          */
/* ---------------------------------------------------------------- */

typedef enum {
    VFX_WS_CONT   = 0x0,
    VFX_WS_TEXT   = 0x1,
    VFX_WS_BINARY = 0x2,
    VFX_WS_CLOSE  = 0x8,
    VFX_WS_PING   = 0x9,
    VFX_WS_PONG   = 0xA
} VfxWsOpcode;

/* Frames larger than this are refused. Poses are tens of bytes; anything near
 * this is a bug or an attack, and refusing beats allocating for it. */
#define VFX_WS_MAX_PAYLOAD 65536u

typedef struct {
    int         fin;
    VfxWsOpcode opcode;
    uint8_t    *payload;      /* points into the caller's buffer */
    size_t      payload_len;
    size_t      consumed;     /* bytes of input this frame occupied */
} VfxWsFrame;

/*
 * Decode one client-to-server frame from buf, unmasking the payload in place.
 * buf must therefore be writable.
 *
 * Returns  1  a whole frame was decoded and *out is filled
 *          0  need more bytes, call again when more have arrived
 *         -1  protocol error, the connection should be closed
 *
 * A client frame that is not masked is a protocol error, per RFC 6455 5.1.
 */
int vfx_ws_decode(uint8_t *buf, size_t len, VfxWsFrame *out);

/*
 * Build a server-to-client frame, which is never masked. Returns the number of
 * bytes written, or 0 if dstcap is too small.
 *
 * payload must not overlap dst: the header is written first, so an overlapping
 * payload would be overwritten before it is copied.
 */
size_t vfx_ws_encode(uint8_t *dst, size_t dstcap, VfxWsOpcode opcode,
                     const void *payload, size_t payload_len);

/* ---------------------------------------------------------------- */
/* The pose wire format                                             */
/* ---------------------------------------------------------------- */

/*
 * One text frame per message, so no JSON parser is needed anywhere:
 *
 *   "P <w> <x> <y> <z> <t_ms>"   a head pose; t_ms is the page's timestamp
 *   "R"                          recentre: treat the next pose as forward
 *
 * Unknown message types are ignored rather than treated as errors, so the page
 * can add messages without breaking an older emulator.
 */
typedef enum {
    VFX_WIRE_NONE = 0,
    VFX_WIRE_POSE,
    VFX_WIRE_RECENTER
} VfxWireKind;

typedef struct {
    VfxWireKind kind;
    float       w, x, y, z;   /* quaternion, only when kind is POSE */
    double      t_ms;
} VfxWireMsg;

/*
 * Parse one message. `msg` need not be NUL-terminated; len bounds it.
 * Returns 1 if a message was recognised (out->kind says which), 0 otherwise.
 */
int vfx_wire_parse(const char *msg, size_t len, VfxWireMsg *out);

/* ---------------------------------------------------------------- */
/* Exposed for testing                                              */
/* ---------------------------------------------------------------- */

void vfx_sha1(const void *data, size_t len, uint8_t digest[20]);
size_t vfx_base64_encode(const uint8_t *src, size_t len, char *dst, size_t dstcap);

#ifdef __cplusplus
}
#endif

#endif /* VFX_WSPROTO_H */
