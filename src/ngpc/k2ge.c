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
#define NOINLINE __attribute__((noinline))          /* kept apart: the profiler names them */

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


/* ---- the cached renderer ---- */
#define PW (256 + K2GE_W)                           /* a plane buffer row: 256 pixels, then its first 160 again */
static uint8_t pbuf[2][256 * PW];                   /* pens of the two scroll planes; 0 = see-through */
static int pbuf_ready;
/* What the last picture was made from, so that a line whose inputs did not change is not drawn again: the plane
   buffer rows redrawn since, the registers every line reads, and each line's sprites (entry, number, row, palette, x) */
uint32_t k2ge_line_gen[K2GE_H_LINES];            /* +1 each time k2ge_frame_cached() writes the line */
static uint8_t prow_dirty[2][256], prow_any;
/* per plane buffer row, one bit per 8-pixel cell column: all 8 pixels drawn (opq) or all see-through (clr) */
static uint32_t opq[2][256], clr[2][256], need[256];
static uint8_t last_regs[11], last_ok;
static const uint8_t *last_out;
static uint8_t ls_n[K2GE_H_LINES];
static uint32_t ls_a[K2GE_H_LINES][64];
static int16_t ls_x[K2GE_H_LINES][64];

static void draw_cell(const uint8_t *v, int which, unsigned cell)
{
    const uint8_t *map = v + (which ? 0x1800 : 0x1000);
    unsigned e = RD16(map + cell * 2), pal = (which ? 128 : 64) + ((e >> 9) & 15) * 4, r, cx = cell & 31, cy = cell >> 5;
    uint32_t base4 = pal * 0x01010101u;
    const uint8_t *chr = v + 0x2000 + (e & 0x1ff) * 16;
    int m = (e & 0x8000) ? 1 : 0;
    memset(&prow_dirty[which][cy * 8], 1, 8); prow_any = 1;
    for (r = 0; r < 8; r++) {
        uint32_t bit = 1u << cx, *po = &opq[which][cy * 8 + r], *pc = &clr[which][cy * 8 + r], k0, k1;
        unsigned bits = RD16(chr + ((e & 0x4000) ? 7 - r : r) * 2), b0 = m ? bits & 0xff : bits >> 8, b1 = m ? bits >> 8 : bits & 0xff;
        uint8_t *q = pbuf[which] + (cy * 8 + r) * PW + cx * 8;
        uint32_t w0 = (pix_tab[m][b0] + base4) & msk_tab[m][b0], w1 = (pix_tab[m][b1] + base4) & msk_tab[m][b1];
        k0 = msk_tab[m][b0]; k1 = msk_tab[m][b1];
        if ((k0 & k1) == 0xffffffffu) *po |= bit; else *po &= ~bit;
        if (!(k0 | k1)) *pc |= bit; else *pc &= ~bit;
        L4(q) = w0; L4(q + 4) = w1;
        if (cx < K2GE_W / 8) { L4(q + 256) = w0; L4(q + 260) = w1; }
    }
}

/* lay the 160 pixels of a plane row over a line: its pens where they are not 0. bit: 0x40 (plane 1) or 0x80 */
static void overlay(uint8_t *out, const uint8_t *src, unsigned bit)
{
    unsigned k, sh = bit == 0x40 ? 6 : 7;
    uint32_t b4 = bit * 0x01010101u;
    for (k = 0; k < K2GE_W; k += 4) {
        uint32_t x = L4(src + k), m;
        if (!x) continue;
        m = (x & b4) >> sh; m = (m << 8) - m;       /* 0xFF in every byte that holds a pen */
        if (m == 0xffffffffu) L4(out + k) = x;
        else L4(out + k) = (L4(out + k) & ~m) | x;
    }
}

/* bring the plane buffers up to date (the cells and characters the game changed); 1 if characters changed */
static NOINLINE int update_planes(const uint8_t *v, K2geDirty *d)
{
    unsigned c;
    int chr_any = 0;
    for (c = 0; c < 512; c += 4) if (L4(d->chr + c)) { chr_any = 1; break; }
    if (chr_any) {                              /* characters changed: every cell that shows one of them */
        const uint8_t *m = v + 0x1000, *ch = d->chr;
        uint8_t *cl = d->cell;
        for (c = 0; c < 2048; c++, m += 2)
            if (cl[c] | ch[m[0] | (m[1] & 1) << 8]) { draw_cell(v, c >> 10, c & 1023); cl[c] = 0; }
        memset(d->chr, 0, sizeof d->chr);
    } else {                                    /* only cells: four flags at a time */
        for (c = 0; c < 2048; c += 4)
            if (L4(d->cell + c)) { unsigned k; for (k = c; k < c + 4; k++) if (d->cell[k]) { draw_cell(v, k >> 10, k & 1023); d->cell[k] = 0; } }
    }
    d->any = 0;
    return chr_any;
}

/* where every sprite is (chains followed), and the lines each one covers, highest number first */
static NOINLINE void sprites_setup(const uint8_t *v)
{
    int lastx = 0, lasty = 0, y, i;
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
    for (i = 63; i >= 0; i--) {
        unsigned e = RD16(v + 0x0800 + i * 4);
        int r;
        if (!((e >> 11) & 3)) continue;
        for (r = 0; r < 8; r++) { int l = spr_y[i] + r; if (l >= 0 && l < K2GE_H_LINES) spr_lines[l][spr_count[l]++] = (uint8_t)i; }
    }
}

void k2ge_frame_cached(const uint8_t *v, K2geDirty *d, uint8_t *out)
{
    static uint32_t lbl[(K2GE_W + 2 * MARGIN + 8) / 4];
    uint8_t *lb = (uint8_t *)lbl;
    int winx = v[2], winy = v[3], winw = v[4], winh = v[5], x, y;
    int lo = winx, hi = (winx + winw < K2GE_W ? winx + winw : K2GE_W) - 1, full = lo == 0 && hi == K2GE_W - 1;
    uint8_t edge = (uint8_t)(0xf8 + (v[0x12] & 7)), back = (uint8_t)(0xf0 + (v[0x118] & 7));
    uint32_t back4 = back * 0x01010101u, edge4 = edge * 0x01010101u;
    int backp = (v[0x30] & 0x80) ? 0 : 1;
    unsigned c;
    int chr_any = 0, all;
    uint8_t regs[11];
    if (!tabs_ready) make_tabs();
    if (!pbuf_ready) {
        for (c = 0; c < 2048; c++) d->cell[c] = 1;
        d->any = 1; pbuf_ready = 1;
        for (c = 0; c < 256; c++) {                 /* the cell columns a 160-pixel window from x = c touches */
            unsigned k, n = (c & 7) ? 21 : 20;
            need[c] = 0;
            for (k = 0; k < n; k++) need[c] |= 1u << (((c >> 3) + k) & 31);
        }
    }
    if (d->any) chr_any = update_planes(v, d);
    regs[0] = v[2]; regs[1] = v[3]; regs[2] = v[4]; regs[3] = v[5]; regs[4] = v[0x12]; regs[5] = v[0x118];
    regs[6] = v[0x30] & 0x80; regs[7] = v[0x32]; regs[8] = v[0x33]; regs[9] = v[0x34]; regs[10] = v[0x35];
    all = !last_ok || out != last_out || memcmp(regs, last_regs, sizeof regs);   /* every line, or only those that changed */
    memcpy(last_regs, regs, sizeof regs); last_ok = 1; last_out = out;
    sprites_setup(v);
    {
        unsigned bsx = v[backp ? 0x34 : 0x32], bsy = v[backp ? 0x35 : 0x33], fsx = v[backp ? 0x32 : 0x34], fsy = v[backp ? 0x33 : 0x35];
        unsigned bbit = backp ? 0x80 : 0x40, fbit = backp ? 0x40 : 0x80;
        const uint8_t *bp = pbuf[backp], *fp = pbuf[!backp];
        for (y = 0; y < K2GE_H_LINES; y++, out += K2GE_W) {
            uint8_t *o;
            unsigned n = spr_count[y], k;
            int same = ls_n[y] == n;
            if (y < winy || y >= winy + winh || lo > hi) {
                if (all) { for (x = 0; x < K2GE_W; x += 4) L4(out + x) = edge4; k2ge_line_gen[y]++; }
                continue;
            }
            for (k = 0; k < n; k++) {               /* the line's sprites against the last picture's */
                unsigned j = spr_lines[y][k];
                uint32_t a = RD16(v + 0x0800 + j * 4) | j << 16 | (unsigned)(y - spr_y[j]) << 22 | (v[0x0c00 + j] & 15u) << 25;
                if (ls_a[y][k] != a || ls_x[y][k] != spr_x[j]) { same = 0; ls_a[y][k] = a; ls_x[y][k] = spr_x[j]; }
            }
            ls_n[y] = (uint8_t)n;
            if (!all && same && !(chr_any && n) &&
                !(prow_any && (prow_dirty[backp][(y + bsy) & 255] || prow_dirty[!backp][(y + fsy) & 255]))) continue;
            k2ge_line_gen[y]++;
            o = n ? lb + MARGIN : out;              /* lines with sprites are built with room on both sides */
            {
                unsigned rb = (y + bsy) & 255, rf = (y + fsy) & 255;
                const uint8_t *sb = bp + rb * PW + bsx;
                if ((opq[backp][rb] & need[bsx]) == need[bsx]) {    /* the back plane covers the line: a copy */
                    for (x = 0; x < K2GE_W; x += 4) L4(o + x) = L4(sb + x);
                } else {
                    for (x = 0; x < K2GE_W; x += 4) L4(o + x) = back4;
                    if (n) sprites_line(v, lb, y, 1);
                    overlay(o, sb, bbit);
                }
                if (n) sprites_line(v, lb, y, 2);
                if ((clr[!backp][rf] & need[fsx]) != need[fsx]) overlay(o, fp + rf * PW + fsx, fbit);   /* not all see-through */
            }
            if (spr_count[y]) { sprites_line(v, lb, y, 3); for (x = 0; x < K2GE_W; x += 4) L4(out + x) = L4(lb + MARGIN + x); }
            if (!full) { for (x = 0; x < lo && x < K2GE_W; x++) out[x] = edge; for (x = hi + 1; x < K2GE_W; x++) out[x] = edge; }
        }
    }
    if (prow_any) { memset(prow_dirty, 0, sizeof prow_dirty); prow_any = 0; }
}
