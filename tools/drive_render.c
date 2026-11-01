/* SPDX-License-Identifier: GPL-3.0-only */
/* A 303 acid line (16 steps, 64 at 120 BPM: 8 s) through dsp/bass303.c at one drive setting, for
 * tools/fit_drive_migration.py, which builds it twice: against this firmware's bass303.c and against
 * the one from before the drive was rescaled.
 *   drive_render TYPE VOLUME DRIVE [PATCH]  TYPE: 1 Soft, 2 RAT; PATCH: an index into PATCHES (0)
 *   drive_render map TYPE                   (built with -DDRIVE_MAP, against this firmware's bass303.c)
 *                                           the migration of every old setting of that type: for each
 *                                           old Volume, each old Drive, a line "new Drive, new Volume"
 * Patch 0 is the one the drive's makeup tables were measured on. Writes to stdout: the length n
 * (int32), then n samples (float32). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bass303.h"

#define STEP 5512.5                          /* a 16th at 120 BPM, in samples */
#define NSTEP 64
#define BLK 256

typedef struct { int note, acc, slide; } step_t;
static const step_t PAT[16] = {
    {36, 1, 0}, {36, 0, 0}, {48, 0, 1}, {46, 0, 0}, {36, 1, 0}, {39, 0, 0}, {36, 0, 1}, {41, 1, 0},
    {36, 0, 0}, {43, 0, 0}, {48, 1, 1}, {46, 0, 0}, {36, 0, 0}, {36, 1, 0}, {39, 0, 1}, {41, 0, 0},
};
static bass303_t B;

/* The patches: wave, cutoff, resonance, env mod, decay, accent. 0 is the one the makeup tables were
 * measured on. */
static const struct { const char *name; int wave, cutoff, reso, envmod, decay, accent; } PATCHES[] = {
    {"acid", 0, 45, 95, 85, 45, 90},       /* dark and resonant, the env mod high */
    {"bright", 1, 90, 40, 50, 45, 40},     /* square, open, little resonance */
    {"mellow", 0, 20, 30, 30, 60, 50},     /* closed, little resonance or env mod */
    {"squelch", 0, 70, 120, 110, 30, 110}, /* open, resonance and env mod near the top */
    {"sqrdark", 1, 35, 70, 60, 50, 70},    /* square, fairly closed */
};
#define NPATCH (int)(sizeof PATCHES / sizeof PATCHES[0])

int main(int argc, char **argv)
{
#ifdef DRIVE_MAP
    if (argc == 3 && !strcmp(argv[1], "map")) {
        for (int v = 0; v < 128; v++)
            for (int d = 0; d < 128; d++) {
                int nd, nv;
                bass303_pre_rescale(atoi(argv[2]), v, d, &nd, &nv);
                printf("%d %d\n", nd, nv);
            }
        return 0;
    }
#endif
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: drive_render TYPE VOLUME DRIVE [PATCH]\n");
        return 2;
    }
    const int pt = argc == 5 ? atoi(argv[4]) : 0;
    if (pt < 0 || pt >= NPATCH) {
        fprintf(stderr, "drive_render: PATCH 0..%d\n", NPATCH - 1);
        return 2;
    }
    const int total = (int)(STEP * NSTEP);
    float *out = calloc((size_t)total + BLK, sizeof *out);
    long next = 0;
    int k = 0, gate_off = -1, prev_slide = 0;
    bass303_init(&B);
    bass303_set(&B, BASS303_WAVE, PATCHES[pt].wave);
    bass303_set(&B, BASS303_CUTOFF, PATCHES[pt].cutoff);
    bass303_set(&B, BASS303_RESO, PATCHES[pt].reso);
    bass303_set(&B, BASS303_ENVMOD, PATCHES[pt].envmod);
    bass303_set(&B, BASS303_DECAY, PATCHES[pt].decay);
    bass303_set(&B, BASS303_ACCENT, PATCHES[pt].accent);
    bass303_set(&B, BASS303_VOLUME, atoi(argv[2]));
    bass303_set(&B, BASS303_DRVTYPE, atoi(argv[1]));
    bass303_set(&B, BASS303_DRIVE, atoi(argv[3]));
    for (int pos = 0; pos < total; pos += BLK)
        for (int i = 0; i < BLK; i += 32) {          /* notes on a 32-sample grid */
            int t = pos + i;
            if (gate_off >= 0 && t >= gate_off) {
                bass303_note_off(&B);
                gate_off = -1;
            }
            if (t >= next) {
                const step_t *s = &PAT[k % 16];
                bass303_note_on(&B, s->note, s->acc, prev_slide);
                gate_off = s->slide ? -1 : (int)(next + STEP / 2);
                prev_slide = s->slide;
                next = (long)(++k * STEP);
            }
            bass303_render(&B, out + t, 32);
        }
    fwrite(&total, sizeof total, 1, stdout);
    fwrite(out, sizeof *out, (size_t)total, stdout);
    return 0;
}
