# X0X

Groovebox firmware for the M-VAVE FM-1, version 0.1. It gives you a 909, an 808, two 303s
and a break, all running at once, each on its own pattern, with a song mode and recorded knob
moves.

**Status.** X0X runs in a simulator on a computer, using the same code as the FM-1. Nobody
has run it on an FM-1 yet.

---

## Contents

1. Install
2. Controls
3. The screen
4. 909 and 808
5. 303A and 303B
6. TB-3PO
7. Break
8. Patterns
9. Song
10. Recording
11. Knob motion
12. Effects
13. Mix and master
14. Settings and saving
15. MIDI
16. Your own breaks
17. Update mode and calibration
18. Parameters
19. Credits

---

## 1. Install

You need Python 3 with `mido` and `python-rtmidi` (`pip3 install mido python-rtmidi`). Connect
the FM-1 over USB and run:

```
python3 tools/fm1_install.py x0x.fwsc
```

The FM-1 restarts into X0X. The web installer on hugelton.github.io installs Felucca, not
X0X.

To go back, use M-VAVE's own updater. If an install is cut off and the FM-1 won't start,
Felucca's FM-1-transporter can still reach it. Third-party firmware is at your own risk.

---

## 2. Controls

![The FM-1 (a drawing), running X0X](img/fm1-panel.svg)

![What the keys do](img/fm1-keys.svg)

These work the same on every screen:

| | |
|---|---|
| ALGORITHM | Pick the part: 909, 808, 303A, 303B, BREAK. |
| PRESETS | Pick the next pattern: for the part on screen, or on HOME for all five. |
| SELECT | Next or previous page. In a list, move. Hold HOME and turn it to set the tempo. |
| KNOB 1–4 | The four values at the bottom of the screen. |
| SEL | Show everything on this screen as a list. In a list, run an action. In a question, yes. |
| HOME | Home. From a list or a question, back (or no). |
| EDIT | The part. Press again for the next page. |
| ARP | TB-3PO (303s only). |
| FX | Effects. |
| LFO | Mix and master. |
| GLO | Settings. |
| SEQ | On a 303, switch the keys between steps and keyboard. On HOME, the song. |
| PLAY, REC, SAVE | Start/stop, record, save. |
| ENV, LFO | Hold them to change what the keys do: accent, slide. |
| OCT-, OCT+ | Steps 1–16 or 17–32. Octave on the 303 keyboard. Mutate and new line in TB-3PO. |
| White keys | Steps. On HOME, patterns. |
| Black keys | Drum tracks, or the break's slices. On HOME, mutes. |
| MASTER | Volume. |

The second button is printed SEL. Felucca calls it SCL.

In a list, ALGORITHM changes the highlighted value. A return arrow marks an action. Anything
that would lose notes asks first.

![A list](img/screen-list.png)
![A question](img/screen-ask.png)

---

## 3. The screen

The **top line** shows the part in its colour (909 orange, 808 red, 303A green, 303B blue,
BREAK violet), the selected track, dots for the pages, the pattern (P3, or P3 >5 when 5 is
next), the tempo, and whether it's playing or recording. The tempo turns amber when it
follows an external clock. A small grey dot means unsaved changes. When a button does
something, the top line says so for a second: SAVED, COPIED TO P5.

The **bottom** shows the four knobs. A value with a few fixed choices shows small squares
instead of a ring. Turn a knob and its value shows large for a second.

![The 909](img/screen-909.png)
![Turning a knob](img/screen-readout.png)

---

## 4. 909 and 808

Eleven tracks each, on the black keys:

| Key | 909 | 808 |
|---|---|---|
| 1 | BD | BD |
| 2 | SD | SD |
| 3–5 | LT MT HT | LT MT HT, or congas |
| 6 | RS | RS, or claves |
| 7 | CP | CP, or maracas |
| 8 | CH | CB |
| 9 | OH | CY |
| 10 | CR | OH |
| 11 | RD | CH |

On the 808, each track's SOUND setting picks tom or conga, rim or claves, clap or maracas.
The 909's hats and cymbals are samples, as on the original; everything else on both machines
is synthesized.

A black key plays and selects its track. A white key puts the track on that step or takes it
off. Hold ENV and press white keys to set accents instead; an accent makes every drum on the
step louder. Patterns go up to 32 steps: OCT- shows 1–16, OCT+ 17–32.

Closed and open hats cut each other off. On the 808, CHOKE on the KIT page sets this.

EDIT steps through the track's sound, then:

- SENDS: the part's reverb and delay sends, and its level.
- PART: LENGTH (1–32 steps), RATE (1/16, 1/16T, 1/32, 1/8T), SWING, ACCENT.
- KIT: settings for the whole kit.

---

## 5. 303A and 303B

Two identical 303s, each with its own sound and line.

![303A](img/screen-303.png)

**Steps.** Tap a white key to turn a step on or off. Hold it and turn the knobs to edit the
step: note, gate (REST, NOTE, TIE), accent, slide. A tie holds the previous note. A slide
glides into the next step's note.

**Keyboard.** SEQ turns all 27 keys into a keyboard; KEYS shows on the top line. OCT- and
OCT+ change octave. Playing a key while another is held slides. SEQ again goes back to steps.

EDIT pages:

- FILTER: CUTOFF, RESO, ENVMOD, DECAY.
- VOICE: ACCENT, WAVE (saw, square), TUNE, VOLUME.
- DRIVE: DRIVE, DRIVE TYPE (off, soft, RAT), SLIDE time, ACCENT DECAY.
- SENDS: reverb, delay, level.
- LINE: LENGTH (1–32), RATE, DIRECTION (forward, reverse, ping-pong, random), TRANSPOSE.

---

## 6. TB-3PO

TB-3PO writes 303 lines. On a 303, press ARP.

![TB-3PO](img/screen-tb3po.png)

**OCT+** writes a new line from a new seed. **OCT-** mutates the line, changing about a
quarter of the steps.

- GENERATE: DENS, ACCENT, SLIDE (how many steps of each), OCTS (1–3 octaves).
- SCALE: ROOT, SCALE (minor, Phrygian, harmonic minor, minor pentatonic, Dorian, major),
  OCTAVE, MUTATE (on its own every 1–16 bars, or off).
- LINE: as on the 303.

Changing anything on GENERATE or SCALE except MUTATE rewrites the line from the same seed,
**replacing steps you edited by hand.** The same seed and settings always give the same line,
the same one Schwung's TB-3PO module gives.

---

## 7. Break

A drum loop cut into eight slices and rearranged as it plays.

![The break, slice 5 playing](img/screen-break.png)

White keys turn the break on or off on each of the bar's 16 steps. Hold black keys 1–8 to
play slices yourself; the generator takes over when you let go. Hold 9 for reverse, 10 for
half speed, 11 to stutter.

What the generator does:

- COMPLEXITY: how often it jumps to a slice other than the next.
- ANCHOR: keeps the kick and snare slices on beats 1 and 3.
- ROLL: repeats a slice, or steps to its neighbour.
- FILL: how much the last bar of a phrase breaks these rules.
- RETRIG 2X, 3X, 4X, 8X: the chance per bar of stuttering a beat.
- PHRASE: 2, 4, 8 or 16 bars, or off.
- B CHANCE: the chance of loop B on the fill bar.
- A LENGTH, B LENGTH: how often a slice is chosen, 1/4 bar to 8 bars.

These are saved with the pattern. The LOOPS page picks loops A and B and has PITCH. X0X comes
with two loops of its own, 909 GR and 909 FL, played by its 909; yours come after them.
Loops stretch to the tempo.

---

## 8. Patterns

![HOME](img/screen-home.png)

There are 16 patterns, and each part plays its own: the 303s can be on pattern 3 while the
drums are on 7. A pattern holds each part's steps, lengths and rates, the break's settings,
the swing and the knob motion. Sounds, effects and the master stay the same when the pattern
changes.

On HOME, each pattern box shows a coloured bar for every part playing it.

- **One part:** on that part's screen, turn PRESETS.
- **All five:** turn PRESETS on HOME, or press a white key on HOME.
- **Chain:** on HOME, hold two white keys. The patterns between them play one bar each, all
  five parts together. Picking a single pattern ends the chain.
- **Copy:** hold SAVE and press a white key. On a part's screen this copies that part; on
  HOME, all five as they're playing now.
- **Clear a part:** hold SAVE and press REC. GLO > CLEAR PATTERN clears all five.
- **Mute:** on HOME, black keys 1–5. Mutes aren't saved, except in a song.

A new pattern starts at the end of the bar; when stopped, at once. A part that changes
pattern starts again from step 1; the others keep going. Parts can have different lengths and
drift against each other. The 909's length is the bar, and the 909's pattern sets the swing.
Swing delays every second 16th: 50% is straight, 75% the most.

HOME's knobs are TEMPO, SWING, PUMP and CUTOFF.

---

## 9. Song

![SONG](img/screen-song.png)

A song is a list of bars, up to 192. Each bar says which pattern each part plays and which
parts are muted. Press SEQ on HOME to open it.

- **Write bars:** press a white key. The selected bar plays that pattern on all five parts,
  and the next bar is selected. Past the end, this adds bars.
- **Edit a bar:** KNOB 1 picks the bar; the other knobs set each part's pattern.
- **Mute in a bar:** black keys 1–5.
- **Play it:** set MODE to SONG (second page, or GLO), then PLAY. It starts at the selected
  bar and loops. The top line shows S and the bar.
- **Record it:** in SONG mode, arm REC and play. Change patterns and mutes as you like; each
  bar is written as it plays, from the start bar on, and the song grows past its end. This
  overwrites what was there.

SEL on the SONG screen has LENGTH, INSERT BAR, DELETE BAR and CLEAR SONG.

---

## 10. Recording

REC arms recording; press it again to stop.

- **Drums:** play the black keys while the pattern runs. Hold ENV for accents.
- **303, live:** in keyboard mode, play while the pattern runs.
- **303, step by step:** in keyboard mode, stopped. Each key fills the next step, starting
  from step 1. Hold ENV for an accent, LFO for a slide. Tap ENV alone for a rest; hold LFO
  and press OCT- or OCT+ for a tie.

---

## 11. Knob motion

![A recorded cutoff sweep playing](img/screen-motion.png)

Arm REC, play, and turn any sound knob: the move is recorded into the pattern, on the steps
where you moved it. Short pauses are filled in, so a slow sweep has no gaps. Playback glides
between steps rather than jumping.

A knob with motion has a dot in its corner, and its ring follows what plays.

- Steps you didn't touch play the knob's own setting.
- Turn the knob without recording and it takes over for one pass of the pattern.
- **Clear:** hold SAVE and turn the knob.
- Motion belongs to the part's pattern: the 303's knobs to the 303's, the effects and master
  to the 909's. It follows copies and clears.
- Stopping puts every knob back to its own setting.

There is room for 160 recorded knobs across all patterns. When it's full, the top line says
MOTION FULL.

---

## 12. Effects

![FX](img/screen-fx.png)

One reverb and one delay, shared by every part. FX opens on the sends: reverb and delay for
each part. Drum tracks also have their own sends, on their own pages.

- REVERB: DECAY, TONE, HPF, LEVEL.
- DELAY: TIME (in note values, 1/32 to a dotted half), FEEDBACK, TONE, LEVEL.
- TAPE: TYPE is DIGI (clean, 12-bit) or TAPE (wobbles, saturates, self-oscillates at high
  feedback). WEAR sets how worn the tape is. HPF.
- KIT DRIVE: the 909's drive and glue compressor, on the whole mix.

---

## 13. Mix and master

![MIX, with the compressor pumping](img/screen-mix.png)

LFO opens MIX: a level and meter for each part, and the compressor's gain reduction in red.

The factory mix is set up for big beat: the break up front with the 909 kick under it, the
303s through their RAT distortion into a tape delay, the clap in the reverb, and the master
compressor squeezing hard (4:1 from -24 dB, +16 dB into the limiter) with 4 dB of pump from
the 909 kick. Turn the ratio to 1:1 and PUMP to 0 for a clean mix.

| Compressor | |
|---|---|
| THRESH | -48 to 0 dB |
| RATIO | 1:1 (off) to 20:1, and INF |
| ATTACK | 0.1 to 100 ms |
| RELEASE | 10 to 1500 ms |
| MAKEUP | 0 to 24 dB |
| MIX | dry to wet, for parallel compression |

**PUMP** ducks the whole mix on each kick, by up to 24 dB, and lets it swell back over the
release. PUMP BY picks the kick: 909, 808 or both. Accented kicks pump harder. It works with
the ratio at 1:1, and with the kick muted.

**FILTER** is off, low pass, band pass or high pass, with CUTOFF and RESO. **LIMIT** stops
clipping; leave it on.

---

## 14. Settings and saving

![GLOBAL](img/screen-global.png)

GLO:

| | |
|---|---|
| CLOCK OUT | Send MIDI clock and start/stop. |
| NOTES OUT | Send the patterns as MIDI notes. |
| KEY LIGHTS | Show steps on the keys. |
| THEME | Green, amber, cyan, red, mono. |
| ACCENT | How loud an unaccented hit is next to an accented one. |
| MODE | PATTERN, or SONG: the song plays. |
| SAVE PROJECT | Save. |
| CLEAR PATTERN | Clear the whole pattern. |
| FACTORY RESET | Factory sounds and patterns, no song, no motion. Your saved project survives until you save. |
| ABOUT X0X | Version and audio load. |

**SAVE** saves the sounds, the 16 patterns, the song, the knob motion, the tempo and the
settings. Anything unsaved is lost at power off.

---

## 15. MIDI

The FM-1 shows up as "X0X FM-1".

| In | |
|---|---|
| ch 10 | 909: 36 kick, 38 snare, 41 45 50 toms, 37 rim, 39 clap, 42 closed hat, 46 open hat, 49 crash, 51 ride. |
| ch 11 | 808: the same, plus 56 cowbell and 49 cymbal. |
| ch 2 | 303A. Overlapping notes slide; velocity 100 and up accents. |
| ch 3 | 303B, the same. |
| ch 4 | Notes 36–43 play break slices 1–8. |
| clock | Followed automatically. X0X goes back to its own tempo half a second after the clock stops. |

Out: clock and start/stop (CLOCK OUT) and the patterns on the channels above (NOTES OUT).

---

## 16. Your own breaks

X0X ships no recorded breaks. The FM-1 has three sample slots of about 7 seconds; a one-bar
loop takes about a third of one, so about nine fit. With the FM-1 connected:

```
python3 tools/upload_breaks.py --bars amen.wav think.wav funky.wav ...
```

`--bars` takes one bar from each file. Without it, files go up as they are. Loops are named
BR1.1, BR1.2 ... BR2.1 and show up after X0X's own. Uploading to a slot replaces what was
there. `--dry-run FOLDER` checks what fits without the FM-1. `--bbgen` uploads BB Gen's
classic breaks if you have schwung-breakbeat next to X0X.

To build loops into the firmware instead, see BUILDING.md.

---

## 17. Update mode and calibration

**Update mode:** hold OCT- and OCT+ for five seconds. Let go during the countdown to cancel.

**Calibration:** if a button or knob does the wrong thing, hold OCT- and OCT+ while switching
on, then press and turn each control when asked.

---

## 18. Parameters

Drum values are 0–127. DIST is DIODE, CLIP, SAT, BFZ, PDIST, FOLD or CRUSH.

| 909 | |
|---|---|
| BD | Tune, Attack, Decay, Level, Pitch depth, Pitch, Drive, Dist |
| SD | Tune, Tone, Snappy, Level, Drive, Dist, Rev, Dly |
| LT MT HT | Tune, Decay, Level, Attack, Drive, Dist, Rev, Dly |
| RS | Level, Tune, Drive, Dist, Rev, Dly |
| CP | Level, Tune, Tail, Drive, Dist, Rev, Dly |
| CH OH | Decay, Level, Tune, Drive, Dist, Rev, Dly |
| CR RD | Tune, Level, Decay, Drive, Dist, Rev, Dly |
| KIT | Accent, Velocity |

| 808 | |
|---|---|
| BD | Level, Tone, Decay, Tune, Attack, Drive, Dist |
| SD | Level, Tone, Snappy, Tune, Decay, Drive, Dist, Rev, Dly |
| LT MT HT | Level, Tune, Decay, Sound, Drive, Dist, Rev, Dly |
| RS | Level, Tune, Decay, Sound, Drive, Dist, Rev, Dly |
| CP | Level, Tune, Decay, Attack, Sound, Drive, Dist, Rev, Dly |
| CB CH | Level, Tune, Decay, Drive, Dist, Rev, Dly |
| CY OH | Level, Decay, Tune, Drive, Dist, Rev, Dly |
| KIT | Level, Accent, Choke (off, closed cuts open, both) |

| 303 | |
|---|---|
| Cutoff, Reso, EnvMod | |
| Decay | 200–2000 ms |
| Accent | |
| Wave | Saw, square |
| Tune | A = 400–480 Hz, centre 440 |
| Volume | |
| Drive, Drive type | Off, soft, RAT |
| Slide | 2–360 ms |
| Acc. decay | 30–3000 ms |

| Break | |
|---|---|
| Complexity, Anchor, Roll, Fill | 0–100 |
| Retrig 2x 3x 4x 8x | 0–100 |
| Phrase | Off, 2, 4, 8, 16 |
| B chance | 0–100 |
| A length, B length | 1/4, 1/2, 1, 2, 4, 8 bars |
| Level | |
| Pitch | -12 to +12 semitones |

| Effects | |
|---|---|
| REVERB | Decay, Tone, HPF, Level |
| DELAY | Time (1/32, 1/16T, 1/16, 1/8T, 1/16., 1/8, 1/4T, 1/8., 1/4, 1/2T, 1/4., 1/2, 1/2.), Feedback, Tone, Level |
| TAPE | Type (DIGI, TAPE), Wear, HPF |
| KIT DRIVE | Volume, Dist, Drive, Comp |

---

## 19. Credits

X0X is free software under the GPL, version 3. If you pass the firmware on, pass on the
source too.

- Platform (hardware layer, USB, update loader, installer, storage): **Felucca** by Leo
  Kuroshita, Hügelton Instruments.
- 909: **9W9** by athousanddetails, after **ER-99** by Matthew Cieplak, whose hat and cymbal
  samples it uses.
- 808: **8W8** by athousanddetails.
- 303: **schwung-303**, built on Open303 by Robin Schmidt (MIT), with extensions after jc303
  and dm-Rat.
- TB-3PO: **schwung-tb3po**, from the Phazerville Hemisphere Suite.
- Break generator: **BB Gen** by mestela, used with permission.
- Fonts: Barlow Semi Condensed and Terminus (SIL Open Font License).

M-VAVE and FM-1 belong to their owners; TR-808, TR-909 and TB-303 are Roland trademarks. X0X
isn't connected with M-VAVE, Roland or Hügelton Instruments.
