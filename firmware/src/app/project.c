/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X project: the settings, the sound, the 16 patterns, the song and the motion lanes, in six
 * storage objects.
 * Every object carries the format number; a load that finds another format keeps the
 * defaults for that object rather than guessing at the bytes. */
#include "x0x.h"

project_t proj;

#define PAT_PER_OBJ (NPAT / 2)

static void arrange_defaults(void);

void project_defaults(void)
{
    int i;
    proj.set.magic = PROJ_MAGIC;
    proj.set.format = PROJ_FORMAT;
    proj.set.palette = 1;                /* amber */
    proj.set.keyled = 1;
    proj.set.keysound = 0;
    proj.set.autosave_off = 0;
    proj.set.clk_out = 1;
    proj.set.notes_out = 0;
    proj.set.bpm_x10 = 1250;
    proj.set.accent_q7 = 88;
    proj.set.stereo = 1;
    engine_sound_defaults(&proj.sound);
    arrange_defaults();
    /* the 303 lines start empty, like the drums: OCT+ on TB-3PO (or keyboard mode) writes one */
    for (i = 0; i < NPAT; i++)
        pattern_init(&proj.pat[i], 0x3B0u + (uint32_t)i * 977u, 0x5A1u + (uint32_t)i * 613u);
}

/* object payload: format word + body */
static uint8_t io_buf[PLAT_STORE_MAX];

/* what each object last held on the flash (a hash of its payload), so a save writes only the
 * objects that changed: an autosave usually erases one or two sectors, not six */
static uint32_t stored_hash[OBJ_NOBJ];
static uint8_t stored_known[OBJ_NOBJ];
static uint32_t hash_of(const uint8_t *p, uint32_t n)        /* FNV-1a */
{
    uint32_t h = 2166136261u;
    while (n--)
        h = (h ^ *p++) * 16777619u;
    return h;
}


static int load_obj(uint32_t obj, void *dst, uint32_t len)
{
    int n = plat_store_load(obj, io_buf, sizeof io_buf);
    uint32_t i, fmt;
    if (n < 0 || (uint32_t)n != len + 4u)
        return -1;
    fmt = (uint32_t)io_buf[0] | (uint32_t)io_buf[1] << 8 | (uint32_t)io_buf[2] << 16 | (uint32_t)io_buf[3] << 24;
    if (fmt != PROJ_FORMAT)
        return -1;
    for (i = 0; i < len; i++)
        ((uint8_t *)dst)[i] = io_buf[4u + i];
    stored_known[obj] = 1;
    stored_hash[obj] = hash_of(io_buf, len + 4u);
    return 0;
}

/* the settings may have grown since they were saved: a shorter payload of this format loads into
 * the front of dst, and the rest keeps what dst held (zeros: the new settings' defaults) */
static int load_settings(settings_t *dst)
{
    int n = plat_store_load(OBJ_SET, io_buf, sizeof io_buf);
    uint32_t i;
    if (n < 20 || (uint32_t)n > sizeof *dst + 4u || io_buf[0] != (uint8_t)PROJ_FORMAT || io_buf[1] || io_buf[2] || io_buf[3])
        return -1;
    for (i = 0; i + 4u < (uint32_t)n; i++)
        ((uint8_t *)dst)[i] = io_buf[4u + i];
    if ((uint32_t)n == sizeof *dst + 4u) {           /* exactly what a save of these settings writes */
        stored_known[OBJ_SET] = 1;
        stored_hash[OBJ_SET] = hash_of(io_buf, (uint32_t)n);
    }
    return 0;
}

static int save_obj(uint32_t obj, const void *src, uint32_t len)
{
    uint32_t i, h;
    int rc;
    if (len + 4u > sizeof io_buf)
        return -1;
    io_buf[0] = (uint8_t)PROJ_FORMAT;
    io_buf[1] = io_buf[2] = io_buf[3] = 0;
    for (i = 0; i < len; i++)
        io_buf[4u + i] = ((const uint8_t *)src)[i];
    h = hash_of(io_buf, len + 4u);
    if (stored_known[obj] && stored_hash[obj] == h)
        return 0;                                    /* the flash already holds exactly this */
    rc = plat_store_save(obj, io_buf, len + 4u);
    stored_known[obj] = rc == 0;
    stored_hash[obj] = h;
    return rc;
}

typedef char pat_fits[(sizeof(pattern_t) * PAT_PER_OBJ + 4u <= PLAT_STORE_MAX) ? 1 : -1];
typedef char sound_fits[(sizeof(sound_t) + 4u <= PLAT_STORE_MAX) ? 1 : -1];
#define ARR_A (PLAT_STORE_MAX - 4u)                   /* arrange_t bytes in OBJ_SONG; the rest in OBJ_MOTION */
typedef char arr_fits[(sizeof(arrange_t) <= 2u * ARR_A) ? 1 : -1];

static void arrange_defaults(void)
{
    uint32_t i;
    uint8_t *z = (uint8_t *)&proj.arr;
    for (i = 0; i < sizeof proj.arr; i++)
        z[i] = 0;
}

/* each object is read straight into its place in proj: load_obj copies only a valid payload */
int project_load(void)
{
    int bad = 0, i;
    settings_t set;
    project_defaults();
    set = proj.set;
    if (load_settings(&set) == 0 && set.magic == PROJ_MAGIC)
        proj.set = set;
    else
        bad = 1;
    if (load_obj(OBJ_SOUND, &proj.sound, sizeof proj.sound))
        bad = 1;
    if (!proj.set.stereo) {                           /* saved mono: no pans there (zeros = hard left) */
        for (i = 0; i < NPARTS; i++)
            proj.sound.v[T_MIX][i][MX_PAN] = 64;
        proj.set.stereo = 1;
    }
    if (load_obj(OBJ_PAT0, &proj.pat[0], sizeof(pattern_t) * PAT_PER_OBJ) ||
        load_obj(OBJ_PAT1, &proj.pat[PAT_PER_OBJ], sizeof(pattern_t) * PAT_PER_OBJ)) {
        for (i = 0; i < NPAT; i++) {                  /* half a set is no set: the defaults */
            pattern_init(&proj.pat[i], 0x3B0u + (uint32_t)i * 977u, 0x5A1u + (uint32_t)i * 613u);
        }
        bad = 1;
    }
    /* the song and the lanes: absent in a project saved before they existed, which is no fault */
    if (load_obj(OBJ_SONG, &proj.arr, ARR_A) ||
        load_obj(OBJ_MOTION, (uint8_t *)&proj.arr + ARR_A, sizeof proj.arr - ARR_A))
        arrange_defaults();
    if (proj.arr.song.len > NSONG)
        proj.arr.song.len = NSONG;
    return bad ? -1 : 0;
}

int project_save(void)
{
    proj.set.bpm_x10 = (uint16_t)(seq.bpm * 10.0f + 0.5f);
    proj.set.accent_q7 = seq.accent_q7;
    proj.set.clk_out = seq.send_clock;
    proj.set.notes_out = seq.send_notes;
    if (save_obj(OBJ_SET, &proj.set, sizeof proj.set))
        return -1;
    if (save_obj(OBJ_SOUND, &proj.sound, sizeof proj.sound))
        return -2;
    if (save_obj(OBJ_PAT0, &proj.pat[0], sizeof(pattern_t) * PAT_PER_OBJ))
        return -3;
    if (save_obj(OBJ_PAT1, &proj.pat[PAT_PER_OBJ], sizeof(pattern_t) * PAT_PER_OBJ))
        return -4;
    if (save_obj(OBJ_SONG, &proj.arr, ARR_A))
        return -5;
    if (save_obj(OBJ_MOTION, (const uint8_t *)&proj.arr + ARR_A, sizeof proj.arr - ARR_A))
        return -6;
    return 0;
}
