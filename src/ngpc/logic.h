/* logic.h - the seam between the game (plain C, also built on the host) and the Amiga side. */
#ifndef NGPC_LOGIC_H
#define NGPC_LOGIC_H
#include <stdint.h>

/* input bits for game_tick(): the kit's joystick bits, plus the Option button */
#define IN_RIGHT  1
#define IN_LEFT   2
#define IN_DOWN   4
#define IN_UP     8
#define IN_A      16            /* button 1: joystick button 1 (JOY_FIRE) or Z */
#define IN_B      32            /* button 2: joystick button 2 (JOY_FIRE2) or X */
#define IN_OPTION 64

void game_init(const uint8_t *rom, uint32_t len, const uint8_t *font);  /* before the first tick */
void game_tick(unsigned short input);           /* one Neo Geo Pocket frame (1/60 s) of game logic */
unsigned short game_bench_input(unsigned int tick);
uint32_t game_checksum(void);

/* the translated code (or the interpreter) calls these where the game stops computing */
void ngp_wait(uint32_t pc);                     /* a loop that waits for the next frame, or a HALT: the frame ends here */
void ngp_poll(uint32_t pc);                     /* a loop that polls memory while it counts: the sound processor gets time */
uint32_t ngp_bios_entry(uint32_t pc);           /* execution reached a BIOS address: do what the BIOS does, return where it goes on */
#endif
