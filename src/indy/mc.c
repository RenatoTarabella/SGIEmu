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
    MC_DMA_CAUSE = 0x164, MC_DMA_CTRL = 0x16C,
    MC_DMA_TLB_HI = 0x184, MC_DMA_TLB_LO = 0x18C,      /* 4 voci, una ogni 16 byte */
    MC_RPSS = 0x1004,
    /* DMA virtuale (VDMA): registri da 64 bit, il valore a +4 */
    DMA_MEMADR = 0x2004, DMA_MEMADRD = 0x200C, DMA_SIZE = 0x2014, DMA_STRIDE = 0x201C,
    DMA_GIO_ADR = 0x2024, DMA_GIO_ADRS = 0x202C, DMA_MODE = 0x2034, DMA_COUNT = 0x203C,
    DMA_STDMA = 0x2044, DMA_RUN = 0x204C, DMA_MEMADRDS = 0x2074
};

/* modo (DMA_MODE) */
#define DMA_MODE_TOHOST 0x02                /* dal GIO alla memoria */
#define DMA_MODE_FILL   0x08                /* riempie la memoria col valore di GIO_ADR */
/* DMA_RUN */
#define DMA_RUNNING     0x40
#define DMA_DONE        0x08                /* finito bene; senza: fermo su una pagina */
#define DMA_BUSY_NS     1000                /* quanto resta "in corso" dopo la partenza */
/* DMA_CTRL */
#define DMA_CTRL_XLATE  0x10                /* MEMADR e' virtuale: TLB del DMA */

/* Indirizzo virtuale del VDMA -> fisico. La TLB del DMA ha 4 voci: HI =
   segmento da 4 MB, LO = pagina della tabella delle pagine in formato
   EntryLo (bit 1 valido); la PTE (4 byte per pagina) ha la pagina fisica
   nello stesso formato e il bit 1 valido. Una PTE non valida ferma il
   DMA: il kernel la sistema e riparte con STDMA. */
static int vdma_xlate(Indy *m, uint32_t va, uint32_t *pa)
{
    const uint32_t *r = m->mc.regs;
    if (!(r[MC_DMA_CTRL >> 2] & DMA_CTRL_XLATE)) { *pa = va; return 1; }
    for (int k = 0; k < 4; k++) {
        uint32_t hi = r[(MC_DMA_TLB_HI + 16 * k) >> 2], lo = r[(MC_DMA_TLB_LO + 16 * k) >> 2];
        if ((va & 0xFFC00000u) != (hi & 0xFFC00000u) || !(lo & 2)) continue;
        uint64_t pte;
        uint32_t pte_pa = ((lo & 0x03FFFFC0u) << 6) + ((va >> 12) & 0x3FF) * 4;
        if (pm_read(&m->pm, pte_pa, 4, &pte) || !(pte & 2)) return 0;
        *pa = (((uint32_t)pte & 0x03FFFFC0u) << 6) | (va & 0xFFF);
        return 1;
    }
    return 0;
}

/* VDMA: SIZE >> 16 righe di SIZE & 0xFFFF byte tra la memoria (MEMADR,
   che avanza, con STRIDE & 0xFFFF byte in piu' dopo ogni riga) e un
   indirizzo GIO fisso (per la grafica hostrw0 della Newport); oppure
   riempimento della memoria. Lato memoria e' un flusso di byte (X manda
   righe di 130 byte da indirizzi non allineati, con GIO_ADR spostato
   dello stesso numero di byte: hostrw0 + 6); lato GIO parole da 64 bit,
   ogni riga dal byte GIO_ADR & 7 (vedi gio_send). Si fa tutto subito; DMA_RUN
   resta in corso per poco perche' la PROM vuole vederlo acceso. */
/* una riga verso il GIO: parole da 64 bit all'indirizzo allineato; se
   GIO_ADR non e' allineato la riga comincia a quel byte, e le parole
   parziali di testa e di coda diventano scritture piu' piccole */
static void gio_send(Indy *m, uint32_t gio, const uint8_t *p, uint32_t len)
{
    uint32_t base = gio & ~7u, lane = gio & 7;
    while (len) {
        if (!lane && len >= 8) {
            pm_write(&m->pm, base, 8, be64(p));
            p += 8; len -= 8;
            continue;
        }
        /* niente 32 bit sul byte 0: per la Newport sarebbe hostrw0 intero
           (col doppio, 8 pixel); da 2 byte e' una parola parziale */
        uint32_t sz = lane == 4 && len >= 4 ? 4 : !(lane & 1) && len >= 2 ? 2 : 1;
        pm_write(&m->pm, base + lane, (int)sz, sz == 4 ? be32(p) : sz == 2 ? be16(p) : *p);
        p += sz; len -= sz;
        lane = (lane + sz) & 7;
    }
}

static void vdma_run(Indy *m)
{
    IndyMC *mc = &m->mc;
    uint32_t *r = mc->regs;
    uint32_t mode = r[DMA_MODE >> 2], width = r[DMA_SIZE >> 2] & 0xFFFF;
    uint32_t gio = r[DMA_GIO_ADR >> 2], va = r[DMA_MEMADR >> 2];
    int32_t skip = (int16_t)(r[DMA_STRIDE >> 2] & 0xFFFF);
    int to_mem = (mode & (DMA_MODE_TOHOST | DMA_MODE_FILL)) != 0;
    uint8_t *buf = mc->dma_line;
    mc->dma_busy_until = indy_now_ns(m) + DMA_BUSY_NS;
    mc->dma_status = DMA_DONE;
    while (mc->dma_lines) {
        uint32_t pos = width - mc->dma_left;
        if (to_mem && !mc->dma_line_ready) {              /* la riga da scrivere in memoria */
            if (mode & DMA_MODE_FILL)
                for (uint32_t k = 0; k < width; k++)
                    buf[k] = (uint8_t)(gio >> (24 - 8 * ((va - pos + k) & 3)));
            else
                for (uint32_t k = 0; k < width; k += 8) {
                    uint64_t v;
                    pm_read(&m->pm, gio, 8, &v);
                    wbe64(buf + k, v);
                }
            mc->dma_line_ready = 1;
        }
        while (mc->dma_left) {
            uint32_t pa, n = 0x1000 - (va & 0xFFF);
            if (!vdma_xlate(m, va, &pa)) {                /* pagina da sistemare */
                mc->dma_status = 0;
                mc->dma_busy_until = 0;
                r[DMA_MEMADR >> 2] = va;
                return;
            }
            if (n > mc->dma_left) n = mc->dma_left;
            if (to_mem) pm_dma_write(&m->pm, pa, buf + pos, n);
            else pm_dma_read(&m->pm, pa, buf + pos, n);
            va += n; pos += n;
            mc->dma_left -= n;
        }
        if (!to_mem) gio_send(m, gio, buf, width);       /* la riga al GIO */
        mc->dma_line_ready = 0;
        va += (uint32_t)skip;
        mc->dma_lines--;
        mc->dma_left = mc->dma_lines ? width : 0;
    }
    r[DMA_MEMADR >> 2] = va;
    mc->dma_paused = 0;
    mc->dma_irq_pending = 1;                         /* interrupt di fine: da service() */
}

static void vdma_start(Indy *m)
{
    IndyMC *mc = &m->mc;
    uint32_t size = mc->regs[DMA_SIZE >> 2];
    mc->dma_lines = size >> 16 ? size >> 16 : 1;
    mc->dma_left = size & 0xFFFF;
    mc->dma_line_ready = 0;
    mc->dma_paused = 1;
    vdma_run(m);
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
    pm_clear_mirrors(pm);
    pm_add_mirror(pm, 0, 0x08000000u, 0x80000u);
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
    case DMA_RUN:      return indy_now_ns(m) < mc->dma_busy_until ? DMA_RUNNING : mc->dma_status;
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
    case MC_DMA_CAUSE:                                   /* 0: interrupt di fine DMA servito */
        mc->regs[off >> 2] = v;
        if (!v) { mc->dma_irq_pending = 0; indy_set_lio0(m, L0_MCDMA, 0); }
        break;
    case DMA_MEMADRD: case DMA_MEMADRDS:                 /* indirizzo e valori di default */
        mc->regs[DMA_MEMADR >> 2] = v;
        mc->regs[DMA_SIZE >> 2] = 0;
        mc->regs[DMA_STRIDE >> 2] = 0;
        mc->regs[DMA_MODE >> 2] = 0;
        if (off == DMA_MEMADRDS) vdma_start(m);
        break;
    case DMA_GIO_ADRS: mc->regs[DMA_GIO_ADR >> 2] = v; vdma_start(m); break;
    case DMA_STDMA:                                      /* 1: riprende dopo una pagina */
        if (!v) { mc->dma_paused = 0; mc->dma_status = 0; mc->dma_busy_until = 0; }
        else if (mc->dma_paused) vdma_run(m);
        else vdma_start(m);
        break;
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
    if (m->mc_log && ((off >= 0x2000 && off < 0x3000) || (off >= 0x160 && off < 0x170)))   /* letture del VDMA */
        fprintf(stderr, "[MC %04X -> %08X pc=%llX]\n", off, w, (unsigned long long)m->cpu.pc);
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
