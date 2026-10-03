/* SPDX-License-Identifier: GPL-3.0-only */
/* TB-3PO: generative 303 lines. A port of schwung-tb3po's generator (itself a port
 * of the Phazerville Hemisphere Suite TB_3PO applet, (c) djphazer and contributors,
 * GPL-3.0). The random draws happen in the same order as schwung-tb3po's
 * generate_pattern / mutate_pattern, so a seed and settings give the same line on
 * both. */
#pragma once
#include "pattern.h"

#define TB3PO_NSCALES 6
extern const char *const TB3PO_SCALE_NAMES[TB3PO_NSCALES];

void tb3po_defaults(tb3po_cfg_t *g, uint32_t seed);
/* write a whole line (len steps) from g (g->seed) */
void tb3po_generate(bpart_t *b);
/* re-roll ~25 % of the steps, continuing the line's random stream from *rng
 * (pass the part's running state; 0 = start from the seed) */
void tb3po_mutate(bpart_t *b, uint32_t *rng);
/* a new seed from any entropy (time, key presses) */
uint32_t tb3po_new_seed(uint32_t entropy);
