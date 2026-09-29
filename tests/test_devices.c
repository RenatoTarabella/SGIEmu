/*
 * test_devices.c - Test dei dispositivi dell'Indy presi uno per uno:
 * EEPROM 93CS56, timer 8254, orologio DS1386, seriale Z85C30, SCSI
 * WD33C93, controller di tastiera 8042.
 */
#include <stdio.h>
#include <string.h>
#include "indy/eeprom93.h"
#include "indy/pit8254.h"
#include "indy/ds1386.h"
#include "indy/z85c30.h"
#include "indy/wd33c93.h"
#include "indy/kbc8042.h"

static int failures, checks;
#define CHECK_EQ(a, b) do { long long a_ = (long long)(a), b_ = (long long)(b); checks++; \
    if (a_ != b_) { failures++; printf("  FALLITO %s:%d: %s = %llX, atteso %llX\n", \
        __FILE__, __LINE__, #a, a_, b_); } } while (0)

/* lettura come in Linux (ip22-nvram.c): start, READ, 8 bit di indirizzo, 16 di dati */
static uint16_t ee_linux_read(Eeprom93 *e, int reg)
{
    uint16_t cmd = (uint16_t)(0xC000 | (reg << 5)), res = 0;
    ee_pins(e, 1, 1, 0);
    for (int i = 0; i < 11; i++) {
        int di = (cmd >> 15) & 1;
        ee_pins(e, 1, 0, di);
        ee_pins(e, 1, 1, di);
        cmd <<= 1;
    }
    for (int i = 0; i < 16; i++) {
        ee_pins(e, 1, 0, 0);
        ee_pins(e, 1, 1, 0);
        res = (uint16_t)((res << 1) | ee_dout(e));
    }
    ee_pins(e, 0, 0, 0);
    return res;
}

static void ee_send(Eeprom93 *e, uint32_t bits, int n)
{
    for (int i = n - 1; i >= 0; i--) {
        int di = (bits >> i) & 1;
        ee_pins(e, 1, 0, di);
        ee_pins(e, 1, 1, di);
    }
}

static void test_eeprom(void)
{
    printf("EEPROM 93CS56\n");
    static Eeprom93 e;
    ee_reset(&e);
    for (int k = 0; k < 128; k++) e.data[k] = (uint16_t)(0x1000 + k);
    CHECK_EQ(ee_linux_read(&e, 5), 0x1005);
    CHECK_EQ(ee_linux_read(&e, 127), 0x107F);
    /* scrittura: EWEN (100 11xxxxxx), WRITE (101 aaaaaaaa dddd) */
    ee_send(&e, 0x4C0, 11); ee_pins(&e, 0, 0, 0);
    ee_send(&e, (0x5u << 8) | 9, 11); ee_send(&e, 0xBEEF, 16); ee_pins(&e, 0, 0, 0);
    CHECK_EQ(ee_linux_read(&e, 9), 0xBEEF);
    CHECK_EQ(e.dirty, 1);
}

static void test_pit(void)
{
    printf("8254\n");
    static Pit8254 p;
    pit_reset(&p);
    uint64_t t = 1000000;                              /* 1 ms */
    pit_write(&p, 3, 0xB4, t);                          /* canale 2, LSB+MSB, modo 2 */
    pit_write(&p, 2, 100, t);
    pit_write(&p, 2, 0, t);                             /* 100 tick = 100 us */
    pit_write(&p, 3, 0x80, t + 30000);                  /* latch del canale 2 */
    int lo = pit_read(&p, 2, t + 30000), hi = pit_read(&p, 2, t + 30000);
    CHECK_EQ(lo | hi << 8, 70);
    pit_write(&p, 3, 0x34, t);                          /* canale 0 contato dal 2 */
    pit_write(&p, 0, 10, t);
    pit_write(&p, 0, 0, t);                             /* 10 x 100 us = 1 ms */
    CHECK_EQ(pit_poll(&p, 0, t + 999000), 0);
    CHECK_EQ(pit_poll(&p, 0, t + 1001000), 1);
    CHECK_EQ(pit_poll(&p, 0, t + 5001000), 4);
}

static void test_rtc(void)
{
    printf("DS1386\n");
    static Ds1386 r;
    memset(&r, 0, sizeof r);
    rtc_init(&r, 1790686800);                          /* 2026-09-29 13:00:00 UTC */
    CHECK_EQ(rtc_read(&r, 10, 0), 0x86);                /* 2026 - 1940 */
    CHECK_EQ(rtc_read(&r, 9, 0), 0x09);
    CHECK_EQ(rtc_read(&r, 8, 0), 0x29);
    CHECK_EQ(rtc_read(&r, 4, 0), 0x13);
    CHECK_EQ(rtc_read(&r, 2, 90ull * 1000000000u), 0x01);   /* 90 s dopo: 13:01:30 */
    /* TE = 0 ferma i registri, si scrive l'ora, TE = 1 la fa ripartire */
    rtc_write(&r, 11, 0x00, 0);
    rtc_write(&r, 4, 0x08, 0);
    rtc_write(&r, 11, 0x80, 0);
    CHECK_EQ(rtc_read(&r, 4, 3600ull * 1000000000u), 0x09);
    rtc_write(&r, 100, 0x5A, 0);
    CHECK_EQ(rtc_read(&r, 100, 0), 0x5A);
    CHECK_EQ(r.dirty, 1);
}

static char txbuf[64];
static int txlen;
static void tx(void *u, int ch, uint8_t c) { (void)u; (void)ch; if (txlen < 63) txbuf[txlen++] = (char)c; }

static void test_scc(void)
{
    printf("Z85C30\n");
    static Z85c30 s;
    scc_reset(&s);
    s.tx = tx;
    CHECK_EQ(scc_read(&s, 1, 0) & 0x04, 0x04);         /* RR0: Tx vuoto */
    scc_write(&s, 1, 1, 'O'); scc_write(&s, 1, 1, 'K');
    CHECK_EQ(txlen, 2);
    CHECK_EQ(txbuf[1], 'K');
    scc_rx(&s, 1, 'x');
    CHECK_EQ(scc_read(&s, 1, 0) & 0x01, 1);            /* Rx disponibile */
    CHECK_EQ(scc_read(&s, 1, 1), 'x');
    CHECK_EQ(scc_read(&s, 1, 0) & 0x01, 0);
    /* interrupt in ricezione: WR1 = Rx su tutti i caratteri, WR9 = MIE */
    scc_write(&s, 1, 0, 1); scc_write(&s, 1, 0, 0x10);
    scc_write(&s, 1, 0, 9); scc_write(&s, 1, 0, 0x08);
    CHECK_EQ(scc_irq(&s), 0);
    scc_rx(&s, 1, 'y');
    CHECK_EQ(scc_irq(&s), 1);
    scc_write(&s, 0, 0, 3);
    CHECK_EQ(scc_read(&s, 0, 0), 0x04);                /* RR3: Rx del canale B */
}

static void test_wd(void)
{
    printf("WD33C93\n");
    static Wd33c93 w;
    wd_reset(&w);
    wd_write(&w, 0, 0x00); wd_write(&w, 1, 0x08 | 7);   /* own ID 7, EAF */
    wd_write(&w, 0, 0x18); wd_write(&w, 1, 0x00);       /* reset */
    CHECK_EQ(wd_read(&w, 0) & 0x80, 0x80);
    wd_write(&w, 0, 0x17);
    CHECK_EQ(wd_read(&w, 1), 0x01);
    CHECK_EQ(w.irq, 0);
    wd_write(&w, 0, 0x15); wd_write(&w, 1, 3);           /* target 3, niente */
    wd_write(&w, 0, 0x18); wd_write(&w, 1, 0x08);        /* select and transfer */
    wd_write(&w, 0, 0x17);
    CHECK_EQ(wd_read(&w, 1), 0x42);                      /* timeout di selezione */
}

static void test_kbc(void)
{
    printf("8042\n");
    static Kbc8042 k;
    kbc_reset(&k);
    kbc_write(&k, 1, 0xAA);
    CHECK_EQ(kbc_read(&k, 1) & 1, 1);
    CHECK_EQ(kbc_read(&k, 0), 0x55);
    kbc_write(&k, 0, 0xFF);                              /* reset della tastiera */
    CHECK_EQ(kbc_read(&k, 0), 0xFA);
    CHECK_EQ(kbc_read(&k, 0), 0xAA);
    kbc_write(&k, 1, 0xD4); kbc_write(&k, 0, 0xF2);      /* identificazione del mouse */
    CHECK_EQ(kbc_read(&k, 1) & 0x20, 0x20);
    CHECK_EQ(kbc_read(&k, 0), 0xFA);
    CHECK_EQ(kbc_read(&k, 0), 0x00);
}

int main(void)
{
    test_eeprom();
    test_pit();
    test_rtc();
    test_scc();
    test_wd();
    test_kbc();
    printf("%d controlli, %d falliti\n", checks, failures);
    return failures != 0;
}
