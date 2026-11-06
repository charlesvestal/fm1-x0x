/* SPDX-License-Identifier: GPL-3.0-only */
/* The 303's rescaled drive (bass303.c), on the acid line its makeup tables were measured on
 * (tools/drive_line.h's patch 0, at Volume 96):
 *  - at every Drive, Soft and RAT, the K-weighted (BS.1770) level is Drive 0's plus the deliberate
 *    rise (3 dB at full Drive, linear in the pot) within 0.25 dB: the makeup tables still match
 *    the drive they were measured on;
 *  - Volume comes after the drive: at Drive 64 the output at Volume 127 is the output at Volume 64
 *    times one gain, sample for sample, on both types, and that gain is the two Volumes' (60/127 dB
 *    a step);
 *  - the drive coming on starts fresh: switched on mid-note after it ran at Drive 64 and then went
 *    off, its output is the output of one that never ran, sample for sample;
 *  - the migration of every old setting (bass303_pre_rescale), Soft and RAT: Drive 0 left as it
 *    was, the new values in range, and the new Drive no lower for more old Drive or old Volume, so
 *    that a lane sweeping either pot moves it one way. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "bass303.h"
#include "../../tools/drive_line.h"

#define SR 44100.0
#define PI 3.14159265358979

static bass303_t B;
static float buf[DRIVE_LINE_BUF], ref[DRIVE_LINE_BUF];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

/* Check bass303_pre_rescale over every old setting of drive type `type`. */
static void check_migration(int type, const char *name)
{
    static int nd[128][128], nv[128][128];
    int bad_range = 0, bad_d = 0, bad_v = 0;
    for (int v = 0; v < 128; v++)
        for (int d = 0; d < 128; d++)
            bass303_pre_rescale(type, v, d, &nd[v][d], &nv[v][d]);
    for (int v = 0; v < 128; v++) {
        CHECK(nd[v][0] == 0 && nv[v][0] == v, "%s: old Volume %d, Drive 0 moved (to %d, %d)", name, v,
              nv[v][0], nd[v][0]);
        for (int d = 1; d < 128; d++) {
            bad_range += nd[v][d] < 1 || nd[v][d] > 127 || nv[v][d] < 0 || nv[v][d] > 127;
            bad_d += d > 1 && nd[v][d] < nd[v][d - 1];
            bad_v += v > 0 && nd[v][d] < nd[v - 1][d];
        }
    }
    printf("  %-4s migration: %d settings out of range, %d where more old Drive lowers the new, %d where more"
           " old Volume does\n", name, bad_range, bad_d, bad_v);
    CHECK(bad_range == 0 && bad_d == 0 && bad_v == 0, "%s: the migration is in range and monotone", name);
}

/* Render the acid line at this drive type, Volume and Drive into out. */
static void render(float *out, int type, int vol, int drive)
{
    static const int from0 = 0;
    drive_line_render(&B, out, 0, type, vol, &drive, &from0, 1);
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
 * x from DRIVE_LINE_SEG0 on. */
static double k_level(const float *x)
{
    static double y[DRIVE_LINE_LEN];
    const int n = DRIVE_LINE_LEN - DRIVE_LINE_SEG0;
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
        y[i] = x[DRIVE_LINE_SEG0 + i];
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
        for (int i = 0; i < DRIVE_LINE_LEN; i++) {
            num += (double)buf[i] * ref[i];
            den += (double)ref[i] * ref[i];
        }
        g = num / den;                               /* the gain that fits best */
        for (int i = 0; i < DRIVE_LINE_LEN; i++) {
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
        drive_line_render(&B, buf, 0, TYPES[k].type, 96, ran, ran_from, 3);
        drive_line_render(&B, ref, 0, TYPES[k].type, 96, fresh, fresh_from, 2);
        for (int i = 110240; i < DRIVE_LINE_LEN; i++)
            diff += buf[i] != ref[i];
        printf("  %-4s on again after Drive 64: %d samples differ from a fresh start\n", TYPES[k].name, diff);
        CHECK(diff == 0, "%s: the drive comes on fresh (%d samples differ)", TYPES[k].name, diff);
    }
    check_migration(BASS303_DRV_SOFT, "Soft");
    check_migration(BASS303_DRV_RAT, "RAT");
    printf(fails ? "bass303_drive: %d FAILED\n" : "bass303_drive: ok\n", fails);
    return fails != 0;
}
