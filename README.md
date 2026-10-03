# triggerspace

> **TESTED** - v0.042 works with some simple bodges (build notes below)
>
> **IN PROGRESS** - v0.5.0 removes the v0.042 bodges and adds RC reconstruction-cap
> sockets on the jack PCB's outputs; not yet fabricated or bench-verified (see
> [CHANGELOG.md](CHANGELOG.md))

![View on KiCanvas](https://img.shields.io/badge/View_on-KiCanvas-purple?color=%238864CB&link=https%3A%2F%2Fgithub.com%2Fsemi-sensible-synth%2Ftriggerspace)


This is a remix of the [NanoGris](https://github.com/Quinienl/NanoGris) trigger sequence generator (by Quinie), which in turn was based on the original [Grids](https://pichenettes.github.io/mutable-instruments-documentation/modules/grids/) hardware design by Mutable Instruments / Émilie Gillet, used under a Creative Commons ShareAlike 3.0 License (CC BY-SA 3.0).

You might also call it _"NanoGris-MIDI"_. The front panel looks something like this:

![triggerspace front panel render](images/front_panel_render.jpg "Front panel")

# Features
- Outputs three channels of 'topographic' drum triggers (kick, snare, hihat), with accent outputs
- Internal clock, with reset and swing
- External clock input, via triggers or MIDI clock
- Beat density (fill), X, Y and Chaos inputs to modify each pattern
- MIDI TRS-A output, General MIDI drum notes on channel 10
- MIDI TRS input accepts Type A and Type B (v0.5.0, [LPZW auto-crossover MIDI input](https://github.com/kay-lpzw/LPZW_TRS_MIDI))
- 38mA peak current draw on +12V rail (28mA idle)

## Modifications from the NanoGris

- Added MIDI-IN TRS-A jack + 6N137 optocoupler for MIDI clock sync. The original Grids always had this option as an extra jumper on the PCB - now it's broken out onto a more cluttered front panel !
- Added MIDI-OUT TRS-A - required changing the pin used for the clock in to allow use of hardware serial TX/RX, so this modules requires [custom firmware](https://github.com/semi-sensible-synth/Gritty-Grids) even if you aren't using the MIDI-OUT option.
  - I've added pin jumpers and cuttable solder jumpers to allow re-routing for stock firmware if required/desired.
- The CLOCK and RND outputs are repurposed as the TRS-A MIDI-OUT and MIDI-IN ports, respectively. Several pin jumpers can be used to choose if these act as MIDI ports or the original voltage-based trigger outputs.
- Made more space between the FILL knobs (closer to original Grids layout), use a tactile button and cap for TAP button, various tweaks to routing and front panel design

## MIDI-out firmware

Use this: [Gritty-Grids firmware (triggerspace fork)](https://github.com/semi-sensible-synth/Gritty-Grids)

- Supports modified Arduino Nano pin assignments used by _triggerspace_ hardware
- Sends General MIDI drum notes on channel 10 to MIDI-OUT (TRS-A)
  - GM MIDI (and Roland TR-8S) drum note numbers:
  ```
  Out 1:             36 (0x24, C1)  - bass/kick drum
  Out 2:             38 (0x26, D1)  - snare drum
  Out 3 (no accent): 42 (0x2a, F#1) - closed hi-hat
  Out 3 (accented):  46 (0x2e, Bb1) - open hi-hat
  ```

### Future firmware ideas

- We could change velocity based on accent (for bass and snare)
- Disting Ex SD 6 Triggers mode, where notes span 48-53 (unaccented and accented)
- Chord/arp output over MIDI. Set the chord root/inversion to be played via one of the CV inputs, or MIDI-IN
  - Read MIDI notes from MIDI-IN, buffer the last 3 or 6 notes seen and play these for Out1-3, Accent 1-3
- (Crazier ideas: pair it with a SAM2695 or VS1053B based module/expander for GM MIDI audio out ! There seem to be NOS versions on AliExpress ....)

## Build options

### MIDI IN optocoupler (U8 / U9)

From v0.5.0 the MIDI input is the
[LPZW auto-crossover MIDI input](https://github.com/kay-lpzw/LPZW_TRS_MIDI) by Kay Knofe
of LPZW.modules, first used in their WK3 MIDI Thru module. **This input stage is not our
design.** Its author asks that it is credited as the "LPZW auto-crossover MIDI input"
wherever it is mentioned; the credit is on the jack PCB silkscreen next to the circuit,
here, and should be kept in any manual.

It accepts TRS Type A and Type B cables with no switch: the two LEDs of a dual
optocoupler sit anti-parallel across tip and ring, each with its own 220R (R51, R48).
Whichever way round the loop current flows, one LED lights, and the two open-collector
outputs share one 1k pull-up (R47), so the Nano sees the same signal either way.

Fit one of these:

| Build | U8 (DIP-8 socket) | U9 (SOIC-8) | D7 | JP3, JP4 | Accepts |
|---|---|---|---|---|---|
| **Default** | HCPL-2630 or HCPL-2631 | not fitted | not fitted | as made (1-2 bridged) | Type A and B |
| SMD optocoupler | not fitted, no socket | HCPL-0630 or HCPL-0631 | not fitted | as made (1-2 bridged) | Type A and B |
| 6N138 (as v0.042) | 6N138 | not fitted | 1N4148 | cut 1-2, bridge 2-3 | Type A only |

- U9 sits inside U8's DIP-8 footprint on the back of the jack PCB, so fit U8 or U9,
  never both - a DIP socket would sit on top of U9. The HCPL-263x (DIP-8) and HCPL-063x
  (SOIC-8) have the same pinout.
- R46 (4.7k) is fitted in every build but only connected (through JP4) in the 6N138
  build, where it is the 6N138's base resistor. R51 is only used by the dual
  optocouplers; with a 6N138 it goes to an unconnected pin.
- D7 protects the 6N138's LED from reverse voltage. Don't fit it with a dual
  optocoupler: it would sit across one of the LEDs and take the current meant for the
  other, so Type B input would stop working.
- The HCPL-263x/063x is guaranteed to switch at 5mA LED current. A 5V MIDI 1.0 sender
  into a 220R receiver gives about 5mA, so worst-case parts are marginal (typical parts
  switch at about 2mA); 3.3V senders built to the MIDI Association's CA-33 values give
  about 7mA. The 6N138 needs only about 1.6mA, so it tolerates weaker Type A senders.
- Sourcing (JLCPCB, October 2026): the SOIC-8 HCPL-0630 is well stocked; DIP-8 HCPL-263x
  stock there is low, so buy those elsewhere or use the SOIC-8 build.

See [Jumpers](#jumpers) for JP3/JP4 and the other jumper settings.

### RC reconstruction caps (CH*_CAP1)

The six 2-pin sockets next to the output jacks on the jack PCB (`CH1_CAP1`,
`CH1_ACC_CAP1`, `CH2_CAP1`, `CH2_ACC_CAP1`, `CH3_CAP1`, `CH3_ACC_CAP1`) take optional
capacitors to GND that filter the sigma-delta hiss from the Gritty-drumsynth firmware's
synthesised drum voices. Leave them empty for trigger outputs (Gritty-Grids firmware).
Values are still to be chosen (see TODO / IDEAS).

## Changelog

See [CHANGELOG.md](CHANGELOG.md) for the changes in each hardware revision.

## v0.042 build guide notes (bodges required !)

  - Trying to power the Nano via 5V from U4 voltage regulator to VIN pin 30 didn't work here, despite the original NanoGris design doing it this way. Upon reflection this is no surprise ... 5V isn't enough for the typical AMS1117-5.0 regulator found on a Nano (clone), which needs ~>= 6.5V input to work ! I fixed this with a bodge as follows:
    - Melt the VIN pin 30 solder joint and push it 'up' so it doesn't insert into the female header. This disconnects the Nano from the U4 regulator, but still allows U4 to power the 5V rail in the rest of the module.
    - Attach a bodge wire from the +12V rail pin on the U4 voltage regulator to the protruding VIN pin 30 (I used a single female header to make it easy to remove the Nano if required).
  - I didn't have an LM4040-5.0 on hand, but a 5.1V Zener diode seems to work well enough. Should be a valid option.
  - Can optionally use an 78L50 (TO-92) voltage regulator rather than L7805 (TO-220)
  - Sometimes flashing the firmware seems to fail (can't connect to device) when the Nano is in the module, but seems to work if you take out out of the PCB to flash ¯\_(ツ)_/¯

## TODO / IDEAS

- Fabricate and bench-verify v0.5.0.
- Choose the RC reconstruction-cap values (and confirm the socket footprint) by bench
  testing with the Gritty-drumsynth firmware.
- Power: the Nano now runs from +12V via its own regulator, which can probably supply
  the whole 5V rail (op amps, CMOS shift register, transistor triggers; the module drew
  28mA idle and 38mA peak with all LEDs on, and a Nano's regulator should supply at
  least 800mA from +12V). We could mark U4 and its capacitors as optional on the
  silkscreen and add a solder bridge jumper from pin 27 (Nano 5V output) to the 5V rail.
  - We also have enough space to use a 16pin power header and use the Eurorack 5V rail.
- Do we have room for mute switches/buttons on each channel ?
