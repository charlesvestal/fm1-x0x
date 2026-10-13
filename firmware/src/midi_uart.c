/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* MIDI IN on the TRS jack (ported from Felucca 1.0's midi_uart.c): PH8 -> input channel 1 -> UART1
 * RX, 31250 baud, RX DMA into a 128-byte ring, polled from the TIMER5 ISR outside the render, the
 * same context as usb_poll. Channel messages and Clock / Start / Continue / Stop go into midi_in_q
 * beside USB's, as USB-MIDI packets (cable 0, CIN = status >> 4), so the engine cannot tell them
 * apart; other system messages and SysEx are ignored (no SysEx -> UBOOT from the jack). */
#include "../hal/fm1_uart.h"   /* registers; relative, so the host test finds it too */

#define UM_RING 128u
static volatile uint8_t um_ring[UM_RING] __attribute__((aligned(16)));
static struct {
    uint32_t rd, pend;
    uint32_t bytes, drops, msgs;
    uint8_t st, need, got, d0, sysex;
} um;

static int um_put(uint32_t pkt)                    /* into midi_in_q (usb.c): 0 = full */
{
    if (mi_w - mi_r >= MQ)
        return 0;
    midi_in_q[mi_w % MQ] = pkt;
    RING_PUBLISH();
    mi_w++;
    return 1;
}

static void uart_midi_init(void)                   /* before TIMER5 starts: PORTH read-modify-write */
{
    fm1_uart1_midi_init(um_ring, UM_RING);
}

static uint32_t um_len(uint32_t s) { return (s & 0xE0u) == 0xC0u ? 1u : 2u; }   /* Cx Dx: 1, else 2 */

static void um_byte(uint32_t b)
{
    if (b >= 0xF8u) {                              /* real time: any time, keeps running status */
        if (b == 0xF8u || b == 0xFAu || b == 0xFBu || b == 0xFCu) {
            if (um_put(0xFu | b << 8))
                um.msgs++;
            else
                um.drops++;
        }
        return;
    }
    if (b & 0x80u) {
        um.sysex = b == 0xF0u;
        um.st = b < 0xF0u ? (uint8_t)b : 0;        /* system common / SysEx end running status */
        um.need = (uint8_t)um_len(b);
        um.got = 0;
        return;
    }
    if (um.sysex || !um.st)
        return;
    if (um.need == 2u && !um.got) {
        um.d0 = (uint8_t)b;
        um.got = 1;
        return;
    }
    {
        uint32_t d1 = um.need == 2u ? um.d0 : b, d2 = um.need == 2u ? b : 0u;
        um.got = 0;
        if (um_put((um.st >> 4) | (uint32_t)um.st << 8 | d1 << 16 | d2 << 24))
            um.msgs++;
        else
            um.drops++;
    }
}

static void uart_midi_take(uint32_t n)
{
    um.pend += n;
    if (um.pend > UM_RING) {                       /* the DMA lapped us: wait for a whole new status */
        um.drops += um.pend - UM_RING;
        um.rd = (um.rd + um.pend - UM_RING) & (UM_RING - 1u);
        um.pend = UM_RING;
        um.st = um.got = um.sysex = 0;
    }
    while (um.pend) {
        um_byte(um_ring[um.rd]);
        um.rd = (um.rd + 1u) & (UM_RING - 1u);
        um.pend--;
        um.bytes++;
    }
}

static void uart_midi_poll(void) { uart_midi_take(fm1_uart1_rx_take()); }   /* TIMER5, outside the render */
