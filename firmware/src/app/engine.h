/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X audio engine: the five parts, the mixer, the send FX and the master, driven by
 * the sequencer. engine_render() runs in the audio ISR; everything else is called
 * from the main loop and reaches the ISR only through the command queue, so the
 * engines' state is only ever touched by one context.
 *
 * Parameters are addressed as (target, voice, index) -> pot value, the same triple
 * the UI pages, the project file and the command queue use:
 *   T_909 / T_808   voice = track 0..10, or DR_KIT / D8_KIT (= 11) for the kit
 *   T_303           voice = 0 / 1 (303 A / B)
 *   T_BRK           voice 0: breaks.h params (the per-pattern ones live in the pattern)
 *   T_FX            voice 0: fxbus.h params (reverb, delay, master drive / comp)
 *   T_MIX           voice = part 0..4: Level, Rev send, Dly send (drums: on top of the per-voice sends)
 *   T_MST           voice 0: master filter + limiter
 */
#pragma once
#include <stdint.h>
#include "../dsp/x0x_param.h"
#include "../seq/sequencer.h"

enum { T_909, T_808, T_303, T_BRK, T_FX, T_MIX, T_MST, NTARGETS };
enum { PART_909, PART_808, PART_303A, PART_303B, PART_BRK, NPARTS };
#define NVOICES_MAX 12                 /* a drum machine: 11 tracks + the kit */
#define NPARAMS_MAX 16                 /* params per (target, voice) */

/* the sound of a project: every pot of every engine, mirrored here by the main loop */
typedef struct {
    uint8_t v[NTARGETS][NVOICES_MAX][NPARAMS_MAX];
} sound_t;

extern seq_t seq;                      /* the sequencer (its pattern pointer is the project's) */
extern volatile uint8_t eng_step[NTRACKS];   /* playheads for the UI */
extern volatile uint16_t eng_peak[NPARTS];   /* part meters, Q15, decaying (UI) */
#define SCOPE_N 256u
extern int16_t eng_scope[SCOPE_N];     /* the last output samples, for the screen */
extern volatile uint32_t eng_scope_w;

void engine_init(pattern_t *patterns);
void engine_render(int32_t *out_lr, uint32_t n);   /* ISR: n stereo frames, 24-bit in int32 */

/* parameter descriptors (UI) */
int engine_nvoices(int target);
int engine_nparams(int target, int voice);
const x0x_param_t *engine_param(int target, int voice, int i);
const char *engine_voice_name(int target, int voice);

/* main loop -> ISR */
void engine_set(int target, int voice, int i, int value);   /* queued; the sound_t mirror is the caller's */
void engine_apply_sound(const sound_t *s);                  /* queue every value (load, init) */
void engine_sound_defaults(sound_t *s);                     /* each engine's power-on values */
void engine_drum(int kit, int voice, float vel);            /* play now (keys) */
void engine_bass_on(int part, int note, int accent, int slide);
void engine_bass_off(int part);
void engine_brk_live(int key, int down);
void engine_brk_loops(void);                                /* (re)read the A / B loops of the current pattern */
int engine_brk_slice(void);                                 /* slice sounding now, -1 none (UI) */
int engine_brk_nslots(void);                                /* built-in loops + user slots */
const char *const *engine_brk_slot_names(void);
