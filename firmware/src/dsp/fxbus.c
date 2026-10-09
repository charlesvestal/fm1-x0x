/* SPDX-License-Identifier: GPL-3.0-only */
/* Send reverb, synced delay and master stage: a port of 9W9's er99_verb_tick,
 * er99_dly_tick, master distortion and master_glue (GPL-3.0). See fxbus.h. */
#include "fxbus.h"

static const int32_t fx_cl[4] = { 1116, 1188, 1277, 1356 };
static const int32_t fx_al[2] = { 556, 441 };
static const int32_t fx_alr[2] = { 579, 457 };
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
static const char *const fx_onoff_names[2] = { "OFF", "ON" };

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
};

#if X0X_PLATE
static const int32_t fx_plen[FX_PN];
#endif

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
    case FX_DL_PING: f->dl_ping = (int32_t)v; break;
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
#if X0X_PLATE
    for (int k = 0, at = 0; k < FX_PN; ++k) {
        f->pl_at[k] = at;
        f->pl_len[k] = fx_plen[k];
        at += fx_plen[k];
    }
    f->pl_c = 1.0f;
#endif

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
#if X0X_PLATE
/* ---- the plate: Dattorro's figure-eight tank (see fxbus.h), one tick per two samples ---- */
static const int32_t fx_plen[FX_PN] = {
    FX_PD(142), FX_PD(107), FX_PD(379), FX_PD(277), FX_PS(672) + FX_PEXC + 2, FX_PS(4453), FX_PS(1800), FX_PS(3720),
    FX_PS(908) + FX_PEXC + 2, FX_PS(4217), FX_PS(2656), FX_PS(3163)
};
#undef FX_RV_QUIET
#define FX_RV_QUIET (FX_PS(4453) + 4)       /* ticks with nothing written: every line is zero */
#define FX_PQ 8192.0f                       /* the lines' scale: 16 bits over +-4 */

static inline float pl_tap(const fxbus_t *f, int l, int k)    /* written k ticks ago, 1..len */
{
    int32_t j = f->pl_pos[l] - k;
    return (float)f->pl[f->pl_at[l] + (j < 0 ? j + f->pl_len[l] : j)] * (1.0f / FX_PQ);
}
static inline int32_t pl_push(fxbus_t *f, int l, float x)
{
    int32_t k = (int32_t)(x * FX_PQ), p = f->pl_pos[l];   /* truncated toward zero: the tail ends */
    k = k > 32767 ? 32767 : k < -32767 ? -32767 : k;
    f->pl[f->pl_at[l] + p] = (int16_t)k;
    f->pl_pos[l] = p + 1 >= f->pl_len[l] ? 0 : p + 1;
    return k;
}
static inline float pl_ap(fxbus_t *f, int l, int len, float x, float g, int32_t *wr)
{
    float z = pl_tap(f, l, len), v = x - g * z;
    *wr |= pl_push(f, l, v);
    return z + g * v;
}
static inline float pl_apm(fxbus_t *f, int l, float len, float x, float g, int32_t *wr)
{
    int k = (int)len;
    float z0 = pl_tap(f, l, k), z = z0 + (pl_tap(f, l, k + 1) - z0) * (len - (float)k), v = x - g * z;
    *wr |= pl_push(f, l, v);
    return z + g * v;
}

static void fx_reverb(fxbus_t *f, const float *in, float *out, float *out_r, int n)
{
    if (f->rv_quiet >= FX_RV_QUIET) {
        int any = 0;
        for (int i = 0; i < n; ++i)
            any |= in[i] != 0.0f;
        if (!any)
            return;
    }
    /* DECAY sets the tail's T60: 0.15 s, 0.5 s at the factory's (the old reverb's), 3 s at the top. The tank
     * takes decay^2 a half-pass of ~165 ms: decay = 2^(-0.822 / T60) */
    const float t = fm_maxf((f->rv_decay - 0.2f) * (1.0f / 0.73f), 1e-4f);
    const float t60 = 0.15f * fm_exp2f(4.32f * fm_exp2f(1.645f * fm_log2f(t)));
    const float decay = fm_exp2f(-0.822f / t60), dd2 = fm_minf(decay + 0.15f, 0.5f);
    const float damp = 0.25f + 0.6f * f->rv_tone, lvl = f->rv_level * 0.5f;
    int32_t quiet = f->rv_quiet;
    for (int i = 0; i < n; ++i) {
        const float x0 = d9_biquad_tick(&f->rv_hp, in[i]);
        if (f->pl_half == 0.0f) {                       /* the first of two: the midpoint out */
            f->pl_half = x0 == 0.0f ? 1e-30f : x0;
            float ml = 0.5f * (f->pl_pl + f->pl_ol), mr = 0.5f * (f->pl_pr + f->pl_or);
            out[i] += (out_r ? ml : 0.5f * (ml + mr)) * lvl;
            if (out_r)
                out_r[i] += mr * lvl;
            continue;
        }
        float x = 0.5f * (f->pl_half + x0), a, b;
        int32_t wr = 0;
        f->pl_half = 0.0f;
        const float fb_l = pl_tap(f, FX_PD2R, FX_PS(3163)), fb_r = pl_tap(f, FX_PD2L, FX_PS(3720));
        f->pl_bw += 0.7f * (x - f->pl_bw);              /* input bandwidth */
        x = pl_ap(f, FX_PIN1, FX_PD(142), f->pl_bw, 0.75f, &wr);
        x = pl_ap(f, FX_PIN2, FX_PD(107), x, 0.75f, &wr);
        x = pl_ap(f, FX_PIN3, FX_PD(379), x, 0.625f, &wr);
        x = pl_ap(f, FX_PIN4, FX_PD(277), x, 0.625f, &wr);
        {   /* the tank's modulation, ~0.9 Hz: sine and cosine for its halves */
            float s = f->pl_s + 0.000256f * f->pl_c, c = f->pl_c - 0.000256f * s;
            f->pl_s = s;
            f->pl_c = c;
        }
        a = pl_apm(f, FX_PAPL, (float)FX_PS(672) + (float)FX_PEXC * f->pl_s, x + decay * fb_l, -0.7f, &wr);
        wr |= pl_push(f, FX_PD1L, a);
        b = pl_tap(f, FX_PD1L, FX_PS(4453));
        f->pl_dl += damp * (b - f->pl_dl);
        b = pl_ap(f, FX_PAP2L, FX_PS(1800), f->pl_dl * decay, dd2, &wr);
        wr |= pl_push(f, FX_PD2L, b);
        a = pl_apm(f, FX_PAPR, (float)FX_PS(908) + (float)FX_PEXC * f->pl_c, x + decay * fb_r, -0.7f, &wr);
        wr |= pl_push(f, FX_PD1R, a);
        b = pl_tap(f, FX_PD1R, FX_PS(4217));
        f->pl_dr += damp * (b - f->pl_dr);
        b = pl_ap(f, FX_PAP2R, FX_PS(2656), f->pl_dr * decay, dd2, &wr);
        wr |= pl_push(f, FX_PD2R, b);
        f->pl_pl = f->pl_ol;
        f->pl_pr = f->pl_or;
        f->pl_ol = pl_tap(f, FX_PD1R, FX_PS(266)) + pl_tap(f, FX_PD1R, FX_PS(2974)) - pl_tap(f, FX_PAP2R, FX_PS(1913)) +
                   pl_tap(f, FX_PD2R, FX_PS(1996)) - pl_tap(f, FX_PD1L, FX_PS(1990)) - pl_tap(f, FX_PAP2L, FX_PS(187)) -
                   pl_tap(f, FX_PD2L, FX_PS(1066));
        f->pl_or = pl_tap(f, FX_PD1L, FX_PS(353)) + pl_tap(f, FX_PD1L, FX_PS(3627)) - pl_tap(f, FX_PAP2L, FX_PS(1228)) +
                   pl_tap(f, FX_PD2L, FX_PS(2673)) - pl_tap(f, FX_PD1R, FX_PS(2111)) - pl_tap(f, FX_PAP2R, FX_PS(335)) -
                   pl_tap(f, FX_PD2R, FX_PS(121));
        out[i] += (out_r ? f->pl_pl : 0.5f * (f->pl_pl + f->pl_pr)) * lvl;   /* one tick late: the midpoint has both */
        if (out_r)
            out_r[i] += f->pl_pr * lvl;
        quiet = (wr == 0 && x == 0.0f) ? quiet + 1 : 0;
    }
    f->rv_quiet = quiet;
}
#else
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
    int16_t *cb[4];
    cb[0] = f->comb;
    cb[1] = cb[0] + fx_cl[0];
    cb[2] = cb[1] + fx_cl[1];
    cb[3] = cb[2] + fx_cl[2];
    float *ab0 = f->ap, *ab1 = f->ap + fx_al[0];
    float *ar0 = f->apr, *ar1 = f->apr + fx_alr[0];
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
#endif

/* ===================================================================== */
/* Delay (er99_dly_tick), adds its return into out                        */
/* ===================================================================== */
static inline float fx_psin(uint32_t ph)                 /* parabolic sine of a turn */
{
    const float x = (float)(int32_t)ph * (1.0f / 2147483648.0f);
    return 4.0f * x * (1.0f - fm_fabsf(x));
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
 * the right at twice it, the loop fed from the right: echoes alternate L R L R, one time apart. The
 * line holds both, so PING needs twice the time to fit (a 2 s line: 1 s); past that it plays centred. */
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
            out[i] += y * lvl;
            if (out_r)
                out_r[i] += y2 * lvl;
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
            out[i] += y * lvl;
            if (out_r)
                out_r[i] += y2 * lvl;
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
    fx_reverb(f, rev, out, 0, n);
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
