# X0X on the M-Vave SMK-37

X0X also builds for the M-Vave SMK-37, a 37-key controller with the same chip as the FM-1. The FM-1 build is
unchanged; everything for the SMK-37 is behind `BOARD_SMK37`.

Until this project's releases include SMK-37 packages, unofficial builds can be installed with the web installer:
https://genkigenki.github.io/smk37/

## Supported models

- **SMK-37 Pro**: the default build (package identity `SMK-37 Pro_900`).
- **SMK-37 Elite**: `MODEL=elite` (identity `SMK-37 Elite_900`).
- **MKE-P37**: `MODEL=mke` (identity `MKE-P37_900`).
- **Starrykey 37 Play**: uses the SMK-37 Pro build.

All of them are the same hardware. Each build carries its model's name, so M-Vave's updater still takes that
unit's stock package later: install the package for your model. The installer takes a package for any of these
models, and refuses an FM-1 package (and an SMK package on an FM-1).

## Controls

The X0X manual still applies; the SMK-37's controls map onto the FM-1's like this.

| SMK-37 | X0X |
| --- | --- |
| Keys F3..G5 (the middle 27) | the FM-1's 27 keys |
| Keys C3..E3 and G#5..C6 (the outer 10) | nothing |
| 16 pads | the 16 white keys as a step grid: steps 1-8 on the top row, 9-16 on the bottom row (on HOME, the patterns). On a 303 while REC is on, the 303's keyboard, low notes on the bottom row. Lit in the colour of the part on screen |
| K1 | MASTER (volume) |
| K2 / K3 / K4 | SELECT / ALGORITHM / PRESETS |
| K5..K8 | KNOB 1..4 |
| FX, SCALE, CHORD, NOTE REPEAT, PARA, GLOBE | FX, SEL, ENV, LFO, EDIT, GLO |
| PATCH / BT | HOME / SAVE |
| ARP / SEQ | ARP / SEQ |
| SEQ PLAY / SEQ REC | PLAY / REC |
| OCTAVE- / OCTAVE+ | OCT- / OCT+ |
| < / > | one step of SELECT back / forward (the page, a list's row); not while HOME is held |
| FADER BANK | switches the faders between bank A and bank B (lit on B) |
| Faders 1..4, bank A | levels of the 909, the 808, 303 A, 303 B |
| Faders 1..4, bank B | BREAK level, reverb return, delay return, master FILTER (middle off, left low-pass, right high-pass) |
| Pitch wheel | on a 303 (the part on screen): its TUNE, about +-1.5 semitones, back when let go; other parts: nothing |
| Mod wheel | on a 303: opens its CUTOFF from the saved value up to the top; other parts: nothing |
| KNOB BANK | nothing |
| OCTAVE- + OCTAVE+ held at power-on | HARDWARE CALIBRATION: teach the buttons and knobs K2..K8 again |

A fader takes over only once it moves (also after a bank change). The faders set the same values as the knobs
(saved with the project; recorded as knob motion while REC is on). The wheels change only what you hear, not
what is saved. The keys and pads play without velocity, as on the FM-1.

The SMK-37's TRS jack is a MIDI output and this build does not use it; there is no TRS MIDI IN. MIDI works over USB.

## Build

See [BUILDING.md](BUILDING.md) for the toolchain.

```sh
BOARD=smk37 ./build.sh                 # build/x0x-smk37.fwsc (SMK-37 Pro, Starrykey 37 Play)
BOARD=smk37 MODEL=elite ./build.sh     # build/x0x-smk37-elite.fwsc
BOARD=smk37 MODEL=mke ./build.sh       # build/x0x-smk37-mke.fwsc
```

The FM-1 build (`./build.sh`) is kept apart and stays as it was.

## Install

Connect the keyboard by USB and run, with the package for your model:

```sh
python3 tools/fm1_install.py build/x0x-smk37.fwsc
```

`fm1_install.py` needs `mido` and `python-rtmidi` (`pip3 install mido python-rtmidi`). It checks that the package
is for the connected model.

## Going back to the stock firmware

Install M-Vave's firmware package for your model with M-Vave's updater, M-UPGRADE. It works directly, because
the installed package keeps the keyboard's model name.

## Safety

- Back up first: have M-Vave's stock package for your model and M-UPGRADE ready before you install.
- Keep the USB cable connected until the install has finished and the keyboard has restarted.
- The first start clears the stock firmware's settings, and any other firmware's data, in the shared data area.
- The firmware never writes the keyboard's calibration, Bluetooth pairing or key areas; the flash map refuses
  them at compile time and again in the flash driver.
- Installing other firmware is at your own risk.
