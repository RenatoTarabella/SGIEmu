/*
 * newport.h - Grafica Newport (XL8/XL24) dell'Indy: REX3, VC2, CMAP, XMAP9
 *
 * Il REX3 disegna nel framebuffer (1280x1024) quando si scrive nella zona
 * "go" dei registri (+0x800): blocchi, righe con pattern (testo), pixel
 * dall'host, linee, copie schermo-schermo, letture. Gli altri chip stanno
 * dietro il Display Control Bus (dcbmode/dcbdata0): VC2 (temporizzazione,
 * cursore), CMAP (tavolozza), XMAP9 (modo dei pixel), BT445 (RAMDAC).
 *
 * Qui si emula il risultato dei comandi, non la pipeline del chip:
 * nessun tempo di esecuzione, GBUSY sempre a zero.
 */
#ifndef NEWPORT_H
#define NEWPORT_H

#include <stdint.h>

#define NP_W 1280                /* area visibile */
#define NP_FBW 1344              /* larghezza del framebuffer del REX3 */
#define NP_X0 8                  /* prima colonna visibile (la PROM usa xywin 0x1008) */
#define NP_H 1024

typedef struct Newport {
    /* REX3: registri di disegno */
    uint32_t drawmode0, drawmode1, lsmode, lspattern, lspatsave, zpattern;
    uint32_t colorback, colorvram, alpharef, smask0x, smask0y;
    int32_t  xstart, ystart, xend, yend, xsave;     /* interi, coordinate di finestra */
    uint32_t fxs, fys, fxe, fye;                    /* gli stessi in virgola fissa (bit 26:7) */
    uint32_t xymove, wrmask, colori, hostrw0, hostrw1;
    uint32_t colorred, coloralpha, colorgrn, colorblue;
    uint32_t bres[8];
    uint32_t dcbmode, dcbdata0, dcbdata1;
    uint32_t misc[0x200];                   /* registri senza effetti, per rileggerli */
    /* registri di configurazione */
    uint32_t smask[4][2], topscan, xywin, clipmode, config;
    int      vrint;                         /* retrace verticale avvenuto */

    /* VC2 */
    uint16_t vc2_reg[32];
    uint16_t vc2_ram[32768];
    uint8_t  vc2_index;
    /* CMAP (le due copie sono uguali) */
    uint32_t cmap[8192];
    uint16_t cmap_addr;
    uint8_t  cmap_rev;                      /* scheda, piani, revisione */
    /* XMAP9 */
    uint8_t  xmap_config, xmap_curs_msb, xmap_pup_msb, xmap_mode_index;
    uint32_t xmap_mode[32];
    /* BT445 */
    uint8_t  bt_addr, bt_reg[256];

    /* framebuffer: CI/RGB (24 bit usati), overlay, popup, CID */
    uint32_t *rgb;
    uint8_t  *olay, *pup, *cid;
    int      planes24;                      /* 1 = XL24, 0 = XL8 */

    uint64_t draws;                         /* comandi eseguiti */
    uint64_t gen;                           /* cambia a ogni modifica di cio' che si vede */
    int      irq;                           /* linea di interrupt (retrace) */
    long     trace;                         /* comandi di disegno da stampare */
    struct { uint32_t dm0, dm1, kind; uint64_t n, px; } prof[32];   /* profilo dei comandi */
    uint64_t pixels;
    int      prof_slot;
    uint64_t trace_after;                   /* traccia solo dopo tanti comandi */
    uint64_t hoststat[2][2][2];             /* scritture di hostrw: [go][rw1][64 bit] */
} Newport;

int      np_init(Newport *n, int planes24);
void     np_free(Newport *n);
void     np_reset(Newport *n);
/* accesso ai registri: offset dalla base 0x1F0F0000 */
uint64_t np_read(Newport *n, uint32_t off, int size);
void     np_write(Newport *n, uint32_t off, int size, uint64_t val);
/* retrace verticale: da chiamare 60 volte al secondo emulato */
void     np_vblank(Newport *n);
/* immagine visibile, 0x00RRGGBB, NP_W x NP_H */
void     np_render(const Newport *n, uint32_t *out);

#endif
