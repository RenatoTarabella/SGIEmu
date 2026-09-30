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
#include <time.h>
#include "indy/indy.h"
#include "cpu/r4k_disasm.h"
#include "frontend/png.h"

static Indy M;
static long exc_print_left = 20;
static int last_tx_ch = 1;
static int col0 = 1;
static int mc_log, scsi_log, nv_log, kbd_log, ndisk;
static long np_log, np_trace;
static uint64_t np_trace_after;
static const char *disk_path[8], *cdrom_path;
static uint64_t dis_addr;
static int dis_count = 48;
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

/* --kbd: i caratteri di --keys vanno alla tastiera PS/2 (disposizione USA)
   invece che alla seriale */
static int kbd_mode;

static void send_char(uint8_t ch)
{
    if (!kbd_mode) { scc_rx(&M.scc, last_tx_ch, ch); return; }
    static const char row[] = "1234567890-=\0\0qwertyuiop[]\0\0asdfghjkl;'`\0\\zxcvbnm,./";
    static const char shifted[] = "!@#$%^&*()_+\0\0QWERTYUIOP{}\0\0ASDFGHJKL:\"~\0|ZXCVBNM<>?";
    int sc = 0, shift = 0;
    if (ch == '\r' || ch == '\n') sc = 0x1C;
    else if (ch == ' ') sc = 0x39;
    else if (ch == '\t') sc = 0x0F;
    else if (ch == 8) sc = 0x0E;
    else {
        for (int k = 0; k < (int)sizeof row - 1; k++) {
            if (row[k] && row[k] == (char)ch) { sc = 0x02 + k; break; }
            if (shifted[k] && shifted[k] == (char)ch) { sc = 0x02 + k; shift = 1; break; }
        }
    }
    if (!sc) return;
    if (shift) kbc_key(&M.kbc, 0x2A, 0, 1);
    kbc_key(&M.kbc, sc, 0, 1);
    kbc_key(&M.kbc, sc, 0, 0);
    if (shift) kbc_key(&M.kbc, 0x2A, 0, 0);
    indy_update_irq(&M);
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
        else if (!strcmp(a, "--kbd")) kbd_mode = 1;
        else if (!strcmp(a, "--nvlog")) nv_log = 1;
        else if (!strcmp(a, "--kbdlog")) kbd_log = 1;
        else if (!strcmp(a, "--nplog") && v) { np_log = atol(v); k++; }
        else if (!strcmp(a, "--nptrace") && v) { np_trace = atol(v); k++; }
        else if (!strcmp(a, "--nptrace-after") && v) { np_trace_after = strtoull(v, NULL, 0); k++; }
        else if (!strcmp(a, "--dis") && v) {           /* --dis ADDR[,N] */
            char *e;
            dis_addr = strtoull(v, &e, 16);
            if (*e == ',') dis_count = atoi(e + 1);
            k++;
        }
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
    M.kbd_log = kbd_log;
    M.gfx_log = np_log;
    M.gfx.trace = np_trace;
    M.gfx.trace_after = np_trace_after;
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
            if (!strncmp(kp, "{CR}", 4)) { send_char(13); kp += 4; next_key += 30 * per_ms; }
            else if (!strncmp(kp, "{WAIT}", 6)) { kp += 6; next_key += 500 * per_ms; }
            else if (!strncmp(kp, "{T=", 3)) {                  /* {T=190}: aspetta il secondo 190 */
                next_key = strtoull(kp + 3, NULL, 10) * 1000 * per_ms;
                kp = strchr(kp, '}') ? strchr(kp, '}') + 1 : kp + 3;
            }
            else if (!strncmp(kp, "{M=", 3)) {                  /* {M=dx,dy,tasti}: mouse a passi di 4 */
                int dx = 0, dy = 0, b = 0;
                sscanf(kp + 3, "%d,%d,%d", &dx, &dy, &b);
                do {
                    int sx = dx > 4 ? 4 : dx < -4 ? -4 : dx, sy = dy > 4 ? 4 : dy < -4 ? -4 : dy;
                    while (kbc_room(&M.kbc) < 16) done += indy_run(&M, per_ms);
                    kbc_mouse(&M.kbc, sx, -sy, b);
                    indy_update_irq(&M);
                    done += indy_run(&M, per_ms / 4);
                    dx -= sx; dy -= sy;
                } while (dx || dy);
                kp = strchr(kp, '}') + 1;
                next_key = M.cpu.icount + 30 * per_ms;
            }
            else if (!strncmp(kp, "{K=", 3) || !strncmp(kp, "{D=", 3) || !strncmp(kp, "{U=", 3)) {
                /* {K=1D} o {K=E038}: scan code del set 1 premuto e rilasciato;
                   {D=..} solo premuto, {U=..} solo rilasciato (modificatori) */
                unsigned v = (unsigned)strtoul(kp + 3, NULL, 16);
                int ext = v >= 0x100;
                if (kp[1] != 'U') kbc_key(&M.kbc, (int)(v & 0xFF), ext, 1);
                if (kp[1] != 'D') kbc_key(&M.kbc, (int)(v & 0xFF), ext, 0);
                indy_update_irq(&M);
                kp = strchr(kp, '}') + 1;
                next_key += 30 * per_ms;
            }
            else if (!strncmp(kp, "{REG}", 5)) {                /* dove e' la CPU adesso */
                fprintf(stderr, "[t=%.3f pc=%llX ra=%llX a0=%llX a1=%llX a2=%llX sp=%llX Status=%llX]\n",
                        M.cpu.icount / (double)per_ms / 1000, (unsigned long long)M.cpu.pc,
                        (unsigned long long)M.cpu.r[31], (unsigned long long)M.cpu.r[4],
                        (unsigned long long)M.cpu.r[5], (unsigned long long)M.cpu.r[6],
                        (unsigned long long)M.cpu.r[29], (unsigned long long)M.cpu.cp0[CP0_STATUS]);
                kp += 5;
                next_key += 200 * per_ms;
            }
            else if (!strncmp(kp, "{PNG}", 5) && png_path && M.has_gfx) {   /* schermata intermedia */
                static uint32_t shot[NP_W * NP_H];
                static int nshot;
                char name[512];
                snprintf(name, sizeof name, "%s.%d.png", png_path, nshot++);
                np_render(&M.gfx, shot);
                if (png_write(name, shot, NP_W, NP_H, 1) ) fprintf(stderr, "schermo in %s (comandi Newport %llu)\n", name,
                                                                            (unsigned long long)M.gfx.draws);
                kp += 5;
            }
            else { send_char((uint8_t)*kp++); next_key += 30 * per_ms; }
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
    if (dis_addr) {                                /* --dis: dis_count istruzioni da li' */
        uint64_t a = (uint64_t)(int64_t)(int32_t)dis_addr;
        for (int j = 0; j < dis_count; j++, a += 4) {
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
    if (M.has_gfx) {                                   /* stato della Newport */
        const Newport *g = &M.gfx;
        long nz[4] = { 0, 0, 0, 0 };
        uint32_t hist[16] = { 0 };
        for (int k = 0; k < NP_FBW * NP_H; k++) {
            if (g->rgb[k]) { nz[0]++; hist[g->rgb[k] & 15]++; }
            if (g->olay[k]) nz[1]++;
            if (g->pup[k]) nz[2]++;
            if (g->cid[k]) nz[3]++;
        }
        printf("Newport: comandi %llu, pixel non nulli rgb=%ld olay=%ld pup=%ld cid=%ld\n",
               (unsigned long long)g->draws, nz[0], nz[1], nz[2], nz[3]);
        printf("  xmap modi:");
        for (int k = 0; k < 32; k++) printf(" %X", g->xmap_mode[k]);
        printf("\n  xmap config=%X curs=%X pup=%X  vc2 ctrl=%X did=%X/%X topscan=%X xywin=%08X\n",
               g->xmap_config, g->xmap_curs_msb, g->xmap_pup_msb, g->vc2_reg[0x10],
               g->vc2_reg[0x0E], g->vc2_reg[0x0F], g->topscan, g->xywin);
        uint16_t de = g->vc2_reg[5];
        printf("  vc2 did entry=%04X: tabella", de);
        for (int k = 0; k < 6; k++) printf(" %04X", g->vc2_ram[(de + k) & 0x7FFF]);
        printf(" ... riga 300 -> %04X:", g->vc2_ram[(de + 300) & 0x7FFF]);
        for (int k = 0; k < 10; k++) printf(" %04X", g->vc2_ram[(g->vc2_ram[(de + 300) & 0x7FFF] + k) & 0x7FFF]);
        for (int rr = 10; rr < 160; rr += 30) {
            uint16_t l = g->vc2_ram[(de + rr) & 0x7FFF];
            printf("\n  riga %d -> %04X:", rr, l);
            for (int k = 0; k < 10; k++) printf(" %04X", g->vc2_ram[(l + k) & 0x7FFF]);
        }
        printf("\n  hostrw (32/64 bit): set0 %llu/%llu set1 %llu/%llu go0 %llu/%llu go1 %llu/%llu",
               (unsigned long long)g->hoststat[0][0][0], (unsigned long long)g->hoststat[0][0][1],
               (unsigned long long)g->hoststat[0][1][0], (unsigned long long)g->hoststat[0][1][1],
               (unsigned long long)g->hoststat[1][0][0], (unsigned long long)g->hoststat[1][0][1],
               (unsigned long long)g->hoststat[1][1][0], (unsigned long long)g->hoststat[1][1][1]);
        printf("\n  riga 800 -> %04X:", g->vc2_ram[(de + 800) & 0x7FFF]);
        for (int k = 0; k < 10; k++) printf(" %04X", g->vc2_ram[(g->vc2_ram[(de + 800) & 0x7FFF] + k) & 0x7FFF]);
        printf("  pixel (100,200)=%X (100,800)=%X (100,1000)=%X", g->rgb[200 * NP_FBW + 108],
               g->rgb[800 * NP_FBW + 108], g->rgb[1000 * NP_FBW + 108]);
        printf("\n  vc2 regs:");
        for (int k = 0; k < 0x11; k++) printf(" %X", g->vc2_reg[k]);
        printf("\n");
        if (getenv("FB_HIST")) {                        /* valori grezzi in un rettangolo x,y,w,h */
            int hx = 300, hy = 10, hw = 256, hh = 60;
            sscanf(getenv("FB_HIST"), "%d,%d,%d,%d", &hx, &hy, &hw, &hh);
            static uint32_t val[64], cnt[64];
            int nval = 0;
            for (int y = hy; y < hy + hh; y++)
                for (int x = hx; x < hx + hw; x++) {
                    uint32_t v = g->rgb[y * NP_FBW + x + NP_X0];
                    int k = 0;
                    while (k < nval && val[k] != v) k++;
                    if (k == nval && nval < 64) { val[nval] = v; cnt[nval++] = 0; }
                    if (k < 64) cnt[k]++;
                }
            printf("  valori del framebuffer in %d,%d %dx%d:", hx, hy, hw, hh);
            for (int k = 0; k < nval; k++) printf(" %X:%u", val[k], cnt[k]);
            printf("\n  prima riga:");
            for (int x = hx; x < hx + 32; x++) printf(" %X", g->rgb[hy * NP_FBW + x + NP_X0]);
            printf("\n");
        }
        printf("  pixel a (640,300): rgb=%X  cmap[0..7]:", g->rgb[300 * NP_FBW + 648]);
        for (int k = 0; k < 8; k++) printf(" %06X", g->cmap[k]);
        printf("\n");
    }
    printf("profilo: accessi Newport %llu (%.1f Gcicli), flush MMU veloce %llu\n",
           (unsigned long long)M.np_calls, M.np_ticks / 1e9, (unsigned long long)M.cpu.flushes);
    if (M.has_gfx) {
        printf("comandi Newport per tipo (quanti, pixel):\n");
        for (int k = 0; k < 32; k++)
            if (M.gfx.prof[k].n)
                printf("  dm0=%08X dm1=%08X k%u  %12llu  %14llu\n", M.gfx.prof[k].dm0, M.gfx.prof[k].dm1,
                       M.gfx.prof[k].kind, (unsigned long long)M.gfx.prof[k].n, (unsigned long long)M.gfx.prof[k].px);
    }
    if (M.has_gfx && getenv("RENDER_BENCH")) {        /* tempo di np_render */
        static uint32_t scr[NP_W * NP_H];
        clock_t t0 = clock();
        for (int k = 0; k < 30; k++) np_render(&M.gfx, scr);
        printf("np_render: %.1f ms\n", (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 30);
    }
    if (png_path && M.has_gfx) {
        static uint32_t screen[NP_W * NP_H];
        np_render(&M.gfx, screen);
        if (png_write(png_path, screen, NP_W, NP_H, 1)) printf("schermo in %s\n", png_path);
    }
    printf("eccezioni:");
    for (int k = 0; k < 32; k++)
        if (c->exc_count[k]) printf(" %s=%llu", r4k_exc_names[k], (unsigned long long)c->exc_count[k]);
    printf("\nMC: mconfig0=%08X mconfig1=%08X cpuctrl0=%08X gstat=%08X\n",
           M.mc.mconfig0, M.mc.mconfig1, M.mc.cpuctrl0, M.mc.gstat);
    printf("INT3: imask0=%02X imask1=%02X cmeimask0=%02X cmeimask1=%02X\n",
           M.ioc.imask0, M.ioc.imask1, M.ioc.cmeimask0, M.ioc.cmeimask1);
    printf("  raw lio0=%02X lio1=%02X map=%02X  8042: %d byte in coda, cmd=%02X aux=%d\n",
           M.ioc.lio0_raw, M.ioc.lio1_raw, M.ioc.map_raw, KBC_Q - 1 - kbc_room(&M.kbc),
           M.kbc.cmdbyte, M.kbc.aux_enabled);
    printf("accessi senza dispositivo (%d blocchi da 256 byte):\n", n_io_pages);
    qsort(io_pages, (size_t)n_io_pages, sizeof io_pages[0], cmp_pages);
    for (int k = 0; k < n_io_pages && k < 20; k++)
        printf("  %08X  letture %-10llu scritture %llu\n", io_pages[k].page << 8,
               (unsigned long long)io_pages[k].reads, (unsigned long long)io_pages[k].writes);
    return 0;
}
