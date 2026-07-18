// Gritty-osc — Milestone 5: six-voice DDS oscillator with note-LUT tuning.
//
// Six independent DDS voices, each a 16-bit phase accumulator, packed into the
// six audio bits of the 74HC595 and shifted out once per sample tick. Bit map
// matches the Grids state byte / triggerspace jack wiring:
//   voice 0 -> 0x01 CH1        voice 3 -> 0x08 CH1_ACC
//   voice 1 -> 0x02 CH2        voice 4 -> 0x10 CH2_ACC
//   voice 2 -> 0x04 CH3        voice 5 -> 0x20 CH3_ACC
// Bit 0x40 (CH_CLOCK jack) carries a first-order sigma-delta PDM *mix* of all six
// voices; bit 0x80 (CH_RND) is left alone. (Those two jacks double as MIDI I/O —
// the mix is only reachable with the CLOCK jack jumpered to voltage-trigger mode.)
//
// Interim control (bring-up; final root/chord/per-channel mapping lands in M6):
//   BD/SD/HH density knobs -> pitch of voices 0/1/2 (note-LUT tuned)
//   TEMPO knob             -> master transpose (+/-2 octaves)
//   voices 3/4/5           -> voices 0/1/2 one octave down (so all six sound)
//   X-CV knob              -> global PWM duty (~12%..88%, 50% = square)
//
// Dropped to 16-bit accumulators + Fs = 62.5 kHz so six voices fit the ISR
// budget at 16 MHz (see plans/gritty-osc.md, Milestone 4).
//
// M5: pitch comes from a precomputed note -> tuning-word table (equal
// temperament, generated for Fs = 62.5 kHz), so no powf()/float is used at
// runtime — a plain flash lookup replaces the per-change float pow.
//
// Fork of Gritty-Grids, based on Mutable Instruments' Grids (Emilie Gillet).
// GPLv3 (see LICENSE).

#include <avr/interrupt.h>
#include <avr/pgmspace.h>

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
  uint8_t sum = 0;     // count of voices currently high (0..6) = mix level
  // Unrolled so the accumulators stay in registers and the timing is
  // deterministic (no loop counter / indexed addressing in the hot path).
  phase[0] += increment[0]; if (phase[0] < d) { out |= 0x01; ++sum; }
  phase[1] += increment[1]; if (phase[1] < d) { out |= 0x02; ++sum; }
  phase[2] += increment[2]; if (phase[2] < d) { out |= 0x04; ++sum; }
  phase[3] += increment[3]; if (phase[3] < d) { out |= 0x08; ++sum; }
  phase[4] += increment[4]; if (phase[4] < d) { out |= 0x10; ++sum; }
  phase[5] += increment[5]; if (phase[5] < d) { out |= 0x20; ++sum; }

  // Mix out on the clock jack (bit 0x40): a first-order sigma-delta modulator
  // turns the 0..6 summed level into a 1-bit PDM stream whose pulse density
  // tracks the mix. Low-passed downstream (1k series R + cap / input capacitance)
  // this reconstructs a mono sum of all six squares; the Fs carrier (62.5 kHz) is
  // inaudible. Requires the CLOCK jack jumper in voltage-trigger (not MIDI) mode.
  // (First order is unconditionally stable — the accumulator simply wraps in 8
  // bits. A second-order loop overloads on loud input and crackles; not worth it,
  // and the first-order "sizzle" is a nice effect through an external low-pass.)
  static uint16_t sd_acc = 0;
  sd_acc += (uint16_t)sum * 42;      // 0..6 -> 0..252 added per tick (<= full scale)
  if (sd_acc & 0xFF00) out |= 0x40;  // carry past 255 -> emit a 1
  sd_acc &= 0x00FF;                  // keep the fractional remainder

  // Hand-written SPI byte to the 74HC595 (hot path — avoids a non-inlined call
  // that would force a full register save). Latch/SS is PB2: pulse low then high
  // to clock the byte to the outputs.
  PORTB &= ~_BV(PB2);
  SPDR = out;
  while (!(SPSR & _BV(SPIF)))
    ;
  PORTB |= _BV(PB2);
}

// MIDI note -> 16-bit DDS tuning word (increment = freq * 2^16 / Fs, rounded).
// Equal temperament, A4 = note 69 = 440 Hz, generated for Fs = 62.5 kHz. Held in
// flash (PROGMEM); indexing this replaces the per-change powf() of earlier
// milestones. Resolution is Fs/65536 = ~0.95 Hz/count (coarse at the sub-audio
// bottom, fine over the useful range).
static const uint16_t kNoteTuningWord[128] PROGMEM = {
      9,     9,    10,    10,    11,    11,    12,    13,
     14,    14,    15,    16,    17,    18,    19,    20,
     22,    23,    24,    26,    27,    29,    31,    32,
     34,    36,    38,    41,    43,    46,    48,    51,
     54,    58,    61,    65,    69,    73,    77,    82,
     86,    92,    97,   103,   109,   115,   122,   129,
    137,   145,   154,   163,   173,   183,   194,   206,
    218,   231,   244,   259,   274,   291,   308,   326,
    346,   366,   388,   411,   435,   461,   489,   518,
    549,   581,   616,   652,   691,   732,   776,   822,
    871,   923,   978,  1036,  1097,  1163,  1232,  1305,
   1383,  1465,  1552,  1644,  1742,  1845,  1955,  2071,
   2195,  2325,  2463,  2610,  2765,  2930,  3104,  3288,
   3484,  3691,  3910,  4143,  4389,  4650,  4927,  5220,
   5530,  5859,  6207,  6577,  6968,  7382,  7821,  8286,
   8779,  9301,  9854, 10440, 11060, 11718, 12415, 13153,
};

// Look up a note's tuning word, clamping the index to the valid MIDI range.
static inline uint16_t NoteTuningWord(int8_t note)
{
  if (note < 0)   note = 0;
  if (note > 127) note = 127;
  return pgm_read_word(&kNoteTuningWord[(uint8_t)note]);
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
        uint16_t inc_main = NoteTuningWord(note + transpose);
        uint16_t inc_sub  = NoteTuningWord(note + transpose - 12);
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
