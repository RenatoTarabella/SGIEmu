/*
 * ds1687.h - Orologio Dallas DS1687 dell'Octane (sul bytebus dell'IOC3)
 *
 * Due porte: indice (bytebus 1) e dati (bytebus 2). Registri 0x00-0x0D
 * dell'orologio, poi 114 byte di NVRAM. Con DV0 (bit 4 del registro A) gli
 * indirizzi 0x40-0x7F sono il banco 1: modello e numero di serie, secolo,
 * registri di controllo estesi 4A/4B, 128 byte di RAM estesa (porte 0x50
 * indirizzo, 0x53 dato).
 *
 * L'accensione dell'Octane passa da qui: il pulsante e' il "kickstart" del
 * DS1687. All'accensione KF (bit 0 di 4A) dice "acceso dal pulsante"; PAB
 * (bit 3 di 4A) a 1 spegne la macchina.
 */
#ifndef DS1687_H
#define DS1687_H

#include <stdint.h>

typedef struct Ds1687 {
    uint8_t  reg[128];         /* banco 0: orologio e NVRAM */
    uint8_t  bank1[64];        /* banco 1, registri 0x40-0x7F */
    uint8_t  ext[128];         /* RAM estesa */
    uint8_t  index;
    int64_t  base_s;           /* secondi (epoch Unix) al tempo emulato 0 */
    int      power_off;        /* PAB scritto a 1 */
    int      dirty;            /* NVRAM cambiata */
} Ds1687;

void    ds1687_init(Ds1687 *r, int64_t host_time);
void    ds1687_select(Ds1687 *r, uint8_t index);
uint8_t ds1687_read(Ds1687 *r, uint64_t now_ns);
void    ds1687_write(Ds1687 *r, uint8_t v, uint64_t now_ns);

#endif
