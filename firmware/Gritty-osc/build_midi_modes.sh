#!/usr/bin/env bash
#
# Build each build-time MIDI-input mode of Gritty-osc as its own .hex.
#
# Each MIDI mode is selected by a #define in grids.cc (all commented out by
# default = the knob/CV + arp oscillator, which a plain `make` builds). This
# script builds every mode with that #define injected via EXTRA_DEFINES and
# collects the resulting hex files into hex/ with descriptive names.
#
# Usage:  ./build_midi_modes.sh
#
set -euo pipefail
cd "$(dirname "$0")"

# The makefile's own EXTRA_DEFINES (polled UART RX) must be preserved when we
# override it on the command line, so repeat it here alongside each mode define.
BASE_DEFINES="-DDISABLE_DEFAULT_UART_RX_ISR"

# "<MODE_DEFINE>:<output-name>" — see the OSC_MIDI_* block in grids.cc.
MODES=(
  "OSC_MIDI_MONO_ROOT:mono-root"   # (A) one channel -> 6-voice chord engine
  "OSC_MIDI_MULTI_3CH:multi-3ch"   # (B) channels 1/2/3 -> osc 0/1/2
  "OSC_MIDI_POLY3:poly3"           # (C1) 3-voice round-robin, subs double mains
  "OSC_MIDI_POLY6:poly6"           # (C2) 6-voice round-robin, independent subs
  "OSC_MIDI_SPLIT:split"           # (D) split keyboard: mains high / subs bass
)

# avr-size is nice-to-have; fall back to the CrossPack path, else skip.
AVRSIZE="$(command -v avr-size || echo /usr/local/CrossPack-AVR/bin/avr-size)"

OUT=hex
mkdir -p "$OUT"

for entry in "${MODES[@]}"; do
  def="${entry%%:*}"
  name="${entry##*:}"
  echo "=== building ${name} (-D${def}) ==="
  rm -rf build
  make EXTRA_DEFINES="${BASE_DEFINES} -D${def}" >/dev/null
  cp build/grids/grids.hex "${OUT}/gritty-osc-${name}.hex"
  [ -x "$AVRSIZE" ] && "$AVRSIZE" build/grids/grids.elf | tail -1 || true
done

rm -rf build
echo
echo "Done. Hex files in ${OUT}/:"
ls -1 "${OUT}"/*.hex
