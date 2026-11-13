/* layers_ngpc.c - how a game uses the K2GE, from the translated game on the host: which plane scrolls, how much of
 * the picture each layer covers, colours, sprites, map cells and characters changed per tick. The numbers in
 * FEASIBILITY.md come from it.
 *   cc -O2 -fno-strict-aliasing -Isrc/ngpc -Isrc tools/layers_ngpc.c src/ngpc/logic.c src/ngpc/ngp.c src/ngpc/k2ge.c \
 *      src/ngpc/run_c.c src/coro_host.c $GEN -o layers   (GEN from port.conf)
 *   ./layers TICKS [input script]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ngp.h"
#include "logic.h"
#include "k2ge.h"
#include "script_input.h"
static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int pct(int *v, int n, int p) { qsort(v, n, sizeof *v, cmp); return n ? v[(n - 1) * p / 100] : 0; }
int main(int argc, char **argv)
{
    static uint8_t rom[0x200000], pic[K2GE_W * K2GE_H_LINES], prev[16];
    static int spr[200000], line_spr[200000], pens[200000], cells[200000], chrs[200000], np = 0;
    FILE *f = fopen("assets/rom.bin", "rb"); unsigned n = (unsigned)atoi(argv[1]), t, i;
    static long prio[4], grp_n[200000], grp_w = 0, grp_h = 0, ngrp = 0; static int gw[64000], gh[64000]; long s1 = 0, s2 = 0, both = 0, pix[4] = {0}, frames = 0, wins = 0, pri1 = 0, maxpen[4] = {0};
    if (fread(rom, 1, sizeof rom, f) != sizeof rom) return 1;
    if (argc > 2 && !script_load(argv[2])) return 2;
    game_init(rom, sizeof rom, 0);
    for (t = 0; t < n; t++) {
        uint32_t g0 = ngp_vid_gen; const uint8_t *v = ngp_vid; int c1, c2, nspr = 0, lines[160] = {0}, ml = 0, dc = 0, dch = 0, lastx = 0, lasty = 0, px[64], py[64], par[64], j2;
        static uint8_t used[256]; int cat[4] = {0};
        game_tick(argc > 2 ? script_input(t) : game_bench_input(t));
        c1 = memcmp(prev, v + 0x32, 2) != 0; c2 = memcmp(prev + 2, v + 0x34, 2) != 0;
        memcpy(prev, v + 0x32, 4);
        for (i = 0; i < 2048; i++) if (ngp_dirty.cell[i]) { dc++; ngp_dirty.cell[i] = 0; }
        for (i = 0; i < 512; i++) if (ngp_dirty.chr[i]) { dch++; ngp_dirty.chr[i] = 0; }
        ngp_dirty.any = 0;
        if (ngp_vid_gen == g0) continue;               /* the picture did not change */
        frames++; s1 += c1; s2 += c2; both += c1 && c2;
        if (v[0x30] & 0x80) pri1++;
        if (v[2] || v[3] || v[4] < 160 || v[5] < 152) wins++;
        for (i = 0; i < 64; i++) {                       /* active sprites on screen, chains followed */
            unsigned e = v[0x800 + i * 4] | v[0x801 + i * 4] << 8; int sx = v[0x802 + i * 4], sy = v[0x803 + i * 4], r;
            if (e & 0x400) sx += lastx; if (e & 0x200) sy += lasty; lastx = sx; lasty = sy;
            if (!((e >> 11) & 3)) continue;
            sx = (sx + v[0x20]) & 255; sy = (sy + v[0x21]) & 255;
            if (sx >= 160 && sx < 248) continue; if (sy >= 152 && sy < 248) continue;
            nspr++; prio[(e >> 11) & 3]++;
            px[nspr - 1] = sx; py[nspr - 1] = sy;
            for (r = 0; r < 8; r++) { int l = (sy + r) & 255; if (l < 152 && ++lines[l] > ml) ml = lines[l]; }
        }
        for (i = 0; i < nspr; i++) par[i] = i;           /* touching sprites form one object */
        for (i = 0; i < nspr; i++) for (j2 = 0; j2 < (int)i; j2++)
            if (abs(px[i] - px[j2]) <= 8 && abs(py[i] - py[j2]) <= 8) { int a = i, b = j2; while (par[a] != a) a = par[a]; while (par[b] != b) b = par[b]; par[a] = b; }
        { int x0[64], x1[64], y0[64], y1[64], r; for (i = 0; i < nspr; i++) { x0[i] = 999; x1[i] = -999; y0[i] = 999; y1[i] = -999; }
          for (i = 0; i < nspr; i++) { r = i; while (par[r] != r) r = par[r];
              if (px[i] < x0[r]) x0[r] = px[i]; if (px[i] > x1[r]) x1[r] = px[i]; if (py[i] < y0[r]) y0[r] = py[i]; if (py[i] > y1[r]) y1[r] = py[i]; }
          for (i = 0; i < nspr; i++) if (par[i] == (int)i) { grp_n[np]++; if (ngrp < 64000) { gw[ngrp] = x1[i] - x0[i] + 8; gh[ngrp] = y1[i] - y0[i] + 8; ngrp++; } } }
        k2ge_frame_idx(v, pic); memset(used, 0, sizeof used);
        for (i = 0; i < sizeof pic; i++) { int p = pic[i], k = p < 64 ? 0 : p < 128 ? 1 : p < 192 ? 2 : 3; pix[k]++; if (!used[p]) { used[p] = 1; cat[k]++; } }
        for (i = 0; i < 4; i++) if (cat[i] > maxpen[i]) maxpen[i] = cat[i];
        spr[np] = nspr; line_spr[np] = ml; pens[np] = cat[0] + cat[1] + cat[2] + cat[3]; cells[np] = dc; chrs[np] = dch; np++;
    }
    { static int g[200000]; long j, big = 0, k = ngrp < 64000 ? ngrp : 64000; for (j = 0; j < np; j++) g[j] = grp_n[j];
      for (j = 0; j < k; j++) { if (gw[j] > 32 || gh[j] > 32) big++; if (gw[j] > grp_w) grp_w = gw[j]; if (gh[j] > grp_h) grp_h = gh[j]; }
      printf("  sprite priority (of all shown): behind both %.0f%%, between %.0f%%, in front %.0f%%\n", 100.0 * prio[1] / (prio[1] + prio[2] + prio[3]),
             100.0 * prio[2] / (prio[1] + prio[2] + prio[3]), 100.0 * prio[3] / (prio[1] + prio[2] + prio[3]));
      { long h[6] = {0}; for (j = 0; j < k; j++) { int m = gw[j] > gh[j] ? gw[j] : gh[j]; h[m <= 8 ? 0 : m <= 16 ? 1 : m <= 32 ? 2 : m <= 64 ? 3 : 4]++; }
        printf("  objects by larger side: 8: %.0f%%, 16: %.0f%%, 32: %.0f%%, 64: %.0f%%, more: %.0f%%\n", 100.0*h[0]/k, 100.0*h[1]/k, 100.0*h[2]/k, 100.0*h[3]/k, 100.0*h[4]/k); }
      printf("  objects (touching sprites) per picture: median %d, 95th %d, max %d; groups larger than 32x32: %.0f%%; largest %ldx%ld\n",
             pct(g, np, 50), pct(g, np, 95), pct(g, np, 100), 100.0 * big / k, grp_w, grp_h); }
    printf("%s: %ld changed pictures in %u ticks\n", argc > 2 ? argv[2] : "bench", frames, n);
    printf("  scroll changes: plane 1 in %.0f%%, plane 2 in %.0f%%, both in the same tick %.0f%%; plane 1 in front %.0f%%; window narrowed %.0f%%\n",
           100.0 * s1 / frames, 100.0 * s2 / frames, 100.0 * both / frames, 100.0 * (frames - pri1) / frames, 100.0 * wins / frames);
    printf("  pixels from: sprites %.0f%%, plane 1 %.0f%%, plane 2 %.0f%%, background %.0f%%\n", 100.0 * pix[0] / (frames * 24320.0),
           100.0 * pix[1] / (frames * 24320.0), 100.0 * pix[2] / (frames * 24320.0), 100.0 * pix[3] / (frames * 24320.0));
    printf("  pens in one picture, most: sprites %ld, plane 1 %ld, plane 2 %ld, back %ld\n", maxpen[0], maxpen[1], maxpen[2], maxpen[3]);
    printf("  active 8x8 sprites: median %d, 95th %d, max %d; on one line: 95th %d, max %d\n", pct(spr, np, 50), pct(spr, np, 95), pct(spr, np, 100), pct(line_spr, np, 95), pct(line_spr, np, 100));
    printf("  pens per picture: median %d, 95th %d, max %d\n", pct(pens, np, 50), pct(pens, np, 95), pct(pens, np, 100));
    printf("  map cells changed per tick: median %d, 95th %d, max %d; characters: median %d, 95th %d, max %d\n",
           pct(cells, np, 50), pct(cells, np, 95), pct(cells, np, 100), pct(chrs, np, 50), pct(chrs, np, 95), pct(chrs, np, 100));
    return 0;
}
