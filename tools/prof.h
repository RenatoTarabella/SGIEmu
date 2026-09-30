/*
 * prof.h - Profiler a campionamento per indy_boot (--prof)
 */
#ifndef PROF_H
#define PROF_H

#include "cpu/r4k.h"

int  prof_start(R4k *c);        /* campiona il thread che la chiama */
void prof_stop_print(void);

#endif
