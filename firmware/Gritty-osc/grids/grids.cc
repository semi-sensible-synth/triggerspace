// Gritty-osc — Milestone 2: single-voice DDS square-wave oscillator.
//
// Repurposes the triggerspace hardware as an oscillator. A high-rate timer ISR
// (the "sample clock") advances a 32-bit phase accumulator by a tuning word each
// tick; the accumulator MSB is emitted as a square wave on the BD jack (74HC595
// bit 0x01). Pitch is set by the TEMPO knob, quantised to equal-tempered
// semitones so a tuner gives a clean pass/fail on pitch accuracy.
//
// Fork of Gritty-Grids, based on Mutable Instruments' Grids (Emilie Gillet).
// GPLv3 (see LICENSE).

#include <avr/interrupt.h>
#include <math.h>

#include "avrlib/adc.h"
#include "avrlib/watchdog_timer.h"

#include "grids/hardware_config.h"

using namespace avrlib;
using namespace grids;

ShiftRegister shift_register;
AdcInputScanner adc;

// ---- DDS sample clock ----------------------------------------------------
// Timer2 in CTC mode, prescaler /1: Fs = F_CPU / (OCR2A + 1).
// 20 MHz / 200 = 100 kHz. One voice + one 8-bit SPI write fits easily in the
// 200-cycle budget; revisit when scaling to 6 voices (see plans/gritty-osc.md).
static const uint32_t kSampleRate = 100000UL;
static const uint8_t  kOcr2a      = (F_CPU / kSampleRate) - 1;   // = 199

// 74HC595 bit that drives the BD jack (matches the Grids state-byte bit map).
static const uint8_t kBdBit = 0x01;

// 32-bit phase accumulator; bit 31 is the 50%-duty square output. Only the ISR
// touches `phase`, so it is not volatile (lets the compiler keep it in registers
// within the ISR). `increment` is the tuning word: written from the main loop
// (under cli) and read in the ISR, so it must be volatile.
uint32_t phase = 0;
volatile uint32_t increment = 0;

ISR(TIMER2_COMPA_vect)
{
  phase += increment;
  // Hand-written SPI byte to the 74HC595 (hot path — avoids a non-inlined call
  // that would force a full register save). Mirrors ShiftRegister::Write: the
  // latch/SS is PB2; pulsing it low then high clocks the byte to the outputs.
  PORTB &= ~_BV(PB2);
  SPDR = (phase & 0x80000000UL) ? kBdBit : 0;
  while (!(SPSR & _BV(SPIF)))
    ;
  PORTB |= _BV(PB2);
}

// increment = freq * 2^32 / Fs
static inline uint32_t TuningWord(float freq_hz)
{
  return (uint32_t)(freq_hz * (4294967296.0f / (float)kSampleRate));
}

void Init()
{
  cli();

  shift_register.Init();

  adc.Init();
  adc.set_num_inputs(ADC_CHANNEL_LAST);
  Adc::set_reference(ADC_DEFAULT);
  Adc::set_alignment(ADC_LEFT_ALIGNED);

  // Timer2: CTC (WGM21), prescaler /1 (CS20), compare-A interrupt = sample clock.
  TCCR2A = _BV(WGM21);
  TCCR2B = _BV(CS20);
  OCR2A  = kOcr2a;
  TIMSK2 = _BV(OCIE2A);

  sei();
}

int main(void)
{
  ResetWatchdog();
  Init();

  uint8_t last_note = 0xff;
  while (1)
  {
    adc.Scan();  // cycle the scanner so Read8() stays fresh

    // TEMPO knob -> equal-tempered MIDI note ~33..92 (A1 55 Hz .. G#6 ~1661 Hz).
    uint8_t pot = adc.Read8(ADC_CHANNEL_TEMPO);
    uint8_t note = 33 + (uint8_t)(((uint16_t)pot * 60) >> 8);
    if (note != last_note)
    {
      last_note = note;
      float freq = 440.0f * powf(2.0f, (float)((int8_t)note - 69) / 12.0f);
      uint32_t inc = TuningWord(freq);
      cli();
      increment = inc;
      sei();
    }
  }
}
