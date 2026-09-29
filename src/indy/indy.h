/*
 * indy.h - La macchina SGI Indy (IP24): CPU, memoria e ASIC
 *
 * Mappa fisica (quella che serve alla PROM):
 *   0x00000000  512 KB bassi, alias dell'inizio della RAM a 0x08000000
 *   0x08000000  RAM (banchi configurati dall'MC), fino a 256 MB
 *   0x1F000000  spazio GIO (grafica Newport a 0x1F0F0000, slot)
 *   0x1FA00000  MC, controller di memoria
 *   0x1FB80000  HPC3: DMA, SCSI, ethernet, PBUS (IOC a 0x1FBD9800), RTC a 0x1FBE0000
 *   0x1FC00000  PROM
 *   0x20000000  RAM alta (banchi oltre i 256 MB)
 *
 * Il tempo e' quello della CPU: ns_per_insn nanosecondi per istruzione.
 * I dispositivi calcolano il loro stato dal tempo quando vengono letti;
 * indy_run li aggiorna tra una fetta di istruzioni e l'altra per gli
 * interrupt.
 */
#ifndef INDY_H
#define INDY_H

#include <stdint.h>
#include "cpu/r4k.h"
#include "mem/physmem.h"
#include "indy/pit8254.h"
#include "indy/z85c30.h"
#include "indy/ds1386.h"
#include "indy/eeprom93.h"
#include "indy/kbc8042.h"
#include "indy/wd33c93.h"

/* linee di interrupt della CPU (r4k_set_irq): IP2..IP7 */
enum { IRQ_LOCAL0 = 0, IRQ_LOCAL1 = 1, IRQ_TIMER0 = 2, IRQ_TIMER1 = 3, IRQ_BUSERR = 4 };

/* bit delle sorgenti di interrupt dell'INT3 */
enum {
    L0_FIFO = 0x01, L0_SCSI0 = 0x02, L0_SCSI1 = 0x04, L0_ENET = 0x08,
    L0_MCDMA = 0x10, L0_PPORT = 0x20, L0_GIO1 = 0x40, L0_MAP0 = 0x80,
    L1_GPL0 = 0x01, L1_PANEL = 0x02, L1_GPL2 = 0x04, L1_MAP1 = 0x08,
    L1_HPCDMA = 0x10, L1_ACFAIL = 0x20, L1_VINO = 0x40, L1_GIO2 = 0x80,
    MAP_VRETRACE = 0x01, MAP_EISA = 0x08, MAP_KBD = 0x10, MAP_SERIAL = 0x20,
    MAP_GIOEXP0 = 0x40, MAP_GIOEXP1 = 0x80
};

typedef struct IndyMC {
    uint32_t cpuctrl0, cpuctrl1, watchdog, sysid, divider, eeprom;
    uint32_t rcntpre, giopar, cputp, lbursttp, mconfig0, mconfig1;
    uint32_t cmacc, gmacc, cerr, cstat, gerr, gstat, semaphore;
    uint64_t dma_busy_until;           /* ns: fine del VDMA in corso */
    uint32_t usem[16];                 /* semafori utente a 0x1FA10000 */
    uint32_t regs[0x2100 / 4];         /* tutto il resto: si ricorda e basta */
} IndyMC;

typedef struct IndyIOC {
    uint8_t gcsel, genctrl, panel, sysid, dmasel, reset, write;
    /* INT3 */
    uint8_t lio0_raw, lio1_raw, map_raw;       /* sorgenti attive */
    uint8_t imask0, imask1, cmeimask0, cmeimask1, cmepol, errstat;
    int     timer_irq[2];                       /* latch degli interrupt 8254 */
} IndyIOC;

typedef struct Indy {
    R4k      cpu;
    PhysMem  pm;

    uint8_t *prom;
    uint32_t prom_size;
    uint8_t *bank[4];                  /* SIMM installati per banco */
    uint32_t bank_size[4];

    IndyMC   mc;
    IndyIOC  ioc;
    uint32_t hpc_regs[0x80000 / 4];    /* registri HPC3 senza effetti particolari */
    uint32_t hpc_gio_misc, hpc_eeprom;

    Pit8254  pit;
    Z85c30   scc;
    Ds1386   rtc;
    Kbc8042  kbc;
    Wd33c93  scsi;                     /* SCSI 0, a 0x1FBC0000 */
    Wd33c93  scsi1;                    /* SCSI 1, a 0x1FBC8000, vuoto */
    ScsiDev  disk[8];                  /* dispositivi collegati, per ID */
    Eeprom93 eeprom;                   /* 93CS56 sul registro EEPROM dell'MC */
    Eeprom93 eeprom_hpc;               /* e quello sull'HPC3 (Indigo2) */

    uint32_t ns_per_insn;
    int      power_off;                /* la PROM o il sistema hanno spento */
    uint32_t last_berr;                /* indirizzo fisico dell'ultimo bus error */

    /* log degli accessi a dispositivi che non ci sono */
    long     io_log_left;
    int      mc_log;                   /* stampa le scritture dell'MC */
    int      scsi_log;                 /* stampa comandi SCSI e DMA */
    int      rtc_log;                  /* stampa le letture della NVRAM */
    void   (*io_log)(struct Indy *m, int write, uint32_t pa, int size, uint64_t val);
} Indy;

int  indy_init(Indy *m, const uint8_t *prom, uint32_t prom_size, unsigned ram_mb);
void indy_free(Indy *m);
void indy_reset(Indy *m);
/* collega un'immagine al bus SCSI 0 (type SCSI_DISK o SCSI_CDROM): 0 = ok */
int  indy_attach_scsi(Indy *m, int id, int type, const char *path, int readonly);
/* Esegue n istruzioni (a fette, aggiornando i dispositivi). */
uint64_t indy_run(Indy *m, uint64_t n);

static inline uint64_t indy_now_ns(const Indy *m) { return m->cpu.icount * m->ns_per_insn; }

/* ricalcola le linee di interrupt della CPU */
void indy_update_irq(Indy *m);
void indy_set_lio0(Indy *m, uint8_t bit, int level);
void indy_set_lio1(Indy *m, uint8_t bit, int level);
void indy_set_map(Indy *m, uint8_t bit, int level);

/* moduli (mc.c, hpc3.c) */
void indy_mc_init(Indy *m);
void indy_mc_remap(Indy *m);
int  indy_mc_read(void *ctx, uint32_t pa, int size, uint64_t *val);
int  indy_mc_write(void *ctx, uint32_t pa, int size, uint64_t val);
void indy_hpc3_init(Indy *m);
int  indy_hpc3_read(void *ctx, uint32_t pa, int size, uint64_t *val);
int  indy_hpc3_write(void *ctx, uint32_t pa, int size, uint64_t val);

#endif
