/* SPDX-License-Identifier: GPL-3.0-only */
/* X0X project: the settings, the sound and the 16 patterns, in four storage objects.
 * Every object carries the format number; a load that finds another format keeps the
 * defaults for that object rather than guessing at the bytes. */
#include "x0x.h"

project_t proj;

#define PAT_PER_OBJ (NPAT / 2)

void project_defaults(void)
{
    int i;
    proj.set.magic = PROJ_MAGIC;
    proj.set.format = PROJ_FORMAT;
    proj.set.palette = 1;                /* amber */
    proj.set.keyled = 1;
    proj.set.clk_out = 1;
    proj.set.notes_out = 0;
    proj.set.bpm_x10 = 1250;
    proj.set.accent_q7 = 88;
    engine_sound_defaults(&proj.sound);
    for (i = 0; i < NPAT; i++) {
        pattern_init(&proj.pat[i], 0x3B0u + (uint32_t)i * 977u, 0x5A1u + (uint32_t)i * 613u);
        tb3po_generate(&proj.pat[i].bass[0]);
        tb3po_generate(&proj.pat[i].bass[1]);
    }
}

/* object payload: format word + body */
static uint8_t io_buf[PLAT_STORE_MAX];

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
    return 0;
}

static int save_obj(uint32_t obj, const void *src, uint32_t len)
{
    uint32_t i;
    if (len + 4u > sizeof io_buf)
        return -1;
    io_buf[0] = (uint8_t)PROJ_FORMAT;
    io_buf[1] = io_buf[2] = io_buf[3] = 0;
    for (i = 0; i < len; i++)
        io_buf[4u + i] = ((const uint8_t *)src)[i];
    return plat_store_save(obj, io_buf, len + 4u);
}

typedef char pat_fits[(sizeof(pattern_t) * PAT_PER_OBJ + 4u <= PLAT_STORE_MAX) ? 1 : -1];
typedef char sound_fits[(sizeof(sound_t) + 4u <= PLAT_STORE_MAX) ? 1 : -1];

int project_load(void)
{
    int bad = 0;
    project_t tmp;
    project_defaults();
    tmp = proj;
    if (load_obj(OBJ_SET, &tmp.set, sizeof tmp.set) == 0 && tmp.set.magic == PROJ_MAGIC)
        proj.set = tmp.set;
    else
        bad = 1;
    if (load_obj(OBJ_SOUND, &tmp.sound, sizeof tmp.sound) == 0)
        proj.sound = tmp.sound;
    else
        bad = 1;
    if (load_obj(OBJ_PAT0, &tmp.pat[0], sizeof(pattern_t) * PAT_PER_OBJ) == 0 &&
        load_obj(OBJ_PAT1, &tmp.pat[PAT_PER_OBJ], sizeof(pattern_t) * PAT_PER_OBJ) == 0) {
        int i;
        for (i = 0; i < NPAT; i++)
            proj.pat[i] = tmp.pat[i];
    } else
        bad = 1;
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
    return 0;
}
