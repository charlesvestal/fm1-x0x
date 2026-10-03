/* SPDX-License-Identifier: GPL-3.0-only */
/* dsp/breaks.c on the host: BB Gen's generator rules (slice_select / perf assertions ported from
 * schwung-breakbeat's tests, used with the author's permission), the generator driven through
 * breaks_step, and the audio: a synthetic 2-bar loop with a distinct tone per 8th, encoded to IMA
 * ADPCM here (tools/sampleio.py's encoder in C), played at 90 and 170 BPM and checked 8th by 8th
 * against the slices the generator chose. Also the step gate, the live keys, the slice-boundary
 * click bound, and build/breaks_demo.wav.
 *
 * Built with -DBRK_FADE=1 -DBRK_CLICK_CONTROL it is the click check's positive control: with the
 * crossfade gone the click check must FAIL (run_breaks.sh requires that). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../firmware/src/dsp/breaks.c"

static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---------------------------------------------------------------------------------------- */
/* synthetic loop: 2 bars at 120 BPM, 22050 Hz, 16 eighths, tone j at F(j) Hz               */

#define SRC_FS 22050
#define LOOP_N 88200                          /* 2 bars at 120 BPM */
#define EIGHTH_SRC (LOOP_N / 16.0)
static double tone_hz(int j, int variant) { return (variant ? 250.0 : 300.0) * pow(2.0, j / 8.0); }

static void synth_loop(int16_t *pcm, int variant)
{
    for (int n = 0; n < LOOP_N; n++) {
        int j = (int)(n / EIGHTH_SRC);
        double t0 = j * EIGHTH_SRC, t = (n - t0) / SRC_FS, len = EIGHTH_SRC / SRC_FS;
        double env = exp(-t / 0.12);
        if (t < 0.001) env *= sin(0.5 * M_PI * t / 0.001) * sin(0.5 * M_PI * t / 0.001);
        if (len - t < 0.003) env *= (len - t) / 0.003;       /* fade out: the source itself is click-free */
        pcm[n] = (int16_t)lrint(20000.0 * env * sin(2.0 * M_PI * tone_hz(j, variant) * t));
    }
}

/* tools/sampleio.py ima_encode, in C: low nibble first, from predictor 0 / index 0 */
static void ima_encode(const int16_t *x, int n, uint8_t *out)
{
    int pred = 0, idx = 0;
    memset(out, 0, (size_t)(n + 1) / 2);
    for (int i = 0; i < n; i++) {
        int step = BRK_IMA_STEP[idx], diff = x[i] - pred, code = 0, vd = step >> 3;
        if (diff < 0) { code = 8; diff = -diff; }
        if (diff >= step) { code |= 4; diff -= step; vd += step; }
        if (diff >= step >> 1) { code |= 2; diff -= step >> 1; vd += step >> 1; }
        if (diff >= step >> 2) { code |= 1; vd += step >> 2; }
        pred = (code & 8) ? pred - vd : pred + vd;
        pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
        idx += BRK_IMA_IDX[code & 7];
        idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
        out[i >> 1] |= (uint8_t)(code << ((i & 1) * 4));
    }
}

static int16_t g_pcm[2][LOOP_N];
static uint8_t g_adpcm[2][LOOP_N / 2 + 1];
static int16_t g_dec[LOOP_N];                /* what the decoder gives back (the true source) */
static brk_loop_t g_loop[2];

static void make_loops(void)
{
    for (int v = 0; v < 2; v++) {
        synth_loop(g_pcm[v], v);
        ima_encode(g_pcm[v], LOOP_N, g_adpcm[v]);
        g_loop[v].adpcm = g_adpcm[v];
        g_loop[v].nsamples = LOOP_N;
        g_loop[v].rate = SRC_FS;
        g_loop[v].bars = 2;
    }
    int32_t p = 0, i = 0;
    for (int n = 0; n < LOOP_N; n++)
        g_dec[n] = (int16_t)ima_decode(g_adpcm[0], (uint32_t)n, &p, &i);
}

/* ---------------------------------------------------------------------------------------- */
/* a little sequencer: steps every 16th at `bpm`, renders in <=256 blocks                   */

typedef struct { long t; int slice, div, bank, forced; } trig_t;
#define MAXTRIG 4096
#define MAXOUT (44100 * 60)
static float g_out[MAXOUT];
static trig_t g_trig[MAXTRIG];
static int g_ntrig;
static int g_step_mask[16];                  /* per 16th enable (default all on) */

static long run(breaks_t *b, float bpm, int bars, long t0, int stop_at_end)
{
    float sp16 = 44100.0f * 60.0f / bpm / 4.0f;
    long t = t0;
    for (int bar = 0; bar < bars; bar++)
        for (int s = 0; s < 16; s++) {
            uint32_t before = b->st_trigs;
            breaks_step(b, s, bar, sp16, g_step_mask[s]);
            if (b->st_trigs != before && g_ntrig < MAXTRIG)
                g_trig[g_ntrig++] = (trig_t){t, b->st_slice, b->st_div, b->st_bank, b->st_forced};
            long end = t0 + lrint((double)(bar * 16 + s + 1) * (double)sp16);
            while (t < end && t < MAXOUT) {
                int n = (int)(end - t > 256 ? 256 : end - t);
                breaks_render(b, g_out + t, n);
                t += n;
            }
        }
    if (stop_at_end)
        breaks_stop(b);
    return t;
}

static void setup(breaks_t *b)
{
    breaks_init(b);
    breaks_set_loop(b, 0, &g_loop[0]);
    for (int s = 0; s < 16; s++) g_step_mask[s] = 1;
    g_ntrig = 0;
}

/* trace-only generator run: decisions per step (no rendering needed) */
typedef struct { int bar, step, slice, bp, div, bank, forced; } dec_t;
static dec_t g_dec_log[MAXTRIG];
static int run_steps(breaks_t *b, int bars)
{
    int n = 0;
    for (int bar = 0; bar < bars; bar++)
        for (int s = 0; s < 16; s++) {
            uint32_t before = b->st_trigs;
            breaks_step(b, s, bar, 5512.5f, 1);
            if (b->st_trigs != before && n < MAXTRIG)
                g_dec_log[n++] = (dec_t){bar, s, b->st_slice, b->st_bp, b->st_div, b->st_bank, b->st_forced};
        }
    return n;
}

/* ---------------------------------------------------------------------------------------- */
/* analysis                                                                                  */

static double goertzel(const float *x, int n, double hz)
{
    double w = 2.0 * M_PI * hz / 44100.0, c = 2.0 * cos(w), s1 = 0, s2 = 0;
    for (int i = 0; i < n; i++) {
        double win = 0.5 - 0.5 * cos(2.0 * M_PI * i / (n - 1)), s0 = (double)x[i] * win + c * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

/* which source 8th (0..15) a window of output sounds like, at playback ratio r (out Hz / src Hz) */
static int identify(const float *x, int n, double r, int variant, double *energy)
{
    int best = -1;
    double bp = 0, e = 0;
    for (int i = 0; i < n; i++) e += (double)x[i] * (double)x[i];
    for (int j = 0; j < 16; j++) {
        double p = goertzel(x, n, tone_hz(j, variant) * r);
        if (p > bp) { bp = p; best = j; }
    }
    if (energy) *energy = e / n;
    return best;
}

static double max_jump(const float *x, long n)
{
    double m = 0;
    for (long i = 1; i < n; i++) {
        double d = fabs((double)x[i] - (double)x[i - 1]);
        if (d > m) m = d;
    }
    return m;
}

/* the largest sample-to-sample step of the decoded loop played straight at ratio inc
 * (linear interpolation, as the player does), in the player's output scale */
static double ref_jump(double inc, float lvl)
{
    double m = 0, prev = 0;
    for (long i = 0;; i++) {
        double p = i * inc;
        long k = (long)p;
        if (k + 1 >= LOOP_N) break;
        double y = (g_dec[k] + (g_dec[k + 1] - g_dec[k]) * (p - k)) * (double)lvl / 32768.0;
        if (i && fabs(y - prev) > m) m = fabs(y - prev);
        prev = y;
    }
    return m;
}

/* ---------------------------------------------------------------------------------------- */
/* 1. BB Gen's slice_select tests                                                             */

static void test_slice_select(void)
{
    breaks_t b;
    static const float locked[8] = {0.0f, 0.5f, 1.0f, 0.7f, 0.0f, 0.5f, 1.0f, 1.2f};
    for (int i = 0; i < 8; i++) {
        CHECK(fabsf(brk_weight_at(i, 0.0f) - 1.0f) < 1e-5f, "weight_at(%d, 0) == 1", i);
        CHECK(fabsf(brk_weight_at(i, 1.0f) - locked[i]) < 1e-5f, "weight_at(%d, 1) locked curve", i);
        CHECK(fabsf(brk_weight_at(i, 0.5f) - (0.5f + 0.5f * locked[i])) < 1e-5f, "weight_at(%d, .5)", i);
    }
    {
        brk_sel_t in = {0, 0, 0.5f, 0.7f, 0.3f, 1.0f, 0, 0};
        float c, a, r;
        brk_apply_phrase(&in, &c, &a, &r);
        CHECK(c == 0.5f && a == 0.7f && r == 0.3f, "phrase off: no modulation");
        in.phrase_bars = 4;
        brk_apply_phrase(&in, &c, &a, &r);
        CHECK(c == 0.5f && a == 0.7f && r == 0.3f, "non-fill bar: no modulation");
        in.bar_in_phrase = 3;
        brk_apply_phrase(&in, &c, &a, &r);
        CHECK(fabsf(c - 1.0f) < 1e-5f && a == 0.0f && r == 0.0f, "fill=1: c->1, anchor, roll -> 0");
        in.anchor = 0.8f; in.roll = 0.6f; in.fill = 0.5f;
        brk_apply_phrase(&in, &c, &a, &r);
        CHECK(fabsf(c - 0.75f) < 1e-5f && fabsf(a - 0.4f) < 1e-5f && fabsf(r - 0.3f) < 1e-5f, "fill=.5 halfway");
    }
    breaks_init(&b);
    b.rng = 12345;
    {
        brk_sel_t in = {3, 3, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0};
        CHECK(brk_select_next(&in, &b) == 3, "sequential: beat_position 3");
        in.current_slice = 7; in.beat_position = 7;
        CHECK(brk_select_next(&in, &b) == 7, "sequential: beat_position 7");
        in.current_slice = 1; in.beat_position = 5;
        CHECK(brk_select_next(&in, &b) == 5, "after a swap the beat plays its own slice");
    }
    {   /* Roll=1 Anchor=0 from 4: never 4, >= 80 % +-1 walks */
        brk_sel_t in = {4, 4, 0.5f, 0.0f, 1.0f, 0.0f, 0, 0};
        int rep = 0, walk = 0;
        b.rng = 0x1234abcd;
        for (int i = 0; i < 2000; i++) {
            int s = brk_select_next(&in, &b);
            rep += s == 4;
            walk += s == 3 || s == 5;
        }
        CHECK(rep == 0, "Roll=1 Anchor=0: never repeats 4 (%d)", rep);
        CHECK(walk > 1600, "Roll=1 Anchor=0: >= 80 %% walks (%d/2000)", walk);
    }
    {   /* Roll=1 Anchor=1 slice 0: repeat >= 80 % */
        brk_sel_t in = {0, 0, 0.5f, 1.0f, 1.0f, 0.0f, 0, 0};
        int rep = 0;
        b.rng = 0xfacefeed;
        for (int i = 0; i < 2000; i++) rep += brk_select_next(&in, &b) == 0;
        CHECK(rep >= 1600, "Roll=1 Anchor=1 slice 0 repeats >= 80 %% (%d)", rep);
    }
    {   /* Complexity=1: uniform */
        brk_sel_t in = {0, 0, 1.0f, 0.0f, 0.0f, 0.0f, 0, 0};
        int cnt[8] = {0}, ok = 1;
        b.rng = 0x55aa55aa;
        for (int i = 0; i < 8000; i++) cnt[brk_select_next(&in, &b)]++;
        for (int i = 0; i < 8; i++) ok &= cnt[i] >= 750 && cnt[i] <= 1250;
        CHECK(ok, "Complexity=1: uniform per slice");
    }
    {   /* fill bar releases the anchor lock */
        brk_sel_t in = {0, 0, 1.0f, 1.0f, 1.0f, 1.0f, 4, 3};
        int rep = 0;
        b.rng = 0xc0ffee;
        for (int i = 0; i < 2000; i++) rep += brk_select_next(&in, &b) == 0;
        CHECK(rep <= 400, "fill bar: anchor lock released (%d)", rep);
    }
    {   /* Anchor=1 Complexity=0 Roll=0 locks to the beat position */
        brk_sel_t in = {0, 0, 0.0f, 1.0f, 0.0f, 0.0f, 0, 0};
        int ok = 1;
        b.rng = 0xa11ce1;
        for (int bp = 0; bp < 8; bp++) {
            in.beat_position = bp;
            in.current_slice = (bp + 3) & 7;
            ok &= brk_select_next(&in, &b) == bp;
        }
        CHECK(ok, "Anchor=1 locks slice to beat_position");
    }
}

/* 2. BB Gen's perf tests (this part's subset) */
static void test_perf(void)
{
    brk_perf_t p;
    memset(&p, 0, sizeof p);
    p.rate_mult = 1.0f;
    perf_push(&p, 1); perf_push(&p, 2); perf_push(&p, 3);
    CHECK(p.count == 3 && perf_top(&p) == 3, "stack depth 3, top 3");
    perf_release(&p, 2);
    CHECK(p.count == 2 && perf_top(&p) == 3, "release middle keeps top");
    perf_release(&p, 3);
    CHECK(perf_top(&p) == 1, "top falls back to 1");
    perf_release(&p, 1);
    perf_release(&p, 5);
    CHECK(p.count == 0 && perf_top(&p) == -1, "empty, release of absent is a no-op");
    perf_push(&p, 4); perf_push(&p, 4); perf_push(&p, 4);
    CHECK(p.count == 1, "repeated presses dedupe");
    perf_release(&p, 4);
    CHECK(p.count == 0, "one release clears it");
    perf_push(&p, 1); perf_push(&p, 2); perf_push(&p, 1);
    CHECK(p.count == 2 && perf_top(&p) == 1, "re-press moves to top");
    perf_half(&p, 1); perf_half(&p, 1);
    CHECK(p.rate_mult == 0.5f, "duplicate half stays 0.5");
    {
        int f[6];
        p.trig_acc = 0.0f;
        for (int i = 0; i < 6; i++) f[i] = perf_trigger_fires(&p);
        CHECK(!f[0] && f[1] && !f[2] && f[3] && !f[4] && f[5], "half speed fires every other trigger");
    }
    perf_half(&p, 0);
    CHECK(p.rate_mult == 1.0f, "release half -> 1.0");
    {
        int all = 1;
        for (int i = 0; i < 5; i++) all &= perf_trigger_fires(&p);
        CHECK(all, "1x fires every trigger");
    }
}

/* 3. the generator through breaks_step */
static void test_generator(void)
{
    breaks_t b;
    int n, ok, cnt;

    /* Complexity 0 + Anchor 0: slices advance in order (1-bar length: one per 8th) */
    setup(&b);
    breaks_set(&b, BRK_COMPLEX, 0);
    n = run_steps(&b, 32);
    ok = n == 32 * 8;
    for (int i = 0; i < n; i++) ok &= g_dec_log[i].slice == (i & 7) && g_dec_log[i].step == (i & 7) * 2;
    CHECK(ok, "Complexity 0: slices 0..7 in order on every 8th (%d triggers)", n);

    /* Complexity 30, Anchor 0, Roll 0: still mostly in order */
    setup(&b);
    breaks_set(&b, BRK_COMPLEX, 30);
    n = run_steps(&b, 64);
    cnt = 0;
    for (int i = 0; i < n; i++) cnt += g_dec_log[i].slice == g_dec_log[i].bp;
    CHECK(cnt > n * 70 / 100 && cnt < n * 85 / 100, "Complexity 30: ~74 %% on their beat slice (%d/%d)", cnt, n);

    /* Anchor 100, Complexity 100, Roll 0: beats 1 and 3 always own slice; others do swap */
    setup(&b);
    breaks_set(&b, BRK_COMPLEX, 100);
    breaks_set(&b, BRK_ANCHOR, 100);
    n = run_steps(&b, 200);
    ok = 1;
    cnt = 0;
    {
        int other = 0, odev = 0;
        for (int i = 0; i < n; i++) {
            dec_t *d = &g_dec_log[i];
            if (d->step == 0) { ok &= d->slice == 0; cnt++; }
            else if (d->step == 8) { ok &= d->slice == 4; cnt++; }
            else { other++; odev += d->slice != d->bp; }
        }
        CHECK(ok && cnt == 400, "Anchor 100: beat 1 -> slice 0 and beat 3 -> slice 4 every bar (%d)", cnt);
        CHECK(odev > other / 4, "Anchor 100, Complexity 100: other 8ths do swap (%d/%d)", odev, other);
    }

    /* Roll 100: mostly repeats / neighbours */
    setup(&b);
    breaks_set(&b, BRK_COMPLEX, 50);
    breaks_set(&b, BRK_ROLL, 100);
    n = run_steps(&b, 100);
    cnt = 0;
    for (int i = 1; i < n; i++) {
        int d = (g_dec_log[i].slice - g_dec_log[i - 1].slice) & 7;
        cnt += d == 0 || d == 1 || d == 7;
    }
    CHECK(cnt >= (n - 1) * 90 / 100, "Roll 100: >= 90 %% repeat or +-1 (%d/%d)", cnt, n - 1);
    setup(&b);
    breaks_set(&b, BRK_COMPLEX, 50);
    breaks_set(&b, BRK_ROLL, 100);
    breaks_set(&b, BRK_ANCHOR, 100);
    n = run_steps(&b, 100);
    cnt = 0;
    for (int i = 1; i < n; i++) cnt += g_dec_log[i].slice == g_dec_log[i - 1].slice;
    CHECK(cnt >= (n - 1) * 70 / 100, "Roll 100 + Anchor 100: camps (repeats %d/%d)", cnt, n - 1);
    {
        int esc = 0;                          /* README: escape fires ~every 20 triggers; not stuck */
        for (int i = 1; i < n; i++) {
            int d = (g_dec_log[i].slice - g_dec_log[i - 1].slice) & 7;
            esc += d >= 2 && d <= 4;
        }
        CHECK(esc > (n - 1) / 40 && esc < (n - 1) / 10, "Roll 100: escape jumps ~5 %% (%d/%d)", esc, n - 1);
    }

    /* Retrig 2x at 100: every trigger (= every beat at 2-bar length) stutters 2x; at 0 never */
    setup(&b);
    breaks_set(&b, BRK_ALEN, 3);
    breaks_set(&b, BRK_R2, 100);
    n = run_steps(&b, 50);
    ok = n == 50 * 4;
    for (int i = 0; i < n; i++) ok &= g_dec_log[i].forced || g_dec_log[i].div == 2;
    for (int i = 0; i < n; i++) ok &= g_dec_log[i].step % 4 == 0;
    CHECK(ok, "Retrig 2x 100: every beat 2x (%d triggers)", n);
    setup(&b);
    breaks_set(&b, BRK_ALEN, 3);
    n = run_steps(&b, 50);
    ok = 1;
    for (int i = 0; i < n; i++) ok &= g_dec_log[i].div == 0;
    CHECK(ok, "Retrig 0: never");
    /* per-bar odds: Retrig 4x at 50 at 1-bar length -> ~50 % of bars have at least one 4x */
    setup(&b);
    breaks_set(&b, BRK_R4, 50);
    n = run_steps(&b, 400);
    {
        int bars_hit = 0, lastbar = -1;
        for (int i = 0; i < n; i++)
            if (g_dec_log[i].div == 4 && g_dec_log[i].bar != lastbar) { bars_hit++; lastbar = g_dec_log[i].bar; }
        CHECK(bars_hit > 170 && bars_hit < 230, "Retrig 4x 50: per-bar odds ~50 %% (%d/400 bars)", bars_hit);
    }

    /* Phrase 4 + Fill 100: the 4th bar deviates. Complexity 10 (BB Gen bypasses the generator
     * entirely at Complexity 0, so Fill does nothing there -- kept). */
    setup(&b);
    breaks_set(&b, BRK_COMPLEX, 10);
    breaks_set(&b, BRK_PHRASE, 2);
    breaks_set(&b, BRK_FILL, 100);
    n = run_steps(&b, 160);
    {
        int gm = 0, gn = 0, fm = 0, fn = 0;
        for (int i = 0; i < n; i++) {
            dec_t *d = &g_dec_log[i];
            if (d->forced) continue;
            if (d->bar % 4 == 3) { fn++; fm += d->slice == d->bp; }
            else { gn++; gm += d->slice == d->bp; }
        }
        CHECK(gm > gn * 85 / 100, "Phrase 4: groove bars on their beat slice (%d/%d)", gm, gn);
        CHECK(fm < fn * 25 / 100, "Phrase 4 Fill 100: fill bar deviates (%d/%d on beat)", fm, fn);
    }
    /* Complexity 0 + Fill 100: no deviation (BB Gen's bypass) */
    setup(&b);
    breaks_set(&b, BRK_COMPLEX, 0);
    breaks_set(&b, BRK_PHRASE, 2);
    breaks_set(&b, BRK_FILL, 100);
    n = run_steps(&b, 16);
    ok = 1;
    for (int i = 0; i < n; i++) ok &= g_dec_log[i].slice == g_dec_log[i].bp;
    CHECK(ok, "Complexity 0: Fill has no effect (BB Gen's bypass)");

    /* B chance 100: loop B on every fill bar, A elsewhere; slice 0 forced at both switches */
    setup(&b);
    breaks_set_loop(&b, 1, &g_loop[1]);
    breaks_set(&b, BRK_PHRASE, 2);
    breaks_set(&b, BRK_BCHANCE, 100);
    n = run_steps(&b, 40);
    ok = 1;
    for (int i = 0; i < n; i++) {
        dec_t *d = &g_dec_log[i];
        ok &= d->bank == (d->bar % 4 == 3);
        if (d->step == 0 && (d->bar % 4 == 3 || d->bar % 4 == 0)) ok &= d->forced && d->slice == 0;
    }
    CHECK(ok, "B chance 100: B on the 4th bar of each phrase, A on the others");
    setup(&b);
    breaks_set(&b, BRK_PHRASE, 2);
    breaks_set(&b, BRK_BCHANCE, 100);         /* no B loaded: stays on A */
    n = run_steps(&b, 16);
    ok = 1;
    for (int i = 0; i < n; i++) ok &= g_dec_log[i].bank == 0;
    CHECK(ok, "B chance 100 with no B loop: stays on A");
    setup(&b);
    breaks_set_loop(&b, 1, &g_loop[1]);
    breaks_set(&b, BRK_PHRASE, 1);            /* Phrase 2: B every other bar (BB Gen's reset pre-schedule) */
    breaks_set(&b, BRK_BCHANCE, 100);
    n = run_steps(&b, 8);
    ok = 1;
    for (int i = 0; i < n; i++) ok &= g_dec_log[i].bank == (g_dec_log[i].bar & 1);
    CHECK(ok, "Phrase 2 + B chance 100: B on bars 1, 3, 5, 7");

    /* determinism: same settings, same seed -> same decisions */
    {
        int a[256], m;
        setup(&b);
        breaks_set(&b, BRK_ROLL, 40);
        breaks_set(&b, BRK_R2, 40);
        m = run_steps(&b, 16);
        for (int i = 0; i < m; i++) a[i] = g_dec_log[i].slice * 16 + g_dec_log[i].div;
        setup(&b);
        breaks_set(&b, BRK_ROLL, 40);
        breaks_set(&b, BRK_R2, 40);
        n = run_steps(&b, 16);
        ok = n == m;
        for (int i = 0; i < n && ok; i++) ok &= a[i] == g_dec_log[i].slice * 16 + g_dec_log[i].div;
        CHECK(ok, "reproducible from the seed");
    }
}

/* ---------------------------------------------------------------------------------------- */
/* 4. audio: the right slice at the right time, at 90 and 170 BPM                            */

static int g_click_fail;

static void test_audio(float bpm)
{
    breaks_t b;
    float sp16 = 44100.0f * 60.0f / bpm / 4.0f;
    double r = (double)bpm / 120.0;                   /* output Hz per source Hz: a 2-bar loop at 120 BPM */
    int eighth = (int)(2.0f * sp16), skip = 132, wl = eighth - skip - 44;
    long t;
    int win = 0, good = 0;

    /* (a) in order, Complexity 0: 8ths 0..15 again and again, independent of the trace */
    setup(&b);
    breaks_set(&b, BRK_COMPLEX, 0);
    breaks_set(&b, BRK_ALEN, 3);              /* A length 2 bars = the loop's bars */
    t = run(&b, bpm, 8, 0, 1);
    for (int k = 0; k < 64; k++) {
        long w0 = lrint(k * 2.0 * (double)sp16) + skip;
        int id = identify(g_out + w0, wl, r, 0, 0);
        win++;
        good += id == (k & 15);
    }
    CHECK(good == win, "%.0f BPM in order: %d/%d 8ths identified as 8th k mod 16", (double)bpm, good, win);
    double rj = ref_jump(r * 0.5, 100.0f / 127.0f * 100.0f / 127.0f);
    double mj = max_jump(g_out, t);
    printf("  %.0f BPM in order: max |dy| %.4f, straight-playback reference %.4f\n", (double)bpm, mj, rj);

    /* (b) generator with retrigs: each trigger's two 8ths against its chosen slice */
    setup(&b);
    breaks_set(&b, BRK_ALEN, 3);
    breaks_set(&b, BRK_COMPLEX, 70);
    breaks_set(&b, BRK_ROLL, 30);
    breaks_set(&b, BRK_ANCHOR, 50);
    breaks_set(&b, BRK_R2, 30);
    breaks_set(&b, BRK_R3, 20);
    breaks_set(&b, BRK_R4, 20);
    breaks_set(&b, BRK_R8, 20);
    t = run(&b, bpm, 16, 0, 1);
    win = good = 0;
    int ndiv = 0, nmove = 0;
    for (int i = 0; i < g_ntrig; i++) {
        trig_t *g = &g_trig[i];
        int exp0 = 2 * g->slice, exp1 = g->div ? 2 * g->slice : 2 * g->slice + 1;
        int id0 = identify(g_out + g->t + skip, wl, r, 0, 0);
        int id1 = identify(g_out + g->t + eighth + skip, wl, r, 0, 0);
        win += 2;
        good += (id0 == exp0) + (id1 == exp1);
        ndiv += g->div != 0;
        nmove += i && g->slice != (g_trig[i - 1].slice + 1) % 8;
        if (id0 != exp0 || id1 != exp1)
            printf("    trig %d t=%ld slice %d div %d: heard %d %d, expected %d %d\n", i, g->t, g->slice, g->div,
                   id0, id1, exp0, exp1);
    }
    printf("  %.0f BPM generated: %d/%d 8ths match, %d triggers, %d retriggered, %d out-of-order\n", (double)bpm, good,
           win, g_ntrig, ndiv, nmove);
    CHECK(good == win, "%.0f BPM generated: %d/%d 8ths match the chosen slice (%d triggers, %d retrig, %d jumps)",
          (double)bpm, good, win, g_ntrig, ndiv, nmove);
    mj = max_jump(g_out, t);
    printf("  %.0f BPM generated: max |dy| %.4f (bound 1.25 x %.4f)\n", (double)bpm, mj, rj);
    if (mj > 1.25 * rj) g_click_fail++;
#ifndef BRK_CLICK_CONTROL
    CHECK(mj <= 1.25 * rj, "%.0f BPM: no clicks at slice boundaries (%.4f <= %.4f)", (double)bpm, mj, 1.25 * rj);
#endif
}

/* 5. the step gate: enabled=0 silences within 3 ms, and re-enabling resumes in place */
static void test_gate(void)
{
    breaks_t b;
    float sp16 = 44100.0f * 60.0f / 120.0f / 4.0f, peak = 0.0f, late = 0.0f;
    setup(&b);
    breaks_set(&b, BRK_COMPLEX, 0);
    breaks_set(&b, BRK_ALEN, 3);
    g_step_mask[5] = 0;                       /* 16th 5 and 6 off: mid-8th 2 .. into 8th 3 */
    g_step_mask[6] = 0;
    long t = run(&b, 120.0f, 2, 0, 0);
    for (int bar = 0; bar < 2; bar++) {
        long s5 = lrint((bar * 16 + 5) * (double)sp16), s7 = lrint((bar * 16 + 7) * (double)sp16);
        for (long i = s5 - 400; i < s5; i++) peak = fmaxf(peak, fabsf(g_out[i]));
        for (long i = s5 + 132; i < s7; i++) late = fmaxf(late, fabsf(g_out[i]));
        int id = identify(g_out + s7 + 132, (int)sp16 - 180, 1.0, 0, 0);   /* 16th 7 = second half of 8th 3 */
        CHECK(id == bar * 8 + 3, "gate: re-enabled step resumes in place (8th %d, heard %d)", bar * 8 + 3, id);
    }
    CHECK(peak > 0.05f && late == 0.0f, "enabled=0: silent 3 ms after the step (before %.3f, after %.6f)",
          (double)peak, (double)late);
    (void)t;
    /* breaks_stop fades out too */
    {
        float tail = 0.0f, buf[256];
        breaks_stop(&b);
        for (int k = 0; k < 4; k++) {
            breaks_render(&b, buf, 256);
            if (k) for (int i = 0; i < 256; i++) tail = fmaxf(tail, fabsf(buf[i]));
        }
        CHECK(tail == 0.0f && !b.v[0].on && !b.v[1].on, "stop: silent and voices released");
    }
}

/* 6. live keys */
static void test_live(void)
{
    breaks_t b;
    float sp16 = 5512.5f, buf[256];
    static float cap[44100];
    setup(&b);
    breaks_set(&b, BRK_ALEN, 3);
    breaks_set(&b, BRK_COMPLEX, 0);
    /* stopped: holding key 5 auditions slice 5 (8ths 10, 11) */
    breaks_live(&b, 5, 1);
    for (int k = 0; k < 40; k++) breaks_render(&b, cap + k * 256, 256);
    CHECK(identify(cap + 132, 5000, 1.0, 0, 0) == 10, "stopped, key 5 held: slice 5 sounds");
    breaks_live(&b, 5, 0);
    for (int k = 0; k < 2; k++) breaks_render(&b, buf, 256);
    breaks_render(&b, buf, 256);
    {
        float m = 0.0f;
        for (int i = 0; i < 256; i++) m = fmaxf(m, fabsf(buf[i]));
        CHECK(m == 0.0f, "stopped, key released: silent");
    }
    /* running: a held slice replaces the engine at every trigger */
    setup(&b);
    breaks_set(&b, BRK_ALEN, 3);
    breaks_set(&b, BRK_COMPLEX, 50);
    breaks_live(&b, 2, 1);
    breaks_live(&b, 6, 1);                    /* last-note priority: 6 */
    int n = run_steps(&b, 2), ok = 1;
    for (int i = 0; i < n; i++) ok &= g_dec_log[i].slice == 6;
    breaks_live(&b, 6, 0);                    /* falls back to 2 */
    n = run_steps(&b, 1);
    for (int i = 0; i < n; i++) ok &= g_dec_log[i].slice == 2;
    breaks_live(&b, 2, 0);
    CHECK(ok, "held slices override the generator, last held wins, release falls back");
    /* half speed: every other trigger; stutter: 4x on every trigger */
    setup(&b);
    breaks_set(&b, BRK_COMPLEX, 0);
    breaks_live(&b, 9, 1);
    n = run_steps(&b, 2);
    CHECK(n == 8, "half speed: every other trigger fires (%d in 2 bars incl. the start)", n);
    breaks_live(&b, 9, 0);
    breaks_live(&b, 10, 1);
    n = run_steps(&b, 1);
    ok = n == 8;
    for (int i = 0; i < n; i++) ok &= g_dec_log[i].div == 4 || g_dec_log[i].forced;
    CHECK(ok, "stutter: 4x on every trigger");
    breaks_live(&b, 10, 0);
    /* reverse: slice 0 played backwards = its second 8th first */
    setup(&b);
    breaks_set(&b, BRK_ALEN, 3);
    breaks_set(&b, BRK_COMPLEX, 0);
    breaks_live(&b, 8, 1);
    long t = run(&b, 120.0f, 1, 0, 1);
    CHECK(identify(g_out + 600, 4000, 1.0, 0, 0) == 1 && identify(g_out + (long)(2 * sp16) + 600, 4000, 1.0, 0, 0) == 0,
          "reverse: slice 0 plays 8th 1 then 8th 0");
    breaks_live(&b, 8, 0);
    (void)t;
    /* replacing loop A while playing kills its voices instead of reading a stale loop */
    setup(&b);
    run(&b, 120.0f, 1, 0, 0);
    breaks_set_loop(&b, 0, 0);
    breaks_render(&b, buf, 256);
    CHECK(!b.v[0].on && !b.v[1].on, "loop removed: voices dropped");
}

/* ---------------------------------------------------------------------------------------- */
/* 7. demo                                                                                     */

static void put32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); fputc(v >> 16 & 255, f); fputc(v >> 24, f); }
static void put16(FILE *f, uint32_t v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }

static void demo(const char *path)
{
    breaks_t b;
    setup(&b);
    breaks_set_loop(&b, 1, &g_loop[1]);
    breaks_set(&b, BRK_ALEN, 3);
    breaks_set(&b, BRK_BLEN, 3);
    breaks_set(&b, BRK_PHRASE, 2);
    breaks_set(&b, BRK_FILL, 70);
    breaks_set(&b, BRK_ROLL, 40);
    breaks_set(&b, BRK_R2, 30);
    breaks_set(&b, BRK_R4, 30);
    breaks_set(&b, BRK_BCHANCE, 50);
    long t = run(&b, 170.0f, 8, 0, 1);
    for (int k = 0; k < 8; k++) breaks_render(&b, g_out + t + k * 256, 256);
    t += 8 * 256;
    FILE *f = fopen(path, "wb");
    if (!f) { CHECK(0, "cannot write %s", path); return; }
    fputs("RIFF", f); put32(f, (uint32_t)(36 + t * 4)); fputs("WAVEfmt ", f);
    put32(f, 16); put16(f, 1); put16(f, 2); put32(f, 44100); put32(f, 44100 * 4); put16(f, 4); put16(f, 16);
    fputs("data", f); put32(f, (uint32_t)(t * 4));
    for (long i = 0; i < t; i++) {
        long v = lrintf(g_out[i] * 32767.0f);
        v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        put16(f, (uint32_t)(v & 0xFFFF)); put16(f, (uint32_t)(v & 0xFFFF));
    }
    fclose(f);
    printf("  wrote %s: %.2f s, %d triggers, Phrase 4 Fill 70 Roll 40 Rtg2x/4x 30 (Cmplx 50, B chance 50)\n", path,
           (double)t / 44100.0, g_ntrig);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    make_loops();
    printf("sizeof(breaks_t) = %u bytes (bank %u, voice %u)\n", (unsigned)sizeof(breaks_t),
           (unsigned)sizeof(brk_bank_t), (unsigned)sizeof(brk_voice_t));
#ifdef BRK_CLICK_CONTROL
    (void)test_slice_select; (void)test_perf; (void)test_generator; (void)test_gate; (void)test_live; (void)demo;
    printf("positive control: BRK_FADE = %d (crossfade off)\n", BRK_FADE);
    test_audio(90.0f);
    test_audio(170.0f);
    printf("control: click check %s\n", g_click_fail ? "FIRED (as it must)" : "did not fire");
    return g_click_fail ? 3 : 0;
#else
    test_slice_select();
    test_perf();
    test_generator();
    test_audio(90.0f);
    test_audio(170.0f);
    test_gate();
    test_live();
    if (argc > 1) demo(argv[1]);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
#endif
}
