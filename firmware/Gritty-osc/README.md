# Gritty-osc

A **six-voice oscillator** firmware for the *triggerspace* (NanoGris-MIDI)
Eurorack module. Unlike [Gritty-drumsynth](../Gritty-drumsynth), it **drops the
drum sequencer** and turns the module into a polyphonic square/pulse (PWM) DDS
oscillator with a built-in chord engine and arpeggiator, playable from the panel
knobs and CV inputs — or from MIDI (build-time modes, see [MIDI](#midi-build-time)).

Lineage: Mutable Instruments **Grids** (Émilie Gillet) → **Gritty-Grids** (Sonic
Insurgence) → **triggerspace** pin remap (Andrew Perry) → this oscillator fork.
GPLv3.

> Since the triggerspace hardware remaps the Nano pins (clock moved off D1 to D8,
> freeing the D0/D1 UART for MIDI), this firmware will not run unmodified on stock
> Grids or clones. Set `TRIGGERSPACE_PINOUT 0` in `grids/hardware_config.h` to
> build for original Grids hardware (clock back on D1; MIDI then unavailable).

## How it sounds / signal path

Six independent **DDS voices** (16-bit phase accumulators), each packed into one
bit of the 74HC595 and shifted out at **Fs = 62.5 kHz**. Each output is 1-bit, so
every jack is a **pulse/square** — pitch is exact (DDS), and the only continuous
per-voice timbre control is **PWM duty** (50% = square). Clean to a few kHz,
usable to ~8 kHz (about 5–6 octaves).

Output routing (matches the Grids state byte / jack wiring):

| Jack (bit) | Voice |
|------------|-------|
| CH1 `0x01` / CH2 `0x02` / CH3 `0x04` | Main voices 0 / 1 / 2 |
| CH1_ACC `0x08` / CH2_ACC `0x10` / CH3_ACC `0x20` | Voices 3 / 4 / 5 = **sub-octaves** of the mains |
| CLOCK `0x40` | **PDM mono mix** of all six voices (1-bit sigma-delta) |
| RND `0x80` | unused |

The **mix on the CLOCK jack** only works with that jack's PCB jumper in
voltage-trigger (not MIDI) mode; low-pass it externally (the ~1 kΩ series R + a
small cap, e.g. 1–2.2 nF, cleans up the carrier). All outputs are 5 V logic —
filter / AC-couple / attenuate for Eurorack audio.

## Controls (default build — "Option B")

| Control | Function |
|---------|----------|
| **Y** knob + CV | Root pitch |
| **Randomness (chaos)** knob + CV | Chord type (8: unison "superpulse" / power / maj / min / sus4 / …) |
| **BD / SD / HH density** knobs | Per-channel **PWM duty** of the three chord tones |
| **X** knob + CV | Detune — first half spreads the sub-octaves, second half also detunes the mains (ensemble / supersaw) |
| **Tempo** | Arp range, 1–4 octaves (ARP trigger mode only) |
| **CLOCK in (D8)** | Arp **step** (advance) |
| **RESET in (D2)** | Arp **reset** to step 0 |
| **Button (D3)** | Cycle arp **direction mode** (readout on LEDs, 1–6) |

CV inputs sum into the pots, so every knob parameter is also CV-modulatable.

### Trigger function (build-time)

The CLOCK / RESET / button inputs drive one of three behaviours, selected by a
`#define` in `grids.cc` (default `OSC_TRIG_ARP`):

- **`OSC_TRIG_ARP`** — CLOCK steps through the chord tones across octaves (chord =
  the note pool); all six voices play the stepping note. TEMPO sets the range
  (1–4 oct). The button cycles direction: up / down / up-down inclusive / up-down
  exclusive / random / random-walk, shown on the LEDs as 1–6. RESET restarts.
- **`OSC_TRIG_ROTATE`** — chord keeps sounding; each step rotates the voicing /
  inversion.
- **`OSC_TRIG_PLUCK`** — chord keeps sounding; each step fires a decaying
  PWM-duty envelope (percussive re-articulation).

### Alternate mapping (build-time)

Define **`OSC_DENSITY_AS_PITCH`** to switch from the chord engine to independent
per-channel pitch: the three density knobs become per-channel pitch, TEMPO becomes
transpose (±2 oct), X becomes a global PWM duty, and Y / chaos are unused.

## Pitch / tuning

Pitch comes from a precomputed **note → tuning-word table** (equal temperament,
generated for Fs = 62.5 kHz) — no floating-point `powf` at runtime, just a flash
lookup. Verified against a tuner on the 16 MHz board.

## MIDI (build-time)

The six voices can be played from the **D0/D1 TRS-A MIDI input** instead of the
knob/CV + trigger control. Define **exactly one** `OSC_MIDI_*` mode near the top
of `grids.cc` (all are commented out by default = the knob/arp oscillator above).
MIDI needs `TRIGGERSPACE_PINOUT` (the D0/D1 UART); the trigger/arp handling is
compiled out in a MIDI build.

| Mode `#define` | Behaviour |
|----------------|-----------|
| **`OSC_MIDI_MONO_ROOT`** (A) | One channel. The (last) held note sets the **root**; the panel still shapes it — chaos = chord type, X = detune, density knobs = per-channel PWM. A full six-voice chord from a single key. |
| **`OSC_MIDI_MULTI_3CH`** (B) | Three channels (base, base+1, base+2) drive **osc 0 / 1 / 2** independently; each sub-oscillator tracks its main an octave down. |
| **`OSC_MIDI_POLY3`** (C1) | **3-voice** polyphony, notes round-robined over osc 0/1/2; each sub doubles its main an octave down. |
| **`OSC_MIDI_POLY6`** (C2) | **6-voice** polyphony, notes round-robined over all six oscillators (subs are independent voices, no doubling). |
| **`OSC_MIDI_SPLIT`** (D) | **Split keyboard**: notes ≥ `OSC_SPLIT_NOTE` round-robin over the mains (0/1/2); notes below are a bassline round-robined over the subs (3/4/5). |

Config `#define`s (also in `grids.cc` / `hardware_config.h`):

- **`OSC_MIDI_CHANNEL`** — base MIDI channel, 0-based (`0` = channel 1). Mode B
  also uses `base+1` and `base+2`.
- **`OSC_SPLIT_NOTE`** — split point for mode D, as a MIDI note (`48` = C3, with
  60 = middle C/C4). Try `60` if that suits the keyboard.

Voice allocation prefers a free voice, else steals round-robin. A released note
silences its voice (its pulse width drops to zero — no audio) rather than gating
an envelope (there is no VCA/envelope yet). The **firing channel's LED blinks**
on each note (`OSC_LED_FLASH_LOOPS` sets the length), so the round-robin
allocation reads as the BD/SD/HH LEDs cycling. The clock-jack mix is unchanged.
Common CCs:

| CC | Function |
|----|----------|
| **1** (mod wheel) | Global **PWM duty** (0 = square → up = thinner/brighter). No internal LFO yet, so the mod wheel drives PWM instead of vibrato. |
| **94** (detune depth) | Detune amount in modes A/B (spreads main vs sub). |
| **123** (all notes off) | Silences all voices. |

> Note velocity, pitch bend and per-voice envelopes are not handled yet; these
> modes are a first pass (see `plans/gritty-osc.md`).

## Build & flash

AVR C++ for ATmega328P at **16 MHz** (already set in the `makefile`). From this
directory:

```bash
rm -rf build && make            # -> build/grids/grids.hex   (make clean leaves stale objects)
```

Flash with a **USBasp** ISP programmer (the Nano serial bootloader does not work
on this board):

```bash
avrdude -C /etc/avrdude.conf -p atmega328p -c usbasp \
        -U flash:w:build/grids/grids.hex:i
```

Flashing can fail with the Nano seated in the module — pull it out to flash if
needed. On Ubuntu, `brltty` may grab a CH34x USB device; stop/mask it first.

## Credits & license

- Original **Grids** design and code: **Émilie Gillet**, Mutable Instruments —
  released open source, with thanks.
- **Gritty-Grids** MIDI improvements: **Sonic Insurgence**.
- triggerspace remap & **Gritty-osc**: **Andrew Perry**.

GPLv3 — this program is free software; redistribute/modify under the GNU General
Public License v3 or (at your option) any later version.
