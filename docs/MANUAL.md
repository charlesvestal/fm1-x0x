# X0X

X0X is groovebox firmware for the M-VAVE FM-1, version 0.1. It turns the FM-1 into a 909, an
808, two 303s and a breakbeat player, all running at the same time. Each part can play its own
pattern, you can arrange patterns into a song, and you can record knob moves.

**Status.** X0X has only been run in a simulator on a computer, using the same code as the
FM-1. Nobody has run it on an FM-1 yet.

---

## Contents

1. Install
2. Getting started
3. Controls
4. The screen
5. 909 and 808
6. 303A and 303B
7. TB-3PO
8. Break
9. Patterns
10. Recording
11. Knob motion
12. Song
13. Effects
14. Mix and master
15. Settings and saving
16. MIDI
17. Troubleshooting
18. Parameters
19. Credits

---

## 1. Install

You can try X0X before installing it: **https://charlesvestal.github.io/fm1-x0x/emu/** runs the
same code in the browser, with sound, played with the mouse, a touch screen or the keyboard.

The easiest way to install X0X is the web installer at
**https://charlesvestal.github.io/fm1-x0x/install/**. It works in Chrome or Edge on a computer,
and you don't need to install anything else. Connect the FM-1 to the computer with a USB data
cable, open the page and press Install. Don't unplug the cable while it's writing. When it
finishes, the FM-1 restarts into X0X.

You can also install from the command line. Download the firmware file (for example
`x0x-0.4-beta.fwsc`) from the
[releases page](https://github.com/charlesvestal/fm1-x0x/releases), get the X0X source code from
GitHub, install Python 3 with the `mido` and `python-rtmidi` packages
(`pip3 install mido python-rtmidi`), and run:

```
python3 tools/fm1_install.py x0x-0.4-beta.fwsc
```

Note that the web installer on hugelton.github.io installs Felucca, not X0X.

To go back to the original firmware, use "Back to the stock firmware" at the bottom of the
web installer: download M-VAVE's FM-1 V15 file from the link there, choose it, and press the
button. (M-VAVE's own updater, M-UPGRADE, works too.) If the FM-1 no longer starts, see
[Recovering an FM-1 that won't start](#recovering-an-fm-1-that-won-t-start). Installing
third-party firmware is at your own risk.

---

## 2. Getting started

Here's a five-minute tour, from switching on to saving your first beat. When X0X starts,
the screen shows the 909, and every part is empty.

1. **Add a kick.** Press black key 1 to select the bass drum; you hear it. Then press white keys
   1, 5, 9 and 13.
2. **Press PLAY.** The kick plays on every beat.
3. **Add a clap.** Press black key 7, then white keys 5 and 13. While the pattern plays,
   selecting a drum is silent, so it doesn't add a stray hit.
4. **Add open hats.** Press black key 9, then white keys 3, 7, 11 and 15.
5. **Write a 303 line.** Turn ALGORITHM two clicks clockwise to 303A, press ARP, then press
   OCT+. Each press writes a new line; keep pressing until you like one.
6. **Shape the 303.** Press EDIT to show the FILTER page. Turn KNOB 1 to open or close the
   filter, and KNOB 2 to change the resonance.
7. **Add the break.** Turn ALGORITHM two more clicks to BREAK, then press all 16 white keys so
   the break plays the whole bar.
8. **Mute a part.** Press HOME, then black key 5 to mute the break. Press it again to bring it
   back.
9. **Record a filter sweep.** Turn ALGORITHM back to 303A. Press REC, slowly turn KNOB 1 for a
   bar or two, then press REC again. The sweep now plays every time the pattern comes round.
10. **Save.** Press SAVE. Everything you've done is kept when you switch off. (When the pattern is
    stopped, X0X also saves by itself after a few seconds; see section 15.)

From here, Patterns (section 9) explains how to make more patterns and switch between them,
and Song (section 12) how to arrange them.

---

## 3. Controls

![The FM-1 (a drawing), running X0X](img/fm1-panel.svg)

![What the keys do](img/fm1-keys.svg)

These controls do the same thing on every screen:

| Control | What it does |
|---|---|
| ALGORITHM | Chooses the part you're working on: 909, 808, 303A, 303B or BREAK. |
| PRESETS | Chooses the next pattern. On a part's screen it changes only that part; on HOME it changes all five. |
| SELECT | Moves to the next or previous page. In a list, it moves up and down. Hold HOME and turn SELECT to set the tempo. |
| KNOB 1–4 | Change the four values shown at the bottom of the screen. |
| SEL | Shows everything on the current screen as a list. In a list, it runs the highlighted action. When X0X asks a question, SEL means yes. |
| HOME | Goes to the HOME screen. In a list, it goes back. When X0X asks a question, HOME means no. |
| EDIT | Shows the part's own screen. Press it again for the next page. |
| ARP | Opens TB-3PO, the 303 line generator (on a 303 only). |
| FX | Opens the effects. |
| LFO | Opens the mixer and master. |
| GLO | Opens the settings. |
| SEQ | On a 303, switches the keys between steps and keyboard. On HOME, opens the song. |
| PLAY | Starts and stops. Hold HOME and press PLAY to redo. |
| REC | Turns recording on and off. Hold HOME and press REC to undo. |
| SAVE | Saves everything. |
| ENV, LFO | Hold one of these while pressing keys to add an accent (ENV) or a slide (LFO). |
| OCT-, OCT+ | Show steps 1–16 or 17–32. On the 303 keyboard they change the octave. In TB-3PO they mutate the line or write a new one. |
| White keys | Steps. On HOME, they choose patterns. |
| Black keys | Drum tracks, or the break's slices. On HOME, they mute parts. |
| MASTER | Volume. |

The second button is printed SEL on the panel. Felucca calls it SCL.

In a list, turn ALGORITHM to change the highlighted value. Rows with a return arrow are
actions: press SEL to run them. Before doing anything that would lose notes, X0X asks first.

**Help on the FM-1:** hold any button for a second without pressing anything else, and a card
shows what it does on the screen you're on, with its combinations (HOME's card, for example,
lists tempo, patterns, undo and redo). Let go and nothing else happens. The card goes away as
soon as you press a key, turn a knob or press another button, so holding a button to combine
it works as always.

**Undo:** hold HOME and press REC to take back the last change; the screen says what it undid,
for example UNDO 909 P1. Hold HOME and press PLAY to redo it. You can go back up to 32 steps
(fewer after very large changes). One step is one gesture: everything you did before pausing
for a moment, such as a few steps tapped in a row or one turn of a knob, or a whole recording
pass. Undo covers the sounds, the patterns, the song and the knob motion, including CLEAR
PATTERN and FACTORY RESET. It doesn't cover the tempo or the settings, and the history starts
again when you switch the FM-1 on.

![A list](img/screen-list.png)
![A question](img/screen-ask.png)

---

## 4. The screen

The **top line** shows, from left to right:

- the part, in its colour: 909 orange, 808 red, 303A green, 303B blue, BREAK violet;
- the selected drum track;
- small dots, one for each page, with the current page lit;
- the pattern playing, such as P3. If you've chosen a different pattern to play next, it
  appears after it: P3 >5 means pattern 5 is coming up;
- the tempo, which turns amber when X0X follows an external MIDI clock;
- a play symbol while playing, and a red dot while recording. A small grey dot means you
  have changes that aren't saved.

When you press a button that does something, the top line says what happened for about a
second, for example SAVED or COPIED TO P5.

The **bottom** of the screen shows the four knobs. Settings with only a few choices show a
row of small squares instead of a ring. When you turn a knob, its value is also shown in
large type for a second.

![The 909](img/screen-909.png)
![Turning a knob](img/screen-readout.png)

---

## 5. 909 and 808

Each drum machine has eleven tracks, one on each black key:

| Key | 909 | 808 |
|---|---|---|
| 1 | Bass drum (BD) | Bass drum (BD) |
| 2 | Snare (SD) | Snare (SD) |
| 3–5 | Low, mid and high tom | Low, mid and high tom, or congas |
| 6 | Rim shot (RS) | Rim shot, or claves |
| 7 | Clap (CP) | Clap, or maracas |
| 8 | Closed hat (CH) | Cowbell (CB) |
| 9 | Open hat (OH) | Cymbal (CY) |
| 10 | Crash (CR) | Open hat (OH) |
| 11 | Ride (RD) | Closed hat (CH) |

On the 808, the SOUND setting of a track switches it between tom and conga, rim and claves,
or clap and maracas. The 909's hats and cymbals are samples, as on the original machine.
Every other sound on both machines is synthesized.

Press a black key to select its track. When the pattern is stopped you also hear it; while it
plays, selecting is silent, so it doesn't add a hit to the groove (GLO > KEY SOUND set to ALWAYS
plays it every time). Then press white keys to turn the selected track on or off at those steps. To set accents instead, hold ENV while pressing white keys.
An accent makes every drum on that step louder.

A pattern can be up to 32 steps long. The white keys show 16 steps at a time: press OCT- for
steps 1–16 and OCT+ for steps 17–32.

The closed hat cuts off the open hat. On the 808 you can change this with CHOKE on the KIT
page.

Press EDIT to step through the selected track's sound settings, followed by these pages:

- SENDS: how much of the part goes to the reverb and the delay, and its level.
- PART: LENGTH (1–32 steps), RATE (1/16, 1/16 triplet, 1/32 or 1/8 triplet), SWING and
  ACCENT.
- KIT: settings for the whole kit.

---

## 6. 303A and 303B

There are two identical 303s. Each has its own sound and its own line.

![303A](img/screen-303.png)

**Steps.** Tap a white key to turn that step on or off. To edit a step, hold its key and turn
the knobs: KNOB 1 sets the note, KNOB 2 the gate (REST, NOTE or TIE), KNOB 3 the accent and
KNOB 4 the slide. A tie holds the previous note through the step. A slide glides from this
step's note into the next one.

**Keyboard.** Press SEQ to turn all 27 keys into a keyboard; KEYS appears on the top line.
OCT- and OCT+ change the octave. If you play a key while still holding another, the 303
slides between them. Press SEQ again to go back to steps.

Press EDIT to step through these pages:

- FILTER: CUTOFF, RESO, ENVMOD and DECAY.
- VOICE: ACCENT, WAVE (saw or square), TUNE and VOLUME.
- DRIVE: DRIVE, DRIVE TYPE (off, soft or RAT), SLIDE time and ACCENT DECAY.
- SENDS: reverb, delay and level.
- LINE: LENGTH (1–32 steps), RATE, DIRECTION (forward, reverse, ping-pong or random) and
  TRANSPOSE.

---

## 7. TB-3PO

TB-3PO writes 303 lines for you. Select a 303 and press ARP.

![TB-3PO](img/screen-tb3po.png)

Press **OCT+** to write a completely new line. Press **OCT-** to mutate the current line,
which changes about a quarter of its steps.

TB-3PO has three pages:

- GENERATE: DENS, ACCENT and SLIDE set how many steps play, are accented and slide. OCTS sets
  the range, from 1 to 3 octaves.
- SCALE: ROOT, SCALE (minor, Phrygian, harmonic minor, minor pentatonic, Dorian or major),
  OCTAVE, and MUTATE, which mutates the line by itself every 1–16 bars (or never).
- LINE: the same as the 303's LINE page.

Changing any setting on GENERATE or SCALE, other than MUTATE, writes the line again.
**This replaces any steps you edited by hand.** The same settings always produce the same
line, and it is the same line that Schwung's TB-3PO module would write.

---

## 8. Break

The break is a drum loop cut into eight slices, which X0X rearranges as it plays.

![The break, slice 5 playing](img/screen-break.png)

The white keys turn the break on or off for each of the bar's 16 steps. To play slices
yourself, hold black keys 1–8; when you let go, the generator takes over again. Hold key 9
to play backwards, key 10 to play at half speed, and key 11 to stutter.

These settings control how the generator rearranges the loop:

- COMPLEXITY: how often it jumps to a different slice instead of playing the next one.
- ANCHOR: how strongly it keeps the kick and snare slices on beats 1 and 3.
- ROLL: how often it repeats a slice or moves to the slice next to it.
- FILL: how much the last bar of a phrase breaks these rules.
- RETRIG 2X, 3X, 4X and 8X: the chance, in each bar, of stuttering a beat.
- PHRASE: the length of a phrase, 2, 4, 8 or 16 bars, or off.
- B CHANCE: the chance of switching to loop B in the last bar of a phrase.
- A LENGTH and B LENGTH: how often a new slice is chosen, from every 1/4 bar to every 8 bars.

These settings are saved with the pattern. The LOOPS page chooses loop A and loop B, and has
a PITCH setting. X0X comes with two loops of its own, 909 GR and 909 FL, played by its 909.
Loops you upload appear after them. Loops are stretched to fit the tempo.

### Your own breaks

X0X doesn't come with any recorded breaks, but you can load your own. The FM-1 has three
sample slots of about 7 seconds each. A one-bar loop takes about a third of a slot, so about
nine loops fit in total. With the FM-1 connected, run:

```
python3 tools/upload_breaks.py --bars amen.wav think.wav funky.wav ...
```

The `--bars` option takes one bar from each file; without it, each file is uploaded whole.
The loops are named BR1.1, BR1.2 and so on, up to BR2.1 and beyond, and they appear after
X0X's own loops. Uploading to a slot replaces whatever was in it.

To check what will fit without connecting the FM-1, add `--dry-run FOLDER`. If you have
schwung-breakbeat next to X0X, `--bbgen` uploads BB Gen's classic breaks.

To build loops into the firmware itself instead, see BUILDING.md.

---

## 9. Patterns

![HOME](img/screen-home.png)

There are 16 patterns. Each part plays its own pattern, so the 303s can play pattern 3 while
the drums play pattern 7. A pattern holds each part's steps, length and rate, the break's
settings, the swing and any knob motion. Sounds, effects and master settings are not part of
a pattern and don't change when the pattern does.

On HOME, each pattern box shows a coloured bar for every part that is playing it.

To choose patterns:

- **One part:** go to that part's screen and turn PRESETS.
- **All five parts:** turn PRESETS on HOME, or press a white key on HOME.
- **A chain:** on HOME, hold two white keys at once. All five parts then play each pattern
  from the first to the second, one bar each, and start again. Choosing a single pattern ends
  the chain.

If the music is playing, the new pattern starts when the current bar ends. If it's stopped,
the new pattern takes over straight away.

When a part changes pattern, it starts again from step 1. The other parts carry on where they
were.

To manage patterns:

- **Copy:** hold SAVE and press a white key to copy to that pattern. On a part's screen this
  copies only that part. On HOME it copies all five parts as they're playing now.
- **Clear one part:** hold SAVE and press REC. To clear all five parts, use GLO > CLEAR
  PATTERN.
- **Mute a part:** on HOME, press black keys 1–5. Mutes aren't saved, except inside a song.

Parts can have different lengths, so they drift against each other. The 909's length sets
the length of a bar, and the 909's pattern sets the swing. Swing delays every second 16th
note: 50% is straight, and 75% is the most swing.

On HOME, the knobs are TEMPO, SWING, PUMP and CUTOFF.

---

## 10. Recording

Press REC to turn recording on, and press it again to turn it off.

- **Drums:** while the pattern plays, press black keys to record hits (with REC on they always
  sound). Hold ENV to record
  accented hits.
- **303, live:** switch the 303 to keyboard mode and play while the pattern runs.
- **303, one step at a time:** switch the 303 to keyboard mode and stop playback. Each key you
  press fills the next step, starting from step 1. Hold ENV while pressing a key for an
  accent, or LFO for a slide. To enter a rest, tap ENV on its own. To enter a tie, hold LFO
  and press OCT- or OCT+.

---

## 11. Knob motion

![A recorded cutoff sweep playing](img/screen-motion.png)

You can record knob moves into a pattern. Turn on REC, press PLAY, and turn any sound knob.
X0X records the knob on the steps where you moved it. Short pauses are filled in, so a slow
sweep has no gaps. On playback, the value glides smoothly from step to step rather than
jumping.

A knob with recorded motion has a dot in its corner, and its ring follows the recorded
value while it plays.

- On steps where you didn't move the knob, the knob's own setting plays.
- If you turn the knob while not recording, your turn takes over for one pass of the
  pattern, and then the recording plays again.
- **To clear a knob's motion,** hold SAVE and turn the knob.
- Motion belongs to a part's pattern: the 303's knobs record into the 303's pattern, and the
  effects and master record into the 909's. Copying or clearing a pattern copies or clears
  its motion too.
- When you stop, every knob goes back to its own setting.

There is room for 160 recorded knobs across all the patterns. When there's no room left, the
top line says MOTION FULL.

---

## 12. Song

![SONG](img/screen-song.png)

A song is a list of up to 192 bars. Each bar says which pattern each part plays and which
parts are muted. To open the song, press SEQ on HOME.

- **Write bars:** press a white key. The selected bar gets that pattern for all five parts,
  and the next bar is selected. Pressing keys at the end of the song adds new bars.
- **Edit a bar:** turn KNOB 1 to select the bar, then use the other knobs to set each part's
  pattern for that bar.
- **Mute parts in a bar:** press black keys 1–5.
- **Play the song:** set MODE to SONG, either on the second page of the SONG screen or in GLO,
  then press PLAY. The song starts at the selected bar and loops. While it plays, the top
  line shows S and the current bar number.
- **Record the song:** in SONG mode, turn on REC and press PLAY. As it plays, change patterns
  and mute parts however you like. Each bar is written into the song as it goes by, starting
  from the bar you started on, and the song grows if you play past its end. Recording
  replaces what was in those bars before.

Press SEL on the SONG screen for LENGTH, INSERT BAR, DELETE BAR and CLEAR SONG.

---

## 13. Effects

![FX](img/screen-fx.png)

There is one reverb and one delay, and every part can send to both. Press FX: the first pages
set how much each part sends to the reverb and the delay. Each drum track also has its own
sends, on that track's own page.

- REVERB: DECAY, TONE, HPF and LEVEL.
- DELAY: TIME, in note values from 1/32 to a dotted half note, plus FEEDBACK, TONE and LEVEL.
- TAPE: TYPE switches the delay between DIGI, a clean 12-bit digital delay, and TAPE, which
  wobbles, saturates, and can feed back on itself at high feedback. WEAR sets how worn the
  tape sounds. HPF cuts low end from the delay.
- KIT DRIVE: the 909's drive and glue compressor, applied to the whole mix.

---

## 14. Mix and master

![MIX, with the compressor pumping](img/screen-mix.png)

Press LFO to open MIX. It shows a level and a meter for each part, and the compressor's gain
reduction in red.

The factory mix is set up for big beat: the break is up front with the 909 kick under it, the
303s run through RAT distortion into a tape delay, and the clap has reverb. The master
compressor squeezes hard, at 4:1 from -24 dB with 16 dB of makeup gain into the limiter, and
each 909 kick makes the whole mix pump by 4 dB. For a clean mix, set RATIO to 1:1 and PUMP
to 0.

| Compressor | Range |
|---|---|
| THRESH | -48 to 0 dB |
| RATIO | 1:1 (off) to 20:1, and INF |
| ATTACK | 0.1 to 100 ms |
| RELEASE | 10 to 1500 ms |
| MAKEUP | 0 to 24 dB |
| MIX | from dry to fully compressed, for parallel compression |

**PUMP** lowers the level of the whole mix each time the kick plays, by up to 24 dB, and lets
it swell back up over the compressor's release time. PUMP BY chooses which kick does this:
the 909's, the 808's, or both. Accented kicks pump harder. PUMP works even with the ratio at
1:1, and even when the kick is muted.

**FILTER** can be off, low pass, band pass or high pass, with CUTOFF and RESO. **LIMIT**
prevents clipping; leave it on.

---

## 15. Settings and saving

![GLOBAL](img/screen-global.png)

Press GLO for these settings:

| Setting | What it does |
|---|---|
| CLOCK OUT | Sends MIDI clock and start/stop messages. |
| NOTES OUT | Sends the patterns out as MIDI notes. |
| KEY LIGHTS | Shows the steps on the key lights. |
| KEY SOUND | STOPPED (the default): the black drum keys play their sound only when the pattern is stopped, or with REC on; while it plays they only select. ALWAYS: they always play. |
| THEME | The screen colour: green, amber, cyan, red or mono. |
| ACCENT | How loud an unaccented drum hit is compared with an accented one. |
| MODE | PATTERN, or SONG to play the song. |
| SAVE PROJECT | Saves everything. |
| AUTOSAVE | ON (the default): while the pattern is stopped, changes are saved by themselves once nothing has been touched for four seconds. OFF: only SAVE saves. |
| CLEAR PATTERN | Clears all five parts of the current pattern. |
| FACTORY RESET | Restores the factory sounds and empty patterns, with no song and no knob motion. Your saved project stays in memory until you save over it. |
| PERFORMANCE | Opens the performance page (below). |
| ABOUT X0X | Shows the version and the audio load. |

Press **SAVE** to save the sounds, all 16 patterns, the song, the knob motion, the tempo and
the settings. A small dot at the top right of the screen means there are changes that aren't
saved yet.

With AUTOSAVE on (the default), you rarely need to: whenever the pattern is stopped and you
haven't touched anything for four seconds, X0X saves your changes and says AUTOSAVED. It never
saves while the pattern plays, because writing to the FM-1's memory briefly silences the sound.
Only the parts that changed are written, so a save is quick. Anything not yet saved is lost when
you switch the FM-1 off. With AUTOSAVE off, only SAVE saves (turning it off saves once more, so
the setting is kept).

### Performance

GLO > PERFORMANCE shows how hard the FM-1 is working, updated every second: its clock speed, the
audio load and its peak, any dropouts, and how much of the processor each part uses.

To measure it properly, press SEL and choose RUN PERF TEST. X0X plays three patterns of its own
on the factory sound for about 15 seconds: a simple loop, all five parts, and everything as busy
as it gets. Then it puts back your patterns, sound and tempo, and shows a table. Your project
isn't changed. HOME or PLAY stops the test early.

X0X hasn't been run on an FM-1 before, so a photo of that table is the most useful thing you can
send us: it decides how X0X uses the FM-1's processor from here.

KNOB 1 switches on extra counters (STALLS). They're experimental: if the FM-1 misbehaves with
them on, switch it off and on again.

---

## 16. MIDI

When connected over USB, the FM-1 shows up as a MIDI device called "X0X FM-1".

X0X responds to this MIDI input:

| Input | What it plays |
|---|---|
| Channel 10 | The 909, with General MIDI drum notes: 36 kick, 38 snare, 41, 45 and 50 toms, 37 rim, 39 clap, 42 closed hat, 46 open hat, 49 crash, 51 ride. |
| Channel 11 | The 808, with the same notes, plus 56 for the cowbell and 49 for the cymbal. |
| Channel 2 | 303A. Overlapping notes slide, and a velocity of 100 or more plays an accent. |
| Channel 3 | 303B, in the same way. |
| Channel 4 | Notes 36–43 play break slices 1–8. |
| Clock | X0X follows an incoming MIDI clock automatically, and returns to its own tempo half a second after the clock stops. |

X0X can also send MIDI: clock and start/stop when CLOCK OUT is on, and the patterns as notes
on the channels above when NOTES OUT is on.

---

## 17. Troubleshooting

**Update mode:** hold OCT- and OCT+ together for five seconds. A countdown appears; let go
before it ends to cancel.

**Calibration:** if a button or knob does the wrong thing, hold OCT- and OCT+ while you switch
the FM-1 on. The screen then asks you to press each button and turn each knob in turn.

**After a crash:** X0X shows a red CRASH screen and restarts by itself a few seconds later. The
next start shows RESTARTED AFTER A CRASH and an address (PC ...) for a moment. If you report the
problem, include that address and what you were doing.

**Safe mode:** if X0X crashes twice in a row while starting, it starts in SAFE MODE instead: no
sound, but USB works, so the web installer can reach it. Reinstall X0X from the installer, or
put the stock firmware back from the same page. Press PLAY to try starting X0X normally again.
If even safe mode fails twice, the FM-1 goes into the chip's own update mode, which only the
rescue below can reach.

### Recovering an FM-1 that won't start

From version 0.5-beta, an FM-1 that keeps crashing starts in safe mode (above), and the web
installer can fix it. Older versions (0.2-beta did: the X0X screen, a red X0X CRASH screen, then a
blank screen) went into the chip's own update mode after two crashes, which neither the web
installer nor M-VAVE's updater can see. A script can put
M-VAVE's stock firmware back from there, on a Mac, with the USB cable you already have:

Open Terminal, paste this line and press Return:

```
bash -c "$(curl -fsSL https://raw.githubusercontent.com/charlesvestal/fm1-x0x/main/tools/fm1_rescue.sh)"
```

1. If a window asks to install the command line developer tools, click Install. When it has
   finished, paste the line again.
2. If it asks for a password, type your Mac password (nothing shows as you type) and press Return.
3. When it says it's waiting, plug the FM-1 in and switch it on. It crashes and goes blank, and
   the script catches it. It checks the chip, saves a backup of the FM-1's memory in the
   `fm1-rescue` folder in your home folder, and asks before it writes anything.
4. Type **yes** and press Return. Don't unplug anything until it says it's restarting the FM-1.
   The FM-1 then starts into its original firmware, and you can install X0X again from there.

The script downloads M-VAVE's FM-1 V15 firmware and JieLi's flash loader, and checks both against
known fingerprints. It writes only the firmware area of the memory, never the bootloader or the
FM-1's own settings, and checks everything it writes. Running it again is always safe. If it fails,
copy everything in the Terminal window and
[open an issue](https://github.com/charlesvestal/fm1-x0x/issues). If the FM-1 never shows up at
all, Felucca's [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter) (a small
RP2040 board wired to the USB lines) can still reach it.

---

## 18. Parameters

Drum settings range from 0 to 127. DIST, the distortion type, can be DIODE, CLIP, SAT, BFZ,
PDIST, FOLD or CRUSH.

| 909 track | Settings |
|---|---|
| BD | Tune, Attack, Decay, Level, Pitch depth, Pitch, Drive, Dist |
| SD | Tune, Tone, Snappy, Level, Drive, Dist, Rev, Dly |
| LT, MT, HT | Tune, Decay, Level, Attack, Drive, Dist, Rev, Dly |
| RS | Level, Tune, Drive, Dist, Rev, Dly |
| CP | Level, Tune, Tail, Drive, Dist, Rev, Dly |
| CH, OH | Decay, Level, Tune, Drive, Dist, Rev, Dly |
| CR, RD | Tune, Level, Decay, Drive, Dist, Rev, Dly |
| KIT | Accent, Velocity |

| 808 track | Settings |
|---|---|
| BD | Level, Tone, Decay, Tune, Attack, Drive, Dist |
| SD | Level, Tone, Snappy, Tune, Decay, Drive, Dist, Rev, Dly |
| LT, MT, HT | Level, Tune, Decay, Sound, Drive, Dist, Rev, Dly |
| RS | Level, Tune, Decay, Sound, Drive, Dist, Rev, Dly |
| CP | Level, Tune, Decay, Attack, Sound, Drive, Dist, Rev, Dly |
| CB, CH | Level, Tune, Decay, Drive, Dist, Rev, Dly |
| CY, OH | Level, Decay, Tune, Drive, Dist, Rev, Dly |
| KIT | Level, Accent, Choke (off, closed cuts open, or both) |

| 303 setting | Range |
|---|---|
| Cutoff, Reso, EnvMod | 0–127 |
| Decay | 200–2000 ms |
| Accent | 0–127 |
| Wave | Saw or square |
| Tune | A from 400 to 480 Hz; the middle is 440 Hz |
| Volume | 0–127 |
| Drive, Drive type | Off, soft or RAT |
| Slide | 2–360 ms |
| Accent decay | 30–3000 ms |

| Break setting | Range |
|---|---|
| Complexity, Anchor, Roll, Fill | 0–100 |
| Retrig 2x, 3x, 4x, 8x | 0–100 |
| Phrase | Off, 2, 4, 8 or 16 bars |
| B chance | 0–100 |
| A length, B length | 1/4, 1/2, 1, 2, 4 or 8 bars |
| Level | 0–127 |
| Pitch | -12 to +12 semitones |

| Effect page | Settings |
|---|---|
| REVERB | Decay, Tone, HPF, Level |
| DELAY | Time (1/32, 1/16T, 1/16, 1/8T, 1/16., 1/8, 1/4T, 1/8., 1/4, 1/2T, 1/4., 1/2, 1/2.), Feedback, Tone, Level |
| TAPE | Type (DIGI or TAPE), Wear, HPF |
| KIT DRIVE | Volume, Dist, Drive, Comp |

The delay follows the tempo, including an external MIDI clock. A delay time longer than two
seconds (1/2. below 90 BPM, 1/2 below 60) plays at half that length, which is still on the beat.
TAPE's Wear sets how much the echoes wow and flutter around the beat.

---

## 19. Credits

X0X is free software under the GNU General Public License, version 3. If you share the
firmware, you must also share its source code.

- The platform (hardware layer, USB, update loader, installer and storage) is from
  **Felucca** by Leo Kuroshita, Hügelton Instruments.
- The 909 is from **9W9** by athousanddetails, which is based on **ER-99** by Matthew
  Cieplak. Its hat and cymbal samples are ER-99's.
- The 808 is from **8W8** by athousanddetails.
- The 303 is from **schwung-303**, built on Open303 by Robin Schmidt (MIT licence), with
  extensions based on jc303 and dm-Rat.
- TB-3PO is from **schwung-tb3po**, which comes from the Phazerville Hemisphere Suite.
- The break generator is from **BB Gen** by mestela, used with permission.
- The fonts are Barlow Semi Condensed and Terminus (SIL Open Font License).

M-VAVE and FM-1 are trademarks of their owners, and TR-808, TR-909 and TB-303 are Roland
trademarks. X0X is not connected with M-VAVE, Roland or Hügelton Instruments.
