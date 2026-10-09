# The second core, and the clock

## Done (2026-10-09): what it took on the device

The split below is in (hal/fm1_cpu1.{h,S}, engine.c render_sub): core 1 renders the 909 and the
808 into the mix while core 0 renders the 303s and the break; the output is bit for bit one core's.
What the device needed, beyond the SDK sequence (several from Melodee's dual-core audio,
github.com/keremimo/melodee, GPL-3.0):

- **Start at power-on**, before the audio and the timers. Started later from the main loop, core 1
  never reached its entry (C1_CON bit 3 did not stay set; no trace).
- **The PC limits apply to core 1**, and it boots through the chip's ROM: open them while it starts
  (fm1_guard_pc_open), arm them again after (Melodee: DBG bit 10, c1_pc_limit_err_r).
- **The SDK's entry**: icfg 0, usp / sp / ssp, then reti / rti into the loop. After rti the core
  runs on **usp**: give that the real stack. With 256 bytes there the render (284 bytes idle, 828
  dense) ran past it into X0X's variables: the master volume and the PERF test broke.
- **The waiting loop in RAM** (.c1_text, inside the PC window): idle, core 1 never fetches from
  the flash, so a flash write needs no parking. tools/build.py must put .c1_text in the image
  (it was missing at first: core 1 ran into erased flash, 0xFFFFFFFF).
- Hold before a reset: C1_CON bit 1 set, then bit 3 cleared.

Measured (PERF TEST, one FM-1): factory loop 43 -> 32 %, all five 83 -> 53 %, worst case 115 -> 82 %
(peak 100 %). Core 1's stack: 828 of 2816 bytes at the deepest.

Research for using the AC79's second core, from JieLi's AC79 SDK
([gitee: Jieli-Tech/fw-AC79_AIoT_SDK](https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK), Apache-2.0): its
headers, its sources, and its precompiled libraries disassembled with the JieLi toolchain. Nothing
here has run on an FM-1.

## Starting core 1: the sequence is known

`EnableOtherCpu` (system.a, port.c.o, LLVM bitcode compiled with JieLi's clang and disassembled):

```c
*(volatile u32 *)0x01C7FFF8 = (u32)cpu1_start;   /* core 1's entry: a word at the top of RAM */
u32 saved = *(volatile u32 *)0x10008;            /* a clock / power register: bit 3 held during start */
*(volatile u32 *)0x10008 |= 0x8;
cpu1_run_flag = 0;
corex2(0)->C1_CON |= 0x8;                        /* 0x1EEE004: core 1 enabled ... */
corex2(0)->C1_CON &= ~0x2;                       /* ... and out of hold */
while (!cpu1_run_flag) ;                         /* core 1 reports in */
*(volatile u32 *)0x10008 = saved;
```

It is the exact inverse of how the SDK's exception handler stops the other core
(`q32DSP(1)->CMD_PAUSE = -1; C1_CON &= ~BIT(3); C1_CON |= BIT(1);`, cpu/wl82/debug.c).

`cpu1_start` (cpu.a, startup.S.o) sets core 1's `usp` and `sp` to its own stacks (`.cpu1_ustack`,
0x300 B; `.cpu1_sstack`, 0x1000 B, from the SDK's linker script) and enters `cpu1_main` with `reti`
/ `rti`. `cpu1_main` (apps/common/system/init.c) sets `cpu1_run_flag = 1`, disables its interrupts,
sets up its interrupt and debug state, and runs the scheduler (or idles).

For X0X: our own `cpu1_start` (stacks in RAM), and a `cpu1_main` that reports in and loops waiting
for work, with no interrupts. 0x01C7FFF8 is in the RAM-top area X0X already leaves alone (boot info).

## Sharing data: the caches are shared

There is one cache controller for the dual-core complex (`corex2(0)->CACHE_CON`, `DCACHE_WAY`,
`ICACHE_WAY`), and the cache way split is "CPU" against "peripheral" (`dcache_way_use_select(cpu_way,
prp_way)`), not core 0 against core 1. The flush calls (`flush_dcache`, `flushinv_dcache`) serve DMA.
So the two cores should see each other's writes without flushes; a `csync` orders them. The ISA has
an atomic `testset b[r]` (the SDK's spinlock) for hand-offs that need one; a single-writer flag per
direction does not.

## Flash: core 1 must be parked

Both cores fetch code from the same SPI flash through the shared I-cache. While X0X saves (or an
update writes) flash, nothing may execute from it: Felucca's flash code runs from RAM with
interrupts off. Core 1 must wait in RAM code (a spin in `.ram_text`) for the whole operation:
core 0 raises a "park" flag, core 1 finishes its current job and spins in RAM, acknowledges, and core
0 writes flash and lowers the flag.

## The split

Today the sequencer and the engines interleave inside the block (`render_sub` runs between events).
For two cores, a block becomes:

1. core 0: drain the command queue, motion, then run the sequencer over the whole block into a list
   of timestamped events (drum hits, 303 notes, break steps);
2. core 1: render the 909 and the 808 through the block, applying the drum events at their samples;
   core 0, at the same time: the two 303s and the break;
3. core 0: wait for core 1, then the sends, the master and the output.

Parameters for the drum engines are applied by core 0 before step 2 only, so the engines are never
written while core 1 renders them. With the worst case measured in the simulator (909 54 + 808 171
against 303s 145 + break 12 ns/sample, FX + master 60), a block costs about the larger half plus the
tail: ~285 instead of ~440 ns, **about -35 %**. The split can also be chosen per block by load.

Risks: both cores missing the I-cache at once contend for the flash; the I-cache is shared, so two
code paths at once may evict more. Behind a switch, with a fallback to one core if core 1 does not
report in.

## The clock: MEASURED 360 MHz (2026-10-05) -- no headroom

Read off a real FM-1 with a development build (X0X_DEBUG=1, tools/fm1_debug.py): reference 24 MHz
(0x119A0 & 0xC000000 != 0x8000000), PLL = 24 / (((0x119A8 >> 2) & 31) + 2) * ((0x119AC & 0xFFF) + 2)
= 24 / 12 * 270 = 540 MHz; system source 0x10014 & 0xF = 6 = 2/3 of the PLL = **360 MHz**; HSB /2
= 180, LSB /3 = 60 (0x10008 bits 16-17 and 8-10, minus one), which is the 60 MHz the LCD assumes.
A one-instruction loop (`if (--r != 0) goto`) runs at 51.43 per us = 7 cycles at 360 MHz.
C0_TL_CKCNT does not count (DBG_CON 0). M-VAVE already runs the chip above the SDK's 320 MHz table
(only its "overclocking" 396 MHz entry is higher): raising the clock is not an option. What follows
was written before the measurement.

### Before the measurement: probably 240 MHz, and 320 is supported

`sys_clock_table` (apps/common/system/system_vdd_clock.c) gives the bus clocks for each CPU clock.
X0X's LCD driver (Felucca's) assumes a 60 MHz peripheral (lsb) clock; that occurs only at **120 MHz
and 240 MHz**. The table also has 320 MHz at the same core voltages, and the SDK times a 240 -> 320
switch at 3.2 ms. If the FM-1 runs at 240, raising it to 320 is +33 % for everything, no sound
change; at 120, +167 %. The switch code is in the closed library (`clock.c.o` in cpu.a) and would
be read the same way as `EnableOtherCpu`. What the boot code actually leaves should be measured
first: a busy loop timed against the 24 MHz TIMER4 gives the clock; X0X could show it in GLO > ABOUT.

## Questions for Felucca's author

Felucca has no Discord; its README points to
[GitHub Discussions](https://github.com/hugelton/Felucca/discussions) and X
([@kurogedelic](https://x.com/kurogedelic)).

1. What clock does the FM-1 run at under Felucca: measured, or is the LCD's 60 MHz lsb an assumption?
2. Has `cpu_q8` been measured at full load (8 voices of the heaviest engine, drums)? How close to the
   85 % shed threshold does it come?
3. Has the second core, or raising the clock to 320 MHz, been tried?
4. Does the audio ISR's timing change with UI activity (the I-cache)?
