/* SPDX-License-Identifier: GPL-3.0-only */
/* The 303's Drive, Volume and drive type turned while a note sounds: no zipper or click. A held, dark note (a
 * near-sine at 65 Hz, so a click stands out above its own highs) is rendered while a pot moves the way a knob
 * or a lane moves it (slow and fast Drive sweeps, Soft and RAT; Drive 0 <-> 1 and 0 <-> 64, where the drive is
 * switched in and out; Volume, with the drive off and on; the drive type), set on a 32-sample grid. Each
 * 64-sample window's energy above 4 kHz is compared with the most that any setting the pot passed through in
 * the window (and the 120 ms before it, the slew's lag) has on its own, rendered with the pot held there: the
 * excess, in dB, is what the movement adds (energy below -80 dBFS counts as -80): a click or a gain step
 * shows as tens of dB (20-40 before the slew), a smooth change as none. Also checks that a setting reached by a
 * sweep, as a lane reaches it, sounds as one set from the start, sample for sample, once the slew has settled. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "bass303.h"

#define SR 44100
#define LEN 88192                                      /* 2 s, whole blocks and windows */
#define BLK 32
#define WIN 64
#define NW (LEN / WIN)
#define LAG 5292                                       /* 120 ms */
#define FLOOR (1e-4 * 1e-4 * WIN)                      /* -80 dBFS in the band */
#define NSTATE 1024

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

typedef struct { int type, drive, vol; } state_t;
typedef state_t (*sweep_fn)(int t);                     /* the pots at sample t */

static bass303_t B;
static float out[LEN];
static state_t cache_st[NSTATE];
static float cache_e[NSTATE][NW];
static int ncache;

static state_t norm(state_t s)
{
    if (s.type == BASS303_DRV_OFF || s.drive == 0)
        s.type = BASS303_DRV_OFF, s.drive = 0;
    return s;
}

/* Render LEN samples of the held note with the pots from f, set every BLK samples. */
static void render(sweep_fn f, float *y)
{
    state_t s = f(0);
    bass303_init(&B);
    bass303_set(&B, BASS303_CUTOFF, 0);
    bass303_set(&B, BASS303_RESO, 0);
    bass303_set(&B, BASS303_ENVMOD, 0);
    bass303_set(&B, BASS303_DECAY, 127);
    bass303_set(&B, BASS303_DRVTYPE, s.type);
    bass303_set(&B, BASS303_DRIVE, s.drive);
    bass303_set(&B, BASS303_VOLUME, s.vol);
    bass303_note_on(&B, 36, 0, 0);
    for (int t = 0; t < LEN; t += BLK) {
        s = f(t);
        if (bass303_get(&B, BASS303_DRVTYPE) != s.type)
            bass303_set(&B, BASS303_DRVTYPE, s.type);
        if (bass303_get(&B, BASS303_DRIVE) != s.drive)
            bass303_set(&B, BASS303_DRIVE, s.drive);
        if (bass303_get(&B, BASS303_VOLUME) != s.vol)
            bass303_set(&B, BASS303_VOLUME, s.vol);
        bass303_render(&B, y + t, BLK);
    }
}

/* The energy above 4 kHz (two 2nd-order Butterworth highpasses) of y, per window. */
static void hf_energy(const float *y, float *e)
{
    const double w = 2 * 3.14159265358979 * 4000.0 / SR, c = cos(w), al = sin(w) / (2 * 0.70710678);
    const double b0 = (1 + c) / 2 / (1 + al), b1 = -(1 + c) / (1 + al), a1 = -2 * c / (1 + al), a2 = (1 - al) / (1 + al);
    double z[2][2] = {{0, 0}, {0, 0}};
    for (int k = 0; k < NW; k++) {
        double s = 0;
        for (int i = k * WIN; i < (k + 1) * WIN; i++) {
            double x = y[i];
            for (int st = 0; st < 2; st++) {
                double v = b0 * x + z[st][0];
                z[st][0] = b1 * x - a1 * v + z[st][1];
                z[st][1] = b0 * x - a2 * v;
                x = v;
            }
            s += x * x;
        }
        e[k] = (float)s;
    }
}

static state_t held_state;
static state_t held(int t) { (void)t; return held_state; }

/* The windows' HF energy of the note with the pots held at s (rendered once). */
static const float *static_e(state_t s)
{
    static float y[LEN];
    s = norm(s);
    for (int i = 0; i < ncache; i++)
        if (!memcmp(&cache_st[i], &s, sizeof s))
            return cache_e[i];
    if (ncache == NSTATE) {
        printf("FAIL more than %d settings: raise NSTATE\n", NSTATE);
        fails++;
        return 0;
    }
    held_state = s;
    render(held, y);
    hf_energy(y, cache_e[ncache]);
    cache_st[ncache] = s;
    return cache_e[ncache++];
}

/* Return the worst excess in dB of f's render over the settings it passed through; *at its time, in ms, *lvl
 * the render's level above 4 kHz there and *rlvl the settings' (dBFS RMS). */
static double excess(sweep_fn f, double *at, double *lvl, double *rlvl)
{
    static float e[NW];
    double worst = -1e9;
    render(f, out);
    hf_energy(out, e);
    for (int k = 0; k < NW; k++) {
        double ref = 0, x;
        state_t last = {-1, -1, -1};
        int t0 = k * WIN - LAG < 0 ? 0 : k * WIN - LAG;
        for (int t = t0 - t0 % BLK; t < (k + 1) * WIN; t += BLK) {
            state_t s = norm(f(t));
            if (!memcmp(&s, &last, sizeof s))
                continue;
            last = s;
            const float *r = static_e(s);
            if (r && r[k] > ref)
                ref = r[k];
        }
        x = 10 * log10((e[k] + FLOOR) / (ref + FLOOR));
        if (x > worst) {
            worst = x;
            *at = k * WIN * 1000.0 / SR;
            *lvl = 10 * log10(e[k] / WIN + 1e-30);
            *rlvl = 10 * log10(ref / WIN + 1e-30);
        }
    }
    return worst;
}

/* ---- the movements: from 0.2 s; the pot at t, as an integer, as a knob sets it ---- */
static int ramp(int t, int t0, int len, int a, int b)   /* a -> b over len samples from t0 */
{
    if (t <= t0)
        return a;
    if (t >= t0 + len)
        return b;
    return a + (int)((double)(b - a) * (t - t0) / len);
}
static int TYPE;
/* 0 -> 127 over 1.5 s, then back to 0 over 0.2 s */
static state_t slow(int t)
{
    state_t s = {TYPE, t < 74970 ? ramp(t, 8820, 66150, 0, 127) : ramp(t, 74970, 8820, 127, 0), 96};
    return s;
}
/* 0 -> 127 -> 0 -> 127 -> 0, 0.15 s each, as a fast turn of the knob */
static state_t fast(int t)
{
    int ph = (t - 8820) / 6615, d = 0;
    if (ph >= 0 && ph <= 3)
        d = ph & 1 ? ramp(t, 8820 + ph * 6615, 6615, 127, 0) : ramp(t, 8820 + ph * 6615, 6615, 0, 127);
    state_t s = {TYPE, d, 96};
    return s;
}
/* Drive 0 <-> 1 every 0.1 s: the drive switched in and out */
static state_t edge(int t)
{
    state_t s = {TYPE, t >= 8820 && ((t - 8820) / 4410) % 2 == 0, 96};
    return s;
}
/* Drive 0 <-> 64 every 0.25 s: a lane jumping */
static state_t jump(int t)
{
    state_t s = {TYPE, t >= 8820 && ((t - 8820) / 11025) % 2 == 0 ? 64 : 0, 96};
    return s;
}
/* Drive 40 <-> 41 every 50 ms: one detent of the knob, back and forth */
static state_t detent(int t)
{
    state_t s = {TYPE, 40 + (t >= 8820 && ((t - 8820) / 2205) % 2 == 0), 96};
    return s;
}
/* Volume 96 -> 30 -> 127 -> 96, 0.2 s each, at Drive 0 (TYPE Off) or 64 */
static state_t vol(int t)
{
    int v = t < 17640 ? ramp(t, 8820, 8820, 96, 30)
          : t < 26460 ? ramp(t, 17640, 8820, 30, 127) : ramp(t, 26460, 8820, 127, 96);
    state_t s = {TYPE, TYPE ? 64 : 0, v};
    return s;
}
/* the drive type Soft, RAT, Off, Soft, ... every 0.3 s at Drive 64 */
static state_t types(int t)
{
    static const int seq[3] = {BASS303_DRV_SOFT, BASS303_DRV_RAT, BASS303_DRV_OFF};
    state_t s = {seq[t < 8820 ? 0 : ((t - 8820) / 13230) % 3], 64, 96};
    return s;
}

/* A sweep that ends at setting s (Drive from 0, Volume from 96, as a lane would move them), against s from the
 * start: the last 0.5 s the same, sample for sample. */
static state_t settle_to(int t)
{
    state_t s = held_state;
    s.drive = ramp(t, 4410, 22050, 0, s.drive);
    s.vol = ramp(t, 4410, 22050, 96, s.vol);
    return s;
}

int main(void)
{
    static const struct { const char *name; sweep_fn f; int type; double limit; } CASES[] = {
        {"Soft slow sweep", slow, BASS303_DRV_SOFT, 6},
        {"Soft fast sweep", fast, BASS303_DRV_SOFT, 6},
        {"Soft one detent", detent, BASS303_DRV_SOFT, 6},
        {"Soft 0 <-> 1", edge, BASS303_DRV_SOFT, 6},
        {"Soft 0 <-> 64", jump, BASS303_DRV_SOFT, 6},
        {"RAT slow sweep", slow, BASS303_DRV_RAT, 6},
        {"RAT fast sweep", fast, BASS303_DRV_RAT, 6},
        {"RAT one detent", detent, BASS303_DRV_RAT, 6},
        {"RAT 0 <-> 1", edge, BASS303_DRV_RAT, 6},
        {"RAT 0 <-> 64", jump, BASS303_DRV_RAT, 6},
        {"Volume, drive off", vol, BASS303_DRV_OFF, 6},
        {"Volume, Soft 64", vol, BASS303_DRV_SOFT, 6},
        {"Volume, RAT 64", vol, BASS303_DRV_RAT, 6},
        {"type Soft/RAT/Off", types, 0, 6},
    };
    static float ref[LEN];
    for (unsigned c = 0; c < sizeof CASES / sizeof CASES[0]; c++) {
        double at = 0, x, lvl = 0, rlvl = 0;
        TYPE = CASES[c].type;
        x = excess(CASES[c].f, &at, &lvl, &rlvl);
        printf("  %-18s worst excess above 4 kHz %+5.1f dB (at %4.0f ms: %6.1f dBFS, held %6.1f)\n", CASES[c].name, x,
               at, lvl, rlvl);
        CHECK(x <= CASES[c].limit, "%s: %+.1f dB at %.0f ms", CASES[c].name, x, at);
    }
    for (int type = BASS303_DRV_OFF; type <= BASS303_DRV_RAT; type++)
        for (int d = 1; d <= 127; d += 63) {
            static const char *const NAME[3] = {"Off", "Soft", "RAT"};
            int diff = 0, v = d == 1 ? 96 : d == 64 ? 40 : 127;
            state_t s = {type, d, v};
            held_state = s;
            render(held, ref);
            render(settle_to, out);
            for (int i = LEN - SR / 2; i < LEN; i++)
                diff += out[i] != ref[i];
            printf("  %-4s swept to Drive %3d, Volume %3d: %d samples of the last 0.5 s differ from it set at the start\n",
                   NAME[type], d, v, diff);
            CHECK(diff == 0, "%s: swept to Drive %d, Volume %d: %d samples differ", NAME[type], d, v, diff);
        }
    printf(fails ? "bass303_slew: %d FAILED\n" : "bass303_slew: ok\n", fails);
    return fails != 0;
}
