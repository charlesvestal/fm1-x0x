/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X TB-303 bass engine: a C99 / float / no-libm port of Open303 (Robin Schmidt, MIT) with
 * the Devilfish ranges and the Soft / RAT drive of schwung-303 (GPL-3.0; the RAT model is
 * from davemollen/dm-Rat, GPL-3.0).
 *
 * What is the same as schwung-303's engine: the signal path and every constant of Open303's
 * getSample() - pitch slew, the decay envelope with its two RC followers, the measured
 * env-mod mapping, the TB_303-mode TeeBeeFilter (mystran & kunn coefficients) with its
 * feedback highpass, the amp envelope, the 200 Hz de-clicker, and the allpass / highpass /
 * notch after the filter - and the pot-to-physical ranges of plugin.cpp.
 *
 * What is different, and why:
 *  - Oscillator: Open303's MipMappedWaveTable (2 x 12 x 2052 doubles, FFT-built) does not fit.
 *    Saw and the "303 square" are computed directly: the saw is polyBLEP; the square is the
 *    same transistor-shaper function Open303 tabulates, -tanh(F*(2p-1) + O), evaluated per
 *    sample (its falling edge is a smooth tanh ramp), with a polyBLEP on its one hard edge
 *    and its DC removed (the mip-map drops DC above 43 Hz).
 *  - Oversampling: BASS303_OS (1, 2 or 4; Open303 uses 4). The decimator is a polyphase IIR
 *    halfband (one stage per factor of 2) instead of Open303's 12th-order elliptic run at
 *    every oversample: 6 (OS 2) or 12 (OS 4) multiplies per output sample instead of ~100.
 *  - Control rate: envelopes, slew, cutoff and the filter's coefficients every BASS303_CTRL
 *    samples (the recursions advanced exactly, the values interpolated linearly between);
 *    Open303 computes them every sample. A trigger still jumps pitch and amp.
 *  - Idle: when the gate is off and the amp envelope has died (< -120 dB) for
 *    BASS303_IDLE_HOLD samples, render writes zeros and only advances the oscillator phase
 *    and the envelopes (closed form, once per block). Filter states are cleared on the way
 *    in, as Open303's own triggerNote() does after idle (schwung-303 never idles).
 *  - Only the four Devilfish controls that fit the panel are exposed (slide time, accent
 *    decay); the others stay at the stock values plugin.cpp uses with Devilfish off.
 *
 * Every instance is self-contained (no mutable static state): two can run side by side. */
#pragma once
#include <stdint.h>
#include "x0x_param.h"

#ifndef BASS303_OS
#define BASS303_OS 2                    /* oversampling factor: 1, 2 or 4 */
#endif
#if BASS303_OS != 1 && BASS303_OS != 2 && BASS303_OS != 4
#error "BASS303_OS must be 1, 2 or 4"
#endif

/* Idle hold-off, in samples of silence before the engine stops computing. Open303 never
 * stops: its oscillator and filters keep running under the closed amp, so a note after a rest
 * starts into warm filter states. Starting into cleared ones makes the first 5 ms of the note
 * 4-5 dB quieter than the reference (measured; every filter in the chain contributes, so no
 * cheap state initialisation or short pre-roll fixes it). Holding off keeps notes within a phrase exactly as
 * Open303 plays them; only a note after a longer silence starts cold. It costs nothing in
 * the worst case, which is a note sounding continuously. 0 = idle as soon as it is silent. */
#ifndef BASS303_IDLE_HOLD
#define BASS303_IDLE_HOLD 22050         /* 500 ms */
#endif
/* Control rate: the envelopes, slew, cutoff and filter coefficients are computed every
 * BASS303_CTRL samples (the envelope recursions advanced in exact closed form) and the
 * coefficients, increment and amp interpolated linearly in between. 1 = every sample, as
 * Open303. tests/host/run_bass303.sh measures 4 against 1. */
#ifndef BASS303_CTRL
#define BASS303_CTRL 4
#endif
#if BASS303_CTRL < 1 || BASS303_CTRL > 4
#error "BASS303_CTRL must be 1..4"
#endif
#define BASS303_SR 44100.0f
#define BASS303_MAX_BLOCK 256

/* parameter indices (pages of four: the UI shows KNOB 1-4) */
enum {
    BASS303_CUTOFF, BASS303_RESO, BASS303_ENVMOD, BASS303_DECAY,     /* page 1 */
    BASS303_ACCENT, BASS303_WAVE, BASS303_TUNE, BASS303_VOLUME,      /* page 2 */
    BASS303_DRIVE, BASS303_DRVTYPE, BASS303_SLIDE, BASS303_ACCDEC,   /* page 3 */
    BASS303_NPARAMS
};
enum { BASS303_DRV_OFF, BASS303_DRV_SOFT, BASS303_DRV_RAT };

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } bass303_bq_t;     /* TDF-II biquad */
typedef struct { float x1, x2, y1, y2; } bass303_df1_t;                /* DF-I state */
typedef struct { float x1, y1; } bass303_ap_t;                         /* halfband allpass */

typedef struct bass303 {
    uint8_t pot[BASS303_NPARAMS];

    /* derived from the pots (bass303_set) */
    float cutoff;            /* nominal cutoff, Hz */
    float reso;              /* resonanceSkewed */
    float env_scaler, env_offset;
    float accent;            /* 0..1 */
    float tuning;            /* A4, Hz */
    float amp_scaler;        /* volume, linear */
    float decay_c, accdec_c; /* main-envelope multipliers for normal / accented notes */
    float slew_c;            /* pitch slew-limiter coefficient */
    int wave;                /* 0 saw, 1 square */
    int drv_type;
    float drv_amt;

    /* fixed coefficients (sample-rate dependent; set in bass303_init) */
    float rc1_c, rc2_c;                 /* filter-envelope RC followers (3 ms, 15 ms) */
    float dc_b0, dc_b1, dc_a1, dc_a2;   /* de-clicker: 200 Hz Butterworth lowpass, b2 = b0 */
    float hp1_b0, hp1_a1;               /* pre-filter highpass at the oversampled rate */
    float fbhp_b0, fbhp_a1;             /* feedback highpass at the oversampled rate */
    float hp1l_b0, hp1l_a1, fbhpl_b0, fbhpl_a1;   /* the same two at the base rate (lite) */
    int lite, lite_cur;      /* X0X overload guard: render without oversampling (bass303_set_lite) */
    float ap_b0, ap_a1;                 /* 14 Hz allpass (b1 = 1) */
    float hp2_b0, hp2_a1;               /* 24 Hz highpass */
    float nt_b0, nt_b1, nt_a1, nt_a2;   /* 7.5 Hz notch (b2 = b0) */
    float sq_dc, sq_h;                  /* 303 square: mean, hard-edge height */

    /* voice */
    int gate;                /* a note is held */
    int idle;                /* nothing sounding: render writes zeros */
    int started;             /* a note has been played since init (oscillator runs) */
    int quiet;               /* samples of silence so far (idle hold-off) */
    int snap;                /* next control step starts without interpolation */
    int trig;                /* next control step follows a trigger: pitch and amp jump */

    /* control rate: c^m for m = 0..BASS303_CTRL of each recursion, the RC followers'
     * closed-form gains, and the interpolated values the samples use */
    float rc1_pw[BASS303_CTRL + 1], rc2_pw[BASS303_CTRL + 1], slew_pw[BASS303_CTRL + 1];
    float ampd_pw[BASS303_CTRL + 1], ampn_pw[BASS303_CTRL + 1], ampa_pw[BASS303_CTRL + 1];
    float env_pw[BASS303_CTRL + 1], g1[BASS303_CTRL + 1], g2[BASS303_CTRL + 1];
    float c_inc, c_b0, c_k, c_g2, c_a;
    float osc_freq, slew_y;
    float phase;             /* oscillator phase in turns, 0..1 */
    float env_c, env_y;      /* main (filter) decay envelope */
    float rc1_y, rc2_y;
    float accent_gain;
    float amp_y;             /* amp envelope */
    int amp_acc;             /* the release is the accented one */
    int amp_trig;            /* next sample is the attack */
    bass303_df1_t dc;
    float hp1_x1, hp1_y1;
    float f_y1, f_y2, f_y3, f_y4, fb_x1, fb_y1;   /* TeeBeeFilter */
#if BASS303_OS >= 2
    bass303_ap_t hb1[6];                 /* 2x -> 1x halfband */
#endif
#if BASS303_OS == 4
    bass303_ap_t hb2[3];                 /* 4x -> 2x halfband */
#endif
    float ap_x1, ap_y1, hp2_x1, hp2_y1;
    bass303_df1_t nt;

    /* drive (schwung-303 drive.h) */
    bass303_bq_t s_pre, s_post, up_lp, down_lp;
    float s_gain, s_inv;
    float r_b[4], r_a[4], r_z[3];        /* RAT op-amp, 3rd-order TDF-II */
    float r_corr_b1, r_corr_z, r_tone_b1, r_tone_z;
    float dcb_x1, dcb_y1;
} bass303_t;

void bass303_init(bass303_t *b);
void bass303_note_on(bass303_t *b, int note, int accent, int slide);
void bass303_note_off(bass303_t *b);
void bass303_all_off(bass303_t *b);
void bass303_render(bass303_t *b, float *out, int n);
/* the engine's overload guard: 1 = no oversampling from the next block (~12 % of the worst case
 * on the FM-1; some aliasing on bright, resonant notes), 0 = back to BASS303_OS */
void bass303_set_lite(bass303_t *b, int on);
int bass303_nparams(void);
const x0x_param_t *bass303_param(int i);
void bass303_set(bass303_t *b, int i, int value);
int bass303_get(const bass303_t *b, int i);
