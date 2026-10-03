/* SPDX-License-Identifier: GPL-3.0-only
 * User sample slots: adapted from Felucca's eng_sample.c, Copyright (C) 2026 Leo Kuroshita
 * (@kurogedelic), Hugelton Instruments. */
/* User sample slots (break loops), uploaded over USB with tools/fm1_sample_upload.py
 * (or the web editor's sample page): 3 slots of 80 KiB at flash 0xA0000.., read in place
 * through the XIP window. Slot = header (magic, count, name, data length, CRC32) + up to
 * 16 zones + IMA ADPCM data at +512. X0X plays zone 0 of a slot as a loop. */
typedef struct {
    uint32_t off, n, ls, le;     /* byte offset (from the slot data), samples, loop start / end */
    uint32_t rate;               /* source rate / 44100, Q16 */
    int16_t root16;
    int16_t pred;
    uint8_t idx, lo, hi, looped;
} smp_zone_t;
#define SMP_USER_SLOTS 3
#define SMP_USER_BASE 0xA0000u
#define SMP_USER_SIZE 0x14000u
#define SMP_USER_DATA 512u
#define SMP_USER_MAGIC 0x504D5346u                  /* "FSMP" */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t nz, rsv;
    char name[8];
    uint32_t data_len, crc, rsv2[2];
    smp_zone_t zone[16];
} smp_user_hdr_t;                                   /* 32 + 16 x 28 = 480 B, data at +512 */
static uint8_t usr_nz[SMP_USER_SLOTS];

#ifndef SMP_USER_XIP                                /* host: a RAM image of the slots */
#define SMP_USER_XIP(k) fm1_xip_ptr(SMP_USER_BASE + (k) * SMP_USER_SIZE)
#endif
static const uint8_t *smp_user_xip(uint32_t k) { return SMP_USER_XIP(k); }

/* (re)read slot k: a valid header with sane zones makes it usable */
static void smp_user_scan(uint32_t k)
{
    const smp_user_hdr_t *h = (const smp_user_hdr_t *)smp_user_xip(k);
    uint32_t i;
    usr_nz[k] = 0;
    if (h->magic != SMP_USER_MAGIC || h->version != 1 || !h->nz || h->nz > 16u ||
        h->data_len > SMP_USER_SIZE - SMP_USER_DATA)
        return;
    for (i = 0; i < h->nz; i++) {
        const smp_zone_t *z = &h->zone[i];
        if (z->off > h->data_len || z->n > 2u * SMP_USER_SIZE || z->off + (z->n + 1u) / 2u > h->data_len ||
            z->idx > 88u || !z->rate || z->rate > 4u << 16)
            return;
    }
    usr_nz[k] = h->nz;
}
