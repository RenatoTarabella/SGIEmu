/*
 * mc.c - MC, il controller di memoria dell'IP22/IP24
 *
 * Registri da 32 bit su un bus a 64: ogni registro sta a offset+4 di una
 * doppia parola. I banchi di RAM seguono MEMCFG0/1: ogni banco ha base e
 * dimensione programmate dalla PROM; se la dimensione programmata supera
 * i SIMM montati la memoria si ripete (e' cosi' che la PROM la misura).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "indy/indy.h"

#define MC_BASE 0x1FA00000u

enum {
    MC_CPUCTRL0 = 0x04, MC_CPUCTRL1 = 0x0C, MC_WATCHDOG = 0x14, MC_SYSID = 0x1C,
    MC_DIVIDER = 0x2C, MC_EEPROM = 0x34, MC_RCNTPRE = 0x44, MC_RCOUNTER = 0x4C,
    MC_GIOPAR = 0x84, MC_CPUTP = 0x8C, MC_LBURSTTP = 0x9C, MC_MCONFIG0 = 0xC4,
    MC_MCONFIG1 = 0xCC, MC_CMACC = 0xD4, MC_GMACC = 0xDC, MC_CERR = 0xE4,
    MC_CSTAT = 0xEC, MC_GERR = 0xF4, MC_GSTAT = 0xFC, MC_SYSSEM = 0x104,
    MC_RPSS = 0x1004,
    /* DMA virtuale (VDMA): registri da 64 bit, il valore a +4 */
    DMA_MEMADR = 0x2004, DMA_MEMADRD = 0x200C, DMA_SIZE = 0x2014, DMA_STRIDE = 0x201C,
    DMA_GIO_ADR = 0x2024, DMA_GIO_ADRS = 0x202C, DMA_MODE = 0x2034, DMA_COUNT = 0x203C,
    DMA_STDMA = 0x2044, DMA_RUN = 0x204C, DMA_MEMADRDS = 0x2074
};

#define DMA_MODE_FILL 0x10
#define DMA_RUNNING   0x40
#define DMA_BUSY_NS   10000                 /* quanto resta "in corso" dopo la partenza */

/* VDMA: il trasferimento si fa tutto alla partenza. Per ora solo il modo
   fill (la PROM azzera la memoria cosi'): scrive il valore di GIO_ADR su
   righe di SIZE & 0xFFFF byte, SIZE >> 16 righe, saltando STRIDE & 0xFFFF
   byte tra una riga e l'altra. */
static void vdma_start(Indy *m)
{
    uint32_t *r = m->mc.regs;
    uint32_t mode = r[DMA_MODE >> 2], size = r[DMA_SIZE >> 2];
    uint32_t addr = r[DMA_MEMADR >> 2];
    uint32_t width = size & 0xFFFF, lines = size >> 16;
    int32_t skip = (int16_t)(r[DMA_STRIDE >> 2] & 0xFFFF);
    m->mc.dma_busy_until = indy_now_ns(m) + DMA_BUSY_NS;
    if (!(mode & DMA_MODE_FILL)) {
        if (m->io_log) m->io_log(m, 1, 0x1FA02034, 4, mode);   /* modo non ancora fatto */
        return;
    }
    uint32_t fill = r[DMA_GIO_ADR >> 2];
    for (uint32_t l = 0; l < (lines ? lines : 1); l++) {
        for (uint32_t b = 0; b < width; b += 4) pm_write(&m->pm, addr + b, 4, fill);
        addr += width + (uint32_t)skip;
    }
    r[DMA_MEMADR >> 2] = addr;
    r4k_fast_flush(&m->cpu);
}

#define MCONFIG_VALID    0x2000
#define MCONFIG_SUBBANKS 0x4000

void indy_mc_init(Indy *m)
{
    IndyMC *mc = &m->mc;
    memset(mc, 0, sizeof *mc);
    mc->sysid = 3;                          /* MC revisione C, niente EISA */
    ee_reset(&m->eeprom);
}

static uint32_t bank_cfg(const IndyMC *mc, int b)
{
    uint32_t r = b > 1 ? mc->mconfig1 : mc->mconfig0;
    return b % 2 ? r & 0xFFFF : r >> 16;
}

void indy_mc_remap(Indy *m)
{
    PhysMem *pm = &m->pm;
    pm_unmap(pm, 0x00000000u, 0x00080000u);
    pm_unmap(pm, 0x08000000u, 0x10000000u);
    pm_unmap(pm, 0x20000000u, 0x10000000u);
    int shift_base = (m->mc.sysid & 15) >= 5 ? 24 : 22;
    int shift_size = (m->mc.sysid & 15) >= 5 ? 16 : 14;
    for (int b = 0; b < 4; b++) {
        uint32_t cfg = bank_cfg(&m->mc, b);
        if (!(cfg & MCONFIG_VALID) || !m->bank_size[b]) continue;
        uint64_t base = (uint64_t)(cfg & 0xFF) << shift_base;
        uint64_t size = (uint64_t)((cfg & 0x1F00) + 0x100) << shift_size;
        /* due sottobanchi: la meta' alta e' la seconda faccia dei SIMM, che
           qui non c'e' (SIMM a una faccia) */
        if (cfg & MCONFIG_SUBBANKS) size /= 2;
        for (uint64_t off = 0; off < size; off += PM_PAGE_SIZE) {
            uint64_t pa = base + off;
            if (pa >= 0x1F000000u && pa < 0x20000000u) continue;    /* mai sopra l'I/O */
            if (pa >> 32) break;
            pm_map_ram(pm, (uint32_t)pa, PM_PAGE_SIZE, m->bank[b] + off % m->bank_size[b], PM_RAM);
        }
    }
    /* i primi 512 KB fisici ripetono l'inizio della RAM a 0x08000000 */
    for (uint32_t off = 0; off < 0x80000u; off += PM_PAGE_SIZE) {
        uint32_t page = (0x08000000u + off) >> PM_PAGE_SHIFT;
        if (pm->host[page]) pm_map_ram(pm, off, PM_PAGE_SIZE, pm->host[page], PM_RAM);
    }
    r4k_fast_flush(&m->cpu);
}

static uint32_t reg_read(Indy *m, uint32_t off)
{
    IndyMC *mc = &m->mc;
    switch (off) {
    case MC_CPUCTRL0:  return mc->cpuctrl0;
    case MC_CPUCTRL1:  return mc->cpuctrl1;
    case MC_WATCHDOG:  return mc->watchdog;
    case MC_SYSID:     return mc->sysid;
    case MC_DIVIDER:   return mc->divider;
    case MC_EEPROM:    return (mc->eeprom & ~0x10u) | (ee_dout(&m->eeprom) ? 0x10u : 0);
    case MC_RCNTPRE:   return mc->rcntpre;
    case MC_RCOUNTER:  return (uint32_t)(indy_now_ns(m) / 60) & 0x3FF;
    case MC_GIOPAR:    return mc->giopar;
    case MC_CPUTP:     return mc->cputp;
    case MC_LBURSTTP:  return mc->lbursttp;
    case MC_MCONFIG0:  return mc->mconfig0;
    case MC_MCONFIG1:  return mc->mconfig1;
    case MC_CMACC:     return mc->cmacc;
    case MC_GMACC:     return mc->gmacc;
    case MC_CERR:      return mc->cerr;
    case MC_CSTAT:     return mc->cstat;
    case MC_GERR:      return mc->gerr;
    case MC_GSTAT:     return mc->gstat;
    case MC_SYSSEM: {                       /* la lettura prende il semaforo */
        uint32_t v = mc->semaphore;
        mc->semaphore = 1;
        return v;
    }
    case MC_RPSS:      return (uint32_t)(indy_now_ns(m) / 100);       /* un tick ogni 100 ns */
    case DMA_RUN:      return indy_now_ns(m) < mc->dma_busy_until ? DMA_RUNNING : 0;
    default:
        if (off >= 0x10000 && off < 0x10080) {                      /* semafori utente */
            uint32_t *s = &mc->usem[((off - 0x10000) >> 3) & 15];
            uint32_t v = *s;
            *s = 1;
            return v;
        }
        return off < sizeof mc->regs ? mc->regs[off >> 2] : 0;
    }
}

static void reg_write(Indy *m, uint32_t off, uint32_t v)
{
    IndyMC *mc = &m->mc;
    if (m->mc_log) fprintf(stderr, "[MC %04X = %08X pc=%llX]\n", off, v, (unsigned long long)m->cpu.pc);
    switch (off) {
    case MC_CPUCTRL0:  mc->cpuctrl0 = v; break;
    case MC_CPUCTRL1:  mc->cpuctrl1 = v; break;
    case MC_WATCHDOG:  mc->watchdog = 0; break;
    case MC_SYSID:     break;
    case MC_DIVIDER:   mc->divider = v; break;
    case MC_EEPROM:
        mc->eeprom = v;
        ee_pins(&m->eeprom, (v >> 1) & 1, (v >> 2) & 1, (v >> 3) & 1);
        break;
    case MC_RCNTPRE:   mc->rcntpre = v; break;
    case MC_GIOPAR:    mc->giopar = v; break;
    case MC_CPUTP:     mc->cputp = v; break;
    case MC_LBURSTTP:  mc->lbursttp = v; break;
    case MC_MCONFIG0:  mc->mconfig0 = v; indy_mc_remap(m); break;
    case MC_MCONFIG1:  mc->mconfig1 = v; indy_mc_remap(m); break;
    case MC_CMACC:     mc->cmacc = v; break;
    case MC_GMACC:     mc->gmacc = v; break;
    case MC_CERR: case MC_CSTAT: mc->cstat = 0; mc->cerr = 0; break;
    case MC_GERR: case MC_GSTAT: mc->gstat = 0; mc->gerr = 0; break;
    case MC_SYSSEM:    mc->semaphore = v & 1; break;
    case DMA_MEMADRD: case DMA_MEMADRDS:                 /* indirizzo e valori di default */
        mc->regs[DMA_MEMADR >> 2] = v;
        mc->regs[DMA_SIZE >> 2] = 0;
        mc->regs[DMA_STRIDE >> 2] = 0;
        mc->regs[DMA_MODE >> 2] = 0;
        if (off == DMA_MEMADRDS) vdma_start(m);
        break;
    case DMA_GIO_ADRS: mc->regs[DMA_GIO_ADR >> 2] = v; vdma_start(m); break;
    case DMA_STDMA:    if (v) vdma_start(m); break;
    default:
        if (off >= 0x10000 && off < 0x10080) {
            mc->usem[((off - 0x10000) >> 3) & 15] = v & 1;
            break;
        }
        if (off < sizeof mc->regs) mc->regs[off >> 2] = v;
        break;
    }
}

/* Accessi: il registro e' la parola da 32 bit a offset | 4 della doppia
   parola; letture piu' piccole prendono i byte giusti in big-endian. */
int indy_mc_read(void *ctx, uint32_t pa, int size, uint64_t *val)
{
    Indy *m = ctx;
    uint32_t off = pa - MC_BASE;
    if (size == 8) { *val = reg_read(m, (off & ~7u) | 4); return 0; }
    uint32_t w = reg_read(m, off & ~3u);
    int sh = 8 * (int)(4 - size - (off & 3));
    *val = (w >> sh) & (size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1);
    return 0;
}

int indy_mc_write(void *ctx, uint32_t pa, int size, uint64_t val)
{
    Indy *m = ctx;
    uint32_t off = pa - MC_BASE;
    if (size == 8) { reg_write(m, (off & ~7u) | 4, (uint32_t)val); return 0; }
    if (size == 4) { reg_write(m, off & ~3u, (uint32_t)val); return 0; }
    uint32_t w = reg_read(m, off & ~3u);
    int sh = 8 * (int)(4 - size - (off & 3));
    uint32_t mask = ((1u << (8 * size)) - 1) << sh;
    reg_write(m, off & ~3u, (w & ~mask) | (((uint32_t)val << sh) & mask));
    return 0;
}
