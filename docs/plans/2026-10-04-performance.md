# Performance: what X0X costs, and what the FM-1 can do

Research before any hardware: what is known about the chip, where X0X spends its time, and what
could be cheaper. Nothing here has been measured on an FM-1.

## The chip

- JieLi AC79 (AC791N family): dual-core 32-bit DSP with a single-precision FPU, **up to 320 MHz**,
  578 KB SRAM, **32 KB 8-way I-cache**, 32 KB D-cache
  ([JieLi's SDK page](https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK)).
- **The clock X0X runs at is unknown.** Neither X0X nor Felucca sets it; it is whatever the boot code
  leaves. One clue: the LCD driver's comment implies a 60 MHz peripheral clock.
- Code runs from SPI flash through the I-cache (XIP). Only `.ram_text` (the audio ISR's entry, 909
  instructions) runs from RAM.
- Felucca publishes no load figures (README, release notes, history). Its audio code sheds a voice
  when a half-buffer takes over 85 % of its time: its author expected overload to be possible. **X0X
  has no such guard**: an overrun is a dropout.

## Where X0X's time goes (simulator)

`X0X_PROFILE=1 sh host/build_host.sh` builds `build/host/x0x_host_prof`, which times each stage of
the render (the hooks compile to nothing in the firmware: the image is byte-identical). Nanoseconds
per sample on the Mac (stable to about 1 % run to run; the ratios are what matter):

| Stage | Idle / muted | Factory loop (909, 303A, break) | All five | Worst case (all dense) |
|---|---|---|---|---|
| 909 | 6 | 22 | 22 | 54 |
| 808 | 9 | 9 | 88 | 171 |
| 303 A | 2 | 71 | 71 | 72 |
| 303 B | 2 | 2 | 72 | 72 |
| Break | **11** | 11 | 11 | 12 |
| FX (reverb, delay, kit drive) | 0.6 with no sends | 20 | 20 | 30 |
| Master | 17-25 | 25 | 26 | 29 |
| Sequencer, motion | 0.8 | 0.8 | 0.8 | 1.0 |
| Host instructions / sample (mean / worst block) | | 1361 / 1534 | 2582 / 2900 | 3759 / 4192 |

Details:

- **One 303 is the most expensive part**: 71 ns, more than three times the whole 909. Its 2x
  oversampling costs 24 ns and its drive 21 ns (both types; drive 0 skips it). The big-beat factory
  mix turns the drive on for both 303s: 42 ns.
- **The 808 is cheap idle and very expensive dense**. Per voice while sounding: cymbal 53, bass drum
  35, snare / cowbell / open hat / toms about 20, clap / rim / closed hat under 5.
- **The master costs 25 ns even in a quiet mix**: the compressor 16 (a log2 and a db2lin per
  sample), the limiter 7 (a divide per sample: `0.8 / env`).
- **The break costs 11 ns when it is not heard**: muted or switched off for a step, it fades its
  gate to zero but keeps decoding the loop sample by sample.

## Code size against the cache

The audio units compile to about **54 KB** (909 10.3, 303 11.0, sequencer / motion / engine 8.7, 808
7.8, break 7.4, FX 5.1, master 3.3). The largest functions are `drum909_render` (8.8 KB) and
`bass303_render` (7.2 KB). The code a block actually runs is smaller, but plausibly near the 32 KB
I-cache, and the UI code competes for it between blocks. If the cache is refilled from flash every
block, that is a cost on top of the instruction counts. It can only be measured on the device.

## Against the budget (an estimate, not a measurement)

A sample's budget is the clock / 44 100: 7256 cycles at 320 MHz, 5442 at 240, 3628 at 160. How
many FM-1 cycles a Mac instruction becomes is unknown (single-issue core, FPU latencies, no SIMD;
the Mac vectorises some loops); assuming about 2:

| | Cycles / sample | At 320 MHz | At 160 MHz |
|---|---|---|---|
| Factory loop | ~2700 | 37 % | 75 % |
| All five parts | ~5200 | 71 % | 143 % |
| Worst case | ~7500 | 103 % | 207 % |

So: the factory sound is probably fine, all five parts at once is fine only if the chip runs near
320 MHz, and a dense worst case probably overloads. The real number comes from the device.

## Savings that do not change the sound

1. **Break: stop decoding when nothing is heard** (gate at zero): about 11 ns whenever the break is
   muted or off, which is most patterns. It would resume at the next step.
2. **Master: compute the compressor gain every 4 samples** (interpolated) and take the limiter's
   divide off the per-sample path: an estimated 10-14 of its 25 ns.
3. **Idle early-outs in the 808 and 909**: their idle 9 and 6 ns.

### Done (77607cc's follow-up)

Silent parts are no longer cleared, mixed or metered (909, 808, 303s, break; exact); the break stops
decoding when it cannot be heard (muted, no steps on: it comes back at its next slice); the master
computes its compressor gain every 4 samples and keeps a running reciprocal in the limiter (no
divide a sample). Host instructions per sample, mean (±0.1 % run to run; the worst-block figure
moves ±8 % and is not used):

| | Before | After |
|---|---|---|
| Factory loop | 1356 | 1263 (-6.9 %) |
| All five | 2581 | 2509 (-2.8 %) |
| Worst case | 3756 | 3692 (-1.7 %) |

**These make the quiet cases cheaper; they barely touch the worst case**, which is the 808 (about
45 %), the 303s (about 35 %) and the 909 (about 13 %) doing real work.

### Done: cheaper maths in the engines (worst case)

Profiled per engine (worst-case patterns, `sample` on a no-inline build), measured in kernel-counted
instructions (stable; wall time on the Mac is not), checked against each engine's reference test:

- `fm_tanhf` takes a Taylor polynomial below |x| = 0.5 (exact to float: no exp2, no divide); the op-amp
  clippers and drives spend most samples there. The 808: 1514 -> 1414 instructions per sample.
- The 303's RAT clipper, x / (1 + x^4)^(1/4) twice a sample, used two `fm_sqrtf` and a divide each: now
  `x s t` with `s = fm_rsqrtf(1 + x^4)`, `t = fm_rsqrtf(s)` (two Newton steps, 5e-6). The 303: 465 -> 433.
  (The pi32v2 FPU divides in one instruction but has no square root: `__builtin_sqrtf` calls libm.)
- Tried and dropped: a tanh table for the 808 above 0.5 (no change: few samples there).

The whole app, mean host instructions per sample, from the start of this work: factory loop 1356 -> 1221
(-10 %), all five 2581 -> 2413 (-6.5 %), worst case 3756 -> 3521 (-6.3 %). The 808's reference worst
rel moved 4.0e-4 -> 6.1e-4 (its BD feedback loop amplifies rounding), under the 1e-3 limit. Image
+2 KB (the inlined polynomial).

What remains is each model's core work (the 808's metal bank and cymbal filters, the 303's filter
loop and 2x decimator, the 909's drive table and envelopes): no expensive call left to replace.

## Savings that change the sound (need ears)

4. **303 drive**: 21 ns per 303. The factory mix could drive 303A only, or use a cheaper shaper.
5. **303 oversampling 2x to 1x**: 24 ns per 303, at the cost of aliasing on bright, resonant notes.
   The engine's reference tests already cover 1x.
6. **808 cymbal and hats**: the dense-808 cost is mostly these models.

## Protection

7. **An overload guard**, as Felucca has: past ~85 % of the block's time, shed what is least missed
   first (the reverb and delay tails, then the 303s' oversampling) rather than drop out.
8. If the device shows load that jumps with UI activity, **move the hot render loops into RAM**
   (as `.ram_text`); RAM has room (48 KB of 96 KB used).

## First measurements on a device

The MIX screen and GLO > ABOUT show the audio load, and the firmware counts dropouts. In order:

1. Idle (stopped), then the factory loop, then the showcase, then all five parts: the load of each.
   With the table above that gives the cycles per Mac instruction, and every other estimate here.
2. The same with the screen still versus animating (turn a knob): a large difference means the
   I-cache (item 8).
3. Dropouts over a few minutes of the showcase.
