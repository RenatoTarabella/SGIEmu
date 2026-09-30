/*
 * r4k_mmu.c - Traduzione degli indirizzi, TLB e MMU veloce
 *
 * Segmenti (in 64 bit; in 32 bit gli indirizzi sono estesi col segno e
 * cadono nei segmenti compatibili: kuseg, kseg0/1, ksseg, kseg3):
 *
 *   0x0000000000000000  xkuseg  mappato (in 32 bit: kuseg fino a 0x7FFFFFFF)
 *   0x4000000000000000  xksseg  mappato, supervisor/kernel
 *   0x8000000000000000  xkphys  non mappato, fisico = bit 35:0
 *   0xC000000000000000  xkseg   mappato, kernel
 *   0xFFFFFFFF80000000  ckseg0  non mappato, fisico = va & 0x1FFFFFFF (cached)
 *   0xFFFFFFFFA0000000  ckseg1  idem, uncached
 *   0xFFFFFFFFC0000000  cksseg  mappato, supervisor/kernel
 *   0xFFFFFFFFE0000000  ckseg3  mappato, kernel
 */
#include "cpu/r4k_internal.h"

enum { XL_OK = 0, XL_ADDR, XL_MISS, XL_INVALID, XL_MOD };

#define VPN2_MASK   0xC00000FFFFFFE000ull     /* R (63:62) e VPN2 (39:13) */
#define SEG_LIMIT   (1ull << 40)               /* 40 bit virtuali          */
#define PA_MASK     0x0000000FFFFFFFFFull      /* 36 bit fisici            */

static int xmode(const R4k *c)                 /* indirizzi a 64 bit nel modo corrente */
{
    uint32_t st = (uint32_t)c->cp0[CP0_STATUS];
    switch (c->mode) {
    case R4K_MODE_KERNEL: return (st & ST_KX) != 0;
    case R4K_MODE_SUPER:  return (st & ST_SX) != 0;
    default:              return (st & ST_UX) != 0;
    }
}

static int tlb_map(R4k *c, uint64_t va, int store, uint64_t *pa, int *wr)
{
    uint8_t asid = (uint8_t)c->cp0[CP0_ENTRYHI];
    for (int i = 0; i < R4K_TLB_ENTRIES; i++) {
        const R4kTlb *e = &c->tlb[i];
        if ((va & e->cmp) != e->vpn || !(e->g || e->asid == asid)) continue;
        uint64_t lo = (va & e->odd) ? e->lo[1] : e->lo[0];
        if (!(lo & 2)) return XL_INVALID;
        if (store && !(lo & 4)) return XL_MOD;
        *wr = (int)(lo >> 2) & 1;
        uint64_t off = e->odd - 1;
        *pa = ((((lo >> 6) & 0xFFFFFF) << 12) & ~off) | (va & off);
        return XL_OK;
    }
    return XL_MISS;
}

static int xlate_core(R4k *c, uint64_t va, int store, uint64_t *pa, int *wr)
{
    uint32_t st = (uint32_t)c->cp0[CP0_STATUS];
    int kernel = c->mode == R4K_MODE_KERNEL;
    int super  = c->mode == R4K_MODE_SUPER;

    if (va <= 0x7FFFFFFFull) {                          /* kuseg */
        if (kernel && (st & ST_ERL)) { *pa = va; *wr = 1; return XL_OK; }
        return tlb_map(c, va, store, pa, wr);
    }
    if (va < 0x4000000000000000ull) {                   /* xkuseg */
        if (!xmode(c) || va >= SEG_LIMIT) return XL_ADDR;
        return tlb_map(c, va, store, pa, wr);
    }
    if (va < 0x8000000000000000ull) {                   /* xksseg */
        if (!(kernel || super) || !xmode(c) || va - 0x4000000000000000ull >= SEG_LIMIT)
            return XL_ADDR;
        return tlb_map(c, va, store, pa, wr);
    }
    if (va < 0xC000000000000000ull) {                   /* xkphys */
        if (!kernel || !(st & ST_KX) || (va & 0x07FFFFFFFFFFFFFFull) > PA_MASK)
            return XL_ADDR;
        *pa = va & PA_MASK; *wr = 1;
        return XL_OK;
    }
    if (va < 0xFFFFFFFF80000000ull) {                   /* xkseg */
        if (!kernel || !(st & ST_KX) || va - 0xC000000000000000ull >= SEG_LIMIT - 0x80000000ull)
            return XL_ADDR;
        return tlb_map(c, va, store, pa, wr);
    }
    if (va < 0xFFFFFFFFC0000000ull) {                   /* kseg0, kseg1 */
        if (!kernel) return XL_ADDR;
        *pa = va & 0x1FFFFFFFull; *wr = 1;
        return XL_OK;
    }
    if (va < 0xFFFFFFFFE0000000ull) {                   /* ksseg */
        if (!(kernel || super)) return XL_ADDR;
        return tlb_map(c, va, store, pa, wr);
    }
    if (!kernel) return XL_ADDR;                        /* kseg3 */
    return tlb_map(c, va, store, pa, wr);
}

void r4k_addr_error(R4k *c, int store, uint64_t va)
{
    c->cp0[CP0_BADVADDR] = va;
    r4k_exception(c, store ? EXC_ADES : EXC_ADEL, 0);
}

static void tlb_exception(R4k *c, int code, uint64_t va, int refill)
{
    c->cp0[CP0_BADVADDR] = va;
    c->cp0[CP0_CONTEXT] = (c->cp0[CP0_CONTEXT] & ~0x7FFFF0ull) | ((va >> 9) & 0x7FFFF0ull);
    c->cp0[CP0_XCONTEXT] = (c->cp0[CP0_XCONTEXT] & ~0x1FFFFFFF0ull)
                         | ((va >> 62) << 31) | ((va >> 9) & 0x7FFFFFF0ull);
    c->cp0[CP0_ENTRYHI] = (va & VPN2_MASK) | (c->cp0[CP0_ENTRYHI] & 0xFF);
    uint32_t off = 0x180;
    if (refill && !(c->cp0[CP0_STATUS] & ST_EXL)) off = xmode(c) ? 0x080 : 0x000;
    r4k_exception_at(c, code, 0, off);
}

static void raise_xlate(R4k *c, int err, uint64_t va, int acc)
{
    int store = acc == ACC_STORE;
    switch (err) {
    case XL_ADDR:    r4k_addr_error(c, store, va); break;
    case XL_MISS:    tlb_exception(c, store ? EXC_TLBS : EXC_TLBL, va, 1); break;
    case XL_INVALID: tlb_exception(c, store ? EXC_TLBS : EXC_TLBL, va, 0); break;
    default:         tlb_exception(c, EXC_MOD, va, 0); break;
    }
}

int r4k_translate(R4k *c, uint64_t va, int acc, uint64_t *pa)
{
    int wr, e = xlate_core(c, va, acc == ACC_STORE, pa, &wr);
    if (e) { raise_xlate(c, e, va, acc); return 0; }
    return 1;
}

int r4k_translate_debug(R4k *c, uint64_t va, int store, uint64_t *pa)
{
    int wr;
    return xlate_core(c, va, store, pa, &wr) == XL_OK;
}

/* ---- MMU veloce ---- */

static void fast_fill(R4k *c, uint64_t va, uint64_t pa, int wr)
{
    if (pa >> 32) return;
    uint32_t page = (uint32_t)(pa >> PM_PAGE_SHIFT);
    uint8_t *h = c->pm->host[page];
    if (!h) return;
    uint8_t fl = c->pm->flags[page];
    uint64_t vpage = va & ~(uint64_t)PM_PAGE_MASK;
    R4kFast *f = r4k_fast(c, va);
    f->tag_r = f->tag_x = vpage;
    f->tag_w = (wr && (fl & PM_RAM) && !(fl & PM_CODE)) ? vpage : R4K_FAST_INVALID;
    f->addend = (uintptr_t)h - (uintptr_t)vpage;
}

void r4k_fast_flush(R4k *c)
{
    c->flushes++;
    memset(c->fast, 0xFF, sizeof c->fast);
}

void r4k_fast_flush_tlb_entry(R4k *c, const R4kTlb *e)
{
    uint64_t npages = (e->odd * 2) >> PM_PAGE_SHIFT;
    if (npages >= R4K_FAST_SIZE) { r4k_fast_flush(c); return; }
    for (uint64_t p = 0; p < npages; p++) {
        uint32_t slot = (uint32_t)((e->vpn >> PM_PAGE_SHIFT) + p) & (R4K_FAST_SIZE - 1);
        for (int m = 0; m < 3; m++) {
            R4kFast *f = &c->fast[m][slot];
            if (f->tag_r != R4K_FAST_INVALID && (f->tag_r & e->cmp) == e->vpn)
                f->tag_r = f->tag_w = f->tag_x = R4K_FAST_INVALID;
        }
    }
}

void r4k_fast_protect(R4k *c, uintptr_t host_page)
{
    for (int m = 0; m < 3; m++)
        for (uint32_t i = 0; i < R4K_FAST_SIZE; i++) {
            R4kFast *f = &c->fast[m][i];
            if (f->tag_w != R4K_FAST_INVALID && (uintptr_t)f->tag_w + f->addend == host_page)
                f->tag_w = R4K_FAST_INVALID;
        }
}

/* ---- vie lente ---- */

int r4k_read_slow(R4k *c, uint64_t va, int size, uint64_t *val)
{
    if (va & (uint64_t)(size - 1)) { r4k_addr_error(c, 0, va); return 0; }
    uint64_t pa; int wr;
    int e = xlate_core(c, va, 0, &pa, &wr);
    if (e) { raise_xlate(c, e, va, ACC_LOAD); return 0; }
    uint8_t *h = pm_host(c->pm, pa);
    if (h) {
        fast_fill(c, va, pa, wr);
        switch (size) {
        case 1: *val = *h; break;
        case 2: *val = be16(h); break;
        case 4: *val = be32(h); break;
        default: *val = be64(h); break;
        }
        return 1;
    }
    if (pm_read(c->pm, pa, size, val)) { r4k_exception(c, EXC_DBE, 0); return 0; }
    return 1;
}

int r4k_write_slow(R4k *c, uint64_t va, int size, uint64_t val)
{
    if (va & (uint64_t)(size - 1)) { r4k_addr_error(c, 1, va); return 0; }
    uint64_t pa; int wr;
    int e = xlate_core(c, va, 1, &pa, &wr);
    if (e) { raise_xlate(c, e, va, ACC_STORE); return 0; }
    fast_fill(c, va, pa, wr);
    if (pm_write(c->pm, pa, size, val)) { r4k_exception(c, EXC_DBE, 0); return 0; }
    return 1;
}

int r4k_fetch_slow(R4k *c, uint64_t va, uint32_t *insn)
{
    if (va & 3) { r4k_addr_error(c, 0, va); return 0; }
    uint64_t pa; int wr;
    int e = xlate_core(c, va, 0, &pa, &wr);
    if (e) { raise_xlate(c, e, va, ACC_FETCH); return 0; }
    uint8_t *h = pm_host(c->pm, pa);
    if (h) { fast_fill(c, va, pa, wr); *insn = be32(h); return 1; }
    uint64_t v;
    if (pm_read(c->pm, pa, 4, &v)) { r4k_exception(c, EXC_IBE, 0); return 0; }
    *insn = (uint32_t)v;
    return 1;
}

int r4k_fetch_fill(R4k *c, uint64_t va)
{
    uint64_t pa; int wr;
    if (va & 3 || xlate_core(c, va, 0, &pa, &wr)) return 0;
    fast_fill(c, va, pa, wr);
    return 1;
}

int r4k_fast_prefill(R4k *c, uint64_t va)
{
    return r4k_fetch_fill(c, va & ~(uint64_t)3);
}

int r4k_peek(R4k *c, uint64_t va, int size, uint64_t *val)
{
    uint64_t pa;
    if (!r4k_translate_debug(c, va, 0, &pa)) return 0;
    return pm_read(c->pm, pa, size, val) == 0;
}
