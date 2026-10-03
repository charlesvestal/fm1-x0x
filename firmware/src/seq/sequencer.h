/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X sequencer: runs in the audio ISR, sample accurate.
 *
 * The engine renders a block like this:
 *     while (n) {
 *         k = seq_until_event(&seq, n);       // samples until the next due event (>= 1)
 *         render k samples;
 *         seq_advance(&seq, k, &sink);        // moves time, fires the events now due
 *         n -= k;
 *     }
 * so a step lands on its exact sample, and engine code never sees time.
 *
 * Five parts, each with its own step counter (lengths and rates may differ):
 * 909, 808, 303 A, 303 B, BREAK (always 16ths, 16 to the bar). The 909 part's length
 * defines the bar: a cued pattern (or the next one of a chain) starts when it wraps.
 *
 * Clock: internal (BPM) or external (24 ppqn MIDI clock, fed with seq_ext_clock()).
 * Swing delays every second 16th (909 shuffle: 0 = straight, 100 = 75 %).
 *
 * 303 semantics (as the TB-303 and Open303's sequencer):
 *   NOTE  plays; the gate falls at half the step, unless the step slides (BS_SLIDE)
 *         or the next step is a TIE, in which case it is held through.
 *   TIE   continues the sounding note (no retrigger); same gate rule.
 *   slide a NOTE following a step with BS_SLIDE glides from the held note
 *         (note_on with slide = 1, envelopes not retriggered).
 *   REST  releases.
 */
#pragma once
#include <stdint.h>
#include "pattern.h"

#define FS 44100u
#define TRK_DRUM 0                    /* + kit: 0 = 909, 1 = 808 */
#define TRK_BASS0 NKIT
#define TRK_BRK (NKIT + NBASS)
#define NTRACKS (NKIT + NBASS + 1)
/* mute bits: kit k voice v = k * NDRUM + v; then 303 A, 303 B, BREAK */
#define MUTE_BASS0 (NKIT * NDRUM)
#define MUTE_BRK (MUTE_BASS0 + NBASS)

typedef struct {
    void (*drum)(void *ctx, int kit, int voice, float vel);
    void (*bass_on)(void *ctx, int part, int note, int accent, int slide);
    void (*bass_off)(void *ctx, int part);
    void (*brk)(void *ctx, int step16, int bar, float samples_per_16th, int enabled);
    void (*brk_stop)(void *ctx);
    void (*midi)(void *ctx, uint8_t status, uint8_t d1, uint8_t d2);   /* clock / transport / notes out */
    void (*step)(void *ctx, int part, int step);                        /* UI playhead (optional) */
    void *ctx;
} seq_sink_t;

typedef struct {                      /* one part's runtime */
    uint8_t pos;                      /* current step */
    int8_t pp_dir;                    /* ping-pong direction */
    uint8_t started;                  /* the first step has fired since start */
    uint8_t sounding;                 /* 303: a note is held */
    uint8_t held_note;
    uint32_t to_next;                 /* samples to the next step (Q8 fraction) */
    uint32_t to_off;                  /* 303: samples to the gate falling, Q8; 0 = none */
    uint32_t rng;                     /* DIR_RANDOM and auto-mutate */
    uint16_t bars;                    /* wraps since the pattern started (auto-mutate) */
    uint8_t ext_ticks;                /* external clock: ticks into this step */
} seq_track_t;

typedef struct {
    pattern_t *pat;                   /* NPAT patterns (the project's) */
    volatile uint8_t playing;         /* 1 = running */
    volatile uint8_t cur, cue;        /* playing pattern; cued (0xFF = none) */
    volatile uint8_t chain_a, chain_b;/* chain range; a == b = no chain */
    volatile uint8_t ext;             /* 1 = following an external clock */
    volatile uint8_t send_clock;      /* 1 = emit MIDI clock + transport when internal */
    volatile uint8_t send_notes;      /* 1 = echo the sequence on MIDI (909 ch 10, 808 ch 11, 303s ch 2 / 3) */
    volatile uint32_t mute;           /* MUTE_* bits */
    volatile uint8_t accent_q7;       /* drum velocity of a non-accented hit, of 127 */
    float bpm;                        /* internal tempo (UI writes; read per step) */
    float ext_bpm;                    /* tempo measured from the external clock */
    seq_track_t t[NTRACKS];           /* TRK_DRUM, TRK_BASS0.., TRK_BRK */
    uint32_t clk_to_next;             /* internal: samples to the next MIDI clock, Q8 */
    uint32_t ext_since;               /* samples since the last external clock (tempo estimate) */
    uint32_t ext_period;              /* smoothed samples per external clock, Q8 */
    volatile uint32_t ext_pending;    /* external clocks received, not yet consumed */
    volatile uint8_t req_start, req_stop, req_cont; /* transport requests from the UI / MIDI */
    uint32_t mut_rng[NBASS];          /* auto-mutate random streams */
    uint8_t bass_notes_out[NBASS];    /* note echoed on MIDI, for the note off */
} seq_t;

void seq_init(seq_t *s, pattern_t *patterns);
uint32_t seq_until_event(seq_t *s, uint32_t n);
void seq_advance(seq_t *s, uint32_t k, const seq_sink_t *o);

/* main loop / MIDI side (all are flag writes picked up at the next block) */
void seq_start(seq_t *s);             /* from step 1 of the current (or cued) pattern */
void seq_stop(seq_t *s);
void seq_continue(seq_t *s);
void seq_ext_clock(seq_t *s);         /* one 24-ppqn tick (USB MIDI 0xF8) */
void seq_cue(seq_t *s, int pattern);  /* switch at the end of the bar (now, if stopped) */
void seq_chain(seq_t *s, int a, int b);

/* steps of one part's rate, in samples Q8 (no swing) at the current tempo */
uint32_t seq_step_q8(const seq_t *s, int rate);
float seq_tempo(const seq_t *s);      /* the tempo in effect (internal or measured) */
