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
#include "../dsp/breaks.h"

#define X0X_VERSION "0.1"
#define PROJ_MAGIC 0x50305830u           /* "0X0P" */
#define PROJ_FORMAT 1u

typedef struct {
    uint32_t magic, format;
    uint8_t palette, keyled, clk_out, notes_out;
    uint16_t bpm_x10;
    uint8_t accent_q7, rsv;
} settings_t;

typedef struct {
    settings_t set;
    sound_t sound;
    pattern_t pat[NPAT];
} project_t;

extern project_t proj;

void project_defaults(void);
int project_load(void);                  /* 0 = loaded; else defaults are in place */
int project_save(void);                  /* 0 = saved and verified */

void ui_init(void);
void ui_frame(void);
void ui_input_only(void);
int ui_dirty(void);
void ui_say(const char *a, const char *b);
