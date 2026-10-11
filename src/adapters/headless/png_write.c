/*
 * png_write.c -- RGBA PNG writer using stored (uncompressed) deflate blocks.
 *
 * No zlib dependency: the headless adapter only needs its output to be
 * viewable, not small.
 */
#include "png_write.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t crc_table[256];
static int      crc_ready;

static void crc_init(void)
{
    uint32_t n, c, k;
    for (n = 0; n < 256u; ++n) {
        c = n;
        for (k = 0; k < 8u; ++k)
            c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static uint32_t crc32_buf(const uint8_t *p, size_t n, uint32_t c)
{
    size_t i;
    if (!crc_ready)
        crc_init();
    for (i = 0; i < n; ++i)
        c = crc_table[(c ^ p[i]) & 0xffu] ^ (c >> 8);
    return c;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int write_chunk(FILE *f, const char *tag, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    uint8_t crcb[4];
    uint32_t c;

    put_be32(hdr, len);
    memcpy(hdr + 4, tag, 4);
    if (fwrite(hdr, 1, 8, f) != 8)
        return 0;
    if (len && fwrite(data, 1, len, f) != len)
        return 0;

    c = crc32_buf((const uint8_t *)tag, 4, 0xffffffffu);
    if (len)
        c = crc32_buf(data, len, c);
    put_be32(crcb, c ^ 0xffffffffu);
    return fwrite(crcb, 1, 4, f) == 4;
}

int vfx_png_write_rgba(const char *path, const uint8_t *rgba,
                       uint32_t w, uint32_t h)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    uint8_t ihdr[13];
    uint8_t *raw = NULL, *z = NULL;
    size_t raw_len, z_len, zi = 0, off;
    uint32_t a = 1u, b = 0u;
    uint32_t y;
    size_t i;
    FILE *f;
    int ok = 0;

    if (!path || !rgba || !w || !h)
        return 0;

    raw_len = (size_t)h * (1u + (size_t)w * 4u);
    raw = (uint8_t *)malloc(raw_len);
    if (!raw)
        return 0;
    for (y = 0; y < h; ++y) {
        uint8_t *dst = raw + (size_t)y * (1u + (size_t)w * 4u);
        dst[0] = 0u; /* filter: none */
        memcpy(dst + 1, rgba + (size_t)y * w * 4u, (size_t)w * 4u);
    }

    /* zlib header + stored deflate blocks + adler32 */
    z_len = 2u + ((raw_len + 65534u) / 65535u) * 5u + raw_len + 4u;
    z = (uint8_t *)malloc(z_len);
    if (!z) { free(raw); return 0; }

    z[zi++] = 0x78u; /* CMF: deflate, 32K window */
    z[zi++] = 0x01u; /* FLG: no dict, fastest */

    for (off = 0; off < raw_len; ) {
        size_t n = raw_len - off;
        int final;
        if (n > 65535u)
            n = 65535u;
        final = (off + n >= raw_len) ? 1 : 0;
        z[zi++] = (uint8_t)final;
        z[zi++] = (uint8_t)(n & 0xffu);
        z[zi++] = (uint8_t)((n >> 8) & 0xffu);
        z[zi++] = (uint8_t)(~n & 0xffu);
        z[zi++] = (uint8_t)((~n >> 8) & 0xffu);
        memcpy(z + zi, raw + off, n);
        zi += n;
        off += n;
    }

    for (i = 0; i < raw_len; ++i) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    put_be32(z + zi, (b << 16) | a);
    zi += 4u;

    f = fopen(path, "wb");
    if (!f)
        goto done;
    if (fwrite(sig, 1, 8, f) != 8)
        goto close_done;

    put_be32(ihdr, w);
    put_be32(ihdr + 4, h);
    ihdr[8]  = 8u; /* bit depth */
    ihdr[9]  = 6u; /* colour type: RGBA */
    ihdr[10] = 0u;
    ihdr[11] = 0u;
    ihdr[12] = 0u;

    if (!write_chunk(f, "IHDR", ihdr, 13u))
        goto close_done;
    if (!write_chunk(f, "IDAT", z, (uint32_t)zi))
        goto close_done;
    if (!write_chunk(f, "IEND", NULL, 0u))
        goto close_done;
    ok = 1;

close_done:
    fclose(f);
done:
    free(raw);
    free(z);
    return ok;
}
