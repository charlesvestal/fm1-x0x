# X0X for the M-VAVE FM-1 — Manual

Firmware version 0.1. This manual covers everything on the instrument. Commands you type on
a computer are shown `like this`.

**Status.** X0X has been built and tested on a computer, in a simulator that runs the same
code as the instrument. It has not yet been run on an FM-1. Read "Installing" before you
install it.

---

## Contents

1. What X0X is
2. Installing, and going back
3. The panel
4. The screen
5. Getting around
6. The 909 and the 808
7. The two 303s
8. TB-3PO
9. The break
10. Patterns
11. Recording
12. Effects
13. Master
14. Global settings and saving
15. MIDI
16. Your own breaks
17. Update mode and calibration
18. Parameter reference
19. Credits and licence

---

## 1. What X0X is

X0X turns the FM-1 into a groovebox with five parts that always run together:

| Part | What it is |
|---|---|
| **909** | A TR-909-style drum machine: 11 tracks. Kick, snare, toms, rim and clap are circuit models; the hats and cymbals are samples, as on the original. |
| **808** | A TR-808-style drum machine: 11 tracks holding 16 sounds. The toms switch to congas, the rim to claves and the clap to maracas, as on the original. Every sound is synthesized. |
| **303A**, **303B** | Two TB-303-style bass lines, each with its own sound, its own line and its own TB-3PO generator. |
| **BREAK** | A breakbeat generator: a drum loop cut into eight slices and rearranged as it plays. |

There are 16 patterns. A pattern holds all five parts. The parts share a reverb and a delay,
and everything goes through a master section with a compressor, a filter and a limiter.

---

## 2. Installing, and going back

You need a computer with Python 3 and the `mido` and `python-rtmidi` packages
(`pip3 install mido python-rtmidi`), and the firmware file `x0x.fwsc`.

1. Connect the FM-1 to the computer with USB.
2. Run `python3 tools/fm1_install.py x0x.fwsc` and follow what it says.
3. The FM-1 restarts into X0X.

The hosted web installer at hugelton.github.io installs Felucca, not X0X.

**Going back.** M-VAVE's own updater puts the official firmware back. If an install is cut
off and the FM-1 no longer starts, Felucca's recovery tool (FM-1-transporter) can still
reach it.

Installing any third-party firmware is at your own risk.

---

## 3. The panel

```
 MASTER  SELECT            screen            KNOB1  KNOB2  KNOB3  KNOB4
 PRESETS ALGORITHM                           FX  SEL  ENV  LFO  EDIT  GLO
 OCT-  OCT+                                  HOME SAVE ARP SEQ  PLAY  REC
               [ 11 black keys ]
               [ 16 white keys ]
```

| Control | Use |
|---|---|
| MASTER | Volume. |
| SELECT | Move: to the next or previous page of knobs, or up and down a list. Hold HOME and turn SELECT to change the tempo. |
| ALGORITHM | Choose the part: 909, 808, 303A, 303B, BREAK. In a list, it changes the highlighted value. |
| PRESETS | Choose the next pattern. It starts at the end of the bar. |
| KNOB 1 to 4 | Change the four values shown at the bottom of the screen. |
| SEL | Open the list of everything on the current screen. In a list, run the highlighted action. In a question, answer yes. |
| HOME | The HOME screen. From a list or a question, go back. |
| EDIT | The part's own screen. Press again for its next page of knobs. |
| ARP | TB-3PO, the bass line generator (303A and 303B only). |
| FX | Effects: sends, reverb, delay. |
| LFO | MIX: levels, the compressor, the filter. |
| GLO | Global settings. |
| SEQ | On a 303: switch the keys between steps and keyboard. |
| PLAY | Start and stop. |
| REC | Record on and off. |
| SAVE | Save everything. |
| ENV, LFO | Held, they change what the keys do (accent, slide). |
| OCT-, OCT+ | Steps 1 to 16 or 17 to 32. On a 303 keyboard: octave. In TB-3PO: mutate and new line. |
| White keys | Steps. On HOME: patterns. |
| Black keys | The drum machine's 11 tracks, or the break's slice pads. On HOME: mutes. |

The second button is printed SEL. (Felucca calls it SCL.)

---

## 4. The screen

The screen has three areas.

**The top line** shows:

- the part, in its colour (909 orange, 808 red, 303A green, 303B blue, BREAK violet), or
  the name of the screen you are on;
- the selected drum track, or TB-3PO or KEYS;
- small dots for the pages of knobs, the current one lit;
- the pattern playing (P3) and the next one if you have chosen it (>5);
- the tempo (amber when it follows an external clock), a play triangle, and a red dot while
  recording. A small grey dot means there are changes that are not saved.

When a button does something, the top line tells you, in the part's colour, for about a
second: SAVED, COPIED TO P5, MUTED 808, and so on.

**The middle** shows the part or screen you are on.

**The bottom** shows four knobs: the name of each value, a ring showing its position, and
the value. They always match KNOB 1 to 4. A value with a fixed set of choices shows a row of
small squares instead of a ring.

When you turn a knob, its cell lights up and the value is shown large across the middle of
the screen for about a second.

---

## 5. Getting around

The same controls do the same things on every screen:

- **ALGORITHM** chooses the part.
- **PRESETS** chooses the next pattern.
- **SELECT** moves between pages (and between rows in a list).
- **KNOB 1 to 4** change the values at the bottom.
- **SEL** opens, **HOME** goes back.

**Pages.** Each screen has a few pages of four values. Turn SELECT, or press the screen's
button again (EDIT, FX, LFO, ARP), to go to the next page.

**Lists.** Press SEL to see every value of the current screen as a list. Turn SELECT to
move, turn ALGORITHM to change the highlighted value. Values with a bar under them are
continuous; a return arrow on the right marks an action, which SEL runs. HOME closes the
list.

**Questions.** Anything that would lose notes asks first, across the screen. SEL answers
yes, HOME answers no.

---

## 6. The 909 and the 808

Turn ALGORITHM to 909 or 808.

The screen shows the 11 tracks and the accent row (AC), 16 steps at a time. The selected
track is highlighted on the left; its steps are bright, the other tracks' steps are dim. The
white bar that moves is the playhead.

**Playing.** Each black key is a track. Pressing one plays it and selects it.

| Black key | 909 | 808 |
|---|---|---|
| 1 | BD bass drum | BD bass drum |
| 2 | SD snare | SD snare |
| 3 | LT low tom | LT low tom or low conga |
| 4 | MT mid tom | MT mid tom or mid conga |
| 5 | HT high tom | HT high tom or high conga |
| 6 | RS rim shot | RS rim shot or claves |
| 7 | CP hand clap | CP hand clap or maracas |
| 8 | CH closed hat | CB cowbell |
| 9 | OH open hat | CY cymbal |
| 10 | CR crash | OH open hat |
| 11 | RD ride | CH closed hat |

On the 808, the choice between tom and conga (and rim or claves, clap or maracas) is the
track's SOUND setting.

The closed and open hats cut each other off. On the 808, CHOKE on the KIT page sets this.

**Programming.** Press a white key to put the selected track on that step, or take it off.
Hold ENV and press white keys to set accents on those steps instead. An accented step plays
all its drums louder.

A pattern can be up to 32 steps long. OCT- shows steps 1 to 16, OCT+ steps 17 to 32. The
key lights show the selected track's steps; the step that is playing blinks.

**Pages.** EDIT goes through the selected track's own values (four per page), then:

- SENDS: the whole drum machine's reverb and delay send, and its level.
- PART: LENGTH (1 to 32 steps), RATE (1/16, 1/16 triplet, 1/32, 1/8 triplet), SWING and
  ACCENT.
- KIT: values for the whole kit.

The drum values are 0 to 127, like the knobs on the original machines. Every track also has
DRIVE and a choice of distortion (DIST), and all but the kick have their own reverb and
delay sends (Rev, Dly). See the parameter reference.

---

## 7. The two 303s

Turn ALGORITHM to 303A or 303B. The two are the same; each has its own sound and line.

The screen shows the line: each step's note as a bar, higher notes higher, with lines at
the octaves of the root. Below it, the accent row (AC) and the slide row (SL), then the
scale and the TB-3PO seed.

**Steps.** With the keys on steps (the normal state), the white keys are the 16 steps.

- Tap a white key to turn its step on or off.
- Hold a white key and turn the knobs to edit that step: KNOB 1 note, KNOB 2 gate (REST,
  NOTE or TIE), KNOB 3 accent, KNOB 4 slide. You hear the note as you hold it.

A **tie** holds the previous note through the step. A **slide** glides from this step's note
into the next one. An **accent** plays the note louder and brighter.

**Keyboard.** Press SEQ to turn the keys into a keyboard (KEYS shows on the top line; SEQ
lights). All 27 keys play the 303. OCT- and OCT+ change the octave. Playing one key while
another is held slides between them. Press SEQ again to go back to steps.

**Pages.** EDIT goes through:

- FILTER: CUTOFF, RESO, ENVMOD, DECAY.
- VOICE: ACCENT, WAVE (saw or square), TUNE, VOLUME.
- DRIVE: DRIVE, DRIVE TYPE (off, soft, RAT), SLIDE time, ACCENT DECAY.
- SENDS: reverb send, delay send, level.
- LINE: LENGTH (1 to 32), RATE, DIRECTION (forward, reverse, ping-pong, random), TRANSPOSE.

---

## 8. TB-3PO

TB-3PO writes 303 lines for you. Choose 303A or 303B and press ARP.

- **OCT+** writes a new line from a new seed. The top line shows the seed.
- **OCT-** mutates the line: about a quarter of the steps change.

Pages:

- GENERATE: DENS (how many steps play), ACCENT (how many are accented), SLIDE (how many
  slide), OCTS (1 to 3 octaves).
- SCALE: ROOT, SCALE (minor, Phrygian, harmonic minor, minor pentatonic, Dorian, major),
  OCTAVE (where the line sits), MUTATE (mutate on its own every 1 to 16 bars, or OFF).
- LINE: the same as the 303's LINE page.

Changing DENS, ACCENT, SLIDE, OCTS, ROOT, SCALE or OCTAVE rewrites the line from the same
seed, so the line follows the settings. **This replaces any steps you have edited by hand.**
MUTATE does not rewrite the line.

The same seed and settings always give the same line, and the same line as the TB-3PO
module for Schwung.

---

## 9. The break

Turn ALGORITHM to BREAK.

The screen shows loop A's waveform, cut into its eight slices. The slice that is playing is
lit. Below: the steps the break plays on, the loops in use, and what the generator is doing
(PLAYING A or B, RETRIG).

**Steps.** The white keys turn the break on or off on each of the 16 steps of the bar. With
all of them on, the break plays all the time.

**Playing.** Hold black keys 1 to 8 to play slices 1 to 8 yourself; the generator takes
over again when you let go. Hold 9 to play backwards, 10 at half speed, 11 to stutter.

**How it plays.** Each time a slice is due, the generator chooses which one:

- COMPLEXITY: how often it picks a slice other than the next one in order.
- ANCHOR: keeps the kick and snare slices on beats 1 and 3.
- ROLL: repeats a slice, or steps to its neighbour.
- FILL: how much the last bar of a phrase breaks these rules.
- RETRIG 2X, 3X, 4X, 8X: the chance, per bar, of stuttering a beat.
- PHRASE: the length of a phrase (2, 4, 8 or 16 bars, or off).
- B CHANCE: the chance of playing loop B on the fill bar.
- A LENGTH, B LENGTH: how often a slice is chosen (1/4 bar to 8 bars).

These settings belong to the pattern, so each pattern can have its own groove.

The LOOPS page chooses loop A and loop B. X0X has two loops of its own (909 GR and 909 FL,
played by its 909). Loops you upload follow them; see "Your own breaks". The loop is
stretched to the tempo. PITCH on the LOOPS page shifts it up or down.

---

## 10. Patterns

There are 16 patterns. A pattern holds all five parts: the drum steps, both 303 lines, the
break's steps and settings, each part's length and rate, and the swing.

The sounds (the knob settings of the drums and 303s, the effects and the master) are not
part of a pattern: they stay the same when the pattern changes.

**Choosing.** Turn PRESETS, or press HOME and press a white key. The new pattern starts at
the end of the bar. Its number shows after the current one (P3 >5) until it starts. When
stopped, it changes at once.

**Chains.** On HOME, hold two white keys: the patterns from the first to the second play in
turn, one bar each. Choosing a single pattern ends the chain.

**Copying.** Hold SAVE and press a white key: the current pattern is copied to that one. If
that pattern has notes, you are asked first.

**Clearing.** Hold SAVE and press REC: the current part is cleared in this pattern (you are
asked first). GLO > CLEAR PATTERN clears all five parts.

**Lengths.** Each part has its own length, so parts of different lengths go in and out of
step with each other. The 909's length sets the bar: a new pattern starts when the 909
comes round.

**Swing** delays every second 16th note. 50% is straight, 75% is the most.

**HOME** shows the 16 patterns along the top (the playing one white, patterns with notes
grey), the five parts with their steps, and the playheads. The knobs are TEMPO, SWING,
PUMP and CUTOFF, the four you most often reach for while playing. Black keys 1 to 5 mute
and unmute the five parts; the lit ones are playing. Mutes are not saved.

---

## 11. Recording

Press REC to arm recording (the red dot on the top line), and again to stop.

**Drums.** While playing, with REC armed, the black keys write hits at the step that is
playing. Hold ENV to write accented hits.

**303, live.** With the keys on keyboard (SEQ) and REC armed, play while the pattern runs:
the notes are written at the step that is playing.

**303, step by step.** With the keys on keyboard, stop the pattern and arm REC. Each key you
press writes the next step, starting from step 1:

- hold ENV while you press a key for an accent;
- hold LFO while you press a key for a slide;
- tap ENV on its own for a rest;
- hold LFO and press OCT- or OCT+ for a tie.

The screen shows which step is written next.

---

## 12. Effects

Press FX. There is one reverb and one delay; every part sends to them.

**Sends.** The first three pages are the sends: reverb and delay for each part. The drum
machines' tracks also have their own sends (on each track's page), on top of the part's.

**Reverb.** DECAY, TONE, HPF (keeps low end out of the reverb), LEVEL.

**Delay.** TIME (in note values, from 1/32 to a dotted half note, following the tempo),
FEEDBACK, TONE, LEVEL.

**Tape.** TYPE switches the delay between DIGI, a clean digital delay with 12-bit grain, and
TAPE, which wobbles and saturates its repeats and can run into self-oscillation at high
feedback. WEAR sets how worn the tape is. HPF keeps low end out of the delay.

**Kit drive.** VOLUME, DIST, DRIVE and COMP: the 909's own drive and glue compressor, on the
whole mix.

The FX screen shows every part's sends, the delay time and type, the feedback and the
reverb decay.

---

## 13. Master

Press LFO for the MIX screen: a level strip and meter for each part, and the compressor's
gain reduction (GR, in red).

**Levels.** LEVEL for each part. 0 dB is unity.

**Compressor.**

| Value | Range |
|---|---|
| THRESH | -48 to 0 dB |
| RATIO | 1:1 (off) to 20:1, and INF |
| ATTACK | 0.1 to 100 ms |
| RELEASE | 10 to 1500 ms |
| MAKEUP | 0 to 24 dB |
| MIX | dry to compressed (for parallel compression) |

At a ratio of 1:1 with PUMP at 0 the compressor does nothing at all.

**PUMP** ducks the whole mix every time a kick plays, by up to 24 dB, and lets it swell back
over the RELEASE time. PUMP BY chooses the kick: the 909's, the 808's, or both. An accented
kick pumps harder. PUMP works without the kick being heard, and with the RATIO at 1:1.

**Filter.** FILTER (off, low pass, band pass, high pass), CUTOFF and RESO, on the whole mix.
CUTOFF is also on HOME.

**Limiter.** LIMIT keeps the output from clipping. Leave it on.

---

## 14. Global settings and saving

Press GLO. Turn SELECT to move, ALGORITHM to change, SEL to run an action, HOME or GLO to
leave.

| Setting | |
|---|---|
| CLOCK OUT | Send MIDI clock and start/stop when X0X runs on its own tempo. |
| NOTES OUT | Send the patterns as MIDI notes. |
| KEY LIGHTS | Show steps on the key lights. |
| THEME | The colour of values and highlights: green, amber, cyan, red, mono. |
| ACCENT | How loud an unaccented drum hit is, compared with an accented one. |
| SAVE PROJECT | Save. |
| CLEAR PATTERN | Clear the current pattern (asks first). |
| FACTORY RESET | Put back the factory sounds and patterns (asks first; your saved project stays saved until you save again). |
| ABOUT X0X | The version and the audio load. |

**Saving.** Press SAVE (or GLO > SAVE PROJECT) to save the sounds, the 16 patterns, the
tempo and the settings. They come back at power on. Changes that are not saved are lost when
the FM-1 is switched off; the grey dot on the top line reminds you.

---

## 15. MIDI

X0X appears on a computer as a USB MIDI device called "X0X FM-1".

**In.**

| Channel | |
|---|---|
| 10 | Plays the 909 (General MIDI drum notes: 36 kick, 38 snare, 41, 45, 50 toms, 37 rim, 39 clap, 42 closed hat, 46 open hat, 49 crash, 51 ride). |
| 11 | Plays the 808 (as above; 56 cowbell, 49 cymbal). |
| 2 | Plays 303A. A note played while another is held slides. Velocity 100 or more is an accent. |
| 3 | Plays 303B, the same way. |
| 4 | Notes 36 to 43 play the break's slices 1 to 8. |
| clock | MIDI clock, start, stop and continue. X0X follows an external clock on its own and goes back to its own tempo half a second after the clock stops. |

**Out.** MIDI clock and start/stop when X0X runs on its own tempo (CLOCK OUT), and, if
NOTES OUT is on, the patterns on the same channels as above.

---

## 16. Your own breaks

X0X itself contains no recorded breaks. You can put your own loops on the FM-1: it has
three sample slots of about 7 seconds each, and one bar of a break takes about a third of a
slot, so about nine one-bar loops fit.

On a computer, with the FM-1 connected:

```
python3 tools/upload_breaks.py --bars amen.wav think.wav funky.wav ...
```

`--bars` cuts one bar from each file (X0X works out how many bars a file holds from its
length). Without it, the files are used as they are. The tool fills slot 1 first and tells
you what each loop will be called on the FM-1: BR1.1, BR1.2, ..., BR2.1. They follow X0X's
own loops on the LOOPS page.

`--dry-run FOLDER` prepares everything without a FM-1 connected, so you can check what fits.
Uploading to a slot replaces what was in it.

If you have a copy of BB Gen (schwung-breakbeat) next to X0X, `--bbgen` uploads its classic
breaks.

Building them into the firmware instead is possible too: see BUILDING.md.

---

## 17. Update mode and calibration

**Update mode.** Hold OCT- and OCT+ for five seconds. A countdown shows from two seconds; let
go to cancel. The FM-1 then waits for a firmware update over USB.

**Calibration.** If a button or knob does the wrong thing, hold OCT- and OCT+ while you
switch the FM-1 on. The screen asks you to press each button and turn each knob in turn.
The result is saved.

---

## 18. Parameter reference

Drum values are 0 to 127 unless shown otherwise. DIST is one of DIODE, CLIP, SAT, BFZ,
PDIST, FOLD, CRUSH.

### 909

| Track | Values |
|---|---|
| BD | Tune, Attack, Decay, Level, Pitch depth, Pitch, Drive, Dist |
| SD | Tune, Tone, Snappy, Level, Drive, Dist, Rev, Dly |
| LT, MT, HT | Tune, Decay, Level, Attack, Drive, Dist, Rev, Dly |
| RS | Level, Tune, Drive, Dist, Rev, Dly |
| CP | Level, Tune, Tail, Drive, Dist, Rev, Dly |
| CH, OH | Decay, Level, Tune, Drive, Dist, Rev, Dly |
| CR, RD | Tune, Level, Decay, Drive, Dist, Rev, Dly |
| KIT | Accent (how loud an accent is), Velocity (how much velocity matters) |

### 808

| Track | Values |
|---|---|
| BD | Level, Tone, Decay, Tune, Attack, Drive, Dist |
| SD | Level, Tone, Snappy, Tune, Decay, Drive, Dist, Rev, Dly |
| LT, MT, HT | Level, Tune, Decay, Sound (tom or conga), Drive, Dist, Rev, Dly |
| RS | Level, Tune, Decay, Sound (rim or claves), Drive, Dist, Rev, Dly |
| CP | Level, Tune, Decay, Attack, Sound (clap or maracas), Drive, Dist, Rev, Dly |
| CB, CH | Level, Tune, Decay, Drive, Dist, Rev, Dly |
| CY, OH | Level, Decay, Tune, Drive, Dist, Rev, Dly |
| KIT | Level, Accent, Choke (off, closed cuts open, both) |

### 303

| Value | |
|---|---|
| Cutoff, Reso, EnvMod, Decay | The filter and its envelope. Decay is 200 to 2000 ms. |
| Accent | How much accents add. |
| Wave | Saw or square. |
| Tune | A = 400 to 480 Hz; the centre is 440 Hz. |
| Volume | |
| Drive, Drive type | Off, soft, or RAT (a distortion pedal). |
| Slide | Slide time, 2 to 360 ms. |
| Acc. decay | Decay of accented notes, 30 to 3000 ms. |

### Break

Complexity, Anchor, Roll, Fill (0 to 100); Retrig 2x, 3x, 4x, 8x (0 to 100); Phrase (off, 2,
4, 8, 16); B chance (0 to 100); A length, B length (1/4, 1/2, 1, 2, 4, 8 bars); Level; Pitch
(-12 to +12 semitones).

### Effects

| Page | Values |
|---|---|
| REVERB | Decay, Tone, HPF, Level |
| DELAY | Time (1/32, 1/16T, 1/16, 1/8T, 1/16., 1/8, 1/4T, 1/8., 1/4, 1/2T, 1/4., 1/2, 1/2.), Feedback, Tone, Level |
| TAPE | Type (DIGI, TAPE), Wear, HPF |
| KIT DRIVE | Volume, Dist (off and the seven types), Drive, Comp |

### Master

Thresh, Ratio, Attack, Release, Makeup, Mix, Pump, Pump by, Filter, Cutoff, Reso, Limit
(see "Master").

---

## 19. Credits and licence

X0X is free software under the GNU General Public License, version 3. Its source code is
available, and if you pass the firmware on, you must pass on the source as well.

- The platform (hardware layer, USB, update loader, installer, storage) is from
  **Felucca** by Leo Kuroshita (Hügelton Instruments).
- The 909 is ported from **9W9** by athousanddetails, after **ER-99** by Matthew Cieplak;
  its hat and cymbal samples are ER-99's.
- The 808 is ported from **8W8** by athousanddetails.
- The 303s are ported from **schwung-303**: Open303 by Robin Schmidt (MIT licence), with
  extensions after jc303 and dm-Rat.
- TB-3PO is ported from **schwung-tb3po**, itself from the Phazerville Hemisphere Suite's
  TB-3PO.
- The break generator is ported from **BB Gen** by mestela, with permission.
- Fonts: Barlow Semi Condensed and Terminus, under the SIL Open Font License.

M-VAVE and FM-1 are trademarks of their owners. TR-808, TR-909 and TB-303 are Roland
trademarks, named only to say what the parts are modelled on. X0X is not connected with
M-VAVE, Roland or Hügelton Instruments.
