/*
 * indy_win32.c - L'Indy in una finestra: schermo Newport, tastiera e mouse PS/2
 *
 *   indy [--prom FILE] [--disk IMG] [--disk2 IMG] [--cdrom ISO] [--ram MB]
 *        [--nvram FILE] [--gfx 8|24] [--mips N]
 *
 * Senza argomenti cerca roms\ip24prom.bin e disks\irix53.img. La NVRAM
 * (orologio e ambiente della PROM) si salva all'uscita in roms\nvram\indy.nvram;
 * la prima volta parte da quella di MAME (roms\nvram\rtc).
 *
 * Tastiera: si passano i tasti fisici (scan code), la disposizione la sceglie
 * IRIX. Mouse: un clic nella finestra lo cattura, Ctrl+F11 lo libera.
 * La console seriale va nel terminale da cui si e' lanciato il programma.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "indy/indy.h"

#define FRAME_HZ 60

typedef struct App {
    HWND     hwnd;
    Indy     m;
    uint32_t pixels[NP_W * NP_H];
    int      captured;
    int      buttons;                 /* tasti del mouse: bit 0 sin, 1 des, 2 centro */
    long     acc_x, acc_y;            /* movimento non ancora mandato */
    int      mouse_dirty;
    char     title[256];
    double   mips;
    char     nvram_path[MAX_PATH];
    uint64_t shown_gen;               /* versione della Newport sullo schermo */
} App;

static App app;

static double now_s(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static void update_title(void)
{
    char t[256];
    snprintf(t, sizeof t, "Indy (IP24)   %.0f MIPS   %s", app.mips,
             app.captured ? "[mouse catturato: Ctrl+F11 per liberarlo]" : "[clic nella finestra per catturare il mouse]");
    if (strcmp(t, app.title)) {
        strcpy(app.title, t);
        SetWindowTextA(app.hwnd, t);
    }
}

static void fit_window(void)
{
    RECT work;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
    int ww = (work.right - work.left) * 95 / 100, wh = (work.bottom - work.top) * 95 / 100;
    int w = NP_W, h = NP_H;
    if (w > ww || h > wh) {                       /* ridotto mantenendo le proporzioni */
        if ((long)ww * NP_H < (long)wh * NP_W) { w = ww; h = ww * NP_H / NP_W; }
        else { h = wh; w = wh * NP_W / NP_H; }
    }
    RECT r = { 0, 0, w, h };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    SetWindowPos(app.hwnd, NULL, work.left + 8, work.top + 8, r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
}

static void present(HDC dc)
{
    np_render(&app.m.gfx, app.pixels);
    RECT rc;
    GetClientRect(app.hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    int sw = cw, sh = cw * NP_H / NP_W;
    if (sh > ch) { sh = ch; sw = ch * NP_W / NP_H; }
    int ox = (cw - sw) / 2, oy = (ch - sh) / 2;
    RECT bands[4] = { { 0, 0, cw, oy }, { 0, oy + sh, cw, ch }, { 0, oy, ox, oy + sh }, { ox + sw, oy, cw, oy + sh } };
    for (int i = 0; i < 4; i++) FillRect(dc, &bands[i], (HBRUSH)GetStockObject(BLACK_BRUSH));
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = NP_W;
    bi.bmiHeader.biHeight = -NP_H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, ox, oy, sw, sh, 0, 0, NP_W, NP_H, app.pixels, &bi, DIB_RGB_COLORS, SRCCOPY);
}

static void capture_mouse(int on)
{
    if (on == app.captured) return;
    app.captured = on;
    if (on) {
        RECT rc;
        GetClientRect(app.hwnd, &rc);
        MapWindowPoints(app.hwnd, NULL, (POINT *)&rc, 2);
        ClipCursor(&rc);
        ShowCursor(FALSE);
        SetCapture(app.hwnd);
    } else {
        ClipCursor(NULL);
        ShowCursor(TRUE);
        ReleaseCapture();
        app.buttons = 0;
        app.mouse_dirty = 1;
    }
    update_title();
}

/* movimento e tasti del mouse raccolti in un frame: un pacchetto PS/2 per volta */
static void flush_mouse(void)
{
    Kbc8042 *k = &app.m.kbc;
    while (app.mouse_dirty || app.acc_x || app.acc_y) {
        if (kbc_room(k) < 16) break;
        long dx = app.acc_x, dy = app.acc_y;
        if (dx > 127) dx = 127; else if (dx < -127) dx = -127;
        if (dy > 127) dy = 127; else if (dy < -127) dy = -127;
        kbc_mouse(k, (int)dx, (int)-dy, app.buttons);
        app.acc_x -= dx; app.acc_y -= dy;
        app.mouse_dirty = 0;
    }
    indy_update_irq(&app.m);
}

static void mouse_button(int bit, int down)
{
    if (down) app.buttons |= bit; else app.buttons &= ~bit;
    app.mouse_dirty = 1;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        if (down && (lp & (1 << 30))) return 0;             /* ripetizione: la fa X */
        if (wp == VK_F11 && GetKeyState(VK_CONTROL) < 0) { if (down) capture_mouse(0); return 0; }
        int sc = (int)((lp >> 16) & 0xFF), ext = (int)((lp >> 24) & 1);
        if (wp == VK_PAUSE || sc == 0) return 0;
        kbc_key(&app.m.kbc, sc, ext, down);
        indy_update_irq(&app.m);
        return 0;
    }
    case WM_SYSCHAR: case WM_CHAR:
        return 0;
    case WM_INPUT: {
        if (!app.captured) break;
        RAWINPUT ri;
        UINT size = sizeof ri;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
            app.acc_x += ri.data.mouse.lLastX;
            app.acc_y += ri.data.mouse.lLastY;
        }
        break;
    }
    case WM_LBUTTONDOWN:
        if (!app.captured) { capture_mouse(1); return 0; }
        mouse_button(1, 1); return 0;
    case WM_LBUTTONUP:   if (app.captured) mouse_button(1, 0); return 0;
    case WM_RBUTTONDOWN: if (app.captured) mouse_button(2, 1); return 0;
    case WM_RBUTTONUP:   if (app.captured) mouse_button(2, 0); return 0;
    case WM_MBUTTONDOWN: if (app.captured) mouse_button(4, 1); return 0;
    case WM_MBUTTONUP:   if (app.captured) mouse_button(4, 0); return 0;
    case WM_KILLFOCUS:   capture_mouse(0); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        present(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: break;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* ---- file ---- */

static const char *find_file(const char *const *rel, size_t n, char *buf, size_t size)
{
    for (size_t i = 0; i < n; i++)
        if (GetFileAttributesA(rel[i]) != INVALID_FILE_ATTRIBUTES) { snprintf(buf, size, "%s", rel[i]); return buf; }
    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash = len ? strrchr(exe, '\\') : NULL;
    if (slash) {
        slash[1] = 0;
        for (size_t i = 0; i < n; i++) {
            snprintf(buf, size, "%s%s", exe, rel[i]);
            if (GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES) return buf;
        }
    }
    return NULL;
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

static void on_tx(void *user, int ch, uint8_t c)
{
    (void)user; (void)ch;
    if (c != '\r') { putchar(c); fflush(stdout); }
}

static void fail(const char *msg)
{
    MessageBoxA(NULL, msg, "Indy", MB_ICONERROR);
    exit(1);
}

int main(int argc, char **argv)
{
    const char *prom_path = NULL, *disk_path[2] = { NULL, NULL }, *cdrom_path = NULL, *nvram_arg = NULL;
    unsigned ram_mb = 64, mips = 100;
    int gfx = 8;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--prom") && v) { prom_path = v; i++; }
        else if (!strcmp(a, "--disk") && v) { disk_path[0] = v; i++; }
        else if (!strcmp(a, "--disk2") && v) { disk_path[1] = v; i++; }
        else if (!strcmp(a, "--cdrom") && v) { cdrom_path = v; i++; }
        else if (!strcmp(a, "--ram") && v) { ram_mb = (unsigned)atoi(v); i++; }
        else if (!strcmp(a, "--nvram") && v) { nvram_arg = v; i++; }
        else if (!strcmp(a, "--gfx") && v) { gfx = atoi(v); i++; }
        else if (!strcmp(a, "--mips") && v) { mips = (unsigned)atoi(v); i++; }   /* CPU emulata piu' veloce */
    }
    /* la console seriale va nel terminale che ha lanciato il programma */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) { freopen("CONOUT$", "w", stdout); printf("\n"); }

    static char prom_buf[MAX_PATH], disk_buf[MAX_PATH];
    if (!prom_path) {
        static const char *p[] = { "roms\\ip24prom.bin", "..\\roms\\ip24prom.bin", "..\\..\\roms\\ip24prom.bin" };
        prom_path = find_file(p, 3, prom_buf, sizeof prom_buf);
        if (!prom_path) fail("PROM non trovata: usa --prom percorso\\ip24prom.bin");
    }
    if (!disk_path[0]) {
        static const char *d[] = { "disks\\irix53.img", "..\\disks\\irix53.img", "..\\..\\disks\\irix53.img" };
        disk_path[0] = find_file(d, 3, disk_buf, sizeof disk_buf);
    }

    long prom_size;
    uint8_t *prom = load_file(prom_path, &prom_size);
    if (!prom) fail("Non riesco a leggere la PROM");
    if (indy_init(&app.m, prom, (uint32_t)prom_size, ram_mb, gfx)) fail("Memoria insufficiente");
    if (mips >= 10 && mips <= 1000) app.m.ps_per_insn = 1000000u / mips;
    free(prom);
    app.m.scc.tx = on_tx;

    /* NVRAM: la nostra accanto alla PROM, la prima volta quella di MAME */
    if (nvram_arg) snprintf(app.nvram_path, sizeof app.nvram_path, "%s", nvram_arg);
    else {
        snprintf(app.nvram_path, sizeof app.nvram_path, "%s", prom_path);
        char *slash = strrchr(app.nvram_path, '\\');
        snprintf(slash ? slash + 1 : app.nvram_path, sizeof app.nvram_path - (slash ? (size_t)(slash + 1 - app.nvram_path) : 0),
                 "nvram\\indy.nvram");
    }
    long nv_size = 0;
    uint8_t *nv = load_file(app.nvram_path, &nv_size);
    if (!nv) {
        char mame[MAX_PATH];
        snprintf(mame, sizeof mame, "%s", app.nvram_path);
        char *slash = strrchr(mame, '\\');
        if (slash) { strcpy(slash + 1, "rtc"); nv = load_file(mame, &nv_size); }
    }
    if (nv && nv_size == DS1386_SIZE) memcpy(app.m.rtc.ram + 14, nv + 14, DS1386_SIZE - 14);
    free(nv);
    int has_mac = 0;
    for (int k = 0; k < 6; k++) has_mac |= app.m.rtc.ram[NVRAM_ENET + k];
    if (!has_mac) {
        static const uint8_t mac[6] = { 0x08, 0x00, 0x69, 0x0E, 0xAF, 0x65 };
        memcpy(app.m.rtc.ram + NVRAM_ENET, mac, 6);
        rtc_fix_checksum(&app.m.rtc);
    }

    for (int k = 0; k < 2; k++)
        if (disk_path[k] && indy_attach_scsi(&app.m, 1 + k, SCSI_DISK, disk_path[k], 0)) {
            char msg[MAX_PATH + 64];
            snprintf(msg, sizeof msg, "Non riesco ad aprire il disco:\n%s", disk_path[k]);
            fail(msg);
        }
    if (cdrom_path && indy_attach_scsi(&app.m, 4, SCSI_CDROM, cdrom_path, 1)) fail("Non riesco ad aprire il CD-ROM");

    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "IndyWindow";
    RegisterClassA(&wc);
    app.hwnd = CreateWindowA(wc.lpszClassName, "Indy", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             NP_W, NP_H, NULL, NULL, inst, NULL);
    RAWINPUTDEVICE rid = { 0x01, 0x02, 0, app.hwnd };            /* mouse: movimento relativo */
    RegisterRawInputDevices(&rid, 1, sizeof rid);
    fit_window();
    update_title();
    ShowWindow(app.hwnd, SW_SHOW);

    timeBeginPeriod(1);
    const uint64_t per_frame = indy_insn_per_ms(&app.m) * 1000 / FRAME_HZ;
    double next = now_s(), last_title = next;
    uint64_t last_icount = app.m.cpu.icount;
    MSG msg;
    int running = 1, frame = 0;
    while (running && !app.m.power_off) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = 0; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!running) break;

        flush_mouse();
        indy_run(&app.m, per_frame);
        if (++frame % 2 == 0 && app.m.gfx.gen != app.shown_gen) {  /* 30 Hz, solo se cambia */
            app.shown_gen = app.m.gfx.gen;
            HDC dc = GetDC(app.hwnd);
            present(dc);
            ReleaseDC(app.hwnd, dc);
        }
        double t = now_s();
        if (t - last_title >= 1.0) {
            app.mips = (double)(app.m.cpu.icount - last_icount) / (t - last_title) / 1e6;
            last_icount = app.m.cpu.icount;
            last_title = t;
            update_title();
        }
        next += 1.0 / FRAME_HZ;
        if (next < t - 0.1) next = t;                             /* se non ce la fa, non accumula */
        double wait = next - now_s();
        if (wait > 0) Sleep((DWORD)(wait * 1000));
    }
    timeEndPeriod(1);
    capture_mouse(0);

    FILE *f = fopen(app.nvram_path, "wb");                        /* NVRAM per la prossima volta */
    if (f) { fwrite(app.m.rtc.ram, 1, DS1386_SIZE, f); fclose(f); }
    indy_free(&app.m);
    return 0;
}
