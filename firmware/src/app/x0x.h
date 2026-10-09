/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X app: the project (everything that is saved) and the app-level entry points.
 * Included by the unity build after gfx.c, so drawing helpers are in scope. */
#pragma once
#include <stdint.h>
#include "plat.h"
#include "engine.h"
#include "../seq/pattern.h"
#include "../seq/sequencer.h"
#include "../seq/tb3po.h"
#include "../seq/motion.h"
#include "../dsp/breaks.h"
#include "../dsp/fxbus.h"            /* FX_* param indices (the send pages) */

#ifndef X0X_VERSION                     /* a release build passes its own (tools/build.py --release) */
#define X0X_VERSION "DEV"
#endif
#define PROJ_MAGIC 0x50305830u           /* "0X0P" */
#define PROJ_FORMAT 1u
#define PROJ_STEREO 2u
#define PROJ_SND_REV 1u                  /* 1: the 909 SD has a DECAY pot (index 3; LEVEL.. moved up one) */

typedef struct {
    uint32_t magic, format;
    uint8_t palette, keyled, clk_out, notes_out;   /* keyled: 0 off, 1 keys and the buttons' glow, 2 keys only */
    uint16_t bpm_x10;
    uint8_t accent_q7, keysound;         /* black drum keys sound: 0 = only when stopped or recording, 1 = always */
    uint8_t autosave_off;                /* 0: save by itself while stopped and idle (ui.c autosave) */
    uint8_t rsv_bright;                  /* was the screen's brightness (removed: always full); unused */
    uint8_t stereo;                      /* the pans the sound has: 0 none (saved mono), 1 the parts', 2 the
                                          * drum voices' too (PROJ_STEREO); a load centres what is missing */
    uint8_t snd_rev;                     /* the sound's layout (PROJ_SND_REV): 0 = before the 909 SD's DECAY */
    uint8_t cpu2;                        /* 1: the 909 and 808 render on the second core (GLO > 2ND CORE) */
    uint8_t rsv[3];                      /* room to grow: an older, shorter object loads (zeros here) */
} settings_t;

/* the song and the motion lanes: one blob, split across OBJ_SONG and OBJ_MOTION */
typedef struct {
    song_t song;
    lane_t lane[NLANE];
} arrange_t;

typedef struct {
    settings_t set;
    sound_t sound;
    pattern_t pat[NPAT];
    arrange_t arr;
} project_t;

extern project_t proj;

void project_defaults(void);
int project_load(void);                  /* 0 = loaded; else defaults are in place */
int project_save(void);                  /* 0 = saved and verified; -7 = too much to fit (nothing written) */
int project_mem_pct(void);               /* how full the room for patterns, song and motion is, % */

void ui_init(void);
void ui_frame(void);
void ui_input_only(void);
int ui_dirty(void);
void ui_say(const char *a, const char *b);
