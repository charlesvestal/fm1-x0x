/* SPDX-License-Identifier: GPL-3.0-only */
/* Send reverb, synced delay and master stage: a port of 9W9's er99_verb_tick,
 * er99_dly_tick, master distortion and master_glue (GPL-3.0). See fxbus.h. */
#include "fxbus.h"

static const int32_t fx_cl[4] = { 1116, 1188, 1277, 1356 };
static const int32_t fx_al[2] = { 556, 441 };
static const int32_t fx_alr[2] = { 579, 457 };
#define FX_RV_QUIET (1356 + 8192)          /* combs silent this long: the allpass tail is < -140 dB */
static void fx_room_coeffs(fxbus_t *f);

/* 1/32 1/16T 1/16 1/8T 1/16. 1/8 1/4T 1/8. 1/4 1/2T 1/4. 1/2 1/2. (in beats) */
static const float fx_beats[13] = {
    0.125f, 1.0f / 6.0f, 0.25f, 1.0f / 3.0f, 0.375f, 0.5f, 2.0f / 3.0f,
    0.75f, 1.0f, 4.0f / 3.0f, 1.5f, 2.0f, 3.0f
};

/* ---- parameters ---- */
enum { CV_LIN, CV_EXP, CV_SW };
typedef struct {
    x0x_param_t ui;
    uint8_t curve, exp;
    float lo, hi;
} fx_pspec_t;

static const char *const fx_dist_names[8] = { "Off", "Diode", "Clip", "SAT", "BFZ", "PDIST", "Fold", "Crush" };
static const char *const fx_div_names[13] = {
    "1/32", "1/16T", "1/16", "1/8T", "1/16.", "1/8", "1/4T", "1/8.", "1/4", "1/2T", "1/4.", "1/2", "1/2."
};
static const char *const fx_type_names[2] = { "DIGI", "TAPE" };
static const char *const fx_onoff_names[2] = { "OFF", "ON" };
static const char *const fx_rv_type_names[2] = { "CLASSIC", "ROOM" };

/* pot defaults: the positions 9W9 seeds from its defaults (the engine itself
 * starts on the exact defaults) */
static const fx_pspec_t fx_p[FX_NPARAMS] = {
    [FX_VOLUME]   = { { "Volume", 127, 44, 0 }, CV_LIN, 0, 0.0f, 1.0f },
    [FX_DIST]     = { { "Dist", 7, 0, fx_dist_names }, CV_SW, 0, 0.0f, 0.0f },
    [FX_DRIVE]    = { { "Drive", 127, 41, 0 }, CV_EXP, X0X_EXP_DRIVE, 0.0f, 0.0f },
    [FX_COMP]     = { { "Comp", 127, 0, 0 }, CV_LIN, 0, 0.0f, 1.0f },
    [FX_RV_DECAY] = { { "RvDec", 127, 73, 0 }, CV_LIN, 0, 0.2f, 0.93f },
    [FX_RV_TONE]  = { { "RvTone", 127, 57, 0 }, CV_LIN, 0, 0.0f, 1.0f },
    [FX_RV_HPF]   = { { "RvHPF", 127, 62, 0 }, CV_EXP, X0X_EXP_FX_HPF, 0.0f, 0.0f },
    [FX_RV_LEVEL] = { { "RvLvl", 127, 85, 0 }, CV_LIN, 0, 0.0f, 1.2f },
    [FX_DL_TIME]  = { { "DlTime", 12, 7, fx_div_names }, CV_SW, 0, 0.0f, 0.0f },
    [FX_DL_FDBK]  = { { "DlFdbk", 127, 52, 0 }, CV_LIN, 0, 0.0f, 0.85f },
    [FX_DL_TONE]  = { { "DlTone", 127, 51, 0 }, CV_LIN, 0, 0.0f, 1.0f },
    [FX_DL_LEVEL] = { { "DlLvl", 127, 85, 0 }, CV_LIN, 0, 0.0f, 1.2f },
    [FX_DL_HPF]   = { { "DlHPF", 127, 62, 0 }, CV_EXP, X0X_EXP_FX_HPF, 0.0f, 0.0f },
    [FX_DL_TYPE]  = { { "DlType", 1, 0, fx_type_names }, CV_SW, 0, 0.0f, 0.0f },
    [FX_DL_WEAR]  = { { "Wear", 127, 64, 0 }, CV_LIN, 0, 0.0f, 1.0f },
    [FX_DL_PING]  = { { "Ping", 1, 0, fx_onoff_names }, CV_SW, 0, 0.0f, 0.0f },
    /* X0X: the one-knob TAPE (ui.c R_TAPE), kept with the sound so the knob reads back where it was
     * left. It sets DlType and Wear, which are what the delay plays; it plays nothing itself. */
    [FX_DL_TAPE]  = { { "Tape", 127, 72, 0 }, CV_SW, 0, 0.0f, 0.0f },
    [FX_DL_WIDTH] = { { "Width", 127, 127, 0 }, CV_LIN, 0, 0.0f, 1.0f },   /* X0X: ping-pong's */
    [FX_RV_TYPE]  = { { "RvType", 1, 0, fx_rv_type_names }, CV_SW, 0, 0.0f, 0.0f },   /* X0X */
};

int fxbus_nparams(void) { return FX_NPARAMS; }

const x0x_param_t *fxbus_param(int i)
{
    return i >= 0 && i < FX_NPARAMS ? &fx_p[i].ui : 0;
}

int fxbus_get(const fxbus_t *f, int i)
{
    return i >= 0 && i < FX_NPARAMS ? f->pots[i] : 0;
}

static void fx_retime(fxbus_t *f)
{
    int32_t i = f->dl_div;
    if (i < 0) i = 0;
    if (i > 12) i = 12;
    const float bpm = f->bpm > 20.0f ? f->bpm : 120.0f;
    float ms = fx_beats[i] * 60000.0f / bpm;
    const float max_ms = (float)(f->dlen - 256) / D9_SR * 1000.0f;
    while (ms > max_ms)                 /* longer than the line (1/2. under 90 BPM): half of it, */
        ms *= 0.5f;                     /* which is still on the beat; cutting it short is not */
    f->dl_time_ms = ms;
}

static void fx_comp_prep(fxbus_t *f)
{
    const float a = f->comp;
    const float ratio = 2.0f + 3.0f * a;
    f->c_thr = 10.0f - 18.0f * a;
    f->c_slope = 1.0f - 1.0f / ratio;
    f->c_makeup = a * 2.2f + a * a * 6.6f;
}

/* Set ping-pong's width, w from 0 (every echo centred) to 1. An echo goes to its own side at dl_wa and
 * to the other at dl_wb, with dl_wa^2 + dl_wb^2 = 1 so that it keeps its power as it moves to the
 * centre. At full width they are exactly 1 and 0, so the delay sounds as it did before WIDTH. */
static void fx_dl_width(fxbus_t *f, float w)
{
    if (w >= 1.0f) {
        f->dl_wa = 1.0f;
        f->dl_wb = 0.0f;
    } else {
        w = w < 0.0f ? 0.0f : w;
        f->dl_wa = fm_sqrtf(0.5f + 0.5f * w);
        f->dl_wb = fm_sqrtf(0.5f - 0.5f * w);
    }
}
static void fx_rv_type(fxbus_t *f, int type);

static void fx_apply(fxbus_t *f, int i, float v)
{
    switch (i) {
    case FX_VOLUME: f->volume = v; break;
    case FX_DIST: f->dist_mode = (int32_t)v; break;
    case FX_DRIVE: f->drive = v; break;
    case FX_COMP: f->comp = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); fx_comp_prep(f); break;
    case FX_RV_DECAY: f->rv_decay = v; fx_room_coeffs(f); break;
    case FX_RV_TONE: f->rv_tone = v; fx_room_coeffs(f); break;
    case FX_RV_HPF: d9_biquad_set(&f->rv_hp, D9_HP, v, 0.7071f); break;
    case FX_RV_LEVEL: f->rv_level = v; break;
    case FX_DL_TIME: f->dl_div = (int32_t)v; fx_retime(f); break;
    case FX_DL_FDBK: f->dl_fdbk = v; break;
    case FX_DL_TONE: f->dl_tone = v; break;
    case FX_DL_LEVEL: f->dl_level = v; break;
    case FX_DL_HPF: d9_biquad_set(&f->dl_hp, D9_HP, v, 0.7071f); break;
    case FX_DL_TYPE: f->dl_type = (int32_t)v; break;
    case FX_DL_WEAR: f->wear = v; break;
    case FX_DL_PING: f->dl_ping = (int32_t)v; break;
    case FX_DL_WIDTH: fx_dl_width(f, v); break;
    case FX_RV_TYPE: fx_rv_type(f, (int)v); break;
    default: break;
    }
    if (i == FX_DIST || i == FX_DRIVE)
        d9_shape_prep(&f->shape, f->drive, f->dist_mode - 1);
}

void fxbus_set(fxbus_t *f, int i, int value)
{
    if (i < 0 || i >= FX_NPARAMS)
        return;
    const fx_pspec_t *s = &fx_p[i];
    if (value < 0) value = 0;
    if (value > s->ui.max) value = s->ui.max;
    f->pots[i] = (uint8_t)value;
    float v;
    if (s->curve == CV_EXP)
        v = x0x_pot_exp[s->exp][value];
    else if (s->curve == CV_SW)
        v = (float)value;
    else
        v = s->lo + (s->hi - s->lo) * ((float)value / 127.0f);
    fx_apply(f, i, v);
}

void fxbus_set_bpm(fxbus_t *f, float bpm)
{
    if (bpm > 20.0f && bpm != f->bpm) {
        f->bpm = bpm;
        fx_retime(f);
    }
}

/* Clear the reverb's buffer, whichever type's lines it holds: in words that may alias either (it runs
 * in the audio interrupt when the type changes, so unrolled: about 7k cycles, not 40k). */
static void fx_rv_clear(fx_rv_buf_t *b)
{
    typedef uint32_t D9_MAY_ALIAS fx_word_t;
    fx_word_t *w = (fx_word_t *)(void *)b;
    D9_UNROLL(8)
    for (unsigned i = 0; i < sizeof *b / sizeof *w; ++i)
        w[i] = 0;
}

void fxbus_init(fxbus_t *f, int16_t *dly_buf, int dly_len)
{
    uint8_t *p = (uint8_t *)f;
    for (unsigned i = 0; i < sizeof(*f); ++i)
        p[i] = 0;
    for (int i = 0; i < dly_len; ++i)
        dly_buf[i] = 0;
    f->dbuf = dly_buf;
    f->dlen = dly_len;
    f->rv_type = FX_RV_CLASSIC;

    f->rv_decay = 0.62f; f->rv_tone = 0.45f; f->rv_level = 0.8f;
    d9_biquad_set(&f->rv_hp, D9_HP, 150.0f, 0.7071f);
    f->rv_quiet = FX_RV_QUIET;

    f->dl_div = 7;
    f->bpm = 120.0f;
    f->dl_fdbk = 0.35f; f->dl_tone = 0.4f; f->dl_level = 0.8f;
    fx_retime(f);
    f->dcur = f->dl_time_ms * 0.001f * D9_SR;
    d9_biquad_set(&f->dl_hp, D9_HP, 150.0f, 0.7071f);
    f->dl_quiet = dly_len;
    f->dl_type = 0;
    f->wear = 64.0f / 127.0f;
    fx_dl_width(f, 1.0f);

    f->comp = 0.0f;
    f->drive = 2.0f;
    f->dist_mode = 0;
    f->volume = 0.35f;
    d9_shape_prep(&f->shape, f->drive, -1);
    fx_comp_prep(f);
    f->c_drel = d9_exp_small(-1.0f / (0.035f * D9_SR));
    f->c_atk = d9_exp_small(-1.0f / (0.003f * D9_SR));
    f->c_rel = d9_exp_small(-1.0f / (0.120f * D9_SR));

    for (int i = 0; i < FX_NPARAMS; ++i)
        f->pots[i] = fx_p[i].ui.def;
}

/* ===================================================================== */
/* Reverb (er99_verb_tick), adds its return into out                      */
/* ===================================================================== */
static void fx_reverb(fxbus_t *f, const float *in, float *out, float *out_r, int n)
{
    if (f->rv_quiet >= FX_RV_QUIET) {
        int any = 0;
        for (int i = 0; i < n; ++i)
            any |= in[i] != 0.0f;
        if (!any)
            return;                         /* silent in, silent lines: 0 out */
    }
    const float damp = 0.75f - f->rv_tone * 0.55f;
    const float fb = f->rv_decay;
    const float lvl = f->rv_level;
    float *const ap = f->rv.classic.ap, *const apr = f->rv.classic.apr;
    int16_t *cb[4];
    cb[0] = f->rv.classic.comb;
    cb[1] = cb[0] + fx_cl[0];
    cb[2] = cb[1] + fx_cl[1];
    cb[3] = cb[2] + fx_cl[2];
    float *ab0 = ap, *ab1 = ap + fx_al[0];
    float *ar0 = apr, *ar1 = apr + fx_alr[0];
    int32_t quiet = f->rv_quiet;
    for (int i = 0; i < n; ++i) {
        const float x = d9_biquad_tick(&f->rv_hp, in[i]);
        float acc = 0.0f, acc_r = 0.0f;
        int32_t wr = 0;
        for (int c = 0; c < 4; ++c) {
            int16_t *b = cb[c];
            const int32_t p = f->cpos[c];
            const float y = (float)b[p] * (1.0f / 2048.0f);
            acc += y;
            acc_r += (c & 1) ? -y : y;
            f->cdmp[c] = y + (f->cdmp[c] - y) * damp;
            /* the loop runs at 12 bits, truncated toward zero (no DC) */
            int32_t k = (int32_t)((x + f->cdmp[c] * fb) * 2048.0f);
            if (k > 32767) k = 32767;
            if (k < -32767) k = -32767;
            b[p] = (int16_t)k;
            wr |= k;
            f->cpos[c] = p + 1 >= fx_cl[c] ? 0 : p + 1;
        }
        float y = acc * 0.25f;
        {
            const int32_t p = f->apos[0];
            const float bo = ab0[p];
            ab0[p] = y + bo * 0.5f;
            y = bo - y * 0.5f;
            f->apos[0] = p + 1 >= fx_al[0] ? 0 : p + 1;
        }
        {
            const int32_t p = f->apos[1];
            const float bo = ab1[p];
            ab1[p] = y + bo * 0.5f;
            y = bo - y * 0.5f;
            f->apos[1] = p + 1 >= fx_al[1] ? 0 : p + 1;
        }
        out[i] += y * lvl;
        if (out_r) {                        /* the right: the combs, alternate signs, its own allpasses */
            float yr = acc_r * 0.25f;
            int32_t p = f->aposr[0];
            float bo = ar0[p];
            ar0[p] = yr + bo * 0.5f;
            yr = bo - yr * 0.5f;
            f->aposr[0] = p + 1 >= fx_alr[0] ? 0 : p + 1;
            p = f->aposr[1];
            bo = ar1[p];
            ar1[p] = yr + bo * 0.5f;
            yr = bo - yr * 0.5f;
            f->aposr[1] = p + 1 >= fx_alr[1] ? 0 : p + 1;
            out_r[i] += yr * lvl;
        }
        quiet = (wr == 0 && in[i] == 0.0f) ? quiet + 1 : 0;
    }
    f->rv_quiet = quiet;
}

/* ===================================================================== */
/* Delay (er99_dly_tick), adds its return into out                        */
/* ===================================================================== */
static inline float fx_psin(uint32_t ph)                 /* parabolic sine of a turn */
{
    const float x = (float)(int32_t)ph * (1.0f / 2147483648.0f);
    return 4.0f * x * (1.0f - fm_fabsf(x));
}

/* ===================================================================== */
/* ROOM (X0X): diffusers into an 8-line feedback delay network            */
/* ===================================================================== */
/* The design, its storage format and its measured costs: docs/plans/2026-10-15-room-reverb.md. */

/* The lines' lengths (fxbus.h). Primes, so no two lines have a resonance in common below the sample
 * rate, spread over about an octave (25.5 .. 50.9 ms); the shortest is just above CLASSIC's shortest
 * comb, so the room sounds the same size. All are equal modulo 16, so every line's read starts at
 * the same place in a block (fx_rm_fetch_all).
 *
 * Their order, and the input's signs (fx_rm_net), decide which lines the mix adds and subtracts at
 * each stage, and so how echoes that took different paths of about the same length add up. That sets
 * how regularly the early tail beats on a held partial; this order beats less than the lengths in
 * rising order, and won a blind test against them on toms, 303 and crashes. */
static const int32_t fx_rm_len[FX_RM_N] = { FX_RM_L0, FX_RM_L1, FX_RM_L2, FX_RM_L3,
                                            FX_RM_L4, FX_RM_L5, FX_RM_L6, FX_RM_L7 };
_Static_assert(FX_RM_L0 <= FX_RM_LMAX && FX_RM_L1 <= FX_RM_LMAX && FX_RM_L2 <= FX_RM_LMAX &&
               FX_RM_L3 <= FX_RM_LMAX && FX_RM_L4 <= FX_RM_LMAX && FX_RM_L5 <= FX_RM_LMAX &&
               FX_RM_L7 <= FX_RM_LMAX, "FX_RM_LMAX: the longest line (FX_RM_QUIET)");
/* The input diffusers (fxbus.h): allpasses in series, which fill in the first 100 ms. */
static const int32_t fx_rm_dif_len[4] = { FX_RM_D0, FX_RM_D1, FX_RM_D2, FX_RM_D3 };
static const float fx_rm_dif_gain[4] = { FX_RM_G0, FX_RM_G1, FX_RM_G2, FX_RM_G3 };
#define FX_RM_DSCALE 2048.0f                /* the diffusers' int16 units: 1.0 is 2048 (CLASSIC's combs') */
#define FX_RM_TINY 1e-6f                    /* a block quieter than this (-120 dB) is stored as silence */

static inline fx_room_t *fx_rm(fxbus_t *f) { return &f->rv.room; }

/* Return 2^e as a float (e within the normal range). */
static inline float fx_pow2i(int32_t e)
{
    fm_bits_t b;
    b.u = (uint32_t)(127 + e) << 23;
    return b.f;
}
/* 1.5 2^k. Adding it to a small x leaves x / 2^(k - 23), rounded to nearest, in the sum's low bits
 * (fx_rm_commit's rounding). */
#define FX_MAGIC_BITS(k) (((uint32_t)(127 + (k)) << 23) | 0x400000u)

/* Quantize line j's finished block (stage[j]) into the line's ring, at its write block, as 8-bit
 * block-float. Choose the step, 2^e / 128, so the block's peak is under 127.5 steps; round each
 * sample to the nearest step; store the 16 mantissas and e. Store a block whose peak is below
 * FX_RM_TINY as zeros. Return whether the stored block is nonzero. */
D9_INLINE int fx_rm_commit(fx_room_t *rm, int j)
{
    const float *v = rm->stage[j];
    const int32_t blk = rm->base[j] + rm->wblk[j];
    int8_t *m = rm->man + blk * FX_RM_BLK;

    /* Find the peak: the largest of the samples' bits with the sign cleared (non-negative floats
     * order as their bits do), as four running maxima so the compares don't wait on each other. */
    uint32_t p0 = 0, p1 = 0, p2 = 0, p3 = 0;
    D9_UNROLL_ALL
    for (int k = 0; k < FX_RM_BLK; k += 4) {
        fm_bits_t c0, c1, c2, c3;
        c0.f = v[k]; c1.f = v[k + 1]; c2.f = v[k + 2]; c3.f = v[k + 3];
        c0.u &= 0x7FFFFFFFu; c1.u &= 0x7FFFFFFFu; c2.u &= 0x7FFFFFFFu; c3.u &= 0x7FFFFFFFu;
        p0 = c0.u > p0 ? c0.u : p0;
        p1 = c1.u > p1 ? c1.u : p1;
        p2 = c2.u > p2 ? c2.u : p2;
        p3 = c3.u > p3 ? c3.u : p3;
    }
    p0 = p1 > p0 ? p1 : p0;
    p2 = p3 > p2 ? p3 : p2;
    const uint32_t peak = p2 > p0 ? p2 : p0;

    const fm_bits_t tiny = { .f = FX_RM_TINY };
    if (D9_UNLIKELY(peak < tiny.u)) {         /* (rare: keep the usual case's path straight) */
        for (int k = 0; k < FX_RM_BLK; ++k)
            m[k] = 0;
        rm->exp[blk] = 0;
        return 0;
    }
    /* Take e from the peak's exponent field: 2^(e-1) <= peak < 2^e puts the peak at 64 .. 128 steps.
     * A peak of 127.5 steps or more would round to 128, so it takes the next e: that is a mantissa
     * field of 0x7F0000 or more (127.5 / 64 = 1 + 0x7F0000 / 2^23), which the added 2^16 carries into
     * the exponent field. */
    const int32_t e = (int32_t)((peak + (1u << 16)) >> 23) - 126;
    /* Round to the nearest step: the last mantissa bit of 1.5 2^(e+16) is worth one step, so the
     * low byte of v[k] + magic is the rounded mantissa. */
    const fm_bits_t magic = { .u = FX_MAGIC_BITS(e + 16) };
    D9_UNROLL_ALL
    for (int k = 0; k < FX_RM_BLK; ++k) {
        fm_bits_t q;
        q.f = v[k] + magic.f;
        m[k] = (int8_t)(uint8_t)q.u;
    }
    rm->exp[blk] = (int8_t)e;
    return 1;
}

/* Decode samples [from, to) of block blk into dst (from < to), each mantissa times the block's step
 * times c (the line's damper input gain, folded into the step once a block rather than applied per
 * sample). from and to are constants wherever this is used, so the loop unrolls completely. Each
 * mantissa is loaded a sample ahead of its use, which lets the compiler pair each conversion with
 * the store before it (fewer cycles on the device; the same values). */
D9_INLINE void fx_rm_unpack(const fx_room_t *rm, int32_t blk, int32_t from, int32_t to, float c, float *dst)
{
    const int8_t *m = rm->man + blk * FX_RM_BLK;
    const float step = fx_pow2i(rm->exp[blk] - 7) * c;           /* 2^e / 128, times c */
    int32_t mk = m[from];
    D9_UNROLL_ALL
    for (int32_t k = from; k < to - 1; ++k) {
        const int32_t next = m[k + 1];
        *dst++ = (float)mk * step;
        mk = next;
    }
    *dst = (float)mk * step;
}

/* Decode line j's next 16 reads into read[j]. A line writes one sample and reads one each sample, the
 * read L behind the write. When a block of writes begins, with L = 16 (nblk - 1) + 16 - off, the read
 * is at offset off of the block about to be written: that block still holds the samples written
 * nblk blocks before, and must be read before its commit. The 16 reads take the rest of it and the
 * start of the next block, the oldest. */
D9_INLINE void fx_rm_fetch(fx_room_t *rm, int j, int32_t off)
{
    const int32_t nblk = rm->nblk[j];
    int32_t b = rm->wblk[j];
    fx_rm_unpack(rm, rm->base[j] + b, off, FX_RM_BLK, rm->c[j], rm->read[j]);
    if (++b == nblk)
        b = 0;
    fx_rm_unpack(rm, rm->base[j] + b, 0, off, rm->c[j], rm->read[j] + FX_RM_BLK - off);
}

/* Fetch the next 16 reads of all eight lines. Every length is the same modulo 16 (checked here), so
 * off is one constant for all of them: the two decodes are straight code, and the loop over the lines
 * is kept rolled so that one copy of that code serves all eight. */
#define FX_RM_OFF(l) ((FX_RM_BLK - (l) % FX_RM_BLK) % FX_RM_BLK)
_Static_assert(FX_RM_OFF(FX_RM_L1) == FX_RM_OFF(FX_RM_L0) && FX_RM_OFF(FX_RM_L2) == FX_RM_OFF(FX_RM_L0) &&
               FX_RM_OFF(FX_RM_L3) == FX_RM_OFF(FX_RM_L0) && FX_RM_OFF(FX_RM_L4) == FX_RM_OFF(FX_RM_L0) &&
               FX_RM_OFF(FX_RM_L5) == FX_RM_OFF(FX_RM_L0) && FX_RM_OFF(FX_RM_L6) == FX_RM_OFF(FX_RM_L0) &&
               FX_RM_OFF(FX_RM_L7) == FX_RM_OFF(FX_RM_L0), "ROOM's lengths: all the same modulo 16");
_Static_assert(FX_RM_OFF(FX_RM_L0) != 0,
               "ROOM's lengths: not a multiple of 16 (fx_rm_unpack needs from < to)");
D9_NOINLINE void fx_rm_fetch_all(fx_room_t *rm)
{
    D9_NO_UNROLL
    for (int j = 0; j < FX_RM_N; ++j)
        fx_rm_fetch(rm, j, FX_RM_OFF(FX_RM_L0));
}

/* Commit all eight lines' finished blocks, and move each line's write on a block. Return whether any
 * block was nonzero. */
D9_NOINLINE int fx_rm_commit_all(fx_room_t *rm)
{
    int heard = 0;
    for (int j = 0; j < FX_RM_N; ++j) {
        heard |= fx_rm_commit(rm, j);
        if (++rm->wblk[j] == rm->nblk[j])
            rm->wblk[j] = 0;
    }
    return heard;
}

/* Set each line's loop gain and damper for the decay times t_dc (at DC) and t_ny (at Nyquist), in
 * seconds, and the outputs' filter.
 *
 * A line of length L has loop gain g = 10^(-3 L / t_dc), so every line decays at t_dc at DC. Its
 * damper (one-pole) then sets one more frequency. A reference line of the lines' mean length has its
 * damper set by t_ny at Nyquist; each line's damper matches the reference's decay at 3 kHz instead
 * (its response there relative to DC: the reference's to the power L / L_ref). Matched at Nyquist,
 * lines of different lengths drift apart in between when t_dc / t_ny is large (by up to 2.3x at the
 * longest, darkest setting), and the network's modes on the slowest lines ring on; matched at 3 kHz,
 * they keep within 1.15x below 5 kHz. */
#define FX_RM_LREF \
    ((float)(FX_RM_L0 + FX_RM_L1 + FX_RM_L2 + FX_RM_L3 + FX_RM_L4 + FX_RM_L5 + FX_RM_L6 + FX_RM_L7) / FX_RM_N)
#define FX_RM_COS_M 0.9100351f                                         /* cos(2 pi 3000 / 44100) */
static void fx_room_times(fx_room_t *rm, float t_dc, float t_ny)
{
    /* the reference's damper, and its response at 3 kHz relative to DC, squared, as log2 */
    const float sref = FX_RM_LREF * (1.0f / D9_SR);
    const float gn = fm_minf(1.0f, fm_exp2f(-9.9657843f * sref / t_ny               /* 3 log2(10) */
                                            + 9.9657843f * sref / t_dc));
    const float ar = (1.0f - gn) / (1.0f + gn);
    const float lq2 = fm_log2f((1.0f - ar) * (1.0f - ar) / (1.0f - 2.0f * ar * FX_RM_COS_M + ar * ar));
    float g_sum = 0.0f, a_sum = 0.0f;
    for (int j = 0; j < FX_RM_N; ++j) {
        const float sec = (float)fx_rm_len[j] * (1.0f / D9_SR);
        const float g = fm_exp2f(-9.9657843f * sec / t_dc);
        /* the pole a whose response at 3 kHz relative to DC, squared, is r: (1 - a)^2 = r (1 - 2 a cos + a^2),
         * the smaller root, in the form that stays exact as r -> 1 (no damping: a = 0) */
        const float r = fm_exp2f(lq2 * (float)fx_rm_len[j] * (1.0f / FX_RM_LREF));
        const float qa = 1.0f - r, qb = 1.0f - r * FX_RM_COS_M;
        rm->a[j] = qa / (qb + fm_sqrtf(qb * qb - qa * qa));
        rm->c[j] = g * 0.35355339f * (1.0f - rm->a[j]);              /* with the mix's 1/sqrt(8) */
        g_sum += g;
        a_sum += rm->a[j];
    }
    /* The outputs are sums of the damped lines (fx_rm_net), so they carry the dampers and the loop
     * gain. Undo the lines' mean damper on them (y = s - tap_a s[-1]), and their mean loop gain and
     * the mix's 1/sqrt(8) (tap_k), so that they sound like the lines themselves (within a fraction of
     * a dB in every band). */
    rm->tap_a = a_sum * 0.125f;
    rm->tap_k = 22.627417f / (g_sum * (1.0f - rm->tap_a));          /* sqrt 8 / (mean g (1 - tap_a)) */
}

/* Set ROOM's decay and output gain from RvDec and RvTone. The knobs map to the decay times t_dc and
 * t_ny, and to the output gain out_gain, by polynomials fitted to CLASSIC's decay as heard and to its
 * loudness over the knobs' range (tools/fit_room.py, which prints them; the design doc has the
 * method). Do nothing unless ROOM is selected: otherwise the buffer is CLASSIC's. */
static void fx_room_coeffs(fxbus_t *f)
{
    if (f->rv_type != FX_RV_ROOM)
        return;
    const float x = (float)f->pots[FX_RV_DECAY] * (1.0f / 127.0f);
    const float y = (float)f->pots[FX_RV_TONE] * (1.0f / 127.0f);
    const float t_dc = fm_expf(-1.7984f + x * (1.1228f + x * (-0.6857f + x * 2.1048f)) + 0.0597f * y);
    const float t_ny = fm_minf(t_dc, fm_expf(-2.0520f - 0.6055f * x - 0.1850f * y + 1.2173f * x * y
                                             + 0.2419f * y * y + 0.5170f * x * x));
    fx_room_t *const rm = fx_rm(f);
    fx_room_times(rm, t_dc, t_ny);
    rm->out_gain = 1.8963f + 0.3712f * x + 0.1889f * y;
}

/* Set ROOM's state as at power-on, in a cleared buffer: lay out the rings, and start each line's read
 * L samples behind its write. */
static void fx_room_reset(fxbus_t *f)
{
    fx_room_t *rm = fx_rm(f);
    int32_t base = 0;
    for (int j = 0; j < FX_RM_N; ++j) {
        const int32_t len = fx_rm_len[j];
        rm->base[j] = (uint16_t)base;
        rm->nblk[j] = (uint16_t)FX_RM_NB(len);
        rm->wblk[j] = 0;
        base += FX_RM_NB(len);
    }
    rm->pos = 0;
    rm->quiet = FX_RM_QUIET;
}

/* Run the dampers of lines j .. j + 3 over samples [pos, pos + m) of the block, then the first two
 * stages of the mix over their outputs, into stage[j .. j + 3].
 *
 * Each damper is a one-pole lowpass, z = a z + in. Its input gain (the loop gain, the mix's 1/sqrt(8)
 * and 1 - a) is already in the reads (fx_rm_unpack's c), so a multiply and an add remain per sample.
 *
 * The mix is an 8-point Walsh-Hadamard transform: three stages of sums and differences. The first two
 * combine lines only within a group of four, so they are done here, while the four dampers' outputs
 * are in registers; fx_rm_net does the third. The lines go in two groups of four because all eight in
 * one loop run out of registers. */
D9_INLINE void fx_rm_damp4(fx_room_t *rm, int j, int32_t pos, int m)
{
    const float *r0 = &rm->read[j][pos], *r1 = &rm->read[j + 1][pos], *r2 = &rm->read[j + 2][pos],
                *r3 = &rm->read[j + 3][pos];
    float *s0 = &rm->stage[j][pos], *s1 = &rm->stage[j + 1][pos], *s2 = &rm->stage[j + 2][pos],
          *s3 = &rm->stage[j + 3][pos];
    const float a0 = rm->a[j], a1 = rm->a[j + 1], a2 = rm->a[j + 2], a3 = rm->a[j + 3];
    float z0 = rm->lp[j], z1 = rm->lp[j + 1], z2 = rm->lp[j + 2], z3 = rm->lp[j + 3];
    for (int t = 0; t < m; ++t) {
        z0 = a0 * z0 + r0[t];
        z1 = a1 * z1 + r1[t];
        z2 = a2 * z2 + r2[t];
        z3 = a3 * z3 + r3[t];
        const float p0 = z0 + z1, p1 = z0 - z1, p2 = z2 + z3, p3 = z2 - z3;
        s0[t] = p0 + p2;
        s1[t] = p1 + p3;
        s2[t] = p0 - p2;
        s3[t] = p1 - p3;
    }
    rm->lp[j] = z0; rm->lp[j + 1] = z1; rm->lp[j + 2] = z2; rm->lp[j + 3] = z3;
}

/* Run the network over samples [pos, pos + m) of the block: the dampers and the mix, with the input
 * xin added to each line at its sign, into stage; and add the outputs, times lvl, into out and out_r.
 * Nothing here reads what it writes: a line's reads were written at least 16 samples before. */
D9_INLINE void fx_rm_net(fx_room_t *rm, const float *xin, int32_t pos, int m, float *out, float *out_r,
                         float lvl)
{
    const float tap_a = rm->tap_a;
    float zl = rm->tap_zl, zr = rm->tap_zr;
    fx_rm_damp4(rm, 0, pos, m);
    fx_rm_damp4(rm, 4, pos, m);
    for (int t = 0; t < m; ++t) {
        /* sample t of the eight lines, one block apart in stage */
        float *col = &rm->stage[0][pos + t];
        const float b0 = col[0], b1 = col[FX_RM_BLK], b2 = col[2 * FX_RM_BLK], b3 = col[3 * FX_RM_BLK];
        const float b4 = col[4 * FX_RM_BLK], b5 = col[5 * FX_RM_BLK];
        const float b6 = col[6 * FX_RM_BLK], b7 = col[7 * FX_RM_BLK];

        /* Do the mix's last stage, and add the input at each line's sign (+ + - + + - - +). */
        const float x = xin[t] * (0.35355339f / FX_RM_DSCALE);
        col[0] = (b0 + b4) + x;
        col[FX_RM_BLK] = (b1 + b5) + x;
        col[2 * FX_RM_BLK] = (b2 + b6) - x;
        col[3 * FX_RM_BLK] = (b3 + b7) + x;
        col[4 * FX_RM_BLK] = (b0 - b4) + x;
        col[5 * FX_RM_BLK] = (b1 - b5) - x;
        col[6 * FX_RM_BLK] = (b2 - b6) - x;
        col[7 * FX_RM_BLK] = (b3 - b7) + x;

        /* Add the outputs: two of the mix's rows, before the input is added. L is row 1
         * (+ - + - + - + -), b1 + b5; R is row 6 (+ + - - - - + +), b2 - b6. Each has the lines' mean
         * damper undone (fx_room_times). */
        const float sl = b1 + b5, sr = b2 - b6;
        out[t] += (sl - tap_a * zl) * lvl;
        out_r[t] += (sr - tap_a * zr) * lvl;
        zl = sl;
        zr = sr;
    }
    rm->tap_zl = zl;
    rm->tap_zr = zr;
}

/* Run fx_rm_net over a whole block, or over part of one. Each is its own function so that the compiler
 * keeps more of it in registers; the whole-block one has its counts as constants. */
D9_NOINLINE void fx_rm_net_blk(fx_room_t *rm, const float *xin, float *out, float *out_r, float lvl)
{
    fx_rm_net(rm, xin, 0, FX_RM_BLK, out, out_r, lvl);
}
D9_NOINLINE void fx_rm_net_part(fx_room_t *rm, const float *xin, int32_t pos, int m, float *out, float *out_r,
                                float lvl)
{
    fx_rm_net(rm, xin, pos, m, out, out_r, lvl);
}

/* Run one diffuser, an allpass, on one sample x, and return its output:
 * bo = buf[p]; v = x + g bo; buf[p] = v; return bo - g v.
 * buf is int16, in the diffusers' units; *pp is the place p in buf[0 .. len), moved on by one. */
D9_INLINE float fx_rm_ap(int16_t **pp, int16_t *buf, int32_t len, float g, float x)
{
    int16_t *const p = *pp;
    const float bo = (float)*p;
    const float v = x + g * bo;
    int32_t q = (int32_t)v;                                     /* toward zero: nothing small circulates */
    q = q < -32767 ? -32767 : q;
    q = q > 32767 ? 32767 : q;
    *p = (int16_t)q;
    *pp = p + 1 == buf + len ? buf : p + 1;                     /* wrap by a select, not a branch */
    return bo - g * v;
}

/* Add ROOM's return for in[0 .. n) into out and out_r (ROOM is always stereo: fxbus_process gives it a
 * right side to discard, so that the network has no test per sample). Work in chunks that end at the
 * lines' block boundaries, each phase over a whole chunk before the next (fewer values live at once:
 * the FPU's registers are the integer ones):
 *   - at the start of a block, fetch the lines' next 16 reads;
 *   - high-pass and diffuse the input;
 *   - run the network;
 *   - when the block is full, commit it to the rings.
 * The high-pass and the diffusers are written out here rather than as inline helpers: as helpers, the
 * same code compiled (device clang) to 5 to 17 more instructions here, with more spills. */
static void fx_room(fxbus_t *f, const float *in, float *out, float *out_r, int n)
{
    fx_room_t *const rm = fx_rm(f);
    if (rm->quiet >= FX_RM_QUIET) {
        int any = 0;
        for (int i = 0; i < n; ++i)
            any |= in[i] != 0.0f;
        if (!any)
            return;                         /* silent in, silent lines: 0 out */
    }
    const float lvl = f->rv_level * rm->out_gain * 0.35355339f * rm->tap_k;   /* 1/sqrt(8): the taps' sum */
    int32_t pos = rm->pos, quiet = rm->quiet;

    for (int i0 = 0; i0 < n;) {
        const int m = n - i0 < FX_RM_BLK - pos ? n - i0 : FX_RM_BLK - pos;
        float xin[FX_RM_BLK];

        if (pos == 0)
            fx_rm_fetch_all(rm);

        /* High-pass the input into xin, in the diffusers' units (d9_biquad_tick, with its state held
         * in registers). */
        {
            d9_biquad_t *h = &f->rv_hp;
            const float b0 = h->b0 * FX_RM_DSCALE, b1 = h->b1 * FX_RM_DSCALE, b2 = h->b2 * FX_RM_DSCALE;
            const float a1 = h->a1, a2 = h->a2;
            float x1 = h->x1, x2 = h->x2, y1 = h->y1 * FX_RM_DSCALE, y2 = h->y2 * FX_RM_DSCALE;
            D9_UNROLL(4)
            for (int t = 0; t < m; ++t) {
                const float x = in[i0 + t], y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
                x2 = x1; x1 = x;
                y2 = y1; y1 = y;
                xin[t] = y;
            }
            h->x1 = x1; h->x2 = x2; h->y1 = y1 * (1.0f / FX_RM_DSCALE); h->y2 = y2 * (1.0f / FX_RM_DSCALE);
        }

        /* Run the four input diffusers in series, in place. All four run on a sample before the next
         * sample, so that it stays in a register between them. */
        {
            int16_t *const d0 = rm->dif, *const d1 = d0 + fx_rm_dif_len[0], *const d2 = d1 + fx_rm_dif_len[1],
                    *const d3 = d2 + fx_rm_dif_len[2];
            int16_t *p0 = d0 + rm->dif_pos[0], *p1 = d1 + rm->dif_pos[1], *p2 = d2 + rm->dif_pos[2],
                    *p3 = d3 + rm->dif_pos[3];
            for (int t = 0; t < m; ++t) {
                float x = xin[t];
                x = fx_rm_ap(&p0, d0, fx_rm_dif_len[0], fx_rm_dif_gain[0], x);
                x = fx_rm_ap(&p1, d1, fx_rm_dif_len[1], fx_rm_dif_gain[1], x);
                x = fx_rm_ap(&p2, d2, fx_rm_dif_len[2], fx_rm_dif_gain[2], x);
                xin[t] = fx_rm_ap(&p3, d3, fx_rm_dif_len[3], fx_rm_dif_gain[3], x);
            }
            rm->dif_pos[0] = (int16_t)(p0 - d0); rm->dif_pos[1] = (int16_t)(p1 - d1);
            rm->dif_pos[2] = (int16_t)(p2 - d2); rm->dif_pos[3] = (int16_t)(p3 - d3);
        }

        if (pos == 0 && m == FX_RM_BLK)
            fx_rm_net_blk(rm, xin, out + i0, out_r + i0, lvl);
        else
            fx_rm_net_part(rm, xin, pos, m, out + i0, out_r + i0, lvl);
        pos += m;
        i0 += m;

        if (pos == FX_RM_BLK) {
            pos = 0;
            quiet = fx_rm_commit_all(rm) ? 0 : quiet + 1;   /* (the input is in the blocks: fx_rm_net) */
            if (quiet == FX_RM_QUIET) {     /* idle from here: clear the dampers, taps and diffusers too */
                for (int j = 0; j < FX_RM_N; ++j)
                    rm->lp[j] = 0.0f;
                rm->tap_zl = rm->tap_zr = 0.0f;
                for (int k = 0; k < FX_RM_DIF_TOTAL; ++k)
                    rm->dif[k] = 0;
            }
        }
    }
    rm->pos = (uint8_t)pos;
    rm->quiet = quiet;
}

/* Set the reverb's type (FX_RV_TYPE): the two share the buffer, so a change clears it, cutting the tail. */
static void fx_rv_type(fxbus_t *f, int type)
{
    type = type ? FX_RV_ROOM : FX_RV_CLASSIC;
    if (type == f->rv_type)
        return;
    f->rv_type = type;
    fx_rv_clear(&f->rv);
    for (int c = 0; c < 4; ++c) {
        f->cpos[c] = 0;
        f->cdmp[c] = 0.0f;
    }
    f->apos[0] = f->apos[1] = f->aposr[0] = f->aposr[1] = 0;
    f->rv_quiet = FX_RV_QUIET;
    if (type == FX_RV_ROOM) {
        fx_room_reset(f);
        fx_room_coeffs(f);
    }
}

/* the line at d samples behind w, interpolated (X0X: the ping-pong tap) */
static inline float dl_tap(const int16_t *buf, int32_t len, float flen, int32_t w, float d)
{
    float rp = (float)w - d;
    while (rp < 0.0f) rp += flen;
    int32_t i0 = (int32_t)rp;
    const float fr = rp - (float)i0;
    if (i0 >= len) i0 -= len;
    const int32_t i1 = i0 + 1 >= len ? 0 : i0 + 1;
    const float b0 = (float)buf[i0] * (1.0f / 2048.0f), b1 = (float)buf[i1] * (1.0f / 2048.0f);
    return b0 + (b1 - b0) * fr;
}

/* out_r 0: 9W9's mono delay. Else both sides get the echo, or with PING the left reads at the time and
 * the right at twice it, the loop fed from the right: echoes alternate L R L R, one time apart, as far
 * apart as the width puts them (fx_dl_width). The line holds both, so PING needs twice the time
 * to fit (a 2 s line: 1 s); past that it plays centred. */
static void fx_delay(fxbus_t *f, const float *in, float *out, float *out_r, int n)
{
    const float target = f->dl_time_ms * 0.001f * D9_SR;
    const int32_t len = f->dlen;
    const int tape = f->dl_type == 1;
    if (f->dl_quiet >= len) {
        int any = 0;
        for (int i = 0; i < n; ++i)
            any |= in[i] != 0.0f;
        if (!any) {
            /* the line is all zeros: nothing to read, but time moves on */
            f->w = (f->w + n) % len;
            /* the time slew, sample by sample as 9W9 runs it: in float it stalls
             * ~1 sample short of the target (the step rounds away), and that
             * stalled value is where 9W9's echoes land. It stops at once
             * when it has stalled, so an idle line costs nothing. */
            {
                float dc = f->dcur;
                for (int i = 0; i < n; ++i) {
                    const float nd = dc + (target - dc) * 0.0008f;
                    if (nd == dc)
                        break;
                    dc = nd;
                }
                f->dcur = dc;
            }
            f->wow_ph += d9_inc(0.5f) * (uint32_t)n;
            f->flut_ph += d9_inc(6.1f) * (uint32_t)n;
            return;
        }
    }
    int16_t *buf = f->dbuf;
    const float flen = (float)len;
    const int ping = out_r && f->dl_ping && 2.0f * fm_maxf(f->dcur, target) + 4.0f < flen;
    const float lvl = f->dl_level;
    /* each side's share of its own tap (y on the left, y2 on the right), and with PING narrowed, of the
     * other's: without PING, or at full width, the delay plays as it did before the width */
    const float la = ping ? lvl * f->dl_wa : lvl, lb = ping ? lvl * f->dl_wb : 0.0f;
    const int cross = lb != 0.0f;
    int32_t quiet = f->dl_quiet;
    int32_t w = f->w;
    float dcur = f->dcur, lp = f->lp;

    if (!tape) {
        const float fdbk = f->dl_fdbk;
        const float tc = 0.06f + f->dl_tone * 0.6f;
        for (int i = 0; i < n; ++i) {
            const float x = d9_biquad_tick(&f->dl_hp, in[i]);
            dcur += (target - dcur) * 0.0008f;
            float rp = (float)w - dcur;
            while (rp < 0.0f) rp += flen;
            int32_t i0 = (int32_t)rp;
            const float fr = rp - (float)i0;
            if (i0 >= len) i0 -= len;
            const int32_t i1 = i0 + 1 >= len ? 0 : i0 + 1;
            const float b0 = (float)buf[i0] * (1.0f / 2048.0f), b1 = (float)buf[i1] * (1.0f / 2048.0f);
            const float y = b0 + (b1 - b0) * fr;
            const float y2 = ping ? dl_tap(buf, len, flen, w, 2.0f * dcur) : y;
            lp += (y2 * fdbk - lp) * tc;
            if (fm_fabsf(lp) < 1e-20f) lp = 0.0f;
            int32_t k = (int32_t)((x + lp) * 2048.0f);       /* 12-bit, toward zero */
            if (k > 32767) k = 32767;
            if (k < -32767) k = -32767;
            buf[w] = (int16_t)k;
            if (++w >= len) w = 0;
            out[i] += y * la;
            if (out_r)
                out_r[i] += y2 * la;
            if (cross) {
                out[i] += y2 * lb;
                out_r[i] += y * lb;
            }
            quiet = (k == 0 && in[i] == 0.0f) ? quiet + 1 : 0;
        }
    } else {
        /* TAPE: wow (0.5 Hz) and flutter (6.1 Hz) on the read, a darker loop,
         * saturation and a 30 Hz DC block inside it, feedback up to 1.15 */
        const float wear = f->wear;
        const float depth = 0.1f + 1.9f * wear;                  /* x nominal; 1 at wear ~0.5 */
        const float wow_a = 1.5f * D9_MS * depth, flut_a = 0.15f * D9_MS * depth;
        const uint32_t wow_i = d9_inc(0.5f), flut_i = d9_inc(6.1f);
        const float fdbk = f->dl_fdbk * (1.15f / 0.85f);
        const float tc = (0.06f + f->dl_tone * 0.6f) * (0.6f - 0.3f * wear);
        const float g = 1.0f + 1.5f * wear, inv_g = 1.0f / g;
        uint32_t wp = f->wow_ph, fp = f->flut_ph;
        float hp = f->tape_hp;
        for (int i = 0; i < n; ++i) {
            const float x = d9_biquad_tick(&f->dl_hp, in[i]);
            dcur += (target - dcur) * 0.0008f;
            wp += wow_i;
            fp += flut_i;
            const float dmod = dcur + wow_a * fx_psin(wp) + flut_a * fx_psin(fp);
            float rp = (float)w - dmod;
            while (rp < 0.0f) rp += flen;
            int32_t i0 = (int32_t)rp;
            const float fr = rp - (float)i0;
            if (i0 >= len) i0 -= len;
            const int32_t i1 = i0 + 1 >= len ? 0 : i0 + 1;
            const float b0 = (float)buf[i0] * (1.0f / 2048.0f), b1 = (float)buf[i1] * (1.0f / 2048.0f);
            const float y = b0 + (b1 - b0) * fr;
            const float y2 = ping ? dl_tap(buf, len, flen, w, 2.0f * dmod) : y;
            lp += (y2 * fdbk - lp) * tc;
            if (fm_fabsf(lp) < 1e-20f) lp = 0.0f;
            float s = d9_tanh(lp * g) * inv_g;                   /* tape saturation holds the loop */
            hp += (s - hp) * 0.0042725f;                          /* 30 Hz: no DC latch-up past unity */
            s -= hp;
            int32_t k = (int32_t)((x + s) * 2048.0f);
            if (k > 32767) k = 32767;
            if (k < -32767) k = -32767;
            buf[w] = (int16_t)k;
            if (++w >= len) w = 0;
            out[i] += y * la;
            if (out_r)
                out_r[i] += y2 * la;
            if (cross) {
                out[i] += y2 * lb;
                out_r[i] += y * lb;
            }
            quiet = (k == 0 && in[i] == 0.0f) ? quiet + 1 : 0;
        }
        f->wow_ph = wp;
        f->flut_ph = fp;
        f->tape_hp = fm_flush(hp);
    }
    f->w = w;
    f->dcur = dcur;
    f->lp = lp;
    f->dl_quiet = quiet;
}

/* ===================================================================== */
/* Master: distortion, glue (master_glue), volume                         */
/* ===================================================================== */
void fxbus_process(fxbus_t *f, const float *dry, const float *rev, const float *dly, float *out, int n)
{
    if (n > 256)
        n = 256;
    for (int i = 0; i < n; ++i)
        out[i] = dry[i];
    if (f->rv_type == FX_RV_ROOM) {
        float discard[256];                       /* ROOM's right side, unused in mono */
        for (int i = 0; i < n; ++i)
            discard[i] = 0.0f;
        fx_room(f, rev, out, discard, n);
    } else {
        fx_reverb(f, rev, out, 0, n);
    }
    fx_delay(f, dly, out, 0, n);

    if (f->dist_mode >= 1) {
        const float k = f->drive;
        for (int i = 0; i < n; ++i)
            out[i] = d9_shape(&f->shape, out[i] * k, f->crush_st) * 0.7f;
    }
    if (f->comp > 0.001f) {
        const float thr = f->c_thr, slope = f->c_slope, makeup = f->c_makeup;
        const float drel = f->c_drel, atk = f->c_atk, rel = f->c_rel;
        float det = f->comp_det, env = f->comp_env_db;
        for (int i = 0; i < n; ++i) {
            const float in = out[i];
            const float mag = fm_fabsf(in);
            det = mag > det ? mag : det * drel;
            const float in_db = det > 1e-9f ? 6.0205999f * fm_log2f(det) : -120.0f;
            const float over = in_db - thr;
            float gr = 0.0f;
            if (over >= 3.0f)
                gr = -over * slope;
            else if (over > -3.0f) {
                const float t = over + 3.0f;
                gr = -(t * t) * (1.0f / 12.0f) * slope;
            }
            env = gr + (env - gr) * (gr < env ? atk : rel);
            out[i] = in * fm_db2lin(env + makeup);
        }
        f->comp_det = det;
        f->comp_env_db = env;
    }
    const float vol = f->volume;
    for (int i = 0; i < n; ++i)
        out[i] *= vol;
}

/* X0X stereo: 9W9's chain on L and R. The distortion runs on each side; the glue compressor reads
 * the louder side and moves both together, so the image holds */
void fxbus_process_st(fxbus_t *f, const float *dry_l, const float *dry_r, const float *rev, const float *dly,
                      float *out_l, float *out_r, int n)
{
    if (n > 256)
        n = 256;
    for (int i = 0; i < n; ++i) {
        out_l[i] = dry_l[i];
        out_r[i] = dry_r[i];
    }
    if (f->rv_type == FX_RV_ROOM)
        fx_room(f, rev, out_l, out_r, n);
    else
        fx_reverb(f, rev, out_l, out_r, n);
    fx_delay(f, dly, out_l, out_r, n);
    if (f->dist_mode >= 1) {
        const float k = f->drive;
        for (int i = 0; i < n; ++i) {
            out_l[i] = d9_shape(&f->shape, out_l[i] * k, f->crush_st) * 0.7f;
            out_r[i] = d9_shape(&f->shape, out_r[i] * k, f->crush_str) * 0.7f;
        }
    }
    if (f->comp > 0.001f) {
        const float thr = f->c_thr, slope = f->c_slope, makeup = f->c_makeup;
        const float drel = f->c_drel, atk = f->c_atk, rel = f->c_rel;
        float det = f->comp_det, env = f->comp_env_db;
        for (int i = 0; i < n; ++i) {
            const float ml = fm_fabsf(out_l[i]), mr = fm_fabsf(out_r[i]);
            const float mag = ml > mr ? ml : mr;
            det = mag > det ? mag : det * drel;
            const float in_db = det > 1e-9f ? 6.0205999f * fm_log2f(det) : -120.0f;
            const float over = in_db - thr;
            float gr = 0.0f;
            if (over >= 3.0f)
                gr = -over * slope;
            else if (over > -3.0f) {
                const float t = over + 3.0f;
                gr = -(t * t) * (1.0f / 12.0f) * slope;
            }
            env = gr + (env - gr) * (gr < env ? atk : rel);
            const float g = fm_db2lin(env + makeup);
            out_l[i] *= g;
            out_r[i] *= g;
        }
        f->comp_det = det;
        f->comp_env_db = env;
    }
    const float vol = f->volume;
    for (int i = 0; i < n; ++i) {
        out_l[i] *= vol;
        out_r[i] *= vol;
    }
}
