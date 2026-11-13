/* run68k.c - the Amiga runs the 68k back end of tools/recomp900.py (src/ngpc/gen68k.s, made by tools/back900.py).
 * This is its C side: the registers between the 68k registers and the CPU state, and what the assembly hands to C
 * (the op_ functions of gen_ops.c, waits, BIOS calls, memory other than RAM and the cartridge). Nothing here interprets
 * the game's code: what the traces never reached stops the game (ngp_fatal reason 2). */
#include <stddef.h>
#include "ngp.h"
#define CPU900_EXTERN
#include "cpu900.h"
#include "gen900.h"
#include "logic.h"
#include "run.h"

/* d3 (flags: C bit 0, V 1, Z 2, S 3, H 5, N 6), d4-d7 (XWA..XHL of the current bank), a2-a5 (XIX..XSP) */
uint32_t asm_regs[9];
/* the assembly reads the four banks and the current bank's address straight from cpu (registers named by their bank) */
typedef char cpu_bank_at_0[offsetof(Cpu900, bank) == 0 ? 1 : -1];
typedef char cpu_cur_at_80[offsetof(Cpu900, cur) == 80 ? 1 : -1];
uint32_t asm_call(uint32_t pc);

static uint8_t sr_of_d3[128], d3_of_sr[256];
static int tabs_ready;
static void make_tabs(void)
{
    unsigned f;
    for (f = 0; f < 256; f++) {
        unsigned d = (f & 1) | ((f >> 2) & 1) << 1 | ((f >> 6) & 1) << 2 | ((f >> 7) & 1) << 3 | ((f >> 4) & 1) << 5 | ((f >> 1) & 1) << 6;
        d3_of_sr[f] = (uint8_t)d;
        if (!(f & 0x28)) sr_of_d3[d] = (uint8_t)f;          /* the SR's bits 3 and 5 are not flags */
    }
    tabs_ready = 1;
}
void asm_regs_out(void)
{
    int i;
    if (!tabs_ready) make_tabs();
    for (i = 0; i < 4; i++) { cpu.cur[i] = asm_regs[1 + i]; cpu.gpr[i] = asm_regs[5 + i]; }
    cpu.sr = (uint16_t)((cpu.sr & 0xff28) | sr_of_d3[asm_regs[0] & 0x7f]);
}
void asm_regs_in(void)
{
    int i;
    if (!tabs_ready) make_tabs();
    for (i = 0; i < 4; i++) { asm_regs[1 + i] = cpu.cur[i]; asm_regs[5 + i] = cpu.gpr[i]; }
    asm_regs[0] = d3_of_sr[cpu.sr & 0xd7];
}
/* an instruction with no inline 68k form: its op_ function (gen_ops.c, recomp900.op_body) on the CPU state */
uint32_t asm_op(uint32_t (*op)(void))
{
    uint32_t r;
    asm_regs_out();
    r = op();
    asm_regs_in();
    return r;
}
void asm_wait(uint32_t pc) { asm_regs_out(); ngp_wait(pc); asm_regs_in(); }
void asm_poll(uint32_t pc) { asm_regs_out(); ngp_poll(pc); asm_regs_in(); }
void asm_swi(void) { asm_regs_out(); ngp_bios_call(cpu.bank[3][0] >> 8 & 0xff); asm_regs_in(); }
uint32_t asm_bios(uint32_t pc) { uint32_t r; asm_regs_out(); r = ngp_bios_entry(pc); asm_regs_in(); return r; }
uint32_t asm_fatal(uint32_t pc, int why) { asm_regs_out(); ngp_fatal(pc, why); asm_regs_in(); return 0; }
void asm_ei(uint32_t n) { cpu.sr = (uint16_t)((cpu.sr & ~0x7000) | (n << 12)); }
uint32_t asm_rd8(uint32_t a) { return RD8(a); }
uint32_t asm_rd16(uint32_t a) { return RD16(a); }
uint32_t asm_rd32(uint32_t a) { return RD32(a); }
void asm_wr8(uint32_t a, uint32_t v) { WR8(a, v); }
void asm_wr16(uint32_t a, uint32_t v) { WR16(a, v); }
void asm_wr32(uint32_t a, uint32_t v) { WR32(a, v); }
/* LDIR, LDDR and their word forms, the whole block: n elements of |step| bytes from src to dst, dst and src moving by step.
   Characters, maps and sprites copied from the cartridge or RAM (a game's uploads) take the direct way; everything else
   goes element by element as the instruction does. Returns the bytes moved (the registers move by that). */
uint32_t asm_ldir(uint32_t dst, uint32_t src, uint32_t n, int32_t step)
{
    uint32_t size = (uint32_t)(step < 0 ? -step : step), bytes = n * size, i;
    dst &= 0xffffff; src &= 0xffffff;
    if (step > 0 && bytes <= 0x3800u && dst - 0x8800u <= 0x3800u - bytes) {
        const uint8_t *s = src - 0x200000u <= 0x200000u - bytes ? ngp_rom + (src - 0x200000)
                         : bytes <= 0x4000u && src - 0x4000u <= 0x4000u - bytes ? ngp_ram + (src - 0x4000) : 0;
        if (s) {                                /* whole characters (16 bytes) as longs; the rest as ngp_vidw does */
            uint32_t o = dst - 0x8000, end = o + bytes;
            while (o < end) {
                if (o >= 0x2000 && !(o & 15) && end - o >= 16) {
                    uint32_t *d = (uint32_t *)(void *)(ngp_vid + o);
                    const uint32_t *q = (const uint32_t *)(const void *)s;
                    if (d[0] != q[0] || d[1] != q[1] || d[2] != q[2] || d[3] != q[3]) {
                        d[0] = q[0]; d[1] = q[1]; d[2] = q[2]; d[3] = q[3];
                        ngp_vid_gen++; ngp_dirty.chr[(o - 0x2000) >> 4] = 1; ngp_dirty.any = 1;
                    }
                    o += 16; s += 16;
                } else { ngp_vidw(o, *s); o++; s++; }
            }
            return bytes;
        }
    }
    if (size == 1) for (i = 0; i < n; i++) { WR8(dst, RD8(src)); dst += (uint32_t)step; src += (uint32_t)step; }
    else for (i = 0; i < n; i++) { WR16(dst, RD16(src)); dst += (uint32_t)step; src += (uint32_t)step; }
    return bytes;
}
uint32_t run_call(uint32_t pc) { return asm_call(pc); }
