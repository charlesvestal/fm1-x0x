/* SPDX-License-Identifier: GPL-3.0-only */
/* Portions Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The render in passes over sub-blocks, the drive in passes, the oversampling loop per wave, the fixed
 * coefficients shared by every instance (bass303_k) and the cold paths' maths out of line: after Optimist
 * (henri / hdavid, github.com/w0ts/optimist 7faf1452, 3b695372, 50a5ed5a, b735cf0d, 0a9fcfcb, a64ca03b, d6eef1d9,
 * GPL-3.0-only); the same samples, bit for bit (tests/host/dsp_hash_test.c). */
/* X0X TB-303 bass engine - see bass303.h for what is ported and what is not.
 *
 * Reference: schwung-303 src/dsp/open303/rosic_Open303.{h,cpp} (getSample, triggerNote,
 * slideToNote, calculateEnvModScalerAndOffset), rosic_TeeBeeFilter.h (TB_303 mode),
 * rosic_AnalogEnvelope / DecayEnvelope / LeakyIntegrator / OnePoleFilter / BiquadFilter,
 * rosic_MipMappedWaveTable.cpp (fillWithSaw303 / fillWithSquare303), src/dsp/plugin.cpp
 * (pot ranges) and src/dsp/drive.h (Soft / RAT). */
#include "bass303.h"
#include "fastmath.h"

#define FSO (BASS303_SR * (float)BASS303_OS)    /* oversampled rate */

/* The maths of the cold paths (pots, init, notes, idle) out of line: one copy of each function instead of one
 * per call site (the same code, so the same values: no fused multiply-add across a call either way). The
 * render keeps its inline calls. */
#define BASS303_COLD __attribute__((noinline))
static BASS303_COLD float c_expf(float x) { return fm_expf(x); }
static BASS303_COLD float c_exp2f(float x) { return fm_exp2f(x); }
static BASS303_COLD float c_log2f(float x) { return fm_log2f(x); }
static BASS303_COLD float c_powf(float x, float y) { return fm_powf(x, y); }
static BASS303_COLD float c_sinf(float x) { return fm_sinf(x); }
static BASS303_COLD float c_cosf(float x) { return fm_cosf(x); }
static float c_db2lin(float db) { return c_exp2f(db * 0.166096405f); }      /* fm_db2lin */
/* The render's passes (one call a sub-block): each register-allocated on its own, so a change elsewhere in the
 * render does not reshuffle (and spill) the hot loops */
#define BASS303_PASS __attribute__((noinline))

static bass303_fixed_t bass303_k;     /* the coefficients every instance shares (bass303.h) */

/* ---- parameters ------------------------------------------------------------------------ */

static const char *const wave_names[] = { "Saw", "Sqr" };
static const char *const drv_names[] = { "Off", "Soft", "RAT" };

/* defaults are schwung-303's (every knob at 0.5, Devilfish values stock), except Volume,
 * lowered from 0.8 so the nominal peak is ~0.5 */
static const x0x_param_t params[BASS303_NPARAMS] = {
    { "Cutoff", 127, 64, 0 },        /* 314 .. 2394 Hz, exponential */
    { "Reso", 127, 64, 0 },          /* 0 .. 100 % */
    { "EnvMod", 127, 64, 0 },        /* 0 .. 100 % */
    { "Decay", 127, 64, 0 },         /* 200 .. 2000 ms, exponential (stock 303 range) */
    { "Accent", 127, 64, 0 },        /* 0 .. 100 % */
    { "Wave", 1, 0, wave_names },
    { "Tune", 127, 64, 0 },          /* A4 = 440 + (pot - 64) * 0.625 Hz: 400 .. 479.4 */
    { "Volume", 127, 96, 0 },        /* -60 .. 0 dB; 96 = -14.6 dB: peaks ~0.5 (schwung-303: 102) */
    { "Drive", 127, 0, 0 },          /* 0 = bypass */
    { "DrvTyp", 2, 1, drv_names },
    { "Slide", 127, 21, 0 },         /* 2 .. 360 ms (Devilfish); 21 = 61 ms, stock is 60 */
    { "AccDec", 127, 7, 0 },         /* 30 .. 3000 ms (Devilfish); 7 = 194 ms, stock is 200 */
};

int bass303_nparams(void) { return BASS303_NPARAMS; }

const x0x_param_t *bass303_param(int i)
{
    return (i >= 0 && i < BASS303_NPARAMS) ? &params[i] : 0;
}

int bass303_get(const bass303_t *b, int i)
{
    return (i >= 0 && i < BASS303_NPARAMS) ? b->pot[i] : 0;
}

/* exp(-1/(tau_ms * fs / 1000)): one-pole / RC coefficient */
static float rc_coeff(float tau_ms, float fs)
{
    return tau_ms > 0.0f ? c_expf(-1000.0f / (tau_ms * fs)) : 0.0f;
}

/* pw[m] = c^m, m = 0..BASS303_CTRL */
static void powers(float *pw, float c)
{
    int m;
    pw[0] = 1.0f;
    for (m = 1; m <= BASS303_CTRL; m++)
        pw[m] = pw[m - 1] * c;
}

/* the main envelope's multiplier changed: its powers, and the closed-form gains of the two RC
 * followers it drives, g[m] = (1-a) sum_{n=1..m} a^(m-n) c^n (so r_m = a^m r_0 + g[m] e_0) */
static void env_tables(bass303_t *b)
{
    int m;
    powers(b->env_pw, b->env_c);
    b->g1[0] = b->g2[0] = 0.0f;
    for (m = 1; m <= BASS303_CTRL; m++) {
        b->g1[m] = bass303_k.rc1_c * b->g1[m - 1] + (1.0f - bass303_k.rc1_c) * b->env_pw[m];
        b->g2[m] = bass303_k.rc2_c * b->g2[m - 1] + (1.0f - bass303_k.rc2_c) * b->env_pw[m];
    }
}

/* Open303::calculateEnvModScalerAndOffset, the measured mapping */
static void calc_envmod(bass303_t *b)
{
    float e = (float)b->pot[BASS303_ENVMOD] * (1.0f / 127.0f);
    float c = c_log2f(b->cutoff * (1.0f / 313.8152786f)) * (1.0f / 2.931683907f); /* log2(c1/c0) */
    float slo = 3.773996325f * e + 0.736965594f;
    float shi = 4.194548788f * e + 0.864344901f;
    b->env_scaler = (1.0f - c) * slo + c * shi;
    b->env_offset = 0.048292931f * c + 0.294391201f;
}

/* drive.h set_lowpass / set_low_shelf (TDF-II coefficients, a0-normalised) */
static void bq_lowpass(bass303_bq_t *q, float fc, float fs)
{
    float w = FM_TWO_PI * fc / fs, cs = c_cosf(w), sn = c_sinf(w);
    float alpha = sn * 0.707106781f, a0 = 1.0f + alpha;     /* sn / (2 q), q = 1/sqrt2 */
    q->b0 = 0.5f * (1.0f - cs) / a0;
    q->b1 = (1.0f - cs) / a0;
    q->b2 = q->b0;
    q->a1 = -2.0f * cs / a0;
    q->a2 = (1.0f - alpha) / a0;
}

static void bq_low_shelf(bass303_bq_t *q, float fc, float gain_db, float fs)
{
    float A = c_db2lin(0.5f * gain_db);                       /* 10^(g/40) */
    float w = FM_TWO_PI * fc / fs, cs = c_cosf(w), sn = c_sinf(w);
    float alpha = sn * 0.707106781f;                           /* S = 1: sn/2 * sqrt(2) */
    float s2 = 2.0f * fm_sqrtf(A) * alpha;
    float a0 = (A + 1.0f) + (A - 1.0f) * cs + s2;
    q->b0 = A * ((A + 1.0f) - (A - 1.0f) * cs + s2) / a0;
    q->b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs) / a0;
    q->b2 = A * ((A + 1.0f) - (A - 1.0f) * cs - s2) / a0;
    q->a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cs) / a0;
    q->a2 = ((A + 1.0f) + (A - 1.0f) * cs - s2) / a0;
}

/* RAT op-amp: drive.h rat::State::update_op_amp_coeffs + the correction one-pole cutoff */
static void rat_coeffs(bass303_t *b)
{
    const float R1 = 100000.0f, C1 = 1e-10f, Z1_B0 = 2.72149e-7f, Z1_B1 = 0.0027354f;
    const float Z1_A0 = 6.27638e-9f, Z1_A1 = 0.0000069f;
    const float t = 1.0f / BASS303_SR, s0 = t * 0.5f, s1 = t * t * 0.25f, s2 = t * t * t * 0.125f;
    float dist = fm_maxf(b->drv_amt, 0.001f);
    float z2_b0 = fm_maxf(dist * R1, 1.0f), z2_a0 = z2_b0 * C1;
    float a0 = Z1_B0 * z2_a0, a1 = Z1_B0 + Z1_B1 * z2_a0, a2 = Z1_B1 + z2_a0;
    float num[4], den[4], nz[4], dz[4], inv;
    int i;
    num[0] = a0; num[1] = a1 + Z1_A0 * z2_b0; num[2] = Z1_A1 * z2_b0 + Z1_B1 + z2_a0; num[3] = 1.0f;
    den[0] = a0; den[1] = a1; den[2] = a2; den[3] = 1.0f;
    for (i = 0; i < 2; i++) {                                  /* bilinear transform */
        const float *x = i ? den : num;
        float *o = i ? dz : nz;
        float x0 = x[0], x1 = x[1] * s0, x2 = x[2] * s1, x3 = x[3] * s2;
        o[0] = x0 + x1 + x2 + x3;
        o[1] = -3.0f * x0 - x1 + x2 + 3.0f * x3;
        o[2] = 3.0f * x0 - x1 - x2 + 3.0f * x3;
        o[3] = -x0 + x1 - x2 + x3;
    }
    inv = 1.0f / dz[0];
    for (i = 0; i < 4; i++) {
        b->r_b[i] = nz[i] * inv;
        b->r_a[i] = dz[i] * inv;
    }
    /* correction cutoff = MAX_GAIN_AT_1HZ / (dist * (MAX_DIST_GAIN - 1) + 1) */
    b->r_corr_b1 = c_expf(-FM_TWO_PI / BASS303_SR * 1119360.558f / (dist * 2306.231003f + 1.0f));
}

void bass303_set(bass303_t *b, int i, int v)
{
    float n;
    if (i < 0 || i >= BASS303_NPARAMS)
        return;
    if (v < 0)
        v = 0;
    if (v > params[i].max)
        v = params[i].max;
    b->pot[i] = (uint8_t)v;
    n = (float)v * (1.0f / 127.0f);
    switch (i) {
    case BASS303_CUTOFF:
        b->cutoff = 314.0f * c_exp2f(n * 2.930586688f);       /* log2(2394/314) */
        calc_envmod(b);
        break;
    case BASS303_ENVMOD:
        calc_envmod(b);
        break;
    case BASS303_RESO:                                         /* TeeBeeFilter::setResonance */
        b->reso = (1.0f - c_expf(-3.0f * n)) * (1.0f / 0.950212932f);
        break;
    case BASS303_DECAY:                                        /* applied at the next note */
        b->decay_c = rc_coeff(200.0f * c_exp2f(n * 3.321928095f), BASS303_SR);
        break;
    case BASS303_ACCDEC:
        b->accdec_c = rc_coeff(30.0f + 2970.0f * n, BASS303_SR);
        break;
    case BASS303_ACCENT:
        b->accent = n;
        break;
    case BASS303_WAVE:
        b->wave = v;
        break;
    case BASS303_TUNE:
        b->tuning = 440.0f + (float)(v - 64) * 0.625f;
        break;
    case BASS303_VOLUME:
        b->amp_scaler = c_db2lin(-60.0f + 60.0f * n);
        break;
    case BASS303_SLIDE:                                        /* slew tau = 0.2 * slide time */
        b->slew_c = rc_coeff(0.2f * (2.0f + 358.0f * n), BASS303_SR);
        powers(b->slew_pw, b->slew_c);
        break;
    case BASS303_DRIVE:
        b->drv_amt = n;
        b->s_gain = c_db2lin(24.0f * n);
        b->s_inv = 1.0f / b->s_gain;
        rat_coeffs(b);
        break;
    case BASS303_DRVTYPE:
        if (v != b->drv_type) {                                /* drive.h set_model: fresh state */
            b->s_pre.z1 = b->s_pre.z2 = b->s_post.z1 = b->s_post.z2 = 0.0f;
            b->up_lp.z1 = b->up_lp.z2 = b->down_lp.z1 = b->down_lp.z2 = 0.0f;
            b->r_z[0] = b->r_z[1] = b->r_z[2] = b->r_corr_z = b->r_tone_z = 0.0f;
            b->dcb_x1 = b->dcb_y1 = 0.0f;
        }
        b->drv_type = v;
        break;
    }
}

/* clear everything that only matters while sound is running (Open303::triggerNote after idle) */
static void clear_audio_state(bass303_t *b)
{
    int i;
    b->dc.x1 = b->dc.x2 = b->dc.y1 = b->dc.y2 = 0.0f;
    b->hp1_x1 = b->hp1_y1 = 0.0f;
    b->f_y1 = b->f_y2 = b->f_y3 = b->f_y4 = b->fb_x1 = b->fb_y1 = 0.0f;
#if BASS303_OS >= 2
    for (i = 0; i < 6; i++)
        b->hb1[i].x1 = b->hb1[i].y1 = 0.0f;
#endif
#if BASS303_OS == 4
    for (i = 0; i < 3; i++)
        b->hb2[i].x1 = b->hb2[i].y1 = 0.0f;
#endif
    b->ap_x1 = b->ap_y1 = b->hp2_x1 = b->hp2_y1 = 0.0f;
    b->nt.x1 = b->nt.x2 = b->nt.y1 = b->nt.y2 = 0.0f;
    b->s_pre.z1 = b->s_pre.z2 = b->s_post.z1 = b->s_post.z2 = 0.0f;
    b->up_lp.z1 = b->up_lp.z2 = b->down_lp.z1 = b->down_lp.z2 = 0.0f;
    for (i = 0; i < 3; i++)
        b->r_z[i] = 0.0f;
    b->r_corr_z = b->r_tone_z = 0.0f;
    b->dcb_x1 = b->dcb_y1 = 0.0f;
    b->amp_y = 0.0f;
}

/* the shared coefficients (Open303 constructor + setSampleRate) */
static void fixed_init(void)
{
    float w, sn, cs, alpha, scale, x, F, O, ln_cosh_p, ln_cosh_m;

    bass303_k.rc1_c = rc_coeff(3.0f, BASS303_SR);           /* normalAttack */
    bass303_k.rc2_c = rc_coeff(15.0f, BASS303_SR);
    powers(bass303_k.rc1_pw, bass303_k.rc1_c);
    powers(bass303_k.rc2_pw, bass303_k.rc2_c);
    powers(bass303_k.ampd_pw, rc_coeff(1230.0f, BASS303_SR));         /* amp: y += (1 - c)(0 - y) */
    powers(bass303_k.ampn_pw, rc_coeff(1.0f, BASS303_SR));            /* release, normal: 1 ms */
    powers(bass303_k.ampa_pw, rc_coeff(50.0f, BASS303_SR));           /* release, accented: 50 ms */

    w = FM_TWO_PI * 200.0f / BASS303_SR;             /* de-clicker: LOWPASS12, q = sqrt(.5) */
    sn = c_sinf(w);
    cs = c_cosf(w);
    alpha = sn * 0.707106781f;
    scale = 1.0f / (1.0f + alpha);
    bass303_k.dc_a1 = 2.0f * cs * scale;
    bass303_k.dc_a2 = (alpha - 1.0f) * scale;
    bass303_k.dc_b1 = (1.0f - cs) * scale;
    bass303_k.dc_b0 = 0.5f * bass303_k.dc_b1;

    x = c_expf(-FM_TWO_PI * 44.486f / FSO);         /* OnePoleFilter HIGHPASS (dspguide) */
    bass303_k.hp1_b0 = 0.5f * (1.0f + x);
    bass303_k.hp1_a1 = x;
    x = c_expf(-FM_TWO_PI * 150.0f / FSO);          /* feedback highpass (stock 150 Hz) */
    bass303_k.fbhp_b0 = 0.5f * (1.0f + x);
    bass303_k.fbhp_a1 = x;
    x = c_expf(-FM_TWO_PI * 44.486f / BASS303_SR);  /* the same two at the base rate (lite) */
    bass303_k.hp1l_b0 = 0.5f * (1.0f + x);
    bass303_k.hp1l_a1 = x;
    x = c_expf(-FM_TWO_PI * 150.0f / BASS303_SR);
    bass303_k.fbhpl_b0 = 0.5f * (1.0f + x);
    bass303_k.fbhpl_a1 = x;
    x = c_expf(-FM_TWO_PI * 24.167f / BASS303_SR);
    bass303_k.hp2_b0 = 0.5f * (1.0f + x);
    bass303_k.hp2_a1 = x;
    x = fm_tanf(FM_PI * 14.008f / BASS303_SR);       /* OnePoleFilter ALLPASS (DAFX) */
    bass303_k.ap_b0 = (x - 1.0f) / (x + 1.0f);

    w = FM_TWO_PI * 7.5164f / BASS303_SR;            /* BANDREJECT, 4.7 octaves */
    sn = c_sinf(w);
    x = c_sinf(0.5f * w);
    cs = 1.0f - 2.0f * x * x;                        /* cos w without the cancellation */
    x = 0.5f * FM_LN2 * 4.7f * w / sn;               /* alpha = sn * sinh(x) */
    alpha = sn * 0.5f * (c_expf(x) - c_expf(-x));
    scale = 1.0f / (1.0f + alpha);
    bass303_k.nt_a1 = 2.0f * cs * scale;
    bass303_k.nt_a2 = (alpha - 1.0f) * scale;
    bass303_k.nt_b0 = scale;
    bass303_k.nt_b1 = -2.0f * cs * scale;

    /* 303 square = -tanh(F * (2p - 1) + O), F = dB2amp(36.9), O = 4.37 (fillWithSquare303,
     * after its 180 degree circular shift). Mean over a cycle, for the DC the mip-map drops:
     * -(ln cosh(F + O) - ln cosh(F - O)) / 2F; ln cosh(y) = y - ln 2 + ln(1 + e^-2y), y > 0. */
    F = c_db2lin(36.9f);
    O = 4.37f;
    ln_cosh_p = (F + O) + (c_log2f(1.0f + c_expf(-2.0f * (F + O))) * FM_LN2);
    ln_cosh_m = (F - O) + (c_log2f(1.0f + c_expf(-2.0f * (F - O))) * FM_LN2);
    bass303_k.sq_dc = -(ln_cosh_p - ln_cosh_m) / (2.0f * F);
    bass303_k.sq_h = fm_tanhf(F + O) + fm_tanhf(F - O);    /* height of the hard edge at p = 0 */

    /* drive.h prepare() */
    bq_low_shelf(&bass303_k.s_pre, 400.0f, 6.0f, BASS303_SR);
    bq_low_shelf(&bass303_k.s_post, 400.0f, -6.0f, BASS303_SR);
    bq_lowpass(&bass303_k.up_lp, 19000.0f, 2.0f * BASS303_SR);
    bq_lowpass(&bass303_k.down_lp, 19000.0f, 2.0f * BASS303_SR);
    /* tone fixed at 0.5: 1 / (2 pi (0.5 * 100k + 1.5k) 3.3n) */
    bass303_k.r_tone_b1 = c_expf(-FM_TWO_PI / BASS303_SR / (FM_TWO_PI * 51500.0f * 3.3e-9f));
    bass303_k.ready = 1;
}

void bass303_init(bass303_t *b)
{
    uint8_t *p = (uint8_t *)b;
    unsigned k;
    int i;

    for (k = 0; k < sizeof(*b); k++)
        p[k] = 0;
    if (!bass303_k.ready)
        fixed_init();
    b->drv_type = -1;
    for (i = 0; i < BASS303_NPARAMS; i++)
        bass303_set(b, i, params[i].def);

    b->osc_freq = b->slew_y = 440.0f;
    b->env_c = b->decay_c;
    env_tables(b);
    b->env_y = 0.0f;
    b->idle = 1;
    b->snap = 1;
}

/* ---- notes ----------------------------------------------------------------------------- */

static void set_accent_state(bass303_t *b, int accent)
{
    if (accent) {
        b->accent_gain = b->accent;
        b->env_c = b->accdec_c;          /* setMainEnvDecay(accentDecay) */
    } else {
        b->accent_gain = 0.0f;
        b->env_c = b->decay_c;
    }
    b->amp_acc = accent != 0;            /* release: accentAmpRelease 50 ms, normal 1 ms */
    env_tables(b);
}

void bass303_note_on(bass303_t *b, int note, int accent, int slide)
{
    float f;
    if (note < 0)
        note = 0;
    if (note > 127)
        note = 127;
    f = b->tuning * c_exp2f((float)(note - 69) * (1.0f / 12.0f));
    set_accent_state(b, accent);
    b->osc_freq = f;
    if (!(slide && b->gate)) {           /* triggerNote; else slideToNote (no retrigger) */
        b->slew_y = f;                   /* pitchSlewLimiter.setState */
        b->env_y = 1.0f / b->env_c;      /* mainEnv.trigger: the next sample is 1 */
        b->amp_trig = 1;                 /* ampEnv.noteOn: attack time 0 */
        b->trig = 1;
    }
    if (b->idle)
        b->snap = 1;                     /* no interpolation from the stale control values */
    b->gate = 1;
    b->idle = 0;
    b->quiet = 0;
    b->started = 1;
}

void bass303_note_off(bass303_t *b) { b->gate = 0; }
void bass303_set_lite(bass303_t *b, int on) { b->lite = on != 0; }
void bass303_all_off(bass303_t *b) { b->gate = 0; }

/* ---- render ---------------------------------------------------------------------------- */

/* polyBLEP residual for a unit (2-high) step at phase 0, t in [0, 1), dt = increment. The
 * divides happen only on the two samples per period next to the step. */
static inline float blep(float t, float dt)
{
    float x;
    if (t < dt) {
        x = t / dt;
        return x + x - x * x - 1.0f;
    }
    if (t > 1.0f - dt) {
        x = (t - 1.0f) / dt;
        return x * x + x + x + 1.0f;
    }
    return 0.0f;
}

#if BASS303_OS == 4                     /* (at 2x run_decim runs the sections in its own loop) */
static inline float hb(bass303_ap_t *s, float a, float x)
{
    float y = a * (x - s->y1) + s->x1;
    s->x1 = x;
    s->y1 = y;
    return y;
}
#endif

/* Halfband decimators: polyphase allpass pairs (de Soras' hiir structure; coefficients from
 * its closed-form elliptic design, response checked numerically). 2x -> 1x: 6 coefficients,
 * transition 0.04 (passband flat to 18.5 kHz, -74.5 dB from 25.6 kHz at the 2x rate). 4x -> 2x:
 * 3 coefficients, transition 0.137 (flat to 19.9 kHz, -62 dB over the band that folds below
 * 20 kHz). 8 / 4 coefficients (-99 / -81 dB) measured the same alias floor in
 * tests/host/run_bass303.sh: what reaches the decimator is already low-passed by the ladder.
 * Paths: even coefficients take the newer sample, odd ones the older. */
#if BASS303_OS == 4
static inline float decim_hb1(bass303_ap_t *s, float x_old, float x_new)
{
    float a = x_new, c = x_old;
    a = hb(&s[0], 0.068204076f, a);
    c = hb(&s[1], 0.240270358f, c);
    a = hb(&s[2], 0.448676236f, a);
    c = hb(&s[3], 0.641122367f, c);
    a = hb(&s[4], 0.799997564f, a);
    c = hb(&s[5], 0.934482236f, c);
    return 0.5f * (a + c);
}
#endif
#if BASS303_OS == 4
static inline float decim_hb2(bass303_ap_t *s, float x_old, float x_new)
{
    float a = x_new, c = x_old;
    a = hb(&s[0], 0.104996100f, a);
    c = hb(&s[1], 0.375357314f, c);
    a = hb(&s[2], 0.754467339f, a);
    return 0.5f * (a + c);
}
#endif

/* While idle, advance what Open303 keeps running so the next note starts where the
 * reference would: oscillator phase, slew, the main envelope and its RC followers
 * (closed forms of N steps of each recursion). */
static void idle_advance(bass303_t *b, int n)
{
    float N = (float)n, cn, an, ph;
    if (!b->started)
        return;                                       /* Open303 does not run before note 1 */
    ph = b->phase + N * b->slew_y * (1.0f / BASS303_SR);    /* N base samples */
    b->phase = ph - fm_floorf(ph);
    an = c_powf(b->slew_c, N);
    b->slew_y = b->osc_freq + an * (b->slew_y - b->osc_freq);
    cn = c_powf(b->env_c, N);
    an = c_powf(bass303_k.rc1_c, N);
    /* r[n] = a r[n-1] + (1-a) u[n], u[n] = E c^n:  r_N = a^N r0 + (1-a) E c (c^N - a^N)/(c - a) */
    b->rc1_y = an * b->rc1_y + (1.0f - bass303_k.rc1_c) * b->env_y * b->env_c * (cn - an) / (b->env_c - bass303_k.rc1_c);
    an = c_powf(bass303_k.rc2_c, N);
    if (b->accent_gain > 0.0f)
        b->rc2_y = an * b->rc2_y + (1.0f - bass303_k.rc2_c) * b->env_y * b->env_c * (cn - an) / (b->env_c - bass303_k.rc2_c);
    else
        b->rc2_y *= an;
    b->env_y = fm_flush(b->env_y * cn);
    b->rc1_y = fm_flush(b->rc1_y);
    b->rc2_y = fm_flush(b->rc2_y);
}

/* The drive (schwung-303 drive.h) in passes over sub-blocks of BASS303_DSUB samples, as the voice: each
 * filter's state and coefficients in registers for its pass, the same operations in the same order. The 2x
 * upsampler's odd input is 0 and the lowpass's b0, b1, b2 are positive, so b * 0 is +0: its zero-input step is
 * written without the three products (0 + z1, (0 - a1 y) + z2, 0 - a2 y), which is the same to the bit. */
#define BASS303_DSUB 32

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } bass303_bql_t;   /* a biquad in locals */

static inline void bq_get(bass303_bql_t *l, const bass303_bq_t *q, const bass303_bqz_t *z)
{
    l->b0 = q->b0, l->b1 = q->b1, l->b2 = q->b2, l->a1 = q->a1, l->a2 = q->a2, l->z1 = z->z1, l->z2 = z->z2;
}

static inline void bq_put(const bass303_bql_t *l, bass303_bqz_t *z) { z->z1 = l->z1, z->z2 = l->z2; }

static inline float bql_run(bass303_bql_t *q, float x)          /* bq_run */
{
    float y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

static inline float bql_run0(bass303_bql_t *q)                  /* bq_run(q, 0.0f), b0, b1, b2 > 0 */
{
    float y = 0.0f + q->z1;
    q->z1 = (0.0f - q->a1 * y) + q->z2;
    q->z2 = 0.0f - q->a2 * y;
    return y;
}

/* biquad q (state z) over x[0..n) in place, times g */
static void bq_pass(const bass303_bq_t *q, bass303_bqz_t *z, float *x, int n, float g)
{
    bass303_bql_t l;
    int i;
    bq_get(&l, q, z);
    for (i = 0; i < n; i++)
        x[i] = bql_run(&l, x[i]) * g;
    bq_put(&l, z);
}

/* the 2x upsampler: x[i], 0 through up_lp, times 2, into u[2i], u[2i + 1] */
static void up_pass(bass303_t *b, const float *x, float *u, int n)
{
    bass303_bql_t l;
    int i;
    bq_get(&l, &bass303_k.up_lp, &b->up_lp);
    for (i = 0; i < n; i++) {
        u[2 * i] = 2.0f * bql_run(&l, x[i]);
        u[2 * i + 1] = 2.0f * bql_run0(&l);
    }
    bq_put(&l, &b->up_lp);
}

/* the 2x downsampler: u[2i], u[2i + 1] through down_lp, the second kept, times g */
static void down_pass(bass303_t *b, const float *u, float *y, int n, float g)
{
    bass303_bql_t l;
    int i;
    bq_get(&l, &bass303_k.down_lp, &b->down_lp);
    for (i = 0; i < n; i++) {
        bql_run(&l, u[2 * i]);
        y[i] = bql_run(&l, u[2 * i + 1]) * g;
    }
    bq_put(&l, &b->down_lp);
}

/* the DC blocker (0.9996) in place */
static void dcb_pass(bass303_t *b, float *x, int n)
{
    float x1 = b->dcb_x1, y1 = b->dcb_y1;
    int i;
    for (i = 0; i < n; i++) {
        float y = x[i], yh = y - x1 + 0.9996f * y1;
        x1 = y;
        y1 = yh;
        x[i] = yh;
    }
    b->dcb_x1 = x1;
    b->dcb_y1 = y1;
}

/* Soft: pre shelf x gain, 2x up, tanh(u + 0.15) - its value at 0, 2x down, x 1/gain, post shelf, DC blocker */
static BASS303_PASS void drive_soft(bass303_t *b, float *out, int n)
{
    float u[2 * BASS303_DSUB];
    int i;
    bq_pass(&bass303_k.s_pre, &b->s_pre, out, n, b->s_gain);
    up_pass(b, out, u, n);
    for (i = 0; i < 2 * n; i++)
        u[i] = fm_tanhf(u[i] + 0.15f) - 0.14888503f;
    down_pass(b, u, out, n, b->s_inv);
    bq_pass(&bass303_k.s_post, &b->s_post, out, n, 1.0f);
    dcb_pass(b, out, n);
}

/* RAT: op-amp (3rd-order TDF-II), the correction one-pole x 1.877, 2x up, the clipper, 2x down x 0.3204805,
 * the tone one-pole, DC blocker */
static BASS303_PASS void drive_rat(bass303_t *b, float *out, int n)
{
    float u[2 * BASS303_DSUB];
    int i;
    {
        const float b0 = b->r_b[0], b1 = b->r_b[1], b2 = b->r_b[2], b3 = b->r_b[3];
        const float a1 = b->r_a[1], a2 = b->r_a[2], a3 = b->r_a[3];
        float z0 = b->r_z[0], z1 = b->r_z[1], z2 = b->r_z[2];
        for (i = 0; i < n; i++) {
            float x = out[i], y = x * b0 + z0;
            z0 = x * b1 - y * a1 + z1;
            z1 = x * b2 - y * a2 + z2;
            z2 = x * b3 - y * a3;
            out[i] = y;
        }
        b->r_z[0] = z0, b->r_z[1] = z1, b->r_z[2] = z2;
    }
    {
        const float cb1 = b->r_corr_b1, cb0 = 1.0f - cb1;
        float z = b->r_corr_z;
        for (i = 0; i < n; i++) {
            z = out[i] * cb0 + z * cb1;
            out[i] = z * 1.877f;
        }
        b->r_corr_z = z;
    }
    up_pass(b, out, u, n);
    for (i = 0; i < 2 * n; i++) {   /* x / (1 + x^4)^(1/4) = x s t, s = y^(-1/2), t = s^(-1/2) = y^(1/4): no divide */
        float x = u[i], x2 = x * x, s = fm_rsqrtf(1.0f + x2 * x2);
        u[i] = x * (s * fm_rsqrtf(s));
    }
    down_pass(b, u, out, n, 0.3204805f);
    {
        const float tb1 = bass303_k.r_tone_b1, tb0 = 1.0f - tb1;
        float z = b->r_tone_z;
        for (i = 0; i < n; i++) {
            z = out[i] * tb0 + z * tb1;
            out[i] = z;
        }
        b->r_tone_z = z;
    }
    dcb_pass(b, out, n);
}

static void drive_block(bass303_t *b, float *out, int n)
{
    int i, ns;
    for (i = 0; i < n; i += ns) {
        ns = n - i < BASS303_DSUB ? n - i : BASS303_DSUB;
        if (b->drv_type == BASS303_DRV_SOFT)
            drive_soft(b, out + i, ns);
        else
            drive_rat(b, out + i, ns);
    }
}

static const float recip[5] = { 0.0f, 1.0f, 0.5f, 0.333333343f, 0.25f };

/* The render runs in passes over sub-blocks of BASS303_SUB samples (a multiple of BASS303_CTRL, so the control
 * steps fall where they would in one pass): the voice (control steps, the amp's ramp, the oversampled
 * oscillator and filters), the de-clicker on the amp, the decimator, the filters after it. Each pass keeps its
 * state in registers (the FPU works on the 16 general registers: one pass for everything spilled on every
 * sample); every value is computed by the same operations in the same order as one pass would, so the output
 * is the same to the bit. */
#define BASS303_SUB 32

/* Pass 1 runs as three loops over the sub-block: the control steps (which also advance the per-sample linear
 * interpolation and keep each sample's values in cs[]), the oscillator and the highpass, the TeeBeeFilter.
 * One loop for all three holds ~24 values on the 16 registers and spills some on every oversample (which ones
 * depends on the code around it); each of these fits. */
typedef struct { float inc, f_b0, k, g2; } bass303_cs_t;      /* a sample's interpolated control values */

/* 1a: per control step, Open303's per-sample recursions advanced m samples in closed form (exact), the cutoff
 * and amp they produce at the chunk's last sample; per sample, the linear interpolation of the increment, the
 * filter's coefficients and the amp into cs[] and ab[] */
static BASS303_PASS void run_control(bass303_t *b, bass303_cs_t *cs, float *ab, int n)
{
    int i, j, m;
    float inc = b->c_inc, f_b0 = b->c_b0, k = b->c_k, g2 = b->c_g2, a = b->c_a;
    const float osc_freq = b->osc_freq, acc = b->accent_gain;
    const float scaler = b->env_scaler, offset = b->env_offset, cutoff = b->cutoff, r = b->reso;
    const float *amp_pw = b->gate ? bass303_k.ampd_pw : b->amp_acc ? bass303_k.ampa_pw : bass303_k.ampn_pw;
    const float amp_boost = b->gate ? 0.45f + 4.0f * acc : 0.0f;
    /* the overload guard's lite mode: no oversampling; a change of rate jumps to the new rate's
     * coefficients (snap) rather than gliding between the two rates' values */
    const int lite = BASS303_LITE && b->lite;
    const float fso = lite ? BASS303_SR : FSO;

    for (i = 0; i < n; i += m) {
        float e0, fc, fx, n_b0, n_k, n_g2, n_inc, n_a, rm, d_inc, d_b0, d_k, d_g2, d_a;
        m = n - i < BASS303_CTRL ? n - i : BASS303_CTRL;

        e0 = b->env_y;                                         /* main envelope: y *= c */
        b->env_y = e0 * b->env_pw[m];
        b->rc1_y = bass303_k.rc1_pw[m] * b->rc1_y + b->g1[m] * e0;    /* RC followers of it */
        b->rc2_y = bass303_k.rc2_pw[m] * b->rc2_y + (acc > 0.0f ? b->g2[m] * e0 : 0.0f);
        b->slew_y = osc_freq + b->slew_pw[m] * (b->slew_y - osc_freq);
        if (b->amp_trig) {                                     /* attack 0: 1, then decay */
            b->amp_y = amp_pw[m - 1];
            b->amp_trig = 0;
        } else {
            b->amp_y *= amp_pw[m];
        }
        if (lite != b->lite_cur) {
            b->lite_cur = lite;
            b->snap = 1;
        }
        n_inc = b->slew_y * (1.0f / fso);
        n_a = b->amp_y + amp_boost * b->env_y;
        fc = cutoff * fm_exp2f(scaler * (b->rc1_y - offset) + acc * b->rc2_y);   /* n1 = n2 = 1 */
        fc = fm_clampf(fc, 200.0f, 20000.0f);
        /* TeeBeeFilter::calculateCoefficientsApprox4, TB_303 branch only (the a1 polynomial
         * it computes first is overwritten there, so it is skipped) */
        fx = fc * (0.707106781f / fso);
        n_b0 = (0.00045522346f + 6.1922189f * fx) / (1.0f + 12.358354f * fx + 4.4156345f * (fx * fx));
        n_k = fx * (fx * (fx * (fx * (fx * (fx + 7198.6997f) - 5837.7917f) - 476.47308f) + 614.95611f) + 213.87126f) + 16.998792f;
        n_g2 = 2.0f * ((n_k * (1.0f / 17.0f) - 1.0f) * r + 1.0f) * (1.0f + r);
        n_k *= r;
        if (b->snap) {                                         /* first chunk after init / idle */
            inc = n_inc; f_b0 = n_b0; k = n_k; g2 = n_g2; a = n_a;
            d_inc = d_b0 = d_k = d_g2 = d_a = 0.0f;
            b->snap = 0;
        } else {                                               /* per-sample linear interpolation */
            rm = recip[m];
            d_inc = (n_inc - inc) * rm;
            d_b0 = (n_b0 - f_b0) * rm;
            d_k = (n_k - k) * rm;
            d_g2 = (n_g2 - g2) * rm;
            d_a = (n_a - a) * rm;
            if (b->trig) {               /* a trigger jumps pitch and amp in Open303: no ramp */
                inc = n_inc;
                a = n_a;
                d_inc = d_a = 0.0f;
            }
        }
        b->trig = 0;

        for (j = i; j < i + m; j++) {
            inc += d_inc;
            f_b0 += d_b0;
            k += d_k;
            g2 += d_g2;
            a += d_a;
            cs[j].inc = inc;
            cs[j].f_b0 = f_b0;
            cs[j].k = k;
            cs[j].g2 = g2;
            ab[j] = a;                                         /* (pass 2 de-clicks it) */
        }
    }
    b->c_inc = inc;
    b->c_b0 = f_b0;
    b->c_k = k;
    b->c_g2 = g2;
    b->c_a = a;
}

/* 1b: n samples of the oversampled oscillator -> highpass, nos values a sample into hb; inlined with nos and
 * square constant where they are (no test of the wave per oversample) */
static inline __attribute__((always_inline)) void osc_samples(bass303_t *b, const bass303_cs_t *cs, float *hb,
                                                              int n, int nos, int square)
{
    const int lite = BASS303_LITE && b->lite;
    const float hp1_b0 = lite ? bass303_k.hp1l_b0 : bass303_k.hp1_b0, hp1_a1 = lite ? bass303_k.hp1l_a1 : bass303_k.hp1_a1;
    const float sq_dc = bass303_k.sq_dc, sq_h2 = 0.5f * bass303_k.sq_h;
    float phase = b->phase, hp1x = b->hp1_x1, hp1y = b->hp1_y1;
    int i, q;
    for (i = 0; i < n; i++) {
        const float inc = cs[i].inc;
        for (q = 0; q < nos; q++) {
            float s, t, ym;
            if (!square) {
                t = phase + 0.5f;                               /* Saw303: rises -1..1, jump at p = .5 */
                if (t >= 1.0f)
                    t -= 1.0f;
                s = -(t + t - 1.0f - blep(t, inc));             /* Open303 inverts the osc */
            } else {
                /* Square303 (shaped saw, minus its mean, polyBLEP on the hard rising edge
                 * at p = 0), times 0.5 as BlendOscillator scales it, inverted */
                s = 0.5f * (fm_tanhf(69.98419960f * (phase + phase - 1.0f) + 4.37f) + sq_dc - sq_h2 * blep(phase, inc));
            }
            phase += inc;
            if (phase >= 1.0f)
                phase -= 1.0f;

            ym = hp1_b0 * (s - hp1x) + hp1_a1 * hp1y;
            hp1x = s;
            hp1y = ym;
            *hb++ = ym;
        }
    }
    b->phase = phase;
    b->hp1_x1 = hp1x;
    b->hp1_y1 = hp1y;
}

static BASS303_PASS void run_osc_saw(bass303_t *b, const bass303_cs_t *cs, float *hb, int n)
{
    osc_samples(b, cs, hb, n, BASS303_OS, 0);
}

static BASS303_PASS void run_osc_sqr(bass303_t *b, const bass303_cs_t *cs, float *hb, int n)
{
    osc_samples(b, cs, hb, n, BASS303_OS, 1);
}

/* 1c: the TeeBeeFilter (its feedback through the highpass) on x's n * nos values, in place */
static inline __attribute__((always_inline)) void ladder_samples(bass303_t *b, const bass303_cs_t *cs, float *x,
                                                                 int n, int nos)
{
    const int lite = BASS303_LITE && b->lite;
    const float fb_b0 = lite ? bass303_k.fbhpl_b0 : bass303_k.fbhp_b0, fb_a1 = lite ? bass303_k.fbhpl_a1 : bass303_k.fbhp_a1;
    float y1 = b->f_y1, y2 = b->f_y2, y3 = b->f_y3, y4 = b->f_y4, fbx = b->fb_x1, fby = b->fb_y1;
    int i, q;
    for (i = 0; i < n; i++) {
        const float f_b0 = cs[i].f_b0, k = cs[i].k, g2 = cs[i].g2;
        for (q = 0; q < nos; q++) {
            float t = k * y4;                                   /* feedback through the highpass */
            fby = fb_b0 * (t - fbx) + fb_a1 * fby;
            fbx = t;
            t = *x - fby;
            y1 += 2.0f * f_b0 * (t - y1 + y2);
            y2 += f_b0 * (y1 - 2.0f * y2 + y3);
            y3 += f_b0 * (y2 - 2.0f * y3 + y4);
            y4 += f_b0 * (y3 - 2.0f * y4);
            *x++ = g2 * y4;
        }
    }
    b->f_y1 = y1;
    b->f_y2 = y2;
    b->f_y3 = y3;
    b->f_y4 = y4;
    b->fb_x1 = fbx;
    b->fb_y1 = fby;
}

static BASS303_PASS void run_ladder(bass303_t *b, const bass303_cs_t *cs, float *x, int n)
{
    ladder_samples(b, cs, x, n, BASS303_OS);
}

/* pass 1: the voice, nos values a sample into osb */
static void run_voice(bass303_t *b, float *ab, float *osb, int n, int nos)
{
    bass303_cs_t cs[BASS303_SUB];
    run_control(b, cs, ab, n);
    if (nos == BASS303_OS) {
        if (b->wave)
            run_osc_sqr(b, cs, osb, n);
        else
            run_osc_saw(b, cs, osb, n);
        run_ladder(b, cs, osb, n);
    } else {                                                   /* (lite) */
        osc_samples(b, cs, osb, n, nos, b->wave);
        ladder_samples(b, cs, osb, n, nos);
    }
}

/* pass 2: the amp de-clicked (200 Hz Butterworth), times the volume, in place */
static BASS303_PASS void run_amp(bass303_t *b, float *ab, int n)
{
    const float b0 = bass303_k.dc_b0, b1 = bass303_k.dc_b1, a1 = bass303_k.dc_a1, a2 = bass303_k.dc_a2, g = b->amp_scaler;
    float x1 = b->dc.x1, x2 = b->dc.x2, y1 = b->dc.y1, y2 = b->dc.y2;
    int i;
    _Pragma("clang loop unroll_count(2)")   /* (no register moves for the state between samples) */
    for (i = 0; i < n; i++) {
        float x = ab[i], y = b0 * (x + x2) + b1 * x1 + a1 * y1 + a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        ab[i] = y * g;
    }
    b->dc.x1 = x1;
    b->dc.x2 = x2;
    b->dc.y1 = y1;
    b->dc.y2 = y2;
}

#if BASS303_OS == 2
/* pass 3: decim_hb1 on each pair. In a chain of allpass sections a section's input is the one before's output,
 * so its x1 is the previous section's y1: each path keeps four values (x, then the three y1) */
static BASS303_PASS void run_decim(bass303_t *b, const float *osb, float *out, int n)
{
    bass303_ap_t *s = b->hb1;
    float a0 = s[0].x1, a1 = s[0].y1, a2 = s[2].y1, a3 = s[4].y1;    /* newer samples: s[0], s[2], s[4] */
    float c0 = s[1].x1, c1 = s[1].y1, c2 = s[3].y1, c3 = s[5].y1;    /* older: s[1], s[3], s[5] */
    int i;
    _Pragma("clang loop unroll_count(2)")   /* (no register moves for the state between samples) */
    for (i = 0; i < n; i++) {
        float xo = osb[2 * i], xn = osb[2 * i + 1], p0, p1, p2, q0, q1, q2;
        p0 = 0.068204076f * (xn - a1) + a0;
        q0 = 0.240270358f * (xo - c1) + c0;
        p1 = 0.448676236f * (p0 - a2) + a1;
        q1 = 0.641122367f * (q0 - c2) + c1;
        p2 = 0.799997564f * (p1 - a3) + a2;
        q2 = 0.934482236f * (q1 - c3) + c2;
        a0 = xn, a1 = p0, a2 = p1, a3 = p2;
        c0 = xo, c1 = q0, c2 = q1, c3 = q2;
        out[i] = 0.5f * (p2 + q2);
    }
    s[0].x1 = a0, s[0].y1 = s[2].x1 = a1, s[2].y1 = s[4].x1 = a2, s[4].y1 = a3;
    s[1].x1 = c0, s[1].y1 = s[3].x1 = c1, s[3].y1 = s[5].x1 = c2, s[5].y1 = c3;
}
#elif BASS303_OS == 4
static BASS303_PASS void run_decim(bass303_t *b, const float *osb, float *out, int n)
{
    int i;
    for (i = 0; i < n; i++, osb += 4)
    {
        /* Both calls advance hb2: sequence older samples first on every compiler. */
        float older = decim_hb2(b->hb2, osb[0], osb[1]);
        float newer = decim_hb2(b->hb2, osb[2], osb[3]);
        out[i] = decim_hb1(b->hb1, older, newer);
    }
}
#endif

/* pass 4: allpass 14 Hz, highpass 24 Hz, notch 7.5 Hz at the base rate, times the amp. A section's x1 is the
 * one before's y1 here too (hp2_x1 = ap_y1, nt.x1 = hp2_y1) */
static BASS303_PASS void run_post(bass303_t *b, const float *ab, float *out, int n)
{
    const float ap_b0 = bass303_k.ap_b0, hp2_b0 = bass303_k.hp2_b0, hp2_a1 = bass303_k.hp2_a1;
    const float nt_b0 = bass303_k.nt_b0, nt_b1 = bass303_k.nt_b1, nt_a1 = bass303_k.nt_a1, nt_a2 = bass303_k.nt_a2;
    float ap_x1 = b->ap_x1, ap_y1 = b->ap_y1, hp2_y1 = b->hp2_y1, nt_x2 = b->nt.x2, nt_y1 = b->nt.y1, nt_y2 = b->nt.y2;
    int i;
    _Pragma("clang loop unroll_count(2)")   /* (no register moves for the state between samples) */
    for (i = 0; i < n; i++) {
        float s = out[i], t, u;
        t = ap_b0 * (s - ap_y1) + ap_x1;
        ap_x1 = s;
        s = hp2_b0 * (t - ap_y1) + hp2_a1 * hp2_y1;
        ap_y1 = t;
        u = nt_b0 * (s + nt_x2) + nt_b1 * hp2_y1 + nt_a1 * nt_y1 + nt_a2 * nt_y2;
        nt_x2 = hp2_y1;
        hp2_y1 = s;
        nt_y2 = nt_y1;
        nt_y1 = u;
        out[i] = u * ab[i];
    }
    b->ap_x1 = ap_x1;
    b->ap_y1 = b->hp2_x1 = ap_y1;
    b->hp2_y1 = b->nt.x1 = hp2_y1;
    b->nt.x2 = nt_x2;
    b->nt.y1 = nt_y1;
    b->nt.y2 = nt_y2;
}

static void run(bass303_t *b, float *out, int n)
{
    float ab[BASS303_SUB];
#if BASS303_OS > 1
    float osb[BASS303_SUB * BASS303_OS];
#endif
    int i, ns;
    for (i = 0; i < n; i += ns) {
        ns = n - i < BASS303_SUB ? n - i : BASS303_SUB;
#if BASS303_OS > 1
        if (!BASS303_LITE || !b->lite) {
            run_voice(b, ab, osb, ns, BASS303_OS);
            run_decim(b, osb, out + i, ns);
        } else
#endif
        {
            run_voice(b, ab, out + i, ns, 1);
        }
        run_amp(b, ab, ns);
        run_post(b, ab, out + i, ns);
    }
    b->env_y = fm_flush(b->env_y);
    b->rc1_y = fm_flush(b->rc1_y);
    b->rc2_y = fm_flush(b->rc2_y);
    b->amp_y = fm_flush(b->amp_y);
}

void bass303_render(bass303_t *b, float *out, int n)
{
    int i;
    if (b->idle) {
        for (i = 0; i < n; i++)
            out[i] = 0.0f;
        idle_advance(b, n);
        return;
    }
    run(b, out, n);

    if (b->drv_type != BASS303_DRV_OFF && b->drv_amt > 0.0f)
        drive_block(b, out, n);

    /* states that decay toward zero while the engine still runs (the idle hold-off, where the
     * amp is exactly 0): flushed once per block, before they can reach denormals */
    b->dc.x1 = fm_flush(b->dc.x1);
    b->dc.x2 = fm_flush(b->dc.x2);
    b->dc.y1 = fm_flush(b->dc.y1);
    b->dc.y2 = fm_flush(b->dc.y2);
    b->s_pre.z1 = fm_flush(b->s_pre.z1);
    b->s_pre.z2 = fm_flush(b->s_pre.z2);
    b->s_post.z1 = fm_flush(b->s_post.z1);
    b->s_post.z2 = fm_flush(b->s_post.z2);
    b->up_lp.z1 = fm_flush(b->up_lp.z1);
    b->up_lp.z2 = fm_flush(b->up_lp.z2);
    b->down_lp.z1 = fm_flush(b->down_lp.z1);
    b->down_lp.z2 = fm_flush(b->down_lp.z2);
    b->r_z[0] = fm_flush(b->r_z[0]);
    b->r_z[1] = fm_flush(b->r_z[1]);
    b->r_z[2] = fm_flush(b->r_z[2]);
    b->r_corr_z = fm_flush(b->r_corr_z);
    b->r_tone_z = fm_flush(b->r_tone_z);
    b->dcb_x1 = fm_flush(b->dcb_x1);
    b->dcb_y1 = fm_flush(b->dcb_y1);

    /* idle: gate off, amp envelope and de-clicker below -120 dB and the drive's DC blocker
     * settled, for BASS303_IDLE_HOLD samples in a row (see bass303.h) */
    if (!b->gate && b->amp_y < 1e-6f && fm_fabsf(b->dc.y1) < 1e-6f && fm_fabsf(b->dc.y2) < 1e-6f
        && fm_fabsf(b->dcb_y1) < 1e-6f) {
        b->quiet += n;
        if (b->quiet >= BASS303_IDLE_HOLD) {
            clear_audio_state(b);
            b->idle = 1;
        }
    } else {
        b->quiet = 0;
    }
}
