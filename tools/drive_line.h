/* SPDX-License-Identifier: GPL-3.0-only */
/* The acid line the 303 drive's tools and test play: tools/drive_render.c (which
 * tools/fit_drive_migration.py and tools/measure_drive_makeup.py run) and
 * tests/host/bass303_drive_test.c. 16 steps, played 64 times a 16th at 120 BPM (8 s), on one of
 * DRIVE_PATCHES; levels are measured from step 16 (DRIVE_LINE_SEG0), past the first bar's
 * settling. The drive's makeup tables were measured on patch 0. Include after bass303.h. */
#ifndef DRIVE_LINE_H
#define DRIVE_LINE_H

#define DRIVE_LINE_STEP 5512.5                 /* a 16th at 120 BPM, in samples */
#define DRIVE_LINE_LEN 352800                  /* 64 steps */
#define DRIVE_LINE_SEG0 88200                  /* step 16 */
#define DRIVE_LINE_BUF (DRIVE_LINE_LEN + 256)  /* what drive_line_render writes: whole blocks */

static const struct { int note, acc, slide; } DRIVE_LINE[16] = {
    {36, 1, 0}, {36, 0, 0}, {48, 0, 1}, {46, 0, 0}, {36, 1, 0}, {39, 0, 0}, {36, 0, 1}, {41, 1, 0},
    {36, 0, 0}, {43, 0, 0}, {48, 1, 1}, {46, 0, 0}, {36, 0, 0}, {36, 1, 0}, {39, 0, 1}, {41, 0, 0},
};

/* The patches: wave, cutoff, resonance, env mod, decay, accent. */
static const struct {
    const char *name;
    int wave, cutoff, reso, envmod, decay, accent;
} DRIVE_PATCHES[] = {
    {"acid", 0, 45, 95, 85, 45, 90},       /* dark and resonant, the env mod high */
    {"bright", 1, 90, 40, 50, 45, 40},     /* square, open, little resonance */
    {"mellow", 0, 20, 30, 30, 60, 50},     /* closed, little resonance or env mod */
    {"squelch", 0, 70, 120, 110, 30, 110}, /* open, resonance and env mod near the top */
    {"sqrdark", 1, 35, 70, 60, 50, 70},    /* square, fairly closed */
};
#define DRIVE_NPATCH (int)(sizeof DRIVE_PATCHES / sizeof DRIVE_PATCHES[0])

/* Render the line on patch `patch` through b at drive type `type` and Volume `vol` into out
 * (DRIVE_LINE_BUF samples), with Drive drives[j] from sample from[j] (from[0] = 0, then rising;
 * a change lands on the 32-sample grid the notes are on). */
static void drive_line_render(bass303_t *b, float *out, int patch, int type, int vol, const int *drives,
                              const int *from, int n)
{
    long next = 0;
    int j = 0, k = 0, gate_off = -1, prev_slide = 0;
    bass303_init(b);
    bass303_set(b, BASS303_WAVE, DRIVE_PATCHES[patch].wave);
    bass303_set(b, BASS303_CUTOFF, DRIVE_PATCHES[patch].cutoff);
    bass303_set(b, BASS303_RESO, DRIVE_PATCHES[patch].reso);
    bass303_set(b, BASS303_ENVMOD, DRIVE_PATCHES[patch].envmod);
    bass303_set(b, BASS303_DECAY, DRIVE_PATCHES[patch].decay);
    bass303_set(b, BASS303_ACCENT, DRIVE_PATCHES[patch].accent);
    bass303_set(b, BASS303_VOLUME, vol);
    bass303_set(b, BASS303_DRVTYPE, type);
    for (int pos = 0; pos < DRIVE_LINE_LEN; pos += 256)
        for (int i = 0; i < 256; i += 32) {
            int t = pos + i;
            for (; j < n && t >= from[j]; j++)
                bass303_set(b, BASS303_DRIVE, drives[j]);
            if (gate_off >= 0 && t >= gate_off) {
                bass303_note_off(b);
                gate_off = -1;
            }
            if (t >= next) {
                bass303_note_on(b, DRIVE_LINE[k % 16].note, DRIVE_LINE[k % 16].acc, prev_slide);
                gate_off = DRIVE_LINE[k % 16].slide ? -1 : (int)(next + DRIVE_LINE_STEP / 2);
                prev_slide = DRIVE_LINE[k % 16].slide;
                next = (long)(++k * DRIVE_LINE_STEP);
            }
            bass303_render(b, out + t, 32);
        }
}

#endif
