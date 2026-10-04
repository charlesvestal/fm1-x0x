/* SPDX-License-Identifier: GPL-3.0-only */
/* The overload guard's lite 303 (bass303_set_lite: no oversampling) against the normal one, on a
 * bright, resonant held note: the same pitch (zero crossings), the level within 1.5 dB, and
 * switching in the middle of the note (both ways) jumps no more than the note itself moves. */
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "bass303.h"

static float buf[44100 * 2];
static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void note(bass303_t *b, int lite)
{
    bass303_init(b);
    bass303_set(b, BASS303_CUTOFF, 90);
    bass303_set(b, BASS303_RESO, 110);
    bass303_set(b, BASS303_ENVMOD, 30);
    bass303_set_lite(b, lite);
    bass303_note_on(b, 45, 1, 0);                    /* A2, accented: the resonance adds crossings of its own, hence 10% */
}
static void stats(const float *x, int n, double *rms, int *zc, float *step)
{
    int i;
    double s = 0;
    *zc = 0;
    *step = 0;
    for (i = 1; i < n; i++) {
        s += (double)x[i] * x[i];
        *zc += (x[i - 1] < 0) != (x[i] < 0);
        if (fabsf(x[i] - x[i - 1]) > *step)
            *step = fabsf(x[i] - x[i - 1]);
    }
    *rms = sqrt(s / n);
}

int main(void)
{
    static bass303_t b;
    double r2, r1;
    int z2, z1, i, sw;
    float s2, s1;
    note(&b, 0);
    for (i = 0; i < 44100; i += 128)
        bass303_render(&b, buf + i, 128);
    stats(buf + 4410, 44100 - 4410, &r2, &z2, &s2);
    note(&b, 1);
    for (i = 0; i < 44100; i += 128)
        bass303_render(&b, buf + i, 128);
    stats(buf + 4410, 44100 - 4410, &r1, &z1, &s1);
    printf("2x: rms %.4f, %d zero crossings, biggest step %.3f; lite: rms %.4f, %d, %.3f\n", r2, z2, s2, r1, z1, s1);
    CHECK(fabs(20 * log10(r1 / r2)) < 1.5, "lite level %.2f dB off", 20 * log10(r1 / r2));
    CHECK(abs(z1 - z2) <= z2 / 10, "lite pitch: %d zero crossings against %d", z1, z2);
    for (sw = 0; sw < 2; sw++) {                     /* 2x -> lite and lite -> 2x in the note */
        float big = 0;
        note(&b, sw);
        for (i = 0; i < 44100; i += 128) {
            if (i == 22016)
                bass303_set_lite(&b, !sw);
            bass303_render(&b, buf + i, 128);
        }
        for (i = 22016 - 512; i < 22016 + 512; i++)
            if (fabsf(buf[i] - buf[i - 1]) > big)
                big = fabsf(buf[i] - buf[i - 1]);
        CHECK(big <= 1.5f * (s1 > s2 ? s1 : s2), "switch %s: a %.3f jump at the switch", sw ? "lite->2x" : "2x->lite", big);
    }
    printf(fails ? "bass303 lite: %d FAILED\n" : "bass303 lite: ok\n", fails);
    return fails != 0;
}
