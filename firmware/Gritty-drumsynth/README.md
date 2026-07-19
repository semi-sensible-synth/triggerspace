# Gritty-drumsynth

A **synthesised drum-machine** firmware for the *triggerspace* (NanoGris-MIDI)
Eurorack module. It keeps Grids' topographic drum **sequencer**, clock and MIDI,
but replaces the raw voltage-trigger outputs with three internally **synthesised
808/909-style drum voices** — kick, snare and hat — rendered as 1-bit PDM on the
BD/SD/HH jacks.

Lineage: Mutable Instruments **Grids** (Émilie Gillet) → **Gritty-Grids** (Sonic
Insurgence, improved MIDI) → **triggerspace** pin remap (Andrew Perry) → this
synth fork. GPLv3.

> This firmware is **mandatory** for triggerspace — the PCB moves the clock input
> off D1 onto D8, freeing the D0/D1 UART for TRS-A MIDI, so stock Grids firmware
> will not run unmodified. To run on original Grids hardware instead, set
> `TRIGGERSPACE_PINOUT 0` in `grids/hardware_config.h` (moves the clock back to D1;
> MIDI is then unavailable).

## How it sounds / signal path

Each drum jack carries a **1-bit sigma-delta (PDM)** bitstream at a 31.25 kHz
carrier. The voices are true synthesis (DDS sine + LFSR noise + exponential
envelopes), oversampled by the modulator, so a clean low kick is possible from a
single output pin.

**The raw output is a 5 V 1-bit stream and sounds fuzzy on its own** — it needs a
low-pass to reconstruct. Options:

- Patch each jack through a Eurorack **filter/VCA** module, or
- Fit a tiny **RC "dongle"** on the jack: the ~1 kΩ series resistor already on
  the board + a small cap to ground (≈ 1–10 nF → ~16–80 kHz corner) tames the
  carrier. This is the only outstanding hardware item; everything else is
  software.

Outputs are 5 V logic level — AC-couple / attenuate for a Eurorack audio input.

## Controls

A **long-hold on the TAP button** (~0.5 s) cycles three edit pages:

```
PERFORM ──hold──▶ META ──hold──▶ VOICE ──hold──▶ PERFORM (saves on the way out)
 (play)      (Grids meta-params)  (drum tuning)
```

The clock, sequencer and audio **keep running in every page**, so edits preview
live on the playing pattern.

### PERFORM (normal play) — stock Grids performance UI

| Control | Function |
|---------|----------|
| **BD / SD / HH density** | Per-instrument hit density (fill) |
| **X** | Map position X (pattern morph) |
| **Y** | Map position Y (pattern morph) |
| **Randomness** | Pattern chaos **+ per-hit humanisation** (see below) |
| **Tempo** | Internal BPM. Fully left = external/MIDI clock mode |
| **TAP (short)** | Tap tempo / reset; mute-unmute in MIDI-clock mode |
| **Clock / Reset in** | External clock & reset (voltage-trigger jumper mode) |

### VOICE (drum tuning) — the six knobs edit **one voice at a time**

Enter with two long-holds. A **short TAP cycles the target voice** BD → SD → HH;
the selected voice's channel LED **blinks** (with the CLOCK LED lit) to show which
one you're editing. Knobs use *catch* behaviour — a knob only takes over once you
move it. Changes are **saved to EEPROM** when you long-hold back to PERFORM.

| Knob | Parameter | Kick (BD) | Snare (SD) | Hat (HH) |
|------|-----------|-----------|------------|----------|
| **BD density** | Pitch (~20 Hz–2 kHz) | body pitch | body pitch | metal-tone pitch |
| **SD density** | Body/amp length (~16–320 ms) | decay | body decay | — |
| **HH density** | Pitch-env depth | "punch" (sweep) | — | — |
| **X** | Tonal level | — | body vs noise | **noise↔metal blend** |
| **Y** | Noise level | — | snare noise | overall hat level |
| **Randomness** | Noise length (~16–320 ms) | — | noise tail | hat decay |
| **Tempo** | Pitch-env time | kick sweep time | — | — |

Notes: the kick is a pitch-swept sine (no noise); the snare is a sine body + a
high-passed-noise layer; the hat is high-passed noise blended with a cheap
two-oscillator **metallic tone** (turn **X** up on the hat to bring in the 808-ish
clang, set its pitch with **BD density**). A "—" means that param doesn't apply to
that voice.

### META (Grids meta-parameters)

Enter with one long-hold. Move a knob to set its parameter (LEDs indicate state):

| Knob | Meta-parameter |
|------|----------------|
| **BD density** | Clock resolution |
| **SD density** | Tap-tempo on/off |
| **HH density** | Swing on/off |
| **X** | Output mode (drums / accent-clock-reset) |
| **Y** | Gate mode |
| **Randomness** | Clock output on/off |

## Per-hit humanisation (Randomness knob)

In PERFORM mode the Randomness knob does double duty: besides pattern chaos it
injects **per-hit variation** into the voices, scaled by the knob (fully left =
identical hits):

- **Pitch:** ±2% max, and only across the **top ~80%** of the knob's travel (the
  bottom fifth leaves pitch locked).
- **Level (velocity)** and **decay length:** up to **±20%**.

## MIDI

MIDI I/O is on the TRS jacks (D0/D1, 31250 baud). Drum channel is **10**.

- **Clock sync:** responds to MIDI Start / Stop / Continue / Clock (turn Tempo
  fully left to enter external/MIDI clock mode; TAP mutes/unmutes).
- **Note-in triggers voices** (independent of the sequencer): note **36** = kick,
  **38** = snare, **42** = closed hat, **46** = open hat (plus GM neighbours).
  Velocity ≥ 96 fires the accent layer.
- **Note-out:** the sequencer also sends GM drum notes (36/38/42/46) on ch 10.
- **CC live voice tuning** (any time, independent of edit page):

  | CC | Parameter | CC | Parameter |
  |----|-----------|----|-----------|
  | 20 | Kick pitch | 24 | Snare noise mix |
  | 21 | Kick decay | 25 | Snare noise decay |
  | 22 | Snare tone (body pitch) | 26 | Hat decay (open/closed) |
  | 23 | Snare body decay | 27 | Hat level |

## Build & flash

AVR C++ for ATmega328P at **16 MHz** (the board's actual crystal — already set in
the `makefile`). From this directory:

```bash
rm -rf build && make            # -> build/grids/grids.hex   (make clean leaves stale objects)
```

Flash with a **USBasp** ISP programmer (the Nano serial bootloader does not work
on this board — `-c arduino`/ttyUSB0 always fails):

```bash
avrdude -C /etc/avrdude.conf -p atmega328p -c usbasp \
        -U flash:w:build/grids/grids.hex:i
```

Flashing can fail with the Nano seated in the module — pull it out to flash if
needed. On Ubuntu, `brltty` may grab a CH34x USB device; stop/mask it first.

### Diagnostics

`grids.cc` has a `//#define DRUMSYNTH_TEST_TONE` near the top: uncomment it to
force a continuous 220 Hz tone on all three jacks, bypassing the sequencer — handy
for verifying the audio ISR / PDM / output path in isolation. Leave it commented
for normal use.

## Credits & license

- Original **Grids** design and code: **Émilie Gillet**, Mutable Instruments —
  released open source, with thanks.
- **Gritty-Grids** MIDI improvements: **Sonic Insurgence**.
- triggerspace remap & **Gritty-drumsynth**: **Andrew Perry**.

GPLv3 — this program is free software; redistribute/modify under the GNU General
Public License v3 or (at your option) any later version.
