909 cymbal samples: hh.wav (hi-hat, open and closed), ride.wav, crash.wav.

Source: ER-99 by Matthew Cieplak (https://github.com/matthewcieplak/er-99),
shipped unchanged by 9W9 (Charles Vestal), which this firmware's drum engine
is ported from. Licence: GPL-3.0 (ER-99 and 9W9 are GPL-3.0).

tools/gen_drum_samples.py converts them to int16 C arrays in
build/gen/x0x_drum_samples.h (hh.wav is 24-bit and is rounded to 16 bits).
