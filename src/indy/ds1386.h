/*
 * ds1386.h - Orologio Dallas DS1386-8K con 8 KB di RAM a batteria
 *
 * Registri 0x00-0x0D: orologio in BCD (l'anno conta dal 1940, lo YRREF
 * di IRIX e della PROM); il resto e' NVRAM, dove la PROM dell'Indy tiene le
 * variabili d'ambiente. L'ora avanza col tempo emulato a partire da
 * quella dell'host al momento dell'accensione.
 */
#ifndef DS1386_H
#define DS1386_H

#include <stdint.h>

#define DS1386_SIZE 8192
#define NVRAM_ENET  0x13A        /* indirizzo Ethernet: base NVRAM 0x40 + NVOFF_ENET 250 */

typedef struct Ds1386 {
    uint8_t  ram[DS1386_SIZE];
    int64_t  base_s;          /* secondi (epoch Unix) al tempo emulato 0 */
    int      dirty;           /* NVRAM cambiata: da salvare */
} Ds1386;

void    rtc_init(Ds1386 *r, int64_t host_time);
uint8_t rtc_read(Ds1386 *r, int reg, uint64_t now_ns);
void    rtc_write(Ds1386 *r, int reg, uint8_t v, uint64_t now_ns);

#endif
