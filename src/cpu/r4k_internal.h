/*
 * r4k_internal.h - Funzioni condivise tra i file della CPU (non API)
 */
#ifndef R4K_INTERNAL_H
#define R4K_INTERNAL_H

#include "cpu/r4k.h"

#define SX32(x) ((uint64_t)(int64_t)(int32_t)(x))

/* tipi di accesso */
enum { ACC_LOAD = 0, ACC_STORE = 1, ACC_FETCH = 2 };

/* ---- eccezioni (r4k_cp0.c) ---- */
void r4k_exception(R4k *c, int code, int ce);             /* vettore generale 0x180 */
void r4k_exception_at(R4k *c, int code, int ce, uint32_t offset);
void r4k_addr_error(R4k *c, int store, uint64_t va);     /* AdEL / AdES */
void r4k_update_mode(R4k *c);
void r4k_update_timer(R4k *c);
void r4k_timer_fire(R4k *c);
void r4k_update_ip(R4k *c);
static inline int r4k_irq_ready(const R4k *c)
{
    uint32_t st = (uint32_t)c->cp0[CP0_STATUS];
    return (st & (ST_IE | ST_EXL | ST_ERL)) == ST_IE &&
           (st & (uint32_t)c->cp0[CP0_CAUSE] & CAUSE_IP);
}
static inline void r4k_kick(R4k *c) { c->limit = 0; }   /* torna al ciclo esterno */
static inline int  r4k_cp0_usable(const R4k *c)
{
    return c->mode == R4K_MODE_KERNEL || (c->cp0[CP0_STATUS] & ST_CU0);
}

/* ---- istruzioni di coprocessore ---- */
void r4k_cop0(R4k *c, uint32_t insn);
void r4k_cache_op(R4k *c, uint32_t insn);
void r4k_cop1(R4k *c, uint32_t insn);

/* ---- memoria (r4k_mmu.c) ---- */
int r4k_read_slow (R4k *c, uint64_t va, int size, uint64_t *val);
int r4k_write_slow(R4k *c, uint64_t va, int size, uint64_t val);
int r4k_fetch_slow(R4k *c, uint64_t va, uint32_t *insn);
/* traduzione con eccezioni: 1 = ok */
int r4k_translate(R4k *c, uint64_t va, int acc, uint64_t *pa);
void r4k_fast_flush_tlb_entry(R4k *c, const R4kTlb *e);

static inline R4kFast *r4k_fast(R4k *c, uint64_t va)
{
    return &c->fast[c->mode][(va >> PM_PAGE_SHIFT) & (R4K_FAST_SIZE - 1)];
}

#define FAST_HOST(f, va) ((uint8_t *)(uintptr_t)((va) + (f)->addend))

/* Le load/store ritornano 1 se va bene, 0 se e' stata presa un'eccezione
   (e allora l'istruzione non deve scrivere niente). */
static inline int ld8(R4k *c, uint64_t va, uint64_t *v)
{
    R4kFast *f = r4k_fast(c, va);
    if (f->tag_r == (va & ~(uint64_t)0xFFF)) { *v = *FAST_HOST(f, va); return 1; }
    return r4k_read_slow(c, va, 1, v);
}
static inline int ld16(R4k *c, uint64_t va, uint64_t *v)
{
    R4kFast *f = r4k_fast(c, va);
    if (f->tag_r == (va & ~(uint64_t)0xFFE)) { *v = be16(FAST_HOST(f, va)); return 1; }
    return r4k_read_slow(c, va, 2, v);
}
static inline int ld32(R4k *c, uint64_t va, uint64_t *v)
{
    R4kFast *f = r4k_fast(c, va);
    if (f->tag_r == (va & ~(uint64_t)0xFFC)) { *v = be32(FAST_HOST(f, va)); return 1; }
    return r4k_read_slow(c, va, 4, v);
}
static inline int ld64(R4k *c, uint64_t va, uint64_t *v)
{
    R4kFast *f = r4k_fast(c, va);
    if (f->tag_r == (va & ~(uint64_t)0xFF8)) { *v = be64(FAST_HOST(f, va)); return 1; }
    return r4k_read_slow(c, va, 8, v);
}
static inline int st8(R4k *c, uint64_t va, uint64_t v)
{
    R4kFast *f = r4k_fast(c, va);
    if (f->tag_w == (va & ~(uint64_t)0xFFF)) { *FAST_HOST(f, va) = (uint8_t)v; return 1; }
    return r4k_write_slow(c, va, 1, v);
}
static inline int st16(R4k *c, uint64_t va, uint64_t v)
{
    R4kFast *f = r4k_fast(c, va);
    if (f->tag_w == (va & ~(uint64_t)0xFFE)) { wbe16(FAST_HOST(f, va), (uint16_t)v); return 1; }
    return r4k_write_slow(c, va, 2, v);
}
static inline int st32(R4k *c, uint64_t va, uint64_t v)
{
    R4kFast *f = r4k_fast(c, va);
    if (f->tag_w == (va & ~(uint64_t)0xFFC)) { wbe32(FAST_HOST(f, va), (uint32_t)v); return 1; }
    return r4k_write_slow(c, va, 4, v);
}
static inline int st64(R4k *c, uint64_t va, uint64_t v)
{
    R4kFast *f = r4k_fast(c, va);
    if (f->tag_w == (va & ~(uint64_t)0xFF8)) { wbe64(FAST_HOST(f, va), v); return 1; }
    return r4k_write_slow(c, va, 8, v);
}

/* ---- registri FPU ---- */
static inline uint32_t fpr_w(const R4k *c, int n)
{
    if (c->cp0[CP0_STATUS] & ST_FR) return (uint32_t)c->fpr[n];
    return (n & 1) ? (uint32_t)(c->fpr[n & ~1] >> 32) : (uint32_t)c->fpr[n];
}
static inline void set_fpr_w(R4k *c, int n, uint32_t v)
{
    if (!(c->cp0[CP0_STATUS] & ST_FR) && (n & 1))
        c->fpr[n & ~1] = (c->fpr[n & ~1] & 0xFFFFFFFFu) | ((uint64_t)v << 32);
    else
        c->fpr[n] = (c->fpr[n] & ~(uint64_t)0xFFFFFFFFu) | v;
}
static inline uint64_t fpr_l(const R4k *c, int n)
{
    return (c->cp0[CP0_STATUS] & ST_FR) ? c->fpr[n] : c->fpr[n & ~1];
}
static inline void set_fpr_l(R4k *c, int n, uint64_t v)
{
    if (c->cp0[CP0_STATUS] & ST_FR) c->fpr[n] = v; else c->fpr[n & ~1] = v;
}

#endif
