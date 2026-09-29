/*
 * pit8254.h - Timer Intel 8254 dell'INT3
 *
 * Il contatore 2 conta a 1 MHz; i contatori 0 e 1 contano gli impulsi
 * dell'uscita del 2 (prescaler), come sull'IP22. Il valore dei contatori
 * si calcola dal tempo al momento della lettura: niente eventi per ogni tick.
 */
#ifndef PIT8254_H
#define PIT8254_H

#include <stdint.h>

typedef struct Pit8254Ch {
    uint32_t reload;       /* 1..65536                                  */
    int      mode, rw, bcd;
    int      running;      /* conteggio caricato e partito              */
    int      wlow;         /* rw = 3: il prossimo byte scritto e' il basso */
    int      rlow;         /* rw = 3: il prossimo byte letto e' il basso   */
    uint8_t  low_written;
    int      latched;      /* 0, 1 o 2 byte ancora da leggere dal latch */
    uint16_t latch;
    uint64_t load_tick;    /* tick del clock del canale al caricamento  */
    uint64_t fired;        /* scadenze gia' segnalate                   */
} Pit8254Ch;

typedef struct Pit8254 {
    Pit8254Ch ch[3];
} Pit8254;

void    pit_reset(Pit8254 *p);
uint8_t pit_read(Pit8254 *p, int port, uint64_t now_ns);   /* port 0..3 */
void    pit_write(Pit8254 *p, int port, uint8_t v, uint64_t now_ns);
/* quante volte il canale e' arrivato a zero dall'ultima chiamata */
uint64_t pit_poll(Pit8254 *p, int ch, uint64_t now_ns);
/* ns al prossimo arrivo a zero del canale (UINT64_MAX se fermo) */
uint64_t pit_next_ns(const Pit8254 *p, int ch, uint64_t now_ns);

#endif
