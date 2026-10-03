# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

v0.5.0 is still in progress: changes are added to it until it is fabricated and
bench-verified.

## [0.5.0] - In progress

Not yet fabricated or bench-verified.

### Added

- RC reconstruction-cap sockets (`CH1_CAP1`, `CH1_ACC_CAP1`, `CH2_CAP1`, `CH2_ACC_CAP1`,
  `CH3_CAP1`, `CH3_ACC_CAP1`) on the jack PCB's six outputs, to filter the sigma-delta
  hiss from the Gritty-drumsynth firmware's synthesised drum voices.
- "RL" silkscreen labels on the LED resistors R25-R28, so they are easy to find and
  change.
- "MIDI IN"/"MIDI OUT" labels on the back silkscreen for the back PCB's 2-pin MIDI
  header (J9).
- Aluminium front-panel fabrication outputs (`production/front_panel_aluminium/`) and
  panel cutout artwork (`svg/`).
- `firmware/` directory: the [Gritty-Grids](https://github.com/semi-sensible-synth/Gritty-Grids)
  firmware as a git submodule, plus two in-tree forks - Gritty-drumsynth (synthesised
  808/909-style drum voices on the output jacks) and Gritty-osc (six-voice DDS chord
  oscillator with arpeggiator and MIDI note control).

### Changed

- Arduino Nano VIN (A1 pin 30) is now fed from +12V (from the U4 input pin) instead of
  the +5V rail, so the v0.042 VIN bodge is no longer needed. U4 still supplies the
  module's +5V rail; the Nano's own 5V output (pin 27) stays unconnected.
- LED resistors R25-R28 changed from 100r to 4.7k (the LEDs were too bright).
- U4 (L7805, TO-220) now uses a horizontal tab-down footprint: it lies flat on the back
  PCB with its M3 tab hole, and the GND pour under the tab (with stitching vias) acts as
  the heatsink. Only GND copper is under the tab on the top layer. C12 moved up slightly
  to make room. The whole v0.042 module (Nano included) drew 28-38mA from +12V, so U4
  dissipates at most (12-5)V x 38mA = ~0.27W and a separate heatsink shouldn't be needed.
- Back PCB outline moved 1mm to the right (as seen from the front panel). All parts and
  inter-board headers stay put, so the back PCB's left edge is now flush with the jack
  PCB's (previously it overhung by ~0.9mm).
- PCB layout revisions.
- Version on the schematic title block and PCB silkscreen updated to v0.5.0.

### Fixed

- The back PCB's 2-pin MIDI header existed only on the PCB and shared the reference J4
  with the jack PCB's MIDI-IN/RND-OUT jumper header. It is now J9 and is in the
  schematic.
- The front silkscreen labels on that header were swapped: pin 1 is D0/RX (MIDI IN) and
  pin 2 is D1/TX (MIDI OUT).

## [0.042] - 2024-12-31

### Added

- Initial release.

[0.5.0]: https://github.com/semi-sensible-synth/triggerspace/compare/v0.042...main
[0.042]: https://github.com/semi-sensible-synth/triggerspace/releases/tag/v0.042
