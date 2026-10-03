# Part patterns, song mode and knob motion

What separates X0X from ReBirth is not sound but structure: each machine plays its own
pattern, a song arranges them bar by bar, and knob moves are recorded. This adds all three.

## Budgets (measured, build of 4479afb)

- Image 558,316 B of the 581 KB app slot: about 23 KB for code. The UI work must stay small.
- RAM .data+.bss 39,244 B of 98,304. Pool 268,276 of 344,064.
- Flash: Felucca's data area is 0x97000..0xDFFFF. X0X uses 0x97000..0x9CFFF (SOUND, PAT0,
  PAT1) and 0xDC000 (PANEL). Free inside it: 0x9D000..0x9FFFF (3 sectors) and 0xDE000..0xDFFFF
  (2). 0xE0000..0xE4FFF is the update loader's staging area; above it nobody has said what
  is safe, so nothing goes there. Two new A/B objects fit: OBJ_SONG at 0x9D000, OBJ_MOTION
  at 0xDE000, 3836 payload bytes each after the format word.

## Part patterns

`seq.ppat[5]`: the pattern each part plays (tracks and parts share an index: 909, 808,
303A, 303B, BREAK). `seq.pcue[5]`: each part's cue. `seq.cur` / `seq.cue` go.

- All cues land at the bar (the 909 wrapping), as before. A part whose pattern changed
  restarts on step 1; the others keep running, so polymeter survives a part change.
- Whole-pattern cue (HOME white key, PRESETS on HOME / FX / MIX) cues all five. PRESETS on a
  part's own screen cues that part only.
- Chains step whole patterns, counting from the 909's.
- Swing is the 909's pattern's (the 909 is the bar).
- Copy (SAVE + white key) copies what you are looking at: on a part's screen that part, on
  HOME all five parts as they are playing now (each from its own pattern).
- No format change: `pattern_t` is untouched, so saved projects load as they are.

## Song

```
song_bar_t { uint8_t pat[5]; uint8_t mute; }    6 B: a pattern per part, part mute bits
song_t     { uint16_t len; uint8_t rsv[2]; song_bar_t bar[192]; }   1156 B
```

One entry per bar (192 bars = 6 min at 128 BPM). A bar is the 909's length.

- SONG mode on: at each bar the next entry sets every part's pattern and the part mutes. The
  song loops. PLAY starts at the selected bar.
- SONG mode + REC: live changes (cues, mutes) are written into the song from the start bar
  on, extending it past the end. Recording overwrites; stopping stops it.
- The SONG screen (SEQ on HOME): a table of bars. KNOB 1 picks the bar, the others its five
  patterns; white key = the whole bar plays that pattern, and the cursor moves on (fast
  entry); black keys 1-5 = that bar's mutes. SEL list: length, insert, delete, clear.

Song logic lives in the sequencer (`bar_end`), so tests/host/seq_test.c can drive it.

## Motion (knob recording)

A lane is one knob's value on each step of one part's pattern:

```
lane_t { used, gen, pat, part, t, v, i, rsv; uint8_t val[32]; }   40 B; val 0xFF = none
```

A pool of 160 lanes (6400 B) shared by all patterns, stored with the song across OBJ_SONG and
OBJ_MOTION (1156 + 6400 = 7556 of 7672 B).

- Owner part: 909/808 sounds -> that kit; 303 -> its part; BREAK -> BREAK; a mixer strip -> its
  part; FX and master -> the 909 (the bar).
- REC + playing + turn a sound knob: the next step takes the knob's value, so the lane holds
  the steps the knob MOVED on. A gap of up to 4 steps between two written steps is filled in a
  straight line (a slow sweep has no holes). While a gesture lasts (a turn within 4 steps) the
  knob, not the lane, is heard. Untouched steps keep what they had. (First version recorded a
  full pass from each turn; a sweep that ran into the next pass was then overwritten by where
  the knob stopped. Found by the song scenario.)
- Playback: a step with a value sets it; between two set steps the value ramps, per audio
  block, so sweeps are smooth rather than stepped at 1/16. A step without a value plays the
  knob's own setting (the base).
- Turn a knob that has a lane, not recording: the knob wins for one pass, then the lane
  takes over again.
- SAVE held + turn a knob: clear that knob's lane in this pattern.
- Stop, pattern change, clear: the parameter goes back to the knob's own value. The saved
  sound is never changed by playback (lanes write the engine, not `proj.sound`).
- Clearing a part or pattern clears its lanes; copying copies them.

The ISR owns the lane runtime (applied value, ramp, record / hold counters); the main loop
only allocates (fields first, `used` last), clears, and raises request flags. `gen` bumps on
allocation so the ISR resets a reused slot's runtime.

Song-level motion (one sweep across many bars of the same pattern) is not in this round: a
pattern carries its motion, so a song can vary it by using copies of the pattern.

## Tests

- seq_test: per-part cue at the bar, other parts not restarted; song playback order, loop,
  mutes; song record writes cues and mutes and extends the length.
- motion unit test: record a pass, play it back, ramp midpoint, hold on turn, clear restores
  base, pattern switch restores base, slot reuse resets runtime.
- Scenarios: song entry and playback, part cue, motion record and playback (`expect` on the
  engine's applied value), save + reboot keeps song and lanes.
