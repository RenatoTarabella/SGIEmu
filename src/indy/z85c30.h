/*
 * z85c30.h - Controller seriale Zilog 85C30 (SCC), due canali
 *
 * A livello di carattere: la trasmissione e' istantanea (il buffer e'
 * sempre vuoto), la ricezione prende i caratteri da una coda. Niente
 * baud rate, niente modem: solo quello che vedono PROM e driver.
 * Canale 0 = A, canale 1 = B.
 */
#ifndef Z85C30_H
#define Z85C30_H

#include <stdint.h>

#define SCC_RXQ 1024

typedef struct Z85c30Ch {
    uint8_t  wr[16];
    uint8_t  rxq[SCC_RXQ];
    int      rx_head, rx_tail;
    int      tx_ip, rx_ip, ext_ip;     /* interrupt pendenti */
} Z85c30Ch;

typedef struct Z85c30 {
    Z85c30Ch ch[2];
    int      ptr[2];                    /* registro puntato, per canale */
    void   (*tx)(void *user, int ch, uint8_t c);
    void    *user;
} Z85c30;

void    scc_reset(Z85c30 *s);
uint8_t scc_read(Z85c30 *s, int ch, int data);          /* data = 1: porta dati */
void    scc_write(Z85c30 *s, int ch, int data, uint8_t v);
void    scc_rx(Z85c30 *s, int ch, uint8_t c);            /* carattere in arrivo */
int     scc_rx_room(const Z85c30 *s, int ch);
int     scc_irq(const Z85c30 *s);                        /* linea di interrupt */

#endif
