/* SPDX-License-Identifier: GPL-3.0-only */
/* The 303's rescaled drive (bass303.c), on the acid line its makeup tables were measured on (cutoff
 * 45, resonance 95, env mod 85, decay 45, accent 90, saw, Volume 96; tools/drive_render.c's):
 *  - at every Drive, Soft and RAT, the K-weighted (BS.1770) level is Drive 0's plus the deliberate
 *    rise (3 dB at full Drive, linear in the pot) within 0.25 dB: the makeup tables still match
 *    the drive they were measured on;
 *  - Volume comes after the drive: at Drive 64 the output at Volume 127 is the output at Volume 64
 *    times one gain, sample for sample, on both types, and that gain is the two Volumes' (60/127 dB
 *    a step);
 *  - the drive coming on starts fresh: switched on mid-note after it ran at Drive 64 and then went
 *    off, its output is the output of one that never ran, sample for sample. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "bass303.h"

#define SR 44100.0
#define STEP 5512.5                            /* a 16th at 120 BPM, in samples */
#define TOTAL 352800                           /* 64 steps */
#define SEG0 88200                             /* measured from step 16: past the first bar's settling */
#define BLK 256
#define PI 3.14159265358979

typedef struct { int note, acc, slide; } step_t;
static const step_t PAT[16] = {
    {36, 1, 0}, {36, 0, 0}, {48, 0, 1}, {46, 0, 0}, {36, 1, 0}, {39, 0, 0}, {36, 0, 1}, {41, 1, 0},
    {36, 0, 0}, {43, 0, 0}, {48, 1, 1}, {46, 0, 0}, {36, 0, 0}, {36, 1, 0}, {39, 0, 1}, {41, 0, 0},
};
static bass303_t B;
static float buf[TOTAL + BLK], ref[TOTAL + BLK];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* Render the acid line at this drive type and Volume into out, with Drive drives[j] from sample
 * from[j] (from[0] = 0, then rising, on the 32-sample grid). */
static void render_seq(float *out, int type, int vol, const int *drives, const int *from, int n)
{
    int j = 0;
    long next = 0;
    int k = 0, gate_off = -1, prev_slide = 0;
    bass303_init(&B);
    bass303_set(&B, BASS303_CUTOFF, 45);
    bass303_set(&B, BASS303_RESO, 95);
    bass303_set(&B, BASS303_ENVMOD, 85);
    bass303_set(&B, BASS303_DECAY, 45);
    bass303_set(&B, BASS303_ACCENT, 90);
    bass303_set(&B, BASS303_VOLUME, vol);
    bass303_set(&B, BASS303_DRVTYPE, type);
    for (int pos = 0; pos < TOTAL; pos += BLK)
        for (int i = 0; i < BLK; i += 32) {           /* notes on a 32-sample grid */
            int t = pos + i;
            for (; j < n && t >= from[j]; j++)
                bass303_set(&B, BASS303_DRIVE, drives[j]);
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
}

/* Render the acid line at this drive type, Volume and Drive into out. */
static void render(float *out, int type, int vol, int drive)
{
    static const int from0 = 0;
    render_seq(out, type, vol, &drive, &from0, 1);
}

/* Run one biquad (b, a normalised so that a[0] = 1) over x in place. */
static void biquad(double *x, int n, const double b[3], const double a[3])
{
    double z1 = 0, z2 = 0;
    for (int i = 0; i < n; i++) {
        double y = b[0] * x[i] + z1;
        z1 = b[1] * x[i] - a[1] * y + z2;
        z2 = b[2] * x[i] - a[2] * y;
        x[i] = y;
    }
}

/* Return the K-weighted (ITU-R BS.1770: a +4 dB shelf at 1682 Hz, a 38 Hz highpass) level in dB of
 * x from SEG0 on. */
static double k_level(const float *x)
{
    static double y[TOTAL];
    const int n = TOTAL - SEG0;
    const double A = pow(10.0, 4.0 / 40.0), w = 2 * PI * 1681.97 / SR, c = cos(w);
    const double al = sin(w) / 2 * sqrt((A + 1 / A) * (1 / 0.7072 - 1) + 2), sa = 2 * sqrt(A) * al;
    const double a0 = (A + 1) - (A - 1) * c + sa;
    const double sb[3] = {A * ((A + 1) + (A - 1) * c + sa) / a0, -2 * A * ((A - 1) + (A + 1) * c) / a0,
                          A * ((A + 1) + (A - 1) * c - sa) / a0};
    const double sh[3] = {1, 2 * ((A - 1) - (A + 1) * c) / a0, ((A + 1) - (A - 1) * c - sa) / a0};
    const double w2 = 2 * PI * 38.13 / SR, c2 = cos(w2), al2 = sin(w2) / (2 * 0.5003), h0 = 1 + al2;
    const double hb[3] = {(1 + c2) / 2 / h0, -(1 + c2) / h0, (1 + c2) / 2 / h0};
    const double ha[3] = {1, -2 * c2 / h0, (1 - al2) / h0};
    double s = 0;
    for (int i = 0; i < n; i++)
        y[i] = x[SEG0 + i];
    biquad(y, n, sb, sh);
    biquad(y, n, hb, ha);
    for (int i = 0; i < n; i++)
        s += y[i] * y[i];
    return 10 * log10(s / n);
}

int main(void)
{
    static const struct { int type; const char *name; double rise; } TYPES[2] = {
        {BASS303_DRV_SOFT, "Soft", 3.0}, {BASS303_DRV_RAT, "RAT", 3.0}};
    for (int k = 0; k < 2; k++) {
        double l0, worst = 0;
        int at = 0;
        render(buf, TYPES[k].type, 96, 0);
        l0 = k_level(buf);
        for (int d = 1; d <= 127; d++) {
            double e;
            render(buf, TYPES[k].type, 96, d);
            e = k_level(buf) - l0 - TYPES[k].rise * d / 127.0;
            if (fabs(e) > fabs(worst)) {
                worst = e;
                at = d;
            }
        }
        printf("  %-4s level against Drive 0 plus the rise: worst %+.2f dB (Drive %d)\n", TYPES[k].name, worst, at);
        CHECK(fabs(worst) <= 0.25, "%s: the makeup keeps the level (Drive %d: %+.2f dB)", TYPES[k].name, at, worst);
    }
    for (int k = 0; k < 2; k++) {
        double num = 0, den = 0, res = 0, pk = 0, g;
        render(ref, TYPES[k].type, 64, 64);
        render(buf, TYPES[k].type, 127, 64);
        for (int i = 0; i < TOTAL; i++) {
            num += (double)buf[i] * ref[i];
            den += (double)ref[i] * ref[i];
        }
        g = num / den;                               /* the gain that fits best */
        for (int i = 0; i < TOTAL; i++) {
            double r = fabs(buf[i] - g * ref[i]);
            res = r > res ? r : res;
            pk = fabs(buf[i]) > pk ? fabs(buf[i]) : pk;
        }
        printf("  %-4s Volume 127 against 64 at Drive 64: gain %.3f dB (the Volumes': %.3f), residual %.1e of the peak\n",
               TYPES[k].name, 20 * log10(g), 60.0 * 63.0 / 127.0, res / pk);
        CHECK(res <= 1e-5 * pk, "%s: Volume after the drive (residual %.1e of the peak)", TYPES[k].name, res / pk);
        CHECK(fabs(20 * log10(g) - 60.0 * 63.0 / 127.0) < 0.01, "%s: Volume's gain (%.3f dB)", TYPES[k].name, 20 * log10(g));
    }
    for (int k = 0; k < 2; k++) {
        static const int ran[3] = {64, 0, 5}, ran_from[3] = {0, 88192, 110240};   /* on at 2.5 s */
        static const int fresh[2] = {0, 5}, fresh_from[2] = {0, 110240};
        int diff = 0;
        render_seq(buf, TYPES[k].type, 96, ran, ran_from, 3);
        render_seq(ref, TYPES[k].type, 96, fresh, fresh_from, 2);
        for (int i = 110240; i < TOTAL; i++)
            diff += buf[i] != ref[i];
        printf("  %-4s on again after Drive 64: %d samples differ from a fresh start\n", TYPES[k].name, diff);
        CHECK(diff == 0, "%s: the drive comes on fresh (%d samples differ)", TYPES[k].name, diff);
    }
    printf(fails ? "bass303_drive: %d FAILED\n" : "bass303_drive: ok\n", fails);
    return fails != 0;
}
