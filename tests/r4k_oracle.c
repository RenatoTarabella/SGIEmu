/*
 * r4k_oracle.c - Esegue piccole sequenze di istruzioni lette da stdin e
 * stampa lo stato risultante, per il confronto con Unicorn
 * (tests/diff_unicorn.py). Tutti i numeri in esadecimale.
 *
 * Riga in ingresso:
 *   addr status fcsr hi lo  ncode w0 w1 ...  ngpr (i v)...  nfpr (i v)...  mem
 * dove mem sono i 64 byte all'indirizzo DATA (128 cifre). I registri FPU
 * sono a 64 bit con Status.FR = 1, a 32 bit (vista del programma) con FR = 0.
 *
 * Riga in uscita:
 *   exc hi lo fcsr r0..r31 f0..f31 mem
 * con exc = codice dell'eccezione presa, -1 se nessuna.
 *
 * Con R4K_JIT=N nell'ambiente il codice gira con il JIT (blocchi di al
 * piu' N istruzioni) e r4k_run: dopo la fine del codice ci sono NOP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/r4k.h"

#define RAM_SIZE (8u << 20)
#define DATA     0x80000u
#define MAXSTEP  32

static int exc_taken;

static void on_exc(R4k *c, int code, void *user)
{
    (void)user;
    exc_taken = code;
    r4k_stop(c);
}

static uint64_t next(char **p)
{
    return strtoull(*p, p, 16);
}

int main(void)
{
    static PhysMem pm;
    static R4k cpu;
    uint8_t *ram = calloc(1, RAM_SIZE);
    if (!ram || pm_init(&pm) || r4k_init(&cpu, &pm)) return 1;
    pm_map_ram(&pm, 0, RAM_SIZE, ram, PM_RAM);
    cpu.exc_hook = on_exc;
    const char *jv = getenv("R4K_JIT");
    int jit = jv != NULL;
    if (jit && r4k_jit_init(&cpu, atoi(jv))) return 1;

    static char line[1 << 16];
    while (fgets(line, sizeof line, stdin)) {
        char *p = line;
        uint64_t addr = next(&p), st = next(&p), fcsr = next(&p), hi = next(&p), lo = next(&p);
        R4k *c = &cpu;
        r4k_reset(c);
        memset(c->fpr, 0, sizeof c->fpr);          /* il reset non li tocca */
        r4k_cp0_write(c, CP0_STATUS, st);
        c->fcr31 = (uint32_t)fcsr;
        c->hi = hi; c->lo = lo;
        int ncode = (int)next(&p);
        if (addr < 0x100000 || addr + 4 * (uint64_t)ncode > RAM_SIZE || ncode > 64) return 1;
        for (int k = 0; k < ncode; k++) wbe32(ram + addr + 4 * k, (uint32_t)next(&p));
        r4k_jit_flush(c);
        int n = (int)next(&p);
        for (int k = 0; k < n; k++) { int i = (int)next(&p); c->r[i] = next(&p); }
        c->r[0] = 0;
        n = (int)next(&p);
        int fr = (st & ST_FR) != 0;
        for (int k = 0; k < n; k++) {
            int i = (int)next(&p);
            uint64_t v = next(&p);
            if (fr) r4k_set_fpr_l(c, i, v); else r4k_set_fpr_w(c, i, (uint32_t)v);
        }
        while (*p == ' ') p++;
        for (int k = 0; k < 64; k++) {
            char h[3] = { p[2 * k], p[2 * k + 1], 0 };
            ram[DATA + k] = (uint8_t)strtoul(h, NULL, 16);
        }

        c->pc = addr;
        exc_taken = -1;
        uint64_t end = addr + 4 * (uint64_t)ncode;
        if (jit) {
            /* dopo il codice solo NOP (la RAM e' vuota): lo stato finale e' lo stesso */
            for (int k = 0; k < 64; k++) wbe32(ram + end + 4 * k, 0);
            r4k_fast_prefill(c, DATA);         /* i dati per la via veloce del JIT */
            r4k_run(c, MAXSTEP);
        } else {
            for (int s = 0; s < MAXSTEP && exc_taken < 0 && c->pc != end; s++) r4k_step(c);
        }

        printf("%d %llx %llx %x", exc_taken, (unsigned long long)c->hi,
               (unsigned long long)c->lo, c->fcr31);
        for (int i = 0; i < 32; i++) printf(" %llx", (unsigned long long)c->r[i]);
        for (int i = 0; i < 32; i++)
            printf(" %llx", fr ? (unsigned long long)r4k_fpr_l(c, i) : (unsigned long long)r4k_fpr_w(c, i));
        printf(" ");
        for (int k = 0; k < 64; k++) printf("%02x", ram[DATA + k]);
        printf("\n");
    }
    return 0;
}
