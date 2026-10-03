/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X pattern model: 16 patterns, each holding the 909, the 808, two 303s and the break.
 *
 * Plain data, no pointers, so a pattern copies with an assignment and the project
 * stores the bytes. The UI edits these in place from the main loop while the
 * sequencer reads them in the audio ISR: every field the sequencer reads is a
 * single byte or an aligned 32-bit word, so a read never sees half a write. */
#pragma once
#include <stdint.h>

#define NPAT 16
#define NSTEPS 32
#define NDRUM 11                     /* tracks per drum machine: the 11 black keys */
#define NKIT 2                       /* 0 = 909 (drum909.h order), 1 = 808 (drum808.h order) */
#define NBASS 2                      /* 303 A, 303 B */

enum { RATE_16, RATE_16T, RATE_32, RATE_8T, NRATES };      /* step length */
enum { DIR_FWD, DIR_REV, DIR_PINGPONG, DIR_RANDOM, NDIRS };  /* 303 playback direction */
enum { G_REST, G_NOTE, G_TIE };                             /* 303 step gate */

/* 303 step: one byte of note, one of flags */
#define BS_GATE_MASK 0x03u
#define BS_ACCENT 0x04u
#define BS_SLIDE 0x08u                /* slide INTO the next step (303 semantics) */
typedef struct {
    uint8_t note;                     /* MIDI note */
    uint8_t flags;                    /* gate | BS_ACCENT | BS_SLIDE */
} bstep_t;

/* TB-3PO settings of one 303 line: the line is reproducible from these and the seed */
typedef struct {
    uint8_t density, accent, slide;   /* probabilities 0..100 % */
    uint8_t oct_range;                /* 1..3 */
    uint8_t root;                     /* 0 = C .. 11 = B */
    uint8_t scale;                    /* tb3po.h SCALES */
    uint8_t base_oct;                 /* octave of the root: note = 12 * (base_oct + 1) + root */
    uint8_t mutate_bars;              /* auto-mutate every N bars, 0 = off */
    uint32_t seed;
} tb3po_cfg_t;

typedef struct {
    bstep_t step[NSTEPS];
    uint8_t len;                      /* 1..32 */
    uint8_t rate;                     /* RATE_* */
    uint8_t dir;                      /* DIR_* */
    uint8_t transpose;                /* semitones + 24 (24 = none), applied when played */
    tb3po_cfg_t gen;
} bpart_t;

typedef struct {
    uint32_t hit[NDRUM];              /* bit s = instrument plays on step s */
    uint32_t accent;                  /* the 909's total-accent row */
    uint8_t len;                      /* 1..32 */
    uint8_t rate;
    uint8_t rsv[2];
} dpart_t;

/* BREAK: the generator's settings (dsp/breaks.h) and the steps it may play on */
enum { BRK_COMPLEX, BRK_ANCHOR, BRK_ROLL, BRK_FILL, BRK_R2, BRK_R3, BRK_R4, BRK_R8,
       BRK_PHRASE, BRK_BCHANCE, BRK_ALEN, BRK_BLEN, BRK_NSET };
typedef struct {
    uint32_t steps;                   /* bit s = the break may sound on 16th s of the bar */
    uint8_t set[BRK_NSET];            /* breaks.h param values, in BRK_* order */
    uint8_t slot_a, slot_b;           /* sample slot of loop A / B (0 = built-in) */
    uint8_t rsv[2];
} brkpart_t;

typedef struct {
    dpart_t drum[NKIT];
    bpart_t bass[NBASS];
    brkpart_t brk;
    uint8_t swing;                    /* 0..100: 0 = straight (50 %), 100 = 75 % shuffle */
    uint8_t rsv[3];
} pattern_t;

static inline int bstep_gate(const bstep_t *s) { return s->flags & BS_GATE_MASK; }

/* default empty pattern: 16 steps, straight, 303 lines empty with TB-3PO defaults */
void pattern_init(pattern_t *p, uint32_t seed_a, uint32_t seed_b);
