/*
 * png.c - Scrittura di PNG RGB senza compressione (blocchi "stored"),
 * per le istantanee dello schermo senza dipendenze esterne.
 */
#include "png.h"
#include <stdio.h>
#include <stdlib.h>

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    while (n--) crc = crc_table[(crc ^ *p++) & 255] ^ (crc >> 8);
    return crc;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static void chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    put32(hdr, len);
    for (int i = 0; i < 4; i++) hdr[4 + i] = (uint8_t)type[i];
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t crc = crc_update(0xFFFFFFFFu, hdr + 4, 4);
    crc = crc_update(crc, data, len) ^ 0xFFFFFFFFu;
    uint8_t c[4];
    put32(c, crc);
    fwrite(c, 1, 4, f);
}

int png_write(const char *path, const uint32_t *rgb, int w, int h, int yscale)
{
    crc_init();
    if (yscale < 1) yscale = 1;
    int oh = h * yscale;
    size_t row = (size_t)w * 3 + 1;
    size_t raw_len = row * (size_t)oh;
    uint8_t *raw = malloc(raw_len);
    if (!raw) return 0;
    for (int y = 0; y < oh; y++) {
        uint8_t *r = raw + (size_t)y * row;
        const uint32_t *src = rgb + (size_t)(y / yscale) * w;
        r[0] = 0;
        for (int x = 0; x < w; x++) {
            r[1 + 3 * x] = (uint8_t)(src[x] >> 16);
            r[2 + 3 * x] = (uint8_t)(src[x] >> 8);
            r[3 + 3 * x] = (uint8_t)src[x];
        }
    }
    /* zlib: intestazione, blocchi stored da 65535 byte, adler32 */
    size_t nblocks = (raw_len + 65534) / 65535;
    size_t z_len = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *z = malloc(z_len);
    if (!z) { free(raw); return 0; }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i += 65535) {
        size_t n = raw_len - i < 65535 ? raw_len - i : 65535;
        z[o++] = (uint8_t)(i + n == raw_len);
        z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        for (size_t j = 0; j < n; j++) {
            uint8_t c = raw[i + j];
            z[o++] = c;
            a = (a + c) % 65521;
            b = (b + a) % 65521;
        }
    }
    put32(z + o, b << 16 | a);
    o += 4;

    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return 0; }
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    put32(ihdr, (uint32_t)w);
    put32(ihdr + 4, (uint32_t)oh);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (uint32_t)o);
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 1;
}
