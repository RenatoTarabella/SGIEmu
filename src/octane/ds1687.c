/*
 * ds1687.c - Orologio Dallas DS1687
 */
#include <string.h>
#include "octane/ds1687.h"

enum { R_SEC = 0, R_MIN = 2, R_HOUR = 4, R_WDAY = 6, R_DATE = 7, R_MONTH = 8, R_YEAR = 9,
       R_A = 10, R_B = 11, R_C = 12, R_D = 13 };
enum { B1_MODEL = 0x40, B1_CENTURY = 0x48, B1_4A = 0x4A, B1_4B = 0x4B,
       B1_RAMADDR = 0x50, B1_RAMDATA = 0x53 };

#define A_DV0   0x10
#define B_SET   0x80
#define B_DM    0x04                   /* 1 = binario, 0 = BCD */
#define B_24H   0x02
#define X4A_VRT2 0x80
#define X4A_BME 0x20
#define X4A_PAB 0x08

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

/* valore nel formato scelto da DM (binario o BCD) */
static uint8_t enc(const Ds1687 *r, int v)
{
    return (r->reg[R_B] & B_DM) ? (uint8_t)v : (uint8_t)(((v / 10) << 4) | (v % 10));
}

static int dec(const Ds1687 *r, uint8_t v)
{
    return (r->reg[R_B] & B_DM) ? v : (v >> 4) * 10 + (v & 15);
}

static void clock_to_regs(Ds1687 *r, uint64_t now_ns)
{
    int64_t t = r->base_s + (int64_t)(now_ns / 1000000000u);
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400;
    int64_t sec = t - days * 86400;
    int y, mo, d, h = (int)(sec / 3600);
    civil_from_days(days, &y, &mo, &d);
    r->reg[R_SEC] = enc(r, (int)(sec % 60));
    r->reg[R_MIN] = enc(r, (int)(sec / 60 % 60));
    if (r->reg[R_B] & B_24H) r->reg[R_HOUR] = enc(r, h);
    else r->reg[R_HOUR] = (uint8_t)((h >= 12 ? 0x80 : 0) | enc(r, h % 12 ? h % 12 : 12));
    r->reg[R_WDAY] = enc(r, (int)((days + 4) % 7 + 1));       /* 1 = domenica */
    r->reg[R_DATE] = enc(r, d);
    r->reg[R_MONTH] = enc(r, mo);
    r->reg[R_YEAR] = enc(r, y % 100);
    r->bank1[B1_CENTURY - 0x40] = enc(r, y / 100);
}

static void regs_to_clock(Ds1687 *r, uint64_t now_ns)
{
    int y = dec(r, r->bank1[B1_CENTURY - 0x40]) * 100 + dec(r, r->reg[R_YEAR]);
    int mo = dec(r, r->reg[R_MONTH]), d = dec(r, r->reg[R_DATE]);
    int h;
    if (r->reg[R_B] & B_24H) h = dec(r, r->reg[R_HOUR]);
    else h = dec(r, r->reg[R_HOUR] & 0x7F) % 12 + ((r->reg[R_HOUR] & 0x80) ? 12 : 0);
    if (y < 1970) y += 100;
    int64_t t = days_from_civil(y, mo ? mo : 1, d ? d : 1) * 86400
              + h * 3600 + dec(r, r->reg[R_MIN]) * 60 + dec(r, r->reg[R_SEC]);
    r->base_s = t - (int64_t)(now_ns / 1000000000u);
}

void ds1687_init(Ds1687 *r, int64_t host_time)
{
    memset(r, 0, sizeof *r);
    r->base_s = host_time;
    r->reg[R_A] = 0x20;                                /* oscillatore acceso */
    r->reg[R_B] = B_24H;
    r->reg[R_D] = 0x80;                                /* VRT: batteria buona */
    r->bank1[B1_MODEL - 0x40] = 0x71;
    r->bank1[0x41 - 0x40] = 0x01;                      /* numero di serie */
    r->bank1[0x46 - 0x40] = 0x2A;
    r->bank1[B1_4A - 0x40] = X4A_VRT2 | 0x01;          /* KF: acceso dal pulsante */
}

void ds1687_select(Ds1687 *r, uint8_t index)
{
    r->index = index & 0x7F;
}

static int is_bank1(const Ds1687 *r)
{
    return r->index >= 0x40 && (r->reg[R_A] & A_DV0);
}

uint8_t ds1687_read(Ds1687 *r, uint64_t now_ns)
{
    int i = r->index;
    if (is_bank1(r)) {
        if (i == B1_CENTURY && !(r->reg[R_B] & B_SET)) clock_to_regs(r, now_ns);
        if (i == B1_RAMDATA) {
            uint8_t v = r->ext[r->bank1[B1_RAMADDR - 0x40] & 0x7F];
            if (r->bank1[B1_4A - 0x40] & X4A_BME) r->bank1[B1_RAMADDR - 0x40]++;
            return v;
        }
        return r->bank1[i - 0x40];
    }
    if (i <= R_YEAR && !(r->reg[R_B] & B_SET)) clock_to_regs(r, now_ns);
    if (i == R_C) { uint8_t v = r->reg[R_C]; r->reg[R_C] = 0; return v; }   /* si azzera leggendo */
    return r->reg[i];
}

void ds1687_write(Ds1687 *r, uint8_t v, uint64_t now_ns)
{
    int i = r->index;
    if (is_bank1(r)) {
        switch (i) {
        case B1_MODEL: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
            return;                                    /* sola lettura */
        case B1_4A: {
            uint8_t old = r->bank1[i - 0x40];
            /* RF, WF, KF si azzerano scrivendo 0; VRT2 e INCR sono di sola lettura */
            r->bank1[i - 0x40] = (uint8_t)(X4A_VRT2 | (v & 0x38) | (old & v & 0x07));
            if (v & X4A_PAB) r->power_off = 1;
            return;
        }
        case B1_RAMDATA:
            r->ext[r->bank1[B1_RAMADDR - 0x40] & 0x7F] = v;
            if (r->bank1[B1_4A - 0x40] & X4A_BME) r->bank1[B1_RAMADDR - 0x40]++;
            r->dirty = 1;
            return;
        case B1_CENTURY:
            r->bank1[i - 0x40] = v;
            if (!(r->reg[R_B] & B_SET)) regs_to_clock(r, now_ns);
            return;
        default:
            r->bank1[i - 0x40] = v;
            return;
        }
    }
    switch (i) {
    case R_C: case R_D: return;                        /* sola lettura */
    case R_A: r->reg[i] = v & 0x7F; return;            /* UIP e' di sola lettura */
    case R_B:
        if ((r->reg[R_B] & B_SET) && !(v & B_SET)) { r->reg[R_B] = v; regs_to_clock(r, now_ns); return; }
        if (!(r->reg[R_B] & B_SET) && (v & B_SET)) clock_to_regs(r, now_ns);   /* congela l'ora */
        r->reg[R_B] = v;
        return;
    default:
        if (i <= R_YEAR && !(r->reg[R_B] & B_SET)) clock_to_regs(r, now_ns);
        r->reg[i] = v;
        if (i <= R_YEAR && i != 1 && i != 3 && i != 5 && !(r->reg[R_B] & B_SET)) regs_to_clock(r, now_ns);
        if (i > R_D) r->dirty = 1;
        return;
    }
}
