/* SPDX-License-Identifier: GPL-3.0-only */
/* The TRS MIDI IN parser (midi_uart.c): bytes as the UART DMA leaves them in, USB-MIDI packets out,
 * into the queue USB fills. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#include "usb.c"
#include "midi_uart.c"

static int fails;
static void feed(const uint8_t *b, uint32_t n)
{
    uint32_t i, w = (um.rd + um.pend) & (UM_RING - 1u);
    for (i = 0; i < n; i++)
        um_ring[(w + i) & (UM_RING - 1u)] = b[i];
    uart_midi_take(n);
}
static void want(const char *what, const uint32_t *pk, uint32_t n)
{
    uint32_t i, got = mi_w - mi_r;
    int ok = got == n;
    for (i = 0; ok && i < n; i++)
        ok = midi_in_q[(mi_r + i) % MQ] == pk[i];
    printf("  %s %s (%u packets)\n", ok ? "ok  " : "FAIL", what, got);
    if (!ok) {
        fails++;
        for (i = 0; i < got; i++)
            printf("       got %08X\n", midi_in_q[(mi_r + i) % MQ]);
    }
    mi_r = mi_w;
}
#define PK(s, a, b) ((uint32_t)((s) >> 4) | (uint32_t)(s) << 8 | (uint32_t)(a) << 16 | (uint32_t)(b) << 24)
#define RT(s) (0xFu | (uint32_t)(s) << 8)

int main(void)
{
    {   /* a note on, then two more by running status, a clock in the middle of one */
        static const uint8_t b[] = {0x91, 60, 100, 62, 0xF8, 90, 64, 0};
        static const uint32_t p[] = {PK(0x91, 60, 100), RT(0xF8), PK(0x91, 62, 90), PK(0x91, 64, 0)};
        feed(b, sizeof b);
        want("running status and a clock inside a message", p, 4);
    }
    {   /* program change and channel pressure have one data byte */
        static const uint8_t b[] = {0xC2, 5, 7, 0xD2, 40};
        static const uint32_t p[] = {PK(0xC2, 5, 0), PK(0xC2, 7, 0), PK(0xD2, 40, 0)};
        feed(b, sizeof b);
        want("one-byte messages", p, 3);
    }
    {   /* transport; active sensing ignored */
        static const uint8_t b[] = {0xFA, 0xFE, 0xFB, 0xFC};
        static const uint32_t p[] = {RT(0xFA), RT(0xFB), RT(0xFC)};
        feed(b, sizeof b);
        want("start, continue, stop (active sensing ignored)", p, 3);
    }
    {   /* SysEx is skipped and ends running status; a clock inside it still counts */
        static const uint8_t b[] = {0x90, 60, 1, 0xF0, 0x7D, 0xF8, 1, 2, 0xF7, 61, 1, 0x80, 61, 0};
        static const uint32_t p[] = {PK(0x90, 60, 1), RT(0xF8), PK(0x80, 61, 0)};
        feed(b, sizeof b);
        want("SysEx skipped, running status not resumed after it", p, 3);
    }
    {   /* the DMA laps the reader: the half message left is dropped, the next status is clean */
        uint8_t b[UM_RING + 10];
        uint32_t i;
        static const uint32_t p[] = {PK(0xB0, 74, 99)};
        for (i = 0; i < sizeof b; i++)
            b[i] = 0x30;                                 /* data bytes with no status before them */
        b[0] = 0x90;
        memcpy(b + sizeof b - 3, "\xB0\x4A\x63", 3);
        feed(b, sizeof b);
        want("overrun: resync on the next status", p, 1);
    }
    printf(fails ? "trs: %d FAILED\n" : "trs: ok\n", fails);
    return fails != 0;
}
