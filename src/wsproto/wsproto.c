/*
 * wsproto.c -- see wsproto.h. No I/O, no allocation, no globals.
 */
#include "wsproto.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---------------------------------------------------------------- */
/* SHA-1 (RFC 3174), needed only for the WebSocket handshake         */
/* ---------------------------------------------------------------- */

static uint32_t rol32(uint32_t v, unsigned n)
{
    return (uint32_t)((v << n) | (v >> (32u - n)));
}

static void sha1_block(uint32_t h[5], const uint8_t block[64])
{
    uint32_t w[80];
    uint32_t a, b, c, d, e;
    int i;

    for (i = 0; i < 16; ++i)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    for (i = 16; i < 80; ++i)
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4];

    for (i = 0; i < 80; ++i) {
        uint32_t f, k, t;
        if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6u; }
        t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol32(b, 30); b = a; a = t;
    }

    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void vfx_sha1(const void *data, size_t len, uint8_t digest[20])
{
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                      0xC3D2E1F0u };
    const uint8_t *p = (const uint8_t *)data;
    uint8_t tail[128];
    size_t whole = len / 64u;
    size_t rest  = len % 64u;
    size_t tail_len;
    uint64_t bits = (uint64_t)len * 8u;
    size_t i;

    for (i = 0; i < whole; ++i)
        sha1_block(h, p + i * 64u);

    memcpy(tail, p + whole * 64u, rest);
    tail[rest] = 0x80u;
    tail_len = rest + 1u;
    /* The length must land in the last 8 bytes of a 64-byte block. */
    while ((tail_len % 64u) != 56u)
        tail[tail_len++] = 0u;
    for (i = 0; i < 8u; ++i)
        tail[tail_len++] = (uint8_t)(bits >> (56u - 8u * i));

    for (i = 0; i < tail_len / 64u; ++i)
        sha1_block(h, tail + i * 64u);

    for (i = 0; i < 5u; ++i) {
        digest[i * 4]     = (uint8_t)(h[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)(h[i]);
    }
}

/* ---------------------------------------------------------------- */
/* base64                                                           */
/* ---------------------------------------------------------------- */

size_t vfx_base64_encode(const uint8_t *src, size_t len, char *dst, size_t dstcap)
{
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((len + 2u) / 3u) * 4u;
    size_t i, o = 0;

    if (!src || !dst || dstcap < need + 1u)
        return 0;

    for (i = 0; i < len; i += 3u) {
        uint32_t v = (uint32_t)src[i] << 16;
        size_t have = len - i;
        if (have > 1u) v |= (uint32_t)src[i + 1] << 8;
        if (have > 2u) v |= (uint32_t)src[i + 2];

        dst[o++] = tbl[(v >> 18) & 0x3Fu];
        dst[o++] = tbl[(v >> 12) & 0x3Fu];
        dst[o++] = (have > 1u) ? tbl[(v >> 6) & 0x3Fu] : '=';
        dst[o++] = (have > 2u) ? tbl[v & 0x3Fu]        : '=';
    }
    dst[o] = '\0';
    return o;
}

/* ---------------------------------------------------------------- */
/* Handshake                                                        */
/* ---------------------------------------------------------------- */

int vfx_ws_accept_key(const char *client_key, char *out, size_t outcap)
{
    /* The GUID is fixed by RFC 6455 section 1.3. */
    static const char magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char buf[256];
    uint8_t digest[20];
    size_t klen;

    if (!client_key || !out)
        return 0;
    klen = strlen(client_key);
    if (klen + sizeof(magic) > sizeof(buf))
        return 0;
    if (outcap < VFX_WS_ACCEPT_LEN)
        return 0;

    memcpy(buf, client_key, klen);
    memcpy(buf + klen, magic, sizeof(magic) - 1u);

    vfx_sha1(buf, klen + sizeof(magic) - 1u, digest);
    return vfx_base64_encode(digest, 20u, out, outcap) == 28u;
}

static int ascii_lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

static int header_name_matches(const char *line, const char *name, size_t nlen)
{
    size_t i;
    for (i = 0; i < nlen; ++i)
        if (ascii_lower((unsigned char)line[i]) != ascii_lower((unsigned char)name[i]))
            return 0;
    return line[nlen] == ':';
}

int vfx_http_header(const char *req, const char *name, char *out, size_t outcap)
{
    size_t nlen;
    const char *p;

    if (!req || !name || !out || !outcap)
        return 0;
    nlen = strlen(name);

    /* Walk line by line, skipping the request line. */
    p = strstr(req, "\r\n");
    if (!p)
        return 0;
    p += 2;

    while (*p && !(p[0] == '\r' && p[1] == '\n')) {
        const char *eol = strstr(p, "\r\n");
        if (!eol)
            eol = p + strlen(p);
        if (strlen(p) >= nlen + 1u && header_name_matches(p, name, nlen)) {
            const char *v = p + nlen + 1u;
            size_t vlen;
            while (v < eol && (*v == ' ' || *v == '\t'))
                ++v;
            vlen = (size_t)(eol - v);
            while (vlen && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t'))
                --vlen;
            if (vlen + 1u > outcap)
                return 0;
            memcpy(out, v, vlen);
            out[vlen] = '\0';
            return 1;
        }
        if (!*eol)
            break;
        p = eol + 2;
    }
    return 0;
}

int vfx_http_request_target(const char *req, const char *method,
                            char *out, size_t outcap)
{
    size_t mlen;
    const char *p, *end;
    size_t tlen;

    if (!req || !method || !out || !outcap)
        return 0;
    mlen = strlen(method);
    if (strncmp(req, method, mlen) != 0 || req[mlen] != ' ')
        return 0;

    p = req + mlen + 1u;
    end = p;
    while (*end && *end != ' ' && *end != '\r' && *end != '\n')
        ++end;
    tlen = (size_t)(end - p);
    if (tlen + 1u > outcap)
        return 0;
    memcpy(out, p, tlen);
    out[tlen] = '\0';
    return 1;
}

int vfx_http_headers_complete(const char *buf, size_t len)
{
    size_t i;
    if (!buf || len < 4u)
        return 0;
    for (i = 0; i + 3u < len; ++i)
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return 1;
    return 0;
}

/* ---------------------------------------------------------------- */
/* Framing                                                          */
/* ---------------------------------------------------------------- */

int vfx_ws_decode(uint8_t *buf, size_t len, VfxWsFrame *out)
{
    size_t need = 2u, hdr;
    uint64_t plen;
    int masked;
    const uint8_t *mask;
    uint64_t i;

    if (!buf || !out)
        return -1;
    if (len < need)
        return 0;

    if (buf[0] & 0x70u)          /* reserved bits must be clear */
        return -1;

    masked = (buf[1] & 0x80u) != 0;
    if (!masked)
        return -1;               /* RFC 6455 5.1: client frames must be masked */

    plen = (uint64_t)(buf[1] & 0x7Fu);
    hdr = 2u;

    if (plen == 126u) {
        if (len < hdr + 2u)
            return 0;
        plen = ((uint64_t)buf[2] << 8) | (uint64_t)buf[3];
        hdr += 2u;
        if (plen < 126u)
            return -1;           /* not minimally encoded */
    } else if (plen == 127u) {
        if (len < hdr + 8u)
            return 0;
        plen = 0;
        for (i = 0; i < 8u; ++i)
            plen = (plen << 8) | (uint64_t)buf[2 + i];
        hdr += 8u;
        if (plen <= 0xFFFFu)
            return -1;
    }

    if (plen > (uint64_t)VFX_WS_MAX_PAYLOAD)
        return -1;

    if (len < hdr + 4u)
        return 0;
    mask = buf + hdr;
    hdr += 4u;

    if (len < hdr + (size_t)plen)
        return 0;

    for (i = 0; i < plen; ++i)
        buf[hdr + i] ^= mask[i & 3u];

    out->fin         = (buf[0] & 0x80u) != 0;
    out->opcode      = (VfxWsOpcode)(buf[0] & 0x0Fu);
    out->payload     = buf + hdr;
    out->payload_len = (size_t)plen;
    out->consumed    = hdr + (size_t)plen;
    return 1;
}

size_t vfx_ws_encode(uint8_t *dst, size_t dstcap, VfxWsOpcode opcode,
                     const void *payload, size_t payload_len)
{
    size_t hdr;

    if (!dst)
        return 0;
    if (payload_len > VFX_WS_MAX_PAYLOAD)
        return 0;

    if (payload_len < 126u)      hdr = 2u;
    else if (payload_len <= 0xFFFFu) hdr = 4u;
    else                          hdr = 10u;

    if (dstcap < hdr + payload_len)
        return 0;

    dst[0] = (uint8_t)(0x80u | (unsigned)opcode);   /* FIN, no fragmentation */
    if (hdr == 2u) {
        dst[1] = (uint8_t)payload_len;
    } else if (hdr == 4u) {
        dst[1] = 126u;
        dst[2] = (uint8_t)(payload_len >> 8);
        dst[3] = (uint8_t)payload_len;
    } else {
        size_t i;
        dst[1] = 127u;
        for (i = 0; i < 8u; ++i)
            dst[2 + i] = (uint8_t)((uint64_t)payload_len >> (56u - 8u * i));
    }
    if (payload_len && payload)
        memcpy(dst + hdr, payload, payload_len);
    return hdr + payload_len;
}

/* ---------------------------------------------------------------- */
/* The pose wire format                                             */
/* ---------------------------------------------------------------- */

int vfx_wire_parse(const char *msg, size_t len, VfxWireMsg *out)
{
    char tmp[160];
    double w, x, y, z, t = 0.0;
    int n;

    if (!msg || !out)
        return 0;
    memset(out, 0, sizeof(*out));

    /* Trim leading space so a hand-typed test message still works. */
    while (len && (*msg == ' ' || *msg == '\t')) { ++msg; --len; }
    if (!len)
        return 0;
    if (len >= sizeof(tmp))
        return 0;
    memcpy(tmp, msg, len);
    tmp[len] = '\0';

    if (tmp[0] == 'R' || tmp[0] == 'r') {
        out->kind = VFX_WIRE_RECENTER;
        return 1;
    }
    if (tmp[0] != 'P' && tmp[0] != 'p')
        return 0;

    n = sscanf(tmp + 1, "%lf %lf %lf %lf %lf", &w, &x, &y, &z, &t);
    if (n < 4)
        return 0;

    out->kind = VFX_WIRE_POSE;
    out->w = (float)w;
    out->x = (float)x;
    out->y = (float)y;
    out->z = (float)z;
    out->t_ms = (n >= 5) ? t : 0.0;
    return 1;
}
