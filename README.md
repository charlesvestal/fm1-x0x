# X0X for the M-VAVE FM-1

**Install:** <https://charlesvestal.github.io/fm1-x0x/install/> (Chrome or Edge, the FM-1 on USB).
**Manual:** <https://charlesvestal.github.io/fm1-x0x/manual/> (source: [docs/MANUAL.md](docs/MANUAL.md)).
**Firmware file:** [releases](https://github.com/charlesvestal/fm1-x0x/releases).

Standalone groovebox firmware for the M-VAVE FM-1: a **TR-909**, a **TR-808**, **two TB-303s**
with **TB-3PO** acid generators, and a **breakbeat generator**, with 16 patterns (each part can
play its own), a **song mode**, **recorded knob moves**, shared reverb / tape delay sends on every
part, and a master compressor with kick-keyed pump, a sweepable filter and a limiter. The factory
sound is set up for big beat.

X0X is a fork of [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita (Hügelton
Instruments). It keeps Felucca's platform — the hardware layer, USB MIDI, the update loader,
the web installer and the flash storage — and replaces the instrument.

> **Status: beta.** X0X runs on the FM-1. With every part playing a dense pattern at once it
> can still run out of processor time (PERF TEST's worst case is ~102 %); an overload guard is
> planned. Installing is at your own risk: the web installer can put M-VAVE's firmware back, an
> FM-1 that keeps crashing starts in safe mode, and the manual's Troubleshooting covers the rest.

## The instrument

| Part | Engine | From |
|---|---|---|
| 909 | 11 voices: circuit-modelled BD SD toms RS CP, sampled hats and cymbals | [9W9](https://github.com/athousanddetails/schwung-9W9) (ER-99 samples) |
| 808 | 16 sounds on 11 tracks, the 808's own Tom/Conga, Rim/Claves, Clap/Maracas switches | [8W8](https://github.com/athousanddetails/schwung-8W8) |
| 303 A, 303 B | Open303 with the Devilfish ranges, each with a TB-3PO generator | [schwung-303](https://github.com/charlesvestal/schwung-303), [schwung-tb3po](https://github.com/charlesvestal/schwung-tb3po) |
| BREAK | 8-slice break player: Complexity, Anchor, Roll, Fill, Retrig, Phrase, A/B loops | [BB Gen](https://github.com/mestela/schwung-breakbeat) |

All five parts run at once, each with its own step length (polymeter) and rate (1/16,
1/16T, 1/32, 1/8T); swing is per pattern. A pattern holds all five parts; switching
waits for the end of the bar, and patterns chain.

## The panel

One grammar everywhere (docs/plans/2026-10-03-ui.md): **ALGORITHM** picks the part,
**PRESETS** cues the pattern, **SELECT** moves (page, list row), **KNOB 1-4** change the four
values in the knob strip (always on screen), **SEL** opens / runs / says yes, **HOME** goes
back. Every knob turn shows its value large for a second; anything that loses notes asks first.

The 16 white keys are steps (on HOME, patterns). The 11 black keys are a drum machine's
tracks, or the break's slice pads (on HOME, part mutes).

| Control | Does |
|---|---|
| ALGORITHM | part: 909 / 808 / 303A / 303B / BREAK (in a list: change the value) |
| PRESETS | cue a pattern (switches at the end of the bar) |
| SELECT | next / previous page (in a list: move); HOME held + SELECT: tempo |
| KNOB 1-4 | the four values on screen, on every screen |
| SEL | the list of everything on this screen; in a list, run an action; in a question, yes |
| HOME | home: patterns, the five parts, mutes; back out of a list or a question |
| EDIT | the part's pages (again: next page) |
| ARP | TB-3PO (303 parts): OCT+ generates a new line, OCT- mutates it |
| FX | sends (every part: reverb + delay), reverb, delay (DIGI / TAPE), kit drive |
| LFO | MIX: levels, the master compressor (+ PUMP), the master filter |
| GLO | GLOBAL: MIDI, key lights, theme, save, clear, factory reset |
| SEQ | 303: steps / keyboard |
| PLAY / REC | transport / record (live drums, live or step-written 303) |
| ENV held | white keys edit the accent row; in 303 note entry, an accent (tapped: a rest) |
| LFO held | in 303 note entry, a slide (with OCT: a tie) |
| OCT- / OCT+ | steps 1-16 / 17-32 (303 keyboard: octave) |
| SAVE | save; SAVE held + white key: copy this pattern there; SAVE + REC: clear this part |
| OCT- + OCT+ held 5 s | update mode (Felucca's) |

On a 303 part, tap a white key to toggle a step; hold it and turn KNOB 1-4 for note, gate
(rest / note / tie), accent and slide.

## Master

After the sends: a bus compressor (threshold, ratio 1:1 to INF, attack, release, makeup,
parallel mix) with **PUMP**, kick-keyed ducking (the 909's or the 808's BD ducks the whole mix
by up to 24 dB and the release swells it back), a resonant LP / BP / HP filter (CUTOFF is on
HOME's PERFORM page), and a limiter. The 909 kit's own drive and glue sit on FX > KIT DRIVE.

## MIDI (USB)

In: ch 10 the 909 and ch 11 the 808 (GM drum notes), ch 2 / 3 the 303s (legato = slide,
velocity ≥ 100 = accent), ch 4 notes 36-43 the break's slices; MIDI clock and
start / stop / continue are followed automatically. Out: clock and transport, and
optionally the sequence (GLO page).

## Breaks

The firmware ships **no recorded breaks**. Its two built-in loops (909 GR, 909 FL) are played
by X0X's own 909 when the firmware is built.

Your breaks go in the FM-1's three sample slots, about 7 s of audio each, packed several to a
slot (one bar of a break is ~20 KB):

```
tools/upload_breaks.py --bars amen.wav think.wav ...    # cut one bar of each, pack, upload
tools/upload_breaks.py --bbgen                          # BB Gen's classic breaks, from ../schwung-breakbeat
tools/upload_breaks.py --dry-run OUTDIR ...             # build the slot images, no device
```

They appear after the built-in loops in LOOP A / LOOP B as BR1.1, BR1.2, ... BR2.1 (the tool
prints which is which).

Opt-in at build time, a bank of one-bar WAVs can replace the 909 loops inside the firmware:

```
tools/import_breaks.py            # one bar of each of BB Gen's classics -> build/breaks/
X0X_BREAK_BANK=build/breaks ./build.sh
```

Recorded breaks are other people's recordings, not covered by this project's licence; whether
a firmware built with them may be passed on is up to whoever builds it.

## Effect sends

One reverb and one delay (9W9's digital delay, or TAPE: wow, flutter, saturation in the
feedback loop), shared by every part. Each part has a reverb send and a delay send. FX opens
on the send pages (909 / 808, the two 303s, the break with the delay type and time). The
drum machines also keep 9W9's and 8W8's per-voice sends on each voice's own pages.

## Building and testing

See [BUILDING.md](BUILDING.md) for the toolchain. In short:

```
tests/run_tests.sh      # maths, sequencer, every engine against its original, the app in the simulator
./build.sh              # build/x0x.fwsc
```

`host/x0x_host` runs the firmware app on a computer from a script
(`tests/scenarios/*.x0x`): button presses, key presses, knob turns and MIDI in; WAV and
PNG screenshots out.

## Licence and credits

Code: GPL-3.0-only (as Felucca, 9W9, 8W8 and schwung-tb3po). Open303 (Robin Schmidt) is
MIT. The BB Gen generator is ported from mestela's schwung-breakbeat with the author's
permission. The 909's hi-hat, ride and crash samples are ER-99's (Matthew Cieplak, GPL-3.0).
Fonts: Barlow Semi Condensed and Terminus (SIL OFL 1.1).

Felucca's own assets that are not under its GPL (the icon atlas, the panel photo, the
Hügelton drum pack) are not part of this fork.

M-VAVE and FM-1 are trademarks of their respective owners. TR-808, TR-909 and TB-303 are
Roland trademarks, used here only to describe what the parts emulate. X0X is not
affiliated with or endorsed by M-VAVE, Roland or Hügelton Instruments.
