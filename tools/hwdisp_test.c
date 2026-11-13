/* hwdisp_test.c - the hardware display (src/ngpc/hwdisp.c) on the host, against the reference picture.
 *
 * A model of the kit's display stands in for the Amiga: the playfield as a 512-pixel-wide ribbon that slides through
 * memory (world (x + 512, y - 1) is (x, y)), 16 x 16 tiles into it, blitter objects through their masks, five 64-pixel
 * sprites (pairs attached) in front, 256 colours. The game runs at 60 ticks a second and a picture is made 50 times
 * a second, as on the Amiga. Every picture's 160 x 152 window is compared with src/ngpc/k2ge.c's picture of the same
 * moment, and the pixels around the window must be black.
 *
 *   cc -O2 -fno-strict-aliasing -Isrc/ngpc -Isrc tools/hwdisp_test.c src/ngpc/hwdisp.c src/ngpc/logic.c src/ngpc/ngp.c \
 *      src/ngpc/k2ge.c src/ngpc/run_c.c src/coro_host.c src/ngpc/gen_ops.c $GEN -o hwtest      (GEN from port.conf)
 *   ./hwtest TICKS [input script] [-t tiles a frame] [-c cells a frame] [-p first-picture-to-save count dir]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ngp.h"
#include "logic.h"
#include "k2ge.h"
#include "hwdisp.h"
#include "script_input.h"

#define RIB_W 512
static uint8_t ribbon[(HW_BUF_H + 4) * RIB_W + HW_LEVEL_W + 2048];
static int32_t sc_x, sc_y;
static uint32_t pal[256];
typedef struct { int32_t x, y; const uint16_t *img, *msk; int ww, h; } Bob;
static Bob bobs[64]; static int nbob;
static struct { uint16_t *blk; int x, y, h, att, on; } spr[8];
static long tiles_total;
static int32_t who[(HW_BUF_H + 4) * RIB_W + HW_LEVEL_W + 2048];   /* which tile wrote each pixel: for finding faults */

static uint8_t *rib(int32_t x, int32_t y) { return ribbon + (y + 2) * RIB_W + x + 64; }
void hw_tile(int32_t x, int32_t y, const uint16_t *t)
{
    int r, i, p;
    tiles_total++;
    if (x < 0 || x % 16 || x > HW_LEVEL_W + 320 || y < 0 || y + 16 > HW_BUF_H + 8) { printf("hw_tile outside: %d %d\n", x, y); return; }
    for (r = 0; r < 16; r++) for (i = 0; i < 16; i++) {
        int pen = 0;
        for (p = 0; p < HW_PLANES; p++) if (t[p * 16 + r] & (0x8000 >> i)) pen |= 1 << p;
        *rib(x + i, y + r) = (uint8_t)pen; who[rib(x + i, y + r) - ribbon] = (int32_t)(x / 16 * 64 + y / 16);
    }
}
void hw_clear(int32_t x, int32_t y, int32_t w, int32_t h)
{
    int32_t i, j;
    if (x < 0 || x % 16 || w % 16) { printf("hw_clear outside: %d %d\n", x, w); return; }
    for (j = y; j < y + h; j++) for (i = x; i < x + w; i++) { *rib(i, j) = 0; who[rib(i, j) - ribbon] = -1; }
}
void hw_bob(int32_t x, int32_t y, const uint16_t *img, const uint16_t *msk, int ww, int h)
{
    if (nbob < 48) { bobs[nbob].x = x; bobs[nbob].y = y; bobs[nbob].img = img; bobs[nbob].msk = msk; bobs[nbob].ww = ww; bobs[nbob].h = h; nbob++; }
    else printf("more than 48 objects in a frame\n");
}
void hw_sprite(int n, uint16_t *blk, int x, int y, int h, int att) { spr[n].blk = blk; spr[n].x = x; spr[n].y = y; spr[n].h = h; spr[n].att = att; spr[n].on = 1; }
void hw_sprite_off(int n) { spr[n].on = 0; }
void hw_scroll(int32_t x, int32_t y) { sc_x = x; sc_y = y; }
void hw_palette(const uint32_t *rgb, int first, int count) { memcpy(pal + first, rgb, (size_t)count * 4); }
void *hw_chip(uint32_t bytes) { return calloc(1, bytes); }

static int spr_bits(int n, int sx, int sy)     /* 2 bits of sprite n at screen (sx, sy), -1 outside */
{
    int dx = sx - spr[n].x, dy = sy - spr[n].y;
    const uint16_t *l;
    if (!spr[n].on || dx < 0 || dx >= 64 || dy < 0 || dy >= spr[n].h) return -1;
    l = spr[n].blk + 8 + dy * 8;
    return ((l[dx >> 4] >> (15 - (dx & 15))) & 1) | ((l[4 + (dx >> 4)] >> (15 - (dx & 15))) & 1) << 1;
}
/* the Amiga's 320 x 152 picture; from[] = 0 playfield, 1 object, 2 sprite */
static void render(uint32_t *out, uint8_t *from)
{
    int x, y, i;
    for (y = 0; y < HW_VIEW_H; y++) for (x = 0; x < 320; x++) {
        int pen = *rib(sc_x + x, sc_y + y), f = 0, n;
        for (i = 0; i < nbob; i++) {
            int bx = sc_x + x - bobs[i].x, by = sc_y + y - bobs[i].y;
            if (bx < 0 || by < 0 || by >= bobs[i].h || bx >= bobs[i].ww * 16) continue;
            if (bobs[i].msk[by * bobs[i].ww + (bx >> 4)] & (0x8000 >> (bx & 15))) {
                int p; pen = 0;
                for (p = 0; p < HW_PLANES; p++) if (bobs[i].img[(p * bobs[i].h + by) * bobs[i].ww + (bx >> 4)] & (0x8000 >> (bx & 15))) pen |= 1 << p;
                f = 1;
            }
        }
        out[y * 320 + x] = pal[pen];
        for (n = 4; n >= 0; n--) {               /* lower numbers in front */
            if (!spr[n].on) continue;
            int c;
            if (n == 1 || n == 3) continue;
            if (n == 4) { c = spr_bits(4, x, y); if (c > 0) { out[y * 320 + x] = pal[HW_SPR_BANK * 16 + 8 + c]; f = 2; } continue; }
            c = spr_bits(n, x, y);
            if (c < 0) continue;
            c |= spr_bits(n + 1, x, y) << 2;
            if (c > 0) { out[y * 320 + x] = pal[HW_SPR_BANK * 16 + c]; f = 2; }
        }
        from[y * 320 + x] = (uint8_t)f;
    }
}
static int cmpi(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int pct(int *v, int n, int p) { qsort(v, (size_t)n, sizeof *v, cmpi); return n ? v[(n - 1) * p / 100] : 0; }

int main(int argc, char **argv)
{
    static uint8_t rom[0x200000];
    static uint32_t pic[320 * HW_VIEW_H]; static uint8_t from[320 * HW_VIEW_H];
    static int tl[100000], cl[100000], bl[100000];
    FILE *f = fopen("assets/rom.bin", "rb");
    unsigned n = (unsigned)atoi(argv[1]), t = 0, acc = 0;
    long frames = 0, exact = 0, bad_pf = 0, bad_spr = 0, bad_obj = 0, bad_border = 0, misses = 0, remaps = 0, rebases = 0, worst = 0, maxfront = 0;
    long w_scroll = 0, w_chr = 0, w_objs = 0, lagging = 0;
    long frames_pf_ok = 0, first_border = -1, first_pf = -1;
    int save_from = -1, save_n = 0, i; const char *save_dir = ".", *script = 0;
    if (!f || fread(rom, 1, sizeof rom, f) != sizeof rom) { printf("assets/rom.bin?\n"); return 1; }
    for (i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-t")) hwdisp_tiles_per_frame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-c")) hwdisp_cells_per_frame = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-s")) hwdisp_s4_strip = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-p")) { save_from = atoi(argv[++i]); save_n = atoi(argv[++i]); save_dir = argv[++i]; }
        else script = argv[i];
    }
    if (script && !script_load(script)) return 2;
    game_init(rom, sizeof rom, 0);
    if (!hwdisp_init()) return 3;
    while (t < n) {
        int x, y, wrong = 0, wrong_pf = 0;
        acc += 60;                              /* 60 ticks a second, 50 pictures */
        while (acc >= 50 && t < n) { acc -= 50; game_tick(script ? script_input(t) : game_bench_input(t)); t++; }
        nbob = 0;
        hwdisp_frame(ngp_vid, &ngp_dirty);
        render(pic, from);
        for (y = 0; y < HW_VIEW_H; y++) {
            uint16_t ref[K2GE_W];
            k2ge_line(ngp_vid, y, ref);
            for (x = 0; x < 320; x++) {
                uint32_t m = pic[y * 320 + x];
                if (x < HW_WIN_X || x >= HW_WIN_X + 160) { if (m) { bad_border++; if (first_border < 0) { first_border = frames; printf("first border pixel: picture %ld view %d,%d from %d (0 playfield 1 object 2 sprite) world %d,%d written by %d/%d\n", frames, x, y, from[y * 320 + x], sc_x + x, sc_y + y, who[rib(sc_x + x, sc_y + y) - ribbon] / 64, who[rib(sc_x + x, sc_y + y) - ribbon] % 64); } } continue; }
                {
                    unsigned c = ref[x - HW_WIN_X];
                    uint32_t r = ((c & 15) * 17u) << 16 | (((c >> 4) & 15) * 17u) << 8 | ((c >> 8) & 15) * 17u;
                    if (m != r) { wrong++; if (from[y * 320 + x] == 2) bad_spr++; else if (from[y * 320 + x] == 1) bad_obj++; else { bad_pf++; wrong_pf++; } }
                }
            }
        }
        if (getenv("HWDBG") && frames == atoi(getenv("HWDBG"))) {
            for (y = 0; y < HW_VIEW_H; y += 16) for (x = HW_WIN_X + 160; x < 320; x += 8)
                if (pic[y * 320 + x]) { int32_t w = who[rib(sc_x + x, sc_y + y) - ribbon]; printf("border px view %d,%d world %d,%d (col %d) written by tile col %d row %d; cam %d,%d\n", x, y, sc_x + x, sc_y + y, (sc_x + x) / 16, w / 64, w % 64, sc_x, sc_y); }
        }
        if (!wrong) exact++;
        if (!wrong_pf) frames_pf_ok++;
        else if (first_pf < 0) { first_pf = frames; printf("first playfield difference: picture %ld (tick %u), %d pixels; window %d,%d %dx%d\n", frames, t, wrong_pf, ngp_vid[2], ngp_vid[3], ngp_vid[4], ngp_vid[5]); }
        if (wrong > worst) worst = wrong;
        tl[frames] = hwdisp_last.tiles; cl[frames] = hwdisp_last.cells; bl[frames] = hwdisp_last.bobs;
        if (hwdisp_last.front_objs > maxfront) maxfront = hwdisp_last.front_objs;
        w_scroll += hwdisp_last.why_scroll; w_chr += hwdisp_last.why_chr; w_objs += hwdisp_last.why_objs; if (hwdisp_last.cells_left) lagging++;
        misses += hwdisp_last.slot_misses; remaps += hwdisp_last.remaps; rebases += hwdisp_last.rebases;
        if (save_from >= 0 && frames >= save_from && frames < save_from + save_n) {
            char name[512]; FILE *o;
            snprintf(name, sizeof name, "%s/hw_%05ld.ppm", save_dir, frames);
            o = fopen(name, "wb");
            fprintf(o, "P6 480 152 255\n");
            for (y = 0; y < HW_VIEW_H; y++) {
                uint16_t ref[K2GE_W];
                k2ge_line(ngp_vid, y, ref);
                for (x = 0; x < 480; x++) {
                    uint32_t c;
                    if (x < 320) c = pic[y * 320 + x];
                    else { unsigned q = ref[x - 320]; c = ((q & 15) * 17u) << 16 | (((q >> 4) & 15) * 17u) << 8 | ((q >> 8) & 15) * 17u; }
                    fputc((int)(c >> 16), o); fputc((int)(c >> 8) & 255, o); fputc((int)c & 255, o);
                }
            }
            fclose(o);
        }
        frames++;
    }
    printf("%s: %ld pictures from %u ticks: %ld exactly the reference (%.1f%%), course and objects right in %ld (%.1f%%)\n",
           script ? script : "bench", frames, n, exact, 100.0 * exact / frames, frames_pf_ok, 100.0 * frames_pf_ok / frames);
    printf("  pixels different: front layer %ld, objects %ld, playfield %ld (%.3f%% of all); worst picture %ld; not black around the window %ld\n",
           bad_spr, bad_obj, bad_pf, 100.0 * (bad_spr + bad_obj + bad_pf) / (frames * 24320.0), worst, bad_border);
    printf("  tiles a frame: median %d, 95th %d, max %d (%ld in all); front cells converted: median %d, 95th %d, max %d\n",
           pct(tl, (int)frames, 50), pct(tl, (int)frames, 95), pct(tl, (int)frames, 100), tiles_total, pct(cl, (int)frames, 50), pct(cl, (int)frames, 95), pct(cl, (int)frames, 100));
    printf("  objects a frame: median %d, max %d; front-layer objects at most %ld; palette slots missed %ld; front remaps %ld; rebases %ld\n",
           pct(bl, (int)frames, 50), pct(bl, (int)frames, 100), maxfront, misses, remaps, rebases);
    printf("  front layer redrawn whole: %ld times for its scroll or window, %ld for characters of its objects; objects changed in %ld pictures; behind in %ld\n", w_scroll, w_chr, w_objs, lagging);
    return 0;
}
