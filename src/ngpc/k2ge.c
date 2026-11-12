/* k2ge.c - draws one scanline of the K2GE picture (colour mode) from the chip's memory. See k2ge.h.
 *
 * Layers, back to front: background colour, sprites of priority 1, the back scroll plane, sprites of priority 2,
 * the front scroll plane, sprites of priority 3. Scroll plane 1 is the front one unless bit 7 of 0x8030 is set.
 * Colour 0 of every palette is transparent. Among sprites of one priority the lower number wins.
 * Not done: the mono (K1GE) mode of Neo Geo Pocket games without colour.
 */
#include <string.h>
#include "k2ge.h"

#define RD16(p) ((uint16_t)((p)[0] | (p)[1] << 8))

static void tile_row(const uint8_t *v, uint8_t *out, uint8_t *z, int x, unsigned tile, int row, int mirror,
                     unsigned pal, int depth, int lo, int hi)
{
    unsigned bits = RD16(v + 0x2000 + tile * 16 + row * 2);         /* leftmost pixel in bits 15-14 */
    int i;
    for (i = 0; i < 8; i++) {
        int px = x + i, c = mirror ? (bits >> (i * 2)) & 3 : (bits >> ((7 - i) * 2)) & 3;
        if (!c || px < lo || px > hi || depth <= z[px]) continue;
        z[px] = (uint8_t)depth;
        out[px] = (uint8_t)(pal + c);
    }
}

static void plane(const uint8_t *v, uint8_t *out, uint8_t *z, int line, int which, int depth, int lo, int hi)
{
    const uint8_t *map = v + (which ? 0x1800 : 0x1000);             /* 0x9000 and 0x9800: 32 x 32 words */
    unsigned pal = which ? 128 : 64;                                /* 0x8280 and 0x8300: 16 palettes of 4 */
    unsigned sx = v[which ? 0x34 : 0x32], sy = v[which ? 0x35 : 0x33];
    unsigned y = (line + sy) & 255, row = y & 7;
    int tx;
    for (tx = 0; tx < 32; tx++) {
        unsigned e = RD16(map + ((y >> 3) * 32 + tx) * 2);
        int x = (tx * 8 - sx) & 255;
        if (x > 248) x -= 256;
        if (x >= K2GE_W) continue;
        tile_row(v, out, z, x, e & 0x1ff, (e & 0x4000) ? 7 - row : row, e & 0x8000, pal + ((e >> 9) & 15) * 4, depth, lo, hi);
    }
}

void k2ge_line_idx(const uint8_t *v, int line, uint8_t *out)
{
    int winx = v[2], winy = v[3], winw = v[4], winh = v[5], x, i;
    int lo = winx, hi = (winx + winw < K2GE_W ? winx + winw : K2GE_W) - 1;
    unsigned edge = 0xf8 + (v[0x12] & 7), back = 0xf0 + (v[0x118] & 7);    /* window palette 0x83F0, background palette 0x83E0 */
    uint8_t z[K2GE_W];
    int lastx = 0, lasty = 0;
    for (x = 0; x < K2GE_W; x++) { out[x] = (uint8_t)edge; z[x] = 0; }
    if (line < winy || line >= winy + winh) return;                 /* above or below the window */
    for (x = lo; x <= hi; x++) out[x] = (uint8_t)back;
    if (v[0x30] & 0x80) { plane(v, out, z, line, 0, 3, lo, hi); plane(v, out, z, line, 1, 5, lo, hi); }
    else                { plane(v, out, z, line, 1, 3, lo, hi); plane(v, out, z, line, 0, 5, lo, hi); }
    for (i = 0; i < 64; i++) {                                      /* sprites: 0x8800, 4 bytes each */
        const uint8_t *s = v + 0x0800 + i * 4;
        unsigned e = RD16(s);
        int pr = (e >> 11) & 3, sx = s[2], sy = s[3], row;
        if (e & 0x0400) sx += lastx;                                /* chained to the sprite before */
        if (e & 0x0200) sy += lasty;
        lastx = sx; lasty = sy;
        if (!pr) continue;
        sx = (sx + v[0x20]) & 255; sy += v[0x21];                   /* 249..255 mean -7..-1 */
        if (sx > 248) sx -= 256;
        if (sy > 248 && sy < 256) sy -= 256; else sy &= 255;
        if (line < sy || line > sy + 7) continue;
        row = line - sy;
        tile_row(v, out, z, sx, e & 0x1ff, (e & 0x4000) ? 7 - row : row, e & 0x8000, (v[0x0c00 + i] & 15) * 4, pr * 2, lo, hi);
    }
}

void k2ge_line(const uint8_t *v, int line, uint16_t *out)
{
    uint8_t idx[K2GE_W];
    int x, neg = v[0x12] & 0x80;
    k2ge_line_idx(v, line, idx);
    for (x = 0; x < K2GE_W; x++) { unsigned c = RD16(v + 0x200 + idx[x] * 2); out[x] = (uint16_t)((neg ? ~c : c) & 0x0fff); }
}

/* ---- the whole picture at once, for the Amiga: the same result as k2ge_line_idx() for every line (host_test -k
   checks it), drawn back to front (painter's order) so that no depth buffer is needed:
   background, sprites of priority 1, the back plane, sprites of priority 2, the front plane, sprites of priority 3.
   Among sprites of one priority the lower number wins, so they are drawn from the highest number down. ---- */

static uint8_t spr_lines[K2GE_H_LINES][64];     /* per line: the sprites on it, highest number first */
static uint8_t spr_count[K2GE_H_LINES];
static int16_t spr_x[64], spr_y[64];
static uint32_t pix_tab[2][256], msk_tab[2][256];   /* [mirrored][a byte of a tile row]: its 4 pixels (0-3), and 0xFF where one is not 0 */
static int tabs_ready;
#define MARGIN 8                                    /* the line buffer has room for a tile row on each side: no clipping */

static void make_tabs(void)
{
    unsigned b, i, m;
    for (b = 0; b < 256; b++)
        for (m = 0; m < 2; m++) {
            uint8_t p[4], k[4];
            for (i = 0; i < 4; i++) { unsigned c = m ? (b >> (i * 2)) & 3 : (b >> ((3 - i) * 2)) & 3; p[i] = (uint8_t)c; k[i] = c ? 0xff : 0; }
            memcpy(&pix_tab[m][b], p, 4); memcpy(&msk_tab[m][b], k, 4);
        }
    tabs_ready = 1;
}

/* 4 pixels at any address: the 68020 and the host both read and write longs at odd addresses (gcc turns a 4-byte
   memcpy into four byte moves on the 68020, so the long is reached through a pointer) */
#define L4(q) (*(uint32_t *)(void *)(q))
static inline void put4(uint8_t *q, unsigned m, unsigned b, uint32_t base4)
{
    uint32_t k = msk_tab[m][b], v;
    if (!k) return;
    v = pix_tab[m][b] + base4;
    if (k != 0xffffffffu) v = (L4(q) & ~k) | (v & k);
    L4(q) = v;
}
/* one row of a tile: 16 bits, the leftmost pixel in bits 15-14 (in bits 1-0 when mirrored) */
static inline void row8(uint8_t *q, unsigned bits, int mirror, unsigned base)
{
    uint32_t base4 = base * 0x01010101u;
    if (mirror) { put4(q, 1, bits & 0xff, base4); put4(q + 4, 1, bits >> 8, base4); }
    else { put4(q, 0, bits >> 8, base4); put4(q + 4, 0, bits & 0xff, base4); }
}

static void plane_line(const uint8_t *v, uint8_t *lb, int line, int which)
{
    const uint8_t *map = v + (which ? 0x1800 : 0x1000), *chr = v + 0x2000;
    unsigned pal = which ? 128 : 64, sx = v[which ? 0x34 : 0x32], sy = v[which ? 0x35 : 0x33];
    unsigned y = (line + sy) & 255, row = y & 7, tx0 = sx >> 3, i;
    const uint8_t *mrow = map + (y >> 3) * 64;
    uint8_t *q = lb + MARGIN - (sx & 7);
    for (i = 0; i < 21; i++, q += 8) {
        unsigned e = RD16(mrow + ((tx0 + i) & 31) * 2), bits;
        bits = RD16(chr + (e & 0x1ff) * 16 + ((e & 0x4000) ? 7 - row : row) * 2);
        if (bits) row8(q, bits, e & 0x8000, pal + ((e >> 9) & 15) * 4);
    }
}

static void sprites_line(const uint8_t *v, uint8_t *lb, int line, unsigned pr)
{
    unsigned k, n = spr_count[line];
    for (k = 0; k < n; k++) {
        unsigned i = spr_lines[line][k], e = RD16(v + 0x0800 + i * 4), row, bits;
        if (((e >> 11) & 3) != pr) continue;
        row = (unsigned)(line - spr_y[i]);
        bits = RD16(v + 0x2000 + (e & 0x1ff) * 16 + ((e & 0x4000) ? 7 - row : row) * 2);
        if (bits && spr_x[i] > -8 && spr_x[i] < K2GE_W) row8(lb + MARGIN + spr_x[i], bits, e & 0x8000, (v[0x0c00 + i] & 15) * 4);
    }
}

void k2ge_frame_idx(const uint8_t *v, uint8_t *out)
{
    static uint32_t lbl[(K2GE_W + 2 * MARGIN + 8) / 4];
    uint8_t *lb = (uint8_t *)lbl;
    int winx = v[2], winy = v[3], winw = v[4], winh = v[5], x, y, i;
    int lo = winx, hi = (winx + winw < K2GE_W ? winx + winw : K2GE_W) - 1, full = lo == 0 && hi == K2GE_W - 1;
    uint8_t edge = (uint8_t)(0xf8 + (v[0x12] & 7)), back = (uint8_t)(0xf0 + (v[0x118] & 7));
    uint32_t back4 = back * 0x01010101u, edge4 = edge * 0x01010101u;
    int lastx = 0, lasty = 0, backp = (v[0x30] & 0x80) ? 0 : 1;   /* the back plane: 1 (0x9800) unless bit 7 of 0x8030 */
    if (!tabs_ready) make_tabs();
    for (y = 0; y < K2GE_H_LINES; y++) spr_count[y] = 0;
    for (i = 0; i < 64; i++) {                      /* where every sprite is, chains followed */
        const uint8_t *s = v + 0x0800 + i * 4;
        unsigned e = RD16(s);
        int sx = s[2], sy = s[3];
        if (e & 0x0400) sx += lastx;
        if (e & 0x0200) sy += lasty;
        lastx = sx; lasty = sy;
        if (!((e >> 11) & 3)) continue;
        sx = (sx + v[0x20]) & 255; sy += v[0x21];
        if (sx > 248) sx -= 256;
        if (sy > 248 && sy < 256) sy -= 256; else sy &= 255;
        spr_x[i] = (int16_t)sx; spr_y[i] = (int16_t)sy;
    }
    for (i = 63; i >= 0; i--) {                     /* the lines each one covers, highest number first */
        unsigned e = RD16(v + 0x0800 + i * 4);
        int r;
        if (!((e >> 11) & 3)) continue;
        for (r = 0; r < 8; r++) { int l = spr_y[i] + r; if (l >= 0 && l < K2GE_H_LINES) spr_lines[l][spr_count[l]++] = (uint8_t)i; }
    }
    for (y = 0; y < K2GE_H_LINES; y++, out += K2GE_W) {
        if (y < winy || y >= winy + winh || lo > hi) { for (x = 0; x < K2GE_W; x += 4) L4(out + x) = edge4; continue; }
        for (x = 0; x < K2GE_W; x += 4) L4(lb + MARGIN + x) = back4;
        if (spr_count[y]) sprites_line(v, lb, y, 1);
        plane_line(v, lb, y, backp);
        if (spr_count[y]) sprites_line(v, lb, y, 2);
        plane_line(v, lb, y, !backp);
        if (spr_count[y]) sprites_line(v, lb, y, 3);
        for (x = 0; x < K2GE_W; x += 4) L4(out + x) = L4(lb + MARGIN + x);
        if (!full) { for (x = 0; x < lo && x < K2GE_W; x++) out[x] = edge; for (x = hi + 1; x < K2GE_W; x++) out[x] = edge; }
    }
}
