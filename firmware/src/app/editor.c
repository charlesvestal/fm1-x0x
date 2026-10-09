/* SPDX-License-Identifier: GPL-3.0-only
 * Sample-slot commands adapted from Felucca's editor.c, Copyright (C) 2026 Leo Kuroshita
 * (@kurogedelic), Hugelton Instruments. */
/* X0X's subset of Felucca's editor protocol (web/EDITOR_PROTOCOL.md):
 *   F0 7D 46 4C cmd args.. F7
 * INFO (1), PING (25) and the sample-slot commands SMP_BEGIN .. SMP_INFO (11..15), so
 * tools/fm1_sample_upload.py loads break loops into the three slots unchanged. The
 * parameter / preset / track commands are Felucca's instrument and are not answered. */
#define ED_HDR0 0x7D
#define ED_HDR1 0x46
#define ED_HDR2 0x4C
enum { ED_INFO = 1, ED_SMP_BEGIN = 11, ED_SMP_WRITE, ED_SMP_END, ED_SMP_ERASE, ED_SMP_INFO, ED_PING = 25 };

static uint8_t ed_out[160];
static uint32_t ed_n;

static void ed_begin(uint32_t cmd)
{
    ed_out[0] = 0xF0;
    ed_out[1] = ED_HDR0;
    ed_out[2] = ED_HDR1;
    ed_out[3] = ED_HDR2;
    ed_out[4] = (uint8_t)cmd;
    ed_n = 5;
}
static void ed_b(uint32_t v)
{
    if (ed_n < sizeof ed_out - 1u)
        ed_out[ed_n++] = (uint8_t)(v & 0x7Fu);
}
static void ed_str(const char *s, uint32_t max)
{
    uint32_t i;
    for (i = 0; s && s[i] && i < max; i++)
        ed_b((uint8_t)s[i] & 0x7Fu);
    ed_b(0);
}
static void ed_send(void)
{
    ed_out[ed_n++] = 0xF7;
    ota_wire_send(ed_out, ed_n);
}

static uint32_t ed_unpack7(const uint8_t *a, uint32_t na, uint8_t *out, uint32_t max)
{
    uint32_t n = 0;
    while (na && n < max) {
        uint32_t m = *a++, j;
        na--;
        for (j = 0; j < 7u && na && n < max; j++, na--)
            out[n++] = (uint8_t)(*a++ | ((m >> j) & 1u) << 7);
    }
    return n;
}
static uint8_t ed_smp_buf[512] __attribute__((aligned(4)));
static uint32_t ed_smp_slot(uint32_t k) { return SMP_USER_BASE + k * SMP_USER_SIZE; }
static void ed_smp_inval(uint32_t k)
{
    fm1_irq_off();
    fm1_cpu1_park();                                  /* the second core out of the flash */
    fl_inval(ed_smp_slot(k), SMP_USER_SIZE);
    fm1_cpu1_unpark();
    fm1_irq_on();
}
static int ed_smp_erase(uint32_t k, uint32_t all)
{
    uint32_t i, took;
    int rc = 0;
    usr_nz[k] = 0;
    engine_brk_loops();                               /* nothing may read the slot now */
    for (i = 0; i < (all ? SMP_USER_SIZE / 0x1000u : 1u) && !rc; i++) {
        audio_silence();
        rc = fl_erase4k(ed_smp_slot(k) + i * 0x1000u, &took);
        fm1_wdt_feed();
    }
    ed_smp_inval(k);
    return rc;
}
static int ed_smp_end(uint32_t k, const uint8_t *a, uint32_t na)
{
    const smp_user_hdr_t *h = (const smp_user_hdr_t *)ed_smp_buf;
    if (ed_unpack7(a, na, ed_smp_buf, sizeof(smp_user_hdr_t)) != sizeof(smp_user_hdr_t))
        return 1;
    if (h->magic != SMP_USER_MAGIC || h->version != 1 || !h->nz || h->nz > 16u ||
        h->data_len > SMP_USER_SIZE - SMP_USER_DATA)
        return 2;
    ed_smp_inval(k);
    if (st_crc32(smp_user_xip(k) + SMP_USER_DATA, h->data_len) != h->crc)
        return 3;
    if (fl_write(ed_smp_slot(k), ed_smp_buf, sizeof(smp_user_hdr_t)))
        return 4;
    ed_smp_inval(k);
    smp_user_scan(k);
    engine_brk_loops();
    return usr_nz[k] ? 0 : 5;
}

#ifndef X0X_DEBUG
#define X0X_DEBUG 0
#endif
#if X0X_DEBUG
/* development builds only (X0X_DEBUG=1; tools/build.py refuses it in a release): the clock work
 * reads and writes the chip's registers live over USB.
 *   PEEK  40 addr n        -> addr, n words      (n <= 16)
 *   POKE  41 addr value    -> addr, old, new
 *   CLOCK 42 iterations    -> 24 MHz ticks a fixed loop took (IRQs off), C0_TL_CKCNT before, after
 * A number is 5 x 7 bits, least significant first. Only these ranges, word aligned: an address
 * outside them gets no reply (a stray read of a gated block could fault). */
enum { ED_PEEK = 40, ED_POKE, ED_CLOCK };
#include "fm1_debug.h"
static uint32_t ed_u35(const uint8_t *a)
{
    return (uint32_t)a[0] | (uint32_t)a[1] << 7 | (uint32_t)a[2] << 14 | (uint32_t)a[3] << 21 | (uint32_t)a[4] << 28;
}
static void ed_w35(uint32_t v)
{
    uint32_t k;
    for (k = 0; k < 5u; k++)
        ed_b(v >> (7u * k));
}
static int ed_debug(uint32_t cmd, const uint8_t *a, uint32_t na)
{
    uint32_t addr, i;
    if (cmd == ED_CLOCK) {
        uint32_t it, c0, c1;
        if (na < 5u)
            return 0;
        it = ed_u35(a);
        if (!it || it > 50000000u)
            return 0;
        ed_w35(fm1_dbg_clock(it, &c0, &c1));
        ed_w35(c0);
        ed_w35(c1);
        return 1;
    }
    if (na < 10u)
        return 0;
    addr = ed_u35(a);
    if (cmd == ED_PEEK) {
        uint32_t n = ed_u35(a + 5);
        if (!n || n > 16u || !fm1_dbg_ok(addr, n))
            return 0;
        ed_w35(addr);
        for (i = 0; i < n; i++)
            ed_w35(fm1_dbg_rd(addr + 4u * i));
        return 1;
    }
    if (!fm1_dbg_ok(addr, 1))
        return 0;
    ed_w35(addr);
    ed_w35(fm1_dbg_rd(addr));
    fm1_dbg_wr(addr, ed_u35(a + 5));
    ed_w35(fm1_dbg_rd(addr));
    return 1;
}
#endif

static void ed_handle(const uint8_t *f, uint32_t n)
{
    uint32_t cmd = f[3], i;
    const uint8_t *a = f + 4;
    uint32_t na = n - 4u;
    ed_begin(cmd);
    switch (cmd) {
    case ED_INFO:
        ed_str("X0X " X0X_VERSION, 24);
        ed_b(0);                                      /* no engines / parameters over this protocol */
        ed_b(0);
        ed_b(0);
        ed_b(NSTEPS);
        ed_b(0);
        ed_b(0);
        break;
    case ED_PING:
        ed_b(0);
        break;
    case ED_SMP_BEGIN:
    case ED_SMP_ERASE:
        if (na < 1u || a[0] >= SMP_USER_SLOTS || !flash_ok)
            return;
        ed_b(a[0]);
        ed_b(ed_smp_erase(a[0], cmd == ED_SMP_ERASE) ? 1u : 0u);
        break;
    case ED_SMP_WRITE: {
        uint32_t off, len, rc = 0, took;
        if (na < 5u || a[0] >= SMP_USER_SLOTS || !flash_ok)
            return;
        off = (uint32_t)a[1] | (uint32_t)a[2] << 7 | (uint32_t)a[3] << 14;
        len = ed_unpack7(a + 4, na - 4u, ed_smp_buf, 256u);
        if (off < SMP_USER_DATA || (off & 0xFFu) || !len || off + len > SMP_USER_SIZE)
            rc = 1;
        else if (usr_nz[a[0]])
            rc = 4;
        else {
            if (!(off & 0xFFFu)) {
                audio_silence();
                rc = fl_erase4k(ed_smp_slot(a[0]) + off, &took) ? 2u : 0u;
            }
            if (!rc && fl_write(ed_smp_slot(a[0]) + off, ed_smp_buf, len))
                rc = 3;
        }
        ed_b(a[0]);
        ed_b(off);
        ed_b(off >> 7);
        ed_b(off >> 14);
        ed_b(rc);
        break;
    }
    case ED_SMP_END:
        if (na < 2u || a[0] >= SMP_USER_SLOTS || !flash_ok)
            return;
        ed_b(a[0]);
        ed_b((uint32_t)ed_smp_end(a[0], a + 1, na - 1u));
        break;
#if X0X_DEBUG
    case ED_PEEK:
    case ED_POKE:
    case ED_CLOCK:
        if (!ed_debug(cmd, a, na))
            return;
        break;
#endif
    case ED_SMP_INFO:
        ed_b(SMP_USER_SLOTS);
        ed_b(SMP_USER_SIZE / 1024u);
        for (i = 0; i < SMP_USER_SLOTS; i++) {
            const smp_user_hdr_t *h = (const smp_user_hdr_t *)smp_user_xip(i);
            char nm[9] = {0};
            uint32_t j;
            ed_b(usr_nz[i]);
            for (j = 0; usr_nz[i] && j < 8u; j++)
                nm[j] = h->name[j] >= 32 && h->name[j] < 127 ? h->name[j] : 0;
            ed_str(nm, 8);
            ed_b(usr_nz[i] ? (h->data_len + 1023u) / 1024u : 0u);
        }
        break;
    default:
        return;                                       /* not ours: no reply */
    }
    ed_send();
}

static void ed_service(void)
{
    const uint8_t *p;
    uint32_t n;
    if (!ota_frame_get(&p, &n) || n < 4u || p[0] != ED_HDR0 || p[1] != ED_HDR1 || p[2] != ED_HDR2)
        return;                                       /* not an editor frame: ota_service() reads it */
    if (p[3] >= ED_SMP_BEGIN && p[3] <= ED_SMP_INFO) {
        ed_handle(p, n);
        ota_frame_done();
        return;
    }
    {
        static uint8_t f[64];
        uint32_t k = n > sizeof f ? sizeof f : n, i;
        for (i = 0; i < k; i++)
            f[i] = p[i];
        ota_frame_done();
        ed_handle(f, k);
    }
}
