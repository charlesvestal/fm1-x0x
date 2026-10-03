/* SPDX-License-Identifier: GPL-3.0-only */
/* The BREAK part's built-in loops, played by this firmware's own 909 (dsp/drum909.c) on
 * the host at build time: two bars each at 96 BPM, a groove (A) and a busier fill (B).
 * No recording ships: the loops are this code's output. Written as IMA ADPCM, mono,
 * 22050 Hz, the format of the user sample slots.
 *   gen_builtin_break OUT.h */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsp/drum909.h"

#define BPM 96.0
#define RATE 44100
#define BARS 2
#define STEPS (16 * BARS)

/* step strings: x = hit, X = accented hit, g = ghost (soft), . = rest */
typedef struct { int voice; const char *s; } lane_t;
static const lane_t GROOVE[] = {
    {DR_BD, "X.....x...x.....X.....x..x......"},
    {DR_SD, "....X..g.g..X..g....X..g.g..X.g."},
    {DR_CH, "x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x."},
    {DR_OH, "...............x...............x"},
    {DR_RD, "................................"},
};
static const lane_t FILL[] = {
    {DR_BD, "X.....x...x.x...X..x..x...x.x..."},
    {DR_SD, "....X..g.g.gX.gg....X.gX.gX.XgXX"},
    {DR_CH, "x.x.x.xxx.x.x.x.x.xxx.x.x.x....."},
    {DR_OH, ".......x.......x.......x........"},
    {DR_LT, "............................x..."},
    {DR_MT, "..........................x....."},
    {DR_HT, "........................x......."},
    {DR_CR, "................................"},
};

static int16_t *render(const lane_t *L, int nl, uint32_t *frames)
{
    static drum909_t d;
    float dry[256], rev[256], dly[256];
    double spb = RATE * 60.0 / BPM / 4.0;            /* samples per 16th */
    uint32_t total = (uint32_t)(spb * STEPS + 0.5), pos = 0, i;
    int16_t *out = malloc(sizeof(int16_t) * total);
    int step = 0;
    drum909_init(&d);
    while (pos < total) {
        uint32_t next = step < STEPS ? (uint32_t)(spb * step + 0.5) : total, n;
        if (pos >= next && step < STEPS) {
            int l;
            for (l = 0; l < nl; l++) {
                char c = L[l].s[step];
                if (c == 'x' || c == 'X' || c == 'g')
                    drum909_trigger(&d, L[l].voice, c == 'X' ? 1.0f : c == 'g' ? 0.35f : 0.75f);
            }
            step++;
            continue;
        }
        n = next - pos;
        if (n > 256)
            n = 256;
        memset(dry, 0, sizeof dry);
        memset(rev, 0, sizeof rev);
        memset(dly, 0, sizeof dly);
        drum909_render(&d, dry, rev, dly, (int)n);
        for (i = 0; i < n; i++) {
            float v = dry[i] * 1.6f;                   /* a loop is normalised, like a sampled break */
            v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
            out[pos + i] = (int16_t)(v * 32767.0f);
        }
        pos += n;
    }
    *frames = total;
    return out;
}

static const int16_t STEP_T[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767};
static const int8_t IDX_T[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

/* 44.1k -> 22.05k (2-tap average) then IMA ADPCM from predictor 0 / index 0 (tools/sampleio.py's encoder) */
static uint8_t *adpcm(const int16_t *x, uint32_t n, uint32_t *ns)
{
    uint32_t m = n / 2, i;
    uint8_t *o = calloc(1, m / 2 + 1);
    int pred = 0, idx = 0;
    for (i = 0; i < m; i++) {
        int s = (x[2 * i] + x[2 * i + 1]) / 2, step = STEP_T[idx], diff = s - pred, code = 0, vd = step >> 3;
        if (diff < 0) {
            code = 8;
            diff = -diff;
        }
        if (diff >= step) { code |= 4; diff -= step; vd += step; }
        if (diff >= step >> 1) { code |= 2; diff -= step >> 1; vd += step >> 1; }
        if (diff >= step >> 2) { code |= 1; vd += step >> 2; }
        pred = code & 8 ? pred - vd : pred + vd;
        pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
        idx += IDX_T[code & 7];
        idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
        if (i & 1)
            o[i >> 1] |= (uint8_t)(code << 4);
        else
            o[i >> 1] = (uint8_t)code;
    }
    *ns = m;
    return o;
}

static void emit(FILE *f, const char *name, const uint8_t *d, uint32_t ns)
{
    uint32_t i, nb = (ns + 1) / 2;
    fprintf(f, "#define %s_N %uu\nstatic const uint8_t %s[%u] = {", name, ns, name, nb);
    for (i = 0; i < nb; i++)
        fprintf(f, "%s%u,", i % 24 ? "" : "\n    ", d[i]);
    fprintf(f, "\n};\n");
}

int main(int argc, char **argv)
{
    uint32_t fa, fb, na, nb;
    int16_t *a = render(GROOVE, (int)(sizeof GROOVE / sizeof GROOVE[0]), &fa);
    int16_t *b = render(FILL, (int)(sizeof FILL / sizeof FILL[0]), &fb);
    uint8_t *ea = adpcm(a, fa, &na), *eb = adpcm(b, fb, &nb);
    FILE *f = fopen(argc > 1 ? argv[1] : "x0x_builtin_break.h", "w");
    if (!f)
        return 1;
    fprintf(f, "/* Generated by tools/gen_builtin_break.c -- do not edit. Two bars at %g BPM played by\n"
               " * this firmware's 909 port; IMA ADPCM, mono, 22050 Hz. */\n#pragma once\n#include <stdint.h>\n"
               "#define X0X_BREAK_RATE 22050u\n#define X0X_BREAK_A_BARS %d\n#define X0X_BREAK_B_BARS %d\n",
            BPM, BARS, BARS);
    emit(f, "X0X_BREAK_A", ea, na);
    emit(f, "X0X_BREAK_B", eb, nb);
    fclose(f);
    printf("builtin break: A %u + B %u samples, %u bytes of flash\n", na, nb, (na + 1) / 2 + (nb + 1) / 2);
    return 0;
}
