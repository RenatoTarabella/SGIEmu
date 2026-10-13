/*
 * octane.c - Montaggio dell'Octane, HEART, XBOW, BRIDGE e seriale dell'IOC3
 *
 * Prima versione: quanto serve alla PROM per partire. Quello che non c'e'
 * ancora finisce nel log degli accessi (io_log).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "octane/octane.h"

#define SLICE      2000u                /* istruzioni tra un aggiornamento e l'altro */
#define HEART_BASE 0x0FF00000u
#define HEART_SIZE 0x00070000u
#define SWIN(w)    (0x10000000u | ((uint32_t)(w) << 24))
#define BRIDGE_W   15
#define IOC3_BASE  0x1F600000u          /* finestra di dispositivo 2 del BRIDGE */

/* identita' dei widget (registro WIDGET_ID, a +4) */
#define WID(rev, part, mfg) ((uint32_t)(rev) << 28 | (uint32_t)(part) << 12 | (uint32_t)(mfg) << 1 | 1)
#define XBOW_ID   WID(4, 0x0000, 0x000)
#define HEART_ID  WID(6, 0xC001, 0x036)   /* rev F: niente casi speciali per i DIMM vecchi */
#define BRIDGE_ID WID(4, 0xC002, 0x036)

/* ---- accessi a registri da 64 bit, anche a pezzi (big-endian) ---- */

static uint64_t sub_read(uint64_t v, uint32_t off, int size)
{
    if (size == 8) return v;
    int sh = (8 - size - (int)(off & 7)) * 8;
    return (v >> sh) & ((1ull << (size * 8)) - 1);
}

static uint64_t sub_write(uint64_t old, uint32_t off, int size, uint64_t val)
{
    if (size == 8) return val;
    int sh = (8 - size - (int)(off & 7)) * 8;
    uint64_t mask = ((1ull << (size * 8)) - 1) << sh;
    return (old & ~mask) | ((val << sh) & mask);
}

static void log_io(Octane *m, int write, uint64_t pa, int size, uint64_t val, const char *what)
{
    if (m->io_log) m->io_log(m, write, pa, size, val, what);
}

/* ---- interrupt: HEART -> CPU ---- */

static void update_irq(Octane *m)
{
    uint64_t p = m->heart.isr & m->heart.imr[0];
    r4k_set_irq(&m->cpu, 0, (p & 0x000000000000FFFFull) != 0);    /* L0 -> IP2 */
    r4k_set_irq(&m->cpu, 1, (p & 0x00000000FFFF0000ull) != 0);    /* L1 -> IP3 */
    r4k_set_irq(&m->cpu, 2, (p & 0x0003FFFF00000000ull) != 0);    /* L2 -> IP4 */
    r4k_set_irq(&m->cpu, 3, (p & 0x0004000000000000ull) != 0);    /* L3 (timer) -> IP5 */
    r4k_set_irq(&m->cpu, 4, (p & 0xFFF8000000000000ull) != 0);    /* L4 (errori) -> IP6 */
}

/* ---- HEART ---- */

/* MEMCFG (4 registri a 32 bit da 0x20): bit 31 valido, 24:22 densita',
   21:16 dimensione in unita' di 32 MB meno 1, 8:0 base in unita' di 32 MB
   da OCT_RAM_BASE.
 *
 * La PROM riconosce i DIMM configurando il banco alla densita' massima
 * (6, 2 GB) e guardando quali bit d'indirizzo il DIMM ignora: scrive a +0
 * e a +256 B, +128 MB, +32 MB, +64 MB (+1 GB con un HEART prima della
 * revisione 6) e vede se si sovrappongono; la combinazione indica una
 * tabella (0x9FC1BF38) con densita' e dimensione.
 * Qui ogni taglia di banco e' un DIMM di quella tabella: finche' MEMCFG non
 * corrisponde al suo tipo, il banco risponde come un DIMM vero (i bit che
 * ignora non contano, poche parole in probe[]); con il tipo giusto la RAM
 * e' mappata e basta. Un banco senza DIMM non risponde. */
static const struct { uint32_t mb, cfg, ignored; } dimm_types[] = {
    /* dimensione, densita' e dimensione in MEMCFG, bit ignorati alla densita' massima */
    {   64, 0x01010000u, 1u << 25 | 1u << 26 | 1u << 27 },
    {  128, 0x01030000u, 1u << 25 | 1u << 27 },
    {  256, 0x01470000u, 1u << 26 | 1u << 27 },
    {  512, 0x014F0000u, 1u << 27 },
    { 1024, 0x019F0000u, 1u << 26 },
};

static uint32_t memcfg(const Octane *m, int b)
{
    return (uint32_t)(m->heart.regs[(0x20 >> 3) + (b >> 1)] >> ((b & 1) ? 0 : 32));
}

static void mem_remap(Octane *m)
{
    for (int b = 0; b < 4; b++)
        if (m->map_size[b]) { pm_unmap(&m->pm, m->map_base[b], m->map_size[b]); m->map_size[b] = 0; }
    for (int b = 0; b < 4; b++) {
        uint32_t cfg = memcfg(m, b);
        m->probe_n[b] = 0;
        if (!(cfg & 0x80000000u) || !m->bank_size[b]) continue;
        uint64_t base = OCT_RAM_BASE + ((uint64_t)(cfg & 0x1FF) << 25);
        uint64_t size = (uint64_t)(((cfg >> 16) & 0x3F) + 1) << 25;
        if (base >= 0x100000000ull) continue;                   /* oltre i 4 GB: per ora no */
        if (base + size > 0x100000000ull) size = 0x100000000ull - base;
        m->map_base[b] = (uint32_t)base;
        if ((cfg & 0x01FF0000u) != m->bank_cfg[b]) { m->map_size[b] = 0; m->probe_size[b] = (uint32_t)size; continue; }
        pm_map_ram(&m->pm, (uint32_t)base, m->bank_size[b], m->bank[b], PM_RAM);
        m->map_size[b] = m->bank_size[b];
        m->probe_size[b] = 0;
    }
    /* i primi OCT_LOWMEM byte della RAM compaiono anche da 0 (vettori delle
       eccezioni) */
    pm_unmap(&m->pm, 0, OCT_LOWMEM);
    pm_clear_mirrors(&m->pm);
    uint8_t *low = pm_host(&m->pm, OCT_RAM_BASE);
    if (low && (m->pm.flags[OCT_RAM_BASE >> PM_PAGE_SHIFT] & PM_RAM)) {
        pm_map_ram(&m->pm, 0, OCT_LOWMEM, low, PM_RAM);
        pm_add_mirror(&m->pm, 0, OCT_RAM_BASE, OCT_LOWMEM);
    }
    r4k_fast_flush(&m->cpu);
}

/* banco configurato con un tipo diverso dal suo: 1 se pa ci cade dentro */
static int probe_access(Octane *m, uint32_t pa, int size, uint64_t *val, int write)
{
    for (int b = 0; b < 4; b++) {
        uint32_t cfg = memcfg(m, b);
        if (!m->probe_size[b] || !(cfg & 0x80000000u) || pa - m->map_base[b] >= m->probe_size[b]) continue;
        uint32_t key = (pa - m->map_base[b]) & ~m->bank_ignored[b] & ~7u;
        int k = 0;
        while (k < m->probe_n[b] && m->probe_key[b][k] != key) k++;
        if (write) {
            if (k == m->probe_n[b]) {
                if (k == OCT_PROBE_WORDS) return 1;
                m->probe_key[b][k] = key; m->probe_val[b][k] = 0; m->probe_n[b]++;
            }
            m->probe_val[b][k] = sub_write(m->probe_val[b][k], pa, size, *val);
        } else *val = k < m->probe_n[b] ? sub_read(m->probe_val[b][k], pa, size) : 0;
        return 1;
    }
    return 0;
}

static uint64_t heart_count(const Octane *m)
{
    return (oct_now_ns(m) / 80 + m->heart.count_ofs) & 0x000FFFFFFFFFFFFFull;
}

static int heart_read(void *ctx, uint32_t pa, int size, uint64_t *val)
{
    Octane *m = ctx;
    OctHeart *h = &m->heart;
    uint32_t off = pa - HEART_BASE, r = off & ~7u;
    uint64_t v;
    if (r == 0xB8) v = 3;                             /* MLAN_CTL: bus MicroLAN vuoto (vedi IOC3) */
    else if (r < 0x100) v = h->regs[r >> 3];
    else switch (r) {
    case 0x10000: case 0x10008: case 0x10010: case 0x10018: v = h->imr[(r >> 3) & 3]; break;
    case 0x10030: v = h->isr; break;
    case 0x10038: v = h->isr & h->imr[0]; break;
    case 0x10040: v = h->cause; break;
    case 0x20000: v = heart_count(m); break;
    case 0x30000: v = h->compare; break;
    case 0x40000: v = h->trigger; break;
    case 0x50000: v = 0; break;                       /* CPU che legge: la 0 */
    case 0x60000: v = h->sync; break;
    default:
        log_io(m, 0, pa, size, 0, "HEART");
        v = 0;
        break;
    }
    *val = sub_read(v, off, size);
    return 0;
}

static int heart_write(void *ctx, uint32_t pa, int size, uint64_t val)
{
    Octane *m = ctx;
    OctHeart *h = &m->heart;
    uint32_t off = pa - HEART_BASE, r = off & ~7u;
    if (r < 0x100) {
        if (r != 0x80) h->regs[r >> 3] = sub_write(h->regs[r >> 3], off, size, val);   /* STATUS: sola lettura */
        if (r >= 0x20 && r < 0x40) mem_remap(m);
        if (m->heart_log) log_io(m, 1, pa, size, val, "HEART reg");
        return 0;
    }
    switch (r) {
    case 0x10000: case 0x10008: case 0x10010: case 0x10018:
        h->imr[(r >> 3) & 3] = sub_write(h->imr[(r >> 3) & 3], off, size, val);
        break;
    case 0x10020: h->isr |= sub_write(0, off, size, val); break;            /* SET_ISR */
    case 0x10028: h->isr &= ~sub_write(0, off, size, val); break;           /* CLR_ISR */
    case 0x10040: h->cause &= ~sub_write(0, off, size, val); break;         /* scrivendo si azzera */
    case 0x20000:
        h->count_ofs = sub_write(heart_count(m), off, size, val) - oct_now_ns(m) / 80;
        break;
    case 0x30000: h->compare = sub_write(h->compare, off, size, val) & 0xFFFFFF; break;
    case 0x40000: h->trigger = sub_write(h->trigger, off, size, val); break;
    case 0x60000: h->sync = sub_write(h->sync, off, size, val); break;
    default:
        log_io(m, 1, pa, size, val, "HEART");
        return 0;
    }
    update_irq(m);
    return 0;
}

/* registri da widget del HEART (finestra del widget 8) */
static int heart_w_read(void *ctx, uint32_t pa, int size, uint64_t *val)
{
    Octane *m = ctx;
    uint32_t off = pa - SWIN(8);
    if (off >= sizeof m->heart.wregs) { log_io(m, 0, pa, size, 0, "HEART widget"); *val = 0; return 0; }
    *val = sub_read(m->heart.wregs[off >> 3], off, size);
    if (m->ioc3_log) log_io(m, 0, pa, size, *val, "HEART wreg");
    return 0;
}

static int heart_w_write(void *ctx, uint32_t pa, int size, uint64_t val)
{
    Octane *m = ctx;
    uint32_t off = pa - SWIN(8);
    if (off >= sizeof m->heart.wregs || off < 8) { log_io(m, 1, pa, size, val, "HEART widget"); return 0; }
    m->heart.wregs[off >> 3] = sub_write(m->heart.wregs[off >> 3], off, size, val);
    return 0;
}

/* ---- XBOW (widget 0) ---- */

static int xbow_read(void *ctx, uint32_t pa, int size, uint64_t *val)
{
    Octane *m = ctx;
    uint32_t off = pa - SWIN(0);
    if (off >= sizeof m->xbow) { log_io(m, 0, pa, size, 0, "XBOW"); *val = 0; return 0; }
    *val = sub_read(m->xbow[off >> 3], off, size);
    if (m->ioc3_log) log_io(m, 0, pa, size, *val, "XBOW reg");
    return 0;
}

static int xbow_write(void *ctx, uint32_t pa, int size, uint64_t val)
{
    Octane *m = ctx;
    uint32_t off = pa - SWIN(0);
    if (off >= sizeof m->xbow || off < 8) { log_io(m, 1, pa, size, val, "XBOW"); return 0; }
    m->xbow[off >> 3] = sub_write(m->xbow[off >> 3], off, size, val);
    return 0;
}

/* ---- slot XIO vuoti: bus error ---- */

static int noxio_read(void *ctx, uint32_t pa, int size, uint64_t *val)
{
    log_io(ctx, 0, pa, size, 0, "XIO vuoto");
    *val = 0;
    return 1;
}

static int noxio_write(void *ctx, uint32_t pa, int size, uint64_t val)
{
    log_io(ctx, 1, pa, size, val, "XIO vuoto");
    return 1;
}

/* ---- seriale 16550 dell'IOC3: ordine standard dei registri; con lo scambio dei
   byte del BRIDGE (bit 13 del registro del dispositivo) l'indirizzo e' ^ 3,
   come nella struttura di Linux ---- */

static uint8_t uart_read(Octane *m, int port, int reg)
{
    OctUart *u = &m->uart[port];
    int dlab = (u->lcr & 0x80) != 0;
    switch (reg) {
    case 0:
        if (dlab) return u->dll;
        if (u->rx_n) {
            uint8_t ch = u->rx[u->rx_head];
            u->rx_head = (u->rx_head + 1) % (int)sizeof u->rx;
            u->rx_n--;
            return ch;
        }
        return 0;
    case 1: return dlab ? u->dlm : u->ier;
    case 2: return u->rx_n ? 0xC4 : 0xC1;             /* IIR: FIFO attive, dato o niente */
    case 3: return u->lcr;
    case 4: return u->mcr;
    case 5: return 0x60 | (u->rx_n ? 1 : 0);          /* LSR: trasmettitore vuoto */
    case 6: return 0xB0;                              /* MSR: DCD, DSR, CTS */
    default: return u->scr;
    }
}

static void uart_write(Octane *m, int port, int reg, uint8_t v)
{
    OctUart *u = &m->uart[port];
    int dlab = (u->lcr & 0x80) != 0;
    switch (reg) {
    case 0:
        if (dlab) u->dll = v;
        else if (m->tx) m->tx(m, port, v);
        break;
    case 1: if (dlab) u->dlm = v; else u->ier = v; break;
    case 2: break;                                     /* FCR */
    case 3: u->lcr = v; break;
    case 4: u->mcr = v; break;
    case 7: u->scr = v; break;
    default: break;
    }
}

void oct_serial_rx(Octane *m, int port, uint8_t ch)
{
    OctUart *u = &m->uart[port & 1];
    if (u->rx_n >= (int)sizeof u->rx) return;
    u->rx[(u->rx_head + u->rx_n) % (int)sizeof u->rx] = ch;
    u->rx_n++;
}

/* ---- IOC3 ---- */

static int uart_reg(const Octane *m, uint32_t off)
{
    int swap = (m->bridge[(0x200 >> 3) + 2] & 0x2000) != 0;    /* dispositivo 2 = IOC3 */
    return (int)(off & 7) ^ (swap ? 3 : 0);
}

static int ioc3_read(Octane *m, uint32_t off, int size, uint64_t *val)
{
    if (size == 1 && off >= 0x20170 && off < 0x20180) {
        *val = uart_read(m, off >= 0x20178 ? 0 : 1, uart_reg(m, off));
        if (m->ioc3_log && uart_reg(m, off) != 5) log_io(m, 0, IOC3_BASE + off, size, *val, "UART");
        return 0;
    }
    /* MCR, controllo MicroLAN (1-Wire): per ora il bus e' vuoto, ogni
       operazione e' subito finita (DONE, bit 1) e la linea legge 1 (bit 0),
       quindi il reset non vede nessun chip. Lo stesso per il NIC del BRIDGE. */
    if (off == 0x30 && size == 4) { *val = 3; return 0; }
    if (off == 0x28 && size == 4) { *val = m->ioc3[off >> 2] | 0x400000; return 0; }   /* SIO_CR: arbitro inattivo */
    if (off < sizeof m->ioc3 && size == 4) {
        *val = m->ioc3[off >> 2];
        if (m->ioc3_log) log_io(m, 0, IOC3_BASE + off, size, *val, "IOC3 reg");
        return 0;
    }
    if (off >= 0xA0000 && off < 0xC0000) { *val = m->rtc.index; return 0; }              /* bytebus 1 */
    if (off >= 0xC0000 && off < 0xE0000) { *val = ds1687_read(&m->rtc, oct_now_ns(m)); return 0; }
    log_io(m, 0, IOC3_BASE + off, size, 0, "IOC3");
    *val = 0;
    return 0;
}

static int ioc3_write(Octane *m, uint32_t off, int size, uint64_t val)
{
    if (size == 1 && off >= 0x20170 && off < 0x20180) {
        uart_write(m, off >= 0x20178 ? 0 : 1, uart_reg(m, off), (uint8_t)val);
        if (m->ioc3_log && uart_reg(m, off) != 0) log_io(m, 1, IOC3_BASE + off, size, val, "UART");
        return 0;
    }
    if (off < sizeof m->ioc3 && size == 4) {
        m->ioc3[off >> 2] = (uint32_t)val;
        if (m->ioc3_log) log_io(m, 1, IOC3_BASE + off, size, val, "IOC3 reg");
        return 0;
    }
    if (off >= 0xA0000 && off < 0xC0000) { ds1687_select(&m->rtc, (uint8_t)val); return 0; }
    if (off >= 0xC0000 && off < 0xE0000) {
        ds1687_write(&m->rtc, (uint8_t)val, oct_now_ns(m));
        if (m->rtc.power_off) m->power_off = 1;       /* PAB: spegnimento */
        return 0;
    }
    log_io(m, 1, IOC3_BASE + off, size, val, "IOC3");
    return 0;
}

/* ---- spazio di configurazione PCI dietro il BRIDGE (0x20000 + slot << 12) ----
   Le parole sono quelle PCI (little-endian): a 32 bit si leggono intere,
   a 8 e 16 bit l'indirizzo e' scambiato come sul bus big-endian. Gli slot
   vuoti leggono tutti 1. */

static const uint32_t pci_ids[8] = {
    0, 0, 0x000310A9,                   /* slot 2: IOC3 (SGI 10A9:0003) */
    0, 0, 0, 0, 0
};

static int pcicfg_read(Octane *m, uint32_t off, int size, uint64_t *val)
{
    int slot = (int)(off >> 12) & 7;
    uint32_t r = off & 0xFF;
    if (!pci_ids[slot] || (off & 0xF00)) {               /* master abort: bus error */
        *val = size == 8 ? ~0ull : (1ull << (size * 8)) - 1;
        if (m->ioc3_log) log_io(m, 0, SWIN(BRIDGE_W) + 0x20000 + off, size, *val, "PCI cfg vuoto");
        return 1;
    }
    {
        uint32_t w = m->pcicfg[slot][r >> 2];
        switch (size) {
        case 1: *val = (w >> (8 * ((r & 3) ^ 3))) & 0xFF; break;
        case 2: *val = (w >> (8 * ((r & 2) ^ 2))) & 0xFFFF; break;
        default: *val = w; break;
        }
    }
    if (m->ioc3_log) log_io(m, 0, SWIN(BRIDGE_W) + 0x20000 + off, size, *val, "PCI cfg");
    return 0;
}

static int pcicfg_write(Octane *m, uint32_t off, int size, uint64_t val)
{
    int slot = (int)(off >> 12) & 7;
    uint32_t r = off & 0xFF;
    if (!pci_ids[slot] || (off & 0xF00) || r < 4) return 0;      /* ID: sola lettura */
    uint32_t *w = &m->pcicfg[slot][r >> 2];
    switch (size) {
    case 1: { int sh = 8 * ((r & 3) ^ 3); *w = (*w & ~(0xFFu << sh)) | ((uint32_t)val & 0xFF) << sh; break; }
    case 2: { int sh = 8 * ((r & 2) ^ 2); *w = (*w & ~(0xFFFFu << sh)) | ((uint32_t)val & 0xFFFF) << sh; break; }
    default: *w = (uint32_t)val; break;
    }
    return 0;
}

/* ---- BRIDGE (widget 15): registri, configurazione PCI, dispositivi ---- */

static int bridge_read(void *ctx, uint32_t pa, int size, uint64_t *val)
{
    Octane *m = ctx;
    uint32_t off = pa - SWIN(BRIDGE_W);
    if ((off & ~7u) == 0xB0) { *val = sub_read(3, off, size); return 0; }   /* NIC: vedi IOC3 MCR */
    if (off < sizeof m->bridge) {
        *val = sub_read(m->bridge[off >> 3], off, size);
        if (m->ioc3_log) log_io(m, 0, pa, size, *val, "BRIDGE reg");
        return 0;
    }
    if (off >= 0x20000 && off < 0x28000) return pcicfg_read(m, off - 0x20000, size, val);
    if (off >= 0x600000 && off < 0x700000) return ioc3_read(m, off - 0x600000, size, val);
    log_io(m, 0, pa, size, 0, "BRIDGE");
    *val = 0;
    return 0;
}

static int bridge_write(void *ctx, uint32_t pa, int size, uint64_t val)
{
    Octane *m = ctx;
    uint32_t off = pa - SWIN(BRIDGE_W);
    if (off < sizeof m->bridge && off >= 8) {
        m->bridge[off >> 3] = sub_write(m->bridge[off >> 3], off, size, val);
        return 0;
    }
    if (off >= 0x20000 && off < 0x28000) return pcicfg_write(m, off - 0x20000, size, val);
    if (off >= 0x600000 && off < 0x700000) return ioc3_write(m, off - 0x600000, size, val);
    log_io(m, 1, pa, size, val, "BRIDGE");
    return 0;
}

/* ---- flash della PROM: Sharp LH28F016SU (comandi Intel, a 16 bit) ----
   Nel modo lettura le pagine sono ROM (il codice della PROM gira da li');
   un comando le toglie dalla mappa, cosi' le letture arrivano qui, e il
   ritorno al modo lettura le rimette. Le scritture sulle pagine ROM
   arrivano comunque qui (unmapped_write). La PROM copia in RAM il codice
   che manda i comandi. */

#define FLASH_BASE  (SWIN(BRIDGE_W) + 0x00C00000u)
enum { FL_ARRAY = 0, FL_ID, FL_STATUS, FL_PROGRAM, FL_ERASE };

static void flash_set_mode(Octane *m, int mode)
{
    if (mode == m->flash_mode) return;
    if (m->flash_mode == FL_ARRAY) pm_unmap(&m->pm, FLASH_BASE, OCT_FLASH_SIZE);
    if (mode == FL_ARRAY) pm_map_ram(&m->pm, FLASH_BASE, OCT_FLASH_SIZE, m->prom, PM_ROM);
    m->flash_mode = mode;
    r4k_fast_flush(&m->cpu);
}

static uint64_t flash_read(Octane *m, uint32_t off, int size)
{
    uint64_t v = 0;
    for (int k = 0; k < size; k += 2) {                  /* parole da 16 bit */
        uint32_t a = (off & ~1u) + (uint32_t)k;
        uint16_t w;
        switch (m->flash_mode) {
        case FL_ID: w = (a >> 1 & 3) == 0 ? 0x00B0 : (a >> 1 & 3) == 1 ? 0x66A8 : 0; break;
        case FL_ARRAY: w = be16(m->prom + a); break;
        default: w = 0x0080; break;                       /* stato: pronto, nessun errore */
        }
        v = v << 16 | w;
    }
    return size == 1 ? (off & 1 ? v & 0xFF : v >> 8) : v;
}

static void flash_write(Octane *m, uint32_t off, int size, uint64_t val)
{
    if (m->flash_mode == FL_PROGRAM) {                    /* dato da programmare: solo 1 -> 0 */
        for (int k = size - 1; k >= 0; k--, val >>= 8) m->prom[off + (uint32_t)k] &= (uint8_t)val;
        m->flash_dirty = 1;
        flash_set_mode(m, FL_STATUS);
        return;
    }
    uint8_t cmd = (uint8_t)val;
    if (m->flash_mode == FL_ERASE) {
        if (cmd == 0xD0) {                                /* conferma: blocco da 64 KB a 0xFF */
            memset(m->prom + (off & ~0xFFFFu), 0xFF, 0x10000);
            m->flash_dirty = 1;
        }
        flash_set_mode(m, FL_STATUS);
        return;
    }
    switch (cmd) {
    case 0xFF: flash_set_mode(m, FL_ARRAY); break;
    case 0x90: flash_set_mode(m, FL_ID); break;
    case 0x70: case 0x71: case 0x72: case 0x50: flash_set_mode(m, FL_STATUS); break;
    case 0x40: case 0x10: flash_set_mode(m, FL_PROGRAM); break;
    case 0x20: flash_set_mode(m, FL_ERASE); break;
    case 0xD0: case 0xB0: break;                          /* riprendi, sospendi */
    default: log_io(m, 1, FLASH_BASE + off, size, val, "flash: comando sconosciuto"); break;
    }
}

/* ---- spazi senza dispositivo ---- */

static int unmapped_read(void *ctx, uint32_t pa, int size, uint64_t *val)
{
    Octane *m = ctx;
    if (pa - FLASH_BASE < OCT_FLASH_SIZE) { *val = flash_read(m, pa - FLASH_BASE, size); return 0; }
    if (probe_access(m, pa, size, val, 0)) return 0;
    log_io(m, 0, pa, size, 0, NULL);
    *val = 0;
    return 0;
}

static int unmapped_write(void *ctx, uint32_t pa, int size, uint64_t val)
{
    Octane *m = ctx;
    if (pa - FLASH_BASE < OCT_FLASH_SIZE) { flash_write(m, pa - FLASH_BASE, size, val); return 0; }
    if (probe_access(m, pa, size, &val, 1)) return 0;
    log_io(m, 1, pa, size, val, NULL);
    return 0;
}

/* ---- montaggio ---- */

int oct_init(Octane *m, const uint8_t *prom, uint32_t prom_size, unsigned ram_mb)
{
    memset(m, 0, sizeof *m);
    m->ps_per_insn = 5000;              /* 200 milioni di istruzioni al secondo emulato */
    m->prom = malloc(OCT_FLASH_SIZE);
    if (!m->prom) return -1;
    memset(m->prom, 0xFF, OCT_FLASH_SIZE);                  /* il resto della flash e' vuoto */
    memcpy(m->prom, prom, prom_size < OCT_FLASH_SIZE ? prom_size : OCT_FLASH_SIZE);
    /* banchi: coppie di DIMM, al piu' 1 GB l'uno, riempiti in ordine */
    uint32_t left = ram_mb;
    for (int b = 0; b < 4 && left; b++) {
        int t = 4;
        while (t > 0 && dimm_types[t].mb > left) t--;
        uint32_t mb = dimm_types[t].mb;
        m->bank_cfg[b] = dimm_types[t].cfg;
        m->bank_ignored[b] = dimm_types[t].ignored;
        m->bank_size[b] = mb << 20;
        m->bank[b] = calloc(1, m->bank_size[b]);
        if (!m->bank[b]) return -1;
        m->ram_size += m->bank_size[b];
        left = left > mb ? left - mb : 0;
    }

    if (pm_init(&m->pm) || r4k_init(&m->cpu, &m->pm) || r4k_set_model(&m->cpu, R4K_R10000)) return -1;
    pm_map_io(&m->pm, HEART_BASE, HEART_SIZE, m, heart_read, heart_write, "HEART");
    pm_map_io(&m->pm, SWIN(0), 0x01000000u, m, xbow_read, xbow_write, "XBOW");
    pm_map_io(&m->pm, SWIN(8), 0x01000000u, m, heart_w_read, heart_w_write, "HEART widget");
    for (int w = 9; w < BRIDGE_W; w++)
        pm_map_io(&m->pm, SWIN(w), 0x01000000u, m, noxio_read, noxio_write, "XIO");
    pm_map_io(&m->pm, SWIN(BRIDGE_W), 0x00C00000u, m, bridge_read, bridge_write, "BRIDGE");
    pm_map_ram(&m->pm, FLASH_BASE, OCT_FLASH_SIZE, m->prom, PM_ROM);
    m->pm.unmapped_read = unmapped_read;
    m->pm.unmapped_write = unmapped_write;
    m->pm.unmapped_ctx = m;
    ds1687_init(&m->rtc, (int64_t)time(NULL));
    oct_reset(m);
    return 0;
}

void oct_free(Octane *m)
{
    r4k_free(&m->cpu);
    pm_free(&m->pm);
    for (int b = 0; b < 4; b++) free(m->bank[b]);
    free(m->prom);
}

void oct_reset(Octane *m)
{
    r4k_reset(&m->cpu);
    OctHeart *h = &m->heart;
    memset(h, 0, sizeof *h);
    /* STATUS: widget 8, big-endian, CPU 0 attiva */
    h->regs[0x80 >> 3] = 0x100 | 0x10 | 8;
    mem_remap(m);                                      /* MEMCFG a zero: nessun banco */
    h->wregs[0] = HEART_ID;
    memset(m->xbow, 0, sizeof m->xbow);
    m->xbow[0] = XBOW_ID;
    /* link dei widget 8..15, un blocco ogni 0x40 da 0x100: la PROM vuole il
       bit 5 di LINK_AUX_STATUS (+0x3C, widget presente) e il bit 31 di
       LINK_STATUS (+0x14, link vivo). Ci sono il HEART (8) e il BRIDGE (15). */
    for (int w = 8; w <= 15; w++) {
        int on = w == 8 || w == BRIDGE_W;
        uint32_t blk = 0x100 + 0x40 * (uint32_t)(w - 8);
        m->xbow[(blk + 0x10) >> 3] = on ? 0x80000000u : 0;
        m->xbow[(blk + 0x38) >> 3] = on ? 0x20u : 0;
    }
    memset(m->bridge, 0, sizeof m->bridge);
    m->bridge[0] = BRIDGE_ID;
    m->bridge[1] = 0x20;                               /* WIDGET_STATUS: bus PCI (non GIO) */
    memset(m->pcicfg, 0, sizeof m->pcicfg);
    for (int s = 0; s < 8; s++) m->pcicfg[s][0] = pci_ids[s];
    m->pcicfg[2][2] = 0xFF000001;                      /* IOC3: classe "altro", revisione 1 */
    memset(m->ioc3, 0, sizeof m->ioc3);
    memset(m->uart, 0, sizeof m->uart);
    m->power_off = 0;
    update_irq(m);
}

static void service(Octane *m)
{
    (void)m;
}

uint64_t oct_run(Octane *m, uint64_t n)
{
    uint64_t done = 0;
    while (done < n && !m->power_off) {
        uint64_t chunk = n - done < SLICE ? n - done : SLICE;
        uint64_t ran = r4k_run(&m->cpu, chunk);
        done += ran;
        service(m);
        if (m->cpu.stop && ran < chunk) break;
    }
    return done;
}
