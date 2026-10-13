/* SPDX-License-Identifier: GPL-3.0-only */
/* FM-1 (AC79) performance counters, from JieLi's AC79 SDK (include_lib/driver/cpu/wl82/asm/csfr.h,
 * the corex2 block at 0x1EEE000; cpu/wl82/debug.c's cpu_effic_calc reads them):
 *
 *   C0_TL_CKCNT  core 0's clock cycles (64-bit; the low word is enough for deltas under ~17 s)
 *   C0_IF_UACNT  ... instruction-fetch cycles the SDK reports against TL_CKCNT: read as stalls
 *   C0_RD_UACNT  ... data reads, the same
 *   C0_WR_UACNT  ... data writes, the same
 *
 * Not verified on hardware. Whether TL_CKCNT counts without anything enabled is unknown: the caller
 * checks that it moves (fm1_perf_cycles_run) and falls back to TIMER4. The three stall counts are
 * enabled by DBG_CON bits 0-2 ("if/of/ex_inv_en" in the SDK, which may also arm fault exceptions):
 * only when asked, never at boot. Felucca's guards use DBG_EN (0x340), not DBG_CON (0x344). */
#pragma once
#include <stdint.h>
#include "fm1_time.h"

#define FM1_C0_IF_UACNTL (*(volatile uint32_t *)0x1EEE200u)
#define FM1_C0_RD_UACNTL (*(volatile uint32_t *)0x1EEE208u)
#define FM1_C0_WR_UACNTL (*(volatile uint32_t *)0x1EEE210u)
#define FM1_C0_TL_CKCNTL (*(volatile uint32_t *)0x1EEE218u)
#define FM1_DBG_CON (*(volatile uint32_t *)0x1EEE344u)

static inline uint32_t fm1_perf_cycles(void) { return FM1_C0_TL_CKCNTL; }

/* the cycle counter moves: two reads 100 us apart differ */
static inline int fm1_perf_cycles_run(void)
{
    uint32_t c0 = FM1_C0_TL_CKCNTL, t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 100u * FM1_TICKS_PER_US)
        ;
    return FM1_C0_TL_CKCNTL != c0;
}

static inline void fm1_perf_stalls(uint32_t s[3])
{
    s[0] = FM1_C0_IF_UACNTL;
    s[1] = FM1_C0_RD_UACNTL;
    s[2] = FM1_C0_WR_UACNTL;
}

static inline void fm1_perf_stalls_enable(int on)
{
    if (on)
        FM1_DBG_CON |= 0x7u;
    else
        FM1_DBG_CON &= ~0x7u;
}
