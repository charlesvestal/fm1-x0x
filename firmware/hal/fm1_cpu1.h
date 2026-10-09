/* SPDX-License-Identifier: GPL-3.0-only */
/* The AC79's second core (docs/plans/2026-10-04-dual-core-and-clock.md).
 *
 * Start (the AC79 SDK's EnableOtherCpu, read from its library): core 1's entry address into the
 * word at 0x01C7FFF8, bit 3 of 0x10008 held while it starts, C1_CON (0x1EEE004) bit 3 set (enabled)
 * and bit 1 cleared (out of hold); the core reports in through fm1_c1_alive. Hold is the inverse.
 *
 * Work: one job at a time, a function and an argument, run by core 1 between c1_job and c1_done
 * (single writer each way; the two cores share the caches, csync orders the writes).
 *
 * Flash: both cores run code from the SPI flash. Before core 0 switches the flash off to erase or
 * program it (irq_save in fm1_flash.h), core 1 is parked in RAM (fm1_c1_park_spin, .ram_text) and
 * core 0 waits until it is there; irq_restore lets it go. Core 1 only ever works for the audio
 * interrupt, which cannot run inside irq_save, so it is never caught mid-job. */
#pragma once
#include <stdint.h>
#define FM1_HAVE_CPU1 1

#define FM1_C1_ENTRY (*(volatile uint32_t *)0x01C7FFF8u)
#define FM1_C1_CON (*(volatile uint32_t *)0x1EEE004u)
#define FM1_C1_CLK (*(volatile uint32_t *)0x00010008u)
#define FM1_T4_CNT (*(volatile uint32_t *)0x10804u)     /* TIMER4, 24 MHz */

extern void fm1_c1_entry(void);
static volatile uint32_t fm1_c1_alive, fm1_c1_job, fm1_c1_done, fm1_c1_park, fm1_c1_parked;
static void (*volatile fm1_c1_fn)(uint32_t);
static volatile uint32_t fm1_c1_arg;
static uint8_t fm1_c1_on;                       /* started and answering: core 0 may hand it work */

static __attribute__((section(".ram_text"), noinline, used)) void fm1_c1_park_spin(void)
{
    fm1_c1_parked = 1;
    while (fm1_c1_park)
        ;
    fm1_c1_parked = 0;
}

void fm1_c1_main(void);
void fm1_c1_main(void)
{
    void (*spin)(void);
    {   /* RAM code is beyond a direct call from flash: through a pointer the compiler cannot fold */
        void *volatile q = (void *)&fm1_c1_park_spin;
        spin = (void (*)(void))q;
    }
    fm1_c1_alive = 1;
    for (;;) {
        uint32_t j = fm1_c1_job;
        if (fm1_c1_park)
            spin();
        else if (j != fm1_c1_done) {
            fm1_c1_fn(fm1_c1_arg);
            __asm__ volatile("csync" ::: "memory");
            fm1_c1_done = j;
        }
    }
}

static void fm1_cpu1_hold(void)
{
    FM1_C1_CON &= ~0x8u;
    FM1_C1_CON |= 0x2u;
    fm1_c1_on = 0;
}

/* 0 = core 1 runs and answered; else it is held again. The top of RAM must be writable
 * (fm1_guard_unlock_top) */
static int fm1_cpu1_start(void)
{
    uint32_t saved, t0;
    if (fm1_c1_on)
        return 0;
    fm1_c1_alive = 0;
    fm1_c1_park = 0;
    fm1_c1_done = fm1_c1_job;
    FM1_C1_ENTRY = (uint32_t)(uintptr_t)&fm1_c1_entry;
    saved = FM1_C1_CLK;
    FM1_C1_CLK |= 0x8u;
    __asm__ volatile("csync" ::: "memory");
    FM1_C1_CON |= 0x8u;
    FM1_C1_CON &= ~0x2u;
    t0 = FM1_T4_CNT;
    while (!fm1_c1_alive && FM1_T4_CNT - t0 < 24000u * 20u)   /* 20 ms */
        ;
    FM1_C1_CLK = saved;
    if (!fm1_c1_alive) {
        fm1_cpu1_hold();
        return -1;
    }
    fm1_c1_on = 1;
    return 0;
}

/* core 0, before switching the flash off: core 1 into RAM (and wait for it there) */
static inline void fm1_cpu1_park(void)
{
    uint32_t t0;
    if (!fm1_c1_on)
        return;
    fm1_c1_park = 1;
    __asm__ volatile("csync" ::: "memory");
    t0 = FM1_T4_CNT;
    while (!fm1_c1_parked)
        if (FM1_T4_CNT - t0 > 24000u * 5u) {    /* 5 ms and not there: stop it, never touch the flash under it */
            fm1_cpu1_hold();
            return;
        }
}
static inline void fm1_cpu1_unpark(void)
{
    __asm__ volatile("csync" ::: "memory");
    fm1_c1_park = 0;
}

/* hand core 1 a job: 1 = it took it (fm1_cpu1_wait before touching what it works on) */
static inline int fm1_cpu1_run(void (*fn)(uint32_t), uint32_t arg)
{
    if (!fm1_c1_on)
        return 0;
    fm1_c1_fn = fn;
    fm1_c1_arg = arg;
    __asm__ volatile("csync" ::: "memory");
    fm1_c1_job = fm1_c1_job + 1u;
    return 1;
}

/* 0 = done; -1 = it did not finish in time: core 1 is held from now on (the caller does the work) */
static inline int fm1_cpu1_wait(void)
{
    uint32_t t0 = FM1_T4_CNT;
    while (fm1_c1_done != fm1_c1_job)
        if (FM1_T4_CNT - t0 > 24000u * 8u) {    /* 8 ms: far past any block */
            fm1_cpu1_hold();
            return -1;
        }
    __asm__ volatile("csync" ::: "memory");
    return 0;
}
