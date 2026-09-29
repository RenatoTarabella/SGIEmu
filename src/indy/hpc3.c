/*
 * hpc3.c - HPC3 e i dispositivi sul suo PBUS: IOC (seriale, tastiera,
 * pannello, registri di sistema), INT3 con il timer 8254, orologio DS1386.
 *
 * Offset dalla base 0x1FB80000:
 *   0x30000  istat0, gio_misc, eeprom, istat1, bestat
 *   0x40000  WD33C93 del SCSI 0: SASR a +3, SCMD a +7
 *   0x48000  WD33C93 del SCSI 1: sull'Indy non ha dispositivi
 *   0x54000  registri esterni ethernet (Seeq 8003)   - non ancora
 *   0x58000  PBUS, 16 canali da 0x400: canale 6 = IOC (0x1FBD9800)
 *   0x5C000  configurazione DMA/PIO del PBUS
 *   0x60000  DS1386: un byte ogni 4 (nel byte basso della parola)
 * I registri dell'IOC sono byte nel byte basso di parole da 32 bit.
 */
#include <stdio.h>
#include <string.h>
#include "indy/indy.h"

#define HPC_BASE  0x1FB80000u
#define IOC_OFF   0x59800u
#define RTC_OFF   0x60000u
#define SCSI0_OFF 0x40000u
#define SCSI1_OFF 0x48000u

enum {
    IOC_UART_B_CTRL = 0x30, IOC_UART_B_DATA = 0x34, IOC_UART_A_CTRL = 0x38,
    IOC_UART_A_DATA = 0x3C, IOC_KBD_DATA = 0x40, IOC_KBD_CMD = 0x44,
    IOC_GCSEL = 0x48, IOC_GENCTRL = 0x4C, IOC_PANEL = 0x50, IOC_SYSID = 0x58,
    IOC_READ = 0x60, IOC_DMASEL = 0x68, IOC_RESET = 0x70, IOC_WRITE = 0x78,
    INT_ISTAT0 = 0x80, INT_IMASK0 = 0x84, INT_ISTAT1 = 0x88, INT_IMASK1 = 0x8C,
    INT_VMEISTAT = 0x90, INT_CMEIMASK0 = 0x94, INT_CMEIMASK1 = 0x98,
    INT_CMEPOL = 0x9C, INT_TCLEAR = 0xA0, INT_ERRSTAT = 0xA4,
    INT_PIT = 0xB0
};

/* ---- DMA SCSI 0: catena di descrittori {pbuf, cntinfo, pnext} ---- */

#define SCSI0_NDPTR 0x10004u
#define SCSI0_CTRL  0x11004u
#define SCTRL_DIR     0x04              /* 1 = dalla memoria al dispositivo */
#define SCTRL_ACTIVE  0x10
#define SCTRL_CRESET  0x40
#define DESC_EOX      0x80000000u

static uint32_t rd32(Indy *m, uint32_t pa)
{
    uint64_t v = 0;
    pm_read(&m->pm, pa, 4, &v);
    return (uint32_t)v;
}

static long scsi_dma(void *user, uint8_t *buf, uint32_t len, int to_mem)
{
    Indy *m = user;
    uint32_t *ctrl = &m->hpc_regs[SCSI0_CTRL >> 2], *nd = &m->hpc_regs[SCSI0_NDPTR >> 2];
    if (!(*ctrl & SCTRL_ACTIVE)) return -1;
    uint32_t desc = *nd, moved = 0;
    int guard = 0;
    while (moved < len && guard++ < 65536) {
        uint32_t pbuf = rd32(m, desc), cnt = rd32(m, desc + 4), next = rd32(m, desc + 8);
        uint32_t n = cnt & 0x3FFF, k = n < len - moved ? n : len - moved;
        if (k) {
            if (to_mem) pm_dma_write(&m->pm, pbuf, buf + moved, k);
            else pm_dma_read(&m->pm, pbuf, buf + moved, k);
            moved += k;
        }
        if (k < n) break;                                 /* descrittore usato a meta' */
        desc = next;
        if (cnt & DESC_EOX) { *ctrl &= ~SCTRL_ACTIVE; break; }
    }
    *nd = desc;
    if (moved >= len) *ctrl &= ~SCTRL_ACTIVE;
    if (m->scsi_log) fprintf(stderr, "[scsi dma %s %u byte]\n", to_mem ? "->mem" : "<-mem", moved);
    return (long)moved;
}

void indy_hpc3_init(Indy *m)
{
    IndyIOC *ioc = &m->ioc;
    memset(ioc, 0, sizeof *ioc);
    ioc->panel = 0xF3;                  /* acceso, nessun tasto premuto */
    ioc->sysid = 0x26;                  /* Indy (non Fullhouse), rev. scheda 3 */
    memset(m->hpc_regs, 0, sizeof m->hpc_regs);
    m->hpc_gio_misc = 0;
    m->hpc_eeprom = 0;
    ee_reset(&m->eeprom_hpc);
    pit_reset(&m->pit);
    scc_reset(&m->scc);
    kbc_reset(&m->kbc);
    wd_reset(&m->scsi);
    m->scsi.dma = scsi_dma;
    m->scsi.user = m;
    wd_reset(&m->scsi1);
}

/* ---- interrupt ---- */

void indy_update_irq(Indy *m)
{
    IndyIOC *ioc = &m->ioc;
    uint8_t map = ioc->map_raw & ~(MAP_SERIAL | MAP_KBD);
    if (scc_irq(&m->scc)) map |= MAP_SERIAL;
    if (kbc_irq(&m->kbc)) map |= MAP_KBD;
    ioc->map_raw = map;
    if (m->scsi.irq) ioc->lio0_raw |= L0_SCSI0; else ioc->lio0_raw &= (uint8_t)~L0_SCSI0;
    if (m->scsi1.irq) ioc->lio0_raw |= L0_SCSI1; else ioc->lio0_raw &= (uint8_t)~L0_SCSI1;
    uint8_t l0 = ioc->lio0_raw | ((map & ioc->cmeimask0) ? L0_MAP0 : 0);
    uint8_t l1 = ioc->lio1_raw | ((map & ioc->cmeimask1) ? L1_MAP1 : 0);
    r4k_set_irq(&m->cpu, IRQ_LOCAL0, (l0 & ioc->imask0) != 0);
    r4k_set_irq(&m->cpu, IRQ_LOCAL1, (l1 & ioc->imask1) != 0);
    r4k_set_irq(&m->cpu, IRQ_TIMER0, ioc->timer_irq[0]);
    r4k_set_irq(&m->cpu, IRQ_TIMER1, ioc->timer_irq[1]);
}

void indy_set_lio0(Indy *m, uint8_t bit, int level)
{
    if (level) m->ioc.lio0_raw |= bit; else m->ioc.lio0_raw &= (uint8_t)~bit;
    indy_update_irq(m);
}

void indy_set_lio1(Indy *m, uint8_t bit, int level)
{
    if (level) m->ioc.lio1_raw |= bit; else m->ioc.lio1_raw &= (uint8_t)~bit;
    indy_update_irq(m);
}

void indy_set_map(Indy *m, uint8_t bit, int level)
{
    if (level) m->ioc.map_raw |= bit; else m->ioc.map_raw &= (uint8_t)~bit;
    indy_update_irq(m);
}

/* ---- IOC e INT3 ---- */

static uint8_t ioc_read(Indy *m, uint32_t reg)
{
    IndyIOC *ioc = &m->ioc;
    uint64_t now = indy_now_ns(m);
    uint8_t v;
    switch (reg) {
    case IOC_UART_B_CTRL: v = scc_read(&m->scc, 1, 0); break;
    case IOC_UART_B_DATA: v = scc_read(&m->scc, 1, 1); break;
    case IOC_UART_A_CTRL: v = scc_read(&m->scc, 0, 0); break;
    case IOC_UART_A_DATA: v = scc_read(&m->scc, 0, 1); break;
    case IOC_KBD_DATA:    v = kbc_read(&m->kbc, 0); break;
    case IOC_KBD_CMD:     v = kbc_read(&m->kbc, 1); break;
    case IOC_GCSEL:       return ioc->gcsel;
    case IOC_GENCTRL:     return ioc->genctrl;
    case IOC_PANEL:       return ioc->panel;
    case IOC_SYSID:       return ioc->sysid;
    case IOC_READ:        return 0;
    case IOC_DMASEL:      return ioc->dmasel;
    case IOC_RESET:       return ioc->reset;
    case IOC_WRITE:       return ioc->write;
    case INT_ISTAT0:
        indy_update_irq(m);
        return (uint8_t)(ioc->lio0_raw | ((ioc->map_raw & ioc->cmeimask0) ? L0_MAP0 : 0));
    case INT_IMASK0:      return ioc->imask0;
    case INT_ISTAT1:
        indy_update_irq(m);
        return (uint8_t)(ioc->lio1_raw | ((ioc->map_raw & ioc->cmeimask1) ? L1_MAP1 : 0));
    case INT_IMASK1:      return ioc->imask1;
    case INT_VMEISTAT:    indy_update_irq(m); return ioc->map_raw;
    case INT_CMEIMASK0:   return ioc->cmeimask0;
    case INT_CMEIMASK1:   return ioc->cmeimask1;
    case INT_CMEPOL:      return ioc->cmepol;
    case INT_TCLEAR:      return 0;
    case INT_ERRSTAT:     return ioc->errstat;
    case INT_PIT: case INT_PIT + 4: case INT_PIT + 8: case INT_PIT + 12:
        return pit_read(&m->pit, (int)(reg - INT_PIT) >> 2, now);
    default:
        if (m->io_log) m->io_log(m, 0, HPC_BASE + IOC_OFF + reg, 1, 0);
        return 0;
    }
    indy_update_irq(m);
    return v;
}

static void ioc_write(Indy *m, uint32_t reg, uint8_t v)
{
    IndyIOC *ioc = &m->ioc;
    uint64_t now = indy_now_ns(m);
    switch (reg) {
    case IOC_UART_B_CTRL: scc_write(&m->scc, 1, 0, v); break;
    case IOC_UART_B_DATA: scc_write(&m->scc, 1, 1, v); break;
    case IOC_UART_A_CTRL: scc_write(&m->scc, 0, 0, v); break;
    case IOC_UART_A_DATA: scc_write(&m->scc, 0, 1, v); break;
    case IOC_KBD_DATA:    kbc_write(&m->kbc, 0, v); break;
    case IOC_KBD_CMD:     kbc_write(&m->kbc, 1, v); break;
    case IOC_GCSEL:       ioc->gcsel = v; break;
    case IOC_GENCTRL:     ioc->genctrl = v; break;
    case IOC_PANEL:
        /* scrivere 0 in POWERON spegne; gli altri bit si azzerano scrivendoli */
        if (!(v & 0x01)) { m->power_off = 1; r4k_stop(&m->cpu); }
        ioc->panel = 0xF3;
        break;
    case IOC_DMASEL:      ioc->dmasel = v; break;
    case IOC_RESET:       ioc->reset = v; break;
    case IOC_WRITE:       ioc->write = v; break;
    case INT_IMASK0:      ioc->imask0 = v; break;
    case INT_IMASK1:      ioc->imask1 = v; break;
    case INT_CMEIMASK0:   ioc->cmeimask0 = v; break;
    case INT_CMEIMASK1:   ioc->cmeimask1 = v; break;
    case INT_CMEPOL:      ioc->cmepol = v; break;
    case INT_TCLEAR:
        if (v & 1) ioc->timer_irq[0] = 0;
        if (v & 2) ioc->timer_irq[1] = 0;
        break;
    case INT_ERRSTAT:     ioc->errstat = 0; break;
    case INT_PIT: case INT_PIT + 4: case INT_PIT + 8: case INT_PIT + 12:
        pit_write(&m->pit, (int)(reg - INT_PIT) >> 2, v, now);
        break;
    default:
        if (m->io_log) m->io_log(m, 1, HPC_BASE + IOC_OFF + reg, 1, v);
        return;
    }
    indy_update_irq(m);
}

/* ---- registri dell'HPC3 ---- */

static uint32_t hpc_read32(Indy *m, uint32_t off)
{
    switch (off) {
    case 0x30000: case 0x3000C: return 0;                       /* istat0/1 */
    case 0x30004: return m->hpc_gio_misc;
    case 0x30008:
        return (m->hpc_eeprom & ~0x10u) | (ee_dout(&m->eeprom_hpc) ? 0x10u : 0);
    case 0x30010: return 0;                                     /* bestat */
    default:
        if (off >= RTC_OFF && off < RTC_OFF + 4 * DS1386_SIZE) {
            int reg = (int)((off - RTC_OFF) >> 2);
            uint8_t v = rtc_read(&m->rtc, reg, indy_now_ns(m));
            if (m->rtc_log && reg >= 14)
                fprintf(stderr, "[nvram %04X -> %02X pc=%llX]\n", reg, v, (unsigned long long)m->cpu.pc);
            return v;
        }
        if (off < 0x30000 || (off >= 0x5C000 && off < RTC_OFF))  /* DMA e configurazione */
            return m->hpc_regs[off >> 2];
        if (m->io_log) m->io_log(m, 0, HPC_BASE + off, 4, 0);
        return m->hpc_regs[(off >> 2) & 0x1FFFF];
    }
}

static void hpc_write32(Indy *m, uint32_t off, uint32_t v)
{
    switch (off) {
    case 0x30004: m->hpc_gio_misc = v; return;
    case 0x30008:
        m->hpc_eeprom = v;
        ee_pins(&m->eeprom_hpc, (v >> 1) & 1, (v >> 2) & 1, (v >> 3) & 1);
        return;
    default:
        if (off >= RTC_OFF && off < RTC_OFF + 4 * DS1386_SIZE) {
            rtc_write(&m->rtc, (int)((off - RTC_OFF) >> 2), (uint8_t)v, indy_now_ns(m));
            return;
        }
        if (off == SCSI0_CTRL && (v & SCTRL_CRESET)) wd_reset(&m->scsi);
        if (!(off < 0x30000 || (off >= 0x5C000 && off < RTC_OFF)) && m->io_log)
            m->io_log(m, 1, HPC_BASE + off, 4, v);
        if (m->scsi_log && off >= 0x10000 && off < 0x14000)
            fprintf(stderr, "[hpc3 %05X = %08X]\n", off, v);
        m->hpc_regs[(off >> 2) & 0x1FFFF] = v;
        return;
    }
}

/* L'IOC e i registri a byte: il dispositivo sta nel byte basso della parola.
   Una lettura a 32 bit lo da' zero-esteso, una a 8 bit solo all'offset +3. */
int indy_hpc3_read(void *ctx, uint32_t pa, int size, uint64_t *val)
{
    Indy *m = ctx;
    uint32_t off = pa - HPC_BASE;
    if (size == 8) { off |= 4; size = 4; }
    if (off >= SCSI0_OFF && off < SCSI0_OFF + 0x4000) {
        *val = ((int)(off & 3) + size == 4) ? wd_read(&m->scsi, (int)((off - SCSI0_OFF) >> 2) & 1) : 0;
        indy_update_irq(m);
        return 0;
    }
    if (off >= SCSI1_OFF && off < SCSI1_OFF + 0x4000) {   /* secondo canale: vuoto */
        *val = ((int)(off & 3) + size == 4) ? wd_read(&m->scsi1, (int)((off - SCSI1_OFF) >> 2) & 1) : 0;
        indy_update_irq(m);
        return 0;
    }
    if (off >= IOC_OFF && off < IOC_OFF + 0x400) {
        uint32_t reg = (off - IOC_OFF) & ~3u;
        int lane = (int)(off & 3);
        *val = (lane + size == 4) ? ioc_read(m, reg) : 0;
        return 0;
    }
    uint32_t w = hpc_read32(m, off & ~3u);
    int sh = 8 * (int)(4 - size - (off & 3));
    *val = size == 4 ? w : (w >> sh) & ((1u << (8 * size)) - 1);
    return 0;
}

int indy_hpc3_write(void *ctx, uint32_t pa, int size, uint64_t val)
{
    Indy *m = ctx;
    uint32_t off = pa - HPC_BASE;
    if (size == 8) { off |= 4; size = 4; }
    if (off >= SCSI0_OFF && off < SCSI0_OFF + 0x4000) {
        if ((int)(off & 3) + size == 4) wd_write(&m->scsi, (int)((off - SCSI0_OFF) >> 2) & 1, (uint8_t)val);
        indy_update_irq(m);
        return 0;
    }
    if (off >= SCSI1_OFF && off < SCSI1_OFF + 0x4000) {
        if ((int)(off & 3) + size == 4) wd_write(&m->scsi1, (int)((off - SCSI1_OFF) >> 2) & 1, (uint8_t)val);
        indy_update_irq(m);
        return 0;
    }
    if (off >= IOC_OFF && off < IOC_OFF + 0x400) {
        uint32_t reg = (off - IOC_OFF) & ~3u;
        if ((int)(off & 3) + size == 4) ioc_write(m, reg, (uint8_t)val);
        return 0;
    }
    if (off >= RTC_OFF && off < RTC_OFF + 4 * DS1386_SIZE) {
        if ((int)(off & 3) + size == 4)
            rtc_write(&m->rtc, (int)((off - RTC_OFF) >> 2), (uint8_t)val, indy_now_ns(m));
        return 0;
    }
    if (size == 4) { hpc_write32(m, off, (uint32_t)val); return 0; }
    uint32_t w = m->hpc_regs[(off >> 2) & 0x1FFFF];
    int sh = 8 * (int)(4 - size - (off & 3));
    uint32_t mask = ((1u << (8 * size)) - 1) << sh;
    hpc_write32(m, off & ~3u, (w & ~mask) | (((uint32_t)val << sh) & mask));
    return 0;
}
