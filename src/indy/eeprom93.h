/*
 * eeprom93.h - EEPROM seriale 93CS56 (128 parole da 16 bit, Microwire)
 *
 * Si pilota a bit: a ogni scrittura del registro di controllo si passano i
 * livelli di CS, CLK e DI; sul fronte di salita di CLK la EEPROM campiona
 * DI e sposta fuori DO. Comando: bit di start, 2 bit di codice, 8 bit di
 * indirizzo (il piu' alto non conta).
 */
#ifndef EEPROM93_H
#define EEPROM93_H

#include <stdint.h>

typedef struct Eeprom93 {
    uint16_t data[128];
    int      cs, clk, dout;
    int      state;            /* 0 attesa start, 1 comando, 2 lettura, 3 dati da scrivere */
    uint32_t shift;
    int      nbits;
    int      addr, op;
    int      wen;              /* scrittura abilitata (EWEN) */
    int      dirty;
} Eeprom93;

void ee_reset(Eeprom93 *e);
void ee_pins(Eeprom93 *e, int cs, int clk, int di);
static inline int ee_dout(const Eeprom93 *e) { return e->dout; }

#endif
