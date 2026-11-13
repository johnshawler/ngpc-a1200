/* front_ngpc.c - where the front scroll plane (plane 1) covers the picture in the ticks the back plane scrolls,
 * per 8 lines. Build as tools/layers_ngpc.c; ./front TICKS [input script]. Used for FEASIBILITY.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ngp.h"
#include "logic.h"
#include "k2ge.h"
#include "script_input.h"
int main(int argc, char **argv)
{
    static uint8_t rom[0x200000], pic[K2GE_W * K2GE_H_LINES], prev[4];
    static double row1[152], row2[152], rows[152]; static long colpanel[160];
    FILE *f = fopen("assets/rom.bin", "rb"); unsigned n = (unsigned)atoi(argv[1]), t, x, y; long k = 0, full = 0, empty = 0, mixed = 0;
    if (fread(rom, 1, sizeof rom, f) != sizeof rom) return 1;
    if (argc > 2 && !script_load(argv[2])) return 2;
    game_init(rom, sizeof rom, 0);
    for (t = 0; t < n; t++) {
        uint32_t g0 = ngp_vid_gen; int sc;
        game_tick(argc > 2 ? script_input(t) : game_bench_input(t));
        sc = memcmp(prev + 2, ngp_vid + 0x34, 2) != 0; memcpy(prev, ngp_vid + 0x32, 4);
        if (!sc || ngp_vid_gen == g0) continue;            /* only pictures where plane 2 scrolled */
        k2ge_frame_idx(ngp_vid, pic); k++;
        for (y = 0; y < 152; y++) {
            int c1 = 0, c2 = 0;
            for (x = 0; x < 160; x++) { int p = pic[y * 160 + x]; c1 += p >= 64 && p < 128; c2 += p >= 128 && p < 192; }
            row1[y] += c1 / 160.0; row2[y] += c2 / 160.0;
            if (c1 > 150) full++; else if (c1 < 10) empty++; else mixed++;
        }
    }
    printf("%s: %ld pictures where plane 2 scrolled. Rows: plane 1 covers >94%%: %.0f%%, <6%%: %.0f%%, in between: %.0f%%\n",
           argc > 2 ? argv[2] : "bench", k, 100.0 * full / (k * 152), 100.0 * empty / (k * 152), 100.0 * mixed / (k * 152));
    printf("  per 8 rows, share of plane 1 / plane 2 pixels:");
    for (y = 0; y < 152; y += 8) { double a = 0, b = 0; unsigned j; for (j = y; j < y + 8; j++) { a += row1[j]; b += row2[j]; } printf(" %d:%.0f/%.0f", y, 100 * a / (8 * k), 100 * b / (8 * k)); }
    printf("\n");
    return 0;
}
