/*
 * kbc8042.h - Controller 8042 di tastiera e mouse PS/2 (nell'IOC)
 *
 * Risponde ai comandi del controller (autotest, byte di comando,
 * abilitazioni) e a quelli di tastiera e mouse (reset, identificazione,
 * set di scan code, LED, velocita', abilitazione del mouse), e fa uscire
 * gli scan code e i pacchetti del mouse che arrivano dal frontend.
 *
 * Il frontend manda i tasti come codici del set 1 (quelli di Windows);
 * qui si convertono nel set scelto dal sistema (2 o 3), o si lasciano
 * nel set 1 se il controller traduce (bit 6 del byte di comando).
 */
#ifndef KBC8042_H
#define KBC8042_H

#include <stdint.h>

#define KBC_Q 512

typedef struct Kbc8042 {
    uint8_t q[KBC_Q];            /* uscita: byte */
    uint8_t qaux[KBC_Q];         /* 1 se il byte viene dal mouse */
    int     head, tail;
    uint8_t cmdbyte;
    int     pending;              /* comando del controller che aspetta un dato */
    int     kbd_pending;          /* comando della tastiera che aspetta un dato */
    int     aux_pending;          /* comando del mouse che aspetta un dato */
    int     set;                  /* set di scan code della tastiera: 1, 2 o 3 */
    int     aux_enabled;          /* il mouse manda pacchetti (0xF4) */
    int     kbd_enabled;
} Kbc8042;

void    kbc_reset(Kbc8042 *k);
uint8_t kbc_read(Kbc8042 *k, int status);
void    kbc_write(Kbc8042 *k, int command, uint8_t v);
/* tasto dal frontend: scan code del set 1, ext = prefisso E0 */
void    kbc_key(Kbc8042 *k, int sc1, int ext, int down);
/* movimento del mouse (dy positivo verso l'alto) e tasti: bit 0 sin, 1 des, 2 centro */
void    kbc_mouse(Kbc8042 *k, int dx, int dy, int buttons);
int     kbc_irq(const Kbc8042 *k);
int     kbc_room(const Kbc8042 *k);

#endif
