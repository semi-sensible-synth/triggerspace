// Copyright 2012 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// Modified by Sonic Insurgence, June 2021 — improved MIDI implementation.
// Modified by Andrew Perry (semi-sensible synth), 2024 — MIDI out on channel 10.
//
// Gritty-drumsynth, 2026
// ----------------------
// This fork keeps the Grids topographic sequencer, clock and MIDI intact, but
// replaces the raw voltage-trigger outputs with three internally SYNTHESISED
// 808/909-style drum voices rendered as 1-bit PDM (delta-sigma) on the existing
// BD/SD/HH jacks. A synthesised low sine is hugely oversampled by the modulator,
// so the kick comes out clean instead of an overdriven 1-bit sample.
//
// Two ISRs, on two timers:
//   * TIMER2 (~8 kHz, unchanged): the Grids control tick — clock, sequencer,
//     MIDI-in, tap button, LEDs. It no longer writes the 595; a sequencer
//     trigger now fires a voice envelope (the single integration point in
//     UpdateShiftRegister()).
//   * TIMER1 (Fs = 31.25 kHz): the audio tick — runs one 1-bit sigma-delta
//     modulator per drum jack on the currently-held voice sample and clocks the
//     packed byte to the 74HC595. It is short and constant-time; because the
//     TIMER2 control ISR is ISR_NOBLOCK, the audio ISR pre-empts it and keeps a
//     steady PDM carrier.
// The heavy per-voice synthesis runs in the main loop, paced by the audio ISR to
// a fixed synth rate (Fs / kSynthDecimation ~= 7.8 kHz); the ISR just holds and
// re-modulates the last sample (zero-order hold). This amortises the DSP math
// out from under the hard-real-time 1-bit output loop.

#include <avr/eeprom.h>
#include <avr/pgmspace.h>

#include "avrlib/adc.h"
#include "avrlib/boot.h"
#include "avrlib/op.h"
#include "avrlib/watchdog_timer.h"

#include "grids/clock.h"
#include "grids/hardware_config.h"
#include "grids/pattern_generator.h"
#include "grids/midi.h"

using namespace avrlib;
using namespace grids;

Leds leds;
ResetInput reset_input;
ButtonInput button_input;
ClockInput clock_input;

AdcInputScanner adc;
ShiftRegister shift_register;
MidiIO midi;

// ===========================================================================
// Drum synthesis engine
// ===========================================================================

// Diagnostic: force a continuous ~220 Hz tone on all three drum jacks, ignoring
// the sequencer / triggers / mute. Use it to verify the audio ISR + PDM + output
// path independently of the trigger logic. Comment out for the real drum engine.
//#define DRUMSYNTH_TEST_TONE

// --- Sample clocks --------------------------------------------------------
// Audio (PDM) ISR on Timer1, CTC, prescaler /1: Fs = F_CPU / (OCR1A + 1).
// 16 MHz / 512 = 31.25 kHz.  Kept moderate so the full Grids control ISR (Timer2,
// ~8 kHz) plus the main-loop synth all fit the 16 MHz budget with headroom; the
// lower carrier also reconstructs better through the trigger output stage.
// Synthesis is decimated by kSynthDecimation -> ~7.8 kHz synth rate (plenty for
// a 55 Hz kick and a few-kHz hat).
static const uint32_t kAudioRate  = 31250UL;
static const uint16_t kOcr1a      = (F_CPU / kAudioRate) - 1;   // = 511 @ 16 MHz
static const uint8_t  kSynthDecimation = 4;                     // -> ~7812 Hz
static const uint32_t kSynthRate  = kAudioRate / kSynthDecimation;

// Convert a frequency (Hz) to a 16-bit phase increment at the synth rate.
// inc = f * 2^16 / Fs_synth.  (Compile-time for the voice defaults.)
#define HZ_TO_INC(hz)  ((uint16_t)(((uint32_t)(hz) * 65536UL) / kSynthRate))

// --- Signed sine, one period, 8-bit (-127..127), 256 entries --------------
static const int8_t kSine[256] PROGMEM = {
     0,    3,    6,    9,   12,   16,   19,   22,
    25,   28,   31,   34,   37,   40,   43,   46,
    49,   51,   54,   57,   60,   63,   65,   68,
    71,   73,   76,   78,   81,   83,   85,   88,
    90,   92,   94,   96,   98,  100,  102,  104,
   106,  107,  109,  111,  112,  113,  115,  116,
   117,  118,  120,  121,  122,  122,  123,  124,
   125,  125,  126,  126,  126,  127,  127,  127,
   127,  127,  127,  127,  126,  126,  126,  125,
   125,  124,  123,  122,  122,  121,  120,  118,
   117,  116,  115,  113,  112,  111,  109,  107,
   106,  104,  102,  100,   98,   96,   94,   92,
    90,   88,   85,   83,   81,   78,   76,   73,
    71,   68,   65,   63,   60,   57,   54,   51,
    49,   46,   43,   40,   37,   34,   31,   28,
    25,   22,   19,   16,   12,    9,    6,    3,
     0,   -3,   -6,   -9,  -12,  -16,  -19,  -22,
   -25,  -28,  -31,  -34,  -37,  -40,  -43,  -46,
   -49,  -51,  -54,  -57,  -60,  -63,  -65,  -68,
   -71,  -73,  -76,  -78,  -81,  -83,  -85,  -88,
   -90,  -92,  -94,  -96,  -98, -100, -102, -104,
  -106, -107, -109, -111, -112, -113, -115, -116,
  -117, -118, -120, -121, -122, -122, -123, -124,
  -125, -125, -126, -126, -126, -127, -127, -127,
  -127, -127, -127, -127, -126, -126, -126, -125,
  -125, -124, -123, -122, -122, -121, -120, -118,
  -117, -116, -115, -113, -112, -111, -109, -107,
  -106, -104, -102, -100,  -98,  -96,  -94,  -92,
   -90,  -88,  -85,  -83,  -81,  -78,  -76,  -73,
   -71,  -68,  -65,  -63,  -60,  -57,  -54,  -51,
   -49,  -46,  -43,  -40,  -37,  -34,  -31,  -28,
   -25,  -22,  -19,  -16,  -12,   -9,   -6,   -3,
};

static inline int8_t Sine(uint8_t phase_hi)
{
  return (int8_t)pgm_read_byte(&kSine[phase_hi]);
}

// --- Voice parameters (RAM; defaults here, live-tunable via MIDI CC) -------
// "Decay" values are the exponential retain-per-synth-tick coefficient (Q8):
// higher = longer tail.  amp/pitch envelopes are 16-bit, x = U16U16MulShift16(x,d).
struct VoiceParams {
  uint16_t base_inc;     // steady-state pitch (phase increment @ synth rate)
  uint16_t pitch_mod0;   // initial pitch-envelope excursion (added to base_inc)
  uint16_t pitch_decay;  // pitch-env retain/tick, 16-bit (65536 = no decay)
  uint16_t amp_decay;    // amp-env retain/tick, 16-bit
  uint16_t noise_decay;  // 2nd (noise) amp-env retain/tick, 16-bit (snare/hat)
  uint8_t  tone_level;   // tonal component level 0..255
  uint8_t  noise_level;  // noise component level 0..255
};

// Kick, snare, hat defaults. Decays are 16-bit exponential retain-per-tick at the
// ~7.8 kHz synth rate: tau_ms ~= -1000 / (ln(d/65536) * 7812). e.g. 65480 ~= 150 ms,
// 65327 ~= 40 ms. (8-bit decays topped out near 16 ms -> everything was a click.)
static VoiceParams params[3] = {
  // base_inc            pitch_mod0                    pDecay aDecay nDecay tone noise
  { HZ_TO_INC(55),  HZ_TO_INC(320) - HZ_TO_INC(55),   65120, 65480,     0, 255,   0 }, // BD kick
  { HZ_TO_INC(190), HZ_TO_INC(120),                   65535, 65380, 65460, 150, 210 }, // SD snare
  { HZ_TO_INC(1800), 0,                               65535, 65535, 65300,   0, 255 }, // HH hat (tone=metal amount)
};

// --- EEPROM persistence for the voice params -------------------------------
// pattern_generator owns EEPROM bytes 0..1; the voice params live well clear of
// it. A magic/version byte guards the block so a fresh chip (or a struct-layout
// change) falls back to the compiled defaults above instead of loading garbage.
static const uint16_t kVoiceEepromAddr  = 16;
static const uint8_t  kVoiceEepromMagic = 0xD4;  // bump if VoiceParams changes

static void SaveVoiceParams()
{
  eeprom_write_block(params, (void*)(kVoiceEepromAddr + 1), sizeof(params));
  eeprom_write_byte((uint8_t*)kVoiceEepromAddr, kVoiceEepromMagic);
}

static void LoadVoiceParams()
{
  if (eeprom_read_byte((uint8_t*)kVoiceEepromAddr) == kVoiceEepromMagic)
  {
    eeprom_read_block(params, (const void*)(kVoiceEepromAddr + 1), sizeof(params));
  }
}

// --- Per-voice runtime state ----------------------------------------------
static uint16_t v_phase[3];       // oscillator phase accumulator
static uint16_t v_pitch_mod[3];   // decaying pitch excursion
static uint16_t v_amp[3];         // tonal amplitude envelope (0..0xffff)
static uint16_t v_namp[3];        // noise amplitude envelope (0..0xffff)
static uint16_t lfsr = 0xACE1u;   // shared noise generator
static int8_t   noise_prev;       // for the 1st-difference high-pass

// Per-hit copies of the pitch / decay params, latched at TriggerVoice() so the
// performance-mode Randomness knob can jitter them per hit without touching the
// stored params[] (see ParamJitter/JitterDecay).
static uint16_t v_base_inc[3];    // this hit's pitch (params.base_inc +/- jitter)
static uint16_t v_amp_decay[3];   // this hit's amp-env decay
static uint16_t v_namp_decay[3];  // this hit's noise-env decay

// The current instantaneous voice output (signed, ~-128..127). Written by the
// main-loop synth, read by the audio ISR — single bytes, atomic on AVR.
static volatile int8_t voice_sample[3] = { 0, 0, 0 };
// Accent-jack gate bits (0x08 BD, 0x10 SD, 0x20 HH), set while an accented hit
// is sounding.  ORed into the 595 byte by the audio ISR.
static volatile uint8_t accent_bits = 0;

// Trigger latches: control ISR / MIDI-in raise a bit per voice, synth consumes.
static volatile uint8_t v_trigger = 0;   // bit i = fire voice i
static volatile uint8_t v_accent  = 0;   // bit i = that hit is accented

// Synth pacing: audio ISR bumps this once per kSynthDecimation ticks; the main
// loop renders that many samples. Keeps the heavy DSP out of the ISR.
static volatile uint8_t synth_pending = 0;

static inline uint8_t NextNoise()
{
  // 16-bit Galois LFSR, taps 0xB400.
  uint8_t lsb = lfsr & 1u;
  lfsr >>= 1;
  if (lsb) lfsr ^= 0xB400u;
  return (uint8_t)(lfsr >> 8);
}

// Per-hit parameter jitter driven by the performance-mode Randomness/chaos knob
// (0..255). Returns a signed perturbation of `base`, scaled by chaos and by
// max_q10 = the maximum |delta|/base at full chaos, in 1/1024 units (20 ~= 2%,
// 205 ~= 20%). chaos=0 => no variation. Shared noise LFSR gives the sign/size.
static int16_t ParamJitter(uint16_t base, uint8_t chaos, uint8_t max_q10)
{
  int8_t r = (int8_t)NextNoise();                 // -128..127
  int16_t s = ((int16_t)chaos * r) >> 8;          // ~ -128..126, scaled by chaos
  return (int16_t)(((int32_t)base * s * max_q10) >> 17);
}

// Jitter a decay COEFFICIENT by perturbing its rate (65536-decay) rather than
// the near-65536 coefficient itself, so the variation lands in decay *time* and
// can't collapse the tail to a click.
static uint16_t JitterDecay(uint16_t decay, uint8_t chaos, uint8_t max_q10)
{
  uint16_t rate = 65536u - decay;                 // 1..65536 (bigger = shorter)
  int32_t nr = (int32_t)rate + ParamJitter(rate, chaos, max_q10);
  if (nr < 1) nr = 1; else if (nr > 8000) nr = 8000;
  return (uint16_t)(65536u - (uint16_t)nr);
}

// Start (or retrigger) a voice. Accent lengthens/brightens by boosting the
// initial envelope headroom. All per-hit values are latched here, jittered by
// the current chaos setting so no two hits are identical (chaos=0 => identical).
// Pitch drift is far more audible than level/length drift, so it is kept subtle
// (+/-2%) and only engages across the top ~80% of the knob; level and length
// get the full +/-20%.
static void TriggerVoice(uint8_t v, bool accent)
{
  uint8_t chaos = pattern_generator.mutable_settings()->options.drums.randomness;
  uint8_t chaos_pitch = 0;                         // gated: dead in bottom ~20%
  if (chaos > 51) { uint8_t d = chaos - 51; chaos_pitch = d + (d >> 2); } // ->0..255

  VoiceParams &p = params[v];

  int32_t amp = accent ? 0xffff : 0xc000;
  amp += ParamJitter((uint16_t)amp, chaos, 205);  // level: +/-20%
  if (amp < 0x4000) amp = 0x4000; else if (amp > 0xffff) amp = 0xffff;

  v_phase[v]      = 0;
  v_base_inc[v]   = p.base_inc + ParamJitter(p.base_inc, chaos_pitch, 20);  // +/-2%
  v_pitch_mod[v]  = p.pitch_mod0;
  v_amp[v]        = (uint16_t)amp;
  v_namp[v]       = (uint16_t)amp;
  v_amp_decay[v]  = JitterDecay(p.amp_decay,   chaos, 205);  // length: +/-20%
  v_namp_decay[v] = JitterDecay(p.noise_decay, chaos, 205);  // length: +/-20%
}

// One synth sample for all three voices -> voice_sample[]. Runs ~7.8 kHz in the
// main loop. Consumes the trigger latches first (atomically).
static void RenderVoices()
{
#ifdef DRUMSYNTH_TEST_TONE
  // Continuous 220 Hz test tone on all three jacks (bypasses everything below).
  static uint16_t tphase;
  tphase += HZ_TO_INC(220);
  int8_t t = (int8_t)S16U8MulShift8(Sine(tphase >> 8), 220);
  voice_sample[0] = t;
  voice_sample[1] = t;
  voice_sample[2] = t;
  return;
#endif

  // Consume the trigger latches (set by the Timer2 ISR); read-and-clear under
  // cli so we don't drop a trigger raised between the read and the clear.
  cli();
  uint8_t trig = v_trigger; v_trigger = 0;
  uint8_t acc  = v_accent;  v_accent  = 0;
  sei();
  for (uint8_t v = 0; v < 3; ++v) {
    if (trig & (1 << v)) {
      TriggerVoice(v, acc & (1 << v));
    }
  }

  uint8_t new_accent = 0;
  int8_t noise = (int8_t)(NextNoise() - 128);
  int16_t noise_hp = (int16_t)noise - noise_prev;   // 1st-difference high-pass
  noise_prev = noise;

  // ---- Kick: pitch-swept sine * exp amp env -----------------------------
  {
    VoiceParams &p = params[0];
    v_pitch_mod[0] = U16U16MulShift16(v_pitch_mod[0], p.pitch_decay);
    v_phase[0] += v_base_inc[0] + v_pitch_mod[0];
    v_amp[0] = U16U16MulShift16(v_amp[0], v_amp_decay[0]);
    int16_t s = S16U8MulShift8(Sine(v_phase[0] >> 8), (uint8_t)(v_amp[0] >> 8));
    voice_sample[0] = (int8_t)s;
    if (v_amp[0] > 0x1400) new_accent |= 0x08;  // accent-jack gate
  }

  // ---- Snare: tonal body (sine) + high-passed noise, separate envs -------
  {
    VoiceParams &p = params[1];
    v_phase[1] += v_base_inc[1];
    v_amp[1]  = U16U16MulShift16(v_amp[1],  v_amp_decay[1]);
    v_namp[1] = U16U16MulShift16(v_namp[1], v_namp_decay[1]);
    int16_t body  = S16U8MulShift8(Sine(v_phase[1] >> 8), (uint8_t)(v_amp[1] >> 8));
    int16_t nz    = S16U8MulShift8(noise_hp, (uint8_t)(v_namp[1] >> 8));
    int16_t s = ((body * p.tone_level) >> 8) + ((nz * p.noise_level) >> 8);
    if (s > 127) s = 127; else if (s < -128) s = -128;
    voice_sample[1] = (int8_t)s;
    if (v_amp[1] > 0x1400) new_accent |= 0x10;
  }

  // ---- Hat: high-passed noise + metallic tone, short exp env ------------
  // tone_level blends bright noise (0) <-> two-oscillator metallic "clang" (255);
  // base_inc sets the metal pitch, noise_level the overall hat level. Default
  // tone_level=0 => pure noise (unchanged from the original hat).
  {
    VoiceParams &p = params[2];
    static uint16_t hmetal_phase;
    v_namp[2] = U16U16MulShift16(v_namp[2], v_namp_decay[2]);
    uint8_t env = (uint8_t)(v_namp[2] >> 8);
    // two detuned squares (~1.625x apart) XORed -> cheap inharmonic metal tone.
    v_phase[2]   += v_base_inc[2];
    hmetal_phase += v_base_inc[2] + (v_base_inc[2] >> 1) + (v_base_inc[2] >> 3);
    int16_t metal = ((v_phase[2] ^ hmetal_phase) & 0x8000) ? 120 : -120;
    int16_t mix = (int16_t)(((int32_t)noise_hp * (255 - p.tone_level)) >> 8)
                + (int16_t)(((int32_t)metal * p.tone_level) >> 8);
    int16_t s = S16U8MulShift8(mix, env);
    s = S16U8MulShift8(s, p.noise_level);
    if (s > 127) s = 127; else if (s < -128) s = -128;
    voice_sample[2] = (int8_t)s;
    if (v_namp[2] > 0x1400) new_accent |= 0x20;
  }

  accent_bits = new_accent;
}

// Audio ISR: three 1-bit first-order sigma-delta modulators (one per drum jack)
// on the held voice samples, packed and clocked to the 595. Constant-time.
ISR(TIMER1_COMPA_vect)
{
  static uint16_t dsm0, dsm1, dsm2;
  static uint8_t decimator;

  uint8_t out = accent_bits;   // accent gates on bits 0x08/0x10/0x20

  dsm0 += (uint8_t)((uint8_t)voice_sample[0] + 128);
  if (dsm0 & 0xFF00) { out |= 0x01; dsm0 &= 0x00FF; }
  dsm1 += (uint8_t)((uint8_t)voice_sample[1] + 128);
  if (dsm1 & 0xFF00) { out |= 0x02; dsm1 &= 0x00FF; }
  dsm2 += (uint8_t)((uint8_t)voice_sample[2] + 128);
  if (dsm2 & 0xFF00) { out |= 0x04; dsm2 &= 0x00FF; }

  // Hand-written SPI to the 74HC595 (hot path — avoids a non-inlined call and a
  // full register save). Latch/SS is PB2: pulse low, clock the byte, pulse high.
  PORTB &= ~_BV(PB2);
  SPDR = out;
  while (!(SPSR & _BV(SPIF)))
    ;
  PORTB |= _BV(PB2);

  // Pace synthesis at the decimated rate; the main loop does the actual DSP.
  if (++decimator >= kSynthDecimation) {
    decimator = 0;
    ++synth_pending;
  }
}

// ===========================================================================
// Grids sequencer / clock / control (largely unchanged from Gritty-Grids)
// ===========================================================================

enum Parameter
{
  PARAMETER_NONE,
  PARAMETER_WAITING,
  PARAMETER_CLOCK_RESOLUTION,
  PARAMETER_TAP_TEMPO,
  PARAMETER_SWING,
  PARAMETER_GATE_MODE,
  PARAMETER_OUTPUT_MODE,
  PARAMETER_CLOCK_OUTPUT
};

uint32_t tap_duration = 0;
uint8_t led_pattern;
uint8_t led_off_timer;

int8_t swing_amount;

volatile Parameter parameter = PARAMETER_NONE;
volatile bool long_press_detected = false;

// Edit pages, cycled by a TAP long-hold: PERFORM (normal Grids play) -> META
// (the PARAMETER_* meta-params above) -> VOICE (drum-voice tuning) -> PERFORM.
// The clock, sequencer and audio keep running in every page so edits preview
// live; a page only changes what the six knobs edit.
enum EditPage
{
  PAGE_PERFORM,
  PAGE_META,
  PAGE_VOICE
};
volatile uint8_t edit_page = PAGE_PERFORM;
volatile uint8_t voice_edit_target = 0;   // which voice (0=BD,1=SD,2=HH) VOICE edits
volatile bool voice_target_changed = false; // TAP short-press asked to re-snapshot
const uint8_t kUpdatePeriod = F_CPU / 32 / 8000;
const uint16_t kMidiClockTimeout = 8000;

uint8_t clocked_by_midi = 0;
uint8_t mute = 0;
uint8_t external_clock = 0;
uint16_t midi_clock_timeout = 0;

// MIDI-in decode results, filled by PollMidiIn() (run every control tick) and
// consumed by HandleClockResetInputs(). Keeping a single MIDI reader lets us add
// note-in triggering without disturbing the validated clock behaviour.
volatile uint8_t midi_clock_ticks = 0;   // pending 0xF8 ticks to process
volatile bool midi_start_flag = false;   // 0xFA
volatile bool midi_continue_flag = false;// 0xFB
volatile bool midi_stop_flag = false;    // 0xFC

inline void UpdateLeds()
{
  uint8_t pattern;
  if (edit_page == PAGE_PERFORM)
  {
    if (led_off_timer)
    {
      --led_off_timer;
      if (!led_off_timer)
      {
        led_pattern = 0;
      }
    }
    if (mute)
    {
      led_pattern ^= LED_BD | LED_SD | LED_HH;
    }
    pattern = led_pattern;
    if (pattern_generator.tap_tempo())
    {
      if (pattern_generator.on_beat())
      {
        pattern |= LED_CLOCK;
      }
    }
    else
    {
      if (pattern_generator.on_first_beat())
      {
        pattern |= LED_CLOCK;
      }
    }
  }
  else if (edit_page == PAGE_VOICE)
  {
    // Voice-tuning page: LED_CLOCK steady + the selected voice's channel LED
    // blinking (~4 Hz) so it's unmistakable from the meta page. Short-press TAP
    // cycles the voice (see HandleTapButton).
    static uint16_t blink;
    ++blink;
    pattern = LED_CLOCK;
    uint8_t vled = (voice_edit_target == 0) ? LED_BD
                 : (voice_edit_target == 1) ? LED_SD
                                            : LED_HH;
    if (blink & 0x0400)
    {
      pattern |= vled;
    }
  }
  else
  {
    pattern = LED_CLOCK;
    switch (parameter)
    {
    case PARAMETER_CLOCK_RESOLUTION:
      pattern |= LED_BD >> pattern_generator.clock_resolution();
      break;

    case PARAMETER_CLOCK_OUTPUT:
      if (pattern_generator.output_clock())
      {
        pattern |= LED_ALL;
      }
      break;

    case PARAMETER_SWING:
      if (pattern_generator.swing())
      {
        pattern |= LED_ALL;
      }
      break;

    case PARAMETER_OUTPUT_MODE:
      if (pattern_generator.output_mode() == OUTPUT_MODE_DRUMS)
      {
        pattern |= LED_ALL;
      }
      break;

    case PARAMETER_TAP_TEMPO:
      if (pattern_generator.tap_tempo())
      {
        pattern |= LED_ALL;
      }
      break;

    case PARAMETER_GATE_MODE:
      if (pattern_generator.gate_mode())
      {
        pattern |= LED_ALL;
      }
    }
  }
  leds.Write(pattern);
}

// MIDI note velocity. When midi_accent_velocity is true, accented drum steps
// are sent at kVelocityAccent and unaccented ones at kVelocityNormal; when
// false (or in a mode without accents), every note is sent at kVelocityAccent.
// Only settable by recompiling for now, but kept as a variable so it can later
// be toggled at runtime (e.g. from the panel controls or a SysEx message).
bool midi_accent_velocity = true;
const uint8_t kVelocityAccent = 127;
const uint8_t kVelocityNormal = 90;

inline uint8_t NoteVelocity(bool has_accents, uint8_t accented)
{
  return (midi_accent_velocity && has_accents && !accented) ? kVelocityNormal : kVelocityAccent;
}

// Accent bits for BD, SD, HH (bits 0-2) from the pattern generator state.
// Only drum mode has accents: one bit per instrument in state bits 3-5, or with
// the clock output option a single common accent bit, applied to every
// instrument. In Euclidean mode bits 3-5 are reset pulses, not accents.
inline uint8_t AccentBits(uint8_t state)
{
  if (pattern_generator.output_mode() != OUTPUT_MODE_DRUMS)
  {
    return 0;
  }
  if (pattern_generator.output_clock())
  {
    return (state & OUTPUT_BIT_COMMON) ? 0x07 : 0;
  }
  return (state >> 3) & 0x07;
}

inline void BufferMidiMessages(uint8_t state)
{
  bool has_accents = pattern_generator.output_mode() == OUTPUT_MODE_DRUMS;
  uint8_t accents = AccentBits(state);

  if (state & 0x01)
  { // BD
    grids::MidiDevice::BufferNote(MIDI_CHANNEL, BD_NOTE, NoteVelocity(has_accents, accents & 0x01));
  }
  if (state & 0x02)
  { // SD
    grids::MidiDevice::BufferNote(MIDI_CHANNEL, SD_NOTE, NoteVelocity(has_accents, accents & 0x02));
  }
  if (state & 0x04)
  { // HH: accented steps play the open hi-hat
    uint8_t velocity = NoteVelocity(has_accents, accents & 0x04);
    if (accents & 0x04)
    {
      grids::MidiDevice::BufferNote(MIDI_CHANNEL, HH_ACCENT_NOTE, velocity);
    }
    else
    {
      grids::MidiDevice::BufferNote(MIDI_CHANNEL, HH_NOTE, velocity);
    }
  }
}

// Integration point: a sequencer trigger no longer writes a raw bit to the 595
// (the audio ISR owns it now) — instead a newly-set drum bit FIRES that voice's
// envelope. Accents come from AccentBits() (none in Euclidean mode).
inline void UpdateShiftRegister()
{
  static uint8_t previous_state = 0;
  uint8_t state = pattern_generator.state();

  if (mute)
  {
    state &= ~(0x07);
    if (pattern_generator.output_mode() == OUTPUT_MODE_DRUMS)
    {
      if (pattern_generator.output_clock())
      {
        state &= ~(OUTPUT_BIT_COMMON);
      }
      else
      {
        state &= ~(0x07 << 3);
      }
    }
  }

  if (state != previous_state)
  {
    uint8_t newly_set = state & ~previous_state;   // rising drum edges
    previous_state = state;

    uint8_t accents = AccentBits(state);
    if (newly_set & 0x01) { v_trigger |= 0x01; if (accents & 0x01) v_accent |= 0x01; }
    if (newly_set & 0x02) { v_trigger |= 0x02; if (accents & 0x02) v_accent |= 0x02; }
    if (newly_set & 0x04) { v_trigger |= 0x04; if (accents & 0x04) v_accent |= 0x04; }

    BufferMidiMessages(state);

    if (!state)
    {
      led_off_timer = 200;
    }
    else
    {
      led_pattern = pattern_generator.led_pattern();
      led_off_timer = 0;
    }
  }
}

uint8_t ticks_granularity[] = {6, 3, 1};

// Drain all pending MIDI-in bytes once per control tick. Realtime clock bytes
// are turned into flags/counters for HandleClockResetInputs; note-on messages on
// the drum channel trigger voices directly (independent of the sequencer).
inline void PollMidiIn()
{
  static uint8_t running_status = 0;
  static uint8_t data0 = 0;
  static uint8_t data_index = 0;

  uint8_t guard = 8;   // bound the per-tick work
  while (midi.readable() && guard--)
  {
    uint8_t byte = midi.ImmediateRead();

    if (byte >= 0xf8)          // system realtime — may interleave anywhere
    {
      if (byte == 0xf8) { if (midi_clock_ticks < 250) ++midi_clock_ticks; }
      else if (byte == 0xfa) { midi_start_flag = true; }
      else if (byte == 0xfb) { midi_continue_flag = true; }
      else if (byte == 0xfc) { midi_stop_flag = true; }
      continue;
    }

    if (byte & 0x80)           // status byte
    {
      running_status = byte;
      data_index = 0;
      continue;
    }

    if (!running_status) continue;   // data with no status — ignore

    if (data_index == 0)
    {
      data0 = byte;
      data_index = 1;
    }
    else
    {
      uint8_t status = running_status & 0xf0;
      uint8_t channel = running_status & 0x0f;
      data_index = 0;         // 2-data-byte messages complete
      if (channel == MIDI_CHANNEL)
      {
        // note-on (with velocity>0) triggers a voice; note-off / vel 0 ignored.
        if (status == 0x90 && byte > 0)
        {
          bool accent = byte >= 0x60;
          if (data0 == BD_NOTE || data0 == 0x23) { v_trigger |= 0x01; if (accent) v_accent |= 0x01; }
          else if (data0 == SD_NOTE || data0 == 0x25) { v_trigger |= 0x02; if (accent) v_accent |= 0x02; }
          else if (data0 == HH_NOTE || data0 == HH_ACCENT_NOTE) { v_trigger |= 0x04; if (accent) v_accent |= 0x04; }
        }
        else if (status == 0xb0)
        {
          // control change -> live voice tuning (CC 20..27).
          switch (data0)
          {
          case 20: params[0].base_inc = HZ_TO_INC(30) + (uint16_t)byte * 3; break;  // kick pitch
          case 21: params[0].amp_decay = 65000 + (uint16_t)byte * 4; break;         // kick decay
          case 22: params[1].base_inc = HZ_TO_INC(120) + (uint16_t)byte * 6; break; // snare tone
          case 23: params[1].amp_decay = 65000 + (uint16_t)byte * 4; break;         // snare body decay
          case 24: params[1].noise_level = byte << 1; break;                        // snare noise mix
          case 25: params[1].noise_decay = 65000 + (uint16_t)byte * 4; break;       // snare noise decay
          case 26: params[2].noise_decay = 64700 + (uint16_t)byte * 6; break;       // hat decay (open/closed)
          case 27: params[2].noise_level = byte << 1; break;                        // hat level
          default: break;
          }
        }
      }
    }
  }
}

inline void HandleClockResetInputs()
{
  static bool previous_clock_value;
  static bool previous_reset_value;

  if (midi_clock_timeout)
  {
    --midi_clock_timeout;
  }

  bool clock_value = !clock_input.Read();
  bool reset_value = !reset_input.Read();
  uint8_t num_ticks = 0;
  uint8_t increment = ticks_granularity[pattern_generator.clock_resolution()];

  // CLOCK
  if (clock.bpm() < 40 && !clock.locked())
  {
    if (!external_clock)
    {
      external_clock = 1;
      mute = 1;
    }
    if ((clock_value) && !(previous_clock_value))
    {
      if (!clocked_by_midi)
      {
        num_ticks = increment;
      }
    }
    if (!(clock_value) && (previous_clock_value))
    {
      pattern_generator.ClockFallingEdge();
    }
    // MIDI realtime, decoded in PollMidiIn().
    if (midi_clock_ticks)
    {
      midi_clock_timeout = kMidiClockTimeout;
      if (clocked_by_midi == 1)
      {
        num_ticks = 1;
      }
      --midi_clock_ticks;
    }
    if (midi_start_flag)
    {
      midi_start_flag = false;
      pattern_generator.Reset();
      clocked_by_midi = 1;
    }
    if (midi_continue_flag)
    {
      midi_continue_flag = false;
      clocked_by_midi = 1;
    }
    if (midi_stop_flag)
    {
      midi_stop_flag = false;
      clocked_by_midi = 2;
      grids::MidiDevice::BufferAllNotesOff(MIDI_CHANNEL);
    }
  }
  else
  {
    if (external_clock)
    {
      external_clock = 0;
      mute = 0;
      led_pattern = 0;
    }
    if (clocked_by_midi)
    {
      clocked_by_midi = 0;
      mute = 0;
      pattern_generator.Reset();
    }
    clock.Tick();
    clock.Wrap(swing_amount);
    if (clock.raising_edge())
    {
      num_ticks = increment;
    }
    if (clock.past_falling_edge())
    {
      pattern_generator.ClockFallingEdge();
    }
  }

  // RESET
  if (clocked_by_midi)
  {
    if ((reset_value) && !(previous_reset_value))
    {
      mute = 1;
    }
    if (!(reset_value) && (previous_reset_value))
    {
      mute = 0;
      led_pattern = 0;
    }
  }
  else
  {
    if ((reset_value) && !(previous_reset_value))
    {
      pattern_generator.Reset();
      if (clock.bpm() >= 40 || clock.locked())
      {
        clock.Reset();
      }
    }
  }
  previous_clock_value = clock_value;
  previous_reset_value = reset_value;

  if (num_ticks)
  {
    swing_amount = pattern_generator.swing_amount();
    pattern_generator.TickClock(num_ticks);
  }
}

enum SwitchState
{
  SWITCH_STATE_JUST_PRESSED = 0xfe,
  SWITCH_STATE_PRESSED = 0x00,
  SWITCH_STATE_JUST_RELEASED = 0x01,
  SWITCH_STATE_RELEASED = 0xff
};

inline void HandleTapButton()
{
  static uint8_t switch_state = 0xff;
  static uint16_t switch_hold_time = 0;

  switch_state = switch_state << 1;
  if (button_input.Read())
  {
    switch_state |= 1;
  }

  if (switch_state == SWITCH_STATE_JUST_PRESSED)
  {
    if (edit_page == PAGE_PERFORM)
    {
      if (clocked_by_midi == 2)
      {
        pattern_generator.Reset();
        clocked_by_midi = 1;
        mute = 0;
        led_pattern = 0;
      }
      else if (clocked_by_midi)
      {
        if (mute)
        {
          mute = 0;
          led_pattern = 0;
        }
        else
        {
          mute = 1;
        }
      }
      else
      {
        if (external_clock == 1)
        {
          mute = 0;
          led_pattern = 0;
          external_clock = 2;
          if (midi_clock_timeout)
          {
            pattern_generator.Reset();
            clocked_by_midi = 1;
          }
        }
        if (!pattern_generator.tap_tempo())
        {
          pattern_generator.Reset();
          if (clock.bpm() >= 40 || clock.locked())
          {
            clock.Reset();
          }
        }
        else
        {
          uint32_t new_bpm = (F_CPU * 60L) / (32L * kUpdatePeriod * tap_duration);
          if (new_bpm >= 30 && new_bpm <= 480)
          {
            clock.Update(new_bpm, pattern_generator.clock_resolution());
            clock.Reset();
            clock.Lock();
          }
          else
          {
            clock.Unlock();
          }
          tap_duration = 0;
        }
      }
    }
    else if (edit_page == PAGE_VOICE)
    {
      // In the voice-tuning page a short TAP cycles which drum voice the knobs
      // edit (BD -> SD -> HH); ask ScanPots to re-snapshot the pots so the knob
      // positions don't instantly overwrite the newly-selected voice's params.
      ++voice_edit_target;
      if (voice_edit_target > 2)
      {
        voice_edit_target = 0;
      }
      voice_target_changed = true;
    }
    switch_hold_time = 0;
  }
  else if (switch_state == SWITCH_STATE_PRESSED)
  {
    ++switch_hold_time;
    if (switch_hold_time == 500)
    {
      long_press_detected = true;
    }
  }
}

ISR(TIMER2_COMPA_vect, ISR_NOBLOCK)
{
  static uint8_t switch_debounce_prescaler;

  ++tap_duration;
  ++switch_debounce_prescaler;
  if (switch_debounce_prescaler >= 10)
  {
    HandleTapButton();
    switch_debounce_prescaler = 0;
  }

  PollMidiIn();
  HandleClockResetInputs();

  adc.Scan();

  pattern_generator.IncrementPulseCounter();
  UpdateShiftRegister();
  UpdateLeds();
}

static int16_t pot_values[8];

static void SnapshotPots()
{
  for (uint8_t i = 0; i < ADC_CHANNEL_LAST; ++i)
  {
    pot_values[i] = adc.Read8(i);
  }
}

void ScanPots()
{
  if (long_press_detected)
  {
    // Cycle the edit page: PERFORM -> META -> VOICE -> PERFORM. Snapshot the pot
    // positions on each entry so a knob only takes effect once moved past the
    // catch threshold, and persist to EEPROM on the way out of each edit page.
    if (edit_page == PAGE_PERFORM)
    {
      SnapshotPots();
      edit_page = PAGE_META;
      parameter = PARAMETER_WAITING;
    }
    else if (edit_page == PAGE_META)
    {
      pattern_generator.SaveSettings();
      SnapshotPots();
      edit_page = PAGE_VOICE;
      parameter = PARAMETER_NONE;
      voice_edit_target = 0;
    }
    else
    {
      SaveVoiceParams();
      edit_page = PAGE_PERFORM;
      parameter = PARAMETER_NONE;
    }
    long_press_detected = false;
  }

  if (edit_page == PAGE_PERFORM)
  {
    uint8_t bpm = adc.Read8(ADC_CHANNEL_TEMPO);
    bpm = U8U8MulShift8(bpm, 220) + 20;
    if (bpm != clock.bpm() && !clock.locked())
    {
      clock.Update(bpm, pattern_generator.clock_resolution());
    }
    PatternGeneratorSettings *settings = pattern_generator.mutable_settings();
    settings->options.drums.x = ~adc.Read8(ADC_CHANNEL_X_CV);
    settings->options.drums.y = ~adc.Read8(ADC_CHANNEL_Y_CV);
    settings->options.drums.randomness = ~adc.Read8(ADC_CHANNEL_RANDOMNESS_CV);
    settings->density[0] = ~adc.Read8(ADC_CHANNEL_BD_DENSITY_CV);
    settings->density[1] = ~adc.Read8(ADC_CHANNEL_SD_DENSITY_CV);
    settings->density[2] = ~adc.Read8(ADC_CHANNEL_HH_DENSITY_CV);
    return;
  }

  if (edit_page == PAGE_VOICE)
  {
    // Drum-voice tuning. Each knob edits one param of the currently-selected
    // voice (short-press TAP cycles the voice). Catch behaviour as in META: a
    // knob only takes over its param once moved. base_inc/pitch envelopes only
    // affect the kick's audible pitch; on snare/hat those knobs are inert.
    if (voice_target_changed)
    {
      voice_target_changed = false;
      SnapshotPots();
    }
    VoiceParams &p = params[voice_edit_target];
    for (uint8_t i = 0; i < ADC_CHANNEL_LAST; ++i)
    {
      int16_t value = adc.Read8(i);
      int16_t delta = value - pot_values[i];
      if (delta < 0)
      {
        delta = -delta;
      }
      if (delta > 24)
      {
        pot_values[i] = value;
        uint8_t u = (uint8_t)value;
        switch (i)
        {
        case ADC_CHANNEL_BD_DENSITY_CV:  // pitch: ~20 Hz .. ~2 kHz
          p.base_inc = HZ_TO_INC(20) + (uint16_t)u * 64;
          break;
        case ADC_CHANNEL_SD_DENSITY_CV:  // amp/body length: ~16 .. ~320 ms
          p.amp_decay = 65000 + (uint16_t)u * 2;
          break;
        case ADC_CHANNEL_HH_DENSITY_CV:  // pitch-env depth (kick "punch")
          p.pitch_mod0 = (uint16_t)u * 20;
          break;
        case ADC_CHANNEL_X_CV:           // tonal component level
          p.tone_level = u;
          break;
        case ADC_CHANNEL_Y_CV:           // noise component level
          p.noise_level = u;
          break;
        case ADC_CHANNEL_RANDOMNESS_CV:  // noise length: ~16 .. ~320 ms
          p.noise_decay = 65000 + (uint16_t)u * 2;
          break;
        case ADC_CHANNEL_TEMPO:          // pitch-env length (kick sweep time)
          p.pitch_decay = 64800 + (uint16_t)u * 2;
          break;
        }
      }
    }
    return;
  }

  // PAGE_META: the original Grids meta-parameter edit loop.
  {
    for (uint8_t i = 0; i < 8; ++i)
    {
      int16_t value = adc.Read8(i);
      int16_t delta = value - pot_values[i];
      if (delta < 0)
      {
        delta = -delta;
      }
      if (delta > 32)
      {
        pot_values[i] = value;
        switch (i)
        {
        case ADC_CHANNEL_BD_DENSITY_CV:
          parameter = PARAMETER_CLOCK_RESOLUTION;
          pattern_generator.set_clock_resolution((255 - value) >> 6);
          clock.Update(clock.bpm(), pattern_generator.clock_resolution());
          pattern_generator.Reset();
          break;

        case ADC_CHANNEL_SD_DENSITY_CV:
          parameter = PARAMETER_TAP_TEMPO;
          pattern_generator.set_tap_tempo(!(value & 0x80));
          if (!pattern_generator.tap_tempo())
          {
            clock.Unlock();
          }
          break;

        case ADC_CHANNEL_HH_DENSITY_CV:
          parameter = PARAMETER_SWING;
          pattern_generator.set_swing(!(value & 0x80));
          break;

        case ADC_CHANNEL_X_CV:
          parameter = PARAMETER_OUTPUT_MODE;
          pattern_generator.set_output_mode(!(value & 0x80) ? 1 : 0);
          break;

        case ADC_CHANNEL_Y_CV:
          parameter = PARAMETER_GATE_MODE;
          pattern_generator.set_gate_mode(!(value & 0x80));
          break;

        case ADC_CHANNEL_RANDOMNESS_CV:
          parameter = PARAMETER_CLOCK_OUTPUT;
          pattern_generator.set_output_clock(!(value & 0x80));
          break;
        }
      }
    }
  }
}

void Init()
{
  sei();

  grids::MidiDevice::Init(midi);

  leds.set_mode(DIGITAL_OUTPUT);
  reset_input.EnablePullUpResistor();
  button_input.EnablePullUpResistor();
  clock_input.EnablePullUpResistor();

  clock.Init();
  adc.Init();
  adc.set_num_inputs(ADC_CHANNEL_LAST);
  Adc::set_reference(ADC_DEFAULT);
  Adc::set_alignment(ADC_LEFT_ALIGNED);
  pattern_generator.Init();
  shift_register.Init();

  // Restore any saved drum-voice tuning (falls back to the compiled defaults on
  // a fresh chip). Done before the timers start so no ISR reads a half-loaded
  // params[] mid-copy.
  LoadVoiceParams();

  // Timer2: ~8 kHz Grids control tick (CTC, prescaler /32).
  TCCR2A = _BV(WGM21);
  TCCR2B = 3;
  OCR2A = kUpdatePeriod - 1;
  TIMSK2 |= _BV(OCIE2A);

  // Timer1: 31.25 kHz audio/PDM tick (CTC, prescaler /1).
  TCCR1A = 0;
  TCCR1B = _BV(WGM12) | _BV(CS10);
  OCR1A = kOcr1a;
  TIMSK1 |= _BV(OCIE1A);
}

int main(void)
{
  ResetWatchdog();
  Init();
  clock.Update(120, pattern_generator.clock_resolution());

  while (1)
  {
    // Render the synth samples the audio ISR has queued, but only a bounded
    // batch per iteration so ScanPots() always gets to run — otherwise, if the
    // synth ever can't quite keep up, synth_pending pins high and the pattern
    // knobs (read in ScanPots) go dead. If the synth briefly lags it just plays
    // catch-up; the knobs stay responsive.
    uint8_t budget = 8;
    while (synth_pending && budget--)
    {
      RenderVoices();
      cli();
      --synth_pending;
      sei();
    }

    ScanPots();

    // Non-blocking MIDI-out: send a byte ONLY when the UART is ready, so a
    // multi-note burst never stalls the loop (which would starve the synth and
    // click the audio). We loop back and service synth_pending immediately.
    if (buffer_tail != buffer_head && (UCSR0A & _BV(UDRE0)))
    {
      UDR0 = output_buffer[buffer_tail];
      buffer_tail = (buffer_tail + 1) % MIDI_BUFFER_SIZE;
    }
  }
}
