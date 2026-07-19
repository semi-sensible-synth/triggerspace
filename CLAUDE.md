# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`triggerspace` (aka "NanoGris-MIDI") is a Eurorack synthesizer module: a topographic drum-trigger
sequencer. It is a hardware remix of [NanoGris](https://github.com/Quinienl/NanoGris), itself based on
Mutable Instruments' [Grids](https://pichenettes.github.io/mutable-instruments-documentation/modules/grids/)
(Émilie Gillet), under CC BY-SA 3.0.

The repo contains two coupled deliverables:
- **Hardware** — a KiCad project (schematic + PCB + fabrication outputs) at the repo root.
- **Firmware** — an AVR C++ project in `Gritty-Grids/`, a fork that targets triggerspace's remapped
  Arduino Nano (ATmega328) pinout.

These two are tightly coupled: the PCB remaps Nano pins so custom firmware is *mandatory* (see below).

## Hardware (KiCad)

KiCad 7 project. Do not hand-edit the s-expression files (`triggerspace.kicad_sch`,
`triggerspace.kicad_pcb`, `triggerspace.kicad_pro`) — open them in KiCad. `-bak` files, `*.kicad_prl`,
and `triggerspace-backups/` are KiCad autosaves/backups and are gitignored.

Key files:
- `triggerspace.kicad_sch` / `.kicad_pcb` / `.kicad_pro` — the design.
- `triggerspace.pdf` — exported schematic.
- `bom/ibom.html` — interactive BOM (regenerate via the KiCad InteractiveHtmlBom plugin).
- `production/` — fabrication outputs (gerbers): `back_pcb/`, `jack_pcb/`, `front_panel_aluminium/`,
  `front_panel_black/`. This is a multi-board module (main/back PCB + jack PCB + front panel).
- `svg/`, `images/` — panel artwork and renders.
- `*-rescue.lib` + `sym-lib-table` — legacy symbol rescue libraries from KiCad version migration; leave
  in place (referenced by the schematic).

`fp-info-cache` is a KiCad footprint cache (gitignored).

## Firmware (Gritty-Grids/)

AVR C++ for ATmega328 at 20 MHz, built on Mutable Instruments' `avrlib`. `Gritty-Grids/` is a nested git
repo (not a submodule) — the firmware has its own git history; commit firmware changes from inside it.

Build and flash (run from `Gritty-Grids/`):
```bash
make                                  # builds build/grids/grids.hex

# Flash via Nano USB bootloader (CH340/CH341 clone on /dev/ttyUSB0):
avrdude -C /etc/avrdude.conf -v -p atmega328p -c arduino -P /dev/ttyUSB0 -b 57600 -D \
        -U flash:w:build/grids/grids.hex:i

# Flash via USBasp ISP programmer:
avrdude -C /etc/avrdude.conf -v -p atmega328p -c usbasp -u \
        -U flash:w:build/grids/grids.hex:i
```
On Ubuntu, `brltty` may claim the CH341 USB device — stop/mask its services first (see
`Gritty-Grids/README.md`). Flashing sometimes fails with the Nano seated in the module; pull it out to
flash if needed.

Firmware layout: `grids/grids.cc` is the main loop; `grids/pattern_generator.*` is the drum-pattern
engine; `grids/clock.*` handles timing; `grids/midi.*` handles MIDI I/O; `grids/resources.cc` holds
generated pattern/lookup tables. `grids/hardware_config.h` is the single source of truth for pin
assignments (see coupling below).

## Hardware/firmware coupling — the central constraint

The PCB uses **non-standard Nano pin assignments** vs. stock Grids/NanoGris, so stock firmware will not
work. The remapping exists to free up the hardware UART (D0/D1) for TRS-A MIDI I/O:

- Clock input moved **D1 → D8 (PB0)** so D1 (TX) can drive MIDI OUT.
- D0/D1 (`SerialPort0`) are now MIDI IN/OUT at 31250 baud.
- Reset (D2) and TAP button (D3) are read as individual `DigitalInput`s, not a packed nibble.
- CLOCK/RND front-panel jacks are repurposed as MIDI OUT/IN; solder/pin jumpers on the PCB switch them
  back to voltage triggers for stock firmware.

`Gritty-Grids/grids/hardware_config.h` encodes all of this. **Any change to Nano pin usage in the KiCad
schematic must be mirrored there, and vice versa.** MIDI OUT sends General MIDI drum notes on channel 10
(kick 36, snare 38, closed hat 42, open hat 46).

## Known hardware issues / roadmap

See the "build guide notes" and "Beyond v0.042" sections of `README.md` for bodges required on the
current revision (notably: Nano VIN must be fed from +12V, not the 5V rail) and planned next-revision
fixes. Consult these before altering the power section or panel layout.

`hardware_debugging/` (gitignored) holds logic-analyzer captures and photos from MIDI bring-up.
