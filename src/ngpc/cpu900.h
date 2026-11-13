/* cpu900.h - a TLCS-900/H interpreter (the Neo Geo Pocket CPU core) and the instruction primitives the
 * translated code is built from. Plain C, no platform header.
 *
 * Before including it, a file must have the two memory functions it calls in scope (src/ngpc/ngp.h has them):
 *     uint8_t mem_r8(uint32_t a);              a is already masked to 24 bits
 *     void    mem_w8(uint32_t a, uint8_t v);
 * Define CPU900_EXTERN before including to share one CPU state between files (it is then defined once, in
 * the file that defines CPU900_DEFINE as well).
 *
 * Users: tools/ngpref/ngpref.c (the reference machine), src/ngpc/run_interp.c (the interpreter with the port's
 * frame model, which the translation is compared with tick by tick) and the code tools/recomp900.py generates.
 *
 * Written from the instruction set as documented by Toshiba. The opcode map is the one of tools/tlcs900.py
 * (checked against MAME's tables); the base cycle counts are in cycles900.h. Where the data sheet leaves a flag
 * undefined, one choice is made here and the translation inherits it, because it calls these same functions.
 */
#ifndef CPU900_H
#define CPU900_H
#include <stdint.h>
#include "cycles900.h"
#ifdef __GNUC__
#define CPU900_UNUSED __attribute__((unused))
#else
#define CPU900_UNUSED
#endif

#define F_S 0x80
#define F_Z 0x40
#define F_H 0x10
#define F_V 0x04
#define F_N 0x02
#define F_C 0x01

typedef struct {
    uint32_t bank[4][4];        /* XWA, XBC, XDE, XHL of the four register banks */
    uint32_t gpr[4];            /* XIX, XIY, XIZ, XSP */
    uint32_t *cur;              /* bank[RFP]: the current bank, kept up to date at every change of RFP (CPU_RFP_SET) */
    uint32_t pc;
    uint16_t sr;                /* 15 SYSM, 14-12 IFF, 11 MAX, 10-8 RFP, 7 S, 6 Z, 4 H, 2 V, 1 N, 0 C */
    uint8_t  f2;                /* F' */
    uint8_t  halted;            /* HALT was executed: waiting for an interrupt */
    uint8_t  cr[256];           /* control registers (LDC), as bytes: the micro DMA registers, INTNEST at 0x3C */
    int      bad;               /* the last step met bytes that are not an instruction (pc is left on them) */
    uint32_t dummy;
} Cpu900;
#if defined(CPU900_EXTERN) && !defined(CPU900_DEFINE)
extern Cpu900 cpu;
#elif defined(CPU900_EXTERN)
Cpu900 cpu;
#else
static Cpu900 cpu;
#endif

#define CPU_RFP   ((cpu.sr >> 8) & 3)
#define CPU_RFP_SET() (cpu.cur = cpu.bank[CPU_RFP])
#define CPU_IFF   ((cpu.sr >> 12) & 7)
#define XSP       cpu.gpr[3]
static const uint8_t cpu_code8[8] = { 0xe1, 0xe0, 0xe5, 0xe4, 0xe9, 0xe8, 0xed, 0xec };     /* W A B C D E H L */
static const uint8_t cpu_code16[8] = { 0xe0, 0xe4, 0xe8, 0xec, 0xf0, 0xf4, 0xf8, 0xfc };    /* WA BC DE HL IX IY IZ SP */
static const uint32_t cpu_mask[5] = { 0, 0xff, 0xffff, 0, 0xffffffffu };
static const uint32_t cpu_sign[5] = { 0, 0x80, 0x8000, 0, 0x80000000u };

/* ---- registers by their full 8-bit code ---- */
static uint32_t *cpu_regp(unsigned code)
{
    if (code < 0x40) return &cpu.bank[code >> 4][(code >> 2) & 3];
    if (code >= 0xf0) return &cpu.gpr[(code >> 2) & 3];
    if (code >= 0xe0) return &cpu.bank[CPU_RFP][(code >> 2) & 3];
    if (code >= 0xd0) return &cpu.bank[(CPU_RFP - 1) & 3][(code >> 2) & 3];
    cpu.dummy = 0; return &cpu.dummy;
}
CPU900_UNUSED static uint32_t cpu_getr(int size, unsigned code)
{
    uint32_t v = *cpu_regp(code);
    if (size == 4) return v;
    if (size == 2) return (v >> ((code & 2) * 8)) & 0xffff;
    return (v >> ((code & 3) * 8)) & 0xff;
}
CPU900_UNUSED static void cpu_setr(int size, unsigned code, uint32_t x)
{
    uint32_t *p = cpu_regp(code);
    if (size == 4) *p = x;
    else if (size == 2) { int sh = (code & 2) * 8; *p = (*p & ~(0xffffu << sh)) | ((x & 0xffff) << sh); }
    else { int sh = (code & 3) * 8; *p = (*p & ~(0xffu << sh)) | ((x & 0xff) << sh); }
}
CPU900_UNUSED static unsigned cpu_short(int size, int r) { return size == 1 ? cpu_code8[r & 7] : cpu_code16[r & 7]; }

/* ---- memory ---- */
CPU900_UNUSED static uint32_t cpu_rd(int size, uint32_t a)
{
    uint32_t v = mem_r8(a & 0xffffff);
    if (size > 1) v |= (uint32_t)mem_r8((a + 1) & 0xffffff) << 8;
    if (size > 2) v |= (uint32_t)mem_r8((a + 2) & 0xffffff) << 16;
    if (size > 3) v |= (uint32_t)mem_r8((a + 3) & 0xffffff) << 24;   /* 3: a 24-bit address, not the next opcode too */
    return v;
}
CPU900_UNUSED static void cpu_wr(int size, uint32_t a, uint32_t v)
{
    mem_w8(a & 0xffffff, (uint8_t)v);
    if (size > 1) mem_w8((a + 1) & 0xffffff, (uint8_t)(v >> 8));
    if (size > 2) { mem_w8((a + 2) & 0xffffff, (uint8_t)(v >> 16)); mem_w8((a + 3) & 0xffffff, (uint8_t)(v >> 24)); }
}
CPU900_UNUSED static uint32_t cpu_fetch(int size) { uint32_t v = cpu_rd(size, cpu.pc); cpu.pc += size; return v; }
CPU900_UNUSED static void cpu_push(int size, uint32_t v) { XSP -= size; cpu_wr(size, XSP, v); }
CPU900_UNUSED static uint32_t cpu_pop(int size) { uint32_t v = cpu_rd(size, XSP); XSP += size; return v; }

/* ---- flags ---- */
CPU900_UNUSED static int cpu_parity(int size, uint32_t v)            /* 1 when the number of set bits is even (8 or 16 bits) */
{
    v &= cpu_mask[size == 4 ? 2 : size];
    v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return !(v & 1);
}
CPU900_UNUSED static void cpu_setsz(int size, uint32_t r)
{
    cpu.sr &= ~(F_S | F_Z);
    if (r & cpu_sign[size]) cpu.sr |= F_S;
    if (!(r & cpu_mask[size])) cpu.sr |= F_Z;
}
CPU900_UNUSED static int cpu_cond(int cc)
{
    int s = !!(cpu.sr & F_S), z = !!(cpu.sr & F_Z), v = !!(cpu.sr & F_V), c = cpu.sr & F_C, r;
    switch (cc & 7) {
    case 0: r = 0; break;               /* F  */
    case 1: r = s ^ v; break;           /* LT */
    case 2: r = z | (s ^ v); break;     /* LE */
    case 3: r = c | z; break;           /* ULE */
    case 4: r = v; break;               /* OV */
    case 5: r = s; break;               /* MI */
    case 6: r = z; break;               /* Z  */
    default: r = c; break;              /* C  */
    }
    return (cc & 8) ? !r : r;           /* 8-15: T GE GT UGT NOV PL NZ NC */
}

/* ALU: 0 ADD 1 ADC 2 SUB 3 SBC 4 AND 5 XOR 6 OR 7 CP. Sets the flags, returns the result (CP: the difference). */
CPU900_UNUSED static uint32_t cpu_alu(int op, int size, uint32_t a, uint32_t b)
{
    uint32_t m = cpu_mask[size], sg = cpu_sign[size], r, cin = 0;
    uint16_t f = cpu.sr & ~(F_S | F_Z | F_H | F_V | F_N | F_C);
    a &= m; b &= m;
    switch (op) {
    case 1: cin = cpu.sr & F_C;         /* fall through */
    case 0: {
        uint64_t w = (uint64_t)a + b + cin;
        r = (uint32_t)w & m;
        if (w > m) f |= F_C;
        if (~(a ^ b) & (a ^ r) & sg) f |= F_V;
        if (size < 4) f |= (a ^ b ^ r) & F_H;
        break; }
    case 3: cin = cpu.sr & F_C;         /* fall through */
    case 2: case 7: {
        uint64_t w = (uint64_t)a - b - cin;
        r = (uint32_t)w & m;
        if ((uint64_t)a < (uint64_t)b + cin) f |= F_C;
        if ((a ^ b) & (a ^ r) & sg) f |= F_V;
        if (size < 4) f |= (a ^ b ^ r) & F_H;
        f |= F_N;
        break; }
    case 4: r = a & b; f |= F_H; if (size < 4 && cpu_parity(size, r)) f |= F_V; break;
    case 5: r = a ^ b; if (size < 4 && cpu_parity(size, r)) f |= F_V; break;
    default: r = a | b; if (size < 4 && cpu_parity(size, r)) f |= F_V; break;
    }
    if (r & sg) f |= F_S;
    if (!r) f |= F_Z;
    cpu.sr = f;
    return r;
}
/* INC and DEC of a byte register or of memory: like ADD and SUB, but the carry flag stays */
CPU900_UNUSED static uint32_t cpu_incdec(int dec, int size, uint32_t a, uint32_t n)
{
    uint16_t c = cpu.sr & F_C;
    uint32_t r = cpu_alu(dec ? 2 : 0, size, a, n);
    cpu.sr = (cpu.sr & ~F_C) | c;
    return r;
}
/* shifts and rotates: 0 RLC 1 RRC 2 RL 3 RR 4 SLA 5 SRA 6 SLL 7 SRL, count 1..16 */
CPU900_UNUSED static uint32_t cpu_shift(int op, int size, uint32_t v, int count)
{
    uint32_t m = cpu_mask[size], sg = cpu_sign[size];
    int bits = size * 8, c = cpu.sr & F_C, i;
    v &= m;
    for (i = 0; i < count; i++) {
        int out;
        switch (op) {
        case 0: out = !!(v & sg); v = ((v << 1) | out) & m; break;
        case 1: out = v & 1; v = (v >> 1) | (out ? sg : 0); break;
        case 2: out = !!(v & sg); v = ((v << 1) | c) & m; break;
        case 3: out = v & 1; v = (v >> 1) | (c ? sg : 0); break;
        case 4: case 6: out = !!(v & sg); v = (v << 1) & m; break;
        case 5: out = v & 1; v = (v >> 1) | (v & sg); break;
        default: out = v & 1; v >>= 1; break;
        }
        c = out;
    }
    (void)bits;
    cpu.sr &= ~(F_S | F_Z | F_H | F_V | F_N | F_C);
    if (c) cpu.sr |= F_C;
    if (v & sg) cpu.sr |= F_S;
    if (!v) cpu.sr |= F_Z;
    if (size < 4 && cpu_parity(size, v)) cpu.sr |= F_V;
    return v;
}
/* DIV and DIVS: quotient in the low half, remainder in the high half; V on overflow or division by zero */
CPU900_UNUSED static uint32_t cpu_div(int size, uint32_t val, uint32_t d, int sgn)
{
    int hb = size * 8;                                  /* size = size of the divisor: 1 or 2 */
    uint32_t hm = cpu_mask[size];
    cpu.sr &= ~F_V;
    if (!(d & hm)) { cpu.sr |= F_V; return val; }
    if (!sgn) {
        uint32_t n = size == 1 ? (val & 0xffff) : val, q = n / (d & hm), r = n % (d & hm);
        if (q > hm) { cpu.sr |= F_V; return val; }
        return (q & hm) | (r << hb);
    } else {
        int32_t n = size == 1 ? (int16_t)val : (int32_t)val, dv = size == 1 ? (int8_t)d : (int16_t)d, q, r;
        if (n == INT32_MIN && dv == -1) { cpu.sr |= F_V; return val; }
        q = n / dv; r = n % dv;
        if (q > (int32_t)(hm >> 1) || q < -(int32_t)(hm >> 1) - 1) { cpu.sr |= F_V; return val; }
        return ((uint32_t)q & hm) | (((uint32_t)r & hm) << hb);
    }
}

CPU900_UNUSED static void cpu_set_sr(uint16_t v) { cpu.sr = v | 0x8800; CPU_RFP_SET(); }

/* Take an interrupt or a software interrupt through vector number n (the table is at 0xFFFF00). */
CPU900_UNUSED static void cpu_vector(int n, int level)
{
    cpu_push(4, cpu.pc);
    cpu_push(2, cpu.sr);
    if (level >= 0) { int l = level + 1; if (l > 7) l = 7; cpu.sr = (cpu.sr & ~0x7000) | (l << 12); cpu.cr[0x3c]++; }
    cpu.pc = cpu_rd(4, 0xffff00 + n * 4) & 0xffffff;
    cpu.halted = 0;
}

/* ---- memory operand of a prefix byte: returns the address; *cyc gets the extra states ---- */
CPU900_UNUSED static uint32_t cpu_ea(int first, int *cyc, int *ok)
{
    int low = first & 15;
    uint32_t a;
    if (first < 0xc0) {
        a = *cpu_regp(cpu_code16[first & 7]);
        if (low >= 8) { a += (int8_t)cpu_fetch(1); *cyc += 1; }
        return a;
    }
    switch (low) {
    case 0: *cyc += 1; return cpu_fetch(1);
    case 1: *cyc += 2; return cpu_fetch(2);
    case 2: *cyc += 3; return cpu_fetch(3);
    case 3: {
        unsigned b = cpu_fetch(1);
        if ((b & 3) == 0) { *cyc += 1; return *cpu_regp(b); }
        if ((b & 3) == 1) { *cyc += 3; a = *cpu_regp(b & 0xfc); return a + (int16_t)cpu_fetch(2); }
        if (b == 0x03) { unsigned r = cpu_fetch(1), x = cpu_fetch(1); *cyc += 3; return *cpu_regp(r & 0xfc) + (int8_t)cpu_getr(1, x); }
        if (b == 0x07) { unsigned r = cpu_fetch(1), x = cpu_fetch(1); *cyc += 3; return *cpu_regp(r & 0xfc) + (int16_t)cpu_getr(2, x & 0xfe); }
        if (b == 0x13) { int16_t d = (int16_t)cpu_fetch(2); *cyc += 3; return cpu.pc + d; }
        *ok = 0; return 0; }
    case 4: {                                           /* (-r32): step 1, 2 or 4 */
        unsigned b = cpu_fetch(1); uint32_t *p = cpu_regp(b & 0xfc);
        if ((b & 3) == 3) { *ok = 0; return 0; }
        *cyc += 1; *p -= 1u << (b & 3); return *p; }
    case 5: {                                           /* (r32+) */
        unsigned b = cpu_fetch(1); uint32_t *p = cpu_regp(b & 0xfc);
        if ((b & 3) == 3) { *ok = 0; return 0; }
        *cyc += 1; a = *p; *p += 1u << (b & 3); return a; }
    }
    *ok = 0; return 0;
}

CPU900_UNUSED static int cpu_bit_mem(int kind, uint32_t ea, int bit)  /* 0 ANDCF 1 ORCF 2 XORCF 3 LDCF 4 STCF 5 TSET 6 RES 7 SET 8 CHG 9 BIT */
{
    uint8_t v = mem_r8(ea & 0xffffff), b = (v >> bit) & 1, c = cpu.sr & F_C;
    switch (kind) {
    case 0: c &= b; break;
    case 1: c |= b; break;
    case 2: c ^= b; break;
    case 3: c = b; break;
    case 4: mem_w8(ea & 0xffffff, (uint8_t)((v & ~(1 << bit)) | (c << bit))); return 1;
    case 5: cpu.sr = (cpu.sr & ~(F_Z | F_N)) | F_H | (b ? 0 : F_Z); mem_w8(ea & 0xffffff, v | (1 << bit)); return 1;
    case 6: mem_w8(ea & 0xffffff, v & ~(1 << bit)); return 1;
    case 7: mem_w8(ea & 0xffffff, v | (1 << bit)); return 1;
    case 8: mem_w8(ea & 0xffffff, v ^ (1 << bit)); return 1;
    default: cpu.sr = (cpu.sr & ~(F_Z | F_N)) | F_H | (b ? 0 : F_Z); return 1;
    }
    cpu.sr = (cpu.sr & ~F_C) | c;
    return 1;
}

/* ---- instructions on a register (prefix C7-CF, D7-DF, E7-EF) ---- */
CPU900_UNUSED static int cpu_op_reg(int size, unsigned code, int first, int second, int *cyc)
{
    uint32_t v = cpu_getr(size, code), m = cpu_mask[size], t;
    int R = second & 7, n;
    unsigned rc = cpu_short(size, R);
    switch (second) {
    case 0x03: cpu_setr(size, code, cpu_fetch(size)); return 1;
    case 0x04: cpu_push(size, v); return 1;
    case 0x05: cpu_setr(size, code, cpu_pop(size)); return 1;
    case 0x06: if (size == 4) return 0; cpu_setr(size, code, ~v); cpu.sr |= F_H | F_N; return 1;
    case 0x07: if (size == 4) return 0; cpu_setr(size, code, cpu_alu(2, size, 0, v)); return 1;
    case 0x08: case 0x09: case 0x0a: case 0x0b: {       /* MUL MULS DIV DIVS rr,# */
        unsigned tc; uint32_t imm, big;
        if (size == 4) return 0;
        if (size == 1) { if ((first & 15) != 7 && !(first & 1)) return 0; tc = code & 0xfe; } else tc = code & 0xfc;
        imm = cpu_fetch(size); big = cpu_getr(size * 2, tc);
        if (second == 0x08) big = (big & m) * imm;
        else if (second == 0x09) big = size == 1 ? (uint32_t)((int8_t)big * (int8_t)imm) : (uint32_t)((int16_t)big * (int16_t)imm);
        else big = cpu_div(size, big, imm, second == 0x0b);
        cpu_setr(size * 2, tc, big); return 1; }
    case 0x0c: if (size != 4) return 0; { int16_t d = (int16_t)cpu_fetch(2); cpu_push(4, v); cpu_setr(4, code, XSP); XSP += d; } return 1;
    case 0x0d: if (size != 4) return 0; XSP = v; cpu_setr(4, code, cpu_pop(4)); return 1;
    case 0x0e: case 0x0f:                               /* BS1F, BS1B A,r: number of the first set bit */
        if (size != 2) return 0;
        cpu.sr &= ~F_V;
        if (!v) { cpu.sr |= F_V; return 1; }
        if (second == 0x0e) { for (n = 0; !(v & (1u << n)); n++) ; } else { for (n = 15; !(v & (1u << n)); n--) ; }
        cpu_setr(1, 0xe0, n); return 1;
    case 0x10: {                                        /* DAA */
        uint8_t a = (uint8_t)v, fix = 0; int c = cpu.sr & F_C;
        if (size != 1) return 0;
        if ((cpu.sr & F_H) || (a & 15) > 9) fix |= 0x06;
        if (c || a > 0x99) { fix |= 0x60; c = 1; }
        t = (cpu.sr & F_N) ? (uint8_t)(a - fix) : (uint8_t)(a + fix);
        cpu.sr &= ~(F_S | F_Z | F_H | F_V | F_C);
        cpu.sr |= ((a ^ t) & F_H) | (c ? F_C : 0) | (cpu_parity(1, t) ? F_V : 0);
        cpu_setsz(1, t); cpu_setr(1, code, t); return 1; }
    case 0x12: if (size == 1) return 0; cpu_setr(size, code, size == 2 ? (v & 0xff) : (v & 0xffff)); return 1;
    case 0x13: if (size == 1) return 0; cpu_setr(size, code, size == 2 ? (uint32_t)(int8_t)v : (uint32_t)(int16_t)v); return 1;
    case 0x14: if (size == 1) return 0; if (v & 1) cpu_setr(size, code, v + 1); return 1;
    case 0x16: if (size != 2) return 0; for (t = 0, n = 0; n < 16; n++) if (v & (1u << n)) t |= 0x8000u >> n; cpu_setr(2, code, t); return 1;
    case 0x19: {                                        /* MULA rr: rr += (XDE) * (XHL), XHL -= 2 */
        uint32_t *de = cpu_regp(0xe8), *hl = cpu_regp(0xec), acc; int32_t p;
        if (size != 2) return 0;
        p = (int16_t)cpu_rd(2, *de) * (int16_t)cpu_rd(2, *hl);
        acc = cpu_getr(4, code & 0xfc); t = acc + (uint32_t)p;
        cpu.sr &= ~(F_S | F_Z | F_V);
        if (t & 0x80000000u) cpu.sr |= F_S;
        if (!t) cpu.sr |= F_Z;
        if (~(acc ^ (uint32_t)p) & (acc ^ t) & 0x80000000u) cpu.sr |= F_V;
        cpu_setr(4, code & 0xfc, t); *hl -= 2; return 1; }
    case 0x1c: {                                        /* DJNZ r,d */
        int8_t d = (int8_t)cpu_fetch(1);
        if (size == 4) return 0;
        v = (v - 1) & m; cpu_setr(size, code, v);
        if (v) { cpu.pc += d; *cyc += 2; }
        return 1; }
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: {    /* bit instructions on a register */
        int bit, b, c = cpu.sr & F_C;
        if (size == 4) return 0;
        bit = (second & 8) ? (cpu_getr(1, 0xe0) & 15) : (cpu_fetch(1) & 15);
        if (bit >= size * 8) {                          /* a bit the register does not have: nothing happens */
            if (second == 0x33 || second == 0x34) cpu.sr = (cpu.sr & ~(F_Z | F_N)) | F_H | F_Z;
            return 1;
        }
        b = (v >> bit) & 1;
        switch (second) {
        case 0x20: case 0x28: c &= b; break;
        case 0x21: case 0x29: c |= b; break;
        case 0x22: case 0x2a: c ^= b; break;
        case 0x23: case 0x2b: c = b; break;
        case 0x24: case 0x2c: cpu_setr(size, code, (v & ~(1u << bit)) | ((uint32_t)c << bit)); return 1;
        case 0x30: cpu_setr(size, code, v & ~(1u << bit)); return 1;
        case 0x31: cpu_setr(size, code, v | (1u << bit)); return 1;
        case 0x32: cpu_setr(size, code, v ^ (1u << bit)); return 1;
        case 0x33: cpu.sr = (cpu.sr & ~(F_Z | F_N)) | F_H | (b ? 0 : F_Z); return 1;
        default:   cpu.sr = (cpu.sr & ~(F_Z | F_N)) | F_H | (b ? 0 : F_Z); cpu_setr(size, code, v | (1u << bit)); return 1;
        }
        cpu.sr = (cpu.sr & ~F_C) | c; return 1; }
    case 0x2e: n = cpu_fetch(1); for (t = 0; t < (uint32_t)size; t++) cpu.cr[(n + t) & 255] = (uint8_t)(v >> (8 * t)); return 1;   /* LDC cr,r */
    case 0x2f: n = cpu_fetch(1); for (v = 0, t = 0; t < (uint32_t)size; t++) v |= (uint32_t)cpu.cr[(n + t) & 255] << (8 * t); cpu_setr(size, code, v); return 1;
    case 0x38: case 0x39: case 0x3a: case 0x3c: case 0x3d: case 0x3e: {       /* MINC1/2/4, MDEC1/2/4 #,r */
        uint32_t step = 1u << (second & 3), num;
        if (size != 2) return 0;
        num = (cpu_fetch(2) + step) & 0xffff;
        if (!num) return 1;
        if (second < 0x3c) { if (v % num == num - step) v -= num - step; else v += step; }
        else { if (v % num == 0) v += num - step; else v -= step; }
        cpu_setr(2, code, v); return 1; }
    }
    switch (second & 0xf8) {
    case 0x40: case 0x48: case 0x50: case 0x58: {       /* MUL MULS DIV DIVS RR,r */
        unsigned tc; uint32_t big;
        if (size == 4) return 0;
        if (size == 1) { if (!(second & 1)) return 0; tc = cpu_code16[R >> 1]; } else tc = cpu_code16[R];
        big = cpu_getr(size * 2, tc);
        if ((second & 0xf8) == 0x40) big = (big & m) * v;
        else if ((second & 0xf8) == 0x48) big = size == 1 ? (uint32_t)((int8_t)big * (int8_t)v) : (uint32_t)((int16_t)big * (int16_t)v);
        else big = cpu_div(size, big, v, (second & 0xf8) == 0x58);
        cpu_setr(size * 2, tc, big); return 1; }
    case 0x60: case 0x68:                               /* INC, DEC #3,r: a word or long register changes no flag */
        n = R ? R : 8;
        if (size == 1) cpu_setr(1, code, cpu_incdec(second & 8, 1, v, n));
        else cpu_setr(size, code, (second & 8) ? v - n : v + n);
        return 1;
    case 0x70: case 0x78: if (size == 4) return 0; cpu_setr(size, code, cpu_cond(second & 15)); return 1;
    case 0x88: cpu_setr(size, rc, v); return 1;         /* LD R,r */
    case 0x98: cpu_setr(size, code, cpu_getr(size, rc)); return 1;            /* LD r,R */
    case 0xa8: cpu_setr(size, code, R); return 1;       /* LD r,#3 */
    case 0xb8: if (size == 4) return 0; t = cpu_getr(size, rc); cpu_setr(size, rc, v); cpu_setr(size, code, t); return 1;
    case 0xd8: cpu_alu(7, size, v, R); return 1;        /* CP r,#3 */
    case 0x80: case 0x90: case 0xa0: case 0xb0: case 0xc0: case 0xd0: case 0xe0: case 0xf0: {   /* ADD.. R,r */
        int op = (second >> 4) & 7;
        t = cpu_alu(op, size, cpu_getr(size, rc), v);
        if (op != 7) cpu_setr(size, rc, t);
        return 1; }
    case 0xc8: {                                        /* ADD.. r,# */
        t = cpu_alu(R, size, v, cpu_fetch(size));
        if (R != 7) cpu_setr(size, code, t);
        return 1; }
    case 0xe8: case 0xf8:                               /* shifts by #4 or by A */
        n = ((second & 0x10) ? cpu_getr(1, 0xe0) : cpu_fetch(1)) & 15;
        if (!n) n = 16;
        *cyc += n / 4;
        cpu_setr(size, code, cpu_shift(R, size, v, n)); return 1;
    }
    return 0;
}

/* ---- instructions with a memory source (prefix 80-AF, C0-E5) ---- */
CPU900_UNUSED static int cpu_op_src(int size, uint32_t ea, int first, int second, int *cyc)
{
    uint32_t m = cpu_mask[size], v, t;
    int R = second & 7, n;
    unsigned rc = cpu_short(size, R);
    if (second >= 0x10 && second <= 0x17) {             /* block moves and compares */
        uint32_t bc;
        int dir = (second & 2) ? -size : size, rep = second & 1;
        if (size == 4) return 0;
        if (second < 0x14) {                            /* LDI LDIR LDD LDDR: (XDE),(XHL) or (XIX),(XIY) */
            uint32_t *d, *s;
            if (first != 0x83 && first != 0x85 && first != 0x93 && first != 0x95) return 0;
            d = cpu_regp((first & 7) == 3 ? 0xe8 : 0xf0); s = cpu_regp((first & 7) == 3 ? 0xec : 0xf4);
            cpu_wr(size, *d, cpu_rd(size, *s)); *d += dir; *s += dir;
            bc = (cpu_getr(2, 0xe4) - 1) & 0xffff; cpu_setr(2, 0xe4, bc);
            cpu.sr &= ~(F_H | F_V | F_N);
            if (bc) { cpu.sr |= F_V; if (rep) { cpu.pc -= 2; *cyc -= 1; } }
        } else {                                        /* CPI CPIR CPD CPDR: A or WA against (r32) */
            uint32_t *p; uint16_t c = cpu.sr & F_C;
            if ((first & 0xe8) != 0x80) return 0;
            p = cpu_regp(cpu_code16[first & 7]);
            cpu_alu(7, size, cpu_getr(size, 0xe0), cpu_rd(size, *p)); *p += dir;
            bc = (cpu_getr(2, 0xe4) - 1) & 0xffff; cpu_setr(2, 0xe4, bc);
            cpu.sr = (cpu.sr & ~(F_C | F_V)) | c | (bc ? F_V : 0);
            if (rep && bc && !(cpu.sr & F_Z)) { cpu.pc -= 2; *cyc -= 1; }
        }
        return 1;
    }
    switch (second) {
    case 0x04: if (size == 4) return 0; cpu_push(size, cpu_rd(size, ea)); return 1;
    case 0x06: case 0x07: {                             /* RLD, RRD A,(mem) */
        uint8_t a = cpu_getr(1, 0xe0), b = mem_r8(ea & 0xffffff);
        if (size != 1) return 0;
        if (second == 0x06) { mem_w8(ea & 0xffffff, (uint8_t)((b << 4) | (a & 15))); a = (a & 0xf0) | (b >> 4); }
        else { mem_w8(ea & 0xffffff, (uint8_t)((b >> 4) | (a << 4))); a = (a & 0xf0) | (b & 15); }
        cpu_setr(1, 0xe0, a);
        cpu.sr &= ~(F_S | F_Z | F_H | F_V | F_N);
        cpu_setsz(1, a); if (cpu_parity(1, a)) cpu.sr |= F_V;
        return 1; }
    case 0x19: if (size == 4) return 0; t = cpu_fetch(2); cpu_wr(size, t, cpu_rd(size, ea)); return 1;       /* LD (nn),(mem) */
    case 0x38: case 0x39: case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f:
        if (size == 4) return 0;
        t = cpu_alu(R, size, cpu_rd(size, ea), cpu_fetch(size));
        if (R != 7) cpu_wr(size, ea, t);
        return 1;
    case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
        if (size == 4) return 0;
        cpu_wr(size, ea, cpu_shift(R, size, cpu_rd(size, ea), 1)); return 1;
    }
    switch (second & 0xf8) {
    case 0x20: cpu_setr(size, rc, cpu_rd(size, ea)); return 1;                /* LD R,(mem) */
    case 0x30: if (size == 4) return 0; v = cpu_rd(size, ea); cpu_wr(size, ea, cpu_getr(size, rc)); cpu_setr(size, rc, v); return 1;
    case 0x40: case 0x48: case 0x50: case 0x58: {       /* MUL MULS DIV DIVS RR,(mem) */
        unsigned tc; uint32_t big;
        if (size == 4) return 0;
        if (size == 1) { if (!(second & 1)) return 0; tc = cpu_code16[R >> 1]; } else tc = cpu_code16[R];
        v = cpu_rd(size, ea); big = cpu_getr(size * 2, tc);
        if ((second & 0xf8) == 0x40) big = (big & m) * v;
        else if ((second & 0xf8) == 0x48) big = size == 1 ? (uint32_t)((int8_t)big * (int8_t)v) : (uint32_t)((int16_t)big * (int16_t)v);
        else big = cpu_div(size, big, v, (second & 0xf8) == 0x58);
        cpu_setr(size * 2, tc, big); return 1; }
    case 0x60: case 0x68:                               /* INC, DEC #3,(mem) */
        if (size == 4) return 0;
        n = R ? R : 8;
        cpu_wr(size, ea, cpu_incdec(second & 8, size, cpu_rd(size, ea), n)); return 1;
    case 0x80: case 0x90: case 0xa0: case 0xb0: case 0xc0: case 0xd0: case 0xe0: case 0xf0: {   /* ADD.. R,(mem) */
        int op = (second >> 4) & 7;
        t = cpu_alu(op, size, cpu_getr(size, rc), cpu_rd(size, ea));
        if (op != 7) cpu_setr(size, rc, t);
        return 1; }
    case 0x88: case 0x98: case 0xa8: case 0xb8: case 0xc8: case 0xd8: case 0xe8: case 0xf8: {   /* ADD.. (mem),R */
        int op = (second >> 4) & 7;
        t = cpu_alu(op, size, cpu_rd(size, ea), cpu_getr(size, rc));
        if (op != 7) cpu_wr(size, ea, t);
        return 1; }
    }
    return 0;
}

/* ---- instructions with a memory destination (prefix B0-BF, F0-F5) ---- */
CPU900_UNUSED static int cpu_op_dst(uint32_t ea, int first, int second, int *cyc)
{
    int R = second & 7;
    switch (second) {
    case 0x00: cpu_wr(1, ea, cpu_fetch(1)); return 1;
    case 0x02: cpu_wr(2, ea, cpu_fetch(2)); return 1;
    case 0x04: cpu_wr(1, ea, cpu_pop(1)); return 1;
    case 0x06: cpu_wr(2, ea, cpu_pop(2)); return 1;
    case 0x14: cpu_wr(1, ea, cpu_rd(1, cpu_fetch(2))); return 1;
    case 0x16: cpu_wr(2, ea, cpu_rd(2, cpu_fetch(2))); return 1;
    case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c:
        if ((cpu_getr(1, 0xe0) & 15) > 7) return 1;     /* a bit a byte does not have */
        return cpu_bit_mem(second - 0x28, ea, cpu_getr(1, 0xe0) & 7);
    }
    switch (second & 0xf8) {
    case 0x20: cpu_setr(2, cpu_code16[R], ea); return 1;                      /* LDA R16,mem */
    case 0x30: cpu_setr(4, cpu_code16[R], ea); return 1;                      /* LDA R32,mem: all 32 bits of the sum */
    case 0x40: cpu_wr(1, ea, cpu_getr(1, cpu_code8[R])); return 1;
    case 0x50: cpu_wr(2, ea, cpu_getr(2, cpu_code16[R])); return 1;
    case 0x60: cpu_wr(4, ea, cpu_getr(4, cpu_code16[R])); return 1;
    case 0x80: case 0x88: case 0x90: case 0x98: case 0xa0: case 0xa8: case 0xb0: case 0xb8: case 0xc0: case 0xc8:
        return cpu_bit_mem((second - 0x80) >> 3, ea, R);
    case 0xd0: case 0xd8: if (cpu_cond(second & 15)) { cpu.pc = ea; *cyc += 3; } return 1;               /* JP cc,mem */
    case 0xe0: case 0xe8: if (cpu_cond(second & 15)) { cpu_push(4, cpu.pc); cpu.pc = ea; *cyc += 8; } return 1;
    case 0xf0: case 0xf8: if (first != 0xb0) return 0; if (cpu_cond(second & 15)) { cpu.pc = cpu_pop(4); *cyc += 8; } return 1;
    }
    return 0;
}

/* Execute one instruction. Returns the number of CPU states it took. When the bytes at pc are not an
   instruction, cpu.bad is set, pc stays on them and 0 is returned. */
CPU900_UNUSED static int cpu_step(void)
{
    uint32_t pc0 = cpu.pc;
    int first = cpu_fetch(1), cyc = 0, ok = 1, r = first & 7;
    cpu.bad = 0;
    if (first >= 0x80 && first < 0xf7) {
        int zz = (first >> 4) & 3, low = first & 15, second;
        if (first >= 0xc0 && low >= 7 && zz != 3) {     /* a register */
            int size = 1 << zz; unsigned code;
            if (low == 7) { code = cpu_fetch(1); cyc += 1; if (code >= 0x40 && code < 0xd0) ok = 0; code &= 0x100 - size; }
            else code = cpu_short(size, r);
            second = cpu_fetch(1);
            cyc += (zz == 0 ? cyc_reg_b : zz == 1 ? cyc_reg_w : cyc_reg_l)[second];
            if (ok) ok = cpu_op_reg(size, code, first, second, &cyc);
        } else if (first >= 0xc0 && low >= 6) ok = 0;
        else {
            uint32_t ea = cpu_ea(first, &cyc, &ok);
            second = cpu_fetch(1);
            if (!ok) ;
            else if (zz == 3) { cyc += cyc_dst[second]; ok = cpu_op_dst(ea, first, second, &cyc); }
            else { cyc += (zz == 0 ? cyc_src_b : zz == 1 ? cyc_src_w : cyc_src_l)[second]; ok = cpu_op_src(1 << zz, ea, first, second, &cyc); }
        }
    } else {
        cyc = cyc_main[first];
        switch (first) {
        case 0x00: break;
        case 0x02: cpu_push(2, cpu.sr); break;
        case 0x03: cpu_set_sr((uint16_t)cpu_pop(2)); break;
        case 0x05: cpu.halted = 1; break;
        case 0x06: { int n = cpu_fetch(1) & 7; cpu.sr = (cpu.sr & ~0x7000) | (n << 12); break; }
        case 0x07: cpu_set_sr((uint16_t)cpu_pop(2)); cpu.pc = cpu_pop(4); cpu.cr[0x3c]--; break;
        case 0x08: { uint32_t a = cpu_fetch(1); cpu_wr(1, a, cpu_fetch(1)); break; }
        case 0x09: cpu_push(1, cpu_fetch(1)); break;
        case 0x0a: { uint32_t a = cpu_fetch(1); cpu_wr(2, a, cpu_fetch(2)); break; }
        case 0x0b: cpu_push(2, cpu_fetch(2)); break;
        case 0x0c: cpu.sr = (cpu.sr & ~0x0700) | (((CPU_RFP + 1) & 3) << 8); CPU_RFP_SET(); break;
        case 0x0d: cpu.sr = (cpu.sr & ~0x0700) | (((CPU_RFP - 1) & 3) << 8); CPU_RFP_SET(); break;
        case 0x0e: cpu.pc = cpu_pop(4); break;
        case 0x0f: { int16_t d = (int16_t)cpu_fetch(2); cpu.pc = cpu_pop(4); XSP += d; break; }
        case 0x10: cpu.sr &= ~(F_H | F_N | F_C); break;
        case 0x11: cpu.sr = (cpu.sr & ~(F_H | F_N)) | F_C; break;
        case 0x12: cpu.sr = (cpu.sr & ~F_N) ^ F_C; break;
        case 0x13: cpu.sr = (cpu.sr & ~(F_N | F_C)) | ((cpu.sr & F_Z) ? 0 : F_C); break;
        case 0x14: cpu_push(1, cpu_getr(1, 0xe0)); break;
        case 0x15: cpu_setr(1, 0xe0, cpu_pop(1)); break;
        case 0x16: { uint8_t t = cpu.sr & 0xff; cpu.sr = (cpu.sr & 0xff00) | cpu.f2; cpu.f2 = t; break; }
        case 0x17: cpu.sr = (cpu.sr & ~0x0700) | ((cpu_fetch(1) & 3) << 8); CPU_RFP_SET(); break;
        case 0x18: cpu_push(1, cpu.sr & 0xff); break;
        case 0x19: cpu.sr = (cpu.sr & 0xff00) | cpu_pop(1); break;
        case 0x1a: cpu.pc = cpu_fetch(2); break;
        case 0x1b: cpu.pc = cpu_fetch(3); break;
        case 0x1c: { uint32_t a = cpu_fetch(2); cpu_push(4, cpu.pc); cpu.pc = a; break; }
        case 0x1d: { uint32_t a = cpu_fetch(3); cpu_push(4, cpu.pc); cpu.pc = a; break; }
        case 0x1e: { int16_t d = (int16_t)cpu_fetch(2); cpu_push(4, cpu.pc); cpu.pc += d; break; }
        case 0xf7: {                                    /* LDX (n),v: F7 00 n 00 v 00 */
            uint32_t a, v;
            cpu_fetch(1); a = cpu_fetch(1); cpu_fetch(1); v = cpu_fetch(1); cpu_fetch(1);
            cpu_wr(1, a, v); break; }
        default:
            if (first >= 0xf8) cpu_vector(first & 7, -1);                    /* SWI n */
            else if (first >= 0x20 && first < 0x28) cpu_setr(1, cpu_code8[r], cpu_fetch(1));
            else if (first >= 0x28 && first < 0x30) cpu_push(2, cpu_getr(2, cpu_code16[r]));
            else if (first >= 0x30 && first < 0x38) cpu_setr(2, cpu_code16[r], cpu_fetch(2));
            else if (first >= 0x38 && first < 0x40) cpu_push(4, cpu_getr(4, cpu_code16[r]));
            else if (first >= 0x40 && first < 0x48) cpu_setr(4, cpu_code16[r], cpu_fetch(4));
            else if (first >= 0x48 && first < 0x50) cpu_setr(2, cpu_code16[r], cpu_pop(2));
            else if (first >= 0x58 && first < 0x60) cpu_setr(4, cpu_code16[r], cpu_pop(4));
            else if (first >= 0x60 && first < 0x70) { int8_t d = (int8_t)cpu_fetch(1); if (cpu_cond(first & 15)) { cpu.pc += d; cyc += 3; } }
            else if (first >= 0x70 && first < 0x80) { int16_t d = (int16_t)cpu_fetch(2); if (cpu_cond(first & 15)) { cpu.pc += d; cyc += 3; } }
            else ok = 0;
        }
    }
    if (!ok) { cpu.pc = pc0; cpu.bad = 1; return 0; }
    cpu.pc &= 0xffffff;
    return cyc > 0 ? cyc : 1;
}
#endif
