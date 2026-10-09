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
 * 909, 808, 303 A, 303 B, BREAK (always 16ths, 16 to the bar). Each part plays its own
 * pattern (ppat[part]); track t is part t. The 909 part's length defines the bar: cued
 * patterns (or the next one of a chain, or the song's next bar) start when it wraps, and a
 * part whose pattern changed restarts on step 1 while the others keep running.
 *
 * Song: one entry per bar, each part's pattern and the part mutes. song_on plays it (and
 * loops); song_on + song_rec writes the live state (cues, mutes) into it from song_start on.
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

/* the mute bits of part p (a drum machine: all its voices) */
static inline uint32_t seq_part_mask(int p)
{
    return p < NKIT ? ((1u << NDRUM) - 1u) << (p * NDRUM) : 1u << (MUTE_BASS0 + p - NKIT);
}

#define NSONG 192
typedef struct {
    uint8_t pat[NTRACKS];             /* each part's pattern */
    uint8_t mute;                     /* bit p = part p muted */
} song_bar_t;
typedef struct {
    uint16_t len;                     /* bars; 0 = empty */
    uint8_t rsv[2];
    song_bar_t bar[NSONG];
} song_t;

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
    song_t *song;                     /* the project's song (may be 0) */
    volatile uint8_t playing;         /* 1 = running */
    volatile uint8_t ppat[NTRACKS];   /* the pattern each part plays */
    volatile uint8_t pcue[NTRACKS];   /* each part's cued pattern (0xFF = none) */
    volatile uint8_t chain_a, chain_b;/* chain range (whole patterns); a == b = no chain */
    volatile uint8_t song_on;         /* 1 = the song decides the patterns, bar by bar */
    volatile uint8_t song_rec;        /* with song_on: write the live state into the song */
    volatile uint16_t song_pos;       /* bar playing */
    volatile uint16_t song_start;     /* bar PLAY starts at */
    volatile uint8_t ext;             /* 1 = following an external clock */
    volatile uint8_t send_clock;      /* 1 = emit MIDI clock + transport when internal */
    volatile uint8_t send_notes;      /* 1 = echo the sequence on MIDI (909 ch 10, 808 ch 11, 303s ch 2 / 3) */
    volatile uint32_t mute;           /* MUTE_* bits */
    volatile uint32_t vmute;          /* drum tracks muted on their own (bit k * NDRUM + v): kept apart from
                                       * the part mutes, which the song and HOME set as a whole */
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
void seq_cue(seq_t *s, int pattern);  /* every part, at the end of the bar (now, if stopped) */
void seq_cue_part(seq_t *s, int part, int pattern);
void seq_chain(seq_t *s, int a, int b);
int seq_cue_of(const seq_t *s, int part);   /* the part's cued pattern, or -1 */
int seq_part_muted(const seq_t *s, int part);

/* steps of one part's rate, in samples Q8 (no swing) at the current tempo */
uint32_t seq_step_q8(const seq_t *s, int rate);
float seq_tempo(const seq_t *s);      /* the tempo in effect (internal or measured) */
int seq_part_len(const seq_t *s, int t);         /* steps in part t's pattern */
int seq_next_step(seq_t *s, int t, int p);       /* the step after p (direction; no commit) */
