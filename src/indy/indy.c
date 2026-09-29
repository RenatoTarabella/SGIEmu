/*
 * indy.c - Montaggio della macchina e ciclo di esecuzione
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "indy/indy.h"

#define PROM_BASE 0x1FC00000u
#define SLICE     2000u                 /* istruzioni tra un aggiornamento e l'altro */

/* Spazio GIO senza schede: bus error in lettura, come fa l'MC dopo il timeout */
static int gio_read(void *ctx, uint32_t pa, int size, uint64_t *val)
{
    Indy *m = ctx;
    (void)size;
    *val = 0;
    m->mc.gerr = pa;
    m->mc.gstat |= 0x400;               /* GIO timeout */
    return 1;
}

static int gio_write(void *ctx, uint32_t pa, int size, uint64_t val)
{
    Indy *m = ctx;
    (void)size; (void)val;
    m->mc.gerr = pa;
    m->mc.gstat |= 0x400;
    return 0;
}

static int unmapped_read(void *ctx, uint32_t pa, int size, uint64_t *val)
{
    Indy *m = ctx;
    if (m->io_log) m->io_log(m, 0, pa, size, 0);
    *val = 0;
    return 0;
}

static int unmapped_write(void *ctx, uint32_t pa, int size, uint64_t val)
{
    Indy *m = ctx;
    if (m->io_log) m->io_log(m, 1, pa, size, val);
    return 0;
}

int indy_init(Indy *m, const uint8_t *prom, uint32_t prom_size, unsigned ram_mb)
{
    memset(m, 0, sizeof *m);
    m->ns_per_insn = 10;                /* 100 milioni di istruzioni al secondo emulato */
    uint32_t map = (prom_size + PM_PAGE_MASK) & ~PM_PAGE_MASK;
    m->prom = calloc(1, map ? map : PM_PAGE_SIZE);
    if (!m->prom) return -1;
    memcpy(m->prom, prom, prom_size);
    m->prom_size = map;

    /* SIMM: banchi da al piu' 128 MB, riempiti in ordine */
    uint32_t left = ram_mb;
    for (int b = 0; b < 4 && left; b++) {
        uint32_t mb = left >= 128 ? 128 : left >= 64 ? 64 : left >= 32 ? 32 : 16;
        m->bank_size[b] = mb << 20;
        m->bank[b] = calloc(1, m->bank_size[b]);
        if (!m->bank[b]) return -1;
        left = left > mb ? left - mb : 0;
    }

    if (pm_init(&m->pm) || r4k_init(&m->cpu, &m->pm)) return -1;
    pm_map_ram(&m->pm, PROM_BASE, m->prom_size, m->prom, PM_ROM);
    pm_map_io(&m->pm, 0x1F000000u, 0x00A00000u, m, gio_read, gio_write, "GIO");
    pm_map_io(&m->pm, 0x1FA00000u, 0x00020000u, m, indy_mc_read, indy_mc_write, "MC");
    pm_map_io(&m->pm, 0x1FB00000u, 0x00080000u, m, gio_read, gio_write, "HPC3#1");
    pm_map_io(&m->pm, 0x1FB80000u, 0x00080000u, m, indy_hpc3_read, indy_hpc3_write, "HPC3");
    m->pm.unmapped_read = unmapped_read;
    m->pm.unmapped_write = unmapped_write;
    m->pm.unmapped_ctx = m;

    rtc_init(&m->rtc, (int64_t)time(NULL));
    for (int k = 0; k < 128; k++) m->eeprom.data[k] = m->eeprom_hpc.data[k] = 0xFFFF;
    indy_reset(m);
    return 0;
}

void indy_free(Indy *m)
{
    r4k_free(&m->cpu);
    pm_free(&m->pm);
    for (int b = 0; b < 4; b++) free(m->bank[b]);
    free(m->prom);
}

void indy_reset(Indy *m)
{
    r4k_reset(&m->cpu);
    indy_mc_init(m);
    indy_hpc3_init(m);
    indy_mc_remap(m);
    m->power_off = 0;
    indy_update_irq(m);
}

/* tra una fetta e l'altra: timer 8254 e interrupt dei dispositivi */
static void service(Indy *m)
{
    uint64_t now = indy_now_ns(m);
    for (int k = 0; k < 2; k++)
        if (pit_poll(&m->pit, k, now)) m->ioc.timer_irq[k] = 1;
    indy_update_irq(m);
}

uint64_t indy_run(Indy *m, uint64_t n)
{
    uint64_t done = 0;
    while (done < n && !m->power_off) {
        uint64_t chunk = n - done < SLICE ? n - done : SLICE;
        uint64_t ran = r4k_run(&m->cpu, chunk);
        done += ran;
        service(m);
        if (m->cpu.stop && ran < chunk) break;          /* fermata chiesta da fuori */
    }
    return done;
}
