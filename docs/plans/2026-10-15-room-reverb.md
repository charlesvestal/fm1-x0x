# ROOM: the second reverb type

ROOM is X0X's second reverb type (REV TYPE), alongside CLASSIC (9W9's four combs and two allpasses).
It is an 8-line feedback delay network. Its tail is denser than CLASSIC's and has no metallic ring,
and for the same knob settings it has CLASSIC's decay time and loudness. The code is in
`firmware/src/dsp/fxbus.c` (the ROOM section) and `fxbus.h` (`fx_room_t`); the test is
`tests/host/reverb_test.c`.

## Constraints

- **Memory.** `tools/build.py` fails a build with less than 8 KB of pool free, and the image is
  close to that limit, so ROOM cannot add memory. The two types share CLASSIC's reverb buffer
  (18,008 B, `fx_rv_buf_t`, a union of the two layouts); `fx_room_t` is 17,676 B. Switching type
  clears the buffer, which cuts off the tail.
- **CPU.** The FX bus runs on core 0 in the audio interrupt. ROOM costs about 41 timer ticks per
  sample (1 tick = 15 cycles = 0.18 % of the core), CLASSIC about 29.
- **Host and device agreement.** Host and device compute the same samples (`-ffp-contract=off`), so
  the DSP is written in plain C and the host tests check the code the device runs.

## Signal flow

1. **High-pass** (RvLowCut, CLASSIC's own filter).
2. **Four allpass diffusers** in series: 211, 157, 397 and 293 samples, gains 0.7, 0.7, 0.65 and 0.65,
   stored as int16 at x2048 (the step and range of CLASSIC's combs). The lines average about 38 ms,
   so at the default decay a sound makes only about 10 trips around the network; the diffusers
   provide the early echo density. With two diffusers, ROOM sounded sparse.
3. **The network:** 8 delay lines, each followed by a one-pole damper, mixed by an 8-point
   Walsh-Hadamard transform (scaled by 1/sqrt(8), so it is orthogonal). The input is added to each
   line with a sign (`+ + - + + - - +`).
4. **Outputs:** two rows of the mix, taken before the input is added: L is row 1
   (`+ - + - + - + -`), R is row 6 (`+ + - - - - + +`). These signs keep L, R and the input
   uncorrelated.

### The lines

The lengths are 1123, 1283, 1459, 1571, 1747, 1907, 2083 and 2243 samples (25.5 to 50.9 ms). In the
mix they are placed in the order 1123, 1459, 1747, 1571, 1283, 2083, 2243, 1907 (see The lines'
order).

- They are primes, so no two lines share a resonance below the sample rate.
- They span about an octave. The shortest is just above CLASSIC's shortest comb, so that the two
  types sound like rooms of the same size. Sets of shorter lines gave tremolo and a low-mid
  resonance.
- They are all equal modulo 16 (all are 3 mod 16; a `_Static_assert` checks this). This is for
  speed; see Reads in blocks of 16.

The set was chosen by ABX against an earlier set; the two could be told apart, but the new one was
not worse. Another pairwise-coprime set with the same offset had clear beating on long tails, so a
new set needs a listening test in addition to the arithmetic checks.

Equal lengths modulo 16 have a side effect. An echo that has travelled k times around the network
falls at 3k mod 16 within a block, whatever its path, so during the first few trips the tail
occupies only a few positions in every 16 samples, and its spectrum repeats every
44100 / 16 = 2756 Hz. The pattern is strongest on clicks at long, bright settings, in the first
150 ms. Two fixes were tried: rings with two residues (a second copy of the fetch's decode), and
lines read a few samples further back (12 floats copied per block, an estimated 0.9 tick). Both
reduced the measured pattern, but a blind preference test on loops and snares at four settings
found no preference (5/16 for the fix), and the lengths were left as they are.

Any change of a few samples to the delays, including these fixes, is easy to identify on repeated
clicks: from 20 to 100 ms after each hit the tail consists of the same few echoes, and listeners
learn their texture. An ABX test on repeated clicks therefore cannot judge a change to the lengths;
a preference test on varied material is needed.

### The lines' order

The assignment of lengths to positions in the mix, and the input's sign at each position, costs
nothing at run time. It determines which lines are added and subtracted at each stage of the
transform, and therefore how echoes that travel different paths of nearly equal length combine.
Where only a few such paths contribute near a given partial, the early tail beats regularly: an
amplitude wobble in the first few hundred milliseconds, most audible on toms, 303 notes and
crashes. A 32-line network in float did not show it.

The following measure agreed with the blind results for orders of these lengths. For tone bursts
from 60 Hz to 6 kHz, it takes the tail's envelope in an ERB-wide band around the burst's frequency,
from 30 to 700 ms after the burst, removes its straight-line decay (in dB), and takes the largest
normalized autocorrelation at lags of 60 to 400 ms. This measures how regular the beating is rather
than how deep; envelope depth did not separate the networks. The measure ranks networks of 4, 8, 16
and 32 lines in the order they sound. A local search over the order, the lines' read signs and the
input's signs, with constraints that echo density, spectral spread and loop-period repeat not get
worse, found the current order with one input sign changed (position 3).

In blind preference tests on headphones, the current order was preferred over the lengths in
ascending order in 16 of 16 trials (toms, 303 and crashes), and over two other orders from the
search in 15 of 16 and 12 of 16. The comparison with ascending order on loudspeakers was close to
chance (9 of 16), so changes to the tail should be judged on headphones.

Tone bursts do not capture all of the effect. The 909 low tom's pitch falls from about 75 to 68 Hz
as it decays, and a real low-tom hit showed wobble on one candidate network (the widest line range
in Tried and rejected) that the tone bursts did not; the same hit, pitch-shifted by up to 8 %, gave
the same result. The search over wider line ranges therefore also scored real tom hits.

### Loop gain and damping

RvDecay and RvTone map to two decay times, at DC (`t_dc`) and at Nyquist (`t_ny`). For a line of
length L:

- the loop gain is `g = 10^(-3 L / t_dc)`, so every line decays at `t_dc` at DC;
- the damper is a one-pole lowpass, `lp = a lp + (1 - a) in`. A reference line of the lines' mean
  length gets the damper whose gain at Nyquist is `10^(-3 L / t_ny) / g`. Each line's damper then
  matches the reference's decay at 3 kHz: its response there, relative to DC, is the reference's
  raised to the power `L / L_ref` (which has a closed form, a quadratic in `a`).

A one-pole damper has two parameters, so a line can match its target decay at only two
frequencies. Matched at DC and Nyquist (the usual choice), lines of different lengths diverge in
between, and more so the larger `t_dc / t_ny`. At the longest, darkest setting (2.15 s against
0.13 s), the shortest line decayed 2.2x more slowly than the longest at 1.4 kHz, and modes on the
short lines rang longer than the frequencies around them (a 1410 Hz ring on a snare's tail, 1.6x
longer than its neighbours). Matched at DC and 3 kHz, the lines stay within 1.15x of each other
below 5 kHz at that setting (within 1.04x at the others), and within 1.43x above 5 kHz, where tails
are quiet and short (1.57x before). This removed the ring, and blind preference tests (drums and a
303 line) found no other difference. Matching at 4 to 5 kHz instead trades the mid band (1.24 to
1.35x) for the top (1.30 to 1.36x).

In the code, the input gain `c = g (1 - a) / sqrt(8)` (including the mix's scale) is folded into the
step used to decode the line's reads, so the damper itself is `z = a z + read`.

### Output compensation

The outputs are sums of the damped lines, so they carry the dampers' lowpass and the loop gain, and
on their own they sound darker than the lines. Each output is therefore passed through the inverse
of the lines' mean damper and gain:

`y = (s - tap_a s[-1]) tap_k`, with `tap_a` = mean a and `tap_k = sqrt(8) / (mean g (1 - tap_a))`.

This matches taking the outputs from the undamped lines to within 0.25 dB rms across bands and
0.13 dB in level. Taking them from the mix's sums avoids separate sums for the taps; together with
folding `c` into the decode, this reduced the network from 140 to 107 instructions per sample.

## Line storage: 8-bit block-float

In this memory, 16-bit lines would limit the mean line length to about 21 ms, and 8-bit samples with
a fixed scale would leave late tails only a few steps above zero. Instead, each line is a ring of
16-sample blocks, each holding 16 signed 8-bit mantissas and one exponent byte `e`, with a step of
`2^e / 128`:

- **Commit.** At the end of every 16 samples, the commit picks the smallest step that keeps the
  block's peak under 127.5 steps and rounds each sample to the nearest step. The noise therefore
  follows the tail down, staying about 47 to 53 dB below the block's own peak. 8-bit and 16-bit
  mantissas could not be told apart by ear.
- **Silence.** A block whose peak is under 1e-6 (-120 dB) is stored as zeros. When every ring holds
  only zeros and the input has been silent for `FX_RM_QUIET` blocks in a row, ROOM goes idle: it
  clears its dampers, filter state and diffusers, and returns immediately until input arrives.
- **Ring size.** A ring has L / 16 + 1 blocks, the fewest that hold L samples. At the start of a
  block of writes, the read's first block is the one the write is about to replace; it still holds
  the samples from L / 16 + 1 blocks earlier. The reads are therefore fetched at the start of each
  block, before that block's commit at its end. The block being written is kept as floats
  (`stage`) until it is committed.

### Loud and quiet tails together

A block's step is set by its loudest sample, so when a loud hit arrives while a quiet tail is still
ringing, the quiet tail is stored with the loud tail's step, but it is not cut off. The commit rounds
to the nearest step, and the loud tail changes by many steps from one sample to the next, so the
rounding error behaves like noise that is independent of the quiet tail (the loud tail acts as
dither). The quiet tail remains in the stored sum at its own level, below that noise. Truncation
would set it to zero instead; that is what makes the decay of CLASSIC's 12-bit combs shorten at low
levels.

Measured against the same network with float lines (and ROOM's int16 diffusers, so that only the
storage differs), reverb only, at the longest decays and the default:

- **Under louder hits.** A tail under later, louder hits keeps its level (within 1 dB) until it is
  about 40 dB below them; below that it sinks into their rounding noise rather than stopping.
  Through a run of hits (eight, 6 dB louder, every 0.25 s), the sum stays within 0.02 dB of the
  float network's level, with its error 35 to 42 dB below it. There is no gating or pumping.
- **Across the spectrum.** Rounding noise is white, so its level in every band follows the loudest
  tail. With loud kicks over quiet hi-hats, the noise in the hats' band rises by about 9 dB while the
  kicks' tails ring, to about 31 dB below the hats' tail, and falls back afterwards. Long, dark
  settings are the worst case.

The diffusers are stored differently: as int16 with a fixed step, truncated, as CLASSIC's combs are,
and with the same step and range (1/2048, up to 16). Their error relative to the signal grows as the
send gets quieter: about 28 dB below it at a send of RMS 0.02, and 13 dB at 0.0034. A finer step
would cost headroom: the loudest send X0X can produce (every part at level 127 with its send up, in a
dense groove) peaks at about 7.3 inside them, and every voice of both kits on the same steps at
about 8.4.

### Reads in blocks of 16

All the lines write together: the 16 samples in progress fill each line's `stage` block. At the start
of those 16 samples, each line's 16 reads are decoded into `read` at once. Because the read is L
samples behind the write, it starts at the same position within a block every time, `-L mod 16`. The
16 reads cover the rest of that block and the start of the next, and both are already in the ring
(the newest sample they reach was written L >= 16 samples earlier).

Because the lengths are all equal modulo 16, that position is the same compile-time constant for
every line. The decode is then straight-line code, with no per-line split or remainder loop, and one
copy of it serves all eight lines. This made the fetch about 40 % cheaper than one with per-line
offsets.

## Knob mapping

With `x` = RvDecay / 127 and `y` = RvTone / 127:

- `ln t_dc = -1.7984 + 1.1228 x - 0.6857 x^2 + 2.1048 x^3 + 0.0597 y`
- `ln t_ny = min(ln t_dc, -2.0520 - 0.6055 x - 0.1850 y + 1.2173 x y + 0.2419 y^2 + 0.5170 x^2)`
- the output gain is `1.8963 + 0.3712 x + 0.1889 y`.

These are fitted to CLASSIC on a grid of 11 x 10 knob settings by `tools/fit_room.py`, which renders
both types with this firmware's fxbus on the host (`tools/fit_room_render.c`) and prints the C for
`fx_room_coeffs`; `tools/fit_room.py --check` measures the firmware's mapping as it is. It needs
numpy and scipy. Refit after changing the lines, the dampers or the outputs.

- **Decay:** CLASSIC's response to a 20 ms noise burst at a snare's send level, measured as the
  decay time (-3 to -23 dB of the backward-integrated energy) in 10 mel-spaced bands, 100 Hz to 10
  kHz. CLASSIC's decay depends on level (its 12-bit loop truncates small values), so the burst level
  matters. At each setting, `t_dc` and `t_ny` are fitted to CLASSIC's bands through ROOM's
  predicted per-band decay, and the two surfaces are then fitted with the polynomials above.
- **Loudness:** at each setting, the output gain that matches ROOM's K-weighted (BS.1770) level to
  CLASSIC's, fitted as a plane.

Against CLASSIC, the result is within 14 % rms in decay time per band (at most 46 %, at RvDec 0,
where two parameters cannot follow CLASSIC's band shape; the best fit at each setting does no
better), and within 0.13 dB rms in level (at most 0.5 dB).

The first coefficients were fitted with an earlier set of lengths, before the outputs were taken
from the mix and before the dampers were matched at 3 kHz. Their decay terms were within about 0.03
of these, but their gain was up to 1.5 dB too loud at low settings (0.7 dB rms). Reordering the
lines (The lines' order) left the decay terms unchanged and moved the gain by up to 0.2 dB; the gain
was refitted.

## Tried and rejected

| Option | Why not |
|---|---|
| Half-rate network | The top end rolls off; a full-rate early path did not fix it |
| Delay-line modulation | Audible wobble at the faster rate, and no clear improvement |
| 4 lines (same memory, doubled lengths) | Pumping and a hall-like sound; the first echo arrives too late (52 ms) |
| 4 lines at the current spread | A buzzy tail |
| 2 diffusers | Sparse at the default decay |
| mu-law companding of the lines | One fixed scale: late tails are reduced to a few coarse steps that recirculate |
| Integer arithmetic in the network | Slower on this core (357 vs 295 cycles per sample) |
| Delays of several residues modulo 16 | No preference on music, and it costs time (see The lines) |
| Pre-delay (10 to 30 ms) | It leaves the early tail exposed before the network has mixed it, and its memory would have to come from the lines (20 ms: 6 % shorter) |
| Two other prime sets over the same range | One was preferred less (3/16), the other equally (6/16) |
| Wider line ranges in the same memory (8 lengths spaced geometrically over 467 to 3923 or 691 to 3251 samples), each in its best order | Preferred on crashes (6/6; a smoother early decay and less mid-band ring), but each order found was worse either on toms (wobble; the current order preferred 16/16, also at the default RvLowCut) or on 303 notes (low-end clutter; the current order preferred 6/6) |
| Other orders with less loop-period repeat | Preferred less than the current order (1/16 and 4/16): more wobble, or low-end clutter on 303 notes |
| Shorter diffusers (73 to 257, or 67 to 131 samples) | Not preferred in informal listening |
| Diffusers spaced at a ratio of 2 or 3 (47 to 397, 13 to 397 samples) | No clear preference over the current ones; ratio 2 would free 640 B |
| Emphasis around the lines' storage (pre-emphasis of the network's input after the diffusers, de-emphasis of its outputs, coefficient 0.8) | It lowers the storage noise on low tom tails at 4 to 12 kHz by about 4 dB at the default RvLowCut (9 dB at its minimum), but costs about 0.5 points of PERF load (2 to 3 ticks), raises the error at the longest settings from about -40 to -33 dB relative to float lines, and raises the error on crashes at 100 Hz to 1 kHz by about 4 dB. At the default RvLowCut the difference was small in listening |

## Cost on the device

Measured with the 24 MHz timer in test builds (not in the repo). Cycle costs on the AC79 core at
360 MHz:

| Operation | Cycles |
|---|---|
| FP add or multiply | 1.9, with no overlap between ops, dependent or not, and no data dependence |
| int / float conversion | 0.9 |
| Load / store | 1.9 / 1.3; a load paired with another op (`#` in the listing) is nearly free |
| Taken branch | 8 |
| Call and return | 44 |
| RAM read, any span or stride | 2.1 (no data-cache effects) |
| Code from flash, up to 16 KB warm | 1.0 per instruction |

Consequences for the code:

- Branches and calls are expensive, so the loops that run per sample have constant trip counts
  where possible (the whole-block network, the decode), and the per-block work is done in a few
  calls.
- FP ops do not overlap, so reordering does not hide their cost; only removing ops helps. This is
  why the gains are folded into the decode and the outputs are taken from the mix.
- There are few registers (the FPU uses the integer registers), so the work is done in phases over
  each chunk of up to 16 samples (high-pass, diffusers, network), with the dampers in two groups of
  four. All eight in one loop spilled registers.

ROOM's profile is about 41 ticks per sample against CLASSIC's 29: high-pass 3, diffusers 8, fetch
6, network 17, commit 8. The PERF TEST (load / peak, %), in a test build in which all three cases
run the same reverb type (code layout moves these figures by about a point between builds, and
repeated runs differ by up to about 0.7 points in load):

| | Factory | All five | Worst |
|---|---|---|---|
| CLASSIC | 40.5 / 49 | 54.6 / 67 | 68.1 / 86 |
| ROOM | 43.3 / 60 | 57.0 / 69 | 71.1 / 90 |

The same ROOM code run from RAM measured 0.6 to 1.8 points lower, with the difference growing with
load. Code from flash costs more inside the audio interrupt than in a warm loop; the reason is not
known.
