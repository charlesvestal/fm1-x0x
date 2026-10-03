/* SPDX-License-Identifier: GPL-3.0-only */
/* BREAK part: an 8-slice breakbeat player plus the BB Gen generator.
 *
 * Generator ported from schwung-breakbeat (BB Gen) by mestela, used with the
 * author's permission. See breaks.c for what was kept and what was adapted.
 *
 * A loop is IMA ADPCM in flash (mono, low nibble first, initial state 0/0). It is
 * cut into 8 equal slices; the generator picks a slice on every trigger, and the
 * player resamples it so the loop's stated `bars` follow the current tempo (pitch
 * follows tempo, as in BB Gen), times the Pitch param.
 *
 * Threading: breaks_set_loop, breaks_set, breaks_live may run in the main loop
 * while breaks_step / breaks_render / breaks_stop run in the audio ISR (single
 * core: the ISR runs to completion). Every cross-context write is a single byte
 * or aligned word; breaks_set_loop marks the bank invalid while it rebuilds it.
 * Output rate is BRK_FS. Float only, no libm, no allocation. */
#pragma once
#include <stdint.h>
#include "x0x_param.h"
#include "../seq/pattern.h"           /* BRK_* settings order */

#define BRK_FS 44100                  /* output sample rate */
#define BRK_NCK 480                   /* ADPCM checkpoints per loop (4 bytes each) */
#define BRK_RCH 512                   /* reverse playback decodes this many samples at a time */
#ifndef BRK_FADE
#define BRK_FADE 88                   /* crossfade / gate ramp: 2 ms at 44.1 kHz */
#endif

typedef struct {
    const uint8_t *adpcm;   /* IMA ADPCM nibbles, low nibble first, initial predictor 0 / index 0 */
    uint32_t nsamples;      /* decoded samples */
    uint32_t rate;          /* 22050 (or 44100) */
    uint8_t bars;           /* musical length of the loop in bars (the user states it; 1..8) */
} brk_loop_t;

/* indices of breaks_param() past the per-pattern settings */
enum { BRK_LEVEL = BRK_NSET, BRK_PITCH, BRK_REV, BRK_DLY, BRK_NPARAM };

typedef struct {
    int16_t pred;                     /* IMA state BEFORE decoding the nibble at that index */
    uint8_t idx, rsv;
} brk_ima_t;

typedef struct {
    const uint8_t *adpcm;
    uint32_t n;                       /* samples */
    float sp16;                       /* source samples per 16th note: n / (bars * 16) */
    uint32_t start[9];                /* slice starts, start[8] = n */
    brk_ima_t at_start[8];            /* exact decoder state at each slice start */
    brk_ima_t ck[BRK_NCK];            /* decoder state every 1 << ck_shift samples */
    uint8_t ck_shift;
    volatile uint8_t valid;           /* 0 while breaks_set_loop rebuilds this bank */
    uint8_t gen;                      /* bumped on every load: voices of an old load die */
    uint8_t rsv;
} brk_bank_t;

typedef struct {
    uint8_t on, rev, bank, gen;
    uint32_t pos;                     /* index of s1 (the sample being approached) */
    int32_t pred, idx;                /* forward: decoder state after decoding pos */
    float frac, s0, s1;               /* output = s0 + (s1 - s0) * frac */
    float g, dg;                      /* crossfade gain and its per-sample slope */
    uint32_t lo, hi;                  /* reverse: wraps within [lo, hi) (the slice) */
    uint32_t rbase;                   /* reverse: rbuf holds samples rbase .. rbase + BRK_RCH - 1 */
    uint8_t rvalid, rsv[3];
    int16_t rbuf[BRK_RCH];
} brk_voice_t;

/* BB Gen's live performance state (perf.h), reduced to this part's keys */
typedef struct {
    int8_t stack[8];                  /* held A slices, most recent last */
    uint8_t count;
    uint8_t reverse, half, stutter;   /* momentary macros held */
    float rate_mult;                  /* 0.5 while half speed is held */
    float trig_acc;                   /* half-speed trigger gate accumulator */
} brk_perf_t;

typedef struct breaks {
    brk_bank_t bank[2];               /* 0 = A, 1 = B */
    brk_voice_t v[2];                 /* the sounding voice and the one fading out */
    uint8_t cur;                      /* index of the sounding voice */
    uint8_t set[BRK_NPARAM];          /* parameter values */
    uint32_t rng;                     /* xorshift32 */

    /* transport / generator (BB Gen's breakbeat_t, tick mode) */
    uint8_t running, engine_bank, render_bank;
    int8_t pending_bank;              /* bank to switch to at the next bar, -1 = none */
    int current_slice;
    int bar;                          /* bar of the latest step */
    int trigger_count;                /* triggers since start or the last bank switch */
    float sp16;                       /* output samples per 16th, from the sequencer */
    float spt;                        /* output samples per trigger */
    int32_t trig_left;                /* samples to a pending mid-16th trigger (1/4 bar length) */
    uint8_t step_on;                  /* this step's enable bit */

    /* retrigger (BB Gen's sub-slice) */
    uint8_t sub_div, sub_count;
    int32_t sub_elapsed;              /* output samples since the trigger */
    float sub_len;                    /* output samples per sub-hit */

    /* gate: the step mask, ramped */
    float gate, gate_to;

    /* live keys: written by breaks_live, applied by the ISR side */
    volatile uint16_t keys;
    uint16_t keys_seen;
    brk_perf_t perf;

    /* status of the latest trigger (the UI's readout, and the tests' probe) */
    uint32_t st_trigs;                /* triggers fired since init */
    int8_t st_slice, st_bank, st_div, st_bp;
    uint8_t st_forced, rsv[3];
} breaks_t;

void breaks_init(breaks_t *b);
void breaks_set_loop(breaks_t *b, int which /*0 = A, 1 = B*/, const brk_loop_t *loop);   /* NULL = empty (silent) */
void breaks_step(breaks_t *b, int step16, int bar, float samples_per_16th, int enabled);
void breaks_stop(breaks_t *b);
void breaks_live(breaks_t *b, int key, int down);  /* 0-7 hold slice of A, 8 reverse, 9 half speed, 10 stutter */
void breaks_render(breaks_t *b, float *out, int n);  /* WRITES mono, n <= 256 */
int breaks_nparams(void);
const x0x_param_t *breaks_param(int i);
void breaks_set(breaks_t *b, int i, int value);
int breaks_get(const breaks_t *b, int i);
float breaks_send(const breaks_t *b, int which);
