# triggerspace [DO NOT BUILD - UNTESTED]

This is a remix of the [NanoGris](https://github.com/Quinienl/NanoGris) trigger sequence generator (by Quinie), which in turn was based on the original [Grids](https://pichenettes.github.io/mutable-instruments-documentation/modules/grids/) hardware design by Mutable Instruments / Émilie Gillet, used under a Creative Commons ShareAlike 3.0 Licesne (CC BY-SA 3.0).

You might call it _"NanoGris-MIDI"_. The front panel looks something like this:

![triggerspace front panel render](images/front_panel_render.jpg "Front panel")

# Features
- Outputs three channels of 'topographic' drum triggers (kick, snare, hihat), with accent outputs
- Internal clock, with reset and swing
- External clock input, via triggers or MIDI clock
- Beat density (fill), X, Y and Chaos inputs to modify each pattern
- _to be implemented in firmware_: TRS-A MIDI output, General MIDI drum notes on channel 10 (and MIDI clock ?)

## Modifications from the NanoGris

- Added MIDI-IN TRS-A jack + 6N137 optocoupler for MIDI clock sync. The original Grids always had this option as an extra jumper on the PCB - now it's busted out onto a more cluttered front panel !
- Added MIDI-OUT TRS-A - required changing the pin used for the clock in, so this modules requires custom firmware even if you aren't using the MIDI-OUT option (firmware is WIP)
  - I've added jumpers and cuttable solder jumpers to allow easy re-routing for stock firmware if required/desired.
- The CLOCK and RND outputs are repurposed as TRS-A MIDI-OUT and MIDI-IN ports, respectively. A system of jumpers can be used to choose if these function as MIDI ports or the original voltage-based trigger outputs.
- Made more space between the FILL knobs (closer to original Grids layout), use a tactile button and cap for TAP button, various tweaks to routing and front panel design

## MIDI-out firmware (WIP)

- General idea: MIDI-OUT GM MIDI drum notes on channel 10
  This would require firmware changes - Mutables `avrlib` has `SerialOut`: 
  https://github.com/sonic-insurgence/Gritty-Grids/blob/master/avrlib/serial.h#L119 we could use.
  An avr MIDI example: https://github.com/dreamiurg/avr-liberty/blob/master/src/ccrma/midi.c
  Mutable MIDIpal: https://github.com/pichenettes/midipal - might help with MIDI-out code ?
- Because the `IN_CLOCK` currently uses the hardware serial TX port on the AtMEGA328p, if we make the `OUT_MIDI` use that port
  we will need to change the `IN_CLOCK` GPIO reading (it's currently read as half of an 8-bit parallel port - see the MIDIpal code, we should be able to switch the clock to an alternative GPIO instead: https://github.com/pichenettes/midipal/blob/master/midipal/hardware_config.h#L44)
- General MIDI (and Roland TR-8S) drum note numbers:
  - 36 (C1) - bass/kick drum
  - 38 (D1) - snare drum
  - 42 (F#1) - closed HH
  - 46 (Bb1) - open HH
- We could either change velocity based on accent, or assign accent to other notes (eg ch3 accent = open HH)
- (Other ideas: Set the note to be played via one of the CV inputs, or MIDI-IN)
- (Crazier ideas: add a SAM2695 or VS1053B chip for GM MIDI out ! There seem to be NOS versions on AliExpress ....)
