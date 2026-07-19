// Gritty-osc — Milestone 6: six-voice DDS chord oscillator.
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
// Control (default = Option B; see OSC_DENSITY_AS_PITCH below to switch):
//   Y knob/CV       -> root pitch
//   chaos knob/CV   -> chord type (8: unison-superpulse/power/maj/min/sus4/…)
//   BD/SD/HH knobs  -> per-channel PWM duty of the three chord tones
//   X knob/CV       -> detune (1st half: sub-octaves; 2nd half: adds the mains)
//   CLOCK/RESET/btn -> trigger function: arp / rotate / pluck (see OSC_TRIG_*)
//   TEMPO           -> arp range 1..4 octaves (OSC_TRIG_ARP only)
//   voices 3/4/5    -> sub-octaves of voices 0/1/2 (so all six sound)
//
// Dropped to 16-bit accumulators + Fs = 62.5 kHz so six voices fit the ISR
// budget at 16 MHz (see plans/gritty-osc.md, Milestone 4).
//
// M5: pitch comes from a precomputed note -> tuning-word table (equal
// temperament, generated for Fs = 62.5 kHz), so no powf()/float is used at
// runtime — a plain flash lookup replaces the per-change float pow.
//
// M7 (optional, build-time): MIDI note control. Define one of the OSC_MIDI_*
// modes below to play the six voices from the D0/D1 TRS-A MIDI input instead of
// the knob/CV + trigger control — a one-key chord, three MIDI channels, 3- or
// 6-voice polyphony, or a split keyboard. Requires TRIGGERSPACE_PINOUT (so that
// D0/D1 are free as a UART).
//
// Fork of Gritty-Grids, based on Mutable Instruments' Grids (Emilie Gillet).
// GPLv3 (see LICENSE).

#include <avr/interrupt.h>
#include <avr/pgmspace.h>

#include "avrlib/adc.h"
#include "avrlib/watchdog_timer.h"

#include "grids/hardware_config.h"

// ---- MIDI note control (build-time) --------------------------------------
// Define exactly ONE of these to turn the module into a MIDI-controlled
// oscillator (needs TRIGGERSPACE_PINOUT — MIDI lives on the D0/D1 UART). With
// none defined the knob/CV + trigger control is used and MIDI is off.
//   OSC_MIDI_MONO_ROOT  (A) one channel; the (last) held note sets the ROOT and
//                           the panel still shapes it — chaos = chord type,
//                           X = detune, density knobs = per-channel PWM. A whole
//                           6-voice chord from a single key.
//   OSC_MIDI_MULTI_3CH  (B) three channels (base, base+1, base+2) drive osc
//                           0/1/2 independently; each sub tracks its main an
//                           octave down. Mod wheel (CC1) = global PWM; CC94 =
//                           detune depth.
//   OSC_MIDI_POLY3      (C1) 3-voice polyphony, round-robin over osc 0/1/2; each
//                           sub doubles its main an octave down. Mod wheel = PWM.
//   OSC_MIDI_POLY6      (C2) 6-voice polyphony, round-robin over all six oscs
//                           (subs are independent voices, no doubling).
//   OSC_MIDI_SPLIT      (D) split keyboard: notes >= OSC_SPLIT_NOTE round-robin
//                           over the mains (0/1/2); notes below are a bassline
//                           round-robined over the subs (3/4/5).
//#define OSC_MIDI_MONO_ROOT
//#define OSC_MIDI_MULTI_3CH
//#define OSC_MIDI_POLY3
//#define OSC_MIDI_POLY6
//#define OSC_MIDI_SPLIT

// Base MIDI channel (0-based: 0 = channel 1). Mode B also uses base+1, base+2.
#ifndef OSC_MIDI_CHANNEL
#define OSC_MIDI_CHANNEL 0
#endif
// Split point for OSC_MIDI_SPLIT, as a MIDI note number (60 = middle C = C4).
// 48 = C3: notes >= this play the mains, below play the sub/bass voices. Try 60.
#ifndef OSC_SPLIT_NOTE
#define OSC_SPLIT_NOTE 48
#endif

#if defined(OSC_MIDI_MONO_ROOT) || defined(OSC_MIDI_MULTI_3CH) || \
    defined(OSC_MIDI_POLY3) || defined(OSC_MIDI_POLY6) || defined(OSC_MIDI_SPLIT)
#define OSC_MIDI
#endif
#if defined(OSC_MIDI) && !TRIGGERSPACE_PINOUT
#error "OSC_MIDI needs the D0/D1 UART; build with TRIGGERSPACE_PINOUT 1."
#endif
#ifdef OSC_MIDI
#include "grids/midi.h"
#endif

using namespace avrlib;
using namespace grids;

ShiftRegister shift_register;
AdcInputScanner adc;
ClockInput clock_input;   // D8 (PB0) trigger in -> "step" (advance)
ResetInput reset_input;   // D2 trigger in       -> "reset"
ButtonInput button_input; // D3 button           -> cycle arp mode (or manual step)
Leds leds;                // D4..D7 -> arp-mode readout
#ifdef OSC_MIDI
MidiIO midi;              // D0/D1 TRS-A MIDI (input used; output unused here)
#endif

static const uint8_t kNumVoices = 6;

// The three "density/fill" pots, used as per-channel PWM (and pitch in the
// OSC_DENSITY_AS_PITCH / mode-A layouts). File scope so RenderMidi() shares them.
static const uint8_t kDensityCh[3] = {
    ADC_CHANNEL_BD_DENSITY_CV,
    ADC_CHANNEL_SD_DENSITY_CV,
    ADC_CHANNEL_HH_DENSITY_CV};

// ---- Control layout (build-time) -----------------------------------------
// Default (Option B): Y = root pitch, chaos = chord type, the three density/fill
// knobs = per-channel PWM, X = detune (movement), TEMPO reserved. Define
// OSC_DENSITY_AS_PITCH to instead make the density knobs independent per-channel
// pitch (the M5 layout): TEMPO = transpose, X = global PWM, Y/chaos unused.
//#define OSC_DENSITY_AS_PITCH

// ---- Trigger function (CLOCK D8 / RESET D2 / button D3) -------------------
// CLOCK and the button both fire "step"; RESET fires "reset". Pick exactly one
// behaviour (Option B builds only; ignored under OSC_DENSITY_AS_PITCH):
//   ARP    - CLOCK steps through the chord tones across octaves (chord = the
//            pool); all six voices play the stepping note. TEMPO = range (1..4
//            octaves). The BUTTON cycles direction mode (up / down / up-down inc /
//            up-down exc / random / random-walk), shown on the LEDs as 1..6.
//   ROTATE - keep the chord sounding; each step rotates the voicing/inversion.
//   PLUCK  - keep the chord sounding; each step fires a decaying PWM-duty
//            envelope (a percussive re-articulation).
#define OSC_TRIG_ARP
//#define OSC_TRIG_ROTATE
//#define OSC_TRIG_PLUCK

// ---- DDS sample clock ----------------------------------------------------
// Timer2 in CTC mode, prescaler /1: Fs = F_CPU / (OCR2A + 1).
// 16 MHz / 256 = 62.5 kHz. That gives a 256-cycle ISR budget for six 16-bit
// voice updates + the packed SPI write; profile before raising Fs. One voice at
// 100 kHz fit in ~137 cyc, so six at 62.5 kHz is the conservative first cut.
static const uint32_t kSampleRate = 62500UL;
static const uint8_t  kOcr2a      = (F_CPU / kSampleRate) - 1;   // = 255 @ 16 MHz

// Per-voice 16-bit DDS state. `phase[]` is ISR-private (not volatile so the
// compiler can keep the accumulators hot). `increment[]` (tuning words) and
// `duty[]` are written from the main loop under cli and read in the ISR, so they
// are volatile. Output bit i is high while phase[i] < duty[i]. Duty is now per
// VOICE (six values): the non-MIDI builds set the sub v+3 to match its main v,
// but the MIDI poly modes drive all six independently and silence a released
// voice by setting its duty to 0 (its pin then holds low -> no audio).
uint16_t phase[kNumVoices] = {0};
volatile uint16_t increment[kNumVoices] = {0};
volatile uint16_t duty[kNumVoices] =
    {0x8000, 0x8000, 0x8000, 0x8000, 0x8000, 0x8000};   // 50% square default

ISR(TIMER2_COMPA_vect)
{
  uint8_t out = 0;
  uint8_t sum = 0;     // count of voices currently high (0..6) = mix level
  // Unrolled, one duty load per voice so the MIDI poly modes can drive the
  // sub-oscillators independently (and gate a single voice to silence). Six
  // 16-bit duty loads still fit the 62.5 kHz / 256-cycle ISR budget with room.
  phase[0] += increment[0]; if (phase[0] < duty[0]) { out |= 0x01; ++sum; }
  phase[1] += increment[1]; if (phase[1] < duty[1]) { out |= 0x02; ++sum; }
  phase[2] += increment[2]; if (phase[2] < duty[2]) { out |= 0x04; ++sum; }
  phase[3] += increment[3]; if (phase[3] < duty[3]) { out |= 0x08; ++sum; }
  phase[4] += increment[4]; if (phase[4] < duty[4]) { out |= 0x10; ++sum; }
  phase[5] += increment[5]; if (phase[5] < duty[5]) { out |= 0x20; ++sum; }

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

// Chord table: 8 types, three semitone intervals each (voice 0/1/2 = root +
// interval). Selected by the chaos knob. Voice v's accent (v+3) tracks one octave
// below voice v.
static const uint8_t kNumChords = 8;
static const int8_t kChord[kNumChords][3] PROGMEM = {
  { 0,  0,  0},   // unison — all six voices at root (detuned "superpulse")
  { 0,  7, 12},   // power (root / 5th / octave)
  { 0,  4,  7},   // major
  { 0,  3,  7},   // minor
  { 0,  5,  7},   // sus4
  { 0,  4, 11},   // major 7 (no 5th)
  { 0,  3, 10},   // minor 7 (no 5th)
  { 0,  5, 10},   // quartal (stacked 4ths)
};

// 8-bit knob -> 16-bit PWM duty, ~12%..88% (50% = square). Trimmed extremes keep
// the whole throw audible (matches the single-voice PWM mapping from M3).
static inline uint16_t KnobToDuty(uint8_t k)
{
  return 0x1F00 + (uint16_t)k * 0xC2;
}

// Per-voice detune spread (signed). At full detune (X = 255) each voice is
// offset by spread * ~0.77% (spread here = ±3 -> ~±40 cents). Voice 0 (root main)
// is nearly anchored so the chord still reads in tune; the rest spread apart so
// their harmonics beat -> ensemble/supersaw shimmer. Voices 0/1/2 = mains,
// 3/4/5 = subs.
static const int8_t kDetuneSpread[kNumVoices] = { -1, +1, -2, +2, -3, +3 };

// Apply a detune offset to a tuning word: base + base * det * spread / 2^15.
static inline uint16_t Detune(uint16_t base, uint8_t det, int8_t spread)
{
  return base + (int16_t)(((int32_t)base * det * spread) >> 15);
}

// Arpeggiator direction modes (cycled by the button; shown on the LEDs as a
// binary count 1..6). RANDWALK is a Brownian +/-1 step, distinct from RANDOM.
enum ArpMode {
  ARP_UP, ARP_DOWN, ARP_UPDOWN_INC, ARP_UPDOWN_EXC, ARP_RANDOM, ARP_RANDWALK,
  ARP_NUM_MODES
};

// Advance the arp step index over n steps per the current mode. `dir` holds the
// up/down direction for the ping-pong modes; `rng` is a private LCG state.
static uint8_t AdvanceArp(uint8_t step, uint8_t n, uint8_t mode,
                          int8_t *dir, uint16_t *rng)
{
  *rng = (uint16_t)(*rng * 31421u + 6927u);
  uint8_t r = (uint8_t)(*rng >> 8);
  switch (mode)
  {
    default:  // ARP_UP
      step = (uint8_t)(step + 1 >= n ? 0 : step + 1);
      break;
    case ARP_DOWN:
      step = (uint8_t)(step == 0 ? n - 1 : step - 1);
      break;
    case ARP_UPDOWN_INC:  // ping-pong, endpoints repeated
      if (*dir > 0) { if (step + 1 >= n) *dir = -1; else step++; }
      else          { if (step == 0)     *dir =  1; else step--; }
      break;
    case ARP_UPDOWN_EXC:  // ping-pong, endpoints played once
      if (*dir > 0) { if (step + 1 >= n) { *dir = -1; step = (uint8_t)(n >= 2 ? n - 2 : 0); } else step++; }
      else          { if (step == 0)     { *dir =  1; step = (uint8_t)(n >= 2 ? 1 : 0);     } else step--; }
      break;
    case ARP_RANDOM:
      step = (uint8_t)(((uint16_t)r * n) >> 8);
      break;
    case ARP_RANDWALK: {
      int8_t d = (r < 85) ? -1 : (r < 170) ? 0 : 1;
      int8_t s = (int8_t)step + d;
      step = (uint8_t)(s < 0 ? 0 : (s >= (int8_t)n ? n - 1 : s));
      break;
    }
  }
  if (step >= n) step = 0;  // safety after a range change
  return step;
}

#ifdef OSC_MIDI
// ===========================================================================
// MIDI note control (build-time mode select at the top of the file). All state
// here lives in the main loop: PollMidi() updates it from the D0/D1 UART and
// RenderMidi() turns it into the increment[]/duty[] the audio ISR reads. A
// released voice is silenced by setting its duty to 0 (its pin then holds low).
// ===========================================================================

static int8_t  midi_note[kNumVoices];   // note per voice (multi/poly/split modes)
static bool    midi_gate[kNumVoices];   // is that voice currently sounding?
static uint8_t midi_rr = 0;             // round-robin allocation pointer
static uint8_t midi_mod = 0;            // mod wheel (CC1) -> global PWM
static uint8_t midi_detune = 0;         // CC94 -> detune depth (0..127 -> 0..255)

#if defined(OSC_MIDI_MONO_ROOT)
static int8_t  held_notes[8];           // last-note-priority stack (mode A)
static uint8_t held_count = 0;
static int8_t  last_root = 60;          // pitch to hold when all keys are up

static void HeldRemove(int8_t n)
{
  for (uint8_t i = 0; i < held_count; ++i)
  {
    if (held_notes[i] == n)
    {
      for (uint8_t j = i; j + 1 < held_count; ++j) held_notes[j] = held_notes[j + 1];
      --held_count;
      return;
    }
  }
}
static void HeldPush(int8_t n)
{
  HeldRemove(n);                        // no duplicates
  if (held_count < 8) held_notes[held_count++] = n;
}
#endif

// Mod wheel 0 -> 50% square; turning it up narrows the pulse (thinner/brighter).
static inline uint16_t MidiDuty()
{
  return (uint16_t)(0x8000u - (uint16_t)midi_mod * 0xC2);   // square .. ~12%
}

#if defined(OSC_MIDI_POLY3) || defined(OSC_MIDI_POLY6) || defined(OSC_MIDI_SPLIT)
// Assign a voice in [lo..hi] to a new note: reuse a released voice if one is
// free, else steal the next in round-robin order.
static uint8_t AllocVoice(uint8_t lo, uint8_t hi)
{
  uint8_t n = (uint8_t)(hi - lo + 1);
  for (uint8_t k = 0; k < n; ++k)
    if (!midi_gate[lo + k]) return (uint8_t)(lo + k);
  uint8_t v = (uint8_t)(lo + (midi_rr % n));
  midi_rr = (uint8_t)((midi_rr + 1) % n);
  return v;
}
static void ReleaseNote(int8_t note, uint8_t lo, uint8_t hi)
{
  for (uint8_t v = lo; v <= hi; ++v)
    if (midi_gate[v] && midi_note[v] == note) midi_gate[v] = false;
}
#endif

static void AllNotesOff()
{
  for (uint8_t v = 0; v < kNumVoices; ++v) midi_gate[v] = false;
#if defined(OSC_MIDI_MONO_ROOT)
  held_count = 0;
#endif
}

// Parse the incoming MIDI byte stream (polled; system-realtime bytes ignored).
// Routes note on/off and CC (mod wheel, detune, all-notes-off) to the per-mode
// state above. Handles running status and 1-byte (program/pressure) messages.
static void PollMidi()
{
  static uint8_t status = 0, data0 = 0, idx = 0;
  uint8_t guard = 16;                   // bound the per-call work
  while (midi.readable() && guard--)
  {
    uint8_t b = midi.ImmediateRead();
    if (b >= 0xf8) continue;            // system realtime — ignore here
    if (b & 0x80) { status = b; idx = 0; continue; }   // status byte
    if (!status) continue;

    uint8_t msg = status & 0xf0;
    if (idx == 0)
    {
      data0 = b;
      if (msg == 0xc0 || msg == 0xd0) continue;   // 1-byte messages: complete
      idx = 1;
      continue;
    }
    idx = 0;                            // 2-byte message complete

    uint8_t ch = status & 0x0f;
    bool note_on  = (msg == 0x90 && b > 0);
    bool note_off = (msg == 0x80) || (msg == 0x90 && b == 0);

    if (msg == 0xb0)                    // control change
    {
      if (data0 == 1)  midi_mod = b;              // mod wheel -> PWM
      else if (data0 == 94) midi_detune = b;      // detune depth
      else if (data0 == 123 && b == 0) AllNotesOff();
      continue;
    }
    if (!note_on && !note_off) continue;

#if defined(OSC_MIDI_MULTI_3CH)
    if (ch >= OSC_MIDI_CHANNEL && ch < OSC_MIDI_CHANNEL + 3)
    {
      uint8_t v = (uint8_t)(ch - OSC_MIDI_CHANNEL);
      if (note_on)                    { midi_note[v] = data0; midi_gate[v] = true; }
      else if (midi_note[v] == data0) { midi_gate[v] = false; }
    }
#else
    if (ch != OSC_MIDI_CHANNEL) continue;
# if defined(OSC_MIDI_MONO_ROOT)
    if (note_on) HeldPush(data0); else HeldRemove(data0);
# elif defined(OSC_MIDI_POLY3)
    if (note_on) { uint8_t v = AllocVoice(0, 2); midi_note[v] = data0; midi_gate[v] = true; }
    else ReleaseNote(data0, 0, 2);
# elif defined(OSC_MIDI_POLY6)
    if (note_on) { uint8_t v = AllocVoice(0, 5); midi_note[v] = data0; midi_gate[v] = true; }
    else ReleaseNote(data0, 0, 5);
# elif defined(OSC_MIDI_SPLIT)
    if (note_on)
    {
      uint8_t v = (data0 >= OSC_SPLIT_NOTE) ? AllocVoice(0, 2) : AllocVoice(3, 5);
      midi_note[v] = data0; midi_gate[v] = true;
    }
    else
    {
      if (data0 >= OSC_SPLIT_NOTE) ReleaseNote(data0, 0, 2);
      else                         ReleaseNote(data0, 3, 5);
    }
# endif
#endif
  }
}

// Translate the MIDI note state into the increment[]/duty[] the ISR reads.
static void RenderMidi()
{
#if defined(OSC_MIDI_MONO_ROOT)
  // (A) one key -> a full 6-voice chord; the panel still shapes it (chaos =
  // chord type, X = detune, density knobs = per-channel PWM).
  int8_t root = held_count ? held_notes[held_count - 1] : last_root;
  if (held_count) last_root = root;
  bool sound = (held_count > 0);
  uint8_t chord    = (uint8_t)((255 - adc.Read8(ADC_CHANNEL_RANDOMNESS_CV)) >> 5);
  uint8_t det      = (uint8_t)(255 - adc.Read8(ADC_CHANNEL_X_CV));
  uint8_t sub_det  = det;
  uint8_t main_det = (det > 128) ? (uint8_t)(det - 128) : 0;
  int8_t  sub_off  = (chord == 0) ? 0 : -12;
  for (uint8_t v = 0; v < 3; ++v)
  {
    int8_t note = (int8_t)(root + (int8_t)pgm_read_byte(&kChord[chord][v]));
    uint16_t inc_main = Detune(NoteTuningWord(note),           main_det, kDetuneSpread[v]);
    uint16_t inc_sub  = Detune(NoteTuningWord(note + sub_off), sub_det,  kDetuneSpread[v + 3]);
    uint16_t dv = KnobToDuty((uint8_t)(255 - adc.Read8(kDensityCh[v])));
    cli();
    increment[v]     = inc_main;
    increment[v + 3] = inc_sub;
    duty[v]     = sound ? dv : 0;
    duty[v + 3] = sound ? dv : 0;
    sei();
  }

#elif defined(OSC_MIDI_MULTI_3CH)
  // (B) three channels drive osc 0/1/2; each sub tracks its main an octave down.
  uint16_t gd = MidiDuty();
  for (uint8_t v = 0; v < 3; ++v)
  {
    bool on = midi_gate[v];
    uint16_t inc_main = Detune(NoteTuningWord(midi_note[v]),      midi_detune, kDetuneSpread[v]);
    uint16_t inc_sub  = Detune(NoteTuningWord(midi_note[v] - 12), midi_detune, kDetuneSpread[v + 3]);
    cli();
    increment[v]     = inc_main;
    increment[v + 3] = inc_sub;
    duty[v]     = on ? gd : 0;
    duty[v + 3] = on ? gd : 0;
    sei();
  }

#elif defined(OSC_MIDI_POLY3)
  // (C1) 3-voice polyphony; each sub doubles its main an octave down.
  uint16_t gd = MidiDuty();
  for (uint8_t v = 0; v < 3; ++v)
  {
    bool on = midi_gate[v];
    cli();
    increment[v]     = NoteTuningWord(midi_note[v]);
    increment[v + 3] = NoteTuningWord(midi_note[v] - 12);
    duty[v]     = on ? gd : 0;
    duty[v + 3] = on ? gd : 0;
    sei();
  }

#else   // (C2) OSC_MIDI_POLY6 or (D) OSC_MIDI_SPLIT: all six voices independent.
  uint16_t gd = MidiDuty();
  for (uint8_t v = 0; v < kNumVoices; ++v)
  {
    bool on = midi_gate[v];
    cli();
    increment[v] = NoteTuningWord(midi_note[v]);
    duty[v]      = on ? gd : 0;
    sei();
  }
#endif
}
#endif  // OSC_MIDI

void Init()
{
  cli();

  shift_register.Init();

  adc.Init();
  adc.set_num_inputs(ADC_CHANNEL_LAST);
  Adc::set_reference(ADC_DEFAULT);
  Adc::set_alignment(ADC_LEFT_ALIGNED);

  // Trigger inputs (active low): CLOCK/RESET jacks + tap button, all pulled up.
  clock_input.EnablePullUpResistor();
  reset_input.EnablePullUpResistor();
  button_input.EnablePullUpResistor();
  leds.set_mode(DIGITAL_OUTPUT);

#ifdef OSC_MIDI
  grids::MidiDevice::Init(midi);   // D0/D1 UART @ 31250 baud (input polled)
#endif

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

#ifdef OSC_MIDI
  // MIDI-controlled oscillator (mode selected at the top of the file). Poll the
  // UART and re-render the six voices every pass; the trigger/arp handling below
  // is compiled out. Voices default to a gated-off middle C so nothing sounds
  // until a note arrives.
  for (uint8_t v = 0; v < kNumVoices; ++v) { midi_note[v] = 60; midi_gate[v] = false; }
  while (1)
  {
    adc.Scan();     // keep the scanner fresh (mode A still reads the knobs)
    PollMidi();
    RenderMidi();
  }
#else
  // All of these pots are wired reversed (stock Grids reads them with ~), so
  // every raw reading is inverted to give 0..255 = clockwise. Pitch is cheap now
  // (note-LUT), so all voices are recomputed every loop — no change tracking.
  // (kDensityCh is now file scope, shared with the MIDI renderer.)

  // Trigger debounce shift registers (0xfe = just went active/low) and the
  // per-mode state they drive. Unused ones fall out under -w.
  uint8_t  clk_state = 0xff, rst_state = 0xff, btn_state = 0xff;
  uint8_t  arp_step = 0;      // OSC_TRIG_ARP:    current step
  uint8_t  arp_mode = ARP_UP; // OSC_TRIG_ARP:    direction mode (button cycles)
  int8_t   arp_dir  = 1;      // OSC_TRIG_ARP:    ping-pong direction
  uint16_t arp_rng  = 0x2E31; // OSC_TRIG_ARP:    random/walk LCG state
  uint8_t  rotate   = 0;      // OSC_TRIG_ROTATE: voicing rotation (0..2)
  uint16_t pluck_env = 0;     // OSC_TRIG_PLUCK:  decaying envelope

  while (1)
  {
    adc.Scan();  // cycle the scanner so Read8() stays fresh

#ifndef OSC_DENSITY_AS_PITCH
    // ---- Option B (default): Y = root pitch, chaos = chord type, density knobs
    // = per-channel PWM, X = detune (movement). ------------------------------
    int8_t  root  = 24 + (int8_t)(((uint16_t)(255 - adc.Read8(ADC_CHANNEL_Y_CV)) * 48) >> 8);  // C1..C5
    uint8_t chord = (255 - adc.Read8(ADC_CHANNEL_RANDOMNESS_CV)) >> 5;   // 0..7 chord type
    uint8_t det   = 255 - adc.Read8(ADC_CHANNEL_X_CV);                   // detune amount
    // Two-phase detune: the sub-octaves spread across the whole knob throw, while
    // the mains only begin to spread past the halfway point (and more gently).
    // So the first half thickens the subs against the mains, and the top half
    // widens the whole six-voice stack.
    uint8_t sub_det  = det;
    uint8_t main_det = (det > 128) ? (uint8_t)(det - 128) : 0;
    // Chord 0 is unison: drop the sub-octave so all six voices land on the root
    // and only the detune spread separates them (a thick "superpulse").
    int8_t  sub_off = (chord == 0) ? 0 : -12;

    // Trigger inputs (edge-detected, active low): CLOCK (D8) = step, RESET (D2) =
    // reset, button (D3) = cycle arp mode (or step in rotate/pluck builds).
    clk_state = (uint8_t)(clk_state << 1) | (clock_input.Read()  ? 1 : 0);
    rst_state = (uint8_t)(rst_state << 1) | (reset_input.Read()  ? 1 : 0);
    btn_state = (uint8_t)(btn_state << 1) | (button_input.Read() ? 1 : 0);
    bool clock_pulse  = (clk_state == 0xfe);
    bool button_pulse = (btn_state == 0xfe);
    bool reset_pulse  = (rst_state == 0xfe);

#if defined(OSC_TRIG_ARP)
    // Arp: CLOCK steps through the chord tones across octaves (chord = the pool);
    // the button cycles the direction mode (shown on the LEDs); RESET restarts.
    // TEMPO sets the range (1..4 octaves = 3..12 steps).
    uint8_t num_steps = 3 * (1 + (adc.Read8(ADC_CHANNEL_TEMPO) >> 6));
    if (button_pulse) { if (++arp_mode >= ARP_NUM_MODES) arp_mode = 0; }
    if (clock_pulse)  arp_step = AdvanceArp(arp_step, num_steps, arp_mode, &arp_dir, &arp_rng);
    if (reset_pulse)  { arp_step = 0; arp_dir = 1; }
    if (arp_step >= num_steps) arp_step = (uint8_t)(num_steps - 1);   // range shrank
    leds.Write((uint8_t)(arp_mode + 1));   // binary mode readout 1..6
    int8_t arp_offset =
        (int8_t)pgm_read_byte(&kChord[chord][arp_step % 3]) + 12 * (arp_step / 3);
#elif defined(OSC_TRIG_ROTATE)
    if (clock_pulse || button_pulse) { if (++rotate >= 3) rotate = 0; }
    if (reset_pulse) { rotate = 0; }
#elif defined(OSC_TRIG_PLUCK)
    if (clock_pulse || button_pulse || reset_pulse) pluck_env = 0xFFFF;
    if (pluck_env) pluck_env -= (uint16_t)((pluck_env >> 8) + 1);   // decay (~85 ms)
#endif

    for (uint8_t v = 0; v < 3; ++v)
    {
#if defined(OSC_TRIG_ARP)
      int8_t note = root + arp_offset;                       // all mains on the arp note
#elif defined(OSC_TRIG_ROTATE)
      int8_t note = root + (int8_t)pgm_read_byte(&kChord[chord][(v + rotate) % 3]);
#else
      int8_t note = root + (int8_t)pgm_read_byte(&kChord[chord][v]);
#endif
      uint16_t inc_main = Detune(NoteTuningWord(note),           main_det, kDetuneSpread[v]);
      uint16_t inc_sub  = Detune(NoteTuningWord(note + sub_off), sub_det,  kDetuneSpread[v + 3]);
      uint16_t dv       = KnobToDuty(255 - adc.Read8(kDensityCh[v]));    // per-channel PWM
#if defined(OSC_TRIG_PLUCK)
      // Pluck narrows the pulse on attack, decaying back to the knob width.
      dv = 0x1F00 + (uint16_t)(((uint32_t)(dv - 0x1F00) * (uint8_t)(255 - (pluck_env >> 8))) >> 8);
#endif
      cli();
      increment[v]     = inc_main;   // voices 0/1/2
      increment[v + 3] = inc_sub;    // voices 3/4/5 = sub-octaves
      duty[v]          = dv;
      duty[v + 3]      = dv;         // sub shares its main's PWM width
      sei();
    }
#else
    // ---- Alt (OSC_DENSITY_AS_PITCH): density knobs = independent per-channel
    // pitch. TEMPO = transpose, X = global PWM, Y/chaos unused. ---------------
    int8_t transpose =
        (int8_t)(((uint16_t)adc.Read8(ADC_CHANNEL_TEMPO) * 48) >> 8) - 24;
    uint16_t gd = KnobToDuty(255 - adc.Read8(ADC_CHANNEL_X_CV));         // global PWM

    for (uint8_t v = 0; v < 3; ++v)
    {
      uint8_t  pot     = 255 - adc.Read8(kDensityCh[v]);                 // 4 octaves from C2
      int8_t   note    = 36 + (int8_t)(((uint16_t)pot * 48) >> 8);
      uint16_t inc_main = NoteTuningWord(note + transpose);
      uint16_t inc_sub  = NoteTuningWord(note + transpose - 12);
      cli();
      increment[v]     = inc_main;   // voices 0/1/2
      increment[v + 3] = inc_sub;    // voices 3/4/5 one octave down
      duty[v]          = gd;
      duty[v + 3]      = gd;         // sub shares the global PWM width
      sei();
    }
#endif
  }
#endif  // OSC_MIDI
}
