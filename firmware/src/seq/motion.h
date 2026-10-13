/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X motion: recorded knob moves, one lane per knob per part pattern.
 *
 * A lane holds a value for each step of one part's pattern (MOT_NONE = the knob's own
 * value, "the base"). Lanes live in a pool the project saves; the runtime beside them
 * (what playback last applied, the ramp, the record / hold counters) belongs to the
 * audio ISR alone.
 *
 * Recording writes the steps the knob MOVED on (each turn while recording raises
 * motion_req_rec(); the next step takes the knob's value). A gap of up to MOT_GAP steps
 * between two written steps is filled in a straight line, so a slow sweep has no holes; the
 * steps you did not touch keep what they had. While a gesture lasts (a turn within MOT_GAP
 * steps) the knob, not the lane, is heard.
 *
 * ISR:  motion_step() on every step a part fires, motion_tick() once per block (the ramps
 *       between two set steps), motion_release() when the transport stops.
 * Main: motion_find() / motion_alloc() / motion_clear(), and the two requests
 *       motion_req_rec() (the knob moved, recording) and motion_req_hold() (the knob moved,
 *       not recording: it wins for a pass). Allocation writes the fields first and `used`
 *       last; `gen` tells the ISR a slot was reused, so it resets that slot's runtime. */
#pragma once
#include <stdint.h>
#include "pattern.h"

#define NLANE 160
#define MOT_NONE 0xFFu
#define MOT_PARTS 5
#define MOT_GAP 4                     /* steps: a longer pause ends a recording gesture */

typedef struct {
    uint8_t used, gen;
    uint8_t pat, part;                /* the part pattern it belongs to */
    uint8_t t, v, i;                  /* the parameter (engine.h target, voice, index) */
    uint8_t rsv;
    uint8_t val[NSTEPS];              /* per step, MOT_NONE = the base */
} lane_t;

typedef struct {
    uint8_t gen;                      /* lane gen this runtime belongs to */
    uint8_t applied;                  /* value playback set, MOT_NONE = the engine has the base / knob */
    uint8_t from, to, ramp;           /* the ramp across the current step */
    uint8_t part_parity;              /* parity of the step the ramp started on */
    volatile uint8_t rec_req, hold_req;
    uint8_t rec_last;                 /* the step a gesture last wrote, MOT_NONE = no gesture */
    uint8_t rec_quiet;                /* steps since */
    uint8_t hold_left;                /* steps the knob still wins */
} lane_rt_t;

typedef struct {
    lane_t *lane;                     /* NLANE, the project's */
    lane_rt_t rt[NLANE];
    uint32_t age[MOT_PARTS];          /* samples since the part's last step */
    uint32_t len[MOT_PARTS][2];       /* the length of its last even / odd step (swing) */
    uint8_t last[MOT_PARTS];          /* its last step */
    int (*base)(void *ctx, int t, int v, int i);
    void (*apply)(void *ctx, int t, int v, int i, int val);
    void *ctx;
} motion_t;

void motion_init(motion_t *m, lane_t *lanes);
void motion_step(motion_t *m, int part, int pat, int step, int next, int pass);   /* ISR */
void motion_tick(motion_t *m, uint32_t n);                                       /* ISR */
void motion_release(motion_t *m);                                                /* ISR */
int motion_value(const motion_t *m, int k);      /* what lane k is applying now, -1 = nothing */

/* main loop */
int motion_find(const lane_t *L, int pat, int part, int t, int v, int i);   /* lane index, -1 */
int motion_alloc(lane_t *L, int pat, int part, int t, int v, int i);       /* -1 = pool full */
void motion_clear(lane_t *L, int k);
int motion_count(const lane_t *L);
void motion_req_rec(motion_t *m, int k);
void motion_req_hold(motion_t *m, int k);
