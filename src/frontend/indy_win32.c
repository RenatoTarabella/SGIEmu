/*
 * indy_win32.c - L'Indy in una finestra: schermo Newport, tastiera e mouse PS/2
 *
 *   indy [--prom FILE] [--disk IMG] [--disk2 IMG] [--cdrom ISO] [--ram MB]
 *        [--nvram FILE] [--gfx 8|24] [--mips N] [--nojit]
 *
 * Senza argomenti apre il dialogo di avvio con le ultime impostazioni
 * (indy.ini accanto all'eseguibile); con argomenti parte subito.
 *
 * La CPU gira con il JIT (--nojit: solo interprete, per confronto).
 * --mips N e' la velocita' della CPU emulata (100 = un'Indy a 100 MHz).
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
#include <commdlg.h>
#include "indy/indy.h"
#include "indy_res.h"

/* controlli con lo stile di Windows (Common Controls 6) nel dialogo di avvio */
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

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
        if (sc == 0x1D && !ext) {
            /* AltGr: Windows manda un Ctrl sinistro finto subito prima
               dell'Alt destro, con lo stesso istante; IRIX (disposizioni
               europee) vuole solo l'Alt destro */
            MSG next;
            if (PeekMessageA(&next, hwnd, WM_KEYFIRST, WM_KEYLAST, PM_NOREMOVE) &&
                (next.message == WM_KEYDOWN || next.message == WM_SYSKEYDOWN ||
                 next.message == WM_KEYUP || next.message == WM_SYSKEYUP) &&
                next.wParam == VK_MENU && ((next.lParam >> 24) & 1) && next.time == (DWORD)GetMessageTime())
                return 0;
        }
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

/* ---- impostazioni e dialogo di avvio ---- */

typedef struct Settings {
    char     prom[MAX_PATH], nvram[MAX_PATH], disk[2][MAX_PATH], cdrom[MAX_PATH];
    unsigned ram_mb, mips;
    int      gfx, jit;
} Settings;

static const unsigned ram_choices[] = { 16, 32, 64, 128, 256 };
static const unsigned mips_choices[] = { 100, 150, 200, 300, 400, 500, 800 };

static void exe_dir_file(const char *name, char *buf, size_t size)
{
    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash = len ? strrchr(exe, '\\') : NULL;
    if (slash) slash[1] = 0; else exe[0] = 0;
    snprintf(buf, size, "%s%s", exe, name);
}

static void full_path(char *path)
{
    char buf[MAX_PATH];
    if (path[0] && GetFullPathNameA(path, MAX_PATH, buf, NULL)) strcpy(path, buf);
}

/* NVRAM di default: roms\nvram\indy.nvram accanto alla PROM */
static void default_nvram(const char *prom, char *buf, size_t size)
{
    snprintf(buf, size, "%s", prom);
    char *slash = strrchr(buf, '\\');
    size_t off = slash ? (size_t)(slash + 1 - buf) : 0;
    snprintf(buf + off, size - off, "nvram\\indy.nvram");
}

static void settings_defaults(Settings *st)
{
    static const char *p[] = { "roms\\ip24prom.bin", "..\\roms\\ip24prom.bin", "..\\..\\roms\\ip24prom.bin" };
    static const char *d[] = { "disks\\irix53.img", "..\\disks\\irix53.img", "..\\..\\disks\\irix53.img" };
    memset(st, 0, sizeof *st);
    if (!find_file(p, 3, st->prom, sizeof st->prom)) st->prom[0] = 0;
    if (!find_file(d, 3, st->disk[0], sizeof st->disk[0])) st->disk[0][0] = 0;
    full_path(st->prom);
    full_path(st->disk[0]);
    st->ram_mb = 64; st->mips = 100; st->gfx = 8; st->jit = 1;
}

static void settings_load(Settings *st)
{
    char ini[MAX_PATH];
    exe_dir_file("indy.ini", ini, sizeof ini);
    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) return;   /* prima volta: i default */
    GetPrivateProfileStringA("Indy", "prom", st->prom, st->prom, MAX_PATH, ini);
    GetPrivateProfileStringA("Indy", "nvram", "", st->nvram, MAX_PATH, ini);
    GetPrivateProfileStringA("Indy", "disk1", "", st->disk[0], MAX_PATH, ini);
    GetPrivateProfileStringA("Indy", "disk2", "", st->disk[1], MAX_PATH, ini);
    GetPrivateProfileStringA("Indy", "cdrom", "", st->cdrom, MAX_PATH, ini);
    st->ram_mb = GetPrivateProfileIntA("Indy", "ram", (INT)st->ram_mb, ini);
    st->mips = GetPrivateProfileIntA("Indy", "mips", (INT)st->mips, ini);
    st->gfx = GetPrivateProfileIntA("Indy", "gfx", st->gfx, ini);
    st->jit = GetPrivateProfileIntA("Indy", "jit", st->jit, ini);
}

static void settings_save(const Settings *st)
{
    char ini[MAX_PATH], v[32];
    exe_dir_file("indy.ini", ini, sizeof ini);
    WritePrivateProfileStringA("Indy", "prom", st->prom, ini);
    WritePrivateProfileStringA("Indy", "nvram", st->nvram, ini);
    WritePrivateProfileStringA("Indy", "disk1", st->disk[0], ini);
    WritePrivateProfileStringA("Indy", "disk2", st->disk[1], ini);
    WritePrivateProfileStringA("Indy", "cdrom", st->cdrom, ini);
    snprintf(v, sizeof v, "%u", st->ram_mb); WritePrivateProfileStringA("Indy", "ram", v, ini);
    snprintf(v, sizeof v, "%u", st->mips);   WritePrivateProfileStringA("Indy", "mips", v, ini);
    snprintf(v, sizeof v, "%d", st->gfx);    WritePrivateProfileStringA("Indy", "gfx", v, ini);
    snprintf(v, sizeof v, "%d", st->jit);    WritePrivateProfileStringA("Indy", "jit", v, ini);
}

static void browse(HWND dlg, int edit, const char *filter)
{
    char file[MAX_PATH], dir[MAX_PATH] = "";
    GetDlgItemTextA(dlg, edit, file, MAX_PATH);
    if (file[0]) {                                   /* parte dalla cartella del file attuale */
        strcpy(dir, file);
        char *slash = strrchr(dir, '\\');
        if (slash) *slash = 0;
        const char *name = slash ? strrchr(file, '\\') + 1 : file;
        memmove(file, name, strlen(name) + 1);
    }
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = dlg;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir[0] ? dir : NULL;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) SetDlgItemTextA(dlg, edit, file);
}

static int file_ok(HWND dlg, const char *path, const char *what, int required)
{
    if (!path[0] && !required) return 1;
    if (path[0] && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return 1;
    char msg[MAX_PATH + 64];
    snprintf(msg, sizeof msg, path[0] ? "%s: file not found\n%s" : "%s: no file given%s", what, path);
    MessageBoxA(dlg, msg, "Indy", MB_ICONWARNING);
    return 0;
}

static INT_PTR CALLBACK setup_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static Settings *st;
    static const char *const f_rom = "PROM (*.bin)\0*.bin\0All files\0*.*\0";
    static const char *const f_nv = "NVRAM (*.nvram)\0*.nvram;rtc\0All files\0*.*\0";
    static const char *const f_disk = "Disk images (*.img, *.raw)\0*.img;*.raw\0All files\0*.*\0";
    static const char *const f_cd = "CD images (*.iso)\0*.iso\0All files\0*.*\0";
    char buf[64];
    switch (msg) {
    case WM_INITDIALOG: {
        st = (Settings *)lp;
        HICON icon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDI_INDY));
        SendMessageA(dlg, WM_SETICON, ICON_BIG, (LPARAM)icon);
        SendMessageA(dlg, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        char nv[MAX_PATH];
        if (st->nvram[0]) strcpy(nv, st->nvram); else default_nvram(st->prom, nv, sizeof nv);
        SetDlgItemTextA(dlg, IDC_PROM, st->prom);
        SetDlgItemTextA(dlg, IDC_NVRAM, nv);
        SetDlgItemTextA(dlg, IDC_DISK1, st->disk[0]);
        SetDlgItemTextA(dlg, IDC_DISK2, st->disk[1]);
        SetDlgItemTextA(dlg, IDC_CDROM, st->cdrom);
        int sel = 2;
        for (int i = 0; i < 5; i++) {
            snprintf(buf, sizeof buf, "%u MB", ram_choices[i]);
            SendDlgItemMessageA(dlg, IDC_RAM, CB_ADDSTRING, 0, (LPARAM)buf);
            if (ram_choices[i] == st->ram_mb) sel = i;
        }
        SendDlgItemMessageA(dlg, IDC_RAM, CB_SETCURSEL, (WPARAM)sel, 0);
        SendDlgItemMessageA(dlg, IDC_GFX, CB_ADDSTRING, 0, (LPARAM)"XL8 (8 bit)");
        SendDlgItemMessageA(dlg, IDC_GFX, CB_ADDSTRING, 0, (LPARAM)"XL24 (24 bit)");
        SendDlgItemMessageA(dlg, IDC_GFX, CB_SETCURSEL, st->gfx == 24 ? 1 : 0, 0);
        for (int i = 0; i < 7; i++) {
            snprintf(buf, sizeof buf, "%u", mips_choices[i]);
            SendDlgItemMessageA(dlg, IDC_MIPS, CB_ADDSTRING, 0, (LPARAM)buf);
        }
        snprintf(buf, sizeof buf, "%u", st->mips);
        SetDlgItemTextA(dlg, IDC_MIPS, buf);
        CheckDlgButton(dlg, IDC_JIT, st->jit ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PROM_B:  browse(dlg, IDC_PROM, f_rom); return TRUE;
        case IDC_NVRAM_B: browse(dlg, IDC_NVRAM, f_nv); return TRUE;
        case IDC_DISK1_B: browse(dlg, IDC_DISK1, f_disk); return TRUE;
        case IDC_DISK2_B: browse(dlg, IDC_DISK2, f_disk); return TRUE;
        case IDC_CDROM_B: browse(dlg, IDC_CDROM, f_cd); return TRUE;
        case IDOK: {
            Settings n = *st;
            GetDlgItemTextA(dlg, IDC_PROM, n.prom, MAX_PATH);
            GetDlgItemTextA(dlg, IDC_NVRAM, n.nvram, MAX_PATH);
            GetDlgItemTextA(dlg, IDC_DISK1, n.disk[0], MAX_PATH);
            GetDlgItemTextA(dlg, IDC_DISK2, n.disk[1], MAX_PATH);
            GetDlgItemTextA(dlg, IDC_CDROM, n.cdrom, MAX_PATH);
            LRESULT ri = SendDlgItemMessageA(dlg, IDC_RAM, CB_GETCURSEL, 0, 0);
            n.ram_mb = ram_choices[ri >= 0 && ri < 5 ? ri : 2];
            n.gfx = SendDlgItemMessageA(dlg, IDC_GFX, CB_GETCURSEL, 0, 0) == 1 ? 24 : 8;
            GetDlgItemTextA(dlg, IDC_MIPS, buf, sizeof buf);
            n.mips = (unsigned)atoi(buf);
            n.jit = IsDlgButtonChecked(dlg, IDC_JIT) == BST_CHECKED;
            if (n.mips < 10 || n.mips > 5000) {
                MessageBoxA(dlg, "Speed: 10 to 5000 MIPS", "Indy", MB_ICONWARNING);
                return TRUE;
            }
            if (!file_ok(dlg, n.prom, "PROM", 1) || !file_ok(dlg, n.disk[0], "Disk SCSI 1", 0) ||
                !file_ok(dlg, n.disk[1], "Disk SCSI 2", 0) || !file_ok(dlg, n.cdrom, "CD-ROM", 0))
                return TRUE;
            /* la NVRAM puo' non esistere ancora: si crea all'uscita */
            char def[MAX_PATH];
            default_nvram(n.prom, def, sizeof def);
            if (!strcmp(n.nvram, def)) n.nvram[0] = 0;    /* segue la PROM */
            *st = n;
            EndDialog(dlg, 1);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, 0);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

int main(int argc, char **argv)
{
    static Settings st;
    settings_defaults(&st);
    if (argc > 1) {                                   /* riga di comando: niente dialogo */
        for (int i = 1; i < argc; i++) {
            const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
            if (!strcmp(a, "--prom") && v) { snprintf(st.prom, MAX_PATH, "%s", v); i++; }
            else if (!strcmp(a, "--disk") && v) { snprintf(st.disk[0], MAX_PATH, "%s", v); i++; }
            else if (!strcmp(a, "--disk2") && v) { snprintf(st.disk[1], MAX_PATH, "%s", v); i++; }
            else if (!strcmp(a, "--cdrom") && v) { snprintf(st.cdrom, MAX_PATH, "%s", v); i++; }
            else if (!strcmp(a, "--ram") && v) { st.ram_mb = (unsigned)atoi(v); i++; }
            else if (!strcmp(a, "--nvram") && v) { snprintf(st.nvram, MAX_PATH, "%s", v); i++; }
            else if (!strcmp(a, "--gfx") && v) { st.gfx = atoi(v); i++; }
            else if (!strcmp(a, "--mips") && v) { st.mips = (unsigned)atoi(v); i++; }   /* CPU emulata piu' veloce */
            else if (!strcmp(a, "--nojit")) st.jit = 0;
        }
    } else {                                          /* dialogo con le ultime impostazioni */
        settings_load(&st);
        if (DialogBoxParamA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(IDD_SETUP), NULL, setup_proc, (LPARAM)&st) != 1)
            return 0;
        settings_save(&st);
    }
    /* la console seriale va nel terminale che ha lanciato il programma */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) { freopen("CONOUT$", "w", stdout); printf("\n"); }

    if (!st.prom[0]) fail("PROM non trovata: usa --prom percorso\\ip24prom.bin");
    const char *prom_path = st.prom, *cdrom_path = st.cdrom[0] ? st.cdrom : NULL;
    const char *disk_path[2] = { st.disk[0][0] ? st.disk[0] : NULL, st.disk[1][0] ? st.disk[1] : NULL };
    const char *nvram_arg = st.nvram[0] ? st.nvram : NULL;
    unsigned ram_mb = st.ram_mb, mips = st.mips;
    int jit = st.jit, gfx = st.gfx;

    long prom_size;
    uint8_t *prom = load_file(prom_path, &prom_size);
    if (!prom) fail("Non riesco a leggere la PROM");
    if (indy_init(&app.m, prom, (uint32_t)prom_size, ram_mb, gfx)) fail("Memoria insufficiente");
    if (mips >= 10 && mips <= 5000) app.m.ps_per_insn = 1000000u / mips;
    if (jit && r4k_jit_init(&app.m.cpu, 0)) fail("Memoria insufficiente per il JIT");
    free(prom);
    app.m.scc.tx = on_tx;

    /* NVRAM: la nostra accanto alla PROM, la prima volta quella di MAME */
    if (nvram_arg) snprintf(app.nvram_path, sizeof app.nvram_path, "%s", nvram_arg);
    else {
        default_nvram(prom_path, app.nvram_path, sizeof app.nvram_path);
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
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(IDI_INDY));
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
