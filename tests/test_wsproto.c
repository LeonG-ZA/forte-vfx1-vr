/*
 * The browser-to-emulator wire: handshake, RFC 6455 framing, pose messages.
 *
 * This code sits on a network boundary and parses attacker-shaped input, so
 * the malformed cases matter as much as the happy path.
 */
#include "vfxtest.h"
#include "../src/wsproto/wsproto.h"
#include <string.h>

static void hex(const uint8_t *d, size_t n, char *out)
{
    static const char *h = "0123456789abcdef";
    size_t i;
    for (i = 0; i < n; ++i) {
        out[i * 2]     = h[d[i] >> 4];
        out[i * 2 + 1] = h[d[i] & 0xF];
    }
    out[n * 2] = '\0';
}

int main(void)
{
    /* ---- SHA-1, against the RFC 3174 vectors ---- */
    {
        uint8_t d[20];
        char s[41];

        vfx_sha1("abc", 3, d);
        hex(d, 20, s);
        CHECK(strcmp(s, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0);

        vfx_sha1("", 0, d);
        hex(d, 20, s);
        CHECK(strcmp(s, "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0);

        vfx_sha1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, d);
        hex(d, 20, s);
        CHECK(strcmp(s, "84983e441c3bd26ebaae4aa1f95129e5e54670f1") == 0);

        /* A million 'a' would be slow here; 1000 exercises multi-block just
         * as well and has a known digest. */
        {
            char big[1000];
            memset(big, 'a', sizeof(big));
            vfx_sha1(big, sizeof(big), d);
            hex(d, 20, s);
            CHECK(strcmp(s, "291e9a6c66994949b57ba5e650361e98fc36b1ba") == 0);
        }
    }

    /* ---- base64 ---- */
    {
        char b[64];
        CHECK(vfx_base64_encode((const uint8_t *)"", 0, b, sizeof(b)) == 0);
        CHECK(strcmp(b, "") == 0);
        vfx_base64_encode((const uint8_t *)"f", 1, b, sizeof(b));
        CHECK(strcmp(b, "Zg==") == 0);
        vfx_base64_encode((const uint8_t *)"fo", 2, b, sizeof(b));
        CHECK(strcmp(b, "Zm8=") == 0);
        vfx_base64_encode((const uint8_t *)"foo", 3, b, sizeof(b));
        CHECK(strcmp(b, "Zm9v") == 0);
        vfx_base64_encode((const uint8_t *)"foobar", 6, b, sizeof(b));
        CHECK(strcmp(b, "Zm9vYmFy") == 0);
        /* Refuses to overflow rather than truncating. */
        CHECK(vfx_base64_encode((const uint8_t *)"foobar", 6, b, 4) == 0);
    }

    /* ---- the handshake vector from RFC 6455 section 1.3 ---- */
    {
        char acc[VFX_WS_ACCEPT_LEN];
        CHECK(vfx_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc, sizeof(acc)) == 1);
        CHECK(strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0);
        CHECK(vfx_ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc, 8) == 0);
        CHECK(vfx_ws_accept_key(NULL, acc, sizeof(acc)) == 0);
    }

    /* ---- HTTP request parsing ---- */
    {
        static const char req[] =
            "GET /pose HTTP/1.1\r\n"
            "Host: localhost:8085\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key:   dGhlIHNhbXBsZSBub25jZQ==  \r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";
        char v[128];

        CHECK(vfx_http_headers_complete(req, strlen(req)) == 1);
        CHECK(vfx_http_headers_complete(req, 20) == 0);

        CHECK(vfx_http_request_target(req, "GET", v, sizeof(v)) == 1);
        CHECK(strcmp(v, "/pose") == 0);
        CHECK(vfx_http_request_target(req, "POST", v, sizeof(v)) == 0);

        /* Header lookup is case-insensitive and trims the value. */
        CHECK(vfx_http_header(req, "sec-websocket-key", v, sizeof(v)) == 1);
        CHECK(strcmp(v, "dGhlIHNhbXBsZSBub25jZQ==") == 0);
        CHECK(vfx_http_header(req, "UPGRADE", v, sizeof(v)) == 1);
        CHECK(strcmp(v, "websocket") == 0);
        CHECK(vfx_http_header(req, "X-Absent", v, sizeof(v)) == 0);
        /* A too-small buffer fails rather than truncating. */
        CHECK(vfx_http_header(req, "Host", v, 4) == 0);
    }

    /* ---- decoding a masked client text frame ---- */
    {
        /* "Hello" masked with 37 fa 21 3d, the example from RFC 6455 5.7. */
        uint8_t f[] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                        0x7f, 0x9f, 0x4d, 0x51, 0x58 };
        VfxWsFrame fr;
        CHECK(vfx_ws_decode(f, sizeof(f), &fr) == 1);
        CHECK(fr.fin == 1);
        CHECK(fr.opcode == VFX_WS_TEXT);
        CHECK(fr.payload_len == 5);
        CHECK(memcmp(fr.payload, "Hello", 5) == 0);
        CHECK(fr.consumed == sizeof(f));
    }

    /* ---- partial input asks for more rather than guessing ---- */
    {
        uint8_t f[] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                        0x7f, 0x9f, 0x4d, 0x51, 0x58 };
        VfxWsFrame fr;
        size_t n;
        for (n = 0; n < sizeof(f); ++n) {
            uint8_t copy[sizeof(f)];
            memcpy(copy, f, sizeof(f));
            CHECK(vfx_ws_decode(copy, n, &fr) == 0);
        }
    }

    /* ---- an unmasked client frame is a protocol error ---- */
    {
        uint8_t f[] = { 0x81, 0x05, 'H', 'e', 'l', 'l', 'o' };
        VfxWsFrame fr;
        CHECK(vfx_ws_decode(f, sizeof(f), &fr) == -1);
    }

    /* ---- reserved bits set is a protocol error ---- */
    {
        uint8_t f[] = { 0xD1, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                        0x7f, 0x9f, 0x4d, 0x51, 0x58 };
        VfxWsFrame fr;
        CHECK(vfx_ws_decode(f, sizeof(f), &fr) == -1);
    }

    /* ---- non-minimal length encodings are rejected ---- */
    {
        VfxWsFrame fr;
        uint8_t small_in_16[] = { 0x81, 0xFE, 0x00, 0x05, 0, 0, 0, 0,
                                  'H', 'e', 'l', 'l', 'o' };
        uint8_t small_in_64[] = { 0x81, 0xFF, 0, 0, 0, 0, 0, 0, 0x00, 0x05,
                                  0, 0, 0, 0, 'H', 'e', 'l', 'l', 'o' };
        CHECK(vfx_ws_decode(small_in_16, sizeof(small_in_16), &fr) == -1);
        CHECK(vfx_ws_decode(small_in_64, sizeof(small_in_64), &fr) == -1);
    }

    /* ---- an absurd declared length is refused, not allocated for ---- */
    {
        uint8_t f[] = { 0x82, 0xFF, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                        0xFF, 0, 0, 0, 0 };
        VfxWsFrame fr;
        CHECK(vfx_ws_decode(f, sizeof(f), &fr) == -1);
    }

    /* ---- a 126-byte payload uses the 16-bit length correctly ---- */
    {
        uint8_t f[4 + 4 + 200];
        VfxWsFrame fr;
        size_t i;
        memset(f, 0, sizeof(f));
        f[0] = 0x81;
        f[1] = 0xFE;
        f[2] = 0x00; f[3] = 0x7E;              /* 126 */
        f[4] = f[5] = f[6] = f[7] = 0x00;      /* mask of zeroes */
        for (i = 0; i < 126u; ++i)
            f[8 + i] = (uint8_t)('a' + (i % 26u));
        CHECK(vfx_ws_decode(f, 8u + 126u, &fr) == 1);
        CHECK(fr.payload_len == 126u);
        CHECK(fr.payload[0] == 'a');
        CHECK(fr.consumed == 8u + 126u);
    }

    /* ---- two frames back to back: consumed says where the next begins ---- */
    {
        uint8_t two[22];
        static const uint8_t one[] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d,
                                       0x7f, 0x9f, 0x4d, 0x51, 0x58 };
        VfxWsFrame fr;
        memcpy(two, one, 11);
        memcpy(two + 11, one, 11);
        CHECK(vfx_ws_decode(two, sizeof(two), &fr) == 1);
        CHECK(fr.consumed == 11u);
        CHECK(vfx_ws_decode(two + fr.consumed, sizeof(two) - fr.consumed, &fr) == 1);
        CHECK(memcmp(fr.payload, "Hello", 5) == 0);
    }

    /* ---- encoding, server to client, never masked ---- */
    {
        uint8_t b[300];
        size_t n = vfx_ws_encode(b, sizeof(b), VFX_WS_TEXT, "Hello", 5);
        CHECK(n == 7u);
        CHECK(b[0] == 0x81);
        CHECK(b[1] == 0x05);          /* mask bit clear */
        CHECK(memcmp(b + 2, "Hello", 5) == 0);

        {
            /* A separate source: payload and dst must not overlap. */
            uint8_t src[200];
            memset(src, 0x5A, sizeof(src));
            n = vfx_ws_encode(b, sizeof(b), VFX_WS_BINARY, src, sizeof(src));
        }
        CHECK(n == 4u + 200u);
        CHECK(b[1] == 126u);

        CHECK(vfx_ws_encode(b, 3, VFX_WS_TEXT, "Hello", 5) == 0);
        CHECK(vfx_ws_encode(b, sizeof(b), VFX_WS_PONG, NULL, 0) == 2u);
    }

    /* ---- a round trip through both halves ---- */
    {
        uint8_t enc[64];
        VfxWsFrame fr;
        size_t n = vfx_ws_encode(enc, sizeof(enc), VFX_WS_TEXT, "P 1 0 0 0", 9);
        /* Flip it to a masked client frame so the decoder will take it. */
        uint8_t cli[64];
        size_t i;
        cli[0] = enc[0];
        cli[1] = (uint8_t)(enc[1] | 0x80u);
        cli[2] = 0xAA; cli[3] = 0xBB; cli[4] = 0xCC; cli[5] = 0xDD;
        for (i = 0; i < n - 2u; ++i)
            cli[6 + i] = enc[2 + i] ^ cli[2 + (i & 3u)];
        CHECK(vfx_ws_decode(cli, 6u + n - 2u, &fr) == 1);
        CHECK(fr.payload_len == 9u);
        CHECK(memcmp(fr.payload, "P 1 0 0 0", 9) == 0);
    }

    /* ---- the pose wire format ---- */
    {
        VfxWireMsg m;
        const char *p;

        p = "P 0.7071 0 0.7071 0 1234.5";
        CHECK(vfx_wire_parse(p, strlen(p), &m) == 1);
        CHECK(m.kind == VFX_WIRE_POSE);
        CHECK_NEAR(m.w, 0.7071, 1e-5);
        CHECK_NEAR(m.y, 0.7071, 1e-5);
        CHECK_NEAR(m.t_ms, 1234.5, 1e-6);

        /* The timestamp is optional. */
        p = "P 1 0 0 0";
        CHECK(vfx_wire_parse(p, strlen(p), &m) == 1);
        CHECK(m.kind == VFX_WIRE_POSE);
        CHECK_NEAR(m.t_ms, 0.0, 1e-9);

        /* Negative components and exponents survive. */
        p = "P -0.5 1e-3 -2.5e-1 0.25 99";
        CHECK(vfx_wire_parse(p, strlen(p), &m) == 1);
        CHECK_NEAR(m.w, -0.5, 1e-6);
        CHECK_NEAR(m.x, 0.001, 1e-6);
        CHECK_NEAR(m.y, -0.25, 1e-6);

        p = "R";
        CHECK(vfx_wire_parse(p, strlen(p), &m) == 1);
        CHECK(m.kind == VFX_WIRE_RECENTER);

        /* Not NUL-terminated: len is what bounds it. */
        {
            const char raw[] = "P 1 0 0 0GARBAGE";
            CHECK(vfx_wire_parse(raw, 9, &m) == 1);
            CHECK(m.kind == VFX_WIRE_POSE);
            CHECK_NEAR(m.w, 1.0, 1e-6);
        }

        /* Malformed input is refused, not half-parsed. */
        CHECK(vfx_wire_parse("P 1 0 0", 7, &m) == 0);
        CHECK(vfx_wire_parse("Z 1 0 0 0", 9, &m) == 0);
        CHECK(vfx_wire_parse("", 0, &m) == 0);
        CHECK(vfx_wire_parse(NULL, 0, &m) == 0);
        CHECK(vfx_wire_parse("P", 1, &m) == 0);
        /* Absurdly long input is refused rather than overflowing. */
        {
            char big[512];
            memset(big, 'P', sizeof(big));
            CHECK(vfx_wire_parse(big, sizeof(big), &m) == 0);
        }
    }

    TEST_MAIN_END();
}
