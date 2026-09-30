/*
 * indy.c - Montaggio della macchina e ciclo di esecuzione
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <intrin.h>
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
    m->last_berr = pa;
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

static int np_io_read(void *ctx, uint32_t pa, int size, uint64_t *val)
{
    Indy *m = ctx;
    uint64_t t0 = __rdtsc();
    *val = np_read(&m->gfx, pa - 0x1F0F0000u, size);
    m->np_calls++; m->np_ticks += __rdtsc() - t0;
    if (!m->gfx.irq && (m->ioc.lio1_raw & L1_RETRACE)) {     /* letto lo stato: retrace servito */
        m->ioc.lio1_raw &= (uint8_t)~L1_RETRACE;
        indy_update_irq(m);
    }
    if (m->gfx_log > 0 && m->gfx.draws >= m->gfx.trace_after) {       /* --nplog N, dopo --nptrace-after */
        m->gfx_log--;
        fprintf(stderr, "[np R%d %04X -> %llX pc=%llX]\n", size * 8, pa & 0xFFFF,
                (unsigned long long)*val, (unsigned long long)m->cpu.pc);
    }
    return 0;
}

static int np_io_write(void *ctx, uint32_t pa, int size, uint64_t val)
{
    Indy *m = ctx;
    if (m->gfx_log > 0 && m->gfx.draws >= m->gfx.trace_after) {       /* --nplog N, dopo --nptrace-after */
        m->gfx_log--;
        fprintf(stderr, "[np W%d %04X = %llX pc=%llX]\n", size * 8, pa & 0xFFFF,
                (unsigned long long)val, (unsigned long long)m->cpu.pc);
    }
    uint64_t t0 = __rdtsc();
    np_write(&m->gfx, pa - 0x1F0F0000u, size, val);
    m->np_calls++; m->np_ticks += __rdtsc() - t0;
    return 0;
}

int indy_init(Indy *m, const uint8_t *prom, uint32_t prom_size, unsigned ram_mb, int gfx)
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
    if (gfx) {
        if (np_init(&m->gfx, gfx == 24)) return -1;
        m->has_gfx = 1;
        pm_map_io(&m->pm, 0x1F0F0000u, 0x00010000u, m, np_io_read, np_io_write, "Newport");
    }
    pm_map_io(&m->pm, 0x1FB00000u, 0x00080000u, m, gio_read, gio_write, "HPC3#1");
    pm_map_io(&m->pm, 0x1FB80000u, 0x00080000u, m, indy_hpc3_read, indy_hpc3_write, "HPC3");
    m->pm.unmapped_read = unmapped_read;
    m->pm.unmapped_write = unmapped_write;
    m->pm.unmapped_ctx = m;

    wd_init(&m->scsi);
    wd_init(&m->scsi1);
    if (!m->scsi.buf || !m->scsi1.buf) return -1;
    rtc_init(&m->rtc, (int64_t)time(NULL));
    for (int k = 0; k < 128; k++) m->eeprom.data[k] = m->eeprom_hpc.data[k] = 0xFFFF;
    indy_reset(m);
    return 0;
}

int indy_attach_scsi(Indy *m, int id, int type, const char *path, int readonly)
{
    if (id < 0 || id > 7 || scsi_dev_open(&m->disk[id], type, path, readonly)) return -1;
    m->scsi.dev[id] = &m->disk[id];
    return 0;
}

void indy_free(Indy *m)
{
    for (int k = 0; k < 8; k++) scsi_dev_close(&m->disk[k]);
    free(m->scsi.buf);
    free(m->scsi1.buf);
    if (m->has_gfx) np_free(&m->gfx);
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
    if (m->has_gfx) np_reset(&m->gfx);
    m->power_off = 0;
    indy_update_irq(m);
}

/* tra una fetta e l'altra: timer 8254 e interrupt dei dispositivi */
static void service(Indy *m)
{
    uint64_t now = indy_now_ns(m);
    for (int k = 0; k < 2; k++)
        if (pit_poll(&m->pit, k, now)) m->ioc.timer_irq[k] = 1;
    if (m->has_gfx) {
        if (now >= m->next_vblank) {                  /* retrace a 60 Hz */
            np_vblank(&m->gfx);
            m->next_vblank = now + 1000000000u / 60;
        }
        if (m->gfx.irq) m->ioc.lio1_raw |= L1_RETRACE; else m->ioc.lio1_raw &= (uint8_t)~L1_RETRACE;
    }
    if (m->mc.dma_irq_pending && now >= m->mc.dma_busy_until) {   /* VDMA finito */
        m->mc.dma_irq_pending = 0;
        m->mc.regs[0x164 >> 2] |= 0x08;
        m->ioc.lio0_raw |= L0_MCDMA;
    }
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
