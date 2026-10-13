/*
 * octane.h - La macchina SGI Octane (IP30): CPU R10000, HEART, XBOW, BRIDGE
 *
 * L'Octane e' fatto di "widget" XIO collegati dallo switch XBOW. Mappa
 * fisica (dai sorgenti Linux IP30 e dalla PROM):
 *   0x0FF00000  registri del HEART (controller di memoria, interrupt, timer)
 *   0x10000000  finestra piccola del widget 0: XBOW
 *   0x1n000000  finestra piccola (16 MB) del widget n, n = 8..15:
 *               8 = HEART, 15 = BRIDGE della BaseIO, 9..14 slot XIO (grafica)
 *   0x1F000000  BRIDGE: registri, spazio di configurazione PCI (0x20000),
 *               finestre dei dispositivi PCI (0x200000..), flash della PROM
 *               (0xC00000, cioe' 0x1FC00000)
 *   0x1F600000  IOC3 (finestra 2 del BRIDGE): seriali, tastiera, orologio
 *   0x20000000  RAM (banchi configurati nel HEART)
 *
 * I registri dei widget sono a 64 bit; quelli a 32 bit stanno nella meta'
 * bassa, cioe' a +4 (big-endian).
 *
 * Il tempo e' quello della CPU, come sull'Indy: ps_per_insn picosecondi per
 * istruzione. Il contatore del HEART avanza ogni 80 ns.
 */
#ifndef OCTANE_H
#define OCTANE_H

#include <stdint.h>
#include "cpu/r4k.h"
#include "mem/physmem.h"
#include "octane/ds1687.h"

#define OCT_PROM_SIZE   (1u << 20)     /* immagine della PROM (MAME ip30prom) */
#define OCT_FLASH_SIZE  (2u << 20)     /* Sharp LH28F016SU: 2 MB, l'ambiente (PDS) e' a +0xF0000 */
#define OCT_RAM_BASE    0x20000000u
#define OCT_PROBE_WORDS 64
#define OCT_LOWMEM      0x00040000u    /* RAM vista anche da 0 (vettori delle eccezioni) */

/* HEART: registri a 0x0FF00000 */
typedef struct OctHeart {
    uint64_t regs[0x100 / 8];          /* 0x00000-0x000FF: modo, memoria, stato, errori */
    uint64_t imr[4];                   /* maschere degli interrupt per CPU */
    uint64_t isr;                      /* interrupt pendenti (64 vettori) */
    uint64_t cause;
    uint64_t compare, trigger, sync;
    uint64_t count_ofs;                /* count = tempo / 80 ns + count_ofs */
    uint64_t wregs[0x100 / 8];         /* registri da widget, a 0x18000000 */
} OctHeart;

/* 16550 dell'IOC3 */
typedef struct OctUart {
    uint8_t ier, lcr, mcr, scr, dll, dlm;
    uint8_t rx[64];
    int     rx_head, rx_n;
} OctUart;

typedef struct Octane {
    R4k      cpu;
    PhysMem  pm;

    uint8_t *prom;                     /* flash da 2 MB con la PROM */
    int      flash_mode;               /* 0 lettura, poi identificazione, stato, programma... */
    int      flash_dirty;              /* ambiente o PROM riscritti: da salvare */
    uint32_t ram_size;                 /* totale */
    /* banchi (coppie di DIMM): memoria vera e dove li ha messi MEMCFG */
    uint8_t *bank[4];
    uint32_t bank_size[4];
    uint32_t bank_cfg[4];              /* densita' e dimensione del tipo di DIMM (MEMCFG 24:16) */
    uint32_t bank_ignored[4];          /* bit d'indirizzo ignorati se configurato diversamente */
    uint32_t map_base[4], map_size[4];
    /* banco configurato con un altro tipo: poche parole, per le prove della PROM */
    uint32_t probe_size[4];
    int      probe_n[4];
    uint32_t probe_key[4][OCT_PROBE_WORDS];
    uint64_t probe_val[4][OCT_PROBE_WORDS];

    OctHeart heart;
    uint64_t xbow[0x400 / 8];          /* registri dello XBOW */
    uint64_t bridge[0x300 / 8];        /* registri del BRIDGE */
    uint32_t pcicfg[8][64];            /* spazio di configurazione PCI per slot (parole little-endian) */
    uint32_t ioc3[0x200 / 4];          /* registri dell'IOC3 da 0 a 0x1FF */
    OctUart  uart[2];                  /* 0 = A (console), 1 = B */
    Ds1687   rtc;                      /* orologio sul bytebus dell'IOC3 */

    uint32_t ps_per_insn;              /* picosecondi per istruzione: 1e6 / MIPS */
    int      power_off;

    /* uscita della seriale (console) e log degli accessi sconosciuti */
    int      heart_log;                /* passa al log anche le scritture dei registri del HEART */
    int      ioc3_log;                 /* e gli accessi ai registri dell'IOC3 e della seriale */
    void   (*tx)(struct Octane *m, int port, uint8_t ch);
    void   (*io_log)(struct Octane *m, int write, uint64_t pa, int size, uint64_t val, const char *what);
} Octane;

int  oct_init(Octane *m, const uint8_t *prom, uint32_t prom_size, unsigned ram_mb);
void oct_free(Octane *m);
void oct_reset(Octane *m);
uint64_t oct_run(Octane *m, uint64_t n);
/* un carattere ricevuto sulla seriale (0 = A) */
void oct_serial_rx(Octane *m, int port, uint8_t ch);

static inline uint64_t oct_now_ns(const Octane *m) { return m->cpu.icount * m->ps_per_insn / 1000; }
static inline uint64_t oct_insn_per_ms(const Octane *m) { return 1000000000ull / m->ps_per_insn; }

#endif
