/*
 * indy_boot.c - Avvio dell'Indy da riga di comando, per seguire il boot
 * della PROM senza finestra.
 *
 *   indy_boot --prom roms/ip24prom.bin [--ram 64] [--n 20000000]
 *             [--trace 200] [--trace-at ADDR] [--io 100] [--exc 50] [--buserr]
 *
 * Per ora c'e' solo la CPU: PROM a 0x1FC00000, RAM a 0x08000000 (e i primi
 * 512 KB ripetuti a 0, come fa il controller di memoria). Ogni accesso a un
 * dispositivo che non esiste ancora viene contato e, i primi --io, stampato:
 * e' la lista dei registri da emulare. Alla fine un riassunto: dove e'
 * arrivata la CPU, le eccezioni, le pagine di I/O piu' usate.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cpu/r4k.h"
#include "cpu/r4k_disasm.h"

#define PROM_BASE 0x1FC00000u
#define RAM_BASE  0x08000000u

static R4k cpu;
static PhysMem pm;
static long io_print_left = 100, exc_print_left = 50;
static int buserr;

/* ---- accessi a indirizzi fisici senza dispositivo ---- */
typedef struct IoPage { uint32_t page; uint64_t reads, writes; } IoPage;
static IoPage io_pages[4096];
static int n_io_pages;

static void count_page(uint32_t pa, int write)
{
    uint32_t page = pa >> 12;
    for (int k = 0; k < n_io_pages; k++)
        if (io_pages[k].page == page) { if (write) io_pages[k].writes++; else io_pages[k].reads++; return; }
    if (n_io_pages < 4096) {
        io_pages[n_io_pages].page = page;
        io_pages[n_io_pages].reads = !write;
        io_pages[n_io_pages].writes = write;
        n_io_pages++;
    }
}

static int unmapped_read(void *ctx, uint32_t pa, int size, uint64_t *val)
{
    (void)ctx;
    count_page(pa, 0);
    if (io_print_left > 0) {
        io_print_left--;
        printf("  io  R%d %08X            pc=%016llX\n", size * 8, pa, (unsigned long long)cpu.pc);
    }
    *val = 0;
    return buserr;
}

static int unmapped_write(void *ctx, uint32_t pa, int size, uint64_t val)
{
    (void)ctx;
    count_page(pa, 1);
    if (io_print_left > 0) {
        io_print_left--;
        printf("  io  W%d %08X = %0*llX  pc=%016llX\n", size * 8, pa, size * 2,
               (unsigned long long)val, (unsigned long long)cpu.pc);
    }
    return 0;
}

static void on_exc(R4k *c, int code, void *user)
{
    (void)user;
    if (exc_print_left > 0) {
        exc_print_left--;
        printf("  exc %-5s EPC=%016llX BadVAddr=%016llX Status=%08llX\n", r4k_exc_names[code],
               (unsigned long long)c->cp0[CP0_EPC], (unsigned long long)c->cp0[CP0_BADVADDR],
               (unsigned long long)(uint32_t)c->cp0[CP0_STATUS]);
    }
}

static void trace_one(R4k *c)
{
    uint64_t insn;
    char buf[80];
    if (r4k_peek(c, c->pc, 4, &insn)) r4k_disasm(c->pc, (uint32_t)insn, buf, sizeof buf);
    else snprintf(buf, sizeof buf, "(non leggibile)");
    printf("  %016llX  %08X  %s\n", (unsigned long long)c->pc, (uint32_t)insn, buf);
}

static int cmp_pages(const void *a, const void *b)
{
    const IoPage *x = a, *y = b;
    uint64_t nx = x->reads + x->writes, ny = y->reads + y->writes;
    return nx < ny ? 1 : nx > ny ? -1 : 0;
}

static void usage(void)
{
    printf("uso: indy_boot --prom FILE [--ram MB] [--n ISTRUZIONI] [--trace N] [--trace-at ADDR]\n"
           "                [--io N] [--exc N] [--buserr]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *prom_path = NULL;
    unsigned ram_mb = 64;
    uint64_t n = 20000000, trace_at = 0;
    long trace = 0;
    for (int k = 1; k < argc; k++) {
        const char *a = argv[k];
        const char *v = k + 1 < argc ? argv[k + 1] : NULL;
        if (!strcmp(a, "--prom") && v) { prom_path = v; k++; }
        else if (!strcmp(a, "--ram") && v) { ram_mb = (unsigned)atoi(v); k++; }
        else if (!strcmp(a, "--n") && v) { n = strtoull(v, NULL, 0); k++; }
        else if (!strcmp(a, "--trace") && v) { trace = atol(v); k++; }
        else if (!strcmp(a, "--trace-at") && v) { trace_at = strtoull(v, NULL, 16); k++; }
        else if (!strcmp(a, "--io") && v) { io_print_left = atol(v); k++; }
        else if (!strcmp(a, "--exc") && v) { exc_print_left = atol(v); k++; }
        else if (!strcmp(a, "--buserr")) buserr = 1;
        else usage();
    }
    if (!prom_path) usage();

    FILE *f = fopen(prom_path, "rb");
    if (!f) { printf("non trovo la PROM %s\n", prom_path); return 1; }
    fseek(f, 0, SEEK_END);
    long prom_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t prom_map = ((uint32_t)prom_size + 0xFFF) & ~0xFFFu;
    uint8_t *prom = calloc(1, prom_map ? prom_map : 4096);
    if (!prom || fread(prom, 1, (size_t)prom_size, f) != (size_t)prom_size) { printf("lettura PROM\n"); return 1; }
    fclose(f);
    printf("PROM %s: %ld byte\n", prom_path, prom_size);

    uint32_t ram_size = ram_mb << 20;
    uint8_t *ram = calloc(1, ram_size);
    if (!ram || pm_init(&pm) || r4k_init(&cpu, &pm)) { printf("memoria\n"); return 1; }
    pm_map_ram(&pm, PROM_BASE, prom_map, prom, PM_ROM);
    pm_map_ram(&pm, RAM_BASE, ram_size, ram, PM_RAM);
    pm_map_ram(&pm, 0, 512 * 1024, ram, PM_RAM);
    pm.unmapped_read = unmapped_read;
    pm.unmapped_write = unmapped_write;
    cpu.exc_hook = on_exc;
    r4k_reset(&cpu);

    uint64_t done = 0;
    while (done < n) {
        if (trace > 0 && (!trace_at || cpu.pc == trace_at)) {
            trace_at = 0;
            trace_one(&cpu);
            r4k_step(&cpu);
            trace--;
            done++;
            continue;
        }
        uint64_t chunk = n - done < 100000 ? n - done : 100000;
        if (trace > 0) {                   /* in attesa di trace_at: a passi singoli */
            for (uint64_t k = 0; k < chunk && cpu.pc != trace_at; k++, done++) r4k_step(&cpu);
            continue;
        }
        done += r4k_run(&cpu, chunk);
    }

    printf("\n%llu istruzioni, pc = %016llX\n", (unsigned long long)cpu.icount,
           (unsigned long long)cpu.pc);
    trace_one(&cpu);
    printf("Status=%08llX Cause=%08llX EPC=%016llX BadVAddr=%016llX\n",
           (unsigned long long)(uint32_t)cpu.cp0[CP0_STATUS], (unsigned long long)(uint32_t)cpu.cp0[CP0_CAUSE],
           (unsigned long long)cpu.cp0[CP0_EPC], (unsigned long long)cpu.cp0[CP0_BADVADDR]);
    for (int i = 0; i < 32; i += 4)
        printf("  %-4s %016llX  %-4s %016llX  %-4s %016llX  %-4s %016llX\n",
               r4k_reg_names[i], (unsigned long long)cpu.r[i], r4k_reg_names[i + 1], (unsigned long long)cpu.r[i + 1],
               r4k_reg_names[i + 2], (unsigned long long)cpu.r[i + 2], r4k_reg_names[i + 3], (unsigned long long)cpu.r[i + 3]);
    printf("eccezioni:");
    for (int k = 0; k < 32; k++)
        if (cpu.exc_count[k]) printf(" %s=%llu", r4k_exc_names[k], (unsigned long long)cpu.exc_count[k]);
    printf("\npagine di I/O senza dispositivo (%d):\n", n_io_pages);
    qsort(io_pages, (size_t)n_io_pages, sizeof io_pages[0], cmp_pages);
    for (int k = 0; k < n_io_pages && k < 20; k++)
        printf("  %08X  letture %-10llu scritture %llu\n", io_pages[k].page << 12,
               (unsigned long long)io_pages[k].reads, (unsigned long long)io_pages[k].writes);
    return 0;
}
