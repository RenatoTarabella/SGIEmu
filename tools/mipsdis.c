/*
 * mipsdis.c - Disassembla un file binario big-endian (PROM, kernel grezzo)
 *
 *   mipsdis FILE [BASE] [DA] [QUANTE]
 *
 * BASE e' l'indirizzo virtuale del primo byte (default 0xBFC00000),
 * DA l'indirizzo da cui partire, QUANTE il numero di istruzioni.
 */
#include <stdio.h>
#include <stdlib.h>
#include "cpu/r4k_disasm.h"

int main(int argc, char **argv)
{
    if (argc < 2) { printf("uso: mipsdis FILE [BASE] [DA] [QUANTE]\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { printf("non trovo %s\n", argv[1]); return 1; }
    uint64_t base = argc > 2 ? strtoull(argv[2], NULL, 16) : 0xBFC00000ull;
    uint64_t from = argc > 3 ? strtoull(argv[3], NULL, 16) : base;
    long count = argc > 4 ? strtol(argv[4], NULL, 0) : 64;
    base = (uint64_t)(int64_t)(int32_t)base;
    from = (uint64_t)(int64_t)(int32_t)from;
    fseek(f, (long)(from - base), SEEK_SET);
    unsigned char b[4];
    char buf[96];
    for (long k = 0; k < count && fread(b, 1, 4, f) == 4; k++) {
        uint32_t w = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
        uint64_t pc = from + 4 * (uint64_t)k;
        r4k_disasm(pc, w, buf, sizeof buf);
        printf("%08llX  %08X  %s\n", (unsigned long long)(uint32_t)pc, w, buf);
    }
    fclose(f);
    return 0;
}
