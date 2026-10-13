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

static void bridge_update_irq(Octane *m);
static uint32_t bridge_reg32(const Octane *m, uint32_t off);

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

/* BAR dei dispositivi: dimensione e spazio (I/O o memoria) */
typedef struct PciBar { uint32_t size; int io; } PciBar;
static const PciBar pci_bars[8][6] = {
    { { 0x100, 1 }, { 0x1000, 0 } },    /* QLogic: registri in I/O e in memoria */
    { { 0x100, 1 }, { 0x1000, 0 } },
    { { 0x100000, 0 } },                /* IOC3: 1 MB di memoria */
};

static const uint32_t pci_ids[8] = {
    0x10201077, 0x10201077,             /* slot 0, 1: QLogic ISP1040 (1077:1020), SCSI 0 e 1 */
    0x000310A9,                         /* slot 2: IOC3 (SGI 10A9:0003) */
    0, 0, 0, 0, 0
};

/* ---- QLogic nelle finestre di dispositivo 0 e 1 del BRIDGE ----
   Mezze parole: indirizzo ^ 2, valore naturale; i byte di conseguenza. */

static int ql_io_read(Octane *m, int n, uint32_t off, int size, uint64_t *val)
{
    QlIsp *q = &m->ql[n];
    switch (size) {
    case 1: { uint16_t v = ql_read(q, (off & ~1u) ^ 2); *val = (off & 1) ? v & 0xFF : v >> 8; break; }
    case 2: *val = ql_read(q, off ^ 2); break;
    case 4: *val = (uint32_t)ql_read(q, off + 2) << 16 | ql_read(q, off); break;
    default:
        *val = (uint64_t)ql_read(q, off + 2) << 48 | (uint64_t)ql_read(q, off) << 32
             | (uint64_t)ql_read(q, off + 6) << 16 | ql_read(q, off + 4);
        break;
    }
    return 0;
}

static int ql_io_write(Octane *m, int n, uint32_t off, int size, uint64_t val)
{
    QlIsp *q = &m->ql[n];
    switch (size) {
    case 1: {
        uint16_t v = ql_read(q, (off & ~1u) ^ 2);
        v = (off & 1) ? (uint16_t)((v & 0xFF00) | (val & 0xFF)) : (uint16_t)((v & 0xFF) | (val & 0xFF) << 8);
        ql_write(q, (off & ~1u) ^ 2, v);
        break;
    }
    case 2: ql_write(q, off ^ 2, (uint16_t)val); break;
    case 4: ql_write(q, off + 2, (uint16_t)(val >> 16)); ql_write(q, off, (uint16_t)val); break;
    default:
        ql_write(q, off + 2, (uint16_t)(val >> 48)); ql_write(q, off, (uint16_t)(val >> 32));
        ql_write(q, off + 6, (uint16_t)(val >> 16)); ql_write(q, off + 4, (uint16_t)val);
        break;
    }
    bridge_update_irq(m);                                    /* il chip puo' aver cambiato l'interrupt */
    return 0;
}

/* indirizzo PCI di un DMA -> indirizzo XIO, che per il HEART e' quello
   fisico. Tre finestre:
     0x80000000-0xFFFFFFFF  diretta: DIR_MAP (0x84) da' l'offset in blocchi
                            da 2 GB (bit 16:0) e +512 MB (bit 17)
     0x40000000-0x7FFFFFFF  ATE: una voce per pagina (16 KB con il bit 21
                            del controllo del widget, se no 4 KB), indirizzo
                            nei bit 47:12, bit 0 valida
     sopra i 4 GB           PCI a 64 bit: attributi nei bit 63:48
   Fuori da queste, o con un'ATE non valida: ~0. */
static uint64_t pci_to_phys(Octane *m, uint64_t pci)
{
    if (pci >> 32) return pci & 0x0000FFFFFFFFFFFFull;
    if (pci & 0x80000000u) {
        uint32_t dm = bridge_reg32(m, 0x84);
        return ((uint64_t)(dm & 0x1FFFF) << 31) + (dm & 0x20000 ? 0x20000000u : 0) + (pci & 0x7FFFFFFFu);
    }
    if (pci & 0x40000000u) {
        uint32_t page = (bridge_reg32(m, 0x24) & (1u << 21)) ? 0x4000 : 0x1000;
        uint32_t idx = (uint32_t)(pci & 0x3FFFFFFF) / page;
        if (idx >= 128 || !(m->bridge_ate[idx] & 1)) return ~0ull;
        return (m->bridge_ate[idx] & 0x0000FFFFFFFFF000ull & ~(uint64_t)(page - 1)) + (pci & (page - 1));
    }
    return ~0ull;
}

/* un DMA puo' attraversare piu' pagine ATE: si spezza a pagine da 4 KB */
static uint32_t dma_pages(Octane *m, uint64_t pci, uint8_t *buf, uint32_t len, int write)
{
    uint32_t done = 0;
    while (done < len) {
        uint32_t n = 0x1000 - (uint32_t)((pci + done) & 0xFFF);
        if (n > len - done) n = len - done;
        uint64_t pa = pci_to_phys(m, pci + done);
        if (pa == ~0ull) break;
        uint32_t k = write ? pm_dma_write(&m->pm, pa, buf + done, n) : pm_dma_read(&m->pm, pa, buf + done, n);
        done += k;
        if (k < n) break;
    }
    return done;
}

static uint32_t ql_dma_read(void *ctx, uint64_t pci, uint8_t *dst, uint32_t len)
{
    return dma_pages(ctx, pci, dst, len, 0);
}

static uint32_t ql_dma_write(void *ctx, uint64_t pci, const uint8_t *src, uint32_t len)
{
    return dma_pages(ctx, pci, (uint8_t *)src, len, 1);
}

static void ql_log(void *ctx, const char *msg)
{
    Octane *m = ctx;
    if (m->scsi_log) fprintf(stderr, "%s  [pc=%llX]\n", msg, (unsigned long long)m->cpu.pc);
}

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
    if (r >= 0x10 && r < 0x28) {                          /* BAR: solo i bit dell'indirizzo */
        const PciBar *b = &pci_bars[slot][(r - 0x10) >> 2];
        *w = b->size ? (*w & ~(b->size - 1)) | (b->io ? 1u : 0u) : 0;
    }
    return 0;
}

/* Finestre di dispositivo del BRIDGE: 0 e 1 da 2 MB a 0x200000, 2..7 da
   1 MB a 0x600000. Il registro del dispositivo della finestra (0x204 + 8 w)
   da' l'indirizzo PCI (bit 11:0 = bit 31:20) e lo spazio (bit 12: memoria,
   se no I/O); risponde il dispositivo che ha un BAR su quell'indirizzo. */
static int pci_decode(Octane *m, uint32_t off, int *slot, uint32_t *dev_off)
{
    int w = off < 0x600000 ? (int)((off - 0x200000) >> 21) : 2 + (int)((off - 0x600000) >> 20);
    uint32_t woff = off < 0x600000 ? off & 0x1FFFFF : off & 0xFFFFF;
    uint32_t dev = bridge_reg32(m, 0x204 + 8u * (uint32_t)w);
    uint32_t pci = ((dev & 0xFFF) << 20) + woff;
    int io = !(dev & 0x1000);
    for (int s = 0; s < 8; s++) {
        if (!pci_ids[s]) continue;
        for (int b = 0; b < 6; b++) {
            const PciBar *bar = &pci_bars[s][b];
            if (!bar->size || bar->io != io) continue;
            uint32_t base = m->pcicfg[s][4 + b] & ~(bar->size - 1);
            if (pci - base < bar->size) { *slot = s; *dev_off = pci - base; return 1; }
        }
    }
    return 0;
}

static int pci_dev_read(Octane *m, uint32_t pa, uint32_t off, int size, uint64_t *val)
{
    int slot; uint32_t o;
    if (!pci_decode(m, off, &slot, &o)) { log_io(m, 0, pa, size, 0, "PCI: nessuno"); *val = 0; return 0; }
    return slot == 2 ? ioc3_read(m, o, size, val) : ql_io_read(m, slot, o, size, val);
}

static int pci_dev_write(Octane *m, uint32_t pa, uint32_t off, int size, uint64_t val)
{
    int slot; uint32_t o;
    if (!pci_decode(m, off, &slot, &o)) { log_io(m, 1, pa, size, val, "PCI: nessuno"); return 0; }
    return slot == 2 ? ioc3_write(m, o, size, val) : ql_io_write(m, slot, o, size, val);
}

/* ---- BRIDGE (widget 15): registri, configurazione PCI, dispositivi ---- */

static int bridge_read(void *ctx, uint32_t pa, int size, uint64_t *val)
{
    Octane *m = ctx;
    uint32_t off = pa - SWIN(BRIDGE_W);
    if ((off & ~7u) == 0xB0) { *val = sub_read(3, off, size); return 0; }   /* NIC: vedi IOC3 MCR */
    if ((off & ~7u) == 0x100) { *val = sub_read(m->bridge_pins, off, size); return 0; }  /* INT_STATUS */
    if (off < sizeof m->bridge) {
        *val = sub_read(m->bridge[off >> 3], off, size);
        if (m->ioc3_log) log_io(m, 0, pa, size, *val, "BRIDGE reg");
        return 0;
    }
    if (off >= 0x20000 && off < 0x28000) return pcicfg_read(m, off - 0x20000, size, val);
    if (off >= 0x10000 && off < 0x10400) { *val = sub_read(m->bridge_ate[(off - 0x10000) >> 3] >> 32, off | 4, size); return 0; }
    if (off >= 0x11000 && off < 0x11400) { *val = sub_read((uint32_t)m->bridge_ate[(off - 0x11000) >> 3], off | 4, size); return 0; }
    if (off >= 0x200000 && off < 0xC00000) return pci_dev_read(m, pa, off, size, val);
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
        if (off >= 0x100 && off < 0x180) bridge_update_irq(m);    /* abilitazioni, vettori */
        return 0;
    }
    if (off >= 0x20000 && off < 0x28000) return pcicfg_write(m, off - 0x20000, size, val);
    if (off >= 0x10000 && off < 0x10400) {                  /* ATE interne (64 bit) */
        uint64_t *a = &m->bridge_ate[(off - 0x10000) >> 3];
        *a = sub_write(*a, off, size, val);
        return 0;
    }
    if (off >= 0x200000 && off < 0xC00000) return pci_dev_write(m, pa, off, size, val);
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
    for (int k = 0; k < 2; k++) {
        if (ql_init(&m->ql[k])) return -1;
        m->ql[k].dma_read = ql_dma_read;
        m->ql[k].dma_write = ql_dma_write;
        m->ql[k].log = ql_log;
        m->ql[k].ctx = m;
        m->ql[k].bus = k;
    }
    oct_reset(m);
    return 0;
}

void oct_free(Octane *m)
{
    for (int b = 0; b < 2; b++) {
        for (int k = 0; k < QL_TARGETS; k++) scsi_dev_close(&m->disk[b][k]);
        ql_free(&m->ql[b]);
    }
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
    memset(m->bridge_ate, 0, sizeof m->bridge_ate);
    m->bridge_pins = m->bridge_sent = 0;
    m->bridge[0] = BRIDGE_ID;
    m->bridge[1] = 0x20;                               /* WIDGET_STATUS: bus PCI (non GIO) */
    memset(m->pcicfg, 0, sizeof m->pcicfg);
    for (int s = 0; s < 8; s++) m->pcicfg[s][0] = pci_ids[s];
    m->pcicfg[2][2] = 0xFF000001;                      /* IOC3: classe "altro", revisione 1 */
    m->pcicfg[0][2] = m->pcicfg[1][2] = 0x01000005;    /* QLogic: SCSI, revisione 5 (1040B) */
    for (int k = 0; k < 2; k++) ql_reset(&m->ql[k]);
    memset(m->ioc3, 0, sizeof m->ioc3);
    memset(m->uart, 0, sizeof m->uart);
    m->power_off = 0;
    update_irq(m);
}

int oct_attach_scsi(Octane *m, int bus, int id, int type, const char *path, int readonly)
{
    if (bus < 0 || bus > 1 || id < 0 || id >= QL_TARGETS) return -1;
    ScsiDev *d = &m->disk[bus][id];
    if (scsi_dev_open(d, type, path, readonly)) return -1;
    m->ql[bus].dev[id] = d;
    return 0;
}

/* Interrupt dei dispositivi PCI: lo slot n alza il piedino n del BRIDGE.
   Su un fronte di salita di un piedino abilitato (INT_ENABLE, 0x10C) il
   BRIDGE manda al HEART un pacchetto che accende il bit dell'ISR scritto in
   INT_ADDR del piedino (0x134 + 8 n, bit 5:0); se INT_MODE (0x11C) lo
   chiede, sulla discesa ne manda uno che lo spegne. INT_STATUS (0x104)
   mostra i piedini. */
static uint32_t bridge_reg32(const Octane *m, uint32_t off)
{
    return (uint32_t)sub_read(m->bridge[off >> 3], off, 4);
}

static void bridge_update_irq(Octane *m)
{
    uint8_t pins = 0;
    if (ql_irq(&m->ql[0])) pins |= 1;
    if (ql_irq(&m->ql[1])) pins |= 2;
    m->bridge_pins = pins;
    uint32_t enable = bridge_reg32(m, 0x10C), mode = bridge_reg32(m, 0x11C);
    for (int p = 0; p < 8; p++) {
        uint8_t bit = (uint8_t)(1u << p);
        uint64_t vec = 1ull << (bridge_reg32(m, 0x134 + 8u * (uint32_t)p) & 63);
        if ((pins & bit) && (enable & bit)) {
            if (!(m->bridge_sent & bit)) { m->heart.isr |= vec; m->bridge_sent |= bit; }
        } else if (m->bridge_sent & bit) {
            m->bridge_sent &= (uint8_t)~bit;
            if (mode & bit) m->heart.isr &= ~vec;
        }
    }
    update_irq(m);
}

/* timer del HEART: il bit 50 dell'ISR quando i 24 bit bassi del contatore
   raggiungono compare */
static void heart_timer(Octane *m)
{
    uint64_t now = heart_count(m), last = m->heart.last_count;
    uint64_t d = (m->heart.compare - last) & 0xFFFFFF;
    if (d && d <= now - last) m->heart.isr |= 1ull << 50;
    m->heart.last_count = now;
}

static void service(Octane *m)
{
    heart_timer(m);
    bridge_update_irq(m);
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
