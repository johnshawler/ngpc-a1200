/* ngp.h - the Neo Geo Pocket Color as the translated game code sees it: memory, the CPU's registers, the K2GE's
 * registers as plain memory, the BIOS's system calls, the sound processor. Plain C, no platform header: the host
 * test and the Amiga build compile exactly this. Drawing is NOT done here: the Amiga side reads ngp_vid once per
 * frame. The reference machine (tools/ngpref) is a separate, timed model of the same hardware.
 */
#ifndef NGP_H
#define NGP_H
#include <stdint.h>

extern uint8_t ngp_ram[0x4000];         /* 0x4000-0x7FFF: 12 KB work RAM, then the 4 KB shared with the Z80 */
extern uint8_t ngp_vid[0x4000];         /* 0x8000-0xBFFF: the K2GE's registers, palettes, sprites, maps, characters */
extern uint8_t ngp_io[0x100];           /* 0x00-0xFF: the CPU's own registers and the console's ports */
extern const uint8_t *ngp_rom;          /* the cartridge: 0x200000 (and 0x800000 for the second 2 MB) */
extern uint32_t ngp_rom_len;
extern const uint8_t *ngp_font;         /* 2 KB: the BIOS's character set, or 0 (then system call 5 draws boxes) */
extern uint8_t ngp_pad;                 /* controller: bit 0 up, 1 down, 2 left, 3 right, 4 A, 5 B, 6 Option */
extern uint32_t ngp_fatal_pc; extern int ngp_fatal_why;       /* set when the program reached something not translated */
extern uint32_t ngp_frames;             /* frames (ticks) run so far */
extern uint32_t ngp_z80_steps, ngp_cpu_steps;   /* instructions run so far: the sound program; the main CPU (interpreter only) */

extern const uint8_t ngp_even[256];     /* 1 = an even number of bits set */
uint8_t ngp_rd_slow(uint32_t a);        /* everything that is not RAM or ROM */
void    ngp_wr_slow(uint32_t a, uint8_t v);

static inline uint8_t mem_r8(uint32_t a)
{
    if (a - 0x4000u < 0x4000u) return ngp_ram[a - 0x4000];
    if (a - 0x200000u < 0x200000u) return ngp_rom[a - 0x200000];
    if (a - 0x8000u < 0x4000u && a > 0x8010) return ngp_vid[a - 0x8000];
    return ngp_rd_slow(a);
}
static inline void mem_w8(uint32_t a, uint8_t v)
{
    if (a - 0x4000u < 0x3000u) ngp_ram[a - 0x4000] = v;               /* shared RAM, the K2GE and I/O have side effects */
    else ngp_wr_slow(a, v);
}

void ngp_init(const uint8_t *rom, uint32_t len, const uint8_t *font);  /* the state the BIOS leaves when it starts a game */
void ngp_bios_call(unsigned n);         /* system call n, parameters in the registers of bank 3 */
int  ngp_bios_at(uint32_t pc);          /* the system call that lives at this BIOS address, or -1 */
void ngp_fatal(uint32_t pc, int why);   /* 1 not an instruction, 2 unknown entry point, 3 jump into the BIOS, 4 interrupt handler
                                           did not return, 5 the sound program (interpreter build) never came back to an idle loop */

/* The frame model (src/ngpc/logic.c) calls these; they never run translated code themselves. */
void ngp_frame_start(void);             /* controller into RAM, vertical blank pending */
int  ngp_irq_next(void);                /* the next interrupt the CPU would take now: its number in the RAM vector table, or -1 */
uint32_t ngp_irq_enter(int idx);        /* push pc and SR, raise the mask: returns the handler's address */
void ngp_z80_frame(void);               /* the sound processor's time for one frame passes */
void ngp_z80_slice(void);               /* ... a little of it: the main program is polling for its answer */
#endif
