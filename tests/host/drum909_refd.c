/* SPDX-License-Identifier: GPL-3.0-only */
/* A second copy of the reference 9W9 engine whose biquads keep their state and
 * arithmetic in double (run_drum909.sh patches a copy of webaudio.h for it).
 * Comparing 9W9 against this copy measures 9W9's own float rounding noise -- the
 * floor under any port that is not bit-identical. Every external symbol is
 * renamed so both copies link into one test. */
#define er99_trigger_names refd_er99_trigger_names
#define er99_engine_init refd_er99_engine_init
#define er99_engine_free refd_er99_engine_free
#define er99_engine_trigger refd_er99_engine_trigger
#define er99_engine_render refd_er99_engine_render
#define er99_engine_set_raw refd_er99_engine_set_raw
#define er99_engine_get_raw refd_er99_engine_get_raw
#define er99_engine_seed_pots refd_er99_engine_seed_pots
#define er99_engine_set_param refd_er99_engine_set_param
#define er99_engine_get_param refd_er99_engine_get_param
#define er99_engine_get_state refd_er99_engine_get_state
#define er99_engine_set_state refd_er99_engine_set_state
#define ref_new refd_new
#define ref_free refd_free
#define ref_set refd_set
#define ref_get refd_get
#define ref_set_raw refd_set_raw
#define ref_trigger refd_trigger
#define ref_render refd_render
#define ref_pot_value refd_pot_value
#define ref_noise refd_noise
#include "er99_engine.c"
#include "drum909_ref.c"
