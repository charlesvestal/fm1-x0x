/* SPDX-License-Identifier: GPL-3.0-only */
/* The master compressor: bypass is exact, the static curve, attack / release times,
 * makeup, parallel mix, and the kick-keyed PUMP. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../../firmware/src/dsp/master.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } else { printf("  ok   "); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define N 44100

static float buf[N * 2];
static master_t M;

static void setup(int thr, int ratio, int att, int rel)
{
    master_init(&M);
    master_set(&M, MST_LIMIT, 0);
    master_set(&M, MST_MODE, 0);
    master_set(&M, MST_THRESH, thr);
    master_set(&M, MST_RATIO, ratio);
    master_set(&M, MST_ATTACK, att);
    master_set(&M, MST_RELEASE, rel);
}

static void sine(float *x, int n, float db, int start)
{
    float a = powf(10.0f, db / 20.0f);
    int i;
    for (i = 0; i < n; i++)
        x[i] = a * sinf(2.0f * 3.14159265f * 1000.0f * (float)(i + start) / 44100.0f);
}

static void run(float *x, int n)
{
    int i;
    for (i = 0; i < n; i += 256)
        master_process(&M, x + i, n - i < 256 ? n - i : 256, 1.0f);
}

static float peak_db(const float *x, int n)
{
    float p = 0;
    int i;
    for (i = 0; i < n; i++)
        if (fabsf(x[i]) > p)
            p = fabsf(x[i]);
    return 20.0f * log10f(p + 1e-12f);
}

int main(void)
{
    int i;
    float ref[N];
    /* 1. bypass: ratio 1:1, PUMP 0, filter off, limiter off: the samples, exactly */
    setup(95, 0, 60, 60);
    sine(buf, N, -3.0f, 0);
    memcpy(ref, buf, sizeof ref);
    run(buf, N);
    CHECK(!memcmp(ref, buf, sizeof ref), "bypass at 1:1 with PUMP 0 is sample-exact");

    /* 2. static curve: -6 dBFS sine, threshold -24 dB, 4:1 -> peak at -24 + 18/4 = -19.5 dB */
    {
        int thr = (int)lroundf((-24.0f + 48.0f) * 127.0f / 48.0f);
        setup(thr, 4, 40, 90);
        sine(buf, 2 * N, -6.0f, 0);
        run(buf, 2 * N);
        float out = peak_db(buf + N, N);
        CHECK(fabsf(out - (-19.5f)) < 1.0f, "4:1 above a -24 dB threshold: -6 dB in -> %.2f dB out (want -19.5)", out);
        /* below the threshold (and below the knee): untouched */
        setup(thr, 4, 40, 90);
        sine(buf, N, -36.0f, 0);
        memcpy(ref, buf, sizeof ref);
        run(buf, N);
        CHECK(fabsf(peak_db(buf, N) - peak_db(ref, N)) < 0.01f, "-36 dB in, under the threshold: unchanged");
        /* INF: a limiter at the threshold (+ the knee's half) */
        setup(thr, 9, 40, 90);
        sine(buf, 2 * N, -6.0f, 0);
        run(buf, 2 * N);
        out = peak_db(buf + N, N);
        CHECK(out < -23.0f && out > -25.5f, "INF ratio holds -6 dB in to %.2f dB (threshold -24)", out);
    }

    /* 3. attack: a step from -40 to -6 dB; the gain reduction reaches 63% of its final value
     *    after about the attack time */
    {
        int thr = (int)lroundf(24.0f * 127.0f / 48.0f), att = 100;      /* -24 dB; attack pot 100 */
        float t_ms = 0.1f * powf(2.0f, 100.0f * 9.966f / 127.0f), final_gr = 0, hit = -1;
        setup(thr, 9, att, 127);
        for (i = 0; i < N; i++)
            buf[i] = 0.5f;                                               /* DC at -6 dB: |x| constant */
        run(buf, 64);
        final_gr = 18.0f;
        setup(thr, 9, att, 127);
        for (i = 0; i < N; i++) {
            float x = 0.5f;
            master_process(&M, &x, 1, 1.0f);
            if (hit < 0 && M.gr >= 0.632f * final_gr)
                hit = (float)i * 1000.0f / 44100.0f;
        }
        CHECK(hit > 0.7f * t_ms && hit < 1.3f * t_ms, "attack: 63%% of the reduction after %.1f ms (attack %.1f ms)", hit, t_ms);
    }

    /* 4. makeup and mix */
    {
        int thr = (int)lroundf(24.0f * 127.0f / 48.0f);
        float wet, half;
        setup(thr, 4, 40, 90);
        master_set(&M, MST_MAKEUP, 64);                                  /* +12.1 dB (24 dB would clip) */
        sine(buf, 2 * N, -6.0f, 0);
        run(buf, 2 * N);
        wet = peak_db(buf + N, N);
        CHECK(fabsf(wet - (-19.5f + 24.0f * 64.0f / 127.0f)) < 1.0f, "makeup +12 dB: %.2f dB (want -7.4)", wet);
        setup(thr, 4, 40, 90);
        master_set(&M, MST_MIX, 64);                                     /* about half wet */
        sine(buf, 2 * N, -6.0f, 0);
        run(buf, 2 * N);
        half = peak_db(buf + N, N);
        CHECK(half > -12.0f && half < -6.5f, "parallel mix 50%%: %.2f dB, between wet -19.5 and dry -6", half);
    }

    /* 5. PUMP: a kick ducks the mix by its depth within ~5 ms, then swells back */
    {
        float before, dip, later;
        setup(127, 0, 60, 60);                                           /* ratio 1:1: PUMP alone */
        master_set(&M, MST_PUMP, 64);                                    /* ~12 dB */
        master_set(&M, MST_PUMPSRC, PUMP_909);
        sine(buf, N, -6.0f, 0);
        run(buf, 4410);
        before = peak_db(buf + 2205, 2205);
        master_key(&M, 1, 1.0f);                                         /* an 808 kick: not the source */
        sine(buf, 441, -6.0f, 4410);
        run(buf, 441);
        CHECK(fabsf(peak_db(buf + 220, 220) - before) < 0.1f, "PUMP keyed by 909 ignores an 808 kick");
        master_key(&M, 0, 1.0f);
        sine(buf, N, -6.0f, 4851);
        run(buf, N);
        dip = peak_db(buf + 330, 110);                                   /* 7.5 - 10 ms after the kick */
        later = peak_db(buf + 40000, 1000);                              /* ~0.9 s later */
        CHECK(before - dip > 9.0f && before - dip < 14.0f, "a 909 kick ducks %.1f dB (depth 12) within 10 ms", before - dip);
        CHECK(before - later < 0.5f, "... and the mix has swelled back after the release (%.2f dB down)", before - later);
    }
    printf("master: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
