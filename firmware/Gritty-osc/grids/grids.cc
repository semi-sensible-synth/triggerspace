// Gritty-osc — Milestone 4: six-voice DDS square/PWM oscillator.
//
// Six independent DDS voices, each a 16-bit phase accumulator, packed into the
// six audio bits of the 74HC595 and shifted out once per sample tick. Bit map
// matches the Grids state byte / triggerspace jack wiring:
//   voice 0 -> 0x01 CH1        voice 3 -> 0x08 CH1_ACC
//   voice 1 -> 0x02 CH2        voice 4 -> 0x10 CH2_ACC
//   voice 2 -> 0x04 CH3        voice 5 -> 0x20 CH3_ACC
// (bits 0x40/0x80 drive CH_CLOCK/CH_RND — left alone; those jacks are MIDI.)
//
// Interim control (bring-up only; proper mapping lands in M5/M6):
//   BD/SD/HH density knobs -> pitch of voices 0/1/2
//   TEMPO knob             -> master transpose (+/-2 octaves)
//   voices 3/4/5           -> voices 0/1/2 one octave down (so all six sound)
//   X-CV knob              -> global PWM duty (~12%..88%, 50% = square)
//
// Dropped to 16-bit accumulators + Fs = 62.5 kHz so six voices fit the ISR
// budget at 16 MHz (see plans/gritty-osc.md, Milestone 4).
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

static const uint8_t kNumVoices = 6;

// ---- DDS sample clock ----------------------------------------------------
// Timer2 in CTC mode, prescaler /1: Fs = F_CPU / (OCR2A + 1).
// 16 MHz / 256 = 62.5 kHz. That gives a 256-cycle ISR budget for six 16-bit
// voice updates + the packed SPI write; profile before raising Fs. One voice at
// 100 kHz fit in ~137 cyc, so six at 62.5 kHz is the conservative first cut.
static const uint32_t kSampleRate = 62500UL;
static const uint8_t  kOcr2a      = (F_CPU / kSampleRate) - 1;   // = 255 @ 16 MHz

// Per-voice 16-bit DDS state. `phase[]` is ISR-private (not volatile so the
// compiler can keep the accumulators hot). `increment[]` (tuning words) and the
// scalar `duty` are written from the main loop under cli and read in the ISR, so
// they are volatile. Output bit i is high while phase[i] < duty.
uint16_t phase[kNumVoices] = {0};
volatile uint16_t increment[kNumVoices] = {0};
volatile uint16_t duty = 0x8000;   // 50% square default

ISR(TIMER2_COMPA_vect)
{
  uint16_t d = duty;   // read the shared threshold once
  uint8_t out = 0;
  // Unrolled so the accumulators stay in registers and the timing is
  // deterministic (no loop counter / indexed addressing in the hot path).
  phase[0] += increment[0]; if (phase[0] < d) out |= 0x01;
  phase[1] += increment[1]; if (phase[1] < d) out |= 0x02;
  phase[2] += increment[2]; if (phase[2] < d) out |= 0x04;
  phase[3] += increment[3]; if (phase[3] < d) out |= 0x08;
  phase[4] += increment[4]; if (phase[4] < d) out |= 0x10;
  phase[5] += increment[5]; if (phase[5] < d) out |= 0x20;

  // Hand-written SPI byte to the 74HC595 (hot path — avoids a non-inlined call
  // that would force a full register save). Latch/SS is PB2: pulse low then high
  // to clock the byte to the outputs.
  PORTB &= ~_BV(PB2);
  SPDR = out;
  while (!(SPSR & _BV(SPIF)))
    ;
  PORTB |= _BV(PB2);
}

// increment = freq * 2^16 / Fs  (rounded). 16-bit -> Fs/65536 = ~0.95 Hz/count
// at 62.5 kHz; worst-case ~15 cents at the very bottom, finer higher up.
static inline uint16_t TuningWord(float freq_hz)
{
  return (uint16_t)(freq_hz * (65536.0f / (float)kSampleRate) + 0.5f);
}

// MIDI note number -> frequency (equal temperament, A4 = 69 = 440 Hz).
static inline float NoteToFreq(int8_t note)
{
  return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
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

  // Density knob -> MIDI note. 0..255 spans ~4 octaves from C2 (36). The knobs
  // are wired reversed, so the raw reading is inverted below.
  static const uint8_t kBaseNote = 36;   // C2
  static const uint8_t kNoteSpan = 48;   // 4 octaves

  int8_t last_note[3] = {-1, -1, -1};
  int8_t last_transpose = 127;

  const uint8_t kDensityCh[3] = {
      ADC_CHANNEL_BD_DENSITY_CV,
      ADC_CHANNEL_SD_DENSITY_CV,
      ADC_CHANNEL_HH_DENSITY_CV};

  while (1)
  {
    adc.Scan();  // cycle the scanner so Read8() stays fresh

    // TEMPO knob -> master transpose, -24..+24 semitones (+/-2 octaves).
    int8_t transpose =
        (int8_t)(((uint16_t)adc.Read8(ADC_CHANNEL_TEMPO) * 48) >> 8) - 24;

    for (uint8_t v = 0; v < 3; ++v)
    {
      uint8_t pot = 255 - adc.Read8(kDensityCh[v]);   // knobs wired reversed
      int8_t note = kBaseNote + (int8_t)(((uint16_t)pot * kNoteSpan) >> 8);
      if (note != last_note[v] || transpose != last_transpose)
      {
        last_note[v] = note;
        uint16_t inc_main = TuningWord(NoteToFreq(note + transpose));
        uint16_t inc_sub  = TuningWord(NoteToFreq(note + transpose - 12));
        cli();
        increment[v]     = inc_main;   // voices 0/1/2
        increment[v + 3] = inc_sub;    // voices 3/4/5 one octave down
        sei();
      }
    }
    last_transpose = transpose;

    // X-CV knob -> global PWM duty. Map 0..255 across ~12%..88% (50% = square);
    // trimming both extremes keeps the whole sweep audible (timbre is symmetric
    // about 50%, so 12% and 88% sound alike — the point is a usable throw).
    uint8_t xcv = adc.Read8(ADC_CHANNEL_X_CV);
    uint16_t d = 0x1F00 + (uint16_t)xcv * 0xC2;   // ~12% floor .. ~88% at 255
    cli();
    duty = d;
    sei();
  }
}
