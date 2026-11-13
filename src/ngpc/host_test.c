/* host_test.c - milestone check: run the game logic on the host for N ticks of the bench input and print the
 * game-state checksum. Built twice from the same files:
 *   with run_c.c + gen_*.c    the translation (what the Amiga runs)
 *   with run_interp.c         the interpreter in the same frame model (the proof: tools/verify_ngpc.sh)
 *
 *   host_test N            print the checksum after N ticks
 *   host_test N -v         also print the checksum every 100 ticks (to find the first tick that differs)
 *   host_test N -i script  play an input script in tools/ngpref's format instead of the bench input (tools/flow_ngpc/
 *                          <game>_*.in): "STOPPED ..." if the game reaches code the translation does not have
 *   host_test N -i script -c flow.txt   built with run_interp.c (the interpreter): also write the flow file of what
 *                          ran, for tools/recomp900.py (tools/explore_ngpc.sh does this)
 * Options combine: host_test N -i script -r file records a scripted run (tools/verify_ngpc.sh N script).
 *   host_test N -V         print the checksum after every tick (tools/debug_ngpc.sh compares them with the Amiga's)
 *   host_test N -r file    also write the machine after every tick to file: RAM 0x4000-0x7FFF (16 KB), the K2GE
 *                          (16 KB), the ports (256 bytes), the registers (88 bytes: 20 longs, SR, F', stop reason, pc)
 *   host_test N -d t1,t2   also write the picture after these ticks as host_NNNNN.ppm
 *   host_test N -k         also check after every tick that k2ge_frame_idx() (every third tick) and k2ge_frame_cached()
 *                          (the others) draw what k2ge_line_idx() draws
 * The cartridge is assets/rom.bin and the BIOS's character set assets/font.bin (tools/make_assets_ngpc.sh).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ngp.h"
#define CPU900_EXTERN
#include "cpu900.h"
#include "logic.h"
#include "k2ge.h"
#include "script_input.h"

static uint8_t *load(const char *name, long *len, int must)
{
    FILE *f = fopen(name, "rb"); uint8_t *p;
    if (!f) { if (must) { fprintf(stderr, "cannot read %s (run tools/make_assets_ngpc.sh)\n", name); exit(2); } return 0; }
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    p = malloc((size_t)*len);
    if (fread(p, 1, (size_t)*len, f) != (size_t)*len) exit(2);
    fclose(f);
    return p;
}

static void picture(const char *name)
{
    FILE *f = fopen(name, "wb"); uint16_t row[K2GE_W]; int x, y;
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", K2GE_W, K2GE_H_LINES);
    for (y = 0; y < K2GE_H_LINES; y++) {
        k2ge_line(ngp_vid, y, row);
        for (x = 0; x < K2GE_W; x++) { fputc((row[x] & 15) * 17, f); fputc(((row[x] >> 4) & 15) * 17, f); fputc(((row[x] >> 8) & 15) * 17, f); }
    }
    fclose(f);
}

static unsigned changed_pictures;
extern FILE *interp_flow __attribute__((weak));  /* run_interp.c has it; the translated build does not */
extern void (*logic_irq_hook)(int idx, uint32_t to);
static void flow_vector(int idx, uint32_t to)
{
    static uint32_t last[16];
    if (idx >= 0 && idx < 16 && last[idx] != to + 1) { last[idx] = to + 1; fprintf(interp_flow, "V %d %06x\n", idx, (unsigned)to); }
}

int main(int argc, char **argv)
{
    unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 3000, t;
    int verbose = 0, every = 0, check = 0, scripted = 0, a;
    FILE *ramlog = 0;
    const char *dumps = 0;
    long romlen = 0, fontlen = 0;
    uint8_t *rom, *font;
    char key[32], list[4096], name[64];
    for (a = 2; a < argc; a++) {                /* options, in any order */
        if (!strcmp(argv[a], "-v")) verbose = 1;
        else if (!strcmp(argv[a], "-V")) every = 1;
        else if (!strcmp(argv[a], "-k")) check = 1;
        else if (!strcmp(argv[a], "-r") && a + 1 < argc) ramlog = fopen(argv[++a], "wb");
        else if (!strcmp(argv[a], "-d") && a + 1 < argc) dumps = argv[++a];
        else if (!strcmp(argv[a], "-i") && a + 1 < argc) {
            if (!script_load(argv[++a])) { fprintf(stderr, "cannot read %s\n", argv[a]); return 2; }
            scripted = 1;
        } else if (!strcmp(argv[a], "-c") && a + 1 < argc) {   /* the interpreter build only: write the flow file */
            if (!&interp_flow) { fprintf(stderr, "-c needs the interpreter build (src/ngpc/run_interp.c)\n"); return 2; }
            interp_flow = fopen(argv[++a], "w");
            logic_irq_hook = flow_vector;
        } else { fprintf(stderr, "host_test: unknown option %s\n", argv[a]); return 2; }
    }
    rom = load("assets/rom.bin", &romlen, 1); font = load("assets/font.bin", &fontlen, 0);
    game_init(rom, (uint32_t)romlen, fontlen == 0x800 ? font : 0);
    for (t = 0; t < n; t++) {
        uint32_t gen0 = ngp_vid_gen;
        game_tick(scripted ? script_input(t) : game_bench_input(t));
        if (ngp_vid_gen != gen0) changed_pictures++;
        if (every) printf("%08x\n", (unsigned)game_checksum());
        if (verbose && (t + 1) % 100 == 0) printf("tick %u checksum %08x\n", t + 1, (unsigned)game_checksum());
        if (ramlog) {
            uint8_t r[88]; int i, j;
            for (i = 0; i < 20; i++) { uint32_t v = i < 16 ? cpu.bank[i >> 2][i & 3] : cpu.gpr[i - 16]; for (j = 0; j < 4; j++) r[i * 4 + j] = (uint8_t)(v >> (8 * j)); }
            r[80] = (uint8_t)cpu.sr; r[81] = (uint8_t)(cpu.sr >> 8); r[82] = cpu.f2; r[83] = (uint8_t)ngp_fatal_why;
            for (j = 0; j < 4; j++) r[84 + j] = (uint8_t)(cpu.pc >> (8 * j));
            fwrite(ngp_ram, 1, sizeof ngp_ram, ramlog); fwrite(ngp_vid, 1, sizeof ngp_vid, ramlog); fwrite(ngp_io, 1, sizeof ngp_io, ramlog); fwrite(r, 1, 88, ramlog);
        }
        if (dumps) { snprintf(key, sizeof key, ",%u,", t + 1); snprintf(list, sizeof list, ",%s,", dumps); if (strstr(list, key)) { snprintf(name, sizeof name, "host_%05u.ppm", t + 1); picture(name); } }
        if (check) {
            static uint8_t fi[K2GE_W * K2GE_H_LINES], fc[K2GE_W * K2GE_H_LINES], ln[K2GE_W]; int y;
            uint8_t *fr = t % 3 == 0 ? fi : fc;     /* the cached one keeps its own picture: it redraws only what changed */
            if (t % 3 == 0) k2ge_frame_idx(ngp_vid, fr); else k2ge_frame_cached(ngp_vid, &ngp_dirty, fr);   /* both renderers, by turns */
            for (y = 0; y < K2GE_H_LINES; y++) { k2ge_line_idx(ngp_vid, y, ln); if (memcmp(ln, fr + y * K2GE_W, K2GE_W)) { printf("%s differs from k2ge_line_idx: tick %u, line %d\n", t % 3 == 0 ? "k2ge_frame_idx" : "k2ge_frame_cached", t + 1, y); return 1; } }
        }
        if (ngp_fatal_why) { fprintf(stderr, "STOPPED at tick %u: reason %d at %06x\n", t + 1, ngp_fatal_why, (unsigned)ngp_fatal_pc); break; }
    }
    if (ramlog) fclose(ramlog);
    if (&interp_flow && interp_flow) fclose(interp_flow);
    if (verbose) printf("sound program: %u instructions (%u a tick); main CPU: %u instructions (%u a tick; counted by the interpreter only)\n",
                        (unsigned)ngp_z80_steps, (unsigned)(ngp_z80_steps / (n ? n : 1)), (unsigned)ngp_cpu_steps, (unsigned)(ngp_cpu_steps / (n ? n : 1)));
    printf("pictures=%u (ticks in which the game changed its picture, palette changes aside)\n", changed_pictures);
    printf("ticks=%u checksum=%08x\n", n, (unsigned)game_checksum());
    return 0;
}
