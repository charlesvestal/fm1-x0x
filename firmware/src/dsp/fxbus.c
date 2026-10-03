/* SPDX-License-Identifier: GPL-3.0-only */
/* Send reverb, synced delay and master stage: a port of 9W9's er99_verb_tick,
 * er99_dly_tick, master distortion and master_glue (GPL-3.0). See fxbus.h. */
#include "fxbus.h"

static const int32_t fx_cl[4] = { 1116, 1188, 1277, 1356 };
static const int32_t fx_al[2] = { 556, 441 };
#define FX_RV_QUIET (1356 + 8192)          /* combs silent this long: the allpass tail is < -140 dB */

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
    if (ms > max_ms) ms = max_ms;
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

static void fx_apply(fxbus_t *f, int i, float v)
{
    switch (i) {
    case FX_VOLUME: f->volume = v; break;
    case FX_DIST: f->dist_mode = (int32_t)v; break;
    case FX_DRIVE: f->drive = v; break;
    case FX_COMP: f->comp = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); fx_comp_prep(f); break;
    case FX_RV_DECAY: f->rv_decay = v; break;
    case FX_RV_TONE: f->rv_tone = v; break;
    case FX_RV_HPF: d9_biquad_set(&f->rv_hp, D9_HP, v, 0.7071f); break;
    case FX_RV_LEVEL: f->rv_level = v; break;
    case FX_DL_TIME: f->dl_div = (int32_t)v; fx_retime(f); break;
    case FX_DL_FDBK: f->dl_fdbk = v; break;
    case FX_DL_TONE: f->dl_tone = v; break;
    case FX_DL_LEVEL: f->dl_level = v; break;
    case FX_DL_HPF: d9_biquad_set(&f->dl_hp, D9_HP, v, 0.7071f); break;
    case FX_DL_TYPE: f->dl_type = (int32_t)v; break;
    case FX_DL_WEAR: f->wear = v; break;
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

void fxbus_init(fxbus_t *f, int16_t *dly_buf, int dly_len)
{
    uint8_t *p = (uint8_t *)f;
    for (unsigned i = 0; i < sizeof(*f); ++i)
        p[i] = 0;
    for (int i = 0; i < dly_len; ++i)
        dly_buf[i] = 0;
    f->dbuf = dly_buf;
    f->dlen = dly_len;

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
static void fx_reverb(fxbus_t *f, const float *in, float *out, int n)
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
    int16_t *cb[4];
    cb[0] = f->comb;
    cb[1] = cb[0] + fx_cl[0];
    cb[2] = cb[1] + fx_cl[1];
    cb[3] = cb[2] + fx_cl[2];
    float *ab0 = f->ap, *ab1 = f->ap + fx_al[0];
    int32_t quiet = f->rv_quiet;
    for (int i = 0; i < n; ++i) {
        const float x = d9_biquad_tick(&f->rv_hp, in[i]);
        float acc = 0.0f;
        int32_t wr = 0;
        for (int c = 0; c < 4; ++c) {
            int16_t *b = cb[c];
            const int32_t p = f->cpos[c];
            const float y = (float)b[p] * (1.0f / 2048.0f);
            acc += y;
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

static void fx_delay(fxbus_t *f, const float *in, float *out, int n)
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
    const float lvl = f->dl_level;
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
            lp += (y * fdbk - lp) * tc;
            if (fm_fabsf(lp) < 1e-20f) lp = 0.0f;
            int32_t k = (int32_t)((x + lp) * 2048.0f);       /* 12-bit, toward zero */
            if (k > 32767) k = 32767;
            if (k < -32767) k = -32767;
            buf[w] = (int16_t)k;
            if (++w >= len) w = 0;
            out[i] += y * lvl;
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
            float rp = (float)w - (dcur + wow_a * fx_psin(wp) + flut_a * fx_psin(fp));
            while (rp < 0.0f) rp += flen;
            int32_t i0 = (int32_t)rp;
            const float fr = rp - (float)i0;
            if (i0 >= len) i0 -= len;
            const int32_t i1 = i0 + 1 >= len ? 0 : i0 + 1;
            const float b0 = (float)buf[i0] * (1.0f / 2048.0f), b1 = (float)buf[i1] * (1.0f / 2048.0f);
            const float y = b0 + (b1 - b0) * fr;
            lp += (y * fdbk - lp) * tc;
            if (fm_fabsf(lp) < 1e-20f) lp = 0.0f;
            float s = d9_tanh(lp * g) * inv_g;                   /* tape saturation holds the loop */
            hp += (s - hp) * 0.0042725f;                          /* 30 Hz: no DC latch-up past unity */
            s -= hp;
            int32_t k = (int32_t)((x + s) * 2048.0f);
            if (k > 32767) k = 32767;
            if (k < -32767) k = -32767;
            buf[w] = (int16_t)k;
            if (++w >= len) w = 0;
            out[i] += y * lvl;
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
    fx_reverb(f, rev, out, n);
    fx_delay(f, dly, out, n);

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
