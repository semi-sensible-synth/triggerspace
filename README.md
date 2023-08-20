# NanoGrids-MIDI "triggerspace"

## Modifications

- Added MIDI-IN TRS-A jack + 6N137 optocoupler for MIDI clock sync (eg https://github.com/sonic-insurgence/Gritty-Grids/tree/master)
- Added MIDI-OUT TRS-A - required changing the pin used for the clock in, so requires custom firmware to use (WIP)
    - I've added jumpers and cuttable solder jumpers to allow easy re-routing for stock firmware if required/desired.
- Various tweaks to routing front panel layout and look

## MIDI-out firmware (WIP)

- Idea: MIDI OUT (eg GM MIDI drums on channel 10)
  This would require firmware changes - Mutables avrlib has SerialOut: 
  https://github.com/sonic-insurgence/Gritty-Grids/blob/master/avrlib/serial.h#L119 we could use.
  An avr MIDI example: https://github.com/dreamiurg/avr-liberty/blob/master/src/ccrma/midi.c
  Mutable MIDIpal: https://github.com/pichenettes/midipal - for MIDI out code ?
- Because the IN_CLOCK currently used the hardware serial TX port on the AtMEGA328p, if we make the IN_MIDI use that port
  we will need to change the IN_CLOCK GPIO reading (it's currently read as half an 8-bit parallel port - see the midipal code, we should be able to switch the clock to a GPIO instead: https://github.com/pichenettes/midipal/blob/master/midipal/hardware_config.h#L44)

# NanoGrids
### Arduino Nano version of Mutable Instruments Grids

Made this version  with Thru Hole parts and a Arduino Nano.
Schematics are heavely based on Mutable Instruments schematics. This is not a Mutable Instuments product.

_Note: panel hole for button might be a bit to big or small (depends if you use buttoncaps)_

I made this for my own and would like to share it with you. I'm doing this as a hobby so expect support to be like that to ;)
Everything at your own risk..

Some info on my website that might help

https://www.quinie.nl/category/diy/nanogris/

https://www.quinie.nl/nanogris-gallery/

If you don not want to order boards yourself please check out my shop.
https://www.quinie.nl/shop/

hex file for uploading here
https://www.quinie.nl/nanogris-the-hex-file/

Update: 6-8-2021
I updated the name of the module. The current version is 0.041

Older boards will have the same function. Did add some better ground planes. Some silkscreen additions to make build easier and changed the footprint of the potentiometer for better fit. 
