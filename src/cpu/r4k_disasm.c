/*
 * r4k_disasm.c - Disassemblatore MIPS III (R4400), sintassi come objdump
 */
#include <stdio.h>
#include "cpu/r4k_disasm.h"

const char *const r4k_reg_names[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"
};

const char *const r4k_cp0_names[32] = {
    "Index", "Random", "EntryLo0", "EntryLo1", "Context", "PageMask", "Wired", "$7",
    "BadVAddr", "Count", "EntryHi", "Compare", "Status", "Cause", "EPC", "PRId",
    "Config", "LLAddr", "WatchLo", "WatchHi", "XContext", "$21", "$22", "$23",
    "$24", "$25", "ECC", "CacheErr", "TagLo", "TagHi", "ErrorEPC", "$31"
};

static const char *const special_names[64] = {
    "sll", 0, "srl", "sra", "sllv", 0, "srlv", "srav",
    "jr", "jalr", 0, 0, "syscall", "break", 0, "sync",
    "mfhi", "mthi", "mflo", "mtlo", "dsllv", 0, "dsrlv", "dsrav",
    "mult", "multu", "div", "divu", "dmult", "dmultu", "ddiv", "ddivu",
    "add", "addu", "sub", "subu", "and", "or", "xor", "nor",
    0, 0, "slt", "sltu", "dadd", "daddu", "dsub", "dsubu",
    "tge", "tgeu", "tlt", "tltu", "teq", 0, "tne", 0,
    "dsll", 0, "dsrl", "dsra", "dsll32", 0, "dsrl32", "dsra32"
};

static const char *const regimm_names[32] = {
    "bltz", "bgez", "bltzl", "bgezl", 0, 0, 0, 0,
    "tgei", "tgeiu", "tlti", "tltiu", "teqi", 0, "tnei", 0,
    "bltzal", "bgezal", "bltzall", "bgezall", 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

static const char *const op_names[64] = {
    0, 0, "j", "jal", "beq", "bne", "blez", "bgtz",
    "addi", "addiu", "slti", "sltiu", "andi", "ori", "xori", "lui",
    0, 0, 0, 0, "beql", "bnel", "blezl", "bgtzl",
    "daddi", "daddiu", "ldl", "ldr", 0, 0, 0, 0,
    "lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr", "lwu",
    "sb", "sh", "swl", "sw", "sdl", "sdr", "swr", "cache",
    "ll", "lwc1", "lwc2", 0, "lld", "ldc1", "ldc2", "ld",
    "sc", "swc1", "swc2", 0, "scd", "sdc1", "sdc2", "sd"
};

static const char *const fpu_names[64] = {
    "add", "sub", "mul", "div", "sqrt", "abs", "mov", "neg",
    "round.l", "trunc.l", "ceil.l", "floor.l", "round.w", "trunc.w", "ceil.w", "floor.w",
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    "cvt.s", "cvt.d", 0, 0, "cvt.w", "cvt.l", 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    "c.f", "c.un", "c.eq", "c.ueq", "c.olt", "c.ult", "c.ole", "c.ule",
    "c.sf", "c.ngle", "c.seq", "c.ngl", "c.lt", "c.nge", "c.le", "c.ngt"
};

#define R(x) r4k_reg_names[x]

void r4k_disasm(uint64_t pc, uint32_t insn, char *buf, size_t n)
{
    int op = insn >> 26, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31;
    int rd = (insn >> 11) & 31, sa = (insn >> 6) & 31, fn = insn & 63;
    int16_t imm = (int16_t)insn;
    uint64_t btarget = pc + 4 + ((uint64_t)(int64_t)imm << 2);
    const char *name;

    if (insn == 0) { snprintf(buf, n, "nop"); return; }

    switch (op) {
    case 0x00:
        name = special_names[fn];
        if (!name) break;
        switch (fn) {
        case 0x00: case 0x02: case 0x03: case 0x38: case 0x3A: case 0x3B:
        case 0x3C: case 0x3E: case 0x3F:
            snprintf(buf, n, "%s %s,%s,%d", name, R(rd), R(rt), sa); return;
        case 0x04: case 0x06: case 0x07: case 0x14: case 0x16: case 0x17:
            snprintf(buf, n, "%s %s,%s,%s", name, R(rd), R(rt), R(rs)); return;
        case 0x08: case 0x11: case 0x13:
            snprintf(buf, n, "%s %s", name, R(rs)); return;
        case 0x09:
            if (rd == 31) snprintf(buf, n, "jalr %s", R(rs));
            else snprintf(buf, n, "jalr %s,%s", R(rd), R(rs));
            return;
        case 0x0C: case 0x0D:
            snprintf(buf, n, "%s 0x%x", name, (insn >> 6) & 0xFFFFF); return;
        case 0x0F: snprintf(buf, n, "sync"); return;
        case 0x10: case 0x12: snprintf(buf, n, "%s %s", name, R(rd)); return;
        case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x36:
            snprintf(buf, n, "%s %s,%s", name, R(rs), R(rt)); return;
        default:
            if (fn == 0x25 && rt == 0) { snprintf(buf, n, "move %s,%s", R(rd), R(rs)); return; }
            snprintf(buf, n, "%s %s,%s,%s", name, R(rd), R(rs), R(rt)); return;
        }
    case 0x01:
        name = regimm_names[rt];
        if (!name) break;
        if (rt >= 8 && rt <= 15) snprintf(buf, n, "%s %s,%d", name, R(rs), imm);
        else snprintf(buf, n, "%s %s,0x%llx", name, R(rs), (unsigned long long)btarget);
        return;
    case 0x02: case 0x03:
        snprintf(buf, n, "%s 0x%llx", op_names[op],
                 (unsigned long long)(((pc + 4) & ~0x0FFFFFFFull) | ((insn & 0x03FFFFFF) << 2)));
        return;
    case 0x04: case 0x05: case 0x14: case 0x15:
        if (op == 4 && rs == 0 && rt == 0) { snprintf(buf, n, "b 0x%llx", (unsigned long long)btarget); return; }
        snprintf(buf, n, "%s %s,%s,0x%llx", op_names[op], R(rs), R(rt), (unsigned long long)btarget);
        return;
    case 0x06: case 0x07: case 0x16: case 0x17:
        snprintf(buf, n, "%s %s,0x%llx", op_names[op], R(rs), (unsigned long long)btarget);
        return;
    case 0x0C: case 0x0D: case 0x0E:
        snprintf(buf, n, "%s %s,%s,0x%x", op_names[op], R(rt), R(rs), insn & 0xFFFF); return;
    case 0x0F:
        snprintf(buf, n, "lui %s,0x%x", R(rt), insn & 0xFFFF); return;
    case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x18: case 0x19:
        if (op == 9 && rs == 0) { snprintf(buf, n, "li %s,%d", R(rt), imm); return; }
        snprintf(buf, n, "%s %s,%s,%d", op_names[op], R(rt), R(rs), imm); return;
    case 0x10:
        switch (rs) {
        case 0: snprintf(buf, n, "mfc0 %s,%s", R(rt), r4k_cp0_names[rd]); return;
        case 1: snprintf(buf, n, "dmfc0 %s,%s", R(rt), r4k_cp0_names[rd]); return;
        case 4: snprintf(buf, n, "mtc0 %s,%s", R(rt), r4k_cp0_names[rd]); return;
        case 5: snprintf(buf, n, "dmtc0 %s,%s", R(rt), r4k_cp0_names[rd]); return;
        default:
            if (!(insn & (1u << 25))) break;
            switch (fn) {
            case 0x01: snprintf(buf, n, "tlbr"); return;
            case 0x02: snprintf(buf, n, "tlbwi"); return;
            case 0x06: snprintf(buf, n, "tlbwr"); return;
            case 0x08: snprintf(buf, n, "tlbp"); return;
            case 0x18: snprintf(buf, n, "eret"); return;
            default: break;
            }
        }
        break;
    case 0x11: {
        static const char fmtc[32] = { [16] = 's', [17] = 'd', [20] = 'w', [21] = 'l' };
        switch (rs) {
        case 0: snprintf(buf, n, "mfc1 %s,$f%d", R(rt), rd); return;
        case 1: snprintf(buf, n, "dmfc1 %s,$f%d", R(rt), rd); return;
        case 2: snprintf(buf, n, "cfc1 %s,$%d", R(rt), rd); return;
        case 4: snprintf(buf, n, "mtc1 %s,$f%d", R(rt), rd); return;
        case 5: snprintf(buf, n, "dmtc1 %s,$f%d", R(rt), rd); return;
        case 6: snprintf(buf, n, "ctc1 %s,$%d", R(rt), rd); return;
        case 8: {
            static const char *const bc[4] = { "bc1f", "bc1t", "bc1fl", "bc1tl" };
            snprintf(buf, n, "%s 0x%llx", bc[rt & 3], (unsigned long long)btarget);
            return;
        }
        default:
            if (!fmtc[rs] || !fpu_names[fn]) break;
            if (fn >= 48) snprintf(buf, n, "%s.%c $f%d,$f%d", fpu_names[fn], fmtc[rs], rd, rt);
            else if (fn <= 3) snprintf(buf, n, "%s.%c $f%d,$f%d,$f%d", fpu_names[fn], fmtc[rs], sa, rd, rt);
            else snprintf(buf, n, "%s.%c $f%d,$f%d", fpu_names[fn], fmtc[rs], sa, rd);
            return;
        }
        break;
    }
    case 0x2F:
        snprintf(buf, n, "cache 0x%x,%d(%s)", rt, imm, R(rs)); return;
    case 0x31: case 0x35: case 0x39: case 0x3D:
        snprintf(buf, n, "%s $f%d,%d(%s)", op_names[op], rt, imm, R(rs)); return;
    default:
        if (op >= 0x1A && op_names[op]) {
            snprintf(buf, n, "%s %s,%d(%s)", op_names[op], R(rt), imm, R(rs));
            return;
        }
        break;
    }
    snprintf(buf, n, ".word 0x%08x", insn);
}
