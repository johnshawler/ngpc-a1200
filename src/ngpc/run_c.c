/* run_c.c - the game's code is the C translation tools/recomp900.py generated (src/ngpc/gen_*.c). */
#include "run.h"
#include "gen_code.h"
uint32_t run_call(uint32_t pc) { return ngp_dispatch(pc); }
