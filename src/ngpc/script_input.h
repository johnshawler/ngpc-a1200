/* script_input.h - input scripts in tools/ngpref's format, for host tools (host_test -i, sndrec_ngpc -i): lines
 * "first last BUTTONS", frames inclusive, buttons UP DOWN LEFT RIGHT A B OPTION. Host only. */
#ifndef SCRIPT_INPUT_H
#define SCRIPT_INPUT_H
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "logic.h"

typedef struct { unsigned first, last; unsigned short in; } ScriptLine;
static ScriptLine *script_lines;
static unsigned script_n;

static int script_load(const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    unsigned cap = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        static const struct { const char *name; unsigned short bit; } names[] = {
            { "UP", IN_UP }, { "DOWN", IN_DOWN }, { "LEFT", IN_LEFT }, { "RIGHT", IN_RIGHT }, { "A", IN_A }, { "B", IN_B }, { "OPTION", IN_OPTION } };
        ScriptLine s = { 0, 0, 0 };
        char *tok;
        unsigned k;
        if (sscanf(line, "%u %u", &s.first, &s.last) != 2) continue;
        tok = strtok(line, " \t\r\n"); tok = strtok(0, " \t\r\n");
        while ((tok = strtok(0, " \t\r\n")) != 0)
            for (k = 0; k < sizeof names / sizeof names[0]; k++) if (!strcmp(tok, names[k].name)) s.in |= names[k].bit;
        if (script_n == cap) { cap = cap ? cap * 2 : 256; script_lines = realloc(script_lines, cap * sizeof *script_lines); }
        script_lines[script_n++] = s;
    }
    fclose(f);
    return 1;
}

static unsigned short script_input(unsigned tick)        /* the buttons held in this frame */
{
    unsigned short in = 0;
    unsigned i;
    for (i = 0; i < script_n; i++) if (tick >= script_lines[i].first && tick <= script_lines[i].last) in |= script_lines[i].in;
    return in;
}
#endif
