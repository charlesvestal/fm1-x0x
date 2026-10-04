/* SPDX-License-Identifier: GPL-3.0-only */
/* Register access for development builds (X0X_DEBUG=1; the app's editor.c PEEK / POKE / CLOCK
 * commands): word reads and writes inside a few ranges only (a stray read of a gated block could
 * fault), and a fixed loop timed against the 24 MHz TIMER4 with IRQs off. */
#pragma once
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_perf.h"

static const uint32_t FM1_DBG_RANGE[][2] = {
    {0x00010000u, 0x00014000u},          /* clock, power, timers */
    {0x00020000u, 0x00020400u},
    {0x00040000u, 0x00041000u},          /* SFC, clock dividers */
    {0x01EEE000u, 0x01EEF400u},          /* corex2, cache, debug */
    {0x01C00000u, 0x01C80000u},          /* RAM */
};

static int fm1_dbg_ok(uint32_t addr, uint32_t words)
{
    uint32_t i;
    if (addr & 3u)
        return 0;
    for (i = 0; i < sizeof FM1_DBG_RANGE / sizeof FM1_DBG_RANGE[0]; i++)
        if (addr >= FM1_DBG_RANGE[i][0] && addr + 4u * words <= FM1_DBG_RANGE[i][1])
            return 1;
    return 0;
}
static uint32_t fm1_dbg_rd(uint32_t addr) { return *(volatile uint32_t *)addr; }
static void fm1_dbg_wr(uint32_t addr, uint32_t v) { *(volatile uint32_t *)addr = v; }

/* 24 MHz ticks a loop of `it` empty iterations took; the cycle counter before and after */
static uint32_t fm1_dbg_clock(uint32_t it, uint32_t *c0, uint32_t *c1)
{
    uint32_t t0, t1, i;
    __asm__ volatile("cli" ::: "memory");
    *c0 = fm1_perf_cycles();
    t0 = fm1_ticks();
    for (i = it; i; i--)
        __asm__ volatile("" ::: "memory");
    t1 = fm1_ticks();
    *c1 = fm1_perf_cycles();
    __asm__ volatile("csync\n\tsti" ::: "memory");
    return t1 - t0;
}
