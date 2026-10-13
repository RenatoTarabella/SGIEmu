/*
 * physmem.c - Spazio degli indirizzi fisici: RAM/ROM diretta e I/O a callback
 */
#include <stdlib.h>
#include "mem/physmem.h"

static int default_read(void *ctx, uint32_t pa, int size, uint64_t *val)
{
    (void)ctx; (void)pa; (void)size;
    *val = 0;
    return 1;                       /* bus error */
}

static int default_write(void *ctx, uint32_t pa, int size, uint64_t val)
{
    (void)ctx; (void)pa; (void)size; (void)val;
    return 0;                       /* scrittura persa */
}

int pm_init(PhysMem *pm)
{
    memset(pm, 0, sizeof *pm);
    pm->host  = calloc(PM_NPAGES, sizeof *pm->host);
    pm->flags = calloc(PM_NPAGES, 1);
    pm->io    = calloc(PM_NPAGES, 1);
    pm->addr_mask = ~0ull;
    pm->unmapped_read  = default_read;
    pm->unmapped_write = default_write;
    if (!pm->host || !pm->flags || !pm->io) { pm_free(pm); return -1; }
    return 0;
}

void pm_free(PhysMem *pm)
{
    free(pm->host);  pm->host = NULL;
    free(pm->flags); pm->flags = NULL;
    free(pm->io);    pm->io = NULL;
}

void pm_map_ram(PhysMem *pm, uint32_t base, uint32_t size, uint8_t *host, int flags)
{
    uint32_t first = base >> PM_PAGE_SHIFT, n = size >> PM_PAGE_SHIFT;
    for (uint32_t k = 0; k < n; k++) {
        pm->host[first + k]  = host + ((size_t)k << PM_PAGE_SHIFT);
        pm->flags[first + k] = (uint8_t)flags;
        pm->io[first + k]    = 0;
    }
    pm->map_gen++;
}

void pm_unmap(PhysMem *pm, uint32_t base, uint32_t size)
{
    uint32_t first = base >> PM_PAGE_SHIFT, n = size >> PM_PAGE_SHIFT;
    for (uint32_t k = 0; k < n; k++) {
        pm->host[first + k] = NULL;
        pm->flags[first + k] = 0;
        pm->io[first + k] = 0;
    }
    pm->map_gen++;
}

int pm_map_io(PhysMem *pm, uint32_t base, uint32_t size, void *ctx,
              PmIoRead rd, PmIoWrite wr, const char *name)
{
    if (pm->nio >= PM_MAX_IO) return -1;
    PmIo *io = &pm->ios[pm->nio++];
    io->base = base; io->size = size; io->ctx = ctx;
    io->read = rd ? rd : default_read;
    io->write = wr ? wr : default_write;
    io->name = name;
    uint32_t first = base >> PM_PAGE_SHIFT;
    uint32_t last = (uint32_t)(((uint64_t)base + size - 1) >> PM_PAGE_SHIFT);
    for (uint32_t p = first; p <= last; p++) {
        pm->host[p] = NULL;
        pm->flags[p] = 0;
        pm->io[p] = (uint8_t)pm->nio;
    }
    pm->map_gen++;
    return 0;
}

void pm_clear_mirrors(PhysMem *pm)
{
    pm->nmirror = 0;
}

void pm_add_mirror(PhysMem *pm, uint32_t a, uint32_t b, uint32_t size)
{
    if (pm->nmirror >= PM_MAX_MIRROR) return;
    pm->mirror[pm->nmirror].a = a;
    pm->mirror[pm->nmirror].b = b;
    pm->mirror[pm->nmirror].size = size;
    pm->nmirror++;
}

static void set_code1(PhysMem *pm, uint32_t pa, int on)
{
    uint8_t *f = &pm->flags[pa >> PM_PAGE_SHIFT];
    if (on) *f |= PM_CODE; else *f &= (uint8_t)~PM_CODE;
}

void pm_set_code(PhysMem *pm, uint64_t pa, int on)
{
    pa &= pm->addr_mask;
    if (pa >> 32) return;
    uint32_t p = (uint32_t)pa;
    set_code1(pm, p, on);
    for (int k = 0; k < pm->nmirror; k++) {
        uint32_t a = pm->mirror[k].a, b = pm->mirror[k].b, n = pm->mirror[k].size;
        if (p - a < n) set_code1(pm, b + (p - a), on);
        else if (p - b < n) set_code1(pm, a + (p - b), on);
    }
}

/* il dispositivo che serve pa: con piu' dispositivi nella stessa pagina
   vince quello che contiene davvero l'indirizzo */
static const PmIo *find_io(const PhysMem *pm, uint32_t pa)
{
    int k = pm->io[pa >> PM_PAGE_SHIFT];
    if (!k) return NULL;
    const PmIo *io = &pm->ios[k - 1];
    if (pa - io->base < io->size) return io;
    for (int j = 0; j < pm->nio; j++)
        if (pa - pm->ios[j].base < pm->ios[j].size) return &pm->ios[j];
    return NULL;
}

const char *pm_io_name(const PhysMem *pm, uint64_t pa)
{
    pa &= pm->addr_mask;
    if (pa >> 32) return NULL;
    const PmIo *io = find_io(pm, (uint32_t)pa);
    return io ? io->name : NULL;
}

int pm_read(PhysMem *pm, uint64_t pa, int size, uint64_t *val)
{
    pa &= pm->addr_mask;
    if (pa >> 32) { *val = 0; return 1; }
    uint8_t *p = pm_host(pm, pa);
    if (p) {
        switch (size) {
        case 1: *val = *p; break;
        case 2: *val = be16(p); break;
        case 4: *val = be32(p); break;
        default: *val = be64(p); break;
        }
        return 0;
    }
    const PmIo *io = find_io(pm, (uint32_t)pa);
    if (io) return io->read(io->ctx, (uint32_t)pa, size, val);
    return pm->unmapped_read(pm->unmapped_ctx, (uint32_t)pa, size, val);
}

int pm_write(PhysMem *pm, uint64_t pa, int size, uint64_t val)
{
    pa &= pm->addr_mask;
    if (pa >> 32) return 1;
    uint32_t page = (uint32_t)(pa >> PM_PAGE_SHIFT);
    uint8_t *p = pm_host(pm, pa);
    if (p && (pm->flags[page] & PM_RAM)) {
        if ((pm->flags[page] & PM_CODE) && pm->code_write)
            pm->code_write(pm->code_ctx, (uint32_t)pa);
        switch (size) {
        case 1: *p = (uint8_t)val; break;
        case 2: wbe16(p, (uint16_t)val); break;
        case 4: wbe32(p, (uint32_t)val); break;
        default: wbe64(p, val); break;
        }
        return 0;
    }
    if (p)                          /* ROM: la scrittura va al gestore degli spazi vuoti */
        return pm->unmapped_write(pm->unmapped_ctx, (uint32_t)pa, size, val);
    const PmIo *io = find_io(pm, (uint32_t)pa);
    if (io) return io->write(io->ctx, (uint32_t)pa, size, val);
    return pm->unmapped_write(pm->unmapped_ctx, (uint32_t)pa, size, val);
}

uint32_t pm_dma_read(PhysMem *pm, uint64_t pa, uint8_t *dst, uint32_t len)
{
    pa &= pm->addr_mask;
    uint32_t done = 0;
    while (done < len) {
        uint8_t *h = pm_host(pm, pa + done);
        if (!h) break;
        uint32_t n = PM_PAGE_SIZE - (uint32_t)((pa + done) & PM_PAGE_MASK);
        if (n > len - done) n = len - done;
        memcpy(dst + done, h, n);
        done += n;
    }
    return done;
}

uint32_t pm_dma_write(PhysMem *pm, uint64_t pa, const uint8_t *src, uint32_t len)
{
    pa &= pm->addr_mask;
    uint32_t done = 0;
    while (done < len) {
        uint64_t a = pa + done;
        uint8_t *h = pm_host(pm, a);
        if (!h || !(pm->flags[a >> PM_PAGE_SHIFT] & PM_RAM)) break;
        uint32_t n = PM_PAGE_SIZE - (uint32_t)(a & PM_PAGE_MASK);
        if (n > len - done) n = len - done;
        if ((pm->flags[a >> PM_PAGE_SHIFT] & PM_CODE) && pm->code_write)
            pm->code_write(pm->code_ctx, (uint32_t)a);
        memcpy(h, src + done, n);
        done += n;
    }
    return done;
}
