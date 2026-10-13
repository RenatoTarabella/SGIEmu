/*
 * octane_boot.c - Avvio dell'Octane (IP30) da riga di comando, per seguire
 * la PROM senza finestra.
 *
 *   octane_boot --prom roms/ip30prom.rev4.9.bin [--ram 256] [--ms 5000]
 *               [--keys "testo{CR}"] [--trace N] [--trace-at ADDR]
 *               [--io N] [--exc N] [--dis ADDR[,N]] [--jit] [--mips N]
 *
 * La console seriale (porta A dell'IOC3) va su stdout. --keys la scrive
 * dopo un secondo emulato, un carattere ogni 30 ms; {CR} e' l'invio, {WAIT}
 * aspetta mezzo secondo, {T=190} fino al secondo 190. Alla fine dove e' la
 * CPU, le eccezioni e gli accessi a dispositivi che non ci sono ancora.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "octane/octane.h"
#include "cpu/r4k_disasm.h"

static Octane M;
static long exc_print_left = 20;
static int col0 = 1;

/* ---- accessi senza dispositivo, per blocchi da 256 byte ---- */
typedef struct IoPage { uint64_t page; uint64_t reads, writes; const char *what; } IoPage;
static IoPage io_pages[4096];
static int n_io_pages;
static long io_print_left = 50;
static int trace_wait;                                 /* --trace: il log aspetta l'inizio della traccia */
static uint64_t io_from;                                /* --io-from: il log parte da quest'istruzione */

static void newline(void) { if (!col0) { printf("\n"); col0 = 1; } }

static void io_log(Octane *m, int write, uint64_t pa, int size, uint64_t val, const char *what)
{
    uint64_t page = pa >> 8;
    int k = 0;
    while (k < n_io_pages && io_pages[k].page != page) k++;
    if (k == n_io_pages && n_io_pages < 4096) { io_pages[k].page = page; io_pages[k].what = what; n_io_pages++; }
    if (k < 4096) { if (write) io_pages[k].writes++; else io_pages[k].reads++; }
    if (io_print_left > 0 && !trace_wait && m->cpu.icount >= io_from) {
        io_print_left--;
        newline();
        if (write) printf("  io  W%d %010llX = %llX  %s pc=%016llX i=%llu\n", size * 8, (unsigned long long)pa,
                          (unsigned long long)val, what ? what : "-", (unsigned long long)m->cpu.pc,
                          (unsigned long long)m->cpu.icount);
        else printf("  io  R%d %010llX  %s pc=%016llX i=%llu\n", size * 8, (unsigned long long)pa,
                    what ? what : "-", (unsigned long long)m->cpu.pc, (unsigned long long)m->cpu.icount);
    }
}

static void on_exc(R4k *c, int code, void *user)
{
    (void)user;
    if (code == EXC_INT || exc_print_left <= 0) return;
    exc_print_left--;
    newline();
    printf("  exc %-5s EPC=%016llX BadVAddr=%016llX\n", r4k_exc_names[code],
           (unsigned long long)c->cp0[CP0_EPC], (unsigned long long)c->cp0[CP0_BADVADDR]);
}

static int stamp;                                      /* --stamp: istruzione a inizio riga */

static void on_tx(Octane *m, int port, uint8_t ch)
{
    if (port) return;                                  /* la porta B non e' la console */
    if (ch == '\r') return;
    if (stamp && col0 && ch != '\n') printf("[%llu] ", (unsigned long long)m->cpu.icount);
    if (ch >= 32 || ch == '\n' || ch == '\t' || ch == 8) putchar(ch);
    else printf("<%02X>", ch);
    col0 = ch == '\n';
    fflush(stdout);
}

static void trace_one(R4k *c)
{
    uint64_t insn;
    char buf[96];
    if (r4k_peek(c, c->pc, 4, &insn)) r4k_disasm(c->pc, (uint32_t)insn, buf, sizeof buf);
    else { insn = 0; snprintf(buf, sizeof buf, "(non leggibile)"); }
    printf("  %016llX  %08X  %s\n", (unsigned long long)c->pc, (uint32_t)insn, buf);
}

static int cmp_pages(const void *a, const void *b)
{
    const IoPage *x = a, *y = b;
    uint64_t nx = x->reads + x->writes, ny = y->reads + y->writes;
    return nx < ny ? 1 : nx > ny ? -1 : 0;
}

static uint8_t *load_file(const char *path, long *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc((size_t)*size + 1);
    if (p && fread(p, 1, (size_t)*size, f) != (size_t)*size) { free(p); p = NULL; }
    fclose(f);
    return p;
}

static void usage(void)
{
    printf("uso: octane_boot --prom FILE [--ram MB] [--ms MS] [--keys TESTO] [--trace N]\n"
           "                 [--trace-at ADDR] [--io N] [--exc N] [--dis ADDR[,N]] [--jit] [--mips N]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *prom_path = NULL, *keys = "";
    unsigned ram_mb = 256, mips = 0;
    uint64_t ms = 5000, trace_at = 0, trace_from = 0, dis_addr = 0;
    int dis_count = 48, jit = 0, heart = 0, ioc3 = 0, scsi = 0, ndisk = 0, snapshot = 0;
    const char *disk_path[4] = { 0 }, *cdrom_path = NULL;
    long trace = 0;
    for (int k = 1; k < argc; k++) {
        const char *a = argv[k], *v = k + 1 < argc ? argv[k + 1] : NULL;
        if (!strcmp(a, "--prom") && v) { prom_path = v; k++; }
        else if (!strcmp(a, "--ram") && v) { ram_mb = (unsigned)atoi(v); k++; }
        else if (!strcmp(a, "--mips") && v) { mips = (unsigned)atoi(v); k++; }
        else if (!strcmp(a, "--ms") && v) { ms = strtoull(v, NULL, 0); k++; }
        else if (!strcmp(a, "--keys") && v) { keys = v; k++; }
        else if (!strcmp(a, "--trace") && v) { trace = atol(v); k++; }
        else if (!strcmp(a, "--trace-at") && v) { trace_at = strtoull(v, NULL, 16); k++; }
        else if (!strcmp(a, "--trace-from") && v) { trace_from = strtoull(v, NULL, 0); k++; }
        else if (!strcmp(a, "--stamp")) stamp = 1;
        else if (!strcmp(a, "--io-from") && v) { io_from = strtoull(v, NULL, 0); k++; }
        else if (!strcmp(a, "--io") && v) { io_print_left = atol(v); k++; }
        else if (!strcmp(a, "--exc") && v) { exc_print_left = atol(v); k++; }
        else if (!strcmp(a, "--dis") && v) {
            char *e;
            dis_addr = strtoull(v, &e, 16);
            if (*e == ',') dis_count = atoi(e + 1);
            k++;
        }
        else if (!strcmp(a, "--jit")) jit = 1;
        else if (!strcmp(a, "--heart")) heart = 1;
        else if (!strcmp(a, "--ioc3")) ioc3 = 1;
        else if (!strcmp(a, "--scsi")) scsi = 1;
        else if (!strcmp(a, "--snapshot")) snapshot = 1;
        else if (!strcmp(a, "--disk") && v) { disk_path[ndisk++ & 3] = v; k++; }
        else if (!strcmp(a, "--cdrom") && v) { cdrom_path = v; k++; }
        else usage();
    }
    if (!prom_path) usage();

    long prom_size;
    uint8_t *prom = load_file(prom_path, &prom_size);
    if (!prom) { printf("non trovo la PROM %s\n", prom_path); return 1; }
    if (oct_init(&M, prom, (uint32_t)prom_size, ram_mb)) { printf("memoria\n"); return 1; }
    if (jit && r4k_jit_init(&M.cpu, 0)) { printf("JIT: memoria\n"); return 1; }
    M.cpu.exc_hook = on_exc;
    M.tx = on_tx;
    M.io_log = io_log;
    M.heart_log = heart;
    M.ioc3_log = ioc3;
    M.scsi_log = scsi;
    /* dischi sul bus interno 0 dall'ID 1 (i vani sono 1-3, dksc(0,1) e' il disco di
       sistema), CD-ROM sul bus esterno 1 all'ID 6 (la PROM cerca solo li' gli altri ID) */
    for (int k = 0; k < ndisk && k < 3; k++) {
        if (oct_attach_scsi(&M, 0, 1 + k, SCSI_DISK, disk_path[k], snapshot ? SCSI_SNAPSHOT : 0)) { printf("non riesco ad aprire %s\n", disk_path[k]); return 1; }
        printf("SCSI 0 ID %d: disco %s (%llu MB)\n", 1 + k, disk_path[k], (unsigned long long)(M.disk[0][1 + k].blocks >> 11));
    }
    if (cdrom_path) {
        if (oct_attach_scsi(&M, 1, 6, SCSI_CDROM, cdrom_path, 1)) { printf("non riesco ad aprire %s\n", cdrom_path); return 1; }
        printf("SCSI 1 ID 6: CD-ROM %s\n", cdrom_path);
    }
    if (mips) M.ps_per_insn = 1000000u / mips;
    printf("PROM %s: %ld byte, RAM %u MB\n", prom_path, prom_size, ram_mb);

    trace_wait = trace > 0;
    const uint64_t per_ms = oct_insn_per_ms(&M);
    uint64_t total = ms * per_ms, done = 0, next_key = 1000 * per_ms;
    const char *kp = keys;
    clock_t t_start = clock();
    while (done < total && !M.power_off) {
        if (trace > 0 && (!trace_at || (uint32_t)M.cpu.pc == (uint32_t)trace_at) && M.cpu.icount >= trace_from) {
            trace_at = 0;
            trace_wait = 0;
            trace_one(&M.cpu);
            done += oct_run(&M, 1);
            trace--;
            continue;
        }
        if (*kp && M.cpu.icount >= next_key) {
            if (!strncmp(kp, "{CR}", 4)) { oct_serial_rx(&M, 0, 13); kp += 4; next_key += 30 * per_ms; }
            else if (!strncmp(kp, "{WAIT}", 6)) { kp += 6; next_key += 500 * per_ms; }
            else if (!strncmp(kp, "{T=", 3)) {
                next_key = strtoull(kp + 3, NULL, 10) * 1000 * per_ms;
                kp = strchr(kp, '}') ? strchr(kp, '}') + 1 : kp + 3;
            }
            else { oct_serial_rx(&M, 0, (uint8_t)*kp++); next_key += 30 * per_ms; }
        }
        uint64_t chunk = trace > 0 && trace_at && M.cpu.icount >= trace_from ? 1 : per_ms;
        if (trace > 0 && M.cpu.icount < trace_from && trace_from - M.cpu.icount < chunk) chunk = trace_from - M.cpu.icount;
        if (chunk > total - done) chunk = total - done;
        done += oct_run(&M, chunk);
    }

    newline();
    double real_s = (double)(clock() - t_start) / CLOCKS_PER_SEC;
    printf("%.3f s emulati, %llu istruzioni, %.2f s reali%s\n", done / (double)per_ms / 1000,
           (unsigned long long)M.cpu.icount, real_s, M.power_off ? " (spenta)" : "");
    R4k *c = &M.cpu;
    trace_one(c);
    printf("Status=%08llX Cause=%08llX EPC=%016llX BadVAddr=%016llX ErrorEPC=%016llX\n",
           (unsigned long long)(uint32_t)c->cp0[CP0_STATUS], (unsigned long long)(uint32_t)c->cp0[CP0_CAUSE],
           (unsigned long long)c->cp0[CP0_EPC], (unsigned long long)c->cp0[CP0_BADVADDR],
           (unsigned long long)c->cp0[CP0_ERROREPC]);
    for (int i = 0; i < 32; i += 4)
        printf("  %-4s %016llX  %-4s %016llX  %-4s %016llX  %-4s %016llX\n",
               r4k_reg_names[i], (unsigned long long)c->r[i], r4k_reg_names[i + 1], (unsigned long long)c->r[i + 1],
               r4k_reg_names[i + 2], (unsigned long long)c->r[i + 2], r4k_reg_names[i + 3], (unsigned long long)c->r[i + 3]);
    if (dis_addr) {
        uint64_t a = dis_addr >> 32 ? dis_addr : (uint64_t)(int64_t)(int32_t)dis_addr;
        for (int j = 0; j < dis_count; j++, a += 4) {
            uint64_t w; char b[96];
            if (!r4k_peek(c, a, 4, &w)) break;
            r4k_disasm(a, (uint32_t)w, b, sizeof b);
            printf("  %016llX  %08X  %s\n", (unsigned long long)a, (uint32_t)w, b);
        }
    }
    printf("BRIDGE device:");
    for (int k = 0; k < 8; k++) printf(" %08X", (uint32_t)M.bridge[(0x200 >> 3) + k]);
    printf("\nHEART MEMCFG: %016llX %016llX  modo %016llX\n", (unsigned long long)M.heart.regs[4],
           (unsigned long long)M.heart.regs[5], (unsigned long long)M.heart.regs[0]);
    printf("eccezioni:");
    for (int k = 0; k < 32; k++)
        if (c->exc_count[k]) printf(" %s=%llu", r4k_exc_names[k], (unsigned long long)c->exc_count[k]);
    printf("\naccessi senza dispositivo (%d blocchi da 256 byte):\n", n_io_pages);
    qsort(io_pages, (size_t)n_io_pages, sizeof io_pages[0], cmp_pages);
    for (int k = 0; k < n_io_pages && k < 25; k++)
        printf("  %010llX  letture %-10llu scritture %-10llu %s\n", (unsigned long long)(io_pages[k].page << 8),
               (unsigned long long)io_pages[k].reads, (unsigned long long)io_pages[k].writes,
               io_pages[k].what ? io_pages[k].what : "");
    return 0;
}
