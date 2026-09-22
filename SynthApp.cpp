/*
  ======================================================================
  Module : DinMeter Synth Controller
  Version: v1.8.4
  Target : M5Stack Din Meter v1.1 + ByteButton + 8Angle + MIDI Unit U187
  ======================================================================

  Required libraries:
    - M5Unified
    - M5Unit-8Angle       (#include <M5_ANGLE8.h>)
    - M5Unit-ByteButton   (#include <unit_byte.hpp>)
    - M5-SAM2695          (#include <M5_SAM2695.h>)

  Also required in this sketch folder:
    - UsbMidi.h / UsbMidi.cpp and the other source files from
      esp32-usb-host-midi-library (Omocha), exactly as used in BringUp.

  Wiring:
    Din Meter PORT A -> Unit Hub -> ByteButton + 8Angle
    Din Meter PORT B -> MIDI Unit U187
    Din Meter USB-C  -> USB MIDI keyboard

  Important:
    ByteButton physical left->right = CH7,6,5,4,3,2,1,0.
    Software maps it to logical left->right = P1..P8.
*/

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_system.h>
#include "esp32-hal-tinyusb.h"

// Embedded in the compiled .bin so Web OTA can inspect the selected
// firmware version BEFORE any upload starts.
static const char DINMETER_FW_MARKER[] __attribute__((used)) =
  "DINMETER_FW_VERSION=v1.8.4";
#include <M5Unified.h>
#include <M5_ANGLE8.h>
#include <unit_byte.hpp>
#include <M5_SAM2695.h>
#include "UsbMidi.h"
#include "LegacyPresets.h"
#include "WifiMaintenance.h"

// ======================================================================
// Hardware
// ======================================================================

static constexpr int I2C_SDA = 13;
static constexpr int I2C_SCL = 15;
static constexpr uint8_t BYTE_ADDR = 0x47;

static constexpr int MIDI_RX = 1;
static constexpr int MIDI_TX = 2;

static constexpr int ENC_PIN_A   = 41;
static constexpr int ENC_PIN_B   = 40;
static constexpr int ENC_BTN_PIN = 42;

// If encoder direction is backwards, change +1 to -1.
static constexpr int ENCODER_DIRECTION = +1;
// Most mechanical encoders generate 4 valid quadrature transitions per detent.
static constexpr int ENC_TRANSITIONS_PER_DETENT = 4;

// If CONFIG/PERFORMANCE is backwards, change true -> false.
static constexpr bool CONFIG_SWITCH_ACTIVE_LEVEL = true;

// If any 8Angle knob runs backwards, set that item true.
static constexpr bool ANGLE_REVERSE[8] = {
  true, true, true, true, true, true, true, true
};

HardwareSerial SynthSerial(1);
M5_SAM2695 synth;
M5_ANGLE8 angle8;
UnitByte byteButton;
UsbMidi usbMidi;
Preferences prefs;

// Maintenance persistence:
// - entering MAINTENANCE sets an NVS flag
// - OTA / ESP.restart keeps the flag, so the next boot returns to maintenance
// - a real power-on/brownout clears the flag, so power cycling exits maintenance
static constexpr const char* MAINT_STICKY_KEY = "maint_sticky";
static constexpr const char* MAINT_MIGRATED_KEY = "maint_migr";
bool resumeMaintenanceOnBoot = false;

// ======================================================================
// MIDI channels = pseudo 3 OSC
// ======================================================================

static constexpr uint8_t OSC_CH[3] = {0, 1, 2};

// ======================================================================
// UI colors (RGB565)
// Black + amber/yellow industrial warning style
// ======================================================================

static constexpr uint16_t C_BLACK  = 0x0000;
static constexpr uint16_t C_AMBER  = 0xFD20;
static constexpr uint16_t C_YELLOW = 0xFFE0;
static constexpr uint16_t C_ORANGE = 0xFBE0;
static constexpr uint16_t C_RED    = 0xF800;
static constexpr uint16_t C_GREEN  = 0x07E0;
static constexpr uint16_t C_BLUE   = 0x001F;
static constexpr uint16_t C_WHITE  = 0xFFFF;
static constexpr uint16_t C_GREY   = 0x8410;
static constexpr uint16_t C_DARK   = 0x2104;

// ======================================================================
// Preset data
// ======================================================================

struct OscState {
  uint8_t bank;
  uint8_t program;
  uint8_t level;       // 0..127
  int8_t  transpose;   // semitones
  int8_t  detune;      // approx cents: -50..+50
  int8_t  cutoffTrim;  // -63..+63
  int8_t  resonanceTrim;
  uint8_t pan;         // 0..127
};

struct ModState {
  uint8_t mwPitch;
  uint8_t mwFilter;
  uint8_t mwAmp;
  uint8_t rate;
  uint8_t pitchDepth;
  uint8_t filterDepth;
  uint8_t ampDepth;
  uint8_t configured;  // 0 = keep SAM default modulation mapping
};

struct Preset {
  char name[32];

  OscState osc[3];

  uint8_t cutoff;
  uint8_t resonance;
  uint8_t attack;
  uint8_t decay;
  uint8_t release;

  uint8_t reverb;
  uint8_t vibratoRate;
  uint8_t vibratoDepth;
  uint8_t vibratoDelay;
  uint8_t glideTime;

  uint8_t mono;
  uint8_t glide;
  uint8_t legato;
  uint8_t soft;
  uint8_t reverbEnabled;
  uint8_t vibratoEnabled;

  ModState mod;

  // Kept from the previous 60-preset engine so old sounds stay close.
  uint8_t legacyChorusProgram;
  uint8_t legacyChorusSend;
  uint8_t legacySpatialVolume;
  uint8_t legacySpatialDelay;
};

static constexpr uint8_t BANK_COUNT = 8;
static constexpr uint8_t PRESETS_PER_BANK = 8;
static constexpr uint8_t PRESET_COUNT = BANK_COUNT * PRESETS_PER_BANK;

Preset currentPreset;

uint8_t browseBank  = 0;  // encoder selects this bank
uint8_t loadedBank  = 0;  // actually sounding bank
uint8_t loadedSlot  = 0;  // 0..7
bool modified       = false;

uint8_t masterVolume = 100;

// Runtime-only states
bool keyboardSustain = false;
bool forceSustain    = false;
bool recoveringFromPanic = false;

// ======================================================================
// Wave/material table
// These are the already researched/tested SAM2695 materials.
// Program values are the same internal values used by the prior firmware.
// ======================================================================

struct WaveMaterial {
  const char* label;
  uint8_t bank;
  uint8_t program;
};

static const WaveMaterial WAVE_TABLE[8] = {
  {"SQR",  127, 47},
  {"SAW",  127, 44},
  {"OCAR",   0, 79},
  {"WHST",   0, 78},
  {"FLUT",   0, 73},
  {"REC",    0, 74},
  {"CLAR",   0, 71},
  {"HARM",   0, 31},
};

// ======================================================================
// CONFIG pages
// ======================================================================

enum ConfigPage : uint8_t {
  PAGE_PERF = 0,
  PAGE_FILTER_ENV,
  PAGE_OSC1,
  PAGE_OSC2,
  PAGE_OSC3,
  PAGE_MOD,
  PAGE_COUNT
};

uint8_t configPage = PAGE_PERF;
bool configMode = false;

static const char* PAGE_NAMES[PAGE_COUNT] = {
  "PERFORMANCE", "FILTER / ENV", "OSC 1", "OSC 2", "OSC 3", "MOD"
};

static const char* PAGE_TAB[PAGE_COUNT] = {
  "PERF", "F/ENV", "OSC1", "OSC2", "OSC3", "MOD"
};

static const char* PERF_LABELS[8] = {
  "VOL", "CUT", "RES", "ATK", "REL", "VIB", "PRT", "GLD"
};

static const char* FILTER_LABELS[8] = {
  "CUT", "RES", "ATK", "DEC", "REL", "REV", "VR", "VD"
};

static const char* OSC_LABELS[8] = {
  "WAV", "LVL", "OCT", "DET", "CUT", "RES", "PAN", "---"
};

static const char* MOD_LABELS[8] = {
  "MWP", "MWF", "MWA", "RATE", "PDP", "FDP", "ADP", "VDL"
};

static const char* CONFIG_BUTTON_LABELS[8] = {
  "MON", "GLD", "LEG", "SUS", "SFT", "REV", "VIB", "PAN"
};

// ======================================================================
// Active notes / mono stack
// ======================================================================

bool heldInput[128] = {};
uint8_t heldVelocity[128] = {};
uint8_t noteOrder[128] = {};
uint8_t heldCount = 0;
int16_t currentMonoNote = -1;

// ======================================================================
// Encoder
// ======================================================================

volatile int32_t encoderTransitions = 0;
volatile uint8_t encoderLastEncoded = 0;
int encoderRemainder = 0;

void IRAM_ATTR onEncoderChange() {
  uint8_t msb = digitalRead(ENC_PIN_A);
  uint8_t lsb = digitalRead(ENC_PIN_B);
  uint8_t encoded = (msb << 1) | lsb;
  uint8_t sum = (encoderLastEncoded << 2) | encoded;

  if (sum == 0b1101 || sum == 0b0100 || sum == 0b0010 || sum == 0b1011) {
    encoderTransitions++;
  } else if (sum == 0b1110 || sum == 0b0111 || sum == 0b0001 || sum == 0b1000) {
    encoderTransitions--;
  }
  encoderLastEncoded = encoded;
}

// Encoder button state
bool encBtnRaw = false;
bool encBtnStable = false;
bool encBtnLongFired = false;
uint32_t encBtnChangedAt = 0;
uint32_t encBtnPressedAt = 0;
static constexpr uint32_t ENC_DEBOUNCE_MS = 25;
static constexpr uint32_t ENC_LONG_MS = 750;

// ======================================================================
// 8Angle pickup
// ======================================================================

struct PickupState {
  bool active;
  uint8_t target;
  uint8_t lastValue;
};

PickupState pickup[8];

// Last value actually accepted as a control change.
// This prevents 1-count ADC jitter around a boundary from repeatedly
// redrawing the screen / re-sending MIDI parameters.
uint8_t lastAcceptedAngleValue[8] = {};
static constexpr uint8_t PICKUP_TOLERANCE = 3;
static constexpr uint8_t ANGLE_VALUE_DEADBAND = 1;

// ======================================================================
// ByteButton debounce
// ======================================================================

bool byteRaw[8] = {};
bool byteStable[8] = {};
uint32_t byteChangedAt[8] = {};
static constexpr uint32_t BYTE_DEBOUNCE_MS = 20;

// ======================================================================
// UI transient state
// ======================================================================

bool screenDirty = true;

bool overlayActive = false;
uint32_t overlayUntil = 0;
char overlayTitle[24] = "";
char overlaySub[24] = "";

bool paramPopupActive = false;
uint32_t paramPopupUntil = 0;
char paramPopupName[16] = "";
char paramPopupValue[20] = "";

bool saveDialog = false;
bool saveChoiceYes = false;

// Maintenance screen redraw cache.
// v1.7 redrew the whole screen every 250 ms, causing visible flicker.
WifiMaintMode lastMaintUiMode = WifiMaintMode::OFF;
String lastMaintUiStatus = "";
String lastMaintUiIp = "";
int lastMaintUiProgress = -1;
bool lastMaintUiUpdating = false;

bool systemMenuActive = false;
uint8_t systemMenuIndex = 0;

bool maintenanceConfirm = false;
bool maintenanceChoiceYes = false;

// v1.7.9 experimental wired flashing:
// ask the Arduino-ESP32 core to restart the ESP32-S3 into the ROM
// USB download bootloader, equivalent in purpose to the rear BOOT-button path.
bool usbFlashConfirm = false;
bool usbFlashChoiceYes = false;

bool systemInfoActive = false;

static constexpr uint8_t SYSTEM_MENU_COUNT = 6;
static const char* SYSTEM_MENU_ITEMS[SYSTEM_MENU_COUNT] = {
  "SAVE PRESET",
  "MAINTENANCE",
  "WIFI SETUP",
  "USB FLASH",
  "SYSTEM INFO",
  "EXIT"
};

// ======================================================================
// Timers
// ======================================================================

uint32_t lastAnglePoll = 0;
uint32_t lastBytePoll = 0;
uint32_t lastUiDraw = 0;

static constexpr uint32_t ANGLE_POLL_MS = 20;
static constexpr uint32_t BYTE_POLL_MS = 12;
static constexpr uint32_t UI_REFRESH_MIN_MS = 30;

// ======================================================================
// MIDI helpers
// ======================================================================

void sendMidi1(uint8_t b1) {
  SynthSerial.write(b1);
}

void sendMidi2(uint8_t b1, uint8_t b2) {
  SynthSerial.write(b1);
  SynthSerial.write(b2);
}

void sendMidi3(uint8_t b1, uint8_t b2, uint8_t b3) {
  SynthSerial.write(b1);
  SynthSerial.write(b2);
  SynthSerial.write(b3);
}

void sendCC(uint8_t ch, uint8_t cc, uint8_t value) {
  sendMidi3(0xB0 | (ch & 0x0F), cc & 0x7F, value & 0x7F);
}

void sendProgram(uint8_t ch, uint8_t program) {
  sendMidi2(0xC0 | (ch & 0x0F), program & 0x7F);
}

void sendPitchBendRaw(uint8_t ch, uint8_t lsb, uint8_t msb) {
  sendMidi3(0xE0 | (ch & 0x0F), lsb & 0x7F, msb & 0x7F);
}

void sendNRPN(uint8_t ch, uint8_t msb, uint8_t lsb, uint8_t value) {
  sendCC(ch, 99, msb);
  sendCC(ch, 98, lsb);
  sendCC(ch, 6, value & 0x7F);
  sendCC(ch, 99, 127);
  sendCC(ch, 98, 127);
}

void setPitchBendRange2(uint8_t ch) {
  sendCC(ch, 101, 0);
  sendCC(ch, 100, 0);
  sendCC(ch, 6, 2);
  sendCC(ch, 38, 0);
  sendCC(ch, 101, 127);
  sendCC(ch, 100, 127);
}

// Previous firmware's spatial/chorus routing.
void configureLegacyEffects(bool chorusOn, bool spatialOn) {
  uint8_t mask = 0;
  if (chorusOn)  mask |= 0x10;
  if (spatialOn) mask |= 0x08;
  sendNRPN(OSC_CH[0], 0x37, 0x5F, mask);
}

void setLegacySpatial(uint8_t volume, uint8_t delayValue) {
  sendNRPN(OSC_CH[0], 0x37, 0x18, 127);
  sendNRPN(OSC_CH[0], 0x37, 0x20, volume);
  sendNRPN(OSC_CH[0], 0x37, 0x2C, delayValue);
  sendNRPN(OSC_CH[0], 0x37, 0x2D, 127);
}

// ======================================================================
// General helpers
// ======================================================================

uint8_t clamp127(int v) {
  return (uint8_t)constrain(v, 0, 127);
}

uint8_t oscEffectiveCutoff(uint8_t oscIndex) {
  return clamp127((int)currentPreset.cutoff + currentPreset.osc[oscIndex].cutoffTrim);
}

uint8_t oscEffectiveRes(uint8_t oscIndex) {
  return clamp127((int)currentPreset.resonance + currentPreset.osc[oscIndex].resonanceTrim);
}

uint8_t currentPresetIndex() {
  return loadedBank * PRESETS_PER_BANK + loadedSlot;
}

uint8_t browsePresetIndex(uint8_t logicalSlot) {
  return browseBank * PRESETS_PER_BANK + logicalSlot;
}

uint8_t logicalToPhysicalButton(uint8_t logical) {
  return 7 - logical;
}

bool oscEnabled(uint8_t osc) {
  return currentPreset.osc[osc].level > 0;
}

uint8_t scaledOscLevel(uint8_t v) {
  return v;
}

uint8_t fineTuneValueFromCents(int cents) {
  cents = constrain(cents, -50, 50);
  // MIDI Fine Tuning neutral = 64. Approx +/-100 cents across full range.
  return clamp127(64 + (cents * 64) / 100);
}

// ======================================================================
// Default preset conversion / NVS
// ======================================================================

Preset makeInitPreset(uint8_t index) {
  Preset p{};
  snprintf(p.name, sizeof(p.name), "INIT %02u", index);

  p.osc[0] = {127, 44, 100, 0, 0, 0, 0, 64};
  p.osc[1] = {127, 47,  40, 0, 4, 0, 0, 64};
  p.osc[2] = {0,   79,   0, 0, 0, 0, 0, 64};

  p.cutoff = 64;
  p.resonance = 64;
  p.attack = 64;
  p.decay = 64;
  p.release = 64;

  p.reverb = 0;
  p.vibratoRate = 64;
  p.vibratoDepth = 0;
  p.vibratoDelay = 0;
  p.glideTime = 40;

  p.mono = 0;
  p.glide = 0;
  p.legato = 1;
  p.soft = 0;
  p.reverbEnabled = 0;
  p.vibratoEnabled = 0;

  p.mod = {0,0,0,0,0,0,0,0};

  p.legacyChorusProgram = 2;
  p.legacyChorusSend = 0;
  p.legacySpatialVolume = 0;
  p.legacySpatialDelay = 29;
  return p;
}

Preset makeDefaultPreset(uint8_t index) {
  if (index >= LEGACY_PATCH_COUNT) {
    return makeInitPreset(index);
  }

  const LegacyPatchDef& l = LEGACY_PATCHES[index];
  Preset p{};
  strncpy(p.name, l.name, sizeof(p.name) - 1);

  const LegacyOscDef lo[3] = {l.osc1, l.osc2, l.osc3};
  for (uint8_t i = 0; i < 3; ++i) {
    p.osc[i].bank = lo[i].bank;
    p.osc[i].program = lo[i].program;
    p.osc[i].level = clamp127((lo[i].volume * 127) / 100);
    p.osc[i].transpose = lo[i].transpose;
    p.osc[i].detune = lo[i].detune;
    p.osc[i].cutoffTrim = 0;
    p.osc[i].resonanceTrim = 0;
    p.osc[i].pan = 64;
  }

  p.cutoff = l.cutoff;
  p.resonance = l.resonance;
  p.attack = l.attack;
  p.decay = l.decay;
  p.release = l.release;

  p.reverb = 0;
  p.vibratoRate = 64;
  p.vibratoDepth = 0;
  p.vibratoDelay = 0;
  p.glideTime = 40;

  p.mono = 0;
  p.glide = 0;
  p.legato = 1;
  p.soft = 0;
  p.reverbEnabled = 0;
  p.vibratoEnabled = 0;

  p.mod = {0,0,0,0,0,0,0,0};

  p.legacyChorusProgram = l.chorusProgram;
  p.legacyChorusSend = l.chorusSend;
  p.legacySpatialVolume = l.spatialVolume;
  p.legacySpatialDelay = l.spatialDelay;
  return p;
}

void presetKey(uint8_t index, char* out, size_t outSize) {
  snprintf(out, outSize, "p%02u", index);
}

void loadPresetData(uint8_t index, Preset& out) {
  out = makeDefaultPreset(index);

  char key[8];
  presetKey(index, key, sizeof(key));

  size_t len = prefs.getBytesLength(key);
  if (len == sizeof(Preset)) {
    prefs.getBytes(key, &out, sizeof(Preset));
  }
}

void saveCurrentPreset() {
  char key[8];
  presetKey(currentPresetIndex(), key, sizeof(key));
  prefs.putBytes(key, &currentPreset, sizeof(Preset));
  modified = false;
  screenDirty = true;
}

void persistLocation() {
  prefs.putUChar("bank", loadedBank);
  prefs.putUChar("slot", loadedSlot);
}

// ======================================================================
// Synth parameter application
// ======================================================================

void applyFilterAll() {
  for (uint8_t i = 0; i < 3; ++i) {
    synth.setTvf(OSC_CH[i], oscEffectiveCutoff(i), oscEffectiveRes(i));
  }
}

void applyEnvelopeAll() {
  for (uint8_t i = 0; i < 3; ++i) {
    synth.setEnvelope(OSC_CH[i], currentPreset.attack, currentPreset.decay, currentPreset.release);
  }
}

void applyReverbAll() {
  uint8_t level = currentPreset.reverbEnabled ? currentPreset.reverb : 0;
  for (uint8_t i = 0; i < 3; ++i) {
    synth.setReverb(OSC_CH[i], 2, level, 0);
  }
}

void applyVibratoAll() {
  uint8_t depth = currentPreset.vibratoEnabled ? currentPreset.vibratoDepth : 0;
  for (uint8_t i = 0; i < 3; ++i) {
    synth.setVibrate(OSC_CH[i],
                     currentPreset.vibratoRate,
                     depth,
                     currentPreset.vibratoDelay);
  }
}

void applyModAll() {
  if (!currentPreset.mod.configured) return;

  for (uint8_t i = 0; i < 3; ++i) {
    synth.setModWheel(OSC_CH[i],
                      currentPreset.mod.mwPitch,
                      currentPreset.mod.mwFilter,
                      currentPreset.mod.mwAmp,
                      currentPreset.mod.rate,
                      currentPreset.mod.pitchDepth,
                      currentPreset.mod.filterDepth,
                      currentPreset.mod.ampDepth);
  }
}

bool effectiveSustain() {
  return keyboardSustain || forceSustain;
}

void applyPedalsAll() {
  uint8_t sustainValue = effectiveSustain() ? 127 : 0;
  uint8_t softValue = currentPreset.soft ? 127 : 0;
  for (uint8_t i = 0; i < 3; ++i) {
    sendCC(OSC_CH[i], 64, sustainValue);
    sendCC(OSC_CH[i], 67, softValue);
  }
}

void applyModeAndGlideAll() {
  // CC126 = Mono Mode On, CC127 = Poly Mode On
  for (uint8_t i = 0; i < 3; ++i) {
    if (currentPreset.mono) {
      sendCC(OSC_CH[i], 126, 1);
    } else {
      sendCC(OSC_CH[i], 127, 0);
    }

    sendCC(OSC_CH[i], 5, currentPreset.glideTime);
    sendCC(OSC_CH[i], 65, currentPreset.glide ? 127 : 0);
  }
}

void applyOscStatic(uint8_t oscIndex) {
  OscState& o = currentPreset.osc[oscIndex];
  uint8_t ch = OSC_CH[oscIndex];

  sendCC(ch, 0, o.bank);
  sendCC(ch, 32, 0);
  sendProgram(ch, o.program);
  delay(2);

  setPitchBendRange2(ch);
  synth.setTuning(ch, fineTuneValueFromCents(o.detune), 64);
  synth.setPan(ch, o.pan);
  synth.setVolume(ch, scaledOscLevel(o.level));

  // Hidden legacy chorus retained to preserve old patch character.
  sendCC(ch, 81, currentPreset.legacyChorusProgram);
  sendCC(ch, 93, currentPreset.legacyChorusSend);
}

void silenceSynthOnly() {
  for (uint8_t i = 0; i < 3; ++i) {
    sendCC(OSC_CH[i], 64, 0);
    sendCC(OSC_CH[i], 123, 0);
    sendCC(OSC_CH[i], 120, 0);
  }
  currentMonoNote = -1;
}

void clearHeldState() {
  memset(heldInput, 0, sizeof(heldInput));
  memset(heldVelocity, 0, sizeof(heldVelocity));
  heldCount = 0;
  currentMonoNote = -1;
}

void pushNoteOrder(uint8_t note) {
  for (uint8_t i = 0; i < heldCount; ++i) {
    if (noteOrder[i] == note) {
      for (uint8_t j = i; j + 1 < heldCount; ++j) {
        noteOrder[j] = noteOrder[j + 1];
      }
      heldCount--;
      break;
    }
  }
  if (heldCount < 128) {
    noteOrder[heldCount++] = note;
  }
}

void removeNoteOrder(uint8_t note) {
  for (uint8_t i = 0; i < heldCount; ++i) {
    if (noteOrder[i] == note) {
      for (uint8_t j = i; j + 1 < heldCount; ++j) {
        noteOrder[j] = noteOrder[j + 1];
      }
      heldCount--;
      return;
    }
  }
}

int16_t lastHeldNote() {
  if (heldCount == 0) return -1;
  return noteOrder[heldCount - 1];
}

void sendOscNote(uint8_t oscIndex, uint8_t messageType, uint8_t inputNote, uint8_t velocity) {
  const OscState& o = currentPreset.osc[oscIndex];
  if (o.level == 0) return;

  int note = (int)inputNote + o.transpose;
  if (note < 0 || note > 127) return;

  sendMidi3(messageType | (OSC_CH[oscIndex] & 0x0F),
            note & 0x7F,
            velocity & 0x7F);
}

void sendLayeredNote(uint8_t messageType, uint8_t inputNote, uint8_t velocity) {
  for (uint8_t i = 0; i < 3; ++i) {
    sendOscNote(i, messageType, inputNote, velocity);
  }
}

void rebuildHeldNotes() {
  currentMonoNote = -1;

  if (currentPreset.mono) {
    int16_t n = lastHeldNote();
    if (n >= 0) {
      sendLayeredNote(0x90, (uint8_t)n, heldVelocity[n]);
      currentMonoNote = n;
    }
    return;
  }

  for (uint8_t i = 0; i < heldCount; ++i) {
    uint8_t n = noteOrder[i];
    if (heldInput[n]) {
      sendLayeredNote(0x90, n, heldVelocity[n]);
    }
  }
}

void applyCurrentPresetToSynth(bool rebuildNotes = true) {
  silenceSynthOnly();

  configureLegacyEffects(currentPreset.legacyChorusSend > 0,
                         currentPreset.legacySpatialVolume > 0);

  if (currentPreset.legacySpatialVolume > 0) {
    setLegacySpatial(currentPreset.legacySpatialVolume,
                     currentPreset.legacySpatialDelay);
  } else {
    setLegacySpatial(0, 29);
  }

  for (uint8_t i = 0; i < 3; ++i) {
    applyOscStatic(i);
  }

  synth.setMasterVolume(masterVolume);

  applyFilterAll();
  applyEnvelopeAll();
  applyReverbAll();
  applyVibratoAll();
  applyModAll();
  applyModeAndGlideAll();
  applyPedalsAll();

  if (rebuildNotes) rebuildHeldNotes();
}

// ======================================================================
// PANIC / note safety
// ======================================================================

void panicAll(const char* reason) {
  silenceSynthOnly();
  clearHeldState();
  keyboardSustain = false;
  forceSustain = false;
  recoveringFromPanic = true;

  snprintf(overlayTitle, sizeof(overlayTitle), "PANIC");
  snprintf(overlaySub, sizeof(overlaySub), "%s", reason ? reason : "RESET");
  overlayActive = true;
  overlayUntil = millis() + 800;
  screenDirty = true;
}

void handleNoteOn(uint8_t note, uint8_t velocity) {
  if (recoveringFromPanic) {
    clearHeldState();
    recoveringFromPanic = false;
  }

  if (heldInput[note]) {
    heldVelocity[note] = velocity;
    return;
  }

  heldInput[note] = true;
  heldVelocity[note] = velocity;
  pushNoteOrder(note);

  if (!currentPreset.mono) {
    sendLayeredNote(0x90, note, velocity);
    return;
  }

  if (currentMonoNote < 0) {
    sendLayeredNote(0x90, note, velocity);
    currentMonoNote = note;
    return;
  }

  uint8_t oldNote = (uint8_t)currentMonoNote;

  if (currentPreset.legato) {
    // New NoteOn first: lets mono synth/portamento behavior do the transition.
    sendLayeredNote(0x90, note, velocity);
    sendLayeredNote(0x80, oldNote, 0);
  } else {
    sendLayeredNote(0x80, oldNote, 0);
    sendLayeredNote(0x90, note, velocity);
  }

  currentMonoNote = note;
}

void handleNoteOff(uint8_t note, uint8_t velocity) {
  if (!heldInput[note]) {
    if (recoveringFromPanic) return;

    // This is the octave-change/stuck-note protection.
    panicAll("NOTE DESYNC");
    return;
  }

  heldInput[note] = false;
  heldVelocity[note] = 0;
  removeNoteOrder(note);

  if (!currentPreset.mono) {
    sendLayeredNote(0x80, note, velocity);
    return;
  }

  if (currentMonoNote != note) {
    return;
  }

  int16_t next = lastHeldNote();
  if (next < 0) {
    sendLayeredNote(0x80, note, velocity);
    currentMonoNote = -1;
    return;
  }

  if (currentPreset.legato) {
    sendLayeredNote(0x90, (uint8_t)next, heldVelocity[next]);
    sendLayeredNote(0x80, note, 0);
  } else {
    sendLayeredNote(0x80, note, 0);
    sendLayeredNote(0x90, (uint8_t)next, heldVelocity[next]);
  }

  currentMonoNote = next;
}

// ======================================================================
// MIDI input routing
// ======================================================================

int midiDataLengthFromCin(uint8_t cin) {
  switch (cin) {
    case 0x2: return 2;
    case 0x3: return 3;
    case 0x4: return 3;
    case 0x5: return 1;
    case 0x6: return 2;
    case 0x7: return 3;
    case 0x8: return 3;
    case 0x9: return 3;
    case 0xA: return 3;
    case 0xB: return 3;
    case 0xC: return 2;
    case 0xD: return 2;
    case 0xE: return 3;
    case 0xF: return 1;
    default:  return 0;
  }
}

void forwardSystemPacket(const uint8_t (&packet)[4]) {
  int len = midiDataLengthFromCin(packet[0] & 0x0F);
  for (int i = 1; i <= len; ++i) {
    SynthSerial.write(packet[i]);
  }
}

void forwardCCToOscs(uint8_t cc, uint8_t value) {
  for (uint8_t i = 0; i < 3; ++i) {
    sendCC(OSC_CH[i], cc, value);
  }
}

void onMidiMessage(const uint8_t (&packet)[4]) {
  uint8_t cin = packet[0] & 0x0F;

  if (cin == 0x8) {
    handleNoteOff(packet[2] & 0x7F, packet[3] & 0x7F);
    return;
  }

  if (cin == 0x9) {
    uint8_t note = packet[2] & 0x7F;
    uint8_t vel  = packet[3] & 0x7F;
    if (vel == 0) handleNoteOff(note, 0);
    else          handleNoteOn(note, vel);
    return;
  }

  if (cin == 0xA) {
    for (uint8_t i = 0; i < 3; ++i) {
      if (oscEnabled(i)) {
        int shifted = (int)(packet[2] & 0x7F) + currentPreset.osc[i].transpose;
        if (shifted >= 0 && shifted <= 127) {
          sendMidi3(0xA0 | OSC_CH[i], shifted, packet[3] & 0x7F);
        }
      }
    }
    return;
  }

  if (cin == 0xB) {
    uint8_t cc = packet[2] & 0x7F;
    uint8_t value = packet[3] & 0x7F;

    if (cc == 64) {
      keyboardSustain = (value >= 64);
      applyPedalsAll();
      return;
    }

    // Keyboard Program/Bank/volume/our internal FX/glide settings are blocked.
    if (cc == 0 || cc == 32 || cc == 7 || cc == 5 || cc == 65 ||
        cc == 81 || cc == 91 || cc == 93 || cc == 126 || cc == 127) {
      return;
    }

    // Mod wheel, expression, etc.
    forwardCCToOscs(cc, value);
    return;
  }

  // Program Change from keyboard is intentionally ignored.
  if (cin == 0xC) return;

  if (cin == 0xD) {
    for (uint8_t i = 0; i < 3; ++i) {
      if (oscEnabled(i)) sendMidi2(0xD0 | OSC_CH[i], packet[2] & 0x7F);
    }
    return;
  }

  if (cin == 0xE) {
    for (uint8_t i = 0; i < 3; ++i) {
      sendPitchBendRaw(OSC_CH[i], packet[2], packet[3]);
    }
    return;
  }

  forwardSystemPacket(packet);
}

void onMidiDeviceConnected() {
  snprintf(overlayTitle, sizeof(overlayTitle), "USB MIDI");
  snprintf(overlaySub, sizeof(overlaySub), "CONNECTED");
  overlayActive = true;
  overlayUntil = millis() + 700;
  screenDirty = true;
}

void onMidiDeviceDisconnected() {
  panicAll("USB LOST");
}

void armPickupForCurrentContext();
void updateByteLeds();
void openSaveDialog();
void drawHazardStripe(int y, int h);
void clearScreen();
void resetMaintenanceUiCache();
void enterUsbFlashBootloader();
void drawUsbFlashConfirm();

// Forward declarations needed in a normal .cpp file.
// Arduino auto-prototypes do not apply here.
uint8_t rawTo127(uint16_t raw, uint8_t knob);
void setParamPopup(const char* name, const char* value);
void formatParamValue(uint8_t page, uint8_t knob, char* out, size_t n);

// ======================================================================
// Preset load
// ======================================================================

void loadPreset(uint8_t bank, uint8_t slot) {
  if (bank >= BANK_COUNT || slot >= PRESETS_PER_BANK) return;

  loadedBank = bank;
  loadedSlot = slot;
  browseBank = bank;

  loadPresetData(currentPresetIndex(), currentPreset);

  // PERFORMANCE physical Portamento knob is authoritative.
  if (!configMode) {
    uint16_t portRaw = angle8.getAnalogInput(6, _12bit);
    uint8_t portV = rawTo127(portRaw, 6);
    bool physicalOn = (portV >= 64);
    currentPreset.glide = physicalOn ? 1 : 0;
    currentPreset.mono  = physicalOn ? 1 : 0;
  }

  modified = false;
  persistLocation();

  applyCurrentPresetToSynth(true);

  // Pickup must be re-armed after preset values changed.
  screenDirty = true;
}

// ======================================================================
// Wave helpers
// ======================================================================

uint8_t waveIndexForOsc(uint8_t oscIndex) {
  const OscState& o = currentPreset.osc[oscIndex];
  for (uint8_t i = 0; i < 8; ++i) {
    if (WAVE_TABLE[i].bank == o.bank && WAVE_TABLE[i].program == o.program) return i;
  }
  return 0;
}

// ======================================================================
// Parameter normalization for pickup / screen
// ======================================================================

uint8_t mapSignedTo127(int value, int minV, int maxV) {
  value = constrain(value, minV, maxV);
  return (uint8_t)map(value, minV, maxV, 0, 127);
}

int map127ToSigned(uint8_t value, int minV, int maxV) {
  return map(value, 0, 127, minV, maxV);
}

uint8_t currentParamAs127(uint8_t knob) {
  uint8_t page = configMode ? configPage : PAGE_PERF;

  if (page == PAGE_PERF) {
    switch (knob) {
      case 0: return masterVolume;
      case 1: return currentPreset.cutoff;
      case 2: return currentPreset.resonance;
      case 3: return currentPreset.attack;
      case 4: return currentPreset.release;
      case 5: return currentPreset.vibratoDepth;
      case 6: return currentPreset.glide ? 127 : 0;
      case 7: return currentPreset.glideTime;
    }
  }

  if (page == PAGE_FILTER_ENV) {
    switch (knob) {
      case 0: return currentPreset.cutoff;
      case 1: return currentPreset.resonance;
      case 2: return currentPreset.attack;
      case 3: return currentPreset.decay;
      case 4: return currentPreset.release;
      case 5: return currentPreset.reverb;
      case 6: return currentPreset.vibratoRate;
      case 7: return currentPreset.vibratoDepth;
    }
  }

  if (page >= PAGE_OSC1 && page <= PAGE_OSC3) {
    uint8_t oi = page - PAGE_OSC1;
    const OscState& o = currentPreset.osc[oi];
    switch (knob) {
      case 0: return (uint8_t)((waveIndexForOsc(oi) * 127) / 7);
      case 1: return o.level;
      case 2: return mapSignedTo127(o.transpose, -24, 24);
      case 3: return mapSignedTo127(o.detune, -50, 50);
      case 4: return mapSignedTo127(o.cutoffTrim, -63, 63);
      case 5: return mapSignedTo127(o.resonanceTrim, -63, 63);
      case 6: return o.pan;
      case 7: return 0;
    }
  }

  if (page == PAGE_MOD) {
    switch (knob) {
      case 0: return currentPreset.mod.mwPitch;
      case 1: return currentPreset.mod.mwFilter;
      case 2: return currentPreset.mod.mwAmp;
      case 3: return currentPreset.mod.rate;
      case 4: return currentPreset.mod.pitchDepth;
      case 5: return currentPreset.mod.filterDepth;
      case 6: return currentPreset.mod.ampDepth;
      case 7: return currentPreset.vibratoDelay;
    }
  }

  return 0;
}

bool knobIsReserved(uint8_t knob) {
  uint8_t page = configMode ? configPage : PAGE_PERF;
  return (page >= PAGE_OSC1 && page <= PAGE_OSC3 && knob == 7);
}

// ======================================================================
// 8Angle LED / pickup
// ======================================================================

uint8_t rawTo127(uint16_t raw, uint8_t knob) {
  raw = constrain(raw, 0, 4095);
  uint16_t v = ANGLE_REVERSE[knob] ? (4095 - raw) : raw;
  return (uint8_t)((v * 127UL + 2047) / 4095);
}

void setAngleLed(uint8_t knob, uint32_t rgb, uint8_t brightness = 35) {
  angle8.setLEDColor(knob, rgb, brightness);
}

void updatePickupLed(uint8_t knob) {
  if (knobIsReserved(knob)) {
    setAngleLed(knob, 0x000000, 0);
    return;
  }

  bool perfContext = ((!configMode) || configPage == PAGE_PERF);
  if (perfContext && knob == 6) {
    // Physical Portamento status: green=ON, dim red=OFF.
    setAngleLed(knob, currentPreset.glide ? 0x00FF00 : 0x501000,
                currentPreset.glide ? 34 : 22);
    return;
  }

  if (pickup[knob].active) {
    setAngleLed(knob, 0x00FF00, 24);  // green
    return;
  }

  if (pickup[knob].lastValue > pickup[knob].target) {
    setAngleLed(knob, 0xFF2000, 34);  // lower it
  } else {
    setAngleLed(knob, 0x0060FF, 34);  // raise it
  }
}

void armPickupForCurrentContext() {
  for (uint8_t i = 0; i < 8; ++i) {
    uint16_t raw = angle8.getAnalogInput(i, _12bit);
    uint8_t now = rawTo127(raw, i);

    pickup[i].lastValue = now;
    pickup[i].target = currentParamAs127(i);
    lastAcceptedAngleValue[i] = now;

    // VOL and physical Portamento switch are always immediate
    // in PERFORMANCE/PAGE_PERF context.
    bool perfContext = ((!configMode) || configPage == PAGE_PERF);
    pickup[i].active = (perfContext && (i == 0 || i == 6));

    if (!pickup[i].active && abs((int)now - (int)pickup[i].target) <= PICKUP_TOLERANCE) {
      pickup[i].active = true;
    }

    updatePickupLed(i);
  }
}

// ======================================================================
// PERFORMANCE physical Portamento switch (8Angle knob #7 / index 6)
// Left side = OFF, right side = ON.
// Hysteresis avoids rapid ON/OFF around the middle.
// ======================================================================

static constexpr uint8_t PORTAMENTO_ON_THRESHOLD  = 72;
static constexpr uint8_t PORTAMENTO_OFF_THRESHOLD = 55;

bool physicalPortamentoState(uint8_t v, bool currentState) {
  if (!currentState && v >= PORTAMENTO_ON_THRESHOLD) return true;
  if ( currentState && v <= PORTAMENTO_OFF_THRESHOLD) return false;
  return currentState;
}

void setPortamentoFromPhysical(uint8_t v, bool showPopup) {
  bool next = physicalPortamentoState(v, currentPreset.glide != 0);
  if (next == (currentPreset.glide != 0)) return;

  silenceSynthOnly();

  currentPreset.glide = next ? 1 : 0;

  // PERFORMANCE physical Portamento is a paired mode selector:
  // right = MONO + PORTAMENTO ON
  // left  = POLY + PORTAMENTO OFF
  currentPreset.mono = currentPreset.glide ? 1 : 0;

  applyModeAndGlideAll();
  rebuildHeldNotes();

  // Physical position is authoritative, so this is treated like VOL:
  // it does not mark the preset MODIFIED.
  updateByteLeds();

  if (showPopup) {
    setParamPopup("PORTAMENTO", currentPreset.glide ? "MONO + ON" : "POLY + OFF");
  }

  screenDirty = true;
}

void syncPerformancePhysicalControls(bool showPopup = false) {
  uint16_t raw = angle8.getAnalogInput(6, _12bit);
  uint8_t v = rawTo127(raw, 6);
  setPortamentoFromPhysical(v, showPopup);
}

// ======================================================================
// Modification / popup helpers
// ======================================================================

void markModified() {
  modified = true;
  screenDirty = true;
}

void setParamPopup(const char* name, const char* value) {
  strncpy(paramPopupName, name, sizeof(paramPopupName) - 1);
  strncpy(paramPopupValue, value, sizeof(paramPopupValue) - 1);
  paramPopupActive = true;
  paramPopupUntil = millis() + 950;
  screenDirty = true;
}

void popupNumber(const char* name, int value) {
  char b[20];
  snprintf(b, sizeof(b), "%d", value);
  setParamPopup(name, b);
}

// ======================================================================
// Structural change helper
// When wave/octave changes while keys are held, stop + reapply + rebuild.
// ======================================================================

void reapplyAndRebuild() {
  applyCurrentPresetToSynth(true);
}

// ======================================================================
// Apply knob value
// ======================================================================

void applyPerformanceKnob(uint8_t knob, uint8_t v) {
  switch (knob) {
    case 0:
      masterVolume = v;
      synth.setMasterVolume(masterVolume);
      popupNumber("VOLUME", masterVolume);
      return;

    case 1:
      if (currentPreset.cutoff != v) {
        currentPreset.cutoff = v;
        applyFilterAll();
        markModified();
      }
      popupNumber("CUTOFF", v);
      return;

    case 2:
      if (currentPreset.resonance != v) {
        currentPreset.resonance = v;
        applyFilterAll();
        markModified();
      }
      popupNumber("RESONANCE", v);
      return;

    case 3:
      if (currentPreset.attack != v) {
        currentPreset.attack = v;
        applyEnvelopeAll();
        markModified();
      }
      popupNumber("ATTACK", v);
      return;

    case 4:
      if (currentPreset.release != v) {
        currentPreset.release = v;
        applyEnvelopeAll();
        markModified();
      }
      popupNumber("RELEASE", v);
      return;

    case 5:
      if (currentPreset.vibratoDepth != v || (v > 0 && !currentPreset.vibratoEnabled)) {
        currentPreset.vibratoDepth = v;
        if (v > 0) currentPreset.vibratoEnabled = 1;
        applyVibratoAll();
        markModified();
      }
      popupNumber("VIB DEPTH", v);
      return;

    case 6:
      setPortamentoFromPhysical(v, true);
      return;

    case 7:
      if (currentPreset.glideTime != v) {
        currentPreset.glideTime = v;
        applyModeAndGlideAll();
        markModified();
      }
      popupNumber("GLIDE TIME", v);
      return;
  }
}

void applyFilterPageKnob(uint8_t knob, uint8_t v) {
  bool changed = false;

  switch (knob) {
    case 0:
      changed = currentPreset.cutoff != v;
      currentPreset.cutoff = v;
      if (changed) applyFilterAll();
      break;
    case 1:
      changed = currentPreset.resonance != v;
      currentPreset.resonance = v;
      if (changed) applyFilterAll();
      break;
    case 2:
      changed = currentPreset.attack != v;
      currentPreset.attack = v;
      if (changed) applyEnvelopeAll();
      break;
    case 3:
      changed = currentPreset.decay != v;
      currentPreset.decay = v;
      if (changed) applyEnvelopeAll();
      break;
    case 4:
      changed = currentPreset.release != v;
      currentPreset.release = v;
      if (changed) applyEnvelopeAll();
      break;
    case 5:
      changed = currentPreset.reverb != v;
      currentPreset.reverb = v;
      if (v > 0) currentPreset.reverbEnabled = 1;
      if (changed) applyReverbAll();
      break;
    case 6:
      changed = currentPreset.vibratoRate != v;
      currentPreset.vibratoRate = v;
      if (changed) applyVibratoAll();
      break;
    case 7:
      changed = currentPreset.vibratoDepth != v;
      currentPreset.vibratoDepth = v;
      if (v > 0) currentPreset.vibratoEnabled = 1;
      if (changed) applyVibratoAll();
      break;
  }

  if (changed) markModified();
}

void applyOscPageKnob(uint8_t page, uint8_t knob, uint8_t v) {
  uint8_t oi = page - PAGE_OSC1;
  OscState& o = currentPreset.osc[oi];
  bool changed = false;

  switch (knob) {
    case 0: {
      uint8_t wi = (uint8_t)(((uint16_t)v * 8) / 128);
      if (wi > 7) wi = 7;
      if (o.bank != WAVE_TABLE[wi].bank || o.program != WAVE_TABLE[wi].program) {
        o.bank = WAVE_TABLE[wi].bank;
        o.program = WAVE_TABLE[wi].program;
        changed = true;
        reapplyAndRebuild();
      }
      break;
    }

    case 1:
      if (o.level != v) {
        o.level = v;
        synth.setVolume(OSC_CH[oi], o.level);
        changed = true;
      }
      break;

    case 2: {
      uint8_t idx = (uint8_t)(((uint16_t)v * 5) / 128);
      if (idx > 4) idx = 4;
      int8_t trans = ((int)idx - 2) * 12;
      if (o.transpose != trans) {
        o.transpose = trans;
        changed = true;
        reapplyAndRebuild();
      }
      break;
    }

    case 3: {
      int8_t det = (int8_t)map127ToSigned(v, -50, 50);
      if (o.detune != det) {
        o.detune = det;
        synth.setTuning(OSC_CH[oi], fineTuneValueFromCents(o.detune), 64);
        changed = true;
      }
      break;
    }

    case 4: {
      int8_t trim = (int8_t)map127ToSigned(v, -63, 63);
      if (o.cutoffTrim != trim) {
        o.cutoffTrim = trim;
        applyFilterAll();
        changed = true;
      }
      break;
    }

    case 5: {
      int8_t trim = (int8_t)map127ToSigned(v, -63, 63);
      if (o.resonanceTrim != trim) {
        o.resonanceTrim = trim;
        applyFilterAll();
        changed = true;
      }
      break;
    }

    case 6:
      if (o.pan != v) {
        o.pan = v;
        synth.setPan(OSC_CH[oi], o.pan);
        changed = true;
      }
      break;

    case 7:
      return;
  }

  if (changed) markModified();
}

void applyModPageKnob(uint8_t knob, uint8_t v) {
  bool changed = false;

  switch (knob) {
    case 0: changed = currentPreset.mod.mwPitch != v;      currentPreset.mod.mwPitch = v; break;
    case 1: changed = currentPreset.mod.mwFilter != v;     currentPreset.mod.mwFilter = v; break;
    case 2: changed = currentPreset.mod.mwAmp != v;        currentPreset.mod.mwAmp = v; break;
    case 3: changed = currentPreset.mod.rate != v;         currentPreset.mod.rate = v; break;
    case 4: changed = currentPreset.mod.pitchDepth != v;   currentPreset.mod.pitchDepth = v; break;
    case 5: changed = currentPreset.mod.filterDepth != v;  currentPreset.mod.filterDepth = v; break;
    case 6: changed = currentPreset.mod.ampDepth != v;     currentPreset.mod.ampDepth = v; break;
    case 7:
      changed = currentPreset.vibratoDelay != v;
      currentPreset.vibratoDelay = v;
      if (changed) applyVibratoAll();
      break;
  }

  if (knob <= 6) {
    currentPreset.mod.configured = 1;
    if (changed) applyModAll();
  }

  if (changed) markModified();
}

void applyKnobValue(uint8_t knob, uint8_t value) {
  uint8_t page = configMode ? configPage : PAGE_PERF;

  if (page == PAGE_PERF) {
    applyPerformanceKnob(knob, value);
  } else if (page == PAGE_FILTER_ENV) {
    applyFilterPageKnob(knob, value);
  } else if (page >= PAGE_OSC1 && page <= PAGE_OSC3) {
    applyOscPageKnob(page, knob, value);
  } else if (page == PAGE_MOD) {
    applyModPageKnob(knob, value);
  }

  screenDirty = true;
}

// ======================================================================
// 8Angle polling / CONFIG switch
// ======================================================================

bool lastConfigRaw = false;

void handleConfigModeChange(bool newMode) {
  if (newMode == configMode) return;

  configMode = newMode;
  paramPopupActive = false;

  if (configMode) {
    configPage = PAGE_PERF;
    snprintf(overlayTitle, sizeof(overlayTitle), "CONFIG MODE");
    snprintf(overlaySub, sizeof(overlaySub), "PAGE 01 / 06");
  } else {
    snprintf(overlayTitle, sizeof(overlayTitle), "PERFORMANCE");
    snprintf(overlaySub, sizeof(overlaySub), "LIVE CONTROL");
  }

  overlayActive = true;
  overlayUntil = millis() + 500;

  if (!configMode) {
    syncPerformancePhysicalControls(false);
  }

  armPickupForCurrentContext();
  updateByteLeds();
  screenDirty = true;
}

void poll8Angle() {
  if (wifiMaintActive() || systemMenuActive || saveDialog ||
      maintenanceConfirm || systemInfoActive) return;

  uint32_t nowMs = millis();
  if (nowMs - lastAnglePoll < ANGLE_POLL_MS) return;
  lastAnglePoll = nowMs;

  bool swRaw = angle8.getDigitalInput();
  bool newConfig = (swRaw == CONFIG_SWITCH_ACTIVE_LEVEL);
  handleConfigModeChange(newConfig);

  for (uint8_t i = 0; i < 8; ++i) {
    uint16_t raw = angle8.getAnalogInput(i, _12bit);
    uint8_t now = rawTo127(raw, i);
    uint8_t prev = pickup[i].lastValue;

    if (now == prev) continue;

    pickup[i].lastValue = now;

    if (knobIsReserved(i)) {
      updatePickupLed(i);
      continue;
    }

    if (!pickup[i].active) {
      bool crossed =
        ((prev <= pickup[i].target && now >= pickup[i].target) ||
         (prev >= pickup[i].target && now <= pickup[i].target) ||
         abs((int)now - (int)pickup[i].target) <= PICKUP_TOLERANCE);

      if (crossed) {
        pickup[i].active = true;
        lastAcceptedAngleValue[i] = now;
        updatePickupLed(i);
        applyKnobValue(i, now);
      } else {
        updatePickupLed(i);
      }
      continue;
    }

    // Ignore +/-1 count jitter. Slow physical movement still works because
    // lastAcceptedAngleValue is only updated when a value is accepted.
    if (abs((int)now - (int)lastAcceptedAngleValue[i]) <= ANGLE_VALUE_DEADBAND) {
      continue;
    }

    lastAcceptedAngleValue[i] = now;
    applyKnobValue(i, now);
  }
}

// ======================================================================
// ByteButton LEDs / input
// ======================================================================

void setLogicalByteLed(uint8_t logical, uint32_t rgb, uint8_t brightness) {
  uint8_t physical = logicalToPhysicalButton(logical);
  byteButton.setLEDBrightness(physical, brightness);
  byteButton.setRGB888(physical, rgb);
}

void updateByteLeds() {
  if (!configMode) {
    // PERFORMANCE preset numbers follow the numbers printed on ByteButton:
    // physical right -> left = preset 0..7.
    for (uint8_t physical = 0; physical < 8; ++physical) {
      bool selected = (browseBank == loadedBank && physical == loadedSlot);
      byteButton.setLEDBrightness(physical, selected ? 90 : 28);
      byteButton.setRGB888(physical, selected ? 0xFFB000 : 0x302000);
    }
    return;
  }

  bool states[7] = {
    (bool)currentPreset.mono,
    (bool)currentPreset.glide,
    (bool)currentPreset.legato,
    forceSustain,
    (bool)currentPreset.soft,
    (bool)currentPreset.reverbEnabled,
    (bool)currentPreset.vibratoEnabled
  };

  for (uint8_t i = 0; i < 7; ++i) {
    if (states[i]) setLogicalByteLed(i, 0x00FF40, 70);
    else           setLogicalByteLed(i, 0x302000, 24);
  }

  // PANIC is an action, always red.
  setLogicalByteLed(7, 0xFF0000, 55);
}

void toggleConfigButton(uint8_t logical) {
  switch (logical) {
    case 0: { // MONO
      silenceSynthOnly();
      currentPreset.mono = !currentPreset.mono;
      if (!currentPreset.mono) currentPreset.glide = 0;
      applyModeAndGlideAll();
      rebuildHeldNotes();
      markModified();
      break;
    }

    case 1: { // GLIDE
      silenceSynthOnly();
      currentPreset.glide = !currentPreset.glide;
      if (currentPreset.glide) currentPreset.mono = 1;
      applyModeAndGlideAll();
      rebuildHeldNotes();
      markModified();
      break;
    }

    case 2: // LEGATO
      currentPreset.legato = !currentPreset.legato;
      markModified();
      break;

    case 3: // SUSTAIN runtime only
      forceSustain = !forceSustain;
      applyPedalsAll();
      screenDirty = true;
      break;

    case 4: // SOFT
      currentPreset.soft = !currentPreset.soft;
      applyPedalsAll();
      markModified();
      break;

    case 5: // REVERB ENABLE
      currentPreset.reverbEnabled = !currentPreset.reverbEnabled;
      applyReverbAll();
      markModified();
      break;

    case 6: // VIBRATO ENABLE
      currentPreset.vibratoEnabled = !currentPreset.vibratoEnabled;
      applyVibratoAll();
      markModified();
      break;

    case 7: // PANIC
      panicAll("MANUAL");
      break;
  }

  updateByteLeds();
}

void handleBytePress(uint8_t logical) {
  if (saveDialog) return;

  if (!configMode) {
    loadPreset(browseBank, logical);
    armPickupForCurrentContext();
    updateByteLeds();

    snprintf(overlayTitle, sizeof(overlayTitle), "PRESET LOAD");
    snprintf(overlaySub, sizeof(overlaySub), "B%u / P%u", loadedBank + 1, loadedSlot + 1);
    overlayActive = true;
    overlayUntil = millis() + 380;
    screenDirty = true;
    return;
  }

  toggleConfigButton(logical);
}

void pollByteButton() {
  if (wifiMaintActive() || systemMenuActive || saveDialog ||
      maintenanceConfirm || systemInfoActive) return;

  uint32_t nowMs = millis();
  if (nowMs - lastBytePoll < BYTE_POLL_MS) return;
  lastBytePoll = nowMs;

  for (uint8_t physical = 0; physical < 8; ++physical) {
    // PERFORMANCE:
    //   preset slot == physical channel number
    //   rightmost CH0 = preset 0, ... leftmost CH7 = preset 7
    //
    // CONFIG:
    //   keep the existing user-facing left->right function layout.
    uint8_t logical = configMode ? (7 - physical) : physical;

    bool raw = byteButton.getSwitchStatus(physical) != 0;

    if (raw != byteRaw[logical]) {
      byteRaw[logical] = raw;
      byteChangedAt[logical] = nowMs;
    }

    if ((nowMs - byteChangedAt[logical]) >= BYTE_DEBOUNCE_MS &&
        byteStable[logical] != byteRaw[logical]) {
      byteStable[logical] = byteRaw[logical];
      if (byteStable[logical]) {
        handleBytePress(logical);
      }
    }
  }
}



// ======================================================================
// Maintenance persistence across software reboot / OTA
// ======================================================================

void setMaintenanceSticky(bool enabled) {
  prefs.putBool(MAINT_STICKY_KEY, enabled);
}

void evaluateMaintenanceResumeAtBoot() {
  esp_reset_reason_t reason = esp_reset_reason();
  bool sticky = prefs.getBool(MAINT_STICKY_KEY, false);

  // A real power cycle is the explicit "leave maintenance" action.
  // Brownout is treated the same way for safety.
  if (reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT) {
    sticky = false;
    prefs.putBool(MAINT_STICKY_KEY, false);
  }

  // One-time migration helper:
  // v1.7.5 cannot set MAINT_STICKY_KEY before the very first OTA to v1.7.6.
  // If this is the first v1.7.6 boot, the reset was software-triggered, and
  // Wi-Fi credentials already exist, assume we arrived through Web/Arduino OTA.
  //
  // This makes the first update into the sticky-maintenance firmware behave
  // like later updates. The marker prevents this heuristic from running again.
  bool migrated = prefs.getBool(MAINT_MIGRATED_KEY, false);
  if (!migrated) {
    prefs.putBool(MAINT_MIGRATED_KEY, true);

    if (!sticky &&
        reason == ESP_RST_SW &&
        wifiMaintHasCredentials()) {
      sticky = true;
      prefs.putBool(MAINT_STICKY_KEY, true);
    }
  }

  resumeMaintenanceOnBoot = sticky;
}

// ======================================================================
// SYSTEM MENU / Wi-Fi Maintenance
// ======================================================================

void openSystemMenu() {
  systemMenuActive = true;
  systemMenuIndex = 0;

  saveDialog = false;
  maintenanceConfirm = false;
  usbFlashConfirm = false;
  systemInfoActive = false;

  paramPopupActive = false;
  overlayActive = false;
  screenDirty = true;
}

void closeSystemMenu() {
  systemMenuActive = false;
  saveDialog = false;
  maintenanceConfirm = false;
  usbFlashConfirm = false;
  systemInfoActive = false;
  screenDirty = true;
}

void enterUsbFlashBootloader() {
  systemMenuActive = false;
  usbFlashConfirm = false;

  // Make the synth quiet before the USB role / boot mode transition.
  silenceSynthOnly();
  clearHeldState();
  keyboardSustain = false;
  forceSustain = false;

  // Wi-Fi is normally already OFF outside MAINTENANCE, but make sure
  // no maintenance web runtime is left active.
  if (wifiMaintActive()) {
    wifiMaintStop();
    resetMaintenanceUiCache();
  }

  clearScreen();
  drawHazardStripe(0, 10);

  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 22);
  M5.Display.print("USB FLASH");

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_WHITE, C_BLACK);
  M5.Display.setCursor(10, 53);
  M5.Display.print("ENTERING ESP32-S3 ROM DOWNLOAD MODE");
  M5.Display.setCursor(10, 70);
  M5.Display.print("KEEP PC USB CABLE CONNECTED");
  M5.Display.setCursor(10, 87);
  M5.Display.print("PC SHOULD RE-DETECT A COM PORT");
  M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.setCursor(10, 111);
  M5.Display.print("REAR BOOT BUTTON = RECOVERY IF NEEDED");

  delay(700);

#if CONFIG_IDF_TARGET_ESP32S3
  // This is the same Arduino-ESP32 core mechanism used by USB CDC's
  // automatic 1200-baud bootloader reset path on ESP32-S3.
  //
  // On success this function calls esp_restart() internally and does
  // not return to the sketch. The ROM then waits for a USB/UART download.
  usb_persist_restart(RESTART_BOOTLOADER);
#endif

  // We should never arrive here on ESP32-S3 if the core accepted the request.
  clearScreen();
  drawHazardStripe(0, 10);
  M5.Display.setTextColor(C_RED, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 28);
  M5.Display.print("USB FLASH FAIL");
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_WHITE, C_BLACK);
  M5.Display.setCursor(10, 65);
  M5.Display.print("USE REAR BOOT BUTTON FOR RECOVERY");
  while (true) {
    M5.update();
    delay(20);
  }
}

void enterMaintenanceMode() {
  systemMenuActive = false;
  maintenanceConfirm = false;
  systemInfoActive = false;

  // Stay in MAINTENANCE across software reboot / OTA.
  setMaintenanceSticky(true);

  // Stop all musical activity before Wi-Fi/OTA.
  silenceSynthOnly();
  clearHeldState();
  keyboardSustain = false;
  forceSustain = false;

  clearScreen();
  drawHazardStripe(0, 10);
  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 25);
  M5.Display.print("MAINTENANCE");
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_WHITE, C_BLACK);
  M5.Display.setCursor(10, 60);
  M5.Display.print("CONNECTING WIFI...");

  resetMaintenanceUiCache();
  wifiMaintStartMaintenance();
  screenDirty = true;
}

void enterWifiSetupMode() {
  systemMenuActive = false;
  maintenanceConfirm = false;
  systemInfoActive = false;

  // WIFI SETUP is not sticky maintenance.
  setMaintenanceSticky(false);
  resumeMaintenanceOnBoot = false;

  silenceSynthOnly();
  clearHeldState();

  resetMaintenanceUiCache();
  wifiMaintStartSetup();
  screenDirty = true;
}

void exitWifiRuntime() {
  if (wifiMaintUpdating()) return;

  // Explicit orange encoder press = leave sticky maintenance.
  setMaintenanceSticky(false);
  resumeMaintenanceOnBoot = false;

  wifiMaintStop();
  resetMaintenanceUiCache();

  // Re-apply synth state after leaving maintenance.
  applyCurrentPresetToSynth(false);
  armPickupForCurrentContext();
  updateByteLeds();

  screenDirty = true;
}

void selectSystemMenuItem() {
  switch (systemMenuIndex) {
    case 0: // SAVE PRESET
      systemMenuActive = false;
      openSaveDialog();
      return;

    case 1: // MAINTENANCE
      systemMenuActive = false;
      maintenanceConfirm = true;
      maintenanceChoiceYes = false;
      screenDirty = true;
      return;

    case 2: // WIFI SETUP
      enterWifiSetupMode();
      return;

    case 3: // USB FLASH
      systemMenuActive = false;
      usbFlashConfirm = true;
      usbFlashChoiceYes = false;
      screenDirty = true;
      return;

    case 4: // SYSTEM INFO
      systemMenuActive = false;
      systemInfoActive = true;
      screenDirty = true;
      return;

    case 5: // EXIT
    default:
      closeSystemMenu();
      return;
  }
}

// ======================================================================
// Encoder processing
// ======================================================================

int takeEncoderDetents() {
  noInterrupts();
  int32_t t = encoderTransitions;
  encoderTransitions = 0;
  interrupts();

  encoderRemainder += (int)t * ENCODER_DIRECTION;
  int detents = encoderRemainder / ENC_TRANSITIONS_PER_DETENT;
  encoderRemainder %= ENC_TRANSITIONS_PER_DETENT;
  return detents;
}

void showPageOverlay() {
  snprintf(overlayTitle, sizeof(overlayTitle), "PAGE CHANGE");
  snprintf(overlaySub, sizeof(overlaySub), "%02u/06 %s", configPage + 1, PAGE_NAMES[configPage]);
  overlayActive = true;
  overlayUntil = millis() + 320;
  screenDirty = true;
}

void handleEncoderRotate(int detents) {
  if (detents == 0) return;

  if (wifiMaintActive()) {
    return;
  }

  if (saveDialog) {
    saveChoiceYes = (detents > 0);
    screenDirty = true;
    return;
  }

  if (maintenanceConfirm) {
    maintenanceChoiceYes = (detents > 0);
    screenDirty = true;
    return;
  }

  if (usbFlashConfirm) {
    usbFlashChoiceYes = (detents > 0);
    screenDirty = true;
    return;
  }

  if (systemInfoActive) {
    return;
  }

  if (systemMenuActive) {
    int m = (int)systemMenuIndex + detents;
    while (m < 0) m += SYSTEM_MENU_COUNT;
    while (m >= SYSTEM_MENU_COUNT) m -= SYSTEM_MENU_COUNT;
    systemMenuIndex = (uint8_t)m;
    screenDirty = true;
    return;
  }

  if (configMode) {
    int p = (int)configPage + detents;
    while (p < 0) p += PAGE_COUNT;
    while (p >= PAGE_COUNT) p -= PAGE_COUNT;
    if ((uint8_t)p != configPage) {
      configPage = (uint8_t)p;
      armPickupForCurrentContext();
      showPageOverlay();
    }
    return;
  }

  int b = (int)browseBank + detents;
  while (b < 0) b += BANK_COUNT;
  while (b >= BANK_COUNT) b -= BANK_COUNT;
  if ((uint8_t)b != browseBank) {
    browseBank = (uint8_t)b;
    updateByteLeds();

    snprintf(overlayTitle, sizeof(overlayTitle), "BANK SELECT");
    snprintf(overlaySub, sizeof(overlaySub), "BANK %u", browseBank + 1);
    overlayActive = true;

    // v1.8.2: keep the bank-selection screen readable.
    // Each encoder move refreshes this timer.
    overlayUntil = millis() + 1200;
    screenDirty = true;
  }
}

void openSaveDialog() {
  saveDialog = true;
  saveChoiceYes = false;
  paramPopupActive = false;
  overlayActive = false;
  screenDirty = true;
}

void handleEncoderShortPress() {
  // During maintenance/setup, short press exits unless an OTA write is active.
  if (wifiMaintActive()) {
    exitWifiRuntime();
    return;
  }

  if (systemInfoActive) {
    systemInfoActive = false;
    systemMenuActive = true;
    screenDirty = true;
    return;
  }

  if (saveDialog) {
    if (saveChoiceYes) {
      saveCurrentPreset();

      saveDialog = false;
      snprintf(overlayTitle, sizeof(overlayTitle), "SAVED");
      snprintf(overlaySub, sizeof(overlaySub), "B%u / P%u", loadedBank + 1, loadedSlot + 1);
      overlayActive = true;
      overlayUntil = millis() + 650;
    } else {
      saveDialog = false;
      systemMenuActive = true;
    }
    screenDirty = true;
    return;
  }

  if (maintenanceConfirm) {
    if (maintenanceChoiceYes) {
      maintenanceConfirm = false;
      enterMaintenanceMode();
    } else {
      maintenanceConfirm = false;
      systemMenuActive = true;
      screenDirty = true;
    }
    return;
  }

  if (usbFlashConfirm) {
    if (usbFlashChoiceYes) {
      usbFlashConfirm = false;
      enterUsbFlashBootloader();
    } else {
      usbFlashConfirm = false;
      systemMenuActive = true;
      screenDirty = true;
    }
    return;
  }

  if (systemMenuActive) {
    selectSystemMenuItem();
    return;
  }

  // Normal-mode short press remains reserved.
  snprintf(overlayTitle, sizeof(overlayTitle),
           configMode ? "CONFIG" : "MENU");
  snprintf(overlaySub, sizeof(overlaySub), "SHORT PRESS RESERVED");
  overlayActive = true;
  overlayUntil = millis() + 350;
  screenDirty = true;
}

void handleEncoderLongPress() {
  if (wifiMaintActive()) return;
  if (saveDialog || maintenanceConfirm || usbFlashConfirm || systemInfoActive) return;

  if (systemMenuActive) {
    closeSystemMenu();
  } else {
    openSystemMenu();
  }
}


void pollEncoderButton() {
  uint32_t nowMs = millis();
  bool raw = (digitalRead(ENC_BTN_PIN) == LOW);

  if (raw != encBtnRaw) {
    encBtnRaw = raw;
    encBtnChangedAt = nowMs;
  }

  if ((nowMs - encBtnChangedAt) >= ENC_DEBOUNCE_MS &&
      encBtnStable != encBtnRaw) {
    encBtnStable = encBtnRaw;

    if (encBtnStable) {
      encBtnPressedAt = nowMs;
      encBtnLongFired = false;
    } else {
      if (!encBtnLongFired) handleEncoderShortPress();
    }
  }

  if (encBtnStable && !encBtnLongFired &&
      (nowMs - encBtnPressedAt) >= ENC_LONG_MS) {
    encBtnLongFired = true;
    handleEncoderLongPress();
  }
}

// ======================================================================
// Screen drawing
// ======================================================================

void drawHazardStripe(int y, int h) {
  int w = M5.Display.width();
  M5.Display.fillRect(0, y, w, h, C_YELLOW);
  for (int x = -h; x < w + h; x += 14) {
    M5.Display.drawLine(x, y + h - 1, x + h, y, C_BLACK);
    M5.Display.drawLine(x + 1, y + h - 1, x + h + 1, y, C_BLACK);
    M5.Display.drawLine(x + 2, y + h - 1, x + h + 2, y, C_BLACK);
  }
}

void clearScreen() {
  M5.Display.fillScreen(C_BLACK);
  M5.Display.setTextWrap(false);
}

void drawOverlayScreen() {
  clearScreen();
  int w = M5.Display.width();

  drawHazardStripe(0, 10);
  M5.Display.drawRect(5, 16, w - 10, 105, C_AMBER);

  if (strcmp(overlayTitle, "BANK SELECT") == 0) {
    M5.Display.setTextColor(C_YELLOW, C_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(15, 25);
    M5.Display.print("BANK SELECT");

    M5.Display.setTextColor(C_GREY, C_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(15, 52);
    M5.Display.print("BANK");

    // BANK_COUNT is 8, so the displayed bank number is always one digit.
    // Use a fixed center position; no text-bounds API is required.
    M5.Display.setTextColor(C_WHITE, C_BLACK);
    M5.Display.setTextSize(4);
    M5.Display.setCursor((w / 2) - 12, 64);
    M5.Display.print(browseBank + 1);

    M5.Display.setTextSize(1);
  } else {
    M5.Display.setTextColor(C_YELLOW, C_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(15, 33);
    M5.Display.print(overlayTitle);

    M5.Display.setTextColor(C_WHITE, C_BLACK);
    M5.Display.setTextSize(1);
    M5.Display.setCursor(15, 70);
    M5.Display.print(overlaySub);
  }

  drawHazardStripe(125, 10);
}

void drawSystemMenu() {
  clearScreen();
  int w = M5.Display.width();

  drawHazardStripe(0, 9);
  M5.Display.drawRect(5, 14, w - 10, 116, C_AMBER);

  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 20);
  M5.Display.print("SYSTEM MENU");

  M5.Display.setTextSize(1);

  for (uint8_t i = 0; i < SYSTEM_MENU_COUNT; ++i) {
    int y = 46 + i * 13;

    if (i == systemMenuIndex) {
      M5.Display.fillRect(10, y - 2, w - 20, 13, C_YELLOW);
      M5.Display.setTextColor(C_BLACK, C_YELLOW);
      M5.Display.setCursor(15, y);
      M5.Display.print("> ");
      M5.Display.print(SYSTEM_MENU_ITEMS[i]);
    } else {
      M5.Display.setTextColor(C_WHITE, C_BLACK);
      M5.Display.setCursor(15, y);
      M5.Display.print("  ");
      M5.Display.print(SYSTEM_MENU_ITEMS[i]);
    }
  }

  M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.setCursor(10, 126);
  M5.Display.print("TURN=SELECT  PUSH=ENTER  HOLD=EXIT");
}

void drawMaintenanceConfirm() {
  clearScreen();
  int w = M5.Display.width();

  drawHazardStripe(0, 9);
  M5.Display.drawRect(5, 14, w - 10, 112, C_AMBER);

  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 25);
  M5.Display.print("MAINTENANCE ?");

  M5.Display.setTextColor(C_WHITE, C_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(10, 55);
  M5.Display.print("STOP PERFORMANCE / START WIFI OTA");

  M5.Display.setTextSize(2);

  M5.Display.setCursor(35, 88);
  if (!maintenanceChoiceYes) M5.Display.setTextColor(C_BLACK, C_YELLOW);
  else                       M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.print(" NO ");

  M5.Display.setCursor(145, 88);
  if (maintenanceChoiceYes) M5.Display.setTextColor(C_BLACK, C_YELLOW);
  else                      M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.print(" YES ");
}

void drawUsbFlashConfirm() {
  clearScreen();
  int w = M5.Display.width();

  drawHazardStripe(0, 9);
  M5.Display.drawRect(5, 14, w - 10, 112, C_AMBER);

  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 23);
  M5.Display.print("USB FLASH ?");

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_WHITE, C_BLACK);
  M5.Display.setCursor(10, 51);
  M5.Display.print("ENTER ROM DOWNLOAD MODE");
  M5.Display.setCursor(10, 66);
  M5.Display.print("USB MIDI STOPS UNTIL NEXT FIRMWARE");
  M5.Display.setCursor(10, 81);
  M5.Display.print("KEEP PC USB CONNECTED");

  M5.Display.setTextSize(2);

  M5.Display.setCursor(35, 98);
  if (!usbFlashChoiceYes) M5.Display.setTextColor(C_BLACK, C_YELLOW);
  else                    M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.print(" NO ");

  M5.Display.setCursor(145, 98);
  if (usbFlashChoiceYes) M5.Display.setTextColor(C_BLACK, C_YELLOW);
  else                   M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.print(" YES ");
}

void drawSystemInfo() {
  clearScreen();
  int w = M5.Display.width();

  drawHazardStripe(0, 9);
  M5.Display.drawRect(5, 14, w - 10, 112, C_AMBER);

  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 22);
  M5.Display.print("SYSTEM INFO");

  M5.Display.setTextColor(C_WHITE, C_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(10, 52);
  M5.Display.print("FW          : v1.8.4");

  M5.Display.setCursor(10, 68);
  M5.Display.print("WIFI SAVED  : ");
  M5.Display.print(wifiMaintHasCredentials() ? "YES" : "NO");

  M5.Display.setCursor(10, 84);
  M5.Display.print("SAVED SSID  : ");
  String ssid = wifiMaintSavedSsid();
  if (ssid.length() == 0) ssid = "-";
  if (ssid.length() > 18) ssid = ssid.substring(0, 18);
  M5.Display.print(ssid);

  M5.Display.setCursor(10, 100);
  M5.Display.printf("PRESET      : B%u / P%u", loadedBank + 1, loadedSlot);

  M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.setCursor(10, 120);
  M5.Display.print("PUSH = BACK");
}

void drawWifiRuntimeScreen() {
  clearScreen();
  int w = M5.Display.width();

  drawHazardStripe(0, 9);
  M5.Display.drawRect(5, 14, w - 10, 115, C_AMBER);

  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(9, 21);

  if (wifiMaintMode() == WifiMaintMode::SETUP_AP) {
    M5.Display.print("WIFI SETUP");
  } else {
    M5.Display.print("MAINTENANCE");
  }

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_WHITE, C_BLACK);

  M5.Display.setCursor(10, 50);
  M5.Display.print(wifiMaintStatus());

  M5.Display.setCursor(10, 66);
  M5.Display.print("MODE : ");
  M5.Display.print(wifiMaintModeText());

  M5.Display.setCursor(10, 82);
  M5.Display.print("IP   : ");
  M5.Display.print(wifiMaintIp());

  M5.Display.setCursor(10, 98);
  if (wifiMaintStaConnected()) {
    M5.Display.print("WEB  : http://dinmeter.local/");
  } else {
    M5.Display.print("AP   : DinMeter-Setup");
  }

  if (wifiMaintUpdating()) {
    M5.Display.setTextColor(C_GREEN, C_BLACK);
    M5.Display.setCursor(10, 114);
    M5.Display.printf("UPDATING... %d%%", wifiMaintProgress());
  } else {
    M5.Display.setTextColor(C_GREY, C_BLACK);
    M5.Display.setCursor(10, 114);
    M5.Display.print("WEB OTA / ARDUINO OTA READY");
    M5.Display.setCursor(10, 126);
    M5.Display.print("PUSH = EXIT");
  }
}

void drawSaveDialog() {
  clearScreen();
  int w = M5.Display.width();

  drawHazardStripe(0, 10);
  M5.Display.drawRect(5, 15, w - 10, 110, C_AMBER);

  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(12, 25);
  M5.Display.print("SAVE PRESET ?");

  M5.Display.setTextColor(C_WHITE, C_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(12, 58);
  M5.Display.printf("B%u / P%u  %.18s", loadedBank + 1, loadedSlot + 1, currentPreset.name);

  M5.Display.setTextSize(2);
  M5.Display.setCursor(35, 90);
  if (!saveChoiceYes) M5.Display.setTextColor(C_BLACK, C_YELLOW);
  else                M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.print(" NO ");

  M5.Display.setCursor(145, 90);
  if (saveChoiceYes) M5.Display.setTextColor(C_BLACK, C_YELLOW);
  else               M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.print(" YES ");
}

void drawParamPopup() {
  clearScreen();
  int w = M5.Display.width();

  drawHazardStripe(0, 8);
  M5.Display.drawRect(5, 13, w - 10, 112, C_AMBER);

  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(14, 28);
  M5.Display.print(paramPopupName);

  M5.Display.setTextColor(C_WHITE, C_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(14, 65);
  M5.Display.print(paramPopupValue);

  // simple value bar if numeric
  int v = atoi(paramPopupValue);
  if ((paramPopupValue[0] >= '0' && paramPopupValue[0] <= '9') ||
      paramPopupValue[0] == '-') {
    v = constrain(v, 0, 127);
    M5.Display.drawRect(14, 108, w - 28, 8, C_GREY);
    M5.Display.fillRect(16, 110, (w - 32) * v / 127, 4, C_AMBER);
  }
}

void drawPerformanceScreen() {
  clearScreen();
  int w = M5.Display.width();
  int cellW = w / 8;

  drawHazardStripe(0, 8);

  // Top status
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setCursor(6, 13);
  M5.Display.printf("B%u", browseBank + 1);

  M5.Display.setCursor(34, 13);
  M5.Display.setTextColor(C_WHITE, C_BLACK);
  M5.Display.printf("P%u", loadedSlot);

  if (browseBank != loadedBank) {
    M5.Display.setTextColor(C_ORANGE, C_BLACK);
    M5.Display.setCursor(w - 75, 13);
    M5.Display.print("SELECT BANK");
  } else if (modified) {
    M5.Display.setTextColor(C_RED, C_BLACK);
    M5.Display.setCursor(w - 58, 13);
    M5.Display.print("MODIFIED");
  }

  // Preset name
  M5.Display.drawRect(5, 27, w - 10, 30, C_AMBER);
  M5.Display.setTextColor(C_WHITE, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 35);
  M5.Display.printf("%.18s", currentPreset.name);

  // 8Angle assignment labels - always visible in PERFORMANCE
  M5.Display.setTextSize(1);
  for (uint8_t i = 0; i < 8; ++i) {
    int x = i * cellW;

    // cell separator
    if (i > 0) {
      M5.Display.drawLine(x, 65, x, 102, C_DARK);
    }

    M5.Display.setTextColor(C_AMBER, C_BLACK);
    M5.Display.setCursor(x + 2, 68);
    M5.Display.print(PERF_LABELS[i]);

    char v[8];
    formatParamValue(PAGE_PERF, i, v, sizeof(v));

    if (i == 6) {
      // PORTAMENTO is a left/right physical switch-like control.
      M5.Display.setTextColor(currentPreset.glide ? C_GREEN : C_GREY, C_BLACK);
      M5.Display.setCursor(x + 4, 84);
      M5.Display.print(currentPreset.glide ? "ON" : "OFF");
    } else {
      M5.Display.setTextColor(C_WHITE, C_BLACK);
      M5.Display.setCursor(x + 3, 84);
      M5.Display.print(v);
    }
  }

  M5.Display.drawLine(0, 105, w, 105, C_AMBER);

  // Bottom status
  M5.Display.setTextColor(currentPreset.mono ? C_YELLOW : C_GREY, C_BLACK);
  M5.Display.setCursor(6, 111);
  M5.Display.print(currentPreset.mono ? "MONO" : "POLY");

  M5.Display.setTextColor(currentPreset.glide ? C_GREEN : C_GREY, C_BLACK);
  M5.Display.setCursor(48, 111);
  M5.Display.print(currentPreset.glide ? "PORT ON" : "PORT OFF");

  M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.setCursor(115, 111);
  M5.Display.print("ENC=BANK");

  M5.Display.setCursor(6, 123);
  M5.Display.print("BYTE=PRESET   HOLD ENC=SAVE");
}

const char** labelsForPage(uint8_t page) {
  if (page == PAGE_PERF) return PERF_LABELS;
  if (page == PAGE_FILTER_ENV) return FILTER_LABELS;
  if (page >= PAGE_OSC1 && page <= PAGE_OSC3) return OSC_LABELS;
  return MOD_LABELS;
}

void formatParamValue(uint8_t page, uint8_t knob, char* out, size_t n) {
  if (page == PAGE_PERF || page == PAGE_FILTER_ENV || page == PAGE_MOD) {
    snprintf(out, n, "%u", currentParamAs127(knob));
    return;
  }

  if (page >= PAGE_OSC1 && page <= PAGE_OSC3) {
    uint8_t oi = page - PAGE_OSC1;
    const OscState& o = currentPreset.osc[oi];

    switch (knob) {
      case 0:
        snprintf(out, n, "%s", WAVE_TABLE[waveIndexForOsc(oi)].label);
        return;
      case 1:
        snprintf(out, n, "%u", o.level);
        return;
      case 2:
        snprintf(out, n, "%+d", o.transpose / 12);
        return;
      case 3:
        snprintf(out, n, "%+d", o.detune);
        return;
      case 4:
        snprintf(out, n, "%+d", o.cutoffTrim);
        return;
      case 5:
        snprintf(out, n, "%+d", o.resonanceTrim);
        return;
      case 6:
        snprintf(out, n, "%u", o.pan);
        return;
      default:
        snprintf(out, n, "-");
        return;
    }
  }

  snprintf(out, n, "-");
}

void drawPageTabs() {
  int w = M5.Display.width();
  int tabW = w / PAGE_COUNT;

  for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
    int x = i * tabW;
    if (i == configPage) {
      M5.Display.fillRect(x, 0, tabW - 1, 16, C_YELLOW);
      M5.Display.setTextColor(C_BLACK, C_YELLOW);
    } else {
      M5.Display.drawRect(x, 0, tabW - 1, 16, C_DARK);
      M5.Display.setTextColor(C_GREY, C_BLACK);
    }
    M5.Display.setTextSize(1);
    M5.Display.setCursor(x + 2, 4);
    M5.Display.print(PAGE_TAB[i]);
  }
}

void drawConfigScreen() {
  clearScreen();
  int w = M5.Display.width();

  drawPageTabs();

  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(5, 20);
  M5.Display.printf("CONFIG // %s", PAGE_NAMES[configPage]);

  drawHazardStripe(34, 6);

  const char** labels = labelsForPage(configPage);
  int cellW = w / 8;

  // 8Angle row
  M5.Display.setTextSize(1);
  for (uint8_t i = 0; i < 8; ++i) {
    int x = i * cellW;
    M5.Display.setTextColor(C_AMBER, C_BLACK);
    M5.Display.setCursor(x + 2, 44);
    M5.Display.print(labels[i]);

    char v[8];
    formatParamValue(configPage, i, v, sizeof(v));
    M5.Display.setTextColor(C_WHITE, C_BLACK);
    M5.Display.setCursor(x + 2, 58);
    M5.Display.print(v);
  }

  M5.Display.drawLine(0, 76, w, 76, C_AMBER);

  // ByteButton row
  bool btnStates[7] = {
    (bool)currentPreset.mono,
    (bool)currentPreset.glide,
    (bool)currentPreset.legato,
    forceSustain,
    (bool)currentPreset.soft,
    (bool)currentPreset.reverbEnabled,
    (bool)currentPreset.vibratoEnabled
  };

  for (uint8_t i = 0; i < 8; ++i) {
    int x = i * cellW;

    M5.Display.setTextColor(C_AMBER, C_BLACK);
    M5.Display.setCursor(x + 2, 83);
    M5.Display.print(CONFIG_BUTTON_LABELS[i]);

    if (i < 7) {
      M5.Display.setTextColor(btnStates[i] ? C_GREEN : C_GREY, C_BLACK);
      M5.Display.setCursor(x + 8, 99);
      M5.Display.print(btnStates[i] ? "ON" : "--");
    } else {
      M5.Display.setTextColor(C_RED, C_BLACK);
      M5.Display.setCursor(x + 5, 99);
      M5.Display.print("!!");
    }
  }

  if (modified) {
    M5.Display.setTextColor(C_RED, C_BLACK);
    M5.Display.setCursor(5, 117);
    M5.Display.print("* MODIFIED");
  } else {
    M5.Display.setTextColor(C_GREY, C_BLACK);
    M5.Display.setCursor(5, 117);
    M5.Display.print("ENC=PAGE   HOLD=SAVE");
  }

  drawHazardStripe(128, 7);
}

bool maintenanceUiChanged() {
  if (!wifiMaintActive()) return false;

  WifiMaintMode mode = wifiMaintMode();
  String status = wifiMaintStatus();
  String ip = wifiMaintIp();
  int progress = wifiMaintProgress();
  bool updating = wifiMaintUpdating();

  bool changed =
      mode != lastMaintUiMode ||
      status != lastMaintUiStatus ||
      ip != lastMaintUiIp ||
      progress != lastMaintUiProgress ||
      updating != lastMaintUiUpdating;

  if (changed) {
    lastMaintUiMode = mode;
    lastMaintUiStatus = status;
    lastMaintUiIp = ip;
    lastMaintUiProgress = progress;
    lastMaintUiUpdating = updating;
  }

  return changed;
}

void resetMaintenanceUiCache() {
  lastMaintUiMode = WifiMaintMode::OFF;
  lastMaintUiStatus = "";
  lastMaintUiIp = "";
  lastMaintUiProgress = -1;
  lastMaintUiUpdating = false;
}

void drawUiIfNeeded() {
  uint32_t nowMs = millis();

  if (overlayActive && (int32_t)(nowMs - overlayUntil) >= 0) {
    overlayActive = false;
    screenDirty = true;
  }

  if (paramPopupActive && (int32_t)(nowMs - paramPopupUntil) >= 0) {
    paramPopupActive = false;
    screenDirty = true;
  }

  // Maintenance mode: redraw only when visible state actually changes.
  // This removes the constant clear-and-redraw flicker from v1.7.
  if (wifiMaintActive() && maintenanceUiChanged()) {
    screenDirty = true;
  }

  if (!screenDirty) return;
  if (nowMs - lastUiDraw < UI_REFRESH_MIN_MS) return;
  lastUiDraw = nowMs;

  if (wifiMaintActive()) {
    drawWifiRuntimeScreen();
  } else if (systemInfoActive) {
    drawSystemInfo();
  } else if (usbFlashConfirm) {
    drawUsbFlashConfirm();
  } else if (maintenanceConfirm) {
    drawMaintenanceConfirm();
  } else if (saveDialog) {
    drawSaveDialog();
  } else if (systemMenuActive) {
    drawSystemMenu();
  } else if (overlayActive) {
    drawOverlayScreen();
  } else if (paramPopupActive && !configMode) {
    drawParamPopup();
  } else if (configMode) {
    drawConfigScreen();
  } else {
    drawPerformanceScreen();
  }

  screenDirty = false;
}

// ======================================================================
// Setup
// ======================================================================

void synthAppSetup() {
  // Display / Din Meter
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setRotation(1);
  M5.Display.setBrightness(170);
  clearScreen();

  // Encoder
  pinMode(ENC_PIN_A, INPUT_PULLUP);
  pinMode(ENC_PIN_B, INPUT_PULLUP);
  pinMode(ENC_BTN_PIN, INPUT_PULLUP);

  encoderLastEncoded = (digitalRead(ENC_PIN_A) << 1) | digitalRead(ENC_PIN_B);
  attachInterrupt(ENC_PIN_A, onEncoderChange, CHANGE);
  attachInterrupt(ENC_PIN_B, onEncoderChange, CHANGE);

  // I2C / Units
  Wire.begin(I2C_SDA, I2C_SCL, 100000);
  delay(20);

  if (!angle8.begin(ANGLE8_I2C_ADDR)) {
    clearScreen();
    M5.Display.setTextColor(C_RED, C_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 30);
    M5.Display.print("8ANGLE FAIL");
    while (true) delay(100);
  }

  if (!byteButton.begin(&Wire, BYTE_ADDR, I2C_SDA, I2C_SCL, 100000)) {
    clearScreen();
    M5.Display.setTextColor(C_RED, C_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(10, 30);
    M5.Display.print("BYTE FAIL");
    while (true) delay(100);
  }

  byteButton.setLEDShowMode(BYTE_LED_USER_DEFINED);

  // MIDI Unit on PORT B
  synth.begin(&SynthSerial, MIDI_BAUD, MIDI_RX, MIDI_TX);
  delay(120);

  // NVS
  prefs.begin("dmsynth-v1", false);

  // Wi-Fi credentials use their own NVS namespace.
  // Normal PERFORMANCE / CONFIG starts with Wi-Fi completely OFF.
  wifiMaintInit();

  // Decide whether this boot should return directly to MAINTENANCE.
  evaluateMaintenanceResumeAtBoot();

  loadedBank = prefs.getUChar("bank", 0);
  loadedSlot = prefs.getUChar("slot", 0);
  if (loadedBank >= BANK_COUNT) loadedBank = 0;
  if (loadedSlot >= PRESETS_PER_BANK) loadedSlot = 0;
  browseBank = loadedBank;

  loadPresetData(currentPresetIndex(), currentPreset);

  // Current physical volume is authoritative at boot.
  uint16_t volRaw = angle8.getAnalogInput(0, _12bit);
  masterVolume = rawTo127(volRaw, 0);

  configMode = (angle8.getDigitalInput() == CONFIG_SWITCH_ACTIVE_LEVEL);
  if (configMode) configPage = PAGE_PERF;

  // If booting into PERFORMANCE, the physical Portamento knob is authoritative.
  if (!configMode) {
    uint16_t portRaw = angle8.getAnalogInput(6, _12bit);
    uint8_t portV = rawTo127(portRaw, 6);
    currentPreset.glide = (portV >= 64) ? 1 : 0;
    currentPreset.mono  = currentPreset.glide ? 1 : 0;
  }

  applyCurrentPresetToSynth(false);

  armPickupForCurrentContext();
  updateByteLeds();

  // USB MIDI host
  usbMidi.onMidiMessage(onMidiMessage);
  usbMidi.onDeviceConnected(onMidiDeviceConnected);
  usbMidi.onDeviceDisconnected(onMidiDeviceDisconnected);
  usbMidi.begin();

  if (resumeMaintenanceOnBoot) {
    // OTA/ESP.restart while maintenance was active:
    // return to home Wi-Fi / AP fallback automatically.
    enterMaintenanceMode();
    return;
  }

  snprintf(overlayTitle, sizeof(overlayTitle), "DIN SYNTH v1.8.4");
  snprintf(overlaySub, sizeof(overlaySub), "WIFI OTA READY");
  overlayActive = true;
  overlayUntil = millis() + 850;
  screenDirty = true;
}

// ======================================================================
// Main loop
// ======================================================================

void synthAppLoop() {
  M5.update();

  if (wifiMaintActive()) {
    // Maintenance mode deliberately pauses performance processing.
    wifiMaintLoop();

    int detents = takeEncoderDetents();
    if (detents != 0) {
      handleEncoderRotate(detents);
    }

    pollEncoderButton();
    drawUiIfNeeded();
    return;
  }

  // Critical: keep USB Host serviced continuously during normal operation.
  usbMidi.update();

  poll8Angle();
  pollByteButton();

  int detents = takeEncoderDetents();
  if (detents != 0) {
    handleEncoderRotate(detents);
  }

  pollEncoderButton();
  drawUiIfNeeded();
}

/*
  ======================================================================
  Module : DinMeter Synth Controller
  Version: v1.8.4
  END
  ======================================================================
*/
