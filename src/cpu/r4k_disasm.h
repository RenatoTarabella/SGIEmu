/*
 * r4k_disasm.h - Disassemblatore MIPS III (R4400)
 */
#ifndef R4K_DISASM_H
#define R4K_DISASM_H

#include <stddef.h>
#include <stdint.h>

/* Scrive in buf la forma testuale di insn all'indirizzo pc. */
void r4k_disasm(uint64_t pc, uint32_t insn, char *buf, size_t n);

extern const char *const r4k_reg_names[32];
extern const char *const r4k_cp0_names[32];

#endif
