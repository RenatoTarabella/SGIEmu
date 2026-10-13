/*
 * test_devices.c - Test dei dispositivi dell'Indy presi uno per uno:
 * EEPROM 93CS56, timer 8254, orologio DS1386, seriale Z85C30, SCSI
 * WD33C93, controller di tastiera 8042.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "indy/eeprom93.h"
#include "indy/pit8254.h"
#include "indy/ds1386.h"
#include "indy/z85c30.h"
#include "indy/wd33c93.h"
#include "indy/kbc8042.h"
#include "octane/ds1687.h"

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

/* disco su un file temporaneo, READ(10) del blocco 2 letto dal registro dati */
static void test_scsi_disk(void)
{
    printf("disco SCSI\n");
    const char *path = "test_scsi_disk.img";
    FILE *f = fopen(path, "wb");
    for (int b = 0; b < 8; b++) for (int k = 0; k < 512; k++) fputc((b * 16 + k) & 0xFF, f);
    fclose(f);
    static ScsiDev d;
    static Wd33c93 w;
    CHECK_EQ(scsi_dev_open(&d, SCSI_DISK, path, 0), 0);
    CHECK_EQ(d.blocks, 8);
    wd_init(&w);
    w.dev[1] = &d;
    static const uint8_t cdb[10] = { 0x28, 0, 0, 0, 0, 2, 0, 0, 1, 0 };
    wd_write(&w, 0, WD_CDB1);
    for (int k = 0; k < 10; k++) wd_write(&w, 1, cdb[k]);          /* autoincremento */
    wd_write(&w, 0, WD_COUNT_HI); wd_write(&w, 1, 0); wd_write(&w, 1, 2); wd_write(&w, 1, 0);
    wd_write(&w, 1, 1);                                              /* DEST_ID = 1 */
    wd_write(&w, 0, WD_COMMAND); wd_write(&w, 1, 0x08);              /* select-and-transfer */
    CHECK_EQ(wd_read(&w, 0) & 0x01, 1);                              /* DBR: dati pronti */
    wd_write(&w, 0, WD_DATA);
    int ok = 1;
    for (int k = 0; k < 512; k++) ok &= wd_read(&w, 1) == ((32 + k) & 0xFF);
    CHECK_EQ(ok, 1);
    CHECK_EQ(wd_read(&w, 0) & 0x80, 0x80);                           /* interrupt di fine */
    wd_write(&w, 0, WD_TARGET_LUN);
    CHECK_EQ(wd_read(&w, 1), 0);                                      /* stato GOOD */
    wd_write(&w, 0, WD_STATUS);
    CHECK_EQ(wd_read(&w, 1), 0x16);
    /* lettura a pezzi: TC = 256 byte per un comando da 1024, come fa IRIX */
    static const uint8_t cdb2[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 2, 0 };
    wd_write(&w, 0, WD_CDB1);
    for (int k = 0; k < 10; k++) wd_write(&w, 1, cdb2[k]);
    wd_write(&w, 0, WD_TARGET_LUN); wd_write(&w, 1, 0); wd_write(&w, 1, 0);   /* LUN, fase 0 */
    wd_write(&w, 0, WD_COUNT_HI); wd_write(&w, 1, 0); wd_write(&w, 1, 1); wd_write(&w, 1, 0);
    wd_write(&w, 0, WD_COMMAND); wd_write(&w, 1, 0x08);
    wd_write(&w, 0, WD_DATA);
    ok = 1;
    for (int k = 0; k < 256; k++) ok &= wd_read(&w, 1) == ((16 + k) & 0xFF);
    CHECK_EQ(ok, 1);
    wd_write(&w, 0, WD_STATUS);
    CHECK_EQ(wd_read(&w, 1), 0x49);                                  /* pausa in fase dati */
    wd_write(&w, 0, WD_PHASE);
    CHECK_EQ(wd_read(&w, 1), 0x45);
    wd_write(&w, 0, WD_COUNT_HI); wd_write(&w, 1, 0); wd_write(&w, 1, 3); wd_write(&w, 1, 0);
    wd_write(&w, 0, WD_COMMAND); wd_write(&w, 1, 0x08);              /* riprende da 0x45 */
    wd_write(&w, 0, WD_DATA);
    ok = 1;
    for (int k = 256; k < 1024; k++) ok &= wd_read(&w, 1) == ((16 + (k >= 512 ? 16 : 0) + (k & 511)) & 0xFF);
    CHECK_EQ(ok, 1);
    wd_write(&w, 0, WD_STATUS);
    CHECK_EQ(wd_read(&w, 1), 0x16);

    /* blocco fuori dal disco: CHECK CONDITION, poi REQUEST SENSE */
    static const uint8_t bad[10] = { 0x28, 0, 0, 0, 0, 100, 0, 0, 1, 0 };
    uint32_t len = 512;
    CHECK_EQ(scsi_dev_exec(&d, bad, w.buf, &len), SCSI_CHECK_CONDITION);
    static const uint8_t rs[6] = { 0x03, 0, 0, 0, 18, 0 };
    len = 18;
    scsi_dev_exec(&d, rs, w.buf, &len);
    CHECK_EQ(w.buf[2], 5);
    CHECK_EQ(w.buf[12], 0x21);
    scsi_dev_close(&d);
    free(w.buf);
    remove(path);
}

/* CD a settori grezzi MODE1/2352: 3 settori = 12 blocchi da 512, i dati
   saltano intestazione ed ECC */
static void test_scsi_raw_cd(void)
{
    printf("CD-ROM grezzo 2352\n");
    const char *path = "test_scsi_raw.bin";
    FILE *f = fopen(path, "wb");
    for (int s = 0; s < 3; s++) {
        uint8_t sec[2352];
        memset(sec, 0xEE, sizeof sec);                               /* ECC fittizio */
        memset(sec, 0xFF, 12); sec[0] = sec[11] = 0; sec[15] = 1;
        for (int k = 0; k < 2048; k++) sec[16 + k] = (uint8_t)(s * 4 + k / 512);
        fwrite(sec, 1, sizeof sec, f);
    }
    fclose(f);
    static ScsiDev d;
    static uint8_t buf[3 * 512];
    CHECK_EQ(scsi_dev_open(&d, SCSI_CDROM, path, 1), 0);
    CHECK_EQ(d.raw, 1);
    CHECK_EQ(d.blocks, 12);
    static const uint8_t cdb[10] = { 0x28, 0, 0, 0, 0, 3, 0, 0, 3, 0 };   /* blocchi 3-5 */
    uint32_t len = sizeof buf;
    CHECK_EQ(scsi_dev_exec(&d, cdb, buf, &len), SCSI_GOOD);
    CHECK_EQ(len, 3 * 512);
    CHECK_EQ(buf[0], 3); CHECK_EQ(buf[511], 3);
    CHECK_EQ(buf[512], 4); CHECK_EQ(buf[1535], 5);                   /* attraversa il settore */
    scsi_dev_close(&d);
    remove(path);
}

/* DS1687 dell'Octane: ora in BCD, banco 1 con KF, RAM estesa, PAB */
static void test_ds1687(void)
{
    printf("DS1687\n");
    static Ds1687 r;
    ds1687_init(&r, 1790000000);                       /* 2026-09-21 ... UTC */
    ds1687_select(&r, 9);
    CHECK_EQ(ds1687_read(&r, 0), 0x26);                /* anno in BCD */
    ds1687_select(&r, 0x0A);                           /* banco 1 */
    ds1687_write(&r, 0x30, 0);
    ds1687_select(&r, 0x4A);
    CHECK_EQ(ds1687_read(&r, 0) & 0x01, 1);            /* KF: acceso dal pulsante */
    ds1687_write(&r, 0x00, 0);                         /* azzera i flag */
    CHECK_EQ(ds1687_read(&r, 0) & 0x07, 0);
    ds1687_select(&r, 0x48);
    CHECK_EQ(ds1687_read(&r, 0), 0x20);                /* secolo */
    ds1687_select(&r, 0x50); ds1687_write(&r, 5, 0);   /* RAM estesa, indirizzo 5 */
    ds1687_select(&r, 0x53); ds1687_write(&r, 0xA5, 0);
    ds1687_select(&r, 0x50); ds1687_write(&r, 5, 0);
    ds1687_select(&r, 0x53);
    CHECK_EQ(ds1687_read(&r, 0), 0xA5);
    ds1687_select(&r, 0x0A);                           /* banco 0: NVRAM a 0x40 */
    ds1687_write(&r, 0x20, 0);
    ds1687_select(&r, 0x40); ds1687_write(&r, 0x77, 0);
    CHECK_EQ(ds1687_read(&r, 0), 0x77);
    /* l'ora avanza col tempo emulato */
    ds1687_select(&r, 0);
    int s0 = ds1687_read(&r, 0);
    int s1 = ds1687_read(&r, 3000000000ull);
    CHECK_EQ(((s1 >> 4) * 10 + (s1 & 15) + 60 - ((s0 >> 4) * 10 + (s0 & 15))) % 60, 3);
    CHECK_EQ(r.power_off, 0);
    ds1687_select(&r, 0x0A); ds1687_write(&r, 0x30, 0);
    ds1687_select(&r, 0x4A); ds1687_write(&r, 0x08, 0);   /* PAB: spegni */
    CHECK_EQ(r.power_off, 1);
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

    /* tasti: 'A' e freccia su, nei tre set */
    kbc_reset(&k);
    kbc_key(&k, 0x1E, 0, 1); kbc_key(&k, 0x1E, 0, 0);          /* traduzione: set 1 */
    CHECK_EQ(kbc_read(&k, 0), 0x1E);
    CHECK_EQ(kbc_read(&k, 0), 0x9E);
    kbc_write(&k, 1, 0x60); kbc_write(&k, 0, 0x03);              /* senza traduzione: set 2 */
    kbc_key(&k, 0x1E, 0, 0);
    CHECK_EQ(kbc_read(&k, 0), 0xF0);
    CHECK_EQ(kbc_read(&k, 0), 0x1C);
    kbc_key(&k, 0x48, 1, 1);
    CHECK_EQ(kbc_read(&k, 0), 0xE0);
    CHECK_EQ(kbc_read(&k, 0), 0x75);
    kbc_write(&k, 0, 0xF0); kbc_write(&k, 0, 0x03);              /* set 3 */
    CHECK_EQ(kbc_read(&k, 0), 0xFA);
    CHECK_EQ(kbc_read(&k, 0), 0xFA);
    kbc_key(&k, 0x48, 1, 1); kbc_key(&k, 0x01, 0, 0);
    CHECK_EQ(kbc_read(&k, 0), 0x63);                             /* su */
    CHECK_EQ(kbc_read(&k, 0), 0xF0);
    CHECK_EQ(kbc_read(&k, 0), 0x08);                             /* Esc rilasciato */
    /* mouse: niente pacchetti finche' non e' abilitato */
    kbc_mouse(&k, 5, -3, 1);
    CHECK_EQ(kbc_read(&k, 1) & 1, 0);
    kbc_write(&k, 1, 0xD4); kbc_write(&k, 0, 0xF4);
    CHECK_EQ(kbc_read(&k, 0), 0xFA);
    kbc_mouse(&k, 5, -3, 1);
    CHECK_EQ(kbc_read(&k, 0), 0x29);                             /* 0x08 | sin | segno di y */
    CHECK_EQ(kbc_read(&k, 0), 5);
    CHECK_EQ(kbc_read(&k, 0), 0xFD);
}

int main(void)
{
    test_eeprom();
    test_pit();
    test_rtc();
    test_scc();
    test_wd();
    test_scsi_disk();
    test_scsi_raw_cd();
    test_ds1687();
    test_kbc();
    printf("%d controlli, %d falliti\n", checks, failures);
    return failures != 0;
}
