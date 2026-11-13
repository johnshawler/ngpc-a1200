/* run_c.c - the game's code is the C translation tools/recomp900.py generated (src/ngpc/gen_*.c). */
#include "run.h"
#include "gen_code.h"
uint32_t run_call(uint32_t pc) { return ngp_dispatch(pc); }
uint32_t asm_interp_calls;                     /* the C translation stops on code it does not have (see run68k.c) */
