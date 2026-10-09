/* SPDX-License-Identifier: GPL-3.0-only */
/* Generator ported from schwung-breakbeat (BB Gen) by mestela, used with the author's permission.
 *
 * BREAK part: an 8-slice ADPCM loop player driven by BB Gen's generator.
 *
 * What is BB Gen's, kept as its code has it (src/dsp/slice_select.c, perf.c, breakbeat.c):
 *   - slice choice per trigger: Complexity 0 bypasses the generator and plays the trigger's
 *     beat position; otherwise a MOVE branch (probability 1 - Roll: swap to a uniform random
 *     slice with probability Complexity * weight(beat position, Anchor), else play the beat
 *     position) or a STAY branch (5 % escape jump of 2..4 forward, else repeat with probability
 *     1 - weight(current slice, Anchor), else walk +-1). Same draw order.
 *   - the Anchor weight curve {0, .5, 1, .7, 0, .5, 1, 1.2}, linearly blended from 1.0.
 *   - fill bar = last bar of a phrase: Complexity -> c + (1 - c) * Fill, Anchor and Roll * (1 - Fill).
 *   - trigger cadence: one trigger every Length / 8 bars (BB Gen's 12 * length clock ticks);
 *     beat position = trigger count mod 8, the count restarting at transport start and at a bank
 *     switch, where slice 0 is forced.
 *   - retrigger: each of 2x/3x/4x/8x rolls per trigger with p = 1 - (1 - P)^(1 / triggers per bar),
 *     one of the fired rates is picked at random; the slice's head is replayed div times.
 *   - phrase A/B: B is rolled (B chance) at the start of the bar before the fill bar and switched
 *     in at the fill bar's start; A is always scheduled back for the first bar of the next phrase.
 *     (X0X: with Phrase off, B chance rolls at the start of every bar, for that bar.)
 *   - live layer: held slices on a last-note-priority stack (dedup on re-press), held slice beats
 *     the engine (the engine still draws), reverse wraps within the slice, half speed = rate * 0.5
 *     and only every other trigger fires, stutter forces a 4x retrigger on each trigger.
 *
 * What is adapted (the reasons are the FM-1's, see the design doc):
 *   - time comes from breaks_step (every 16th) instead of MIDI clock ticks; the 1/4-bar length's
 *     second trigger inside a 16th is scheduled half a 16th later.
 *   - playback rate follows the loop's stated `bars` (source samples per 16th / output samples per
 *     16th) times Pitch, instead of BB Gen's slice length / trigger interval. The two agree when
 *     A Length equals the loop's bars; otherwise Length only sets how often slices are triggered.
 *   - retrigger sub-hits are timed in output samples (interval / div) instead of source samples,
 *     so they stay on the beat whatever Pitch is; in reverse the reversed head repeats.
 *   - every jump (trigger, sub-hit, reverse flip, wrap at the loop end is seamless) crossfades two
 *     voices over 2 ms; BB Gen jumps play_pos and clicks.
 *   - the pattern's step bit gates the output (2 ms ramp); the generator runs underneath, so a
 *     re-enabled step resumes exactly where the break is. A held slice key opens the gate.
 *   - transport start returns the engine to bank A; randomness is one xorshift32 seeded in init.
 *
 * IMA ADPCM tables and decoder step from Felucca's eng_sample.c
 * (Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hugelton Instruments, GPL-3.0). */
#include "breaks.h"
#include "fastmath.h"

static const int16_t BRK_IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
    4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
    22385, 24623, 27086, 29794, 32767};
static const int8_t BRK_IMA_IDX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

/* ---- parameters ------------------------------------------------------------------------- */

static const char *const N_PHRASE[] = {"Off", "2", "4", "8", "16"};
static const char *const N_LEN[] = {"1/4", "1/2", "1", "2", "4", "8"};
static const char *const N_PITCH[] = {"-12", "-11", "-10", "-9", "-8", "-7", "-6", "-5", "-4", "-3", "-2",
                                      "-1", "0", "+1", "+2", "+3", "+4", "+5", "+6", "+7", "+8", "+9",
                                      "+10", "+11", "+12"};
/* defaults are BB Gen's module.json defaults */
static const x0x_param_t BRK_PARAMS[BRK_NPARAM] = {
    {"Cmplx", 100, 50, 0},  {"Anchor", 100, 0, 0}, {"Roll", 100, 0, 0},   {"Fill", 100, 0, 0},
    {"Rtg2x", 100, 0, 0},   {"Rtg3x", 100, 0, 0},  {"Rtg4x", 100, 0, 0},  {"Rtg8x", 100, 0, 0},
    {"Phrase", 4, 0, N_PHRASE}, {"BChnc", 100, 0, 0}, {"A Len", 5, 2, N_LEN}, {"B Len", 5, 2, N_LEN},
    {"Level", 127, 100, 0}, {"Pitch", 24, 12, N_PITCH}, {"Rev", 127, 0, 0}, {"Dly", 127, 0, 0},
};
static const uint8_t PHRASE_BARS[5] = {0, 2, 4, 8, 16};
static const uint8_t LEN_TICKS[6] = {3, 6, 12, 24, 48, 96};   /* BB Gen: 12 * length, 24 ppq */

int breaks_nparams(void) { return BRK_NPARAM; }
const x0x_param_t *breaks_param(int i) { return (i >= 0 && i < BRK_NPARAM) ? &BRK_PARAMS[i] : 0; }
void breaks_set(breaks_t *b, int i, int value)
{
    if (i < 0 || i >= BRK_NPARAM)
        return;
    if (value < 0)
        value = 0;
    if (value > BRK_PARAMS[i].max)
        value = BRK_PARAMS[i].max;
    b->set[i] = (uint8_t)value;
}
int breaks_get(const breaks_t *b, int i) { return (i >= 0 && i < BRK_NPARAM) ? b->set[i] : 0; }
float breaks_send(const breaks_t *b, int which)
{
    return (float)b->set[which ? BRK_DLY : BRK_REV] * (1.0f / 127.0f);
}

/* ---- randomness ------------------------------------------------------------------------- */

static uint32_t brk_rand_u(breaks_t *b)
{
    uint32_t x = b->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    b->rng = x;
    return x;
}
static float brk_rand(breaks_t *b) { return (float)(brk_rand_u(b) >> 8) * (1.0f / 16777216.0f); }

/* ---- BB Gen's slice selection (slice_select.c) ------------------------------------------ */

typedef struct {
    int current_slice, beat_position;
    float complexity, anchor, roll, fill;     /* 0..1 */
    int phrase_bars, bar_in_phrase;
} brk_sel_t;

#define BRK_ESCAPE_P 0.05f

static float brk_weight_at(int slice, float anchor)
{
    static const float locked[8] = {0.0f, 0.5f, 1.0f, 0.7f, 0.0f, 0.5f, 1.0f, 1.2f};
    slice = slice < 0 ? 0 : slice > 7 ? 7 : slice;
    anchor = fm_clampf(anchor, 0.0f, 1.0f);
    return (1.0f - anchor) + locked[slice] * anchor;
}

static void brk_apply_phrase(const brk_sel_t *in, float *c, float *a, float *r)
{
    float f;
    *c = in->complexity;
    *a = in->anchor;
    *r = in->roll;
    if (in->phrase_bars <= 0 || in->bar_in_phrase != in->phrase_bars - 1)
        return;
    f = fm_clampf(in->fill, 0.0f, 1.0f);
    *c = in->complexity + (1.0f - in->complexity) * f;
    *a = in->anchor * (1.0f - f);
    *r = in->roll * (1.0f - f);
}

static int brk_select_next(const brk_sel_t *in, breaks_t *b)
{
    float c, a, r;
    brk_apply_phrase(in, &c, &a, &r);
    if (brk_rand(b) < 1.0f - r) {                          /* MOVE */
        if (brk_rand(b) < c * brk_weight_at(in->beat_position, a)) {
            int j = (int)(brk_rand(b) * 8.0f);
            return j < 0 ? 0 : j > 7 ? 7 : j;
        }
        return in->beat_position;                          /* no swap: the beat's own slice */
    }
    if (brk_rand(b) < BRK_ESCAPE_P) {                      /* STAY: escape 2..4 forward */
        int j = 2 + (int)(brk_rand(b) * 3.0f);
        j = j < 2 ? 2 : j > 4 ? 4 : j;
        return (in->current_slice + j) & 7;
    }
    if (brk_rand(b) < 1.0f - brk_weight_at(in->current_slice, a))
        return in->current_slice;                          /* repeat */
    return (in->current_slice + (brk_rand(b) < 0.5f ? 1 : 7)) & 7;   /* walk +-1 */
}

/* ---- BB Gen's live layer (perf.c), this part's subset ------------------------------------ */

static void perf_release(brk_perf_t *p, int slice)
{
    int i, found = -1;
    for (i = (int)p->count - 1; i >= 0; i--)
        if (p->stack[i] == slice) {
            found = i;
            break;
        }
    if (found < 0)
        return;
    for (i = found; i < (int)p->count - 1; i++)
        p->stack[i] = p->stack[i + 1];
    p->count--;
}
static void perf_push(brk_perf_t *p, int slice)
{
    if (slice < 0 || slice > 7)
        return;
    perf_release(p, slice);                                /* a re-press moves it to the top */
    if (p->count >= 8)
        return;
    p->stack[p->count++] = (int8_t)slice;
}
static int perf_top(const brk_perf_t *p) { return p->count ? p->stack[p->count - 1] : -1; }
static void perf_half(brk_perf_t *p, int on)
{
    if (on && !p->half) {
        p->half = 1;
        p->rate_mult *= 0.5f;
    } else if (!on && p->half) {
        p->half = 0;
        p->rate_mult *= 2.0f;
    }
}
/* 1 = this clock trigger fires; half speed fires every other one */
static int perf_trigger_fires(brk_perf_t *p)
{
    float m = p->rate_mult > 0.0f ? p->rate_mult : 1.0f;
    p->trig_acc += m < 1.0f ? m : 1.0f;
    if (p->trig_acc >= 0.999f) {
        p->trig_acc -= 1.0f;
        return 1;
    }
    return 0;
}

/* ---- ADPCM ------------------------------------------------------------------------------ */

static inline int32_t ima_decode(const uint8_t *d, uint32_t i, int32_t *pred, int32_t *idx)
{
    uint32_t byte = d[i >> 1], code = (i & 1u) ? (byte >> 4) : (byte & 15u);
    int32_t step = BRK_IMA_STEP[*idx], vd = step >> 3, p, x;
    if (code & 4u)
        vd += step;
    if (code & 2u)
        vd += step >> 1;
    if (code & 1u)
        vd += step >> 2;
    p = *pred + ((code & 8u) ? -vd : vd);
    *pred = p < -32768 ? -32768 : p > 32767 ? 32767 : p;
    x = *idx + BRK_IMA_IDX[code & 7u];
    *idx = x < 0 ? 0 : x > 88 ? 88 : x;
    return *pred;
}

/* decoder state before nibble p (p < n) */
static void bank_seek(const brk_bank_t *k, uint32_t p, int32_t *pred, int32_t *idx)
{
    uint32_t c = p >> k->ck_shift, i = c << k->ck_shift;
    *pred = k->ck[c].pred;
    *idx = k->ck[c].idx;
    while (i < p)
        ima_decode(k->adpcm, i++, pred, idx);
}

void breaks_set_loop(breaks_t *b, int which, const brk_loop_t *loop)
{
    brk_bank_t *k;
    uint32_t n, i, s, sh;
    int32_t pred = 0, idx = 0;
    if (which < 0 || which > 1)
        return;
    k = &b->bank[which];
    k->valid = 0;
    k->gen++;
    if (!loop || !loop->adpcm || loop->nsamples < 64 || !loop->bars)
        return;
    n = loop->nsamples;
    for (sh = 6; ((n + (1u << sh) - 1u) >> sh) > BRK_NCK; sh++)
        ;
    k->ck_shift = (uint8_t)sh;
    k->adpcm = loop->adpcm;
    k->n = n;
    k->sp16 = (float)n / ((float)loop->bars * 16.0f);
    for (s = 0; s <= 8; s++)
        k->start[s] = (n >> 3) * s + ((n & 7u) * s >> 3);  /* n * s / 8 without overflow */
    for (i = 0, s = 0; i < n; i++) {                       /* one pass: record states, decode on */
        if (!(i & ((1u << sh) - 1u))) {
            k->ck[i >> sh].pred = (int16_t)pred;
            k->ck[i >> sh].idx = (uint8_t)idx;
        }
        while (s < 8 && k->start[s] == i) {
            k->at_start[s].pred = (int16_t)pred;
            k->at_start[s].idx = (uint8_t)idx;
            s++;
        }
        ima_decode(k->adpcm, i, &pred, &idx);
    }
    k->valid = 1;
}

/* ---- voices ----------------------------------------------------------------------------- */

static float brk_inc(const breaks_t *b, const brk_bank_t *k)
{
    float sp16 = b->sp16 > 1.0f ? b->sp16 : (float)BRK_FS * 0.125f;   /* 120 BPM until told */
    float semi = (float)((int)b->set[BRK_PITCH] - 12);
    return k->sp16 / sp16 * fm_exp2f(semi * (1.0f / 12.0f)) * b->perf.rate_mult;
}

static float rev_fetch(brk_voice_t *v, const brk_bank_t *k, uint32_t q)
{
    if (!v->rvalid || q < v->rbase || q >= v->rbase + BRK_RCH) {
        uint32_t base = q & ~(uint32_t)(BRK_RCH - 1), m = k->n - base, i;
        int32_t pred, idx;
        if (m > BRK_RCH)
            m = BRK_RCH;
        bank_seek(k, base, &pred, &idx);
        for (i = 0; i < m; i++)
            v->rbuf[i] = (int16_t)ima_decode(k->adpcm, base + i, &pred, &idx);
        v->rbase = base;
        v->rvalid = 1;
    }
    return (float)v->rbuf[q - v->rbase];
}
static inline uint32_t rev_next(const brk_voice_t *v, uint32_t q) { return q > v->lo ? q - 1u : v->hi - 1u; }

/* start a new voice at sample p of bank bk, slice sl, crossfading out the current one.
 * have_state: (pred, idx) is the decoder state before nibble p; otherwise it is sought. */
static void voice_start(breaks_t *b, int bk, int sl, uint32_t p, int rev, int have_state, int32_t pred,
                        int32_t idx)
{
    const brk_bank_t *k = &b->bank[bk];
    brk_voice_t *o = &b->v[b->cur], *v;
    if (!k->valid)
        return;
    if (o->on)
        o->dg = -1.0f / (float)BRK_FADE;                   /* fade the old one out from where it is */
    b->cur ^= 1u;
    v = &b->v[b->cur];
    v->on = 1;
    v->bank = (uint8_t)bk;
    v->gen = k->gen;
    v->rev = (uint8_t)rev;
    v->lo = k->start[sl];
    v->hi = k->start[sl + 1];
    v->frac = 0.0f;
    v->g = 0.0f;
    v->dg = 1.0f / (float)BRK_FADE;
    if (p >= k->n)
        p = 0;
    if (rev) {
        v->rvalid = 0;
        v->s0 = rev_fetch(v, k, p);
        v->pos = rev_next(v, p);
        v->s1 = rev_fetch(v, k, v->pos);
    } else {
        if (!have_state)
            bank_seek(k, p, &pred, &idx);
        v->s0 = (float)ima_decode(k->adpcm, p, &pred, &idx);
        if (++p >= k->n) {
            p = 0;
            pred = 0;
            idx = 0;
        }
        v->s1 = (float)ima_decode(k->adpcm, p, &pred, &idx);
        v->pos = p;
        v->pred = pred;
        v->idx = idx;
    }
}

/* (re)start slice sl of bank bk from its beginning: forward from its first sample, reverse
 * from its first sample then wrapping to its end (BB Gen's reverse) */
static void slice_start(breaks_t *b, int bk, int sl)
{
    const brk_bank_t *k = &b->bank[bk];
    voice_start(b, bk, sl, k->start[sl], b->perf.reverse, 1, k->at_start[sl].pred, k->at_start[sl].idx);
}

/* ---- the generator ---------------------------------------------------------------------- */

static int bank_ticks(const breaks_t *b, int bk) { return LEN_TICKS[b->set[bk ? BRK_BLEN : BRK_ALEN] % 6u]; }

/* BB_FIRE_TRIGGER: decide, then sound. forced = transport start / bank switch: slice 0, no retrig */
static void fire_trigger(breaks_t *b, int bp, int forced)
{
    int slice, held, rb, tpt = bank_ticks(b, b->engine_bank), i, n = 0, fired[4];
    if (forced)
        slice = 0;
    else if (b->set[BRK_COMPLEX] == 0)
        slice = bp;
    else {
        brk_sel_t in;
        int pb = PHRASE_BARS[b->set[BRK_PHRASE] % 5u];
        in.current_slice = b->current_slice;
        in.beat_position = bp;
        float ramp = fm_minf((float)b->set[BRK_COMPLEX] * 0.04f, 1.0f);   /* X0X: COMPLEXITY is the amount: */
        in.complexity = (float)b->set[BRK_COMPLEX] * 0.01f;      /* ROLL and FILL come in with it, full */
        in.anchor = (float)b->set[BRK_ANCHOR] * 0.01f;           /* from 25 % (low settings stay gentle) */
        in.roll = (float)b->set[BRK_ROLL] * 0.01f * ramp;
        in.fill = (float)b->set[BRK_FILL] * 0.01f * ramp;
        in.phrase_bars = pb;
        in.bar_in_phrase = pb ? b->bar % pb : 0;
        slice = brk_select_next(&in, b);
    }
    held = perf_top(&b->perf);
    if (held >= 0)
        slice = held;                                      /* held slice beats the engine */
    b->current_slice = slice;
    b->sub_div = 0;
    if (!forced) {
        static const uint8_t RDIV[4] = {2, 3, 4, 8};
        float inv_tpb = (float)tpt * (1.0f / 96.0f);       /* 1 / triggers per bar */
        for (i = 0; i < 4; i++) {
            float pbar = (float)b->set[BRK_R2 + i] * 0.01f, p;
            if (pbar <= 0.0f)
                continue;
            p = pbar >= 1.0f ? 1.0f : 1.0f - fm_powf(1.0f - pbar, inv_tpb);
            if (brk_rand(b) < p)
                fired[n++] = i;
        }
        if (n)
            b->sub_div = RDIV[fired[brk_rand_u(b) % (uint32_t)n]];
        if (b->perf.stutter)
            b->sub_div = 4;                                /* BB Gen's Stutter 4x pad */
    }
    rb = held >= 0 ? 0 : b->engine_bank;                   /* this part's slice keys are A's */
    if (!b->bank[rb].valid)
        rb ^= 1;
    if (!b->bank[rb].valid)
        rb = b->engine_bank;
    b->render_bank = (uint8_t)rb;
    b->sub_count = 0;
    b->sub_elapsed = 0;
    b->sub_len = b->sub_div ? b->spt / (float)b->sub_div : 0.0f;
    slice_start(b, rb, slice);
    b->st_trigs++;
    b->st_slice = (int8_t)slice;
    b->st_bank = (int8_t)rb;
    b->st_div = (int8_t)b->sub_div;
    b->st_bp = (int8_t)bp;
    b->st_forced = (uint8_t)forced;
}

/* X0X: a step with a slice of its own plays it, from its start, in place of the generator's choice
 * (no retrigger); the generator still counts the trigger the step would have made */
static void pin_trigger(breaks_t *b, int slice)
{
    b->current_slice = slice;
    b->render_bank = 0;
    b->sub_div = 0;
    b->sub_count = 0;
    b->sub_elapsed = 0;
    b->sub_len = 0.0f;
    slice_start(b, 0, slice);
    b->st_trigs++;
    b->st_slice = (int8_t)slice;
    b->st_bank = 0;
    b->st_div = 0;
    b->st_forced = 0;
}

void breaks_pin(breaks_t *b, int slice) { b->pin = (int8_t)(slice >= 0 && slice < 8 ? slice : -1); }

/* a clock trigger: BB Gen counts it whether or not half speed lets it fire */
static void clock_trigger(breaks_t *b)
{
    int bp = b->trigger_count % 8;
    b->trigger_count++;
    if (perf_trigger_fires(&b->perf))
        fire_trigger(b, bp, 0);
}

static void gate_update(breaks_t *b)
{
    int held = b->perf.count > 0;
    b->gate_to = (held || (b->running && b->step_on)) ? 1.0f : 0.0f;
}

/* apply live key changes (keys is written by breaks_live, possibly from another context) */
static void perf_sync(breaks_t *b)
{
    uint16_t k = b->keys, d = (uint16_t)(k ^ b->keys_seen);
    int key;
    if (!d)
        return;
    for (key = 0; key <= 10; key++) {
        uint16_t bit = (uint16_t)(1u << key);
        int down = (k & bit) != 0;
        if (!(d & bit))
            continue;
        if (key < 8) {
            if (down) {
                perf_push(&b->perf, key);
                if (b->bank[0].valid) {                    /* instant hit */
                    b->current_slice = key;
                    b->render_bank = 0;
                    b->sub_div = 0;
                    slice_start(b, 0, key);
                }
            } else
                perf_release(&b->perf, key);
        } else if (key == 8)
            b->perf.reverse = (uint8_t)down;
        else if (key == 9)
            perf_half(&b->perf, down);
        else
            b->perf.stutter = (uint8_t)down;
    }
    b->keys_seen = k;
    gate_update(b);
}

void breaks_live(breaks_t *b, int key, int down)
{
    uint16_t bit;
    if (key < 0 || key > 10)
        return;
    bit = (uint16_t)(1u << key);
    b->keys = (uint16_t)(down ? (b->keys | bit) : (b->keys & ~bit));
}

void breaks_step(breaks_t *b, int step16, int bar, float samples_per_16th, int enabled)
{
    int pb, tpt, tick, forced = 0, pin;
    perf_sync(b);
    b->sp16 = samples_per_16th;
    b->bar = bar;
    b->step_on = (uint8_t)(enabled != 0);
    if (!b->running) {                                     /* transport start (bb_reset_transport) */
        b->running = 1;
        b->trigger_count = 0;
        b->current_slice = 0;
        b->engine_bank = 0;
        b->pending_bank = -1;
        b->perf.trig_acc = 0.0f;
        forced = 1;
    }
    pb = PHRASE_BARS[b->set[BRK_PHRASE] % 5u];
    if (step16 == 0) {                                     /* bar boundary */
        if (!forced && b->pending_bank >= 0) {
            b->engine_bank = (uint8_t)b->pending_bank;
            forced = 1;
        }
        b->pending_bank = -1;
        if (pb) {
            int bip = bar % pb;
            if (bip == pb - 2) {
                if (brk_rand(b) < (float)b->set[BRK_BCHANCE] * 0.01f && b->bank[1].valid)
                    b->pending_bank = 1;
            } else if (bip == pb - 1)
                b->pending_bank = 0;
        } else if (b->set[BRK_BCHANCE] && !forced) {      /* X0X: no phrase: B chance rolls for every bar */
            int bank = brk_rand(b) < (float)b->set[BRK_BCHANCE] * 0.01f && b->bank[1].valid;
            if (bank != b->engine_bank) {
                b->engine_bank = (uint8_t)bank;
                forced = 1;
            }
        } else if (!forced && b->engine_bank) {            /* B CHANCE turned to 0 while B played */
            b->engine_bank = 0;
            forced = 1;
        }
    }
    tpt = bank_ticks(b, b->engine_bank);
    b->spt = samples_per_16th * (float)tpt * (1.0f / 6.0f);
    tick = 6 * (step16 & 15);
    b->trig_left = 0;
    pin = b->pin;
    b->pin = -1;
    if (pin >= 0 && (!enabled || !b->bank[0].valid || b->perf.count))
        pin = -1;                                          /* off, no loop A, or a held slice key wins */
    if (pin >= 0) {
        if (forced)
            b->trigger_count = 1;
        else if (tick % tpt == 0)
            b->trigger_count++;
        pin_trigger(b, pin);
    } else if (forced) {
        b->trigger_count = 1;
        fire_trigger(b, 0, 1);
    } else if (tick % tpt == 0)
        clock_trigger(b);
    if (tpt == 3)                                          /* 1/4 bar: a 32nd-note trigger too */
        b->trig_left = (int32_t)(samples_per_16th * 0.5f + 0.5f) + 1;   /* fires at that sample */
    gate_update(b);
}

void breaks_stop(breaks_t *b)
{
    b->running = 0;
    b->trig_left = 0;
    b->sub_div = 0;
    gate_update(b);
}

void breaks_init(breaks_t *b)
{
    uint8_t *p = (uint8_t *)b;
    uint32_t i;
    for (i = 0; i < sizeof(*b); i++)
        p[i] = 0;
    for (i = 0; i < BRK_NPARAM; i++)
        b->set[i] = BRK_PARAMS[i].def;
    b->rng = 0x2545F491u;
    b->pending_bank = -1;
    b->perf.rate_mult = 1.0f;
    b->st_slice = -1;
    b->pin = -1;
}

/* ---- render ----------------------------------------------------------------------------- */

void breaks_set_quiet(breaks_t *b, int quiet) { b->quiet = (uint8_t)(quiet != 0); }

/* X0X: nothing can sound this block: no voice, the gate shut and staying shut, no trigger or
 * retrigger due inside it. The render would write zeros and change nothing else. */
int breaks_silent(const breaks_t *b)
{
    return !b->v[0].on && !b->v[1].on && b->gate == 0.0f && b->gate_to == 0.0f && b->trig_left == 0 &&
           b->sub_count + 1 >= b->sub_div && b->keys == b->keys_seen;
}

void breaks_render(breaks_t *b, float *out, int n)
{
    float inc[2], lvl, gstep = 1.0f / (float)BRK_FADE;
    int i, j;
    if (b->quiet && b->gate == 0.0f && b->gate_to == 0.0f)
        b->v[0].on = b->v[1].on = 0;                       /* X0X: unheard: stop decoding */
    if (breaks_silent(b)) {                                /* X0X: skip a block that writes zeros */
        for (i = 0; i < n; i++)
            out[i] = 0.0f;
        return;
    }
    perf_sync(b);
    for (j = 0; j < 2; j++) {
        brk_voice_t *v = &b->v[j];
        const brk_bank_t *k = &b->bank[v->bank];
        if (v->on && (!k->valid || k->gen != v->gen))
            v->on = 0;                                     /* its loop was replaced */
        inc[j] = v->on ? brk_inc(b, k) : 0.0f;
    }
    {   /* reverse pressed or released: continue from where the playhead is, the other way */
        brk_voice_t *v = &b->v[b->cur];
        if (v->on && v->rev != b->perf.reverse && v->dg >= 0.0f) {
            int bk = v->bank, sl = 0;
            uint32_t p = v->rev ? v->pos : (v->pos ? v->pos - 1u : 0u);
            while (sl < 7 && b->bank[bk].start[sl + 1] <= v->lo)
                sl++;
            voice_start(b, bk, sl, p, b->perf.reverse, 0, 0, 0);
            inc[b->cur] = brk_inc(b, &b->bank[bk]);
        }
    }
    lvl = (float)b->set[BRK_LEVEL] * (1.0f / 127.0f);
    lvl = lvl * lvl * (1.0f / 32768.0f);
    for (i = 0; i < n; i++) {
        float y = 0.0f;
        if (b->trig_left > 0 && --b->trig_left == 0 && b->running) {
            clock_trigger(b);
            inc[b->cur] = brk_inc(b, &b->bank[b->v[b->cur].bank]);
        }
        if (b->sub_count + 1 < b->sub_div) {                /* retrigger: replay the slice head */
            if ((float)b->sub_elapsed >= (float)(b->sub_count + 1) * b->sub_len) {
                b->sub_count++;
                slice_start(b, b->render_bank, b->current_slice);
                inc[b->cur] = brk_inc(b, &b->bank[b->render_bank]);
            }
            b->sub_elapsed++;
        }
        for (j = 0; j < 2; j++) {
            brk_voice_t *v = &b->v[j];
            const brk_bank_t *k;
            if (!v->on)
                continue;
            k = &b->bank[v->bank];
            y += (v->s0 + (v->s1 - v->s0) * v->frac) * v->g;
            v->g += v->dg;
            if (v->g >= 1.0f) {
                v->g = 1.0f;
                v->dg = 0.0f;
            } else if (v->g <= 0.0f) {
                v->on = 0;
                continue;
            }
            v->frac += inc[j];
            while (v->frac >= 1.0f) {
                v->frac -= 1.0f;
                v->s0 = v->s1;
                if (v->rev) {
                    v->pos = rev_next(v, v->pos);
                    v->s1 = rev_fetch(v, k, v->pos);
                } else {
                    if (++v->pos >= k->n) {                /* BB Gen loops at the end; state 0/0 */
                        v->pos = 0;
                        v->pred = 0;
                        v->idx = 0;
                    }
                    v->s1 = (float)ima_decode(k->adpcm, v->pos, &v->pred, &v->idx);
                }
            }
        }
        if (b->gate < b->gate_to)
            b->gate = fm_minf(b->gate + gstep, b->gate_to);
        else if (b->gate > b->gate_to)
            b->gate = fm_maxf(b->gate - gstep, b->gate_to);
        out[i] = y * b->gate * lvl;
    }
    if (b->gate == 0.0f && b->gate_to == 0.0f && !b->running) {
        b->v[0].on = 0;                                    /* stopped and silent: stop decoding */
        b->v[1].on = 0;
    }
}
