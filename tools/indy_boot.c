/*
 * indy_boot.c - Avvio dell'Indy da riga di comando, per seguire il boot
 * della PROM senza finestra.
 *
 *   indy_boot --prom roms/ip24prom.bin [--nvram roms/nvram/rtc] [--ram 64]
 *             [--ms 5000] [--keys "testo{CR}"] [--trace N] [--trace-at ADDR]
 *             [--io N] [--exc N]
 *
 * La console seriale va su stdout. --keys scrive i caratteri sulla seriale
 * (sul canale che ha trasmesso per ultimo) dopo un secondo emulato, uno
 * ogni 2 ms; {CR} e' l'invio, {WAIT} aspetta mezzo secondo, {T=190} fino al
 * secondo emulato 190. Alla fine un
 * riassunto: dove e' la CPU, le eccezioni, gli accessi a dispositivi che
 * non ci sono ancora.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "indy/indy.h"
#include "cpu/r4k_disasm.h"
#include "frontend/png.h"

static Indy M;
static long exc_print_left = 20;
static int last_tx_ch = 1;
static int col0 = 1;
static int mc_log, scsi_log, nv_log, ndisk;
static long np_log, np_trace;
static const char *disk_path[8], *cdrom_path;
static uint64_t dis_addr;
static const char *png_path, *save_nvram;
static uint64_t bp_addr;
static long bp_left = 200;
static int gfx = 8;

/* ---- accessi senza dispositivo ---- */
typedef struct IoPage { uint32_t page; uint64_t reads, writes; } IoPage;
static IoPage io_pages[4096];
static int n_io_pages;
static long io_print_left = 50;

static void count_page(uint32_t pa, int write)
{
    uint32_t page = pa >> 8;                       /* per blocchi da 256 byte */
    for (int k = 0; k < n_io_pages; k++)
        if (io_pages[k].page == page) { if (write) io_pages[k].writes++; else io_pages[k].reads++; return; }
    if (n_io_pages < 4096) {
        io_pages[n_io_pages].page = page;
        io_pages[n_io_pages].reads = !write;
        io_pages[n_io_pages].writes = write;
        n_io_pages++;
    }
}

static void io_log(Indy *m, int write, uint32_t pa, int size, uint64_t val)
{
    count_page(pa, write);
    if (io_print_left > 0) {
        io_print_left--;
        if (!col0) { printf("\n"); col0 = 1; }
        if (write) printf("  io  W%d %08X = %llX  pc=%016llX\n", size * 8, pa,
                          (unsigned long long)val, (unsigned long long)m->cpu.pc);
        else printf("  io  R%d %08X  pc=%016llX\n", size * 8, pa, (unsigned long long)m->cpu.pc);
    }
}

static void scsi_cmd_log(void *user, int id, const uint8_t *cdb, int status, uint32_t len)
{
    (void)user;
    if (!col0) { printf("\n"); col0 = 1; }
    printf("  scsi %d: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X -> stato %d, %u byte\n", id,
           cdb[0], cdb[1], cdb[2], cdb[3], cdb[4], cdb[5], cdb[6], cdb[7], cdb[8], cdb[9], status, len);
}

static void on_exc(R4k *c, int code, void *user)
{
    (void)user;
    if (code == EXC_INT) return;
    if (exc_print_left > 0) {
        exc_print_left--;
        if (!col0) { printf("\n"); col0 = 1; }
        printf("  exc %-5s EPC=%016llX BadVAddr=%016llX", r4k_exc_names[code],
               (unsigned long long)c->cp0[CP0_EPC], (unsigned long long)c->cp0[CP0_BADVADDR]);
        if (code == EXC_DBE || code == EXC_IBE) printf(" fisico=%08X", M.last_berr);
        printf("\n");
    }
}

static void on_tx(void *user, int ch, uint8_t c)
{
    (void)user;
    last_tx_ch = ch;
    if (c == '\r') return;
    if (c >= 32 || c == '\n' || c == '\t' || c == 8) putchar(c);
    else printf("<%02X>", c);
    col0 = c == '\n';
    fflush(stdout);
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
    printf("uso: indy_boot --prom FILE [--nvram FILE] [--ram MB] [--ms MS] [--keys TESTO]\n"
           "                [--trace N] [--trace-at ADDR] [--io N] [--exc N]\n");
    exit(2);
}

int main(int argc, char **argv)
{
    const char *prom_path = NULL, *nvram_path = "roms/nvram/rtc", *keys = "";
    unsigned ram_mb = 64;
    uint64_t ms = 5000, trace_at = 0;
    long trace = 0;
    for (int k = 1; k < argc; k++) {
        const char *a = argv[k], *v = k + 1 < argc ? argv[k + 1] : NULL;
        if (!strcmp(a, "--prom") && v) { prom_path = v; k++; }
        else if (!strcmp(a, "--nvram") && v) { nvram_path = v; k++; }
        else if (!strcmp(a, "--ram") && v) { ram_mb = (unsigned)atoi(v); k++; }
        else if (!strcmp(a, "--ms") && v) { ms = strtoull(v, NULL, 0); k++; }
        else if (!strcmp(a, "--keys") && v) { keys = v; k++; }
        else if (!strcmp(a, "--trace") && v) { trace = atol(v); k++; }
        else if (!strcmp(a, "--trace-at") && v) { trace_at = strtoull(v, NULL, 16); k++; }
        else if (!strcmp(a, "--io") && v) { io_print_left = atol(v); k++; }
        else if (!strcmp(a, "--exc") && v) { exc_print_left = atol(v); k++; }
        else if (!strcmp(a, "--mc")) mc_log = 1;
        else if (!strcmp(a, "--scsi")) scsi_log = 1;
        else if (!strcmp(a, "--nvlog")) nv_log = 1;
        else if (!strcmp(a, "--nplog") && v) { np_log = atol(v); k++; }
        else if (!strcmp(a, "--nptrace") && v) { np_trace = atol(v); k++; }
        else if (!strcmp(a, "--dis") && v) { dis_addr = strtoull(v, NULL, 16); k++; }
        else if (!strcmp(a, "--png") && v) { png_path = v; k++; }
        else if (!strcmp(a, "--bp") && v) { bp_addr = strtoull(v, NULL, 16); k++; }
        else if (!strcmp(a, "--save-nvram") && v) { save_nvram = v; k++; }
        else if (!strcmp(a, "--gfx") && v) { gfx = !strcmp(v, "none") ? 0 : atoi(v); k++; }
        else if (!strcmp(a, "--disk") && v) { disk_path[ndisk++ & 7] = v; k++; }
        else if (!strcmp(a, "--cdrom") && v) { cdrom_path = v; k++; }
        else usage();
    }
    if (!prom_path) usage();

    long prom_size, nv_size;
    uint8_t *prom = load_file(prom_path, &prom_size);
    if (!prom) { printf("non trovo la PROM %s\n", prom_path); return 1; }
    if (indy_init(&M, prom, (uint32_t)prom_size, ram_mb, gfx)) { printf("memoria\n"); return 1; }
    uint8_t *nv = load_file(nvram_path, &nv_size);
    if (nv && nv_size == DS1386_SIZE) {
        memcpy(M.rtc.ram + 14, nv + 14, DS1386_SIZE - 14);      /* l'ora resta quella dell'host */
        printf("NVRAM %s\n", nvram_path);
    }
    /* indirizzo Ethernet (NVOFF_ENET): senza, la PROM non crea eaddr e il
       kernel di IRIX si ferma all'avvio */
    static const uint8_t mac[6] = { 0x08, 0x00, 0x69, 0x0E, 0xAF, 0x65 };
    int has_mac = 0;
    for (int k = 0; k < 6; k++) has_mac |= M.rtc.ram[NVRAM_ENET + k];
    if (!has_mac) { memcpy(M.rtc.ram + NVRAM_ENET, mac, 6); rtc_fix_checksum(&M.rtc); }
    M.io_log = io_log;
    M.mc_log = mc_log;
    M.scsi_log = scsi_log;
    M.rtc_log = nv_log;
    M.gfx_log = np_log;
    M.gfx.trace = np_trace;
    if (scsi_log) M.scsi.log = scsi_cmd_log;
    /* dischi a partire dall'ID 1 (dksc(0,1) e' il disco di sistema), CD-ROM al 4 */
    for (int k = 0; k < ndisk; k++)
        if (indy_attach_scsi(&M, 1 + k + (k >= 3), SCSI_DISK, disk_path[k], 0))
            { printf("non riesco ad aprire %s\n", disk_path[k]); return 1; }
        else printf("SCSI %d: disco %s (%llu MB)\n", 1 + k + (k >= 3), disk_path[k],
                    (unsigned long long)(M.disk[1 + k + (k >= 3)].blocks >> 11));
    if (cdrom_path) {
        if (indy_attach_scsi(&M, 4, SCSI_CDROM, cdrom_path, 1)) { printf("non riesco ad aprire %s\n", cdrom_path); return 1; }
        printf("SCSI 4: CD-ROM %s\n", cdrom_path);
    }
    M.cpu.exc_hook = on_exc;
    M.scc.tx = on_tx;
    printf("PROM %s: %ld byte, RAM %u MB\n", prom_path, prom_size, ram_mb);

    const uint64_t per_ms = 1000000u / M.ns_per_insn;
    uint64_t total = ms * per_ms, done = 0;
    uint64_t next_key = 1000 * per_ms;
    const char *kp = keys;
    while (done < total && !M.power_off) {
        if (bp_addr) {                                  /* --bp: a passi, stampa i registri */
            uint64_t stop = done + per_ms;
            while (done < stop && done < total && !M.power_off) {
                if ((uint32_t)M.cpu.pc == (uint32_t)bp_addr && bp_left > 0) {
                    bp_left--;
                    printf("  bp %08llX:", (unsigned long long)(uint32_t)M.cpu.pc);
                    for (int i = 1; i < 32; i++)
                        if (M.cpu.r[i]) printf(" %s=%llX", r4k_reg_names[i], (unsigned long long)M.cpu.r[i]);
                    printf("\n");
                }
                done += indy_run(&M, 1);
            }
            continue;
        }
        if (trace > 0 && (!trace_at || M.cpu.pc == trace_at)) {
            trace_at = 0;
            trace_one(&M.cpu);
            done += indy_run(&M, 1);
            trace--;
            continue;
        }
        if (*kp && M.cpu.icount >= next_key) {
            if (!strncmp(kp, "{CR}", 4)) { scc_rx(&M.scc, last_tx_ch, '\r'); kp += 4; next_key += 2 * per_ms; }
            else if (!strncmp(kp, "{WAIT}", 6)) { kp += 6; next_key += 500 * per_ms; }
            else if (!strncmp(kp, "{T=", 3)) {                  /* {T=190}: aspetta il secondo 190 */
                next_key = strtoull(kp + 3, NULL, 10) * 1000 * per_ms;
                kp = strchr(kp, '}') ? strchr(kp, '}') + 1 : kp + 3;
            }
            else { scc_rx(&M.scc, last_tx_ch, (uint8_t)*kp++); next_key += 2 * per_ms; }
        }
        uint64_t chunk = trace > 0 ? 1 : per_ms;
        if (chunk > total - done) chunk = total - done;
        done += indy_run(&M, chunk);
    }

    printf("%s\n%.3f s emulati, %llu istruzioni%s\n", col0 ? "" : "\n", done / (double)per_ms / 1000,
           (unsigned long long)M.cpu.icount, M.power_off ? " (spenta)" : "");
    trace_one(&M.cpu);
    R4k *c = &M.cpu;
    printf("Status=%08llX Cause=%08llX EPC=%016llX BadVAddr=%016llX\n",
           (unsigned long long)(uint32_t)c->cp0[CP0_STATUS], (unsigned long long)(uint32_t)c->cp0[CP0_CAUSE],
           (unsigned long long)c->cp0[CP0_EPC], (unsigned long long)c->cp0[CP0_BADVADDR]);
    for (int i = 0; i < 32; i += 4)
        printf("  %-4s %016llX  %-4s %016llX  %-4s %016llX  %-4s %016llX\n",
               r4k_reg_names[i], (unsigned long long)c->r[i], r4k_reg_names[i + 1], (unsigned long long)c->r[i + 1],
               r4k_reg_names[i + 2], (unsigned long long)c->r[i + 2], r4k_reg_names[i + 3], (unsigned long long)c->r[i + 3]);
    if (dis_addr) {                                /* --dis: 48 istruzioni da li' */
        uint64_t a = (uint64_t)(int64_t)(int32_t)dis_addr;
        for (int j = 0; j < 48; j++, a += 4) {
            uint64_t w; char b[96];
            if (!r4k_peek(&M.cpu, a, 4, &w)) break;
            r4k_disasm(a, (uint32_t)w, b, sizeof b);
            printf("  %08llX  %08X  %s\n", (unsigned long long)(uint32_t)a, (uint32_t)w, b);
        }
    }
    if (save_nvram) {
        FILE *nf = fopen(save_nvram, "wb");
        if (nf) { fwrite(M.rtc.ram, 1, DS1386_SIZE, nf); fclose(nf); printf("NVRAM salvata in %s\n", save_nvram); }
    }
    if (png_path && M.has_gfx) {
        static uint32_t screen[NP_W * NP_H];
        np_render(&M.gfx, screen);
        if (png_write(png_path, screen, NP_W, NP_H, 1) == 0) printf("schermo in %s\n", png_path);
    }
    printf("eccezioni:");
    for (int k = 0; k < 32; k++)
        if (c->exc_count[k]) printf(" %s=%llu", r4k_exc_names[k], (unsigned long long)c->exc_count[k]);
    printf("\nMC: mconfig0=%08X mconfig1=%08X cpuctrl0=%08X gstat=%08X\n",
           M.mc.mconfig0, M.mc.mconfig1, M.mc.cpuctrl0, M.mc.gstat);
    printf("accessi senza dispositivo (%d blocchi da 256 byte):\n", n_io_pages);
    qsort(io_pages, (size_t)n_io_pages, sizeof io_pages[0], cmp_pages);
    for (int k = 0; k < n_io_pages && k < 20; k++)
        printf("  %08X  letture %-10llu scritture %llu\n", io_pages[k].page << 8,
               (unsigned long long)io_pages[k].reads, (unsigned long long)io_pages[k].writes);
    return 0;
}
