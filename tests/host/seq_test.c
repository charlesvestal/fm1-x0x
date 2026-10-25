/* SPDX-License-Identifier: GPL-3.0-only */
/* Sequencer and TB-3PO tests on the host: event times to the sample, 303 gate / slide /
 * tie semantics, swing, rates, polymeter, pattern cue + chain, external clock, MIDI clock
 * out, the break clock, and TB-3PO lines identical to schwung-tb3po's for the same seed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/src/seq/sequencer.h"
#include "../../firmware/src/seq/tb3po.h"  /* IWYU pragma: keep (tb3po_* below) */

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- event log */
enum { E_DRUM, E_ON, E_OFF, E_BRK, E_BRKSTOP, E_MIDI };
typedef struct { uint32_t t; int kind, a, b, c, d; } ev_t;
static ev_t evs[100000];
static int nev;
static uint32_t now;
static void log_ev(int k, int a, int b, int c, int d)
{
    if (nev < (int)(sizeof evs / sizeof evs[0]))
        evs[nev++] = (ev_t){now, k, a, b, c, d};
}
static int s_drum(void *x, int k, int v, float vel) { (void)x; log_ev(E_DRUM, k * 100 + v, (int)(vel * 127.0f + 0.5f), 0, 0); return 1; }
static void s_on(void *x, int p, int n, int acc, int sl) { (void)x; log_ev(E_ON, p, n, acc, sl); }
static void s_off(void *x, int p) { (void)x; log_ev(E_OFF, p, 0, 0, 0); }
static void s_brk(void *x, int s16, int bar, float spb, int en) { (void)x; log_ev(E_BRK, s16, bar, (int)spb, en); }
static void s_brkstop(void *x) { (void)x; log_ev(E_BRKSTOP, 0, 0, 0, 0); }
static void s_midi(void *x, uint8_t st, uint8_t d1, uint8_t d2) { (void)x; log_ev(E_MIDI, st, d1, d2, 0); }
static const seq_sink_t SINK = {s_drum, s_on, s_off, s_brk, s_brkstop, s_midi, 0, 0};

static seq_t S;
static pattern_t P[NPAT];

/* render like the engine: block of 256, split at events */
static void run(uint32_t samples)
{
    while (samples) {
        uint32_t n = samples < 256 ? samples : 256;
        seq_advance(&S, 0, &SINK);
        while (n) {
            uint32_t k = seq_until_event(&S, n);
            now += k;
            seq_advance(&S, k, &SINK);
            n -= k;
            samples -= k;
        }
    }
}

static void reset(void)
{
    int i;
    for (i = 0; i < NPAT; i++)
        pattern_init(&P[i], 0x100 + i, 0x200 + i);
    seq_init(&S, P);
    S.send_clock = 0;
    nev = 0;
    now = 0;
}

static int find(int kind, int a, int from)
{
    int i;
    for (i = from; i < nev; i++)
        if (evs[i].kind == kind && (a < 0 || evs[i].a == a))
            return i;
    return -1;
}

static void test_drum_timing(void)
{
    int i, k, n = 0;
    double step = 44100.0 * 60.0 / 120.0 / 4.0;      /* 5512.5 samples */
    reset();
    S.bpm = 120.0f;
    for (i = 0; i < 16; i += 4)
        P[0].drum[0].hit[0][0] |= 1u << i;                 /* BD on the quarters */
    P[0].drum[0].hit[7][0] = 0xAAAAu;                      /* CH on the off-16ths */
    P[0].drum[0].accent[0] = 1u;                           /* accent on step 1 */
    seq_start(&S);
    run(44100 * 4 - 1);                              /* 2 bars, not the downbeat of the 3rd */
    for (i = 0, k = 0; (i = find(E_DRUM, 0, i)) >= 0; i++, k++) {
        double want = k * 4 * step;
        CHECK(abs((int)evs[i].t - (int)(want + 0.5)) <= 1, "BD %d at %u, want %.1f", k, evs[i].t, want);
        if (k % 4 == 0)
            CHECK(evs[i].b == 127, "BD %d accented vel %d", k, evs[i].b);
        else
            CHECK(evs[i].b == 88, "BD %d vel %d, want 88", k, evs[i].b);
        n++;
    }
    CHECK(n == 8, "BD hits %d, want 8 in 2 bars", n);
    for (i = 0, k = 0; (i = find(E_DRUM, 7, i)) >= 0; i++, k++)
        CHECK(abs((int)evs[i].t - (int)((2 * k + 1) * step + 0.5)) <= 1, "CH %d at %u", k, evs[i].t);
    CHECK(k == 16, "CH hits %d", k);
    printf("  drum timing: %d BD, %d CH on the sample\n", n, k);
}

static void test_swing_and_rates(void)
{
    int i, k;
    double step = 44100.0 * 60.0 / 120.0 / 4.0;
    reset();
    S.bpm = 120.0f;
    P[0].swing = 100;                                /* 75 %: odd 16ths half a step late */
    P[0].drum[0].hit[7][0] = 0xFFFFu;
    seq_start(&S);
    run(44100 * 2);
    for (i = 0, k = 0; (i = find(E_DRUM, 7, i)) >= 0 && k < 16; i++, k++) {
        double want = k * step + ((k & 1) ? 0.5 * step : 0.0);
        CHECK(abs((int)evs[i].t - (int)(want + 0.5)) <= 1, "swung CH %d at %u want %.1f", k, evs[i].t, want);
    }
    /* triplets: 1/16T = 1/6 quarter, never swung */
    reset();
    S.bpm = 120.0f;
    P[0].swing = 100;
    P[0].drum[0].rate = RATE_16T;
    P[0].drum[0].len = 12;
    P[0].drum[0].hit[7][0] = 0xFFFu;
    seq_start(&S);
    run(44100);
    for (i = 0, k = 0; (i = find(E_DRUM, 7, i)) >= 0 && k < 12; i++, k++) {
        double want = k * 44100.0 * 60.0 / 120.0 / 6.0;
        CHECK(abs((int)evs[i].t - (int)(want + 0.5)) <= 1, "triplet %d at %u want %.1f", k, evs[i].t, want);
    }
    printf("  swing 75%%, 1/16T unswung: checked\n");
}

/* Set up the swing tests' parts: the 909's closed hat on every step, and 303 A playing in direction dir
 * at length len, each step's note 36 + its position, so that a note event tells where it was. Both parts
 * play at the same rate (RATE_*). */
static void swing_setup(int dir, int len, int rate)
{
    bpart_t *b = &P[0].bass[0];
    int s;
    reset();
    S.bpm = 120.0f;
    P[0].swing = 100;
    P[0].drum[0].rate = (uint8_t)rate;
    P[0].drum[0].hit[7][0] = 0xFFFFu;
    b->rate = (uint8_t)rate;
    b->len = (uint8_t)len;
    b->dir = (uint8_t)dir;
    for (s = 0; s < len; s++)
        b->step[s] = (bstep_t){(uint8_t)(36 + s), G_NOTE};
}

/* Return whether a part's k-th step since the start, at position pos, is an odd one in the order it plays
 * them: the 909's and a forward or ping-pong 303's by position, a reverse 303's counted from where it
 * starts (its last step), a random one's by count. */
static int odd_step(int is303, int dir, int len, int pos, int k)
{
    if (!is303 || dir == DIR_FWD || dir == DIR_PINGPONG)
        return pos & 1;
    return dir == DIR_REV ? (len - 1 - pos) & 1 : k & 1;
}

/* Return how late swing sw makes an odd step of length L (Q8), in samples, as the sequencer computes it
 * (for a rate that swings: triplets never do). */
static double swing_samples(uint32_t L, int sw)
{
    return (double)(L / 200u * (uint32_t)sw) / 256.0;
}

/* Return the swing in force at sample t (the last change at or before it), or -1 if a change fell
 * within a sample of t. */
static int swing_at(const uint32_t *ct, const uint8_t *cs, int n, uint32_t t)
{
    int i, v = cs[0];
    for (i = 0; i < n; i++) {
        if (i && ct[i] + 1 >= t && ct[i] <= t + 1)
            return -1;
        if (ct[i] <= t)
            v = cs[i];
    }
    return v;
}

/* Send n external clock ticks, per_tick samples apart, playing through each. */
static void clock_ticks(int n, uint32_t per_tick)
{
    int i;
    for (i = 0; i < n; i++) {
        seq_ext_clock(&S);
        run(per_tick);
    }
}

/* Check that turning the swing while playing never moves a part off its grid.
 *
 * Play the 909 and 303 A at 16ths and at 32nds, the 303 in every direction and at lengths 16, 7 and 3,
 * changing the swing 60 times at pseudo-random moments (from a fixed seed, so every run is the same).
 * Every step must land on its grid slot, late by the swing in force when it was scheduled (at the step
 * before) if it is an odd one in play order, else on its slot. A step scheduled within a sample of a
 * change is skipped: either swing is right for it. */
static void test_swing_grid(void)
{
    static const int LENS[] = {16, 7, 3}, RATES[] = {RATE_16, RATE_32};
    int ri, dir, li, nchecked = 0;
    for (ri = 0; ri < 2; ri++)
        for (dir = 0; dir < NDIRS; dir++)
            for (li = 0; li < 3; li++) {
                int rate = RATES[ri], len = LENS[li], n = 0, part;
                const double step = 44100.0 * 60.0 / 120.0 / (rate == RATE_32 ? 8 : 4);
                uint32_t ct[64], r = 0x9E3779B9u ^ (uint32_t)(ri * 256 + dir * 16 + len), L;
                uint8_t cs[64];

                swing_setup(dir, len, rate);
                L = seq_step_q8(&S, rate);

                /* Play, changing the swing every 1000 to 9000 samples; log each change (when, and the new
                 * swing). */
                ct[n] = 0;
                cs[n++] = 100;
                seq_start(&S);
                while (n < 61) {
                    r = r * 1664525u + 1013904223u;
                    run(1000u + (r >> 8) % 8000u);
                    P[0].swing = (uint8_t)((r >> 20) % 101u);
                    ct[n] = now;
                    cs[n++] = P[0].swing;
                }

                /* Check that each step of the 909's hats, then of 303 A's notes, lands on its slot plus its
                 * swing. */
                for (part = 0; part < 2; part++) {
                    int kind = part ? E_ON : E_DRUM, a = part ? 0 : 7, i, k, prev = -1, bad = 0;
                    for (i = 0, k = 0; (i = find(kind, a, i)) >= 0; prev = i++, k++) {
                        int pos = part ? evs[i].b - 36 : k % 16, sw = k ? swing_at(ct, cs, n, evs[prev].t) : 0;
                        double want;
                        if (sw < 0)
                            continue;
                        want = k * step + (odd_step(part, dir, len, pos, k) ? swing_samples(L, sw) : 0.0);
                        nchecked++;
                        if (!bad && abs((int)evs[i].t - (int)(want + 0.5)) > 1) {   /* (report a part's first miss only) */
                            CHECK(0, "%s rate %d dir %d len %d: step %d (pos %d) at %u, want %.1f",
                                  part ? "303" : "909", rate, dir, len, k, pos, evs[i].t, want);
                            bad = 1;
                        }
                    }
                }
            }
    CHECK(nchecked > 3000, "checked %d steps: too few (the changes skipped too many)", nchecked);
    printf("  swing changed while playing, 16ths and 32nds, every direction, lengths 16/7/3: %d steps on the grid\n",
           nchecked);
}

/* Check that on an external clock every step lands on its tick, late by the swing if it is an odd one in
 * the order its part plays them, and that a swing changed while a late step is pending moves only the
 * steps armed after it.
 *
 * Follow a steady clock of about 120 BPM for two bars, playing the 909 and 303 A at 16ths, 32nds and 16th
 * triplets (which never swing), the 303 in every direction and at lengths 16 and 7. A reverse 303 counts
 * its odd steps from where it starts and a random one by count, as on the internal clock. */
static void test_swing_ext(void)
{
    static const int LENS[] = {16, 7}, RATES[] = {RATE_16, RATE_32, RATE_16T}, TPS[] = {6, 3, 4};
    const uint32_t per_tick = 919;
    int ri, dir, li, nchecked = 0;
    for (ri = 0; ri < 3; ri++)
        for (dir = 0; dir < NDIRS; dir++)
            for (li = 0; li < 2; li++) {
                int rate = RATES[ri], tps = TPS[ri], len = LENS[li], change, part, i;
                uint32_t t_start;

                swing_setup(dir, len, rate);

                /* Send a quarter note of clocks before the start, so that the tempo is steady. */
                clock_ticks(24, per_tick);

                /* Play two bars. Half a tick after the tick that arms an odd step (step 17, or 35 at
                 * 32nds), while that step is pending, change the swing to 30. */
                seq_start(&S);
                nev = 0;
                t_start = now;
                change = (rate == RATE_32 ? 35 : 17) * tps;
                for (i = 0; i < 24 * 8; i++) {
                    seq_ext_clock(&S);
                    if (i == change) {
                        run(per_tick / 2);
                        P[0].swing = 30;
                        run(per_tick - per_tick / 2);
                    } else {
                        run(per_tick);
                    }
                }
                CHECK(S.ext == 1, "rate %d dir %d len %d: following the external clock", rate, dir, len);

                /* Check that each step of the 909's hats, then of 303 A's notes, lands on its tick plus the
                 * swing in force at that tick. */
                for (part = 0; part < 2; part++) {
                    int kind = part ? E_ON : E_DRUM, a = part ? 0 : 7, k, bad = 0;
                    for (i = 0, k = 0; (i = find(kind, a, i)) >= 0; i++, k++) {
                        int pos = part ? evs[i].b - 36 : k % 16, sw = k * tps <= change ? 100 : 30;
                        int late = rate != RATE_16T && odd_step(part, dir, len, pos, k);
                        double want = t_start + (double)k * tps * per_tick +
                                      (late ? swing_samples(S.ext_period * (uint32_t)tps, sw) : 0.0);
                        nchecked++;
                        if (!bad && abs((int)evs[i].t - (int)(want + 0.5)) > 1) {   /* (report a part's first miss only) */
                            CHECK(0, "ext %s rate %d dir %d len %d: step %d (pos %d) at %u, want %.1f",
                                  part ? "303" : "909", rate, dir, len, k, pos, evs[i].t, want);
                            bad = 1;
                        }
                    }
                }
            }
    CHECK(nchecked > 2000, "ext: checked %d steps: too few", nchecked);
    printf("  external clock, 16ths/32nds/16T, every direction, lengths 16/7, swing changed once: "
           "%d steps on the grid\n", nchecked);
}

/* Check that Continue resumes on the grid, on either clock: the step it resumes on plays late by the
 * swing if it is an odd one, and the steps after it keep the grid from there.
 *
 * Play the 909 and 303 A, the 303 in every direction and at lengths 16 and 7, stop after 5 steps (so
 * that the step resumed on is odd) or after 6 (even), and continue a moment later. */
static void test_swing_continue(void)
{
    static const int LENS[] = {16, 7};
    const uint32_t per_tick = 919;
    int ext, dir, li, played, nchecked = 0;
    for (ext = 0; ext < 2; ext++)
        for (dir = 0; dir < NDIRS; dir++)
            for (li = 0; li < 2; li++)
                for (played = 5; played <= 6; played++) {
                    int len = LENS[li], part;
                    const double step = ext ? 6.0 * per_tick : 44100.0 * 60.0 / 120.0 / 4.0;
                    uint32_t L, t_cont;

                    swing_setup(dir, len, RATE_16);

                    /* Play the first steps, stop between the last of them and the next, and wait. The external
                     * clock starts a quarter note early, so that the tempo is steady, and keeps running while
                     * stopped. */
                    if (ext) {
                        clock_ticks(24, per_tick);
                        seq_start(&S);
                        clock_ticks(6 * played, per_tick);
                        seq_stop(&S);
                        clock_ticks(12, per_tick);
                    } else {
                        seq_start(&S);
                        run((uint32_t)((played - 0.25) * step));
                        seq_stop(&S);
                        run(10000);
                    }

                    /* Continue and play a bar, logging only what follows the Continue. */
                    nev = 0;
                    t_cont = now;
                    seq_continue(&S);
                    if (ext)
                        clock_ticks(24 * 4, per_tick);
                    else
                        run(44100 * 2 - 1);                      /* (not the next bar's first step) */
                    L = ext ? S.ext_period * 6u : seq_step_q8(&S, RATE_16);

                    /* Check that each step of the 909's hats, then of 303 A's notes, lands on its slot (counted
                     * from the Continue) plus its swing. */
                    for (part = 0; part < 2; part++) {
                        int kind = part ? E_ON : E_DRUM, a = part ? 0 : 7, i, k, bad = 0;
                        for (i = 0, k = played; (i = find(kind, a, i)) >= 0; i++, k++) {
                            int pos = part ? evs[i].b - 36 : k % 16;
                            double want = t_cont + (k - played) * step +
                                          (odd_step(part, dir, len, pos, k) ? swing_samples(L, 100) : 0.0);
                            nchecked++;
                            if (!bad && abs((int)evs[i].t - (int)(want + 0.5)) > 1) {   /* (report a part's first miss only) */
                                CHECK(0, "%s continue %s dir %d len %d after %d: step %d (pos %d) at %u, want %.1f",
                                      ext ? "ext" : "int", part ? "303" : "909", dir, len, played, k, pos,
                                      evs[i].t, want);
                                bad = 1;
                            }
                        }
                        CHECK(k - played == 16, "%s continue %s dir %d len %d after %d: %d steps in the bar, want 16",
                              ext ? "ext" : "int", part ? "303" : "909", dir, len, played, k - played);
                    }
                }
    printf("  Continue on either clock, every direction, lengths 16/7, odd and even: %d steps on the grid\n",
           nchecked);
}

static void test_303(void)
{
    bpart_t *b;
    int i, on, off;
    double step = 44100.0 * 60.0 / 120.0 / 4.0;
    reset();
    S.bpm = 120.0f;
    b = &P[0].bass[0];
    b->len = 8;
    b->step[0] = (bstep_t){36, G_NOTE};                       /* plain: off at half step */
    b->step[1] = (bstep_t){38, G_NOTE | BS_SLIDE};            /* slides into step 2 */
    b->step[2] = (bstep_t){41, G_NOTE | BS_ACCENT};           /* reached by slide, accented */
    b->step[3] = (bstep_t){41, G_TIE};                        /* tie: 2 held through 3 */
    b->step[4] = (bstep_t){0, G_REST};
    b->step[5] = (bstep_t){43, G_TIE};                        /* tie after rest = a note */
    b->transpose = 26;                                        /* +2 */
    seq_start(&S);
    run((uint32_t)(step * 8));
    on = find(E_ON, 0, 0);
    CHECK(on >= 0 && evs[on].b == 38 && evs[on].d == 0 && evs[on].t == 0, "step 1 note 36+2, no slide");
    off = find(E_OFF, 0, on);
    CHECK(off >= 0 && abs((int)evs[off].t - (int)(step / 2)) <= 1, "step 1 off at half step (%u)", off >= 0 ? evs[off].t : 0);
    on = find(E_ON, 0, on + 1);
    CHECK(on >= 0 && evs[on].b == 40 && evs[on].d == 0, "step 2 on");
    i = find(E_ON, 0, on + 1);
    off = find(E_OFF, 0, on + 1);
    CHECK(i >= 0 && evs[i].b == 43 && evs[i].d == 1 && evs[i].c == 1, "step 3 slides (slide=%d acc=%d)", i >= 0 ? evs[i].d : -1, i >= 0 ? evs[i].c : -1);
    CHECK(off > i, "no gate off between a slide and its target");
    CHECK(off >= 0 && abs((int)evs[off].t - (int)(3.5 * step + 0.5)) <= 1, "tie holds step 3 into step 4, whose gate falls at its half (%u, want %.0f)", off >= 0 ? evs[off].t : 0, 3.5 * step);
    i = find(E_ON, 0, off);
    CHECK(i >= 0 && evs[i].b == 45 && abs((int)evs[i].t - (int)(5 * step + 0.5)) <= 1, "tie after a rest plays as a note");
    printf("  303 gate / slide / tie / transpose: checked\n");
}

static void test_polymeter_cue_chain(void)
{
    int i, n, last;
    double step = 44100.0 * 60.0 / 120.0 / 4.0;
    reset();
    S.bpm = 120.0f;
    P[0].bass[0].len = 3;
    P[0].bass[0].step[0] = (bstep_t){36, G_NOTE};
    P[0].drum[0].hit[0][0] = 1u;
    P[1].drum[0].hit[1][0] = 1u;                                    /* SD only in pattern 2 */
    seq_start(&S);
    run((uint32_t)(step * 15) - 1);                         /* not the note due exactly at step 15 */
    for (i = 0, n = 0; (i = find(E_ON, 0, i)) >= 0; i++, n++)
        CHECK(abs((int)evs[i].t - (int)(n * 3 * step + 0.5)) <= 1, "3-step 303 loop %d at %u", n, evs[i].t);
    CHECK(n == 5, "3-step line over 15 steps: %d notes", n);
    seq_cue(&S, 1);
    nev = 0;
    run((uint32_t)(step * 17));                               /* to the bar line and one more */
    i = find(E_DRUM, 1, 0);
    CHECK(i >= 0 && abs((int)evs[i].t - (int)(16 * step + 0.5)) <= 1, "cued pattern starts on the bar (%u)", i >= 0 ? evs[i].t : 0);
    CHECK(S.ppat[0] == 1 && S.ppat[4] == 1, "now playing pattern 2");
    i = find(E_ON, 0, 0);
    /* pattern 2's 303 A line is empty: no notes after the switch */
    for (n = 0; (i = find(E_ON, 0, i < 0 ? 0 : i)) >= 0; i++)
        if (evs[i].t >= (uint32_t)(16 * step))
            n++;
    CHECK(n == 0, "303 restarts with the new (empty) pattern");
    /* chain 3..5 */
    reset();
    S.bpm = 120.0f;
    for (i = 0; i < NPAT; i++)
        P[i].drum[0].hit[0][0] = 1u;
    for (i = 0; i < NTRACKS; i++)
        S.ppat[i] = 3;
    seq_chain(&S, 3, 5);
    seq_start(&S);
    for (i = 0, last = -1; i < 7; i++) {
        uint8_t c = S.ppat[0];
        run((uint32_t)(16 * step));
        if (i < 6)
            CHECK(c == 3 + (i % 3), "chain bar %d plays pattern %d (got %d)", i, 3 + i % 3, c + 1 - 1);
        last = c;
    }
    (void)last;
    printf("  polymeter, cue on the bar, chain 4-6: checked\n");
}

/* seq_sync (a tap): every part to the 909's nearest quarter, its step fired now, the beat counted
 * from there. Late (first half): back, and at the bar's start no bar end (a cue waits for the real
 * one). Early (second half): on, the steps between unplayed; before the bar line the bar ends now. */
static void sync_at(double steps)
{
    reset();
    S.bpm = 120.0f;
    P[0].drum[0].hit[0][0] = 1u << 4 | 1u << 8;            /* BD on steps 5 and 9, SD on 7, LT on 1 */
    P[0].drum[0].hit[1][0] = 1u << 6;
    P[0].drum[0].hit[2][0] = 1u;
    P[1].drum[0].hit[3][0] = 1u;                            /* pattern 2: MT on 1 */
    P[0].bass[0].len = 3;                                   /* a 3-step 303: notes 36 and 48 on its steps 2, 3 */
    P[0].bass[0].step[1] = (bstep_t){36, G_NOTE};
    P[0].bass[0].step[2] = (bstep_t){48, G_NOTE};
    seq_start(&S);
    run((uint32_t)(44100.0 * 60.0 / 120.0 / 4.0 * steps));
}

static int at_once(int kind, int a, uint32_t t0)
{
    int i = find(kind, a, 0);
    return i >= 0 && evs[i].t == t0;
}

static void test_sync(void)
{
    int i;
    uint32_t t0;
    double step = 44100.0 * 60.0 / 120.0 / 4.0;
    sync_at(5.5);                                           /* late: in step 6, the quarter began at 5 */
    nev = 0;
    t0 = now;
    seq_sync(&S);
    run(16);
    CHECK(at_once(E_DRUM, 0, t0), "late sync: BD (step 5) again at once");
    CHECK(S.t[TRK_DRUM].pos == 4, "late sync: the 909 on step 5 (pos %d)", S.t[TRK_DRUM].pos);
    i = find(E_ON, 0, 0);                                   /* the 303 on its step 3, 1 back = step 2 */
    CHECK(i >= 0 && evs[i].t == t0 && evs[i].b == 36, "late sync: the 303 went back as far");
    run((uint32_t)(step * 3));
    i = find(E_DRUM, 1, 0);
    CHECK(i >= 0 && abs((int)evs[i].t - (int)(t0 + 2 * step + 0.5)) <= 1, "late sync: SD two steps after the tap (%u)",
          i >= 0 ? evs[i].t : 0);
    sync_at(6.5);                                           /* early: in step 7, the next quarter at 9 */
    nev = 0;
    t0 = now;
    seq_sync(&S);
    run(16);
    CHECK(at_once(E_DRUM, 0, t0), "early sync: BD (step 9) at once");
    CHECK(S.t[TRK_DRUM].pos == 8, "early sync: the 909 on step 9 (pos %d)", S.t[TRK_DRUM].pos);
    i = find(E_ON, 0, 0);                                   /* the 303 on its step 1, on 2 = step 3 */
    CHECK(i >= 0 && evs[i].t == t0 && evs[i].b == 48, "early sync: the 303 went on as far");
    CHECK(find(E_DRUM, 1, 0) < 0, "early sync: SD (step 7) left out");
    /* late, in the bar's first quarter: step 1 again, a start not a wrap: the cue waits for the bar */
    sync_at(17.5);
    seq_cue(&S, 1);
    nev = 0;
    t0 = now;
    seq_sync(&S);
    run(16);
    CHECK(at_once(E_DRUM, 2, t0), "late sync on the bar: LT (step 1) again at once");
    CHECK(S.ppat[0] == 0 && find(E_DRUM, 3, 0) < 0, "late sync on the bar: no bar end, the cue waits");
    run((uint32_t)(step * 16));
    i = find(E_DRUM, 3, 0);
    CHECK(i >= 0 && abs((int)evs[i].t - (int)(t0 + 16 * step + 0.5)) <= 1, "the cue on the next bar, 16 steps on (%u)",
          i >= 0 ? evs[i].t : 0);
    /* early, before the bar line: the bar ends now, the cue with it */
    sync_at(14.5);
    seq_cue(&S, 1);
    nev = 0;
    t0 = now;
    seq_sync(&S);
    run(16);
    CHECK(at_once(E_DRUM, 3, t0) && S.ppat[0] == 1, "early sync before the bar: the cued pattern at once");
    /* a 909 of 14 steps: its last quarter is 13-14, so half a step before the bar line is early: it ends now */
    sync_at(0);
    P[0].drum[0].len = 14;
    run((uint32_t)(step * 13.5));
    nev = 0;
    t0 = now;
    seq_sync(&S);
    run(16);
    CHECK(at_once(E_DRUM, 2, t0) && S.t[TRK_DRUM].pos == 0, "early sync, short bar: step 1 at once (pos %d)",
          S.t[TRK_DRUM].pos);
    printf("  sync (tap): the nearest quarter, back or on, the cue on the bar: checked\n");
}

/* one part's cue: it switches on the bar and restarts; the others keep their place */
static void test_part_cue(void)
{
    int i, n;
    double step = 44100.0 * 60.0 / 120.0 / 4.0;
    reset();
    S.bpm = 120.0f;
    P[0].drum[0].hit[0][0] = 1u;
    P[0].bass[1].len = 3;                                       /* 303 B: a 3-step loop */
    P[0].bass[1].step[0] = (bstep_t){40, G_NOTE};
    P[1].bass[0].step[0] = (bstep_t){50, G_NOTE};               /* 303 A in pattern 2 */
    seq_start(&S);
    run((uint32_t)(step * 5));
    seq_cue_part(&S, TRK_BASS0, 1);
    run((uint32_t)(step * 11) - 1);                             /* to just before the bar */
    CHECK(S.ppat[TRK_BASS0] == 0, "a part's cue waits for the bar");
    nev = 0;
    run((uint32_t)(step * 4));
    CHECK(S.ppat[TRK_BASS0] == 1 && S.ppat[0] == 0 && S.ppat[TRK_BASS0 + 1] == 0, "only 303 A switched");
    i = find(E_ON, 0, 0);
    CHECK(i >= 0 && evs[i].b == 50 && abs((int)evs[i].t - (int)(16 * step + 0.5)) <= 1, "303 A's new line on the bar");
    for (i = 0, n = 0; (i = find(E_ON, 1, i)) >= 0; i++, n++)   /* 303 B: steps 18 (its 7th loop), not 16 */
        CHECK(abs((int)evs[i].t - (int)(18 * step + 0.5)) <= 1, "303 B kept its place (note at %u)", evs[i].t);
    CHECK(n == 1, "303 B: one note in the window, %d", n);
    printf("  a part's own cue: checked\n");
}

static void test_song(void)
{
    static song_t sg;
    double bar = 44100.0 * 60.0 / 120.0 * 4.0;
    int i;
    /* playback: three bars, looping; a bar's mutes */
    reset();
    S.bpm = 120.0f;
    S.song = &sg;
    sg.len = 3;
    for (i = 0; i < NTRACKS; i++) {
        sg.bar[0].pat[i] = 0;
        sg.bar[1].pat[i] = 1;
        sg.bar[2].pat[i] = 2;
    }
    sg.bar[1].pat[TRK_BRK] = 7;
    sg.bar[0].mute = sg.bar[1].mute = 0;
    sg.bar[2].mute = 1u << 1;                                   /* the 808 out in bar 3 */
    S.song_on = 1;
    S.song_start = 1;
    seq_start(&S);
    run(1);
    CHECK(S.ppat[0] == 1 && S.ppat[TRK_BRK] == 7 && S.song_pos == 1, "starts at the start bar");
    run((uint32_t)bar);
    CHECK(S.ppat[0] == 2 && S.song_pos == 2 && seq_part_muted(&S, 1) && !seq_part_muted(&S, 0), "bar 3, 808 muted");
    run((uint32_t)bar);
    CHECK(S.ppat[0] == 0 && S.song_pos == 0 && !seq_part_muted(&S, 1), "loops to bar 1, 808 back");
    seq_cue(&S, 9);
    run((uint32_t)bar);
    CHECK(S.ppat[0] == 1, "playing the song, a cue does not override it");
    /* recording: from an empty song, the live cues and mutes are written bar by bar */
    reset();
    S.bpm = 120.0f;
    S.song = &sg;
    sg.len = 0;
    S.song_on = S.song_rec = 1;
    S.song_start = 0;
    seq_start(&S);
    run(1);
    CHECK(sg.len == 1 && sg.bar[0].pat[0] == 0, "record: bar 1 written at the start");
    seq_cue_part(&S, TRK_BASS0, 5);
    S.mute |= seq_part_mask(TRK_BRK);
    run((uint32_t)bar);
    CHECK(sg.len == 2 && sg.bar[1].pat[TRK_BASS0] == 5 && sg.bar[1].pat[0] == 0 && sg.bar[1].mute == 1u << TRK_BRK,
          "record: bar 2 has the cue and the mute (len %d, pat %d, mute %d)", sg.len, sg.bar[1].pat[TRK_BASS0], sg.bar[1].mute);
    run((uint32_t)bar * 2);
    CHECK(sg.len == 4 && sg.bar[3].pat[TRK_BASS0] == 5, "record: the song grows while it runs (%d)", sg.len);
    /* overdub from bar 2 plays bar 2 as it is, then writes on */
    seq_stop(&S);
    run(1);
    S.song_start = 1;
    sg.bar[1].pat[0] = 6;
    seq_start(&S);
    run(1);
    CHECK(S.ppat[0] == 6 && sg.len == 4, "overdub starts from the song's bar");
    printf("  song: playback, loop, mutes, record, overdub: checked\n");
}

static void test_ext_clock_and_midi_out(void)
{
    int i, k, clocks;
    uint32_t per_tick = 919;                                  /* ~120 BPM: 44100*60/120/24 = 918.75 */
    reset();
    P[0].drum[0].hit[0][0] = 0xFFFFu;
    for (i = 0; i < 2; i++) {                                 /* clocks before start: tempo only */
        seq_ext_clock(&S);
        run(per_tick);
    }
    seq_start(&S);
    nev = 0;
    {
    uint32_t t_start = now;
    for (i = 0; i < 24 * 4; i++) {                            /* one bar of ticks */
        uint32_t t0 = now;
        seq_ext_clock(&S);
        run(per_tick);
        (void)t0;
    }
    CHECK(S.ext == 1, "following the external clock");
    for (i = 0, k = 0; (i = find(E_DRUM, 0, i)) >= 0; i++, k++)
        CHECK((evs[i].t - t_start) == (uint32_t)k * 6u * per_tick, "ext step %d at +%u, want tick %d", k, evs[i].t - t_start, 6 * k);
    }
    CHECK(k == 16, "16 steps in 96 ticks, got %d", k);
    CHECK(S.ext_bpm > 119.0f && S.ext_bpm < 121.0f, "tempo estimate %.2f", (double)S.ext_bpm);
    run(44100);                                               /* clock gone: back to internal */
    CHECK(S.ext == 0, "internal again after the clock stops");
    /* MIDI clock out: 24 per quarter */
    reset();
    S.send_clock = 1;
    S.bpm = 120.0f;
    seq_start(&S);
    run(44100 * 2 - 1);                                       /* 4 quarters, not the next downbeat */
    for (i = 0, clocks = 0; i < nev; i++)
        if (evs[i].kind == E_MIDI && evs[i].a == 0xF8)
            clocks++;
    CHECK(clocks == 96, "MIDI clocks in 2 s at 120 BPM: %d, want 96", clocks);
    CHECK(find(E_MIDI, 0xFA, 0) == 0, "Start sent first");
    seq_stop(&S);
    run(256);
    CHECK(find(E_MIDI, 0xFC, 0) > 0, "Stop sent");
    printf("  external clock follow + tempo, MIDI clock out: checked\n");
}

static void test_break_clock(void)
{
    int i, k;
    reset();
    S.bpm = 170.0f;
    P[0].drum[0].rate = RATE_16T;                                /* the break stays on 16ths regardless */
    P[0].drum[0].len = 12;
    P[0].brk.steps = 0x00FFu;                                 /* first half of the bar */
    seq_start(&S);
    run(44100 * 3);
    for (i = 0, k = 0; (i = find(E_BRK, -1, i)) >= 0; i++, k++) {
        CHECK(evs[i].a == k % 16, "break step %d is %d", k, evs[i].a);
        CHECK(evs[i].b == k / 16, "break bar %d", evs[i].b);
        CHECK(evs[i].d == (k % 16 < 8), "break step %d enabled=%d", k, evs[i].d);
    }
    CHECK(k > 16 * 2, "break clocked %d times", k);
    seq_stop(&S);
    run(256);
    CHECK(find(E_BRKSTOP, -1, 0) >= 0, "break told to stop");
    printf("  break clock: %d 16ths, bars counted, step mask honoured\n", k);
}

/* ---- TB-3PO against schwung-tb3po: include its plugin source and call its generator */
#ifdef REF_TB3PO
#define move_plugin_init_v2 ref_move_plugin_init_v2
#define slot_init_defaults ref_slot_init_defaults
#define generate_pattern ref_generate_pattern
#define mutate_pattern ref_mutate_pattern
#include REF_TB3PO
static void test_tb3po_matches_reference(void)
{
    int seed, len, i, same = 0, total = 0;
    for (seed = 1; seed < 400; seed++) {
        for (len = 8; len <= 32; len += 8) {
            tb3po_slot_t ref;
            bpart_t b;
            ref_slot_init_defaults(&ref);
            ref.length = len;
            ref.density = (float)(30 + seed % 70) / 100.0f;
            ref.accent = (float)(seed % 90) / 100.0f;
            ref.slide = (float)(seed % 60) / 100.0f;
            ref.octave_range = 1 + seed % 3;
            ref.scale = seed % 6;
            ref.root = seed % 12;
            ref_generate_pattern(&ref, (uint32_t)seed * 7919u);
            memset(&b, 0, sizeof b);
            b.len = (uint8_t)len;
            tb3po_defaults(&b.gen, (uint32_t)seed * 7919u);
            b.gen.density = (uint8_t)(30 + seed % 70);
            b.gen.accent = (uint8_t)(seed % 90);
            b.gen.slide = (uint8_t)(seed % 60);
            b.gen.oct_range = (uint8_t)(1 + seed % 3);
            b.gen.scale = (uint8_t)(seed % 6);
            b.gen.root = (uint8_t)(seed % 12);
            tb3po_generate(&b);
            for (i = 0; i < len; i++) {
                int rk = ref.steps[i], gate = bstep_gate(&b.step[i]);
                int kind = gate == G_REST ? 0 : (b.step[i].flags & BS_SLIDE) ? 3 : (b.step[i].flags & BS_ACCENT) ? 2 : 1;
                int ok = rk == kind;
                if (ok && kind)
                    ok = b.step[i].note == note_for_step(&ref, i);
                same += ok;
                total++;
                if (!ok && total - same < 5)
                    printf("  seed %d len %d step %d: ref kind %d note %d, ours %d note %d\n", seed, len, i, rk,
                           note_for_step(&ref, i), kind, b.step[i].note);
            }
        }
    }
    CHECK(same == total, "TB-3PO steps identical to schwung-tb3po: %d of %d", same, total);
    printf("  TB-3PO vs schwung-tb3po: %d / %d steps identical over 396 seeds x 4 lengths\n", same, total);
}
#endif

int main(void)
{
    test_drum_timing();
    test_swing_and_rates();
    test_swing_grid();
    test_swing_ext();
    test_swing_continue();
    test_303();
    test_polymeter_cue_chain();
    test_sync();
    test_part_cue();
    test_song();
    test_ext_clock_and_midi_out();
    test_break_clock();
#ifdef REF_TB3PO
    test_tb3po_matches_reference();
#endif
    printf("seq: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
