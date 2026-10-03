/* SPDX-License-Identifier: GPL-3.0-only */
/* Thin wrapper around the reference 9W9 engine (built from 9W9's own sources by
 * run_drum909.sh, with libm and doubles), so drum909_test.c never includes a
 * reference header and builds under the port's warning flags. */
#include <stdlib.h>
#include "er99_engine.h"

void *ref_new(const char *module_dir)
{
    er99_engine_t *e = (er99_engine_t *)calloc(1, sizeof(er99_engine_t));
    er99_engine_init(e, 44100.0f, module_dir);
    return e;
}

void ref_free(void *e)
{
    er99_engine_free((er99_engine_t *)e);
    free(e);
}

int ref_set(void *e, const char *key, float pot) { return er99_engine_set_param((er99_engine_t *)e, key, pot); }

int ref_get(void *e, const char *key, float *pot) { return er99_engine_get_param((er99_engine_t *)e, key, pot); }

int ref_set_raw(void *e, const char *key, float v) { return er99_engine_set_raw((er99_engine_t *)e, key, v); }

void ref_trigger(void *e, int which, int vel) { er99_engine_trigger((er99_engine_t *)e, (er99_trigger_t)which, vel); }

void ref_render(void *e, float *out, int n) { er99_engine_render((er99_engine_t *)e, out, n); }

/* er99_pot_to_value for a pot key; -1e30 if unknown */
float ref_pot_value(const char *key, int pot)
{
    for (int i = 0; i < ER99_POT_COUNT; ++i)
        if (!strcmp(g_er99_pots[i].key, key))
            return er99_pot_to_value(&g_er99_pots[i], pot);
    return -1e30f;
}

/* the reference noise generator's first n values */
void ref_noise(float *out, int n)
{
    wa_noise_t z;
    wa_noise_init(&z, 0xC0FFEEu);
    for (int i = 0; i < n; ++i)
        out[i] = wa_noise_tick(&z);
}
