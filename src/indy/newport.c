/*
 * newport.c - Grafica Newport: REX3 a livello di comando, DCB, uscita video
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "indy/newport.h"

/* ---- registri del REX3 (offset nella zona "set"; +0x800 = "go") ---- */
enum {
    R_DRAWMODE1 = 0x000, R_DRAWMODE0 = 0x004, R_LSMODE = 0x008, R_LSPATTERN = 0x00C,
    R_LSPATSAVE = 0x010, R_ZPATTERN = 0x014, R_COLORBACK = 0x018, R_COLORVRAM = 0x01C,
    R_ALPHAREF = 0x020, R_SMASK0X = 0x028, R_SMASK0Y = 0x02C,
    R_XSTART = 0x100, R_YSTART = 0x104, R_XEND = 0x108, R_YEND = 0x10C, R_XSAVE = 0x110,
    R_XYMOVE = 0x114, R_BRESD = 0x118, R_XSTARTF = 0x138, R_YSTARTF = 0x13C,
    R_XENDF = 0x140, R_YENDF = 0x144, R_XSTARTI = 0x148, R_XENDF1 = 0x14C,
    R_XYSTARTI = 0x150, R_XYENDI = 0x154, R_XSTARTENDI = 0x158,
    R_COLORRED = 0x200, R_COLORALPHA = 0x204, R_COLORGRN = 0x208, R_COLORBLUE = 0x20C,
    R_WRMASK = 0x220, R_COLORI = 0x224, R_COLORX = 0x228, R_HOSTRW0 = 0x230,
    R_HOSTRW1 = 0x234, R_DCBMODE = 0x238, R_DCBDATA0 = 0x240, R_DCBDATA1 = 0x244
};
/* registri di configurazione (da 0x1300, "go" da 0x1B00) */
enum {
    C_SMASK1X = 0x00, C_TOPSCAN = 0x20, C_XYWIN = 0x24, C_CLIPMODE = 0x28,
    C_CONFIG = 0x30, C_STATUS = 0x38, C_USTATUS = 0x3C, C_DCBRESET = 0x40
};

/* drawmode0 */
#define DM0_OPMASK   0x00000003
#define DM0_READ     1
#define DM0_DRAW     2
#define DM0_S2S      3
#define DM0_AM(x)    (((x) >> 2) & 7)
#define AM_SPAN      0
#define AM_BLOCK     1
#define DM0_DOSETUP  0x00000020
#define DM0_CHOST    0x00000040
#define DM0_STOPX    0x00000100
#define DM0_STOPY    0x00000200
#define DM0_SK1ST    0x00000400
#define DM0_SKLST    0x00000800
#define DM0_ZPENAB   0x00001000
#define DM0_L32      0x00008000
#define DM0_ZOPQ     0x00010000
#define DM0_SHADE    0x00040000
#define DM0_LRONLY   0x00080000
/* drawmode1 */
#define DM1_PLANES(x) ((x) & 7)
#define DM1_DD(x)    (((x) >> 3) & 3)
#define DM1_DBLSRC   0x00000020
#define DM1_RWPCKD   0x00000080
#define DM1_RWDBL    0x00000400
#define DM1_HD(x)    (((x) >> 8) & 3)
#define DM1_RGBMD    0x00008000
#define DM1_DITHER   0x00010000
#define DM1_FCLR     0x00020000
#define DM1_LO(x)    ((x) >> 28)

#define STAT_VRINT   0x00000020
#define VC2_CONTROL  0x10
#define VC2_CTRL_EVIRQ  0x0001
#define VC2_CTRL_EDISP  0x0002
#define VC2_CTRL_ECURS  0x0010

int np_init(Newport *n, int planes24)
{
    memset(n, 0, sizeof *n);
    n->rgb = calloc(NP_FBW * NP_H, sizeof *n->rgb);
    n->olay = calloc(NP_FBW * NP_H, 1);
    n->pup = calloc(NP_FBW * NP_H, 1);
    n->cid = calloc(NP_FBW * NP_H, 1);
    if (!n->rgb || !n->olay || !n->pup || !n->cid) { np_free(n); return -1; }
    n->planes24 = planes24;
    np_reset(n);
    return 0;
}

void np_free(Newport *n)
{
    free(n->rgb); free(n->olay); free(n->pup); free(n->cid);
    n->rgb = NULL; n->olay = n->pup = n->cid = NULL;
}

void np_reset(Newport *n)
{
    n->drawmode0 = n->drawmode1 = 0;
    n->wrmask = 0xFFFFFF;
    n->xywin = 0x10001000;
    n->topscan = 0x3FF;
    n->clipmode = 0;
    n->vrint = 0;
    n->irq = 0;
    memset(n->vc2_reg, 0, sizeof n->vc2_reg);
    /* revisione della scheda: bit 7 = 8 piani, 6:4 scheda, 2:0 CMAP */
    n->cmap_rev = (uint8_t)((n->planes24 ? 0 : 0x80) | (3 << 4) | 3);
}

/* ---- pixel ---- */

static uint32_t depth_mask(uint32_t dm1)
{
    static const uint32_t m[4] = { 0xF, 0xFF, 0xFFF, 0xFFFFFF };
    return m[DM1_DD(dm1)];
}

static uint32_t logic_op(int op, uint32_t s, uint32_t d)
{
    switch (op) {
    case 0:  return 0;
    case 1:  return s & d;
    case 2:  return s & ~d;
    case 3:  return s;
    case 4:  return ~s & d;
    case 5:  return d;
    case 6:  return s ^ d;
    case 7:  return s | d;
    case 8:  return ~(s | d);
    case 9:  return ~(s ^ d);
    case 10: return ~d;
    case 11: return s | ~d;
    case 12: return ~s;
    case 13: return ~s | d;
    case 14: return ~(s & d);
    default: return 0xFFFFFFFF;
    }
}

/* RGB a 8 bit per canale -> pixel alla profondita' di disegno (1:2:1,
   3:3:2, 4:4:4, 8:8:8, rosso nei bit bassi). Col dither (DM1_DITHER,
   matrice 4x4 ordinata) il resto del troncamento si distribuisce. */
static const uint8_t bayer4[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static uint32_t chan(uint32_t c, int bits, int d)
{
    if (d >= 0 && (c += (uint32_t)d * (256u >> bits) / 16) > 255) c = 255;
    return c >> (8 - bits);
}

static uint32_t pack_rgb(uint32_t dm1, uint32_t r, uint32_t g, uint32_t b, int x, int y)
{
    int d = (dm1 & DM1_DITHER) ? bayer4[y & 3][x & 3] : -1;
    switch (DM1_DD(dm1)) {
    case 0:  return chan(r, 1, d) | chan(g, 2, d) << 1 | chan(b, 1, d) << 3;
    case 1:  return chan(r, 3, d) | chan(g, 3, d) << 3 | chan(b, 2, d) << 6;
    case 2:  return chan(r, 4, d) | chan(g, 4, d) << 4 | chan(b, 4, d) << 8;
    default: return r | g << 8 | b << 16;
    }
}

/* il contrario, per le letture a 32 bit (lrectread) */
static uint32_t unpack_rgb(uint32_t dm1, uint32_t p)
{
    uint32_t r, g, b;
    switch (DM1_DD(dm1)) {
    case 0:  r = (p & 1) * 255; g = ((p >> 1) & 3) * 85; b = ((p >> 3) & 1) * 255; break;
    case 1:  r = (p & 7) * 255 / 7; g = ((p >> 3) & 7) * 255 / 7; b = ((p >> 6) & 3) * 85; break;
    case 2:  r = (p & 15) * 17; g = ((p >> 4) & 15) * 17; b = ((p >> 8) & 15) * 17; break;
    default: return p;
    }
    return r | g << 8 | b << 16;
}

/* colore da disegnare (CI o RGB impacchettato secondo la profondita', senza dither) */
static uint32_t draw_color(const Newport *n)
{
    uint32_t dm1 = n->drawmode1;
    if (dm1 & DM1_FCLR) return n->colorvram & depth_mask(dm1);   /* fast clear */
    if (!(dm1 & DM1_RGBMD)) return (n->colorred >> 11) & 0xFFF & depth_mask(dm1);   /* CI: parte intera del rosso */
    return pack_rgb(dm1 & ~DM1_DITHER, (n->colorred >> 11) & 0xFF, (n->colorgrn >> 11) & 0xFF,
                    (n->colorblue >> 11) & 0xFF, 0, 0);
}

/* il colore del comando va calcolato pixel per pixel (dither) */
static int color_dithered(const Newport *n)
{
    uint32_t dm1 = n->drawmode1;
    return (dm1 & (DM1_RGBMD | DM1_DITHER | DM1_FCLR)) == (DM1_RGBMD | DM1_DITHER) && DM1_DD(dm1) < 3;
}

static uint32_t color_at(const Newport *n, int x, int y)
{
    return pack_rgb(n->drawmode1, (n->colorred >> 11) & 0xFF, (n->colorgrn >> 11) & 0xFF,
                    (n->colorblue >> 11) & 0xFF, x, y);
}

/* Gouraud (drawmode0 SHADE): i registri di colore avanzano della pendenza
   (0x210 rosso, 0x218 verde, 0x21C blu) a ogni pixel. GL le scrive come
   float con bias: bit 31 segno, bit bassi modulo (-4096.0 = pendenza 0);
   la rilettura invece e' quella che controlla il test della PROM. Il canale e' nei bit 19:11; sopra ci sono esponente e bias
   dei float che GL ci scrive, non un segno. Si limita cosi': 0-255 il
   valore, 256-383 sforato in alto (255), 384-511 piccolo negativo (0). */
static uint32_t chan_clamp(uint32_t reg)
{
    uint32_t f = (reg >> 11) & 0x1FF;
    return f < 256 ? f : f < 384 ? 255 : 0;
}

static uint32_t shade_color(const Newport *n, int x, int y)
{
    uint32_t dm1 = n->drawmode1;
    if (!(dm1 & DM1_RGBMD)) return (n->colorred >> 11) & 0xFFF & depth_mask(dm1);
    return pack_rgb(dm1, chan_clamp(n->colorred), chan_clamp(n->colorgrn), chan_clamp(n->colorblue), x, y);
}

static void shade_step(Newport *n)
{
    n->colorred = (n->colorred + (uint32_t)n->slope[0]) & 0xFFFFFF;
    n->colorgrn = (n->colorgrn + (uint32_t)n->slope[2]) & 0xFFFFF;
    n->colorblue = (n->colorblue + (uint32_t)n->slope[3]) & 0xFFFFF;
}

static int inside(uint32_t mx, uint32_t my, int x, int y)
{
    int x0 = (int)(mx >> 16), x1 = (int)(mx & 0xFFFF), y0 = (int)(my >> 16), y1 = (int)(my & 0xFFFF);
    return x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

/* coordinate di finestra -> schermo, clipping */
static int to_screen(const Newport *n, int wx, int wy, int *sx, int *sy)
{
    *sx = wx + (int)((n->xywin >> 16) & 0xFFFF) - 0x1000;
    *sy = wy + (int)(n->xywin & 0xFFFF) - 0x1000;
    if (*sy >= NP_H && *sy < 2 * NP_H) *sy -= NP_H;               /* le righe girano a 1024 */
    if (*sx < 0 || *sx >= NP_FBW || *sy < 0 || *sy >= NP_H) return 0;
    uint32_t cm = n->clipmode;
    if ((cm & 1) && !inside(n->smask0x, n->smask0y, wx, wy)) return 0;
    if (cm & 0x1E) {
        int ok = 0;
        for (int k = 0; k < 4 && !ok; k++)
            if ((cm >> (k + 1)) & 1) ok = inside(n->smask[k][0], n->smask[k][1], *sx + 0x1000, *sy + 0x1000);
        if (!ok) return 0;
    }
    return 1;
}

/* Piani RGB/CI: un pixel a 4, 8 o 12 bit si ripete su tutti i piani
   (doppio buffer: 4+4 bit sulla XL8, 12+12 sulla XL24) e wrmask sceglie
   il buffer; in lettura DBLSRC prende il secondo. */
static uint32_t replicate(uint32_t dm1, uint32_t v)
{
    switch (DM1_DD(dm1)) {
    case 0:  return (v & 0xF) * 0x111111u;
    case 1:  return (v & 0xFF) * 0x010101u;
    case 2:  return (v & 0xFFF) | (v & 0xFFF) << 12;
    default: return v;
    }
}

static void put_pixel(Newport *n, int wx, int wy, uint32_t src)
{
    n->pixels++;
    int sx, sy;
    if (!to_screen(n, wx, wy, &sx, &sy)) return;
    uint32_t dm1 = n->drawmode1, dmask = depth_mask(dm1);
    int idx = sy * NP_FBW + sx;
    uint32_t mask = n->wrmask & dmask;
    switch (DM1_PLANES(dm1)) {
    case 1: case 2: {
        uint32_t d = n->rgb[idx];
        uint32_t v = logic_op((int)DM1_LO(dm1), replicate(dm1, src), d);
        mask = n->wrmask & (n->planes24 ? 0xFFFFFFu : 0xFFu);
        n->rgb[idx] = (d & ~mask) | (v & mask);
        break;
    }
    case 4: {
        uint32_t d = n->olay[idx], v = logic_op((int)DM1_LO(dm1), src, d);
        n->olay[idx] = (uint8_t)((d & ~mask) | (v & mask));
        break;
    }
    /* popup e CID: il valore a 2 bit si ripete su tutto il byte e wrmask
       sceglie la coppia (X usa 0xCC: popup nei bit 3:2 e 7:6) */
    case 5: {
        uint32_t d = n->pup[idx], v = logic_op((int)DM1_LO(dm1), (src & 3) * 0x55u, d);
        n->pup[idx] = (uint8_t)((d & ~mask) | (v & mask));
        break;
    }
    case 6: {
        uint32_t d = n->cid[idx], v = logic_op((int)DM1_LO(dm1), (src & 3) * 0x55u, d);
        n->cid[idx] = (uint8_t)((d & ~mask) | (v & mask));
        break;
    }
    default: break;
    }
}

static uint32_t get_pixel(const Newport *n, int wx, int wy)
{
    int sx = wx + (int)((n->xywin >> 16) & 0xFFFF) - 0x1000;
    int sy = wy + (int)(n->xywin & 0xFFFF) - 0x1000;
    if (sy >= NP_H && sy < 2 * NP_H) sy -= NP_H;
    if (sx < 0 || sx >= NP_FBW || sy < 0 || sy >= NP_H) return 0;
    int idx = sy * NP_FBW + sx;
    uint32_t dm1 = n->drawmode1;
    switch (DM1_PLANES(dm1)) {
    case 4:  return n->olay[idx];
    case 5:  return n->pup[idx];
    case 6:  return n->cid[idx];
    default: {
        static const int shift[4] = { 4, 8, 12, 0 };
        uint32_t p = n->rgb[idx];
        if (DM1_DD(dm1) == 3) return p;
        return (dm1 & DM1_DBLSRC ? p >> shift[DM1_DD(dm1)] : p) & depth_mask(dm1);
    }
    }
}

/* ---- disegno ---- */

/* pixel contenuti in una parola dall'host, dal piu' significativo */
static int host_pixels(const Newport *n, uint32_t w, uint32_t *px)
{
    uint32_t dm1 = n->drawmode1;
    static const int bits[4] = { 4, 8, 12, 32 };
    int b = bits[DM1_HD(dm1)];
    if (!(dm1 & DM1_RWPCKD) || b == 32) {
        px[0] = b == 32 ? w : w >> (32 - b);
        return 1;
    }
    int cnt = 32 / b;
    for (int k = 0; k < cnt; k++) px[k] = (w >> (32 - b * (k + 1))) & ((1u << b) - 1);
    return cnt;
}

/* avanza il cursore del blocco; 1 = finita la riga */
static int step_block(const Newport *n, int *x, int *y, int ydir)
{
    if (*x == n->xend) {
        *x = n->xsave;
        *y += ydir;
        return 1;
    }
    *x += n->xend >= n->xsave ? 1 : -1;
    return 0;
}

enum { GO_FULL, GO_PATTERN, GO_HOST };

static void draw_block(Newport *n, int kind)
{
    uint32_t dm0 = n->drawmode0;
    int am = DM0_AM(dm0);
    int x = n->xstart, y = n->ystart;
    uint32_t fg = draw_color(n), bg = n->colorback & depth_mask(n->drawmode1);
    uint32_t pat = n->zpattern, dm1 = n->drawmode1;
    int dith = color_dithered(n);
    int shade = (dm0 & DM0_SHADE) && !(dm1 & DM1_FCLR) && kind != GO_HOST;
    /* dall'host in RGB a 32 bit (ABGR) verso una profondita' minore: si converte */
    int hconv = kind == GO_HOST && (dm1 & DM1_RGBMD) && DM1_HD(dm1) == 3 && DM1_DD(dm1) < 3;
    uint32_t host[16];
    int nhost = 0;
    if (kind == GO_HOST && n->hpx) {                          /* scrittura parziale */
        nhost = n->nhpx;
        memcpy(host, n->hpx, (size_t)nhost * sizeof *host);
    } else if (kind == GO_HOST) {
        nhost = host_pixels(n, n->hostrw0, host);
        if (n->drawmode1 & DM1_RWDBL) nhost += host_pixels(n, n->hostrw1, host + nhost);   /* 64 bit */
    }
    if ((dm0 & DM0_LRONLY) && n->xend < n->xsave) return;

    /* pattern: L32 = al piu' 32 pixel; senza, con STOPX fino a xend (i caratteri di GL) */
    long limit = kind == GO_HOST ? nhost
               : kind == GO_PATTERN ? ((dm0 & DM0_L32) ? 32 : (dm0 & DM0_STOPX) ? -1 : 1) : -1;
    long count = 0;
    int ydir = n->yend >= y ? 1 : -1;
    for (;;) {
        uint32_t col = kind == GO_HOST ? host[count] : shade ? shade_color(n, x, y)
                     : dith ? color_at(n, x, y) : fg;
        if (hconv) col = pack_rgb(dm1, col & 0xFF, (col >> 8) & 0xFF, (col >> 16) & 0xFF, x, y);
        int draw = 1;
        if (dm0 & DM0_ZPENAB) {
            int bit = kind == GO_PATTERN ? (int)(count & 31) : (int)((x - n->xsave) & 31);
            if (x < n->xsave) bit = (int)((n->xsave - x) & 31);
            if (!((pat >> (31 - bit)) & 1)) { if (dm0 & DM0_ZOPQ) col = bg; else draw = 0; }
        }
        if (draw) put_pixel(n, x, y, col);
        if (shade) shade_step(n);
        count++;
        int row_end = step_block(n, &x, &y, ydir);
        if (limit >= 0 && count >= limit) break;
        if (row_end) {
            /* pattern e dati dall'host: a fine riga il resto della parola si scarta */
            if (kind == GO_PATTERN || kind == GO_HOST) break;
            if (am == AM_SPAN) break;
            if (kind == GO_FULL && !(dm0 & DM0_STOPY)) break;     /* senza STOPY: una riga */
            if ((ydir > 0 && y > n->yend) || (ydir < 0 && y < n->yend)) break;
        }
    }
    n->xstart = x;
    n->ystart = y;
}

static void draw_line(Newport *n)
{
    uint32_t dm0 = n->drawmode0;
    int x0 = n->xstart, y0 = n->ystart, x1 = n->xend, y1 = n->yend;
    /* senza DOSETUP il REX3 non ricalcola la linea dagli estremi: GL
       disegna cosi' i punti (solo xstartf/ystartf), un pixel */
    int setup = (dm0 & DM0_DOSETUP) != 0;
    if (!setup) { x1 = x0; y1 = y0; }
    int dx = abs(x1 - x0), dy = -abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, first = 1, k = 0;
    uint32_t fg = draw_color(n), bg = n->colorback & depth_mask(n->drawmode1);
    int dith = color_dithered(n);
    int shade = (dm0 & DM0_SHADE) && !(n->drawmode1 & DM1_FCLR);
    for (;;) {
        int last = x0 == x1 && y0 == y1;
        int skip = (first && (dm0 & DM0_SK1ST)) || (last && (dm0 & DM0_SKLST));
        if (!skip) {
            uint32_t col = shade ? shade_color(n, x0, y0) : dith ? color_at(n, x0, y0) : fg;
            int draw = 1;
            if (dm0 & DM0_ZPENAB) {
                if (!((n->zpattern >> (31 - (k & 31))) & 1)) { if (dm0 & DM0_ZOPQ) col = bg; else draw = 0; }
            }
            if (draw) put_pixel(n, x0, y0, col);
        }
        if (shade && !skip) shade_step(n);
        k++;
        first = 0;
        if (last) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
    if (setup) {
        n->xstart = n->xsave = n->xend;
        n->ystart = n->yend;
    }
}

static void screen_to_screen(Newport *n)
{
    int mx = (int16_t)(n->xymove >> 16), my = (int16_t)(n->xymove & 0xFFFF);
    int x0 = n->xstart < n->xend ? n->xstart : n->xend, x1 = n->xstart < n->xend ? n->xend : n->xstart;
    int y0 = n->ystart < n->yend ? n->ystart : n->yend, y1 = n->ystart < n->yend ? n->yend : n->ystart;
    if (!(n->drawmode0 & DM0_STOPY)) y1 = y0 = n->ystart;
    int w = x1 - x0 + 1, h = y1 - y0 + 1;
    if (w <= 0 || h <= 0 || (long)w * h > NP_FBW * NP_H) return;
    uint32_t *tmp = malloc((size_t)w * h * sizeof *tmp);
    if (!tmp) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) tmp[y * w + x] = get_pixel(n, x0 + x, y0 + y);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) put_pixel(n, x0 + x + mx, y0 + y + my, tmp[y * w + x]);
    free(tmp);
    n->ystart = n->ystart <= n->yend ? y1 + 1 : y0 - 1;
    n->xstart = n->xsave;
}

/* READ: la lettura di hostrw0 restituisce i pixel successivi */
static uint32_t read_pixels(Newport *n)
{
    uint32_t dm1 = n->drawmode1, w = 0;
    static const int bits[4] = { 4, 8, 12, 32 };
    int b = bits[DM1_HD(dm1)];
    int cnt = (dm1 & DM1_RWPCKD) && b != 32 ? 32 / b : 1;
    int x = n->xstart, y = n->ystart, ydir = n->yend >= y ? 1 : -1;
    for (int k = 0; k < cnt; k++) {
        uint32_t p = get_pixel(n, x, y);
        if (b == 32 && (dm1 & DM1_RGBMD)) p = unpack_rgb(dm1, p);
        if (b == 32) w = p;
        else w |= (p & ((1u << b) - 1)) << (32 - b * (k + 1));
        step_block(n, &x, &y, ydir);
    }
    n->xstart = x; n->ystart = y;
    return w;
}

static void go_body(Newport *n, int kind)
{
    uint32_t dm0 = n->drawmode0;
    int op = (int)(dm0 & DM0_OPMASK), am = DM0_AM(dm0);
    n->draws++;
    n->gen++;
    {
        int slot = -1;
        for (int k = 0; k < 32; k++)
            if (n->prof[k].n && n->prof[k].dm0 == dm0 && n->prof[k].dm1 == n->drawmode1 && n->prof[k].kind == (uint32_t)kind) { slot = k; break; }
            else if (!n->prof[k].n && slot < 0) { slot = k; n->prof[k].dm0 = dm0; n->prof[k].dm1 = n->drawmode1; n->prof[k].kind = (uint32_t)kind; break; }
        if (slot >= 0) { n->prof[slot].n++; n->prof[slot].px -= n->pixels; }
        n->prof_slot = slot;
    }
    if (n->trace > 0 && op && n->draws > n->trace_after) {  /* --nptrace N [--nptrace-after M] */
        n->trace--;
        fprintf(stderr, "[go k%d dm0=%08X dm1=%08X (%d,%d)-(%d,%d) host=%08X ci=%X win=%08X clip=%X mv=%X wm=%X rgb=%X,%X,%X]\n",
                kind, dm0, n->drawmode1, n->xstart, n->ystart, n->xend, n->yend, n->hostrw0,
                n->colori, n->xywin, n->clipmode, n->xymove, n->wrmask, n->colorred, n->colorgrn, n->colorblue);
    }
    if (op == DM0_S2S) { screen_to_screen(n); return; }
    if (op != DM0_DRAW) return;
    if (am == AM_SPAN || am == AM_BLOCK) {
        if (kind == GO_FULL && (dm0 & DM0_CHOST)) return;       /* aspetta i dati dall'host */
        draw_block(n, kind);
    } else {
        draw_line(n);
    }
}

static void go(Newport *n, int kind)
{
    go_body(n, kind);
    if (n->prof_slot >= 0) n->prof[n->prof_slot].px += n->pixels;
}

/* ---- Display Control Bus ---- */

static int dcb_width(const Newport *n)
{
    int w = (int)(n->dcbmode & 3);
    return w ? w : 4;
}

static void dcb_next(Newport *n)
{
    if (n->dcbmode & 0x08) {                                      /* ECINC: CRS successivo */
        uint32_t crs = ((n->dcbmode >> 4) + 1) & 7;
        n->dcbmode = (n->dcbmode & ~0x70u) | (crs << 4);
    }
}

static void dcb_write(Newport *n, uint32_t data, int nb)
{
    n->gen++;                                                     /* palette, modi, cursore */
    int slave = (int)(n->dcbmode >> 7) & 15, crs = (int)(n->dcbmode >> 4) & 7;
    switch (slave) {
    case 0:                                                       /* VC2 */
        if (nb == 4) data >>= 8;                                  /* indice + 16 bit + byte in piu' */
        if (crs == 0) {
            if (nb >= 3) {
                n->vc2_index = (uint8_t)(data >> 16);
                n->vc2_reg[n->vc2_index & 63] = (uint16_t)data;
            } else n->vc2_index = (uint8_t)data;
        } else if (crs == 1) {
            n->vc2_reg[n->vc2_index & 63] = (uint16_t)data;
        } else if (crs == 3) {
            uint16_t a = n->vc2_reg[7];
            n->vc2_ram[a & 0x7FFF] = (uint16_t)data;
            n->vc2_reg[7] = (uint16_t)(a + 1);
        }
        break;
    case 1: case 2: case 3:                                       /* CMAP */
        if (crs == 0) {
            if (nb >= 2) n->cmap_addr = (uint16_t)(data & 0x1FFF);
            else n->cmap_addr = (uint16_t)((n->cmap_addr & 0x1F00) | (data & 0xFF));
        } else if (crs == 1) {
            n->cmap_addr = (uint16_t)((n->cmap_addr & 0xFF) | ((data & 0x1F) << 8));
        } else if (crs == 2) {
            n->cmap[n->cmap_addr & 0x1FFF] = data & 0xFFFFFF;
            n->cmap_addr = (uint16_t)((n->cmap_addr + 1) & 0x1FFF);
        }
        break;
    case 4: case 5: case 6:                                       /* XMAP9 */
        switch (crs) {
        case 0: n->xmap_config = (uint8_t)data; break;
        case 3: n->xmap_curs_msb = (uint8_t)data; break;
        case 4: n->xmap_pup_msb = (uint8_t)data; break;
        case 5:
            if (nb == 4) n->xmap_mode[(data >> 24) & 31] = data & 0xFFFFFF;
            else n->xmap_mode[n->xmap_mode_index & 31] = data & 0xFFFFFF;
            break;
        case 7: n->xmap_mode_index = (uint8_t)data; break;
        default: break;
        }
        break;
    case 7:                                                       /* BT445 */
        if (crs == 0) n->bt_addr = (uint8_t)data;
        else { n->bt_reg[(uint8_t)(crs * 32 + n->bt_addr)] = (uint8_t)data; }
        break;
    default:
        break;
    }
    dcb_next(n);
}

static uint32_t dcb_read(Newport *n, int nb)
{
    int slave = (int)(n->dcbmode >> 7) & 15, crs = (int)(n->dcbmode >> 4) & 7;
    uint32_t v = 0;
    (void)nb;
    switch (slave) {
    case 0:
        if (crs == 0) v = n->vc2_index;
        else if (crs == 1) {
            if (n->vc2_index == 0x20) v = 1u << 5;                /* configurazione: rev. B */
            else v = n->vc2_reg[n->vc2_index & 63];
        } else if (crs == 3) {
            uint16_t a = n->vc2_reg[7];
            v = n->vc2_ram[a & 0x7FFF];
            n->vc2_reg[7] = (uint16_t)(a + 1);
        }
        break;
    case 1: case 2: case 3:
        if (crs == 0) v = n->cmap_addr & 0xFF;
        else if (crs == 1) v = n->cmap_addr >> 8;
        else if (crs == 2) { v = n->cmap[n->cmap_addr & 0x1FFF]; n->cmap_addr = (uint16_t)((n->cmap_addr + 1) & 0x1FFF); }
        else if (crs == 4) v = 0x08;                              /* stato: pronto */
        else if (crs == 6) v = n->cmap_rev;
        break;
    case 4: case 5: case 6:
        switch (crs) {
        case 0: v = n->xmap_config; break;
        case 1: v = 1; break;                                     /* revisione */
        case 2: v = 2; break;                                     /* FIFO vuota */
        case 3: v = n->xmap_curs_msb; break;
        case 4: v = n->xmap_pup_msb; break;
        case 5: v = n->xmap_mode[n->xmap_mode_index & 31]; break;
        case 7: v = n->xmap_mode_index; break;
        default: break;
        }
        break;
    case 7:
        if (crs == 0) v = n->bt_addr;
        else if (crs == 2) v = 0xA0;                              /* revisione A */
        else v = n->bt_reg[(uint8_t)(crs * 32 + n->bt_addr)];
        break;
    default:
        break;
    }
    dcb_next(n);
    return v;
}

/* ---- accesso ai registri ---- */

/* coordinate: virgola fissa con 4 bit di frazione nei bit 26:7, intero nei 26:11 */
#define FX_MASK 0x07FFFF80u
#define FXF_MASK 0x007FFF80u
static int32_t fx_to_int(uint32_t v) { return (int32_t)(int16_t)((v >> 11) & 0xFFFF); }
static uint32_t int_to_fx(int32_t v) { return ((uint32_t)v & 0xFFFF) << 11; }

/* il disegno lavora sugli interi: prima di leggere si riallinea la virgola fissa */
static void sync_fx(Newport *n)
{
    if (fx_to_int(n->fxs) != n->xstart) n->fxs = int_to_fx(n->xstart);
    if (fx_to_int(n->fys) != n->ystart) n->fys = int_to_fx(n->ystart);
    if (fx_to_int(n->fxe) != n->xend) n->fxe = int_to_fx(n->xend);
    if (fx_to_int(n->fye) != n->yend) n->fye = int_to_fx(n->yend);
}

static uint32_t rex_read(Newport *n, uint32_t off)
{
    switch (off) {
    case R_DRAWMODE1: return n->drawmode1;
    case R_DRAWMODE0: return n->drawmode0;
    case R_LSMODE:    return n->lsmode;
    case R_LSPATTERN: return n->lspattern;
    case R_LSPATSAVE: return n->lspatsave;
    case R_ZPATTERN:  return n->zpattern;
    case R_COLORBACK: return n->colorback;
    case R_COLORVRAM: return n->colorvram;
    case R_ALPHAREF:  return n->alpharef;
    case R_SMASK0X:   return n->smask0x;
    case R_SMASK0Y:   return n->smask0y;
    case R_XSTART:    sync_fx(n); return n->fxs;
    case R_YSTART:    sync_fx(n); return n->fys;
    case R_XEND:      sync_fx(n); return n->fxe;
    case R_YEND:      sync_fx(n); return n->fye;
    case R_XSAVE:     return (uint32_t)n->xsave & 0xFFFF;
    case R_XYMOVE:    return n->xymove;
    case R_COLORRED:  return n->colorred;
    case R_COLORALPHA: return n->coloralpha;
    case R_COLORGRN:  return n->colorgrn;
    case R_COLORBLUE: return n->colorblue;
    case R_WRMASK:    return n->wrmask;
    case R_COLORI:    return n->colori;
    case R_HOSTRW0:
        if ((n->drawmode0 & DM0_OPMASK) == DM0_READ) {
            uint32_t w = read_pixels(n);
            if (n->drawmode1 & DM1_RWDBL) n->hostrw1 = read_pixels(n);  /* 64 bit: la seconda parola */
            return w;
        }
        return n->hostrw0;
    case R_HOSTRW1:   return n->hostrw1;
    case R_DCBMODE:   return n->dcbmode;
    default:          return n->misc[(off >> 2) & 0x1FF];
    }
}

static void rex_write(Newport *n, uint32_t off, uint32_t v, int is_go)
{
    int kind = GO_FULL;
    switch (off) {
    case R_DRAWMODE1: n->drawmode1 = v; break;
    case R_DRAWMODE0: n->drawmode0 = v; break;
    case R_LSMODE:    n->lsmode = v & 0x0FFFFFFF; break;
    case R_LSPATTERN: n->lspattern = v; break;
    case R_LSPATSAVE: n->lspatsave = v; break;
    case R_ZPATTERN:  n->zpattern = v; kind = GO_PATTERN; break;
    case R_COLORBACK: n->colorback = v; break;
    case R_COLORVRAM: n->colorvram = v; break;
    case R_ALPHAREF:  n->alpharef = v & 0xFF; break;
    case R_SMASK0X:   n->smask0x = v; break;
    case R_SMASK0Y:   n->smask0y = v; break;
    case R_XSTART:  n->fxs = v & FX_MASK; n->xstart = n->xsave = fx_to_int(n->fxs); break;
    case R_YSTART:  n->fys = v & FX_MASK; n->ystart = fx_to_int(n->fys); break;
    case R_XEND:    n->fxe = v & FX_MASK; n->xend = fx_to_int(n->fxe); break;
    case R_YEND:    n->fye = v & FX_MASK; n->yend = fx_to_int(n->fye); break;
    /* registri "F": intero senza segno a 12 bit (22:11). GL ci scrive dei
       float con un 4096 aggiunto: la mantissa e' gia' la virgola fissa */
    case R_XSTARTF: n->fxs = v & FXF_MASK; n->xstart = n->xsave = fx_to_int(n->fxs); break;
    case R_YSTARTF: n->fys = v & FXF_MASK; n->ystart = fx_to_int(n->fys); break;
    case R_XENDF: case R_XENDF1: n->fxe = v & FXF_MASK; n->xend = fx_to_int(n->fxe); break;
    case R_YENDF:   n->fye = v & FXF_MASK; n->yend = fx_to_int(n->fye); break;
    case R_XSAVE:     n->xsave = (int16_t)(v & 0xFFFF); break;
    case R_XYMOVE:    n->xymove = v; break;
    case R_XSTARTI:
        n->xstart = n->xsave = (int16_t)(v & 0xFFFF);
        n->fxs = int_to_fx(n->xstart);
        break;
    case R_XYSTARTI:
        n->xstart = n->xsave = (int16_t)(v >> 16); n->ystart = (int16_t)(v & 0xFFFF);
        n->fxs = int_to_fx(n->xstart); n->fys = int_to_fx(n->ystart);
        break;
    case R_XYENDI:
        n->xend = (int16_t)(v >> 16); n->yend = (int16_t)(v & 0xFFFF);
        n->fxe = int_to_fx(n->xend); n->fye = int_to_fx(n->yend);
        break;
    case R_XSTARTENDI:
        n->xstart = n->xsave = (int16_t)(v >> 16); n->xend = (int16_t)(v & 0xFFFF);
        n->fxs = int_to_fx(n->xstart); n->fxe = int_to_fx(n->xend);
        break;
    case R_COLORRED:  n->colorred = v & 0xFFFFFF; break;
    case R_COLORALPHA: n->coloralpha = v & 0xFFFFF; break;
    case R_COLORGRN:  n->colorgrn = v & 0xFFFFF; break;
    case R_COLORBLUE: n->colorblue = v & 0xFFFFF; break;
    case R_WRMASK:    n->wrmask = v & 0xFFFFFF; break;
    case R_COLORI:
        n->colori = v;
        if (n->drawmode1 & DM1_RGBMD) {
            n->colorred = (v & 0xFF) << 11;
            n->colorgrn = ((v >> 8) & 0xFF) << 11;
            n->colorblue = ((v >> 16) & 0xFF) << 11;
        } else n->colorred = (v & 0xFFF) << 11;                  /* CI: l'indice va nel rosso */
        break;
    case R_HOSTRW0:      /* in modo doppio (RWDBL) disegna hostrw0 e poi hostrw1, gia' scritto */
        n->hostrw0 = v;
        kind = GO_HOST;
        break;
    case R_HOSTRW1:
        n->hostrw1 = v;
        is_go = 0;
        break;
    case R_DCBMODE:   n->dcbmode = v; break;
    default:
        if (off >= 0x210 && off <= 0x21C) {                      /* pendenze: segno e modulo */
            uint32_t bits = off == 0x210 ? 23 : 19, mag = (1u << bits) - 1;
            n->slope[(off - 0x210) >> 2] = (int32_t)(v & mag) * ((v & 0x80000000u) ? -1 : 1);   /* GL: segno e modulo */
            v = (v & 0x80000000u) ? (1u << bits) | ((0u - v) & mag) : v & mag;          /* rilettura (test PROM) */
        }
        if (off >= R_BRESD && off < R_BRESD + 24) {             /* Bresenham: larghezze del chip */
            static const uint32_t bm[6] = { 0x07FFFFFF, 0x0001FFFF, 0x070FFFFF, 0xFF1FFFFF, 0x0000FFFF, 0x03FFFFFF };
            v &= bm[(off - R_BRESD) >> 2];
            n->bres[(off - R_BRESD) >> 2] = v;
        }
        n->misc[(off >> 2) & 0x1FF] = v;
        break;
    }
    if (is_go) go(n, kind);
}

static uint32_t creg_read(Newport *n, uint32_t off)
{
    switch (off) {
    case C_TOPSCAN:  return n->topscan;
    case C_XYWIN:    return n->xywin;
    case C_CLIPMODE: return n->clipmode;
    case C_CONFIG:   return n->config;
    case C_STATUS: {
        uint32_t v = 2 | (n->vrint ? STAT_VRINT : 0);             /* REX3 revisione C */
        n->vrint = 0;
        n->irq = 0;
        return v;
    }
    case C_USTATUS:  return 2 | (n->vrint ? STAT_VRINT : 0);
    default:
        if (off < 0x20) return n->smask[off >> 3][(off >> 2) & 1];
        return 0;
    }
}

static void creg_write(Newport *n, uint32_t off, uint32_t v)
{
    n->gen++;
    switch (off) {
    case C_TOPSCAN:  n->topscan = v & 0x3FF; break;
    case C_XYWIN:    n->xywin = v; break;
    case C_CLIPMODE: n->clipmode = v & 0x1FFF; break;
    case C_CONFIG:   n->config = v; break;
    case C_DCBRESET: break;
    default:
        if (off < 0x20) n->smask[off >> 3][(off >> 2) & 1] = v;
        break;
    }
}

uint64_t np_read(Newport *n, uint32_t off, int size)
{
    off &= 0xFFFF;
    if (size == 8) return ((uint64_t)np_read(n, off, 4) << 32) | np_read(n, off + 4, 4);
    uint32_t base = off & ~3u, w;
    if ((base & ~0x800u) == R_DCBDATA0) {
        uint32_t d = dcb_read(n, dcb_width(n));
        return size == 4 ? d : d & ((1u << (8 * size)) - 1);
    }
    if (off >= 0x1300) w = creg_read(n, (off - 0x1300) & 0x7FF & ~3u);
    else w = rex_read(n, base & 0x7FF);
    int sh = 8 * (int)(4 - size - (off & 3));
    return size == 4 ? w : (w >> sh) & ((1u << (8 * size)) - 1);
}

/* Scrittura parziale nella zona go di hostrw0/1 (il VDMA con righe che non
   cominciano o finiscono sui 64 bit, es. X che ridisegna lo sfondo): si
   aggiornano solo quei byte e si disegnano solo i pixel che contengono. */
static void host_partial(Newport *n, int lane, int size, uint64_t val)
{
    uint32_t dm1 = n->drawmode1, px[8];
    int np = 0;
    for (int i = 0; i < size; i++) {
        uint32_t b = (uint32_t)(val >> (8 * (size - 1 - i))) & 0xFF;
        int l = lane + i, sh = 8 * (3 - (l & 3));
        uint32_t *reg = l < 4 ? &n->hostrw0 : &n->hostrw1;
        *reg = (*reg & ~(0xFFu << sh)) | b << sh;
        if (DM1_HD(dm1) == 1) px[np++] = b;                      /* 8 bit: un pixel per byte */
        else if (DM1_HD(dm1) == 0) { px[np++] = b >> 4; px[np++] = b & 15; }
    }
    if (DM1_HD(dm1) == 3 && size == 4) px[np++] = (uint32_t)val;
    if (!(dm1 & DM1_RWPCKD) || !np) return;                      /* 12 bit o non impacchettati: non visti */
    n->hpx = px;
    n->nhpx = np;
    go(n, GO_HOST);
    n->hpx = NULL;
}

void np_write(Newport *n, uint32_t off, int size, uint64_t val)
{
    off &= 0xFFFF;
    if ((off & 0x7F8) == R_HOSTRW0)                   /* statistiche: come arrivano i dati */
        n->hoststat[(off >> 11) & 1][(off >> 2) & 1][size == 8]++;
    if ((off & 0xFF8) == 0x800 + R_HOSTRW0) {        /* dati dall'host con go */
        if (size == 8 && !(off & 7)) {                /* hostrw0 e hostrw1, un comando */
            rex_write(n, R_HOSTRW1, (uint32_t)val, 0);
            rex_write(n, R_HOSTRW0, (uint32_t)(val >> 32), 1);
            return;
        }
        if (size != 4 || (off & 7)) { host_partial(n, (int)(off & 7), size, val); return; }
    }
    if (size == 8) {             /* due registri, ma un solo go: dopo il secondo */
        np_write(n, off & ~0x800u, 4, val >> 32);
        np_write(n, off + 4, 4, (uint32_t)val);
        return;
    }
    uint32_t base = off & ~3u, v = (uint32_t)val;
    if ((base & ~0x800u) == R_DCBDATA0) {
        int nb = dcb_width(n);
        uint32_t d = size > nb ? v >> (8 * (size - nb)) : v;
        dcb_write(n, d, nb);
        return;
    }
    if (off >= 0x1300) {
        uint32_t o = (off - 0x1300) & 0x7FF & ~3u;
        creg_write(n, o, v);
        return;
    }
    if (size != 4) {                                   /* scritture parziali: si fondono */
        uint32_t old = rex_read(n, base & 0x7FF);
        int sh = 8 * (int)(4 - size - (off & 3));
        uint32_t mask = ((1u << (8 * size)) - 1) << sh;
        v = (old & ~mask) | ((v << sh) & mask);
    }
    rex_write(n, base & 0x7FF, v, (off & 0x800) != 0);
}

void np_vblank(Newport *n)
{
    n->vrint = 1;
    if (n->vc2_reg[VC2_CONTROL] & VC2_CTRL_EVIRQ) n->irq = 1;
}

/* ---- uscita video ---- */

/* Modo XMAP: bit 0 = buffer mostrato nel doppio buffer (0 = nibble o
   meta' bassa, 1 = alta), 7:3 banco della CMAP, 9:8 CI/RGB, 11:10
   profondita' (4, 8, 12, 24 bit). */
static uint32_t buffer_shift(const Newport *n, uint32_t mode)
{
    if (!(mode & 1)) return 0;
    switch ((mode >> 10) & 3) {
    case 0:  return 4;
    case 1:  return n->planes24 ? 8 : 0;
    case 2:  return 12;
    default: return 0;
    }
}

static uint32_t expand(const Newport *n, uint32_t pix, uint32_t mode)
{
    uint32_t msb = (mode >> 3) & 0x1F;
    pix >>= buffer_shift(n, mode);
    switch ((mode >> 10) & 3) {                                  /* profondita': bit 11:10 */
    case 0:
        if ((mode >> 8) & 3) {                                   /* RGB 1:2:1 */
            uint32_t r = pix & 1, g = (pix >> 1) & 3, b = (pix >> 3) & 1;
            return (r * 255) << 16 | (g * 85) << 8 | b * 255;
        }
        return n->cmap[((msb << 8) | (pix & 0xF)) & 0x1FFF];
    case 1:
        if ((mode >> 8) & 3) {                                   /* bit 9:8 = modo: RGB 3:3:2 */
            uint32_t r = pix & 7, g = (pix >> 3) & 7, b = (pix >> 6) & 3;
            return ((r * 255 / 7) << 16) | ((g * 255 / 7) << 8) | (b * 85);
        }
        return n->cmap[((msb << 8) | (pix & 0xFF)) & 0x1FFF];
    case 2:  return n->cmap[pix & 0xFFF];
    default: {
        uint32_t r = pix & 0xFF, g = (pix >> 8) & 0xFF, b = (pix >> 16) & 0xFF;
        return (r << 16) | (g << 8) | b;
    }
    }
}

static uint32_t cmap_rgb(uint32_t c)                            /* cmap: R nei bit alti */
{
    return ((c >> 16) & 0xFF) << 16 | ((c >> 8) & 0xFF) << 8 | (c & 0xFF);
}

void np_render(const Newport *n, uint32_t *out)
{
    if (!(n->vc2_reg[VC2_CONTROL] & VC2_CTRL_EDISP)) {
        memset(out, 0, NP_W * NP_H * sizeof *out);
        return;
    }
    /* DID: il registro 5 del VC2 punta a una tabella con un puntatore per
       riga; ogni riga e' una lista di (x << 5) | DID finita da 0xFFFF,
       con x in colonne del framebuffer. Il DID sceglie il modo XMAP. */
    uint16_t did_table = n->vc2_reg[5];
    for (int y = 0; y < NP_H; y++) {
        int fy = (int)((y + n->topscan + 1) & 0x3FF);
        const uint32_t *src = n->rgb + fy * NP_FBW;
        const uint8_t *pup = n->pup + fy * NP_FBW;
        uint32_t *dst = out + y * NP_W;
        uint16_t list = n->vc2_ram[(did_table + y) & 0x7FFF];
        uint16_t e = did_table ? n->vc2_ram[list & 0x7FFF] : 0xFFFF;
        uint32_t mode = n->xmap_mode[0];
        int k = 0, fx = NP_X0;
        while (fx < NP_X0 + NP_W) {
            /* tratto [fx, end) con lo stesso modo */
            int end = NP_X0 + NP_W;
            while (did_table && k < 64 && e != 0xFFFF) {
                if ((int)(e >> 5) > fx) { if ((int)(e >> 5) < end) end = e >> 5; break; }
                mode = n->xmap_mode[e & 31];
                e = n->vc2_ram[(list + ++k) & 0x7FFF];
            }
            if (((mode >> 10) & 3) == 1 && !((mode >> 8) & 3) && !buffer_shift(n, mode)) {  /* CI a 8 bit: banco diretto */
                const uint32_t *bank = n->cmap + ((((mode >> 3) & 0x1F) << 8) & 0x1FFF);
                for (; fx < end; fx++)
                    dst[fx - NP_X0] = pup[fx] & 0x0C ? n->cmap[((uint32_t)n->xmap_pup_msb << 5 | (pup[fx] >> 2 & 3)) & 0x1FFF]
                                              : bank[src[fx] & 0xFF];
            } else {
                for (; fx < end; fx++) {
                    uint32_t c = pup[fx] & 0x0C ? n->cmap[((uint32_t)n->xmap_pup_msb << 5 | (pup[fx] >> 2 & 3)) & 0x1FFF]
                                         : expand(n, src[fx], mode);
                    dst[fx - NP_X0] = cmap_rgb(c);
                }
            }
        }
    }
    /* cursore del VC2: 32x32 a 2 bit, due piani uno dopo l'altro */
    if (n->vc2_reg[VC2_CONTROL] & VC2_CTRL_ECURS) {
        int cx = (int)n->vc2_reg[2] - 31, cy = (int)n->vc2_reg[3] - 31;
        uint16_t base = n->vc2_reg[1];
        for (int r = 0; r < 32; r++) {
            uint32_t p0 = (uint32_t)n->vc2_ram[(base + r * 2) & 0x7FFF] << 16 | n->vc2_ram[(base + r * 2 + 1) & 0x7FFF];
            uint32_t p1 = (uint32_t)n->vc2_ram[(base + 64 + r * 2) & 0x7FFF] << 16 | n->vc2_ram[(base + 64 + r * 2 + 1) & 0x7FFF];
            for (int k = 0; k < 32; k++) {
                int v = (int)((p0 >> (31 - k)) & 1) | (int)(((p1 >> (31 - k)) & 1) << 1);
                int X = cx + k, Y = cy + r;
                if (!v || X < 0 || X >= NP_W || Y < 0 || Y >= NP_H) continue;
                out[Y * NP_W + X] = cmap_rgb(n->cmap[((uint32_t)n->xmap_curs_msb << 5 | (uint32_t)v) & 0x1FFF]);
            }
        }
    }
}
