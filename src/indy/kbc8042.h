/*
 * kbc8042.h - Controller 8042 di tastiera e mouse PS/2 (nell'IOC)
 *
 * Risponde ai comandi del controller (autotest, byte di comando,
 * abilitazioni) e a quelli di tastiera e mouse (reset, identificazione,
 * ACK), e fa uscire gli scan code messi in coda dal frontend.
 */
#ifndef KBC8042_H
#define KBC8042_H

#include <stdint.h>

#define KBC_Q 256

typedef struct Kbc8042 {
    uint8_t q[KBC_Q];            /* uscita: byte */
    uint8_t qaux[KBC_Q];         /* 1 se il byte viene dal mouse */
    int     head, tail;
    uint8_t cmdbyte;
    int     pending;              /* comando del controller che aspetta un dato */
} Kbc8042;

void    kbc_reset(Kbc8042 *k);
uint8_t kbc_read(Kbc8042 *k, int status);
void    kbc_write(Kbc8042 *k, int command, uint8_t v);
void    kbc_key(Kbc8042 *k, uint8_t scancode);       /* dal frontend */
int     kbc_irq(const Kbc8042 *k);

#endif
