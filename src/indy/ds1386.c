/*
 * ds1386.c - Orologio DS1386 e NVRAM
 */
#include <string.h>
#include <time.h>
#include "indy/ds1386.h"

enum { R_HSEC = 0, R_SEC, R_MIN, R_MINA, R_HOUR, R_HOURA, R_DAY, R_DAYA,
       R_DATE, R_MONTH, R_YEAR, R_CMD, R_WHSEC, R_WSEC };
#define CMD_TE 0x80

static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

/* giorni dall'epoch per una data del calendario gregoriano */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

static void clock_to_regs(Ds1386 *r, uint64_t now_ns)
{
    int64_t t = r->base_s + (int64_t)(now_ns / 1000000000u);
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int64_t sec = t - days * 86400;
    int y, mo, d;
    civil_from_days(days, &y, &mo, &d);
    r->ram[R_HSEC] = bcd((int)(now_ns / 10000000u % 100));
    r->ram[R_SEC] = bcd((int)(sec % 60));
    r->ram[R_MIN] = bcd((int)(sec / 60 % 60));
    r->ram[R_HOUR] = (uint8_t)((r->ram[R_HOUR] & 0x40 ? 0x40 : 0) | bcd((int)(sec / 3600)));
    r->ram[R_DAY] = bcd((int)((days + 4) % 7 + 1));       /* 1 = domenica */
    r->ram[R_DATE] = bcd(d);
    r->ram[R_MONTH] = (uint8_t)((r->ram[R_MONTH] & 0xC0) | bcd(mo));
    r->ram[R_YEAR] = bcd((y - 1940) % 100);
}

static void regs_to_clock(Ds1386 *r, uint64_t now_ns)
{
    int y = 1940 + unbcd(r->ram[R_YEAR]);
    int mo = unbcd(r->ram[R_MONTH] & 0x1F), d = unbcd(r->ram[R_DATE] & 0x3F);
    int64_t t = days_from_civil(y, mo ? mo : 1, d ? d : 1) * 86400
              + unbcd(r->ram[R_HOUR] & 0x3F) * 3600 + unbcd(r->ram[R_MIN]) * 60 + unbcd(r->ram[R_SEC]);
    r->base_s = t - (int64_t)(now_ns / 1000000000u);
}

/* Checksum dell'ambiente della PROM: 256 byte da NVRAM_BASE, saltando il
   byte del checksum; rotazione a sinistra e XOR, partendo da 0x8B.
   Ricavato da due NVRAM valide (quella di MAME e una scritta dalla PROM). */
void rtc_fix_checksum(Ds1386 *r)
{
    uint8_t c = 0x8B;
    for (int off = 1; off < 256; off++) {
        c = (uint8_t)((c << 1) | (c >> 7));
        c ^= r->ram[NVRAM_BASE + off];
    }
    r->ram[NVRAM_BASE] = c;
}

void rtc_init(Ds1386 *r, int64_t host_time)
{
    r->base_s = host_time;
    r->ram[R_CMD] |= CMD_TE;
    r->ram[R_HOUR] &= ~0x40;                                  /* 24 ore */
    r->ram[R_MONTH] &= ~0x80;                                 /* oscillatore acceso */
    r->dirty = 0;
}

uint8_t rtc_read(Ds1386 *r, int reg, uint64_t now_ns)
{
    reg &= DS1386_SIZE - 1;
    if (reg <= R_YEAR && reg != R_MINA && reg != R_HOURA && reg != R_DAYA && (r->ram[R_CMD] & CMD_TE))
        clock_to_regs(r, now_ns);
    return r->ram[reg];
}

void rtc_write(Ds1386 *r, int reg, uint8_t v, uint64_t now_ns)
{
    reg &= DS1386_SIZE - 1;
    if (reg == R_CMD) {
        int te_on = !(r->ram[R_CMD] & CMD_TE) && (v & CMD_TE);
        int te_off = (r->ram[R_CMD] & CMD_TE) && !(v & CMD_TE);
        if (te_off) clock_to_regs(r, now_ns);                 /* i registri si fermano qui */
        r->ram[R_CMD] = v;
        if (te_on) regs_to_clock(r, now_ns);                  /* ora impostata dal programma */
        return;
    }
    if (reg <= R_YEAR && (r->ram[R_CMD] & CMD_TE) && reg != R_MINA && reg != R_HOURA && reg != R_DAYA) {
        /* scrittura con TE = 1: l'orologio prende subito il nuovo valore */
        clock_to_regs(r, now_ns);
        r->ram[reg] = v;
        regs_to_clock(r, now_ns);
        return;
    }
    if (reg > R_WSEC && r->ram[reg] != v) r->dirty = 1;
    r->ram[reg] = v;
}
