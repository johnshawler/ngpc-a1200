/* psgtrace_ngpc.c - the real sound program's chip state 50 times a second over the bench, on the host.
 *
 * Usage: psgtrace_ngpc rom.bin ticks out.txt
 *
 * Built with -DNGP_Z80_INTERP: the game runs with its real sound program on the Z80 interpreter, every write to the
 * sound chip goes into a PsgState (src/ngpc/psg.c), and one line per 50 Hz frame is written in the format of
 * sndrec_ngpc's compare file. tools/check_music_ngpc.sh compares the two.
 */
#include <stdio.h>
#include <stdlib.h>
#include "ngp.h"
#include "logic.h"
#include "psg.h"

static Psg psg;
static void hook(unsigned port, uint8_t v) { psg_write(&psg, port, v); }

int main(int argc, char **argv)
{
    static uint8_t rom[0x400000];
    FILE *f, *o;
    long romlen;
    unsigned n, t = 0, fr, nf;
    if (argc < 4) { fprintf(stderr, "usage: psgtrace_ngpc rom.bin ticks out.txt\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    romlen = (long)fread(rom, 1, sizeof rom, f); fclose(f);
    n = (unsigned)atoi(argv[2]);
    o = fopen(argv[3], "w");
    if (!o) { perror(argv[3]); return 1; }
    game_init(rom, (uint32_t)romlen, 0);
    psg_reset(&psg);
    ngp_psg_hook = hook;
    nf = (unsigned)((double)n / 59.95 * 50);
    for (fr = 0; fr < nf; fr++) {              /* the same times as sndrec_ngpc's compare file */
        unsigned tick = (unsigned)(fr * 59.95 / 50), j;
        while (t <= tick && t < n) { game_tick(game_bench_input(t)); t++; }
        fprintf(o, "%u", fr);
        for (j = 0; j < PSG_STATE; j++) fprintf(o, " %02x", ((uint8_t *)&psg.s)[j]);
        fputc('\n', o);
    }
    fclose(o);
    return 0;
}
