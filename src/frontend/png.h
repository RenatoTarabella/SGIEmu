#ifndef PNG_H
#define PNG_H

#include <stdint.h>

/* Salva pixel &00RRGGBB; yscale ripete le righe (modi con pixel alti). */
int png_write(const char *path, const uint32_t *rgb, int w, int h, int yscale);

#endif
