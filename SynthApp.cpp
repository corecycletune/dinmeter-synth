/*
  ======================================================================
  Module : DinMeter Synth Controller
  Version: v1.10.9
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
#include <stddef.h>
#include <math.h>
#include <esp_system.h>
#include "esp32-hal-tinyusb.h"

// Embedded in the compiled .bin so Web OTA can inspect the selected
// firmware version BEFORE any upload starts.
static const char DINMETER_FW_MARKER[] __attribute__((used)) =
  "DINMETER_FW_VERSION=v1.10.9";
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
static constexpr uint8_t GM_CH = 3;

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

// The DIN Meter window hides a few pixels on the physical left edge.
// Primary performance/config UI is inset slightly so labels and tabs remain
// fully visible while preserving the existing right edge.
static constexpr int PRIMARY_UI_X_OFFSET = 6;

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

// Stored v1.9.x layout. Keep this byte-for-byte compatible so presets already
// saved in NVS can be migrated when the modular tail is introduced.
struct StoredPresetV1910 {
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

  uint8_t legacyChorusProgram;
  uint8_t legacyChorusSend;
  uint8_t legacySpatialVolume;
  uint8_t legacySpatialDelay;
};

static constexpr uint8_t ENV_COUNT = 3;
static constexpr uint8_t LFO_COUNT = 3;
static constexpr uint8_t MOD_ROUTE_COUNT = 16;
static constexpr uint8_t MODULAR_SCHEMA_VERSION = 1;

enum EnvCurve : uint8_t {
  ENV_CURVE_LINEAR = 0,
  ENV_CURVE_EXP = 1,
  ENV_CURVE_LOG = 2
};

enum LfoWaveform : uint8_t {
  LFO_WAVE_SINE = 0,
  LFO_WAVE_TRIANGLE = 1,
  LFO_WAVE_SAW_UP = 2,
  LFO_WAVE_SAW_DOWN = 3,
  LFO_WAVE_SQUARE = 4,
  LFO_WAVE_RANDOM = 5
};

enum ModSource : uint8_t {
  MODSRC_NONE = 0,
  MODSRC_ENV1 = 1,
  MODSRC_ENV2 = 2,
  MODSRC_ENV3 = 3,
  MODSRC_LFO1 = 4,
  MODSRC_LFO2 = 5,
  MODSRC_LFO3 = 6,
  MODSRC_VELOCITY = 7,
  MODSRC_KEY = 8,
  MODSRC_MODWHEEL = 9,
  MODSRC_AFTERTOUCH = 10
};

enum ModDestination : uint8_t {
  MODDST_NONE = 0,
  MODDST_CUTOFF = 1,
  MODDST_PITCH = 2,
  MODDST_AMP = 3
};

struct EnvelopeState {
  uint8_t attack;      // normalized 0..127; runtime maps this to time
  uint8_t decay;       // normalized 0..127
  uint8_t sustain;     // 0..127
  uint8_t release;     // normalized 0..127
  uint8_t curve;       // EnvCurve
  uint8_t retrigger;   // 0 = legato/free, 1 = NoteOn retrigger
};

struct LfoState {
  uint8_t waveform;    // LfoWaveform
  uint8_t rate;        // normalized 0..127; runtime maps this to Hz
  uint8_t delay;       // normalized 0..127
  uint8_t fade;        // normalized 0..127
  uint8_t retrigger;   // 0 = free-running, 1 = restart on NoteOn
  uint8_t phase;       // startup phase 0..127
};

struct ModRoute {
  uint8_t source;      // ModSource
  uint8_t destination; // ModDestination
  int8_t amount;       // bipolar -127..+127
  uint8_t enabled;
};

struct Preset {
  // IMPORTANT: everything above modularSchemaVersion must remain identical to
  // StoredPresetV1910. This lets old saved presets migrate losslessly.
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

  // v1.10 modular tail. These are currently data-only; the modulation engine
  // will begin consuming them in later v1.10.x steps.
  uint8_t modularSchemaVersion;
  EnvelopeState env[ENV_COUNT];
  LfoState lfo[LFO_COUNT];
  ModRoute routes[MOD_ROUTE_COUNT];
};

static_assert(offsetof(Preset, modularSchemaVersion) == sizeof(StoredPresetV1910),
              "Preset legacy prefix changed; NVS migration would break.");

static constexpr uint8_t BANK_COUNT = 9;
static constexpr uint8_t PRESETS_PER_BANK = 8;
static constexpr uint8_t GM_BANK_INDEX = BANK_COUNT - 1;
static constexpr uint8_t PRESET_COUNT = BANK_COUNT * PRESETS_PER_BANK;

static const char* BANK_SHORT_NAMES[BANK_COUNT] = {
  "B1", "B2", "B3", "B4", "B5", "B6", "B7", "B8", "GM"
};

static const char* BANK_GROUP_NAMES[BANK_COUNT] = {
  "BANK 1", "BANK 2", "BANK 3", "BANK 4",
  "BANK 5", "BANK 6", "BANK 7", "BANK 8", "GM"
};

struct __attribute__((packed)) GmLayerState {
  uint8_t version;
  uint8_t program;
  uint8_t level;
  int8_t transpose;
  uint8_t pan;
};

static constexpr uint8_t GM_LAYER_VERSION = 1;

Preset currentPreset;
GmLayerState currentGmLayer = {GM_LAYER_VERSION, 0, 0, 0, 64};

uint8_t browseBank  = 0;  // encoder selects this bank
uint8_t loadedBank  = 0;  // actually sounding bank
uint8_t loadedSlot  = 0;  // 0..7
bool modified       = false;

uint8_t masterVolume = 100;

// Runtime-only states
bool keyboardSustain = false;
bool recoveringFromPanic = false;

bool configAuditionEnabled = false;
bool configAuditionNoteOn = false;
uint8_t configAuditionStep = 0;
uint8_t configAuditionNote = 60;
uint32_t configAuditionChangedAt = 0;

static constexpr uint32_t CONFIG_AUDITION_NOTE_MS = 800;
static constexpr uint32_t CONFIG_AUDITION_GAP_MS  = 800;
static constexpr uint8_t CONFIG_AUDITION_VELOCITY = 96;

// C2..C6 around middle C, deliberately moving up/down across the keyboard.
static const uint8_t CONFIG_AUDITION_NOTES[] = {
  60, 67, 72, 76, 60, 55, 48, 52, 60, 79, 43, 84, 36, 64
};
static constexpr uint8_t CONFIG_AUDITION_NOTE_COUNT =
    sizeof(CONFIG_AUDITION_NOTES) / sizeof(CONFIG_AUDITION_NOTES[0]);

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

// Bank 9 starts with these eight useful GM programs. The GM layer is not
// restricted to them: GM CONFIG can select any standard program 1..128.
struct GmQuickPreset {
  const char* name;
  uint8_t program;
};

static const GmQuickPreset GM_QUICK_PRESETS[PRESETS_PER_BANK] = {
  {"GM Grand Piano", 0},
  {"GM EPiano 1",    4},
  {"GM EPiano 2",    5},
  {"GM Vibraphone", 11},
  {"GM Draw Organ", 16},
  {"GM Finger Bass",33},
  {"GM Warm Pad",   89},
  {"GM Saw Lead",   81},
};

static const char* GM_PROGRAM_NAMES[128] = {
  "GrandPno","BrightPno","EGrand","HonkyTonk","EPiano1","EPiano2","Harpsi","Clav",
  "Celesta","Glock","MusicBox","Vibes","Marimba","Xylophone","TubBell","Dulcimer",
  "DrawOrgan","PercOrgan","RockOrgan","ChurchOrg","ReedOrgan","Accordion","Harmonica","TangoAcc",
  "NylonGtr","SteelGtr","JazzGtr","CleanGtr","MuteGtr","Overdrive","DistGtr","GtrHarm",
  "AcBass","FingerBass","PickBass","Fretless","SlapBass1","SlapBass2","SynBass1","SynBass2",
  "Violin","Viola","Cello","Contrabass","TremStr","PizzStr","Harp","Timpani",
  "Strings1","Strings2","SynStr1","SynStr2","ChoirAah","VoiceOoh","SynVoice","OrchHit",
  "Trumpet","Trombone","Tuba","MuteTrump","FrenchHorn","Brass1","SynBrass1","SynBrass2",
  "SopSax","AltoSax","TenorSax","BariSax","Oboe","EngHorn","Bassoon","Clarinet",
  "Piccolo","Flute","Recorder","PanFlute","Bottle","Shakuhachi","Whistle","Ocarina",
  "SquareLead","SawLead","Calliope","ChiffLead","Charang","VoiceLead","Fifths","BassLead",
  "NewAgePad","WarmPad","PolyPad","ChoirPad","BowedPad","MetalPad","HaloPad","SweepPad",
  "RainFX","Soundtrack","Crystal","Atmosphere","Brightness","Goblins","Echoes","SciFi",
  "Sitar","Banjo","Shamisen","Koto","Kalimba","Bagpipe","Fiddle","Shanai",
  "TinkleBell","Agogo","SteelDrum","Woodblock","Taiko","MelodTom","SynDrum","RevCymbal",
  "FretNoise","Breath","Seashore","Bird","Telephone","Helicopter","Applause","Gunshot"
};

const char* gmProgramName(uint8_t program) {
  return GM_PROGRAM_NAMES[program & 0x7F];
}

// ======================================================================
// CONFIG pages
// ======================================================================

enum ConfigPage : uint8_t {
  PAGE_PERF = 0,
  PAGE_FILTER_ENV,
  PAGE_VOICE_ENV,
  PAGE_OSC,
  PAGE_GM,
  PAGE_ENV,
  PAGE_LFO,
  PAGE_ROUTE,
  PAGE_MOD,
  PAGE_COUNT
};

uint8_t configPage = PAGE_PERF;
bool configMode = false;
uint8_t selectedOsc = 0;
uint8_t selectedEnv = 0;
uint8_t selectedLfo = 0;
uint8_t selectedRoute = 0;

static const char* PAGE_NAMES[PAGE_COUNT] = {
  "PERFORMANCE", "TONE / FX", "VOICE ENV", "OSC",
  "GM LAYER", "MOD ENV", "LFO", "ROUTE", "MOD"
};

static const char* PAGE_TAB[PAGE_COUNT] = {
  "PF", "TN", "VN", "OS", "GM", "EN", "LF", "RT", "MD"
};

static const char* PERF_LABELS[8] = {
  "VOL", "CUT", "RES", "V-A", "V-R", "VIB", "PRT", "GLD"
};

static const char* FILTER_LABELS[8] = {
  "CUT", "RES", "REV", "VR", "VD", "---", "---", "---"
};

static const char* VOICE_ENV_LABELS[8] = {
  "ATK", "DEC", "REL", "---", "---", "---", "---", "---"
};

static const char* OSC_LABELS[8] = {
  "WAV", "LVL", "OCT", "DET", "CUT", "RES", "PAN", "---"
};

static const char* GM_LABELS[8] = {
  "PRG", "LVL", "OCT", "PAN", "---", "---", "---", "---"
};

static const char* ENV_LABELS[8] = {
  "ATK", "DEC", "SUS", "REL", "CUR", "RTR", "---", "---"
};

static const char* LFO_LABELS[8] = {
  "WAV", "RATE", "DLY", "FADE", "RTR", "PHS", "---", "---"
};

static const char* ROUTE_LABELS[8] = {
  "SRC", "DST", "AMT", "ON", "---", "---", "---", "---"
};

static const char* MOD_LABELS[8] = {
  "MWP", "MWF", "MWA", "RATE", "PDP", "FDP", "ADP", "VDL"
};

static const char* CONFIG_BUTTON_LABELS[8] = {
  "MON", "GLD", "LEG", "SFT", "REV", "VIB", "PNC", "TST"
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
// Modular modulation runtime
// ENV is calculated inside the ESP32. Only the final destination value is
// emitted to SAM2695, avoiding one MIDI stream per virtual patch cable.
// ======================================================================

enum EnvStage : uint8_t {
  ENV_STAGE_IDLE = 0,
  ENV_STAGE_ATTACK,
  ENV_STAGE_DECAY,
  ENV_STAGE_SUSTAIN,
  ENV_STAGE_RELEASE
};

struct EnvRuntime {
  uint8_t stage;
  float value;          // normalized 0.0 .. 1.0
  float stageStartValue;
  uint32_t stageStartedAt;
};

struct LfoRuntime {
  float phase;          // 0.0 .. <1.0
  float value;          // bipolar -1.0 .. +1.0 after delay/fade
  float randomValue;    // held value for S&H/random waveform
  uint32_t lastUpdatedAt;
  uint32_t triggeredAt;
  bool active;
};

EnvRuntime envRuntime[ENV_COUNT] = {};
LfoRuntime lfoRuntime[LFO_COUNT] = {};
uint32_t lastModOutputAt = 0;
static constexpr uint32_t MOD_OUTPUT_INTERVAL_MS = 32; // ~31 Hz, leaves MIDI headroom

void resetLfoRuntime() {
  uint32_t nowMs = millis();

  for (uint8_t i = 0; i < LFO_COUNT; ++i) {
    lfoRuntime[i].phase = (float)currentPreset.lfo[i].phase / 128.0f;
    lfoRuntime[i].value = 0.0f;
    lfoRuntime[i].randomValue =
        ((float)(esp_random() & 0xFFFF) / 32767.5f) - 1.0f;
    lfoRuntime[i].lastUpdatedAt = nowMs;
    lfoRuntime[i].triggeredAt = 0;
    lfoRuntime[i].active = false;
  }
}

void resetModulationRuntime() {
  uint32_t nowMs = millis();

  for (uint8_t i = 0; i < ENV_COUNT; ++i) {
    envRuntime[i].stage = ENV_STAGE_IDLE;
    envRuntime[i].value = 0.0f;
    envRuntime[i].stageStartValue = 0.0f;
    envRuntime[i].stageStartedAt = nowMs;
  }

  resetLfoRuntime();
  lastModOutputAt = 0;
}

// DinMeter-owned mono/legato voice.
// While software portamento is active, SAM2695 receives only one NoteOn per
// phrase. Overlapping keys move that sounding voice with Pitch Bend instead
// of creating another voice / retriggering the envelope.
int16_t monoAnchorNote = -1;       // physical note that produced the NoteOn
int16_t monoTargetNote = -1;       // last-note-priority destination
int16_t monoBendCurrent = 8192;    // 14-bit MIDI Pitch Bend
int16_t monoBendStart = 8192;
int16_t monoBendTarget = 8192;
int16_t performanceBendCurrent = 8192; // external pitch wheel when glide is inactive
uint32_t monoGlideStartedAt = 0;
uint32_t monoGlideDurationMs = 0;
uint32_t monoGlideLastSendAt = 0;

static constexpr int16_t PITCH_BEND_CENTER = 8192;
static constexpr uint8_t SOFTWARE_GLIDE_BEND_RANGE = 24; // +/-24 semitones
static constexpr uint32_t GLIDE_UPDATE_MS = 5;           // ~200 Hz

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

bool wifiSelectActive = false;
uint8_t wifiSelectIndex = 0;  // 0=AUTO, 1..N=saved Wi-Fi profile

bool wifiSetupMenuActive = false;
uint8_t wifiSetupMenuIndex = 0;  // ADD / DELETE / BACK

bool wifiScanListActive = false;
uint8_t wifiScanIndex = 0;
int wifiScanLastResult = 0;

enum class TextEditorPurpose : uint8_t {
  NONE = 0,
  WIFI_PASSWORD,
  PRESET_NAME
};

bool textEditorActive = false;
TextEditorPurpose textEditorPurpose = TextEditorPurpose::NONE;
String textEditorContext;
String textEditorBuffer;
String textEditorMessage;
uint8_t textEditorGroup = 0;     // ABC / abc / 123 / symbols
uint8_t textEditorCharIndex = 0;
uint8_t textEditorMaxLen = 63;

bool wifiDeleteListActive = false;
uint8_t wifiDeleteIndex = 0;     // saved-profile ordinal, count means BACK
bool wifiDeleteConfirm = false;
bool wifiDeleteChoiceYes = false;

bool maintenanceConfirm = false;
bool maintenanceChoiceYes = false;

// v1.7.9 experimental wired flashing:
// ask the Arduino-ESP32 core to restart the ESP32-S3 into the ROM
// USB download bootloader, equivalent in purpose to the rear BOOT-button path.
bool usbFlashConfirm = false;
bool usbFlashChoiceYes = false;

bool systemInfoActive = false;

static constexpr uint8_t SYSTEM_MENU_COUNT = 7;
static const char* SYSTEM_MENU_ITEMS[SYSTEM_MENU_COUNT] = {
  "SAVE PRESET",
  "MAINTENANCE",
  "WIFI SETUP",
  "WIFI SELECT",
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

static constexpr uint8_t LIVE_FILTER_CC1 = 16;  // reserved inside DinMeter
static constexpr uint8_t LIVE_FILTER_CC2 = 17;  // reserved inside DinMeter

// Defined in General helpers below.
uint8_t oscEffectiveCutoff(uint8_t oscIndex);

void applyMasterVolume() {
  synth.setMasterVolume(masterVolume);
}

uint8_t gsPartForMidiChannel(uint8_t ch) {
  ch &= 0x0F;

  // SAM2695 default GS part assignment:
  // part 0 -> MIDI ch10 (index 9)
  // parts 1..9 -> MIDI ch1..9 (indices 0..8)
  // parts 10..15 -> MIDI ch11..16 (indices 10..15)
  if (ch == 9) return 0;
  if (ch <= 8) return ch + 1;
  return ch;
}

void sendGsPartParameter(uint8_t ch, uint8_t addressBlock, uint8_t parameter, uint8_t value) {
  // SAM2695 GS DT1 style message. GS addresses use PART number, not MIDI
  // channel number, so translate the default channel->part mapping first.
  const uint8_t part = gsPartForMidiChannel(ch);
  const uint8_t msg[11] = {
    0xF0, 0x41, 0x00, 0x42, 0x12, 0x40,
    (uint8_t)(addressBlock | (part & 0x0F)),
    parameter,
    (uint8_t)(value & 0x7F),
    0x00,
    0xF7
  };
  SynthSerial.write(msg, sizeof(msg));
}

void configureLiveCutoffController(uint8_t ch) {
  // Assign two internal controllers to TVF cutoff, but keep the MIDI
  // controller values pinned at maximum. The knob will move the GS TVF
  // CUTOFF CONTROL depth itself (0..127, 64=center), which lets us test
  // the full negative-to-positive modulation range on a sounding voice.
  sendGsPartParameter(ch, 0x10, 0x1F, LIVE_FILTER_CC1);
  sendGsPartParameter(ch, 0x10, 0x20, LIVE_FILTER_CC2);

  // CC1: cutoff only. Start neutral until sendLiveCutoff() sets the depth.
  sendGsPartParameter(ch, 0x20, 0x40, 0x40); // pitch: neutral
  sendGsPartParameter(ch, 0x20, 0x41, 0x40); // TVF cutoff: neutral
  sendGsPartParameter(ch, 0x20, 0x42, 0x40); // amplitude: neutral
  sendGsPartParameter(ch, 0x20, 0x44, 0x00); // LFO pitch depth: off
  sendGsPartParameter(ch, 0x20, 0x45, 0x00); // LFO TVF depth: off
  sendGsPartParameter(ch, 0x20, 0x46, 0x00); // LFO TVA depth: off

  // CC2: same cutoff-only setup.
  sendGsPartParameter(ch, 0x20, 0x50, 0x40); // pitch: neutral
  sendGsPartParameter(ch, 0x20, 0x51, 0x40); // TVF cutoff: neutral
  sendGsPartParameter(ch, 0x20, 0x52, 0x40); // amplitude: neutral
  sendGsPartParameter(ch, 0x20, 0x54, 0x00); // LFO pitch depth: off
  sendGsPartParameter(ch, 0x20, 0x55, 0x00); // LFO TVF depth: off
  sendGsPartParameter(ch, 0x20, 0x56, 0x00); // LFO TVA depth: off

  // Hold both assignable MIDI controllers at full scale. Real-time movement
  // is now done by changing the GS cutoff-control depth, not the CC value.
  sendCC(ch, LIVE_FILTER_CC1, 127);
  sendCC(ch, LIVE_FILTER_CC2, 127);
}

int modulationSourceValue(uint8_t source) {
  switch (source) {
    case MODSRC_ENV1:
    case MODSRC_ENV2:
    case MODSRC_ENV3: {
      uint8_t i = source - MODSRC_ENV1;
      if (i >= ENV_COUNT) return 0;
      return constrain((int)(envRuntime[i].value * 127.0f + 0.5f), 0, 127);
    }

    case MODSRC_LFO1:
    case MODSRC_LFO2:
    case MODSRC_LFO3: {
      uint8_t i = source - MODSRC_LFO1;
      if (i >= LFO_COUNT) return 0;
      return constrain((int)(lfoRuntime[i].value * 127.0f), -127, 127);
    }

    // Velocity / key / controllers remain reserved for later stages.
    default:
      return 0;
  }
}

int modulationForDestination(uint8_t destination) {
  int total = 0;

  for (uint8_t i = 0; i < MOD_ROUTE_COUNT; ++i) {
    const ModRoute& route = currentPreset.routes[i];
    if (!route.enabled || route.destination != destination) continue;
    if (route.source == MODSRC_NONE || route.destination == MODDST_NONE) continue;

    int source = modulationSourceValue(route.source);
    total += (source * (int)route.amount) / 127;
  }

  return total;
}

bool modulationSourceIsEnvelope(uint8_t source) {
  return source >= MODSRC_ENV1 && source <= MODSRC_ENV3;
}

uint8_t modulatedAmpLevel(uint8_t baseLevel) {
  int level = baseLevel;

  for (uint8_t i = 0; i < MOD_ROUTE_COUNT; ++i) {
    const ModRoute& route = currentPreset.routes[i];
    if (!route.enabled || route.destination != MODDST_AMP) continue;
    if (route.source == MODSRC_NONE) continue;

    int source = modulationSourceValue(route.source);
    int amount = route.amount;
    int factor = 127; // unity

    if (modulationSourceIsEnvelope(route.source)) {
      // Positive ENV amount behaves like a VCA envelope:
      // +127 => ENV 0..127 maps gain 0..1.
      // Negative amount produces the inverse contour.
      if (amount >= 0) {
        factor = 127 - amount + (source * amount) / 127;
      } else {
        int depth = -amount;
        factor = 127 - (source * depth) / 127;
      }
    } else {
      // Bipolar sources such as LFO act as tremolo around unity.
      factor = 127 + (source * amount) / 127;
    }

    factor = constrain(factor, 0, 254);
    level = constrain((level * factor + 63) / 127, 0, 127);
  }

  return (uint8_t)level;
}

uint8_t lastLiveAmpSent[4] = {255, 255, 255, 255};

void invalidateLiveAmpCache() {
  for (uint8_t i = 0; i < 4; ++i) lastLiveAmpSent[i] = 255;
}

void sendLiveAmpOsc(uint8_t oscIndex, bool force = false) {
  if (oscIndex >= 3) return;
  uint8_t level = modulatedAmpLevel(currentPreset.osc[oscIndex].level);
  if (!force && lastLiveAmpSent[oscIndex] == level) return;

  synth.setVolume(OSC_CH[oscIndex], level);
  lastLiveAmpSent[oscIndex] = level;
}

void sendLiveAmpGm(bool force = false) {
  uint8_t level = modulatedAmpLevel(currentGmLayer.level);
  if (!force && lastLiveAmpSent[3] == level) return;

  synth.setVolume(GM_CH, level);
  lastLiveAmpSent[3] = level;
}

void sendLiveAmpAll(bool force = false) {
  for (uint8_t i = 0; i < 3; ++i) {
    sendLiveAmpOsc(i, force);
  }
  sendLiveAmpGm(force);
}

uint8_t modulatedCutoffForOsc(uint8_t oscIndex) {
  int value = (int)oscEffectiveCutoff(oscIndex);
  value += modulationForDestination(MODDST_CUTOFF);
  return (uint8_t)constrain(value, 0, 127);
}

uint8_t lastLiveCutoffSent[3] = {255, 255, 255};

void invalidateLiveCutoffCache() {
  for (uint8_t i = 0; i < 3; ++i) lastLiveCutoffSent[i] = 255;
}

void sendLiveCutoff(uint8_t oscIndex, bool force = false) {
  if (oscIndex >= 3) return;
  const uint8_t ch = OSC_CH[oscIndex];
  const uint8_t depth = modulatedCutoffForOsc(oscIndex);

  if (!force && lastLiveCutoffSent[oscIndex] == depth) return;

  // 0 = maximum negative, 64 = neutral, 127 = maximum positive.
  // Stack CC1 and CC2 with the same depth to maximize the audible sweep.
  sendGsPartParameter(ch, 0x20, 0x41, depth);
  sendGsPartParameter(ch, 0x20, 0x51, depth);
  lastLiveCutoffSent[oscIndex] = depth;
}

void sendLiveCutoffAll(bool force = false) {
  for (uint8_t i = 0; i < 3; ++i) {
    sendLiveCutoff(i, force);
  }
}

// Defined below after Pitch Bend helpers.
void refreshModulationOutputs(bool force);

uint32_t envelopeTimeMs(uint8_t value) {
  // Cubic mapping gives usable short times while still reaching long sweeps:
  // 0=0ms, 32~128ms, 64~1024ms, 127~8000ms.
  if (value == 0) return 0;
  uint32_t v = value;
  return (v * v * v * 8000UL) / (127UL * 127UL * 127UL);
}

float shapeEnvelopeProgress(float t, uint8_t curve) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;

  if (curve == ENV_CURVE_EXP) return t * t;
  if (curve == ENV_CURVE_LOG) {
    float inv = 1.0f - t;
    return 1.0f - inv * inv;
  }
  return t;
}

void startEnvelopeStage(uint8_t i, uint8_t stage, uint32_t nowMs, float startValue) {
  if (i >= ENV_COUNT) return;
  envRuntime[i].stage = stage;
  envRuntime[i].stageStartedAt = nowMs;
  envRuntime[i].stageStartValue = startValue;
}

void triggerModEnvelopes(bool phraseStart) {
  uint32_t nowMs = millis();

  for (uint8_t i = 0; i < ENV_COUNT; ++i) {
    const EnvelopeState& env = currentPreset.env[i];

    // First note of a phrase always starts the envelope. Overlapping notes only
    // retrigger modules whose RTR flag is enabled.
    if (!phraseStart && !env.retrigger) continue;

    if (env.attack == 0) {
      envRuntime[i].value = 1.0f;
      startEnvelopeStage(i, ENV_STAGE_DECAY, nowMs, 1.0f);
    } else {
      envRuntime[i].value = 0.0f;
      startEnvelopeStage(i, ENV_STAGE_ATTACK, nowMs, 0.0f);
    }
  }

  // For zero-attack envelopes, push all destinations before NoteOn.
  refreshModulationOutputs(false);
}

void releaseModEnvelopes() {
  uint32_t nowMs = millis();

  for (uint8_t i = 0; i < ENV_COUNT; ++i) {
    if (envRuntime[i].stage == ENV_STAGE_IDLE) continue;

    if (currentPreset.env[i].release == 0) {
      envRuntime[i].value = 0.0f;
      startEnvelopeStage(i, ENV_STAGE_IDLE, nowMs, 0.0f);
    } else {
      startEnvelopeStage(i, ENV_STAGE_RELEASE, nowMs, envRuntime[i].value);
    }
  }
}

void updateEnvelopeRuntime(uint8_t i, uint32_t nowMs) {
  if (i >= ENV_COUNT) return;

  EnvRuntime& rt = envRuntime[i];
  const EnvelopeState& env = currentPreset.env[i];
  uint32_t elapsed = nowMs - rt.stageStartedAt;

  switch (rt.stage) {
    case ENV_STAGE_IDLE:
      rt.value = 0.0f;
      return;

    case ENV_STAGE_ATTACK: {
      uint32_t duration = envelopeTimeMs(env.attack);
      if (duration == 0 || elapsed >= duration) {
        rt.value = 1.0f;
        startEnvelopeStage(i, ENV_STAGE_DECAY, nowMs, 1.0f);
        return;
      }

      float t = shapeEnvelopeProgress((float)elapsed / (float)duration, env.curve);
      rt.value = rt.stageStartValue + (1.0f - rt.stageStartValue) * t;
      return;
    }

    case ENV_STAGE_DECAY: {
      float sustain = (float)env.sustain / 127.0f;
      uint32_t duration = envelopeTimeMs(env.decay);
      if (duration == 0 || elapsed >= duration) {
        rt.value = sustain;
        startEnvelopeStage(i, ENV_STAGE_SUSTAIN, nowMs, sustain);
        return;
      }

      float t = shapeEnvelopeProgress((float)elapsed / (float)duration, env.curve);
      rt.value = rt.stageStartValue + (sustain - rt.stageStartValue) * t;
      return;
    }

    case ENV_STAGE_SUSTAIN:
      rt.value = (float)env.sustain / 127.0f;
      return;

    case ENV_STAGE_RELEASE: {
      uint32_t duration = envelopeTimeMs(env.release);
      if (duration == 0 || elapsed >= duration) {
        rt.value = 0.0f;
        startEnvelopeStage(i, ENV_STAGE_IDLE, nowMs, 0.0f);
        return;
      }

      float t = shapeEnvelopeProgress((float)elapsed / (float)duration, env.curve);
      rt.value = rt.stageStartValue * (1.0f - t);
      return;
    }
  }
}

float lfoRateHz(uint8_t value) {
  // Exponential musical range. The current GS cutoff transport is rate-limited
  // to ~42 updates/s, so cap this first implementation at 8 Hz.
  const float normalized = (float)value / 127.0f;
  return 0.05f * powf(160.0f, normalized); // 0.05 .. 8.0 Hz
}

uint32_t lfoTimeMs(uint8_t value) {
  // Quadratic 0..5000 ms mapping gives useful resolution near zero.
  if (value == 0) return 0;
  uint32_t v = value;
  return (v * v * 5000UL) / (127UL * 127UL);
}

float lfoWaveValue(uint8_t waveform, float phase, float randomValue) {
  switch (waveform) {
    case LFO_WAVE_SINE:
      return sinf(phase * 2.0f * PI);

    case LFO_WAVE_TRIANGLE:
      return 1.0f - 4.0f * fabsf(phase - 0.5f);

    case LFO_WAVE_SAW_UP:
      return phase * 2.0f - 1.0f;

    case LFO_WAVE_SAW_DOWN:
      return 1.0f - phase * 2.0f;

    case LFO_WAVE_SQUARE:
      return phase < 0.5f ? 1.0f : -1.0f;

    case LFO_WAVE_RANDOM:
      return randomValue;

    default:
      return 0.0f;
  }
}

void triggerModLfos(bool phraseStart) {
  uint32_t nowMs = millis();

  for (uint8_t i = 0; i < LFO_COUNT; ++i) {
    const LfoState& lfo = currentPreset.lfo[i];

    if (!phraseStart && !lfo.retrigger) continue;

    lfoRuntime[i].active = true;
    lfoRuntime[i].triggeredAt = nowMs;

    if (lfo.retrigger) {
      lfoRuntime[i].phase = (float)lfo.phase / 128.0f;
      lfoRuntime[i].randomValue =
          ((float)(esp_random() & 0xFFFF) / 32767.5f) - 1.0f;
    }
  }
}

void updateLfoRuntime(uint8_t i, uint32_t nowMs) {
  if (i >= LFO_COUNT) return;

  LfoRuntime& rt = lfoRuntime[i];
  const LfoState& lfo = currentPreset.lfo[i];

  uint32_t deltaMs = nowMs - rt.lastUpdatedAt;
  rt.lastUpdatedAt = nowMs;

  float nextPhase = rt.phase + ((float)deltaMs / 1000.0f) * lfoRateHz(lfo.rate);
  bool wrapped = nextPhase >= 1.0f;

  if (nextPhase >= 1.0f) {
    nextPhase -= floorf(nextPhase);
  }
  rt.phase = nextPhase;

  if (wrapped && lfo.waveform == LFO_WAVE_RANDOM) {
    rt.randomValue =
        ((float)(esp_random() & 0xFFFF) / 32767.5f) - 1.0f;
  }

  if (!rt.active) {
    rt.value = 0.0f;
    return;
  }

  uint32_t elapsed = nowMs - rt.triggeredAt;
  uint32_t delayMs = lfoTimeMs(lfo.delay);
  if (elapsed < delayMs) {
    rt.value = 0.0f;
    return;
  }

  float gain = 1.0f;
  uint32_t fadeMs = lfoTimeMs(lfo.fade);
  if (fadeMs > 0) {
    uint32_t fadeElapsed = elapsed - delayMs;
    gain = min(1.0f, (float)fadeElapsed / (float)fadeMs);
  }

  rt.value = lfoWaveValue(lfo.waveform, rt.phase, rt.randomValue) * gain;
}

bool hasDynamicRouteForDestination(uint8_t destination) {
  for (uint8_t i = 0; i < MOD_ROUTE_COUNT; ++i) {
    const ModRoute& route = currentPreset.routes[i];
    if (!route.enabled || route.destination != destination) continue;
    if (route.source >= MODSRC_ENV1 && route.source <= MODSRC_LFO3) return true;
  }
  return false;
}

// Implemented below, after Pitch Bend helpers are defined.
void sendCurrentPitchModulation();

void updateModulationEngine() {
  uint32_t nowMs = millis();

  for (uint8_t i = 0; i < ENV_COUNT; ++i) {
    updateEnvelopeRuntime(i, nowMs);
  }
  for (uint8_t i = 0; i < LFO_COUNT; ++i) {
    updateLfoRuntime(i, nowMs);
  }

  const bool cutoffActive = hasDynamicRouteForDestination(MODDST_CUTOFF);
  const bool pitchActive  = hasDynamicRouteForDestination(MODDST_PITCH);
  const bool ampActive    = hasDynamicRouteForDestination(MODDST_AMP);

  if (!cutoffActive && !pitchActive && !ampActive) return;
  if (nowMs - lastModOutputAt < MOD_OUTPUT_INTERVAL_MS) return;
  lastModOutputAt = nowMs;

  if (cutoffActive) {
    // Only active oscillator layers need continuous GS filter updates.
    for (uint8_t i = 0; i < 3; ++i) {
      if (currentPreset.osc[i].level > 0) sendLiveCutoff(i);
    }
  }

  if (pitchActive) sendCurrentPitchModulation();

  if (ampActive) {
    for (uint8_t i = 0; i < 3; ++i) {
      if (currentPreset.osc[i].level > 0) sendLiveAmpOsc(i);
    }
    if (currentGmLayer.level > 0) sendLiveAmpGm();
  }
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

void setPitchBendRange(uint8_t ch, uint8_t semitones) {
  sendCC(ch, 101, 0);
  sendCC(ch, 100, 0);
  sendCC(ch, 6, semitones & 0x7F);
  sendCC(ch, 38, 0);
  sendCC(ch, 101, 127);
  sendCC(ch, 100, 127);
}

void sendPitchBend14(uint8_t ch, int bend) {
  bend = constrain(bend, 0, 16383);
  sendPitchBendRaw(ch, bend & 0x7F, (bend >> 7) & 0x7F);
}

bool softwareMonoVoiceActive() {
  // DinMeter owns the sounding mono voice whenever either GLIDE or LEGATO
  // needs behavior that SAM2695's native mono mode cannot provide cleanly.
  return currentPreset.mono && (currentPreset.glide || currentPreset.legato);
}

bool softwarePortamentoActive() {
  return currentPreset.mono && currentPreset.glide;
}

int modularPitchBendOffset() {
  int modulation = constrain(modulationForDestination(MODDST_PITCH), -127, 127);
  if (modulation == 0) return 0;

  // Full route depth = +/-2 semitones. During software glide the channel
  // bend range is +/-24 semitones, so convert the same musical depth into
  // the smaller 14-bit offset required by that wider bend range.
  uint8_t rangeSemitones =
      softwareMonoVoiceActive() ? SOFTWARE_GLIDE_BEND_RANGE : 2;

  long numerator = (long)modulation * 8192L * 2L;
  long denominator = 127L * max((int)rangeSemitones, 1);
  return (int)(numerator / denominator);
}

void sendSoftwareBendAll(int bend) {
  bend = constrain(bend + modularPitchBendOffset(), 0, 16383);

  for (uint8_t i = 0; i < 3; ++i) {
    if (currentPreset.osc[i].level > 0) {
      sendPitchBend14(OSC_CH[i], bend);
    }
  }
  if (currentGmLayer.level > 0) sendPitchBend14(GM_CH, bend);
}

void sendCurrentPitchModulation() {
  int baseBend = softwareMonoVoiceActive()
               ? monoBendCurrent
               : performanceBendCurrent;
  sendSoftwareBendAll(baseBend);
}

void refreshModulationOutputs(bool force) {
  sendLiveCutoffAll(force);
  sendCurrentPitchModulation();
  sendLiveAmpAll(force);
}

void resetSoftwareBend(bool sendNow = true) {
  monoBendCurrent = PITCH_BEND_CENTER;
  monoBendStart = PITCH_BEND_CENTER;
  monoBendTarget = PITCH_BEND_CENTER;
  performanceBendCurrent = PITCH_BEND_CENTER;
  monoGlideStartedAt = millis();
  monoGlideDurationMs = 0;
  monoGlideLastSendAt = 0;
  if (sendNow) sendSoftwareBendAll(PITCH_BEND_CENTER);
}

int bendForTargetNote(int16_t anchorNote, int16_t targetNote) {
  if (anchorNote < 0 || targetNote < 0) return PITCH_BEND_CENTER;
  int deltaSemitones = (int)targetNote - (int)anchorNote;
  deltaSemitones = constrain(deltaSemitones,
                             -(int)SOFTWARE_GLIDE_BEND_RANGE,
                              (int)SOFTWARE_GLIDE_BEND_RANGE);
  long offset = ((long)deltaSemitones * 8192L) / SOFTWARE_GLIDE_BEND_RANGE;
  return constrain((int)(PITCH_BEND_CENTER + offset), 0, 16383);
}

uint32_t glideDurationFromValue(uint8_t value) {
  // Musical response: short values stay fast while the upper half gains a
  // wider slow-glide range. 0 = immediate, 127 ~= 2.5 s.
  if (value == 0) return 0;
  uint32_t v = value;
  return 8UL + (v * v * 2492UL) / (127UL * 127UL);
}

void startSoftwareGlideTo(int16_t targetNote) {
  if (monoAnchorNote < 0 || targetNote < 0) return;

  monoTargetNote = targetNote;
  monoBendStart = monoBendCurrent;
  monoBendTarget = bendForTargetNote(monoAnchorNote, targetNote);
  monoGlideStartedAt = millis();
  monoGlideDurationMs = currentPreset.glide
                      ? glideDurationFromValue(currentPreset.glideTime)
                      : 0;

  if (monoGlideDurationMs == 0 || monoBendStart == monoBendTarget) {
    monoBendCurrent = monoBendTarget;
    sendSoftwareBendAll(monoBendCurrent);
    monoGlideLastSendAt = millis();
  }
}

void updateSoftwareGlide() {
  if (!softwareMonoVoiceActive()) return;
  if (monoAnchorNote < 0) return;
  if (monoBendCurrent == monoBendTarget) return;

  uint32_t now = millis();
  if (now - monoGlideLastSendAt < GLIDE_UPDATE_MS) return;
  monoGlideLastSendAt = now;

  if (monoGlideDurationMs == 0) {
    monoBendCurrent = monoBendTarget;
  } else {
    uint32_t elapsed = now - monoGlideStartedAt;
    if (elapsed >= monoGlideDurationMs) {
      monoBendCurrent = monoBendTarget;
    } else {
      long span = (long)monoBendTarget - (long)monoBendStart;
      monoBendCurrent = (int16_t)((long)monoBendStart +
                        (span * (long)elapsed) / (long)monoGlideDurationMs);
    }
  }

  sendSoftwareBendAll(monoBendCurrent);
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

void initModularDefaults(Preset& p) {
  p.modularSchemaVersion = MODULAR_SCHEMA_VERSION;

  // MOD ENV is intentionally independent from SAM2695's per-voice envelope.
  // ENV1 starts as a short filter-style contour; ENV2/3 remain neutral.
  p.env[0] = {0, 48, 0, 32, ENV_CURVE_LOG, 1};
  p.env[1] = {0, 64, 0, 64, ENV_CURVE_LINEAR, 1};
  p.env[2] = {0, 64, 0, 64, ENV_CURVE_LINEAR, 1};

  // LFO1 mirrors legacy vibrato timing, but no modular route is enabled yet.
  p.lfo[0] = {LFO_WAVE_SINE, p.vibratoRate, p.vibratoDelay, 0, 0, 0};
  p.lfo[1] = {LFO_WAVE_TRIANGLE, 48, 0, 0, 0, 0};
  p.lfo[2] = {LFO_WAVE_TRIANGLE, 48, 0, 0, 0, 0};

  for (uint8_t i = 0; i < MOD_ROUTE_COUNT; ++i) {
    p.routes[i] = {MODSRC_NONE, MODDST_NONE, 0, 0};
  }
}

void migrateStoredPresetV1910(const StoredPresetV1910& oldPreset, Preset& out) {
  // Preset keeps the entire v1.10.9 object as a byte-compatible prefix.
  memcpy(&out, &oldPreset, sizeof(oldPreset));
  initModularDefaults(out);
}

void ensureModularPresetValid(Preset& p) {
  if (p.modularSchemaVersion != MODULAR_SCHEMA_VERSION) {
    initModularDefaults(p);
  }
}

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
  initModularDefaults(p);
  return p;
}

Preset makeGmQuickPreset(uint8_t slot) {
  Preset p{};
  const GmQuickPreset& g = GM_QUICK_PRESETS[slot % PRESETS_PER_BANK];
  strncpy(p.name, g.name, sizeof(p.name) - 1);

  // OSC1..3 are deliberately silent. The audible GM source is GM_CH.
  for (uint8_t i = 0; i < 3; ++i) {
    p.osc[i] = {0, 0, 0, 0, 0, 0, 0, 64};
  }

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
  initModularDefaults(p);
  return p;
}

Preset makeDefaultPreset(uint8_t index) {
  const uint8_t gmStart = GM_BANK_INDEX * PRESETS_PER_BANK;
  if (index >= gmStart && index < PRESET_COUNT) {
    return makeGmQuickPreset(index - gmStart);
  }

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
  initModularDefaults(p);
  return p;
}

void presetKey(uint8_t index, char* out, size_t outSize) {
  snprintf(out, outSize, "p%02u", index);
}

void gmLayerKey(uint8_t index, char* out, size_t outSize) {
  snprintf(out, outSize, "g%02u", index);
}

GmLayerState defaultGmLayer(uint8_t index) {
  GmLayerState state = {GM_LAYER_VERSION, 0, 0, 0, 64};
  const uint8_t gmStart = GM_BANK_INDEX * PRESETS_PER_BANK;

  if (index >= gmStart && index < PRESET_COUNT) {
    uint8_t slot = index - gmStart;
    state.program = GM_QUICK_PRESETS[slot].program;
    state.level = 127;
  }

  return state;
}

void loadGmLayerData(uint8_t index, GmLayerState& out) {
  out = defaultGmLayer(index);

  char key[8];
  gmLayerKey(index, key, sizeof(key));
  if (prefs.getBytesLength(key) != sizeof(GmLayerState)) return;

  GmLayerState stored{};
  prefs.getBytes(key, &stored, sizeof(stored));
  if (stored.version != GM_LAYER_VERSION) return;

  stored.program &= 0x7F;
  stored.level &= 0x7F;
  stored.transpose = constrain((int)stored.transpose, -48, 48);
  stored.pan &= 0x7F;
  out = stored;
}

void saveGmLayerData(uint8_t index, const GmLayerState& state) {
  char key[8];
  gmLayerKey(index, key, sizeof(key));
  prefs.putBytes(key, &state, sizeof(state));
}

bool allOscillatorsSilent(const Preset& p) {
  return p.osc[0].level == 0 && p.osc[1].level == 0 && p.osc[2].level == 0;
}

void recoverAccidentallySilentFactoryPreset(uint8_t index, Preset& out) {
  if (index >= LEGACY_PATCH_COUNT || !allOscillatorsSilent(out)) return;

  Preset factory = makeDefaultPreset(index);
  bool looksFactory =
      out.name[0] == '\0' ||
      strncmp(out.name, factory.name, sizeof(out.name)) == 0;

  if (looksFactory) out = factory;
}

void loadPresetData(uint8_t index, Preset& out) {
  out = makeDefaultPreset(index);

  char key[8];
  presetKey(index, key, sizeof(key));

  size_t len = prefs.getBytesLength(key);
  if (len == sizeof(Preset)) {
    prefs.getBytes(key, &out, sizeof(Preset));
    ensureModularPresetValid(out);
    recoverAccidentallySilentFactoryPreset(index, out);
    return;
  }

  if (len == sizeof(StoredPresetV1910)) {
    StoredPresetV1910 oldPreset{};
    prefs.getBytes(key, &oldPreset, sizeof(oldPreset));
    migrateStoredPresetV1910(oldPreset, out);
    recoverAccidentallySilentFactoryPreset(index, out);
  }
}

void saveCurrentPreset() {
  char key[8];
  presetKey(currentPresetIndex(), key, sizeof(key));
  prefs.putBytes(key, &currentPreset, sizeof(Preset));
  saveGmLayerData(currentPresetIndex(), currentGmLayer);
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
  // Keep the static NRPN cutoff at its neutral center. The live GS depth
  // parameters sweep from maximum negative through neutral to maximum positive,
  // so the same 0..127 knob can test the widest range the internal TVF exposes.
  // Resonance remains on the SAM2695 NRPN path.
  for (uint8_t i = 0; i < 3; ++i) {
    synth.setTvf(OSC_CH[i], 64, oscEffectiveRes(i));
  }
  sendLiveCutoffAll();
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
  return keyboardSustain;
}

void applyPedalsAll() {
  uint8_t sustainValue = effectiveSustain() ? 127 : 0;
  uint8_t softValue = currentPreset.soft ? 127 : 0;

  for (uint8_t i = 0; i < 3; ++i) {
    sendCC(OSC_CH[i], 64, sustainValue);
    sendCC(OSC_CH[i], 67, softValue);
  }
  sendCC(GM_CH, 64, sustainValue);
  sendCC(GM_CH, 67, softValue);
}

void applyMonoModeAll() {
  // SAM2695 supports MIDI mono/poly channel mode directly.
  for (uint8_t i = 0; i < 3; ++i) {
    if (currentPreset.mono) {
      sendCC(OSC_CH[i], 126, 1);  // Mono Mode On
    } else {
      sendCC(OSC_CH[i], 127, 0);  // Poly Mode On
    }
  }
}

void applyGlideTimeAll() {
  // CC5 only. Do not resend mono/poly or portamento switch while the
  // performer is simply moving the GLIDE TIME knob.
  for (uint8_t i = 0; i < 3; ++i) {
    sendCC(OSC_CH[i], 5, currentPreset.glideTime);
  }
}

void applyPortamentoSwitchAll() {
  for (uint8_t i = 0; i < 3; ++i) {
    sendCC(OSC_CH[i], 65, currentPreset.glide ? 127 : 0);
  }
}

void applyModeAndGlideAll() {
  if (softwareMonoVoiceActive()) {
    // DinMeter owns mono note priority whenever GLIDE or LEGATO is active.
    // Keep SAM2695 internally poly with native portamento OFF; only DinMeter
    // decides whether pitch glides and whether the envelope is retriggered.
    for (uint8_t i = 0; i < 3; ++i) {
      sendCC(OSC_CH[i], 127, 0);  // Poly Mode On internally
      sendCC(OSC_CH[i], 65, 0);   // native Portamento Off
      setPitchBendRange(OSC_CH[i], SOFTWARE_GLIDE_BEND_RANGE);
    }
    sendCC(GM_CH, 127, 0);
    sendCC(GM_CH, 65, 0);
    setPitchBendRange(GM_CH, SOFTWARE_GLIDE_BEND_RANGE);
    resetSoftwareBend(true);
    return;
  }

  applyMonoModeAll();
  applyGlideTimeAll();
  applyPortamentoSwitchAll();
  for (uint8_t i = 0; i < 3; ++i) {
    setPitchBendRange(OSC_CH[i], 2);
  }
  sendCC(GM_CH, 127, 0);
  sendCC(GM_CH, 65, 0);
  setPitchBendRange(GM_CH, 2);
  resetSoftwareBend(true);
}

void applyOscStatic(uint8_t oscIndex) {
  OscState& o = currentPreset.osc[oscIndex];
  uint8_t ch = OSC_CH[oscIndex];

  sendCC(ch, 0, o.bank);
  sendCC(ch, 32, 0);
  sendProgram(ch, o.program);
  delay(2);

  configureLiveCutoffController(ch);

  setPitchBendRange(ch, softwareMonoVoiceActive() ? SOFTWARE_GLIDE_BEND_RANGE : 2);
  synth.setTuning(ch, fineTuneValueFromCents(o.detune), 64);
  synth.setPan(ch, o.pan);
  sendLiveAmpOsc(oscIndex, true);

  // Hidden legacy chorus retained to preserve old patch character.
  sendCC(ch, 81, currentPreset.legacyChorusProgram);
  sendCC(ch, 93, currentPreset.legacyChorusSend);
}

void applyGmLayerStatic() {
  // A dedicated bank-0 GM layer that can be mixed with OSC1..3.
  sendCC(GM_CH, 120, 0);
  sendCC(GM_CH, 121, 0);
  sendCC(GM_CH, 0, 0);
  sendCC(GM_CH, 32, 0);
  sendProgram(GM_CH, currentGmLayer.program);
  delay(2);
  sendLiveAmpGm(true);
  synth.setPan(GM_CH, currentGmLayer.pan);
  setPitchBendRange(GM_CH, softwareMonoVoiceActive() ? SOFTWARE_GLIDE_BEND_RANGE : 2);
  sendCC(GM_CH, 127, 0); // DinMeter note manager owns mono behavior
  sendCC(GM_CH, 65, 0);  // software glide owns portamento
  sendPitchBendRaw(GM_CH, 0, 64);
  sendCC(GM_CH, 64, effectiveSustain() ? 127 : 0);
  sendCC(GM_CH, 67, currentPreset.soft ? 127 : 0);
}

void silenceSynthOnly() {
  for (uint8_t i = 0; i < 3; ++i) {
    sendCC(OSC_CH[i], 64, 0);
    sendCC(OSC_CH[i], 123, 0);
    sendCC(OSC_CH[i], 120, 0);
  }
  sendCC(GM_CH, 64, 0);
  sendCC(GM_CH, 123, 0);
  sendCC(GM_CH, 120, 0);
  currentMonoNote = -1;
  monoAnchorNote = -1;
  monoTargetNote = -1;
  resetSoftwareBend(false);
}

void clearHeldState() {
  memset(heldInput, 0, sizeof(heldInput));
  memset(heldVelocity, 0, sizeof(heldVelocity));
  heldCount = 0;
  currentMonoNote = -1;
  monoAnchorNote = -1;
  monoTargetNote = -1;
  configAuditionEnabled = false;
  configAuditionNoteOn = false;
  configAuditionStep = 0;
  resetSoftwareBend(false);
  resetModulationRuntime();
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

void sendGmLayerNote(uint8_t messageType, uint8_t inputNote, uint8_t velocity) {
  if (currentGmLayer.level == 0) return;

  int note = (int)inputNote + currentGmLayer.transpose;
  if (note < 0 || note > 127) return;

  sendMidi3(messageType | (GM_CH & 0x0F), note & 0x7F, velocity & 0x7F);
}

void sendLayeredNote(uint8_t messageType, uint8_t inputNote, uint8_t velocity) {
  for (uint8_t i = 0; i < 3; ++i) {
    sendOscNote(i, messageType, inputNote, velocity);
  }
  sendGmLayerNote(messageType, inputNote, velocity);
}

void rebuildHeldNotes() {
  currentMonoNote = -1;
  monoAnchorNote = -1;
  monoTargetNote = -1;
  resetSoftwareBend(true);

  if (softwareMonoVoiceActive()) {
    int16_t n = lastHeldNote();
    if (n >= 0) {
      monoAnchorNote = n;
      monoTargetNote = n;
      currentMonoNote = n;
      sendLayeredNote(0x90, (uint8_t)n, heldVelocity[n]);
    }
    return;
  }

  if (currentPreset.mono && !currentPreset.legato) {
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
      currentMonoNote = n;
    }
  }
}

void applyCurrentPresetToSynth(bool rebuildNotes = true) {
  silenceSynthOnly();
  resetModulationRuntime();
  invalidateLiveCutoffCache();
  invalidateLiveAmpCache();

  applyMasterVolume();

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
  applyGmLayerStatic();

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
  configAuditionEnabled = false;
  configAuditionNoteOn = false;
  recoveringFromPanic = true;

  snprintf(overlayTitle, sizeof(overlayTitle), "PANIC");
  snprintf(overlaySub, sizeof(overlaySub), "%s", reason ? reason : "RESET");
  overlayActive = true;
  overlayUntil = millis() + 800;
  screenDirty = true;
}

void retriggerSoftwareMonoEnvelope(uint8_t velocity) {
  if (monoAnchorNote < 0) return;

  // Keep the current Pitch Bend value unchanged. Re-triggering the same
  // anchor note therefore restarts the envelope at the currently heard pitch
  // instead of jumping back to the anchor pitch.
  sendLayeredNote(0x80, (uint8_t)monoAnchorNote, 0);
  sendLayeredNote(0x90, (uint8_t)monoAnchorNote, velocity);
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

  const bool phraseStart = (heldCount == 1);
  triggerModEnvelopes(phraseStart);
  triggerModLfos(phraseStart);

  if (!currentPreset.mono) {
    sendLayeredNote(0x90, note, velocity);
    return;
  }

  if (softwareMonoVoiceActive()) {
    if (monoAnchorNote < 0) {
      // First note of the phrase: one real oscillator NoteOn.
      resetSoftwareBend(true);
      monoAnchorNote = note;
      monoTargetNote = note;
      currentMonoNote = note;
      sendLayeredNote(0x90, note, velocity);
      return;
    }

    // LEGATO controls only envelope retriggering.
    // GLIDE controls only pitch transition time.
    if (!currentPreset.legato) {
      retriggerSoftwareMonoEnvelope(velocity);
    }

    currentMonoNote = note;
    startSoftwareGlideTo(note);
    return;
  }

  // MONO only: classic hard-retrigger single-note behavior.
  if (currentMonoNote < 0) {
    sendLayeredNote(0x90, note, velocity);
    currentMonoNote = note;
    return;
  }

  uint8_t oldNote = (uint8_t)currentMonoNote;
  sendLayeredNote(0x80, oldNote, 0);
  sendLayeredNote(0x90, note, velocity);
  currentMonoNote = note;
}

void handleNoteOff(uint8_t note, uint8_t velocity) {
  if (!heldInput[note]) {
    if (recoveringFromPanic) return;

    // Existing stuck-note protection. This is independent of the mono voice
    // manager and can be relaxed separately if necessary.
    panicAll("NOTE DESYNC");
    return;
  }

  heldInput[note] = false;
  heldVelocity[note] = 0;
  removeNoteOrder(note);

  if (heldCount == 0) {
    releaseModEnvelopes();
  }

  if (!currentPreset.mono) {
    sendLayeredNote(0x80, note, velocity);
    return;
  }

  if (softwareMonoVoiceActive()) {
    int16_t next = lastHeldNote();

    if (next < 0) {
      // End of phrase. Release the only real note at its current pitch.
      // Do not reset Pitch Bend until the next phrase; resetting immediately
      // would bend the release tail back toward the anchor pitch.
      if (monoAnchorNote >= 0) {
        sendLayeredNote(0x80, (uint8_t)monoAnchorNote, velocity);
      }
      monoAnchorNote = -1;
      monoTargetNote = -1;
      currentMonoNote = -1;
      monoGlideDurationMs = 0;
      return;
    }

    // Releasing a key that is not the current last-note-priority target only
    // changes the held-note stack.
    if (currentMonoNote != note) return;

    // Fall back to the newest still-held note.
    // With LEGATO OFF the envelope is re-triggered; with LEGATO ON it remains
    // continuous. GLIDE independently decides whether this move is instant.
    if (!currentPreset.legato) {
      retriggerSoftwareMonoEnvelope(heldVelocity[next]);
    }

    currentMonoNote = next;
    startSoftwareGlideTo(next);
    return;
  }

  // MONO only.
  if (currentMonoNote != note) return;

  int16_t next = lastHeldNote();
  if (next < 0) {
    sendLayeredNote(0x80, note, velocity);
    currentMonoNote = -1;
    return;
  }

  sendLayeredNote(0x80, note, 0);
  sendLayeredNote(0x90, (uint8_t)next, heldVelocity[next]);
  currentMonoNote = next;
}

// ======================================================================
// CONFIG audition generator
// ======================================================================

void stopConfigAuditionNote() {
  if (!configAuditionNoteOn) return;

  sendLayeredNote(0x80, configAuditionNote, 0);
  configAuditionNoteOn = false;

  // The audition generator owns its own test phrase. If no real keyboard note
  // is being held, let the modular envelopes enter release normally.
  if (heldCount == 0) releaseModEnvelopes();
}

void startConfigAuditionNote(uint32_t nowMs) {
  configAuditionNote =
      CONFIG_AUDITION_NOTES[configAuditionStep % CONFIG_AUDITION_NOTE_COUNT];

  // Treat every generated note as a fresh phrase for envelope auditioning.
  // LFO modules with RTR off keep their phase; RTR on restarts as expected.
  triggerModEnvelopes(true);
  triggerModLfos(true);
  sendLayeredNote(0x90, configAuditionNote, CONFIG_AUDITION_VELOCITY);

  configAuditionNoteOn = true;
  configAuditionChangedAt = nowMs;
}

void setConfigAuditionEnabled(bool enabled) {
  if (enabled == configAuditionEnabled) return;

  if (!enabled) {
    stopConfigAuditionNote();
    configAuditionEnabled = false;
    configAuditionStep = 0;
    screenDirty = true;
    return;
  }

  configAuditionEnabled = true;
  configAuditionStep = 0;
  startConfigAuditionNote(millis());
  screenDirty = true;
}

void updateConfigAudition() {
  // CONFIG audition never leaks into PERFORMANCE, menus, Wi-Fi maintenance,
  // or other modal screens.
  bool allowed =
      configMode &&
      !wifiMaintActive() &&
      !systemMenuActive &&
      !wifiSelectActive &&
      !wifiSetupMenuActive &&
      !wifiScanListActive &&
      !textEditorActive &&
      !wifiDeleteListActive &&
      !wifiDeleteConfirm &&
      !saveDialog &&
      !maintenanceConfirm &&
      !systemInfoActive;

  if (!allowed) {
    if (configAuditionEnabled || configAuditionNoteOn) {
      setConfigAuditionEnabled(false);
    }
    return;
  }

  if (!configAuditionEnabled) return;

  uint32_t nowMs = millis();
  uint32_t elapsed = nowMs - configAuditionChangedAt;

  if (configAuditionNoteOn) {
    if (elapsed < CONFIG_AUDITION_NOTE_MS) return;

    stopConfigAuditionNote();
    configAuditionChangedAt = nowMs;
    return;
  }

  if (elapsed < CONFIG_AUDITION_GAP_MS) return;

  configAuditionStep = (configAuditionStep + 1) % CONFIG_AUDITION_NOTE_COUNT;
  startConfigAuditionNote(nowMs);
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
  if (currentGmLayer.level > 0) sendCC(GM_CH, cc, value);
}

// Used by incoming MIDI CC7 so the keyboard controls the same VOLUME state
// and UI path as the PERFORMANCE 8Angle volume knob.
void popupNumber(const char* name, int value);
void updatePickupLed(uint8_t knob);

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

    if (currentGmLayer.level > 0) {
      int shifted = (int)(packet[2] & 0x7F) + currentGmLayer.transpose;
      if (shifted >= 0 && shifted <= 127) {
        sendMidi3(0xA0 | GM_CH, shifted, packet[3] & 0x7F);
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

    if (cc == 7) {
      // Keyboard CC7 and PERFORMANCE 8Angle VOL are the same logical control.
      // Update the shared value so both sound and on-screen VOL follow CC7.
      masterVolume = value;
      applyMasterVolume();

      // The physical knob may now be at a different position. Re-arm pickup
      // only for VOL so touching the knob does not cause an abrupt jump.
      pickup[0].target = masterVolume;
      pickup[0].active = false;
      updatePickupLed(0);

      popupNumber("VOLUME", masterVolume);
      screenDirty = true;
      return;
    }

    // Keyboard Program/Bank/our internal FX/glide settings are blocked.
    // CC16/17 are reserved internally for the two live TVF cutoff controllers.
    if (cc == 0 || cc == 32 || cc == 5 || cc == 65 ||
        cc == LIVE_FILTER_CC1 || cc == LIVE_FILTER_CC2 ||
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
    if (currentGmLayer.level > 0) sendMidi2(0xD0 | GM_CH, packet[2] & 0x7F);
    return;
  }

  if (cin == 0xE) {
    // Software portamento owns the base Pitch Bend while a legato phrase is
    // active. Outside that mode, preserve the external wheel and add modular
    // pitch movement on top of it.
    if (softwareMonoVoiceActive()) return;

    performanceBendCurrent =
        (int16_t)(((uint16_t)(packet[3] & 0x7F) << 7) |
                  (uint16_t)(packet[2] & 0x7F));
    sendSoftwareBendAll(performanceBendCurrent);
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
void drawHazardStripe(int y, int h, int x0 = 0, int width = -1);
void clearScreen();
void resetMaintenanceUiCache();
void enterUsbFlashBootloader();
void drawUsbFlashConfirm();
void enterMaintenanceMode();

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
  loadGmLayerData(currentPresetIndex(), currentGmLayer);

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
  uint8_t page = configMode ? configPage : (uint8_t)PAGE_PERF;

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
      case 2: return currentPreset.reverb;
      case 3: return currentPreset.vibratoRate;
      case 4: return currentPreset.vibratoDepth;
      default: return 0;
    }
  }

  if (page == PAGE_VOICE_ENV) {
    switch (knob) {
      case 0: return currentPreset.attack;
      case 1: return currentPreset.decay;
      case 2: return currentPreset.release;
      default: return 0;
    }
  }

  if (page == PAGE_OSC) {
    const OscState& o = currentPreset.osc[selectedOsc];
    switch (knob) {
      case 0: return (uint8_t)((waveIndexForOsc(selectedOsc) * 127) / 7);
      case 1: return o.level;
      case 2: return mapSignedTo127(o.transpose, -48, 48);
      case 3: return mapSignedTo127(o.detune, -50, 50);
      case 4: return mapSignedTo127(o.cutoffTrim, -63, 63);
      case 5: return mapSignedTo127(o.resonanceTrim, -63, 63);
      case 6: return o.pan;
      case 7: return 0;
    }
  }

  if (page == PAGE_GM) {
    switch (knob) {
      case 0: return currentGmLayer.program;
      case 1: return currentGmLayer.level;
      case 2: return mapSignedTo127(currentGmLayer.transpose, -48, 48);
      case 3: return currentGmLayer.pan;
      default: return 0;
    }
  }

  if (page == PAGE_ENV) {
    const EnvelopeState& env = currentPreset.env[selectedEnv];
    switch (knob) {
      case 0: return env.attack;
      case 1: return env.decay;
      case 2: return env.sustain;
      case 3: return env.release;
      case 4: return (uint8_t)((min((int)env.curve, 2) * 127) / 2);
      case 5: return env.retrigger ? 127 : 0;
      default: return 0;
    }
  }

  if (page == PAGE_LFO) {
    const LfoState& lfo = currentPreset.lfo[selectedLfo];
    switch (knob) {
      case 0: return (uint8_t)((min((int)lfo.waveform, 5) * 127) / 5);
      case 1: return lfo.rate;
      case 2: return lfo.delay;
      case 3: return lfo.fade;
      case 4: return lfo.retrigger ? 127 : 0;
      case 5: return lfo.phase;
      default: return 0;
    }
  }

  if (page == PAGE_ROUTE) {
    const ModRoute& route = currentPreset.routes[selectedRoute];
    switch (knob) {
      case 0: {
        uint8_t src = (route.source <= MODSRC_LFO3) ? route.source : (uint8_t)MODSRC_NONE;
        return (uint8_t)(((uint16_t)src * 127) / MODSRC_LFO3);
      }
      case 1: {
        uint8_t dst = route.destination <= MODDST_AMP
                    ? route.destination
                    : (uint8_t)MODDST_NONE;
        return (uint8_t)(((uint16_t)dst * 127) / MODDST_AMP);
      }
      case 2: return mapSignedTo127(route.amount, -127, 127);
      case 3: return route.enabled ? 127 : 0;
      default: return 0;
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
  uint8_t page = configMode ? configPage : (uint8_t)PAGE_PERF;

  if (page == PAGE_FILTER_ENV) return knob >= 5;
  if (page == PAGE_VOICE_ENV) return knob >= 3;
  if (page == PAGE_OSC) return knob == 7;
  if (page == PAGE_GM) return knob >= 4;
  if (page == PAGE_ENV) return knob >= 6;
  if (page == PAGE_LFO) return knob >= 6;
  if (page == PAGE_ROUTE) return knob >= 4;

  return false;
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
      applyMasterVolume();
      popupNumber("VOLUME", masterVolume);
      return;

    case 1:
      if (currentPreset.cutoff != v) {
        currentPreset.cutoff = v;
        sendLiveCutoffAll();
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
      popupNumber("VOICE ATK", v);
      return;

    case 4:
      if (currentPreset.release != v) {
        currentPreset.release = v;
        applyEnvelopeAll();
        markModified();
      }
      popupNumber("VOICE REL", v);
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
        applyGlideTimeAll();
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
      if (changed) sendLiveCutoffAll();
      break;

    case 1:
      changed = currentPreset.resonance != v;
      currentPreset.resonance = v;
      if (changed) applyFilterAll();
      break;

    case 2:
      changed = currentPreset.reverb != v;
      currentPreset.reverb = v;
      if (v > 0) currentPreset.reverbEnabled = 1;
      if (changed) applyReverbAll();
      break;

    case 3:
      changed = currentPreset.vibratoRate != v;
      currentPreset.vibratoRate = v;
      if (changed) applyVibratoAll();
      break;

    case 4:
      changed = currentPreset.vibratoDepth != v;
      currentPreset.vibratoDepth = v;
      if (v > 0) currentPreset.vibratoEnabled = 1;
      if (changed) applyVibratoAll();
      break;

    default:
      return;
  }

  if (changed) markModified();
}

void applyVoiceEnvPageKnob(uint8_t knob, uint8_t v) {
  bool changed = false;

  switch (knob) {
    case 0:
      changed = currentPreset.attack != v;
      currentPreset.attack = v;
      break;
    case 1:
      changed = currentPreset.decay != v;
      currentPreset.decay = v;
      break;
    case 2:
      changed = currentPreset.release != v;
      currentPreset.release = v;
      break;
    default:
      return;
  }

  if (changed) {
    applyEnvelopeAll();
    markModified();
  }
}

void applyOscPageKnob(uint8_t knob, uint8_t v) {
  uint8_t oi = selectedOsc;
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
        sendLiveAmpOsc(oi, true);
        changed = true;
      }
      break;

    case 2: {
      uint8_t idx = (uint8_t)(((uint16_t)v * 9) / 128);
      if (idx > 8) idx = 8;
      int8_t trans = ((int)idx - 4) * 12;
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
        sendLiveCutoff(oi);
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

void applyGmPageKnob(uint8_t knob, uint8_t v) {
  bool changed = false;

  switch (knob) {
    case 0:
      if (currentGmLayer.program != v) {
        currentGmLayer.program = v;
        changed = true;
        reapplyAndRebuild();

        char title[16];
        snprintf(title, sizeof(title), "GM %03u", v + 1);
        setParamPopup(title, gmProgramName(v));
      }
      break;

    case 1: {
      uint8_t oldLevel = currentGmLayer.level;
      if (oldLevel != v) {
        currentGmLayer.level = v;
        changed = true;
        if ((oldLevel == 0) != (v == 0)) {
          reapplyAndRebuild();
        } else {
          sendLiveAmpGm(true);
        }
      }
      break;
    }

    case 2: {
      uint8_t idx = (uint8_t)(((uint16_t)v * 9) / 128);
      if (idx > 8) idx = 8;
      int8_t trans = ((int)idx - 4) * 12;
      if (currentGmLayer.transpose != trans) {
        currentGmLayer.transpose = trans;
        changed = true;
        reapplyAndRebuild();
      }
      break;
    }

    case 3:
      if (currentGmLayer.pan != v) {
        currentGmLayer.pan = v;
        synth.setPan(GM_CH, currentGmLayer.pan);
        changed = true;
      }
      break;

    default:
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

void applyEnvPageKnob(uint8_t knob, uint8_t v) {
  EnvelopeState& env = currentPreset.env[selectedEnv];
  bool changed = false;

  switch (knob) {
    case 0: changed = env.attack != v;  env.attack = v; break;
    case 1: changed = env.decay != v;   env.decay = v; break;
    case 2: changed = env.sustain != v; env.sustain = v; break;
    case 3: changed = env.release != v; env.release = v; break;

    case 4: {
      uint8_t curve = (uint8_t)(((uint16_t)v * 3) / 128);
      if (curve > ENV_CURVE_LOG) curve = ENV_CURVE_LOG;
      changed = env.curve != curve;
      env.curve = curve;
      break;
    }

    case 5: {
      uint8_t retrigger = v >= 64 ? 1 : 0;
      changed = env.retrigger != retrigger;
      env.retrigger = retrigger;
      break;
    }

    default:
      return;
  }

  if (changed) {
    markModified();
    refreshModulationOutputs(false);
  }
}

void applyLfoPageKnob(uint8_t knob, uint8_t v) {
  LfoState& lfo = currentPreset.lfo[selectedLfo];
  bool changed = false;

  switch (knob) {
    case 0: {
      uint8_t wave = (uint8_t)(((uint16_t)v * 6) / 128);
      if (wave > LFO_WAVE_RANDOM) wave = LFO_WAVE_RANDOM;
      changed = lfo.waveform != wave;
      lfo.waveform = wave;
      break;
    }
    case 1: changed = lfo.rate != v;  lfo.rate = v; break;
    case 2: changed = lfo.delay != v; lfo.delay = v; break;
    case 3: changed = lfo.fade != v;  lfo.fade = v; break;
    case 4: {
      uint8_t retrigger = v >= 64 ? 1 : 0;
      changed = lfo.retrigger != retrigger;
      lfo.retrigger = retrigger;
      break;
    }
    case 5:
      changed = lfo.phase != v;
      lfo.phase = v;
      break;
    default:
      return;
  }

  if (changed) {
    markModified();
    resetLfoRuntime();
    refreshModulationOutputs(false);
  }
}

void applyRoutePageKnob(uint8_t knob, uint8_t v) {
  ModRoute& route = currentPreset.routes[selectedRoute];
  bool changed = false;

  switch (knob) {
    case 0: {
      uint8_t source = (uint8_t)(((uint16_t)v * 7) / 128);
      if (source > MODSRC_LFO3) source = MODSRC_LFO3;
      changed = route.source != source;
      route.source = source;
      break;
    }

    case 1: {
      uint8_t destination = (uint8_t)(((uint16_t)v * 4) / 128);
      if (destination > MODDST_AMP) destination = MODDST_AMP;
      changed = route.destination != destination;
      route.destination = destination;
      break;
    }

    case 2: {
      int8_t amount = (int8_t)map127ToSigned(v, -127, 127);
      changed = route.amount != amount;
      route.amount = amount;
      break;
    }

    case 3: {
      uint8_t enabled = v >= 64 ? 1 : 0;
      changed = route.enabled != enabled;
      route.enabled = enabled;
      break;
    }

    default:
      return;
  }

  if (changed) {
    refreshModulationOutputs(true);
    markModified();
  }
}

void applyKnobValue(uint8_t knob, uint8_t value) {
  uint8_t page = configMode ? configPage : (uint8_t)PAGE_PERF;

  if (page == PAGE_PERF) {
    applyPerformanceKnob(knob, value);
  } else if (page == PAGE_FILTER_ENV) {
    applyFilterPageKnob(knob, value);
  } else if (page == PAGE_VOICE_ENV) {
    applyVoiceEnvPageKnob(knob, value);
  } else if (page == PAGE_OSC) {
    applyOscPageKnob(knob, value);
  } else if (page == PAGE_GM) {
    applyGmPageKnob(knob, value);
  } else if (page == PAGE_ENV) {
    applyEnvPageKnob(knob, value);
  } else if (page == PAGE_LFO) {
    applyLfoPageKnob(knob, value);
  } else if (page == PAGE_ROUTE) {
    applyRoutePageKnob(knob, value);
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

  if (!newMode) {
    setConfigAuditionEnabled(false);
  }

  configMode = newMode;
  paramPopupActive = false;

  if (configMode) {
    configPage = PAGE_PERF;
    snprintf(overlayTitle, sizeof(overlayTitle), "CONFIG MODE");
    snprintf(overlaySub, sizeof(overlaySub), "PAGE 01 / %02u", PAGE_COUNT);
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
  if (wifiMaintActive() || systemMenuActive || wifiSelectActive || wifiSetupMenuActive ||
      wifiScanListActive || textEditorActive || wifiDeleteListActive || wifiDeleteConfirm || saveDialog ||
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

  bool states[6] = {
    (bool)currentPreset.mono,
    (bool)currentPreset.glide,
    (bool)currentPreset.legato,
    (bool)currentPreset.soft,
    (bool)currentPreset.reverbEnabled,
    (bool)currentPreset.vibratoEnabled
  };

  for (uint8_t i = 0; i < 6; ++i) {
    if (states[i]) setLogicalByteLed(i, 0x00FF40, 70);
    else           setLogicalByteLed(i, 0x302000, 24);
  }

  // PANIC is an action, always red. TEST is an ON/OFF audition state.
  setLogicalByteLed(6, 0xFF0000, 55);
  setLogicalByteLed(7,
                    configAuditionEnabled ? 0x00FF40 : 0x302000,
                    configAuditionEnabled ? 70 : 24);
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

    case 3: // SOFT
      currentPreset.soft = !currentPreset.soft;
      applyPedalsAll();
      markModified();
      break;

    case 4: // REVERB ENABLE
      currentPreset.reverbEnabled = !currentPreset.reverbEnabled;
      applyReverbAll();
      markModified();
      break;

    case 5: // VIBRATO ENABLE
      currentPreset.vibratoEnabled = !currentPreset.vibratoEnabled;
      applyVibratoAll();
      markModified();
      break;

    case 6: // PANIC
      panicAll("MANUAL");
      break;

    case 7: // CONFIG TEST / audition generator
      setConfigAuditionEnabled(!configAuditionEnabled);
      break;
  }

  updateByteLeds();
}


static const char* TEXT_GROUPS[4] = {
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
  "abcdefghijklmnopqrstuvwxyz",
  "0123456789",
  "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
};

const char* textEditorChars() {
  return TEXT_GROUPS[textEditorGroup < 4 ? textEditorGroup : 0];
}

void syncByteDebounceForMapping(bool leftToRight) {
  uint32_t nowMs = millis();
  for (uint8_t physical = 0; physical < 8; ++physical) {
    uint8_t logical = leftToRight ? (7 - physical)
                                  : (configMode ? (7 - physical) : physical);
    bool raw = byteButton.getSwitchStatus(physical) != 0;
    byteRaw[logical] = raw;
    byteStable[logical] = raw;
    byteChangedAt[logical] = nowMs;
  }
}

void updateTextEditorByteLeds() {
  for (uint8_t i = 0; i < 4; ++i) {
    setLogicalByteLed(i,
                      i == textEditorGroup ? 0x00FF50 : 0x302000,
                      i == textEditorGroup ? 75 : 24);
  }
  setLogicalByteLed(4, 0x2050A0, 42);  // SPACE
  setLogicalByteLed(5, 0x805000, 48);  // DELETE
  setLogicalByteLed(6, 0xA02020, 52);  // CANCEL
  setLogicalByteLed(7, 0x00A040, 58);  // OK
}

void restoreByteAfterTextEditor() {
  syncByteDebounceForMapping(false);
  updateByteLeds();
}

void beginTextEditor(TextEditorPurpose purpose,
                     const String& context,
                     const String& initial,
                     uint8_t maxLen) {
  textEditorPurpose = purpose;
  textEditorContext = context;
  textEditorBuffer = initial;
  textEditorMaxLen = maxLen;
  textEditorMessage = "";
  textEditorGroup = 0;
  textEditorCharIndex = 0;
  textEditorActive = true;
  wifiScanListActive = false;
  wifiSetupMenuActive = false;

  syncByteDebounceForMapping(true);
  updateTextEditorByteLeds();
  screenDirty = true;
}

void cancelTextEditor() {
  textEditorActive = false;
  textEditorPurpose = TextEditorPurpose::NONE;
  textEditorMessage = "";
  wifiScanListActive = true;
  restoreByteAfterTextEditor();
  screenDirty = true;
}

void appendTextEditorChar(char c) {
  if (textEditorBuffer.length() >= textEditorMaxLen) {
    textEditorMessage = "MAX LENGTH";
    screenDirty = true;
    return;
  }
  textEditorBuffer += c;
  textEditorMessage = "";
  screenDirty = true;
}

void appendCurrentTextEditorChar() {
  const char* chars = textEditorChars();
  size_t len = strlen(chars);
  if (len == 0) return;
  if (textEditorCharIndex >= len) textEditorCharIndex = 0;
  appendTextEditorChar(chars[textEditorCharIndex]);
}

void finishTextEditor() {
  if (textEditorPurpose == TextEditorPurpose::WIFI_PASSWORD) {
    if (textEditorBuffer.length() < 8 || textEditorBuffer.length() > 63) {
      textEditorMessage = "PASSWORD MUST BE 8-63";
      screenDirty = true;
      return;
    }

    if (!wifiMaintSaveCredential(textEditorContext, textEditorBuffer, true)) {
      textEditorMessage = "SAVE FAILED";
      screenDirty = true;
      return;
    }

    textEditorActive = false;
    textEditorPurpose = TextEditorPurpose::NONE;
    wifiScanListActive = false;
    restoreByteAfterTextEditor();
    enterMaintenanceMode();
    return;
  }

  // PRESET_NAME is intentionally reserved for reuse by the preset editor.
  textEditorMessage = "EDITOR PURPOSE NOT READY";
  screenDirty = true;
}

void handleTextEditorByte(uint8_t logical) {
  if (logical <= 3) {
    textEditorGroup = logical;
    textEditorCharIndex = 0;
    textEditorMessage = "";
    updateTextEditorByteLeds();
    screenDirty = true;
    return;
  }

  switch (logical) {
    case 4:
      appendTextEditorChar(' ');
      return;
    case 5:
      if (textEditorBuffer.length() > 0) {
        textEditorBuffer.remove(textEditorBuffer.length() - 1);
      }
      textEditorMessage = "";
      screenDirty = true;
      return;
    case 6:
      cancelTextEditor();
      return;
    case 7:
      finishTextEditor();
      return;
  }
}

void handleBytePress(uint8_t logical) {
  if (textEditorActive) {
    handleTextEditorByte(logical);
    return;
  }

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
  if (wifiMaintActive() || systemMenuActive || wifiSelectActive || wifiSetupMenuActive ||
      wifiScanListActive || wifiDeleteListActive || wifiDeleteConfirm || saveDialog ||
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
    uint8_t logical = (textEditorActive || configMode) ? (7 - physical) : physical;

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
  wifiSelectActive = false;
  wifiSetupMenuActive = false;
  wifiScanListActive = false;
  textEditorActive = false;
  textEditorPurpose = TextEditorPurpose::NONE;
  wifiDeleteListActive = false;
  wifiDeleteConfirm = false;
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
  wifiSelectActive = false;
  wifiSetupMenuActive = false;
  wifiScanListActive = false;
  textEditorActive = false;
  textEditorPurpose = TextEditorPurpose::NONE;
  wifiDeleteListActive = false;
  wifiDeleteConfirm = false;
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


void startWifiScanUi() {
  wifiSetupMenuActive = false;
  wifiScanListActive = false;
  textEditorActive = false;

  silenceSynthOnly();
  clearHeldState();
  keyboardSustain = false;

  // Draw immediately because Wi-Fi scanning is synchronous for a few seconds.
  M5.Display.fillScreen(C_BLACK);
  M5.Display.setTextWrap(false);
  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 28);
  M5.Display.print("SCANNING WIFI...");
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_WHITE, C_BLACK);
  M5.Display.setCursor(10, 62);
  M5.Display.print("2.4 GHz NETWORKS");
  M5.Display.setCursor(10, 82);
  M5.Display.print("PLEASE WAIT");

  wifiScanLastResult = wifiMaintScanNetworks();
  wifiScanIndex = 0;
  wifiScanListActive = true;
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
      systemMenuActive = false;
      wifiSetupMenuActive = true;
      wifiSetupMenuIndex = 0;
      screenDirty = true;
      return;

    case 3: // WIFI SELECT
      systemMenuActive = false;
      wifiSelectActive = true;
      {
        int preferred = wifiMaintPreferredProfile();
        wifiSelectIndex = (preferred >= 0) ? (uint8_t)(preferred + 1) : 0;
        uint8_t optionCount = wifiMaintProfileCount() + 1;
        if (wifiSelectIndex >= optionCount) wifiSelectIndex = 0;
      }
      screenDirty = true;
      return;

    case 4: // USB FLASH
      systemMenuActive = false;
      usbFlashConfirm = true;
      usbFlashChoiceYes = false;
      screenDirty = true;
      return;

    case 5: // SYSTEM INFO
      systemMenuActive = false;
      systemInfoActive = true;
      screenDirty = true;
      return;

    case 6: // EXIT
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
  snprintf(overlaySub, sizeof(overlaySub), "%02u/%02u %s",
           configPage + 1, PAGE_COUNT, PAGE_NAMES[configPage]);
  overlayActive = true;
  overlayUntil = millis() + 500;
  screenDirty = true;
}

void handleEncoderRotate(int detents) {
  if (detents == 0) return;

  if (wifiMaintActive()) {
    return;
  }

  if (textEditorActive) {
    const char* chars = textEditorChars();
    int count = (int)strlen(chars);
    if (count < 1) count = 1;
    int next = (int)textEditorCharIndex + detents;
    while (next < 0) next += count;
    while (next >= count) next -= count;
    textEditorCharIndex = (uint8_t)next;
    textEditorMessage = "";
    screenDirty = true;
    return;
  }

  if (wifiScanListActive) {
    int optionCount = (int)wifiMaintScanCount() + 3; // RESCAN / MANUAL-AP / BACK
    int next = (int)wifiScanIndex + detents;
    while (next < 0) next += optionCount;
    while (next >= optionCount) next -= optionCount;
    wifiScanIndex = (uint8_t)next;
    screenDirty = true;
    return;
  }

  if (wifiSetupMenuActive) {
    int next = (int)wifiSetupMenuIndex + detents;
    while (next < 0) next += 3;
    while (next >= 3) next -= 3;
    wifiSetupMenuIndex = (uint8_t)next;
    screenDirty = true;
    return;
  }

  if (wifiDeleteListActive) {
    int optionCount = (int)wifiMaintProfileCount() + 1; // profiles + BACK
    int next = (int)wifiDeleteIndex + detents;
    while (next < 0) next += optionCount;
    while (next >= optionCount) next -= optionCount;
    wifiDeleteIndex = (uint8_t)next;
    screenDirty = true;
    return;
  }

  if (wifiDeleteConfirm) {
    wifiDeleteChoiceYes = (detents > 0);
    screenDirty = true;
    return;
  }

  if (wifiSelectActive) {
    int optionCount = (int)wifiMaintProfileCount() + 1; // AUTO + saved profiles
    if (optionCount < 1) optionCount = 1;
    int next = (int)wifiSelectIndex + detents;
    while (next < 0) next += optionCount;
    while (next >= optionCount) next -= optionCount;
    wifiSelectIndex = (uint8_t)next;
    screenDirty = true;
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
      updateByteLeds();
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
    snprintf(overlaySub, sizeof(overlaySub), "%s", BANK_GROUP_NAMES[browseBank]);
    overlayActive = true;
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
  // In HOME WIFI maintenance, short press is the physical GitHub update action.
  // In AP/setup modes, short press keeps the old "exit" behavior.
  if (wifiMaintActive()) {
    if (wifiMaintUpdating()) return;

    if (wifiMaintMode() == WifiMaintMode::MAINT_STA) {
      if (wifiMaintGithubUpdateAvailable()) {
        wifiMaintStartGithubUpdate();
      } else {
        wifiMaintCheckLatestRelease();
      }
      screenDirty = true;
      return;
    }

    exitWifiRuntime();
    return;
  }

  if (textEditorActive) {
    appendCurrentTextEditorChar();
    return;
  }

  if (wifiScanListActive) {
    uint8_t count = wifiMaintScanCount();

    if (wifiScanIndex < count) {
      String ssid = wifiMaintScanSsid(wifiScanIndex);

      // Already-saved profiles keep their existing password when blank is supplied.
      // Open networks are stored with an empty password.
      if (wifiMaintScanSaved(wifiScanIndex) || !wifiMaintScanSecured(wifiScanIndex)) {
        if (wifiMaintSaveCredential(ssid, "", true)) {
          wifiScanListActive = false;
          enterMaintenanceMode();
        } else {
          wifiScanLastResult = -99;
          screenDirty = true;
        }
        return;
      }

      beginTextEditor(TextEditorPurpose::WIFI_PASSWORD, ssid, "", 63);
      return;
    }

    if (wifiScanIndex == count) {
      startWifiScanUi();
      return;
    }

    if (wifiScanIndex == count + 1) {
      wifiScanListActive = false;
      enterWifiSetupMode(); // hidden/manual network browser fallback
      return;
    }

    wifiScanListActive = false;
    wifiSetupMenuActive = true;
    wifiSetupMenuIndex = 0;
    screenDirty = true;
    return;
  }

  if (wifiDeleteConfirm) {
    if (wifiDeleteChoiceYes) {
      wifiMaintDeleteProfile(wifiDeleteIndex);
      uint8_t count = wifiMaintProfileCount();
      if (wifiDeleteIndex > count) wifiDeleteIndex = count;
      wifiDeleteConfirm = false;
      wifiDeleteListActive = true;
    } else {
      wifiDeleteConfirm = false;
      wifiDeleteListActive = true;
    }
    screenDirty = true;
    return;
  }

  if (wifiDeleteListActive) {
    uint8_t count = wifiMaintProfileCount();
    if (wifiDeleteIndex >= count) {
      wifiDeleteListActive = false;
      wifiSetupMenuActive = true;
      wifiSetupMenuIndex = 1;
    } else {
      wifiDeleteListActive = false;
      wifiDeleteConfirm = true;
      wifiDeleteChoiceYes = false;
    }
    screenDirty = true;
    return;
  }

  if (wifiSetupMenuActive) {
    if (wifiSetupMenuIndex == 0) {
      startWifiScanUi();
    } else if (wifiSetupMenuIndex == 1) {
      wifiSetupMenuActive = false;
      wifiDeleteListActive = true;
      wifiDeleteIndex = 0;
      screenDirty = true;
    } else {
      wifiSetupMenuActive = false;
      systemMenuActive = true;
      screenDirty = true;
    }
    return;
  }

  if (wifiSelectActive) {
    int selectedProfile = (wifiSelectIndex == 0) ? -1 : ((int)wifiSelectIndex - 1);
    if (wifiMaintSelectProfile(selectedProfile)) {
      wifiSelectActive = false;
      enterMaintenanceMode();
    } else {
      wifiSelectActive = false;
      systemMenuActive = true;
      snprintf(overlayTitle, sizeof(overlayTitle), "WIFI SELECT");
      snprintf(overlaySub, sizeof(overlaySub), "PROFILE NOT FOUND");
      overlayActive = true;
      overlayUntil = millis() + 900;
      screenDirty = true;
    }
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

  if (configMode && configPage == PAGE_OSC) {
    selectedOsc = (selectedOsc + 1) % 3;
    armPickupForCurrentContext();

    snprintf(overlayTitle, sizeof(overlayTitle), "OSC SELECT");
    snprintf(overlaySub, sizeof(overlaySub), "OSC %u / 3", selectedOsc + 1);
    overlayActive = true;
    overlayUntil = millis() + 500;
    screenDirty = true;
    return;
  }

  if (configMode && configPage == PAGE_LFO) {
    selectedLfo = (selectedLfo + 1) % LFO_COUNT;
    armPickupForCurrentContext();

    snprintf(overlayTitle, sizeof(overlayTitle), "LFO SELECT");
    snprintf(overlaySub, sizeof(overlaySub), "LFO %u / %u",
             selectedLfo + 1, LFO_COUNT);
    overlayActive = true;
    overlayUntil = millis() + 500;
    screenDirty = true;
    return;
  }

  if (configMode && configPage == PAGE_VOICE_ENV) {
    currentPreset.attack = 64;
    currentPreset.decay = 64;
    currentPreset.release = 64;
    applyEnvelopeAll();
    markModified();
    armPickupForCurrentContext();

    snprintf(overlayTitle, sizeof(overlayTitle), "VOICE ENV");
    snprintf(overlaySub, sizeof(overlaySub), "NEUTRAL 64/64/64");
    overlayActive = true;
    overlayUntil = millis() + 700;
    screenDirty = true;
    return;
  }

  if (configMode && configPage == PAGE_ENV) {
    selectedEnv = (selectedEnv + 1) % ENV_COUNT;
    armPickupForCurrentContext();

    snprintf(overlayTitle, sizeof(overlayTitle), "ENV SELECT");
    snprintf(overlaySub, sizeof(overlaySub), "ENV %u / %u",
             selectedEnv + 1, ENV_COUNT);
    overlayActive = true;
    overlayUntil = millis() + 500;
    screenDirty = true;
    return;
  }

  if (configMode && configPage == PAGE_ROUTE) {
    selectedRoute = (selectedRoute + 1) % MOD_ROUTE_COUNT;
    armPickupForCurrentContext();

    snprintf(overlayTitle, sizeof(overlayTitle), "ROUTE SELECT");
    snprintf(overlaySub, sizeof(overlaySub), "ROUTE %02u / %02u",
             selectedRoute + 1, MOD_ROUTE_COUNT);
    overlayActive = true;
    overlayUntil = millis() + 500;
    screenDirty = true;
    return;
  }

  snprintf(overlayTitle, sizeof(overlayTitle),
           configMode ? "CONFIG" : "MENU");
  snprintf(overlaySub, sizeof(overlaySub), "SHORT PRESS RESERVED");
  overlayActive = true;
  overlayUntil = millis() + 350;
  screenDirty = true;
}

void handleEncoderLongPress() {
  if (wifiMaintActive()) {
    exitWifiRuntime();
    return;
  }

  if (textEditorActive) {
    cancelTextEditor();
    return;
  }

  if (wifiScanListActive) {
    wifiScanListActive = false;
    wifiSetupMenuActive = true;
    wifiSetupMenuIndex = 0;
    screenDirty = true;
    return;
  }

  if (wifiDeleteConfirm) {
    wifiDeleteConfirm = false;
    wifiDeleteListActive = true;
    screenDirty = true;
    return;
  }

  if (wifiDeleteListActive) {
    wifiDeleteListActive = false;
    wifiSetupMenuActive = true;
    wifiSetupMenuIndex = 1;
    screenDirty = true;
    return;
  }

  if (wifiSetupMenuActive) {
    wifiSetupMenuActive = false;
    systemMenuActive = true;
    screenDirty = true;
    return;
  }

  if (wifiSelectActive) {
    wifiSelectActive = false;
    systemMenuActive = true;
    screenDirty = true;
    return;
  }

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

void drawHazardStripe(int y, int h, int x0, int width) {
  int screenW = M5.Display.width();
  if (width < 0) width = screenW - x0;
  if (width <= 0) return;

  M5.Display.fillRect(x0, y, width, h, C_YELLOW);
  int xEnd = x0 + width;
  for (int x = x0; x < xEnd; x += 14) {
    M5.Display.drawLine(x, y + h - 1, min(x + h, xEnd - 1), y, C_BLACK);
    M5.Display.drawLine(x + 1, y + h - 1, min(x + h + 1, xEnd - 1), y, C_BLACK);
    M5.Display.drawLine(x + 2, y + h - 1, min(x + h + 2, xEnd - 1), y, C_BLACK);
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
  } else if (strcmp(overlayTitle, "PAGE CHANGE") == 0) {
    // Page switching must be readable at a glance while the encoder is moving.
    M5.Display.setTextColor(C_YELLOW, C_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(18, 29);
    M5.Display.print("PAGE CHANGE");

    M5.Display.setTextColor(C_WHITE, C_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(18, 67);
    M5.Display.print(overlaySub);

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
    int y = 44 + i * 11;

    if (i == systemMenuIndex) {
      M5.Display.fillRect(10, y - 2, w - 20, 11, C_YELLOW);
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

void drawWifiSetupMenu() {
  clearScreen();
  int w = M5.Display.width();

  drawHazardStripe(0, 9);
  M5.Display.drawRect(5, 14, w - 10, 116, C_AMBER);

  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 20);
  M5.Display.print("WIFI SETUP");

  static const char* ITEMS[3] = {
    "ADD NETWORK",
    "DELETE NETWORK",
    "BACK"
  };

  M5.Display.setTextSize(1);
  for (uint8_t i = 0; i < 3; ++i) {
    int y = 52 + i * 18;
    if (i == wifiSetupMenuIndex) {
      M5.Display.fillRect(10, y - 3, w - 20, 14, C_YELLOW);
      M5.Display.setTextColor(C_BLACK, C_YELLOW);
    } else {
      M5.Display.setTextColor(C_WHITE, C_BLACK);
    }
    M5.Display.setCursor(15, y);
    M5.Display.print(i == wifiSetupMenuIndex ? "> " : "  ");
    M5.Display.print(ITEMS[i]);
  }

  M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.setCursor(10, 116);
  M5.Display.print("ADD = SCAN NEARBY WIFI");
  M5.Display.setCursor(10, 127);
  M5.Display.print("TURN SELECT / PUSH ENTER / HOLD BACK");
}


void drawWifiScanList() {
  clearScreen();
  int w = M5.Display.width();

  drawHazardStripe(0, 9);
  M5.Display.drawRect(5, 14, w - 10, 116, C_AMBER);

  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 18);
  M5.Display.print("WIFI NETWORKS");

  uint8_t count = wifiMaintScanCount();
  uint8_t optionCount = count + 3;
  static constexpr uint8_t VISIBLE_ROWS = 6;

  int first = 0;
  if (wifiScanIndex >= VISIBLE_ROWS) {
    first = (int)wifiScanIndex - VISIBLE_ROWS + 1;
  }
  if (first + VISIBLE_ROWS > optionCount) {
    first = max(0, (int)optionCount - (int)VISIBLE_ROWS);
  }

  M5.Display.setTextSize(1);
  for (uint8_t row = 0; row < VISIBLE_ROWS; ++row) {
    int option = first + row;
    if (option >= optionCount) break;

    int y = 42 + row * 13;
    bool selected = option == wifiScanIndex;

    if (selected) {
      M5.Display.fillRect(10, y - 2, w - 20, 11, C_YELLOW);
      M5.Display.setTextColor(C_BLACK, C_YELLOW);
    } else {
      M5.Display.setTextColor(C_WHITE, C_BLACK);
    }

    M5.Display.setCursor(14, y);
    M5.Display.print(selected ? "> " : "  ");

    if (option < count) {
      String ssid = wifiMaintScanSsid(option);
      if (ssid.length() > 23) ssid = ssid.substring(0, 20) + "...";
      M5.Display.print(ssid);

      if (wifiMaintScanSaved(option)) {
        M5.Display.setCursor(174, y);
        M5.Display.print("S");
      }
      if (wifiMaintScanSecured(option)) {
        M5.Display.setCursor(187, y);
        M5.Display.print("L");
      }

      int32_t rssi = wifiMaintScanRssi(option);
      uint8_t bars = (rssi >= -50) ? 4 : (rssi >= -65) ? 3 : (rssi >= -75) ? 2 : 1;
      for (uint8_t b = 0; b < 4; ++b) {
        int bh = 2 + b * 2;
        int bx = 207 + b * 7;
        int by = y + 7 - bh;
        uint16_t c = (b < bars)
                       ? (selected ? C_BLACK : C_GREEN)
                       : (selected ? C_BLACK : C_GREY);
        M5.Display.fillRect(bx, by, 4, bh, c);
      }
    } else if (option == count) {
      M5.Display.print("RESCAN");
    } else if (option == count + 1) {
      M5.Display.print("MANUAL / PHONE SETUP");
    } else {
      M5.Display.print("BACK");
    }
  }

  M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.setCursor(10, 123);
  if (count == 0) {
    M5.Display.print(wifiScanLastResult < 0 ? "SCAN ERROR" : "NO 2.4GHz WIFI FOUND");
  } else {
    M5.Display.print("S=SAVED L=LOCK  PUSH=SELECT");
  }
}

void drawTextEditor() {
  clearScreen();
  int w = M5.Display.width();

  M5.Display.drawRect(4, 4, w - 8, 104, C_AMBER);
  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(9, 10);
  M5.Display.print(textEditorPurpose == TextEditorPurpose::WIFI_PASSWORD
                     ? "PASSWORD"
                     : "TEXT EDITOR");

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_WHITE, C_BLACK);
  M5.Display.setCursor(9, 34);
  M5.Display.print("SSID: ");
  String ctx = textEditorContext;
  if (ctx.length() > 31) ctx = ctx.substring(0, 28) + "...";
  M5.Display.print(ctx);

  String mask = "";
  uint8_t shown = min((size_t)28, textEditorBuffer.length());
  if (textEditorBuffer.length() > shown) mask = "...";
  for (uint8_t i = 0; i < shown; ++i) mask += '*';

  M5.Display.setCursor(9, 49);
  M5.Display.print("VALUE: ");
  M5.Display.print(mask);

  M5.Display.setCursor(9, 63);
  M5.Display.printf("LEN %u/%u", (unsigned)textEditorBuffer.length(), textEditorMaxLen);
  if (textEditorBuffer.length() > 0) {
    M5.Display.print("  LAST: ");
    M5.Display.print(textEditorBuffer[textEditorBuffer.length() - 1]);
  }

  const char* chars = textEditorChars();
  size_t charCount = strlen(chars);
  char selectedChar = charCount ? chars[textEditorCharIndex % charCount] : '?';
  static const char* GROUP_LABELS[4] = {"ABC", "abc", "123", "SYM"};

  M5.Display.setCursor(9, 78);
  M5.Display.print("CHAR: ");
  M5.Display.print(selectedChar);
  M5.Display.print("  [");
  M5.Display.print(GROUP_LABELS[textEditorGroup]);
  M5.Display.print("]");

  M5.Display.setCursor(9, 92);
  if (textEditorMessage.length() > 0) {
    M5.Display.setTextColor(C_RED, C_BLACK);
    M5.Display.print(textEditorMessage);
  } else {
    M5.Display.setTextColor(C_GREY, C_BLACK);
    M5.Display.print("TURN=CHAR  PUSH=ADD");
  }

  // ByteButton legend. Logical order is mapped left -> right while editing.
  static const char* BYTE_LABELS[8] = {
    "ABC", "abc", "123", "SYM", "SPC", "DEL", "CAN", "OK"
  };
  int cellW = w / 8;
  for (uint8_t i = 0; i < 8; ++i) {
    int x = i * cellW;
    bool groupSelected = (i < 4 && i == textEditorGroup);
    uint16_t border = groupSelected ? C_YELLOW : C_GREY;
    uint16_t fill = groupSelected ? C_YELLOW : C_BLACK;
    uint16_t text = groupSelected ? C_BLACK : C_WHITE;
    M5.Display.fillRect(x, 111, cellW - 1, 23, fill);
    M5.Display.drawRect(x, 111, cellW - 1, 23, border);
    M5.Display.setTextColor(text, fill);
    M5.Display.setCursor(x + 4, 119);
    M5.Display.print(BYTE_LABELS[i]);
  }
}

void drawWifiDeleteList() {
  clearScreen();
  int w = M5.Display.width();

  drawHazardStripe(0, 9);
  M5.Display.drawRect(5, 14, w - 10, 116, C_AMBER);

  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 20);
  M5.Display.print("DELETE WIFI");

  M5.Display.setTextSize(1);
  uint8_t count = wifiMaintProfileCount();
  for (uint8_t i = 0; i <= count; ++i) {
    int y = 45 + i * 13;
    bool selected = i == wifiDeleteIndex;
    if (selected) {
      M5.Display.fillRect(10, y - 2, w - 20, 11, C_YELLOW);
      M5.Display.setTextColor(C_BLACK, C_YELLOW);
    } else {
      M5.Display.setTextColor(C_WHITE, C_BLACK);
    }

    M5.Display.setCursor(15, y);
    M5.Display.print(selected ? "> " : "  ");
    if (i == count) {
      M5.Display.print("BACK");
    } else {
      String label = wifiMaintProfileSsid(i);
      if (label.length() > 25) label = label.substring(0, 22) + "...";
      M5.Display.print(label);
    }
  }

  M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.setCursor(10, 126);
  M5.Display.print("PUSH=SELECT  HOLD=BACK");
}

void drawWifiDeleteConfirm() {
  clearScreen();
  int w = M5.Display.width();

  drawHazardStripe(0, 9);
  M5.Display.drawRect(5, 14, w - 10, 112, C_AMBER);

  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 22);
  M5.Display.print("DELETE WIFI ?");

  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_WHITE, C_BLACK);
  M5.Display.setCursor(10, 53);
  String ssid = wifiMaintProfileSsid(wifiDeleteIndex);
  if (ssid.length() > 30) ssid = ssid.substring(0, 27) + "...";
  M5.Display.print(ssid);

  M5.Display.setTextSize(2);
  M5.Display.setCursor(35, 88);
  if (!wifiDeleteChoiceYes) M5.Display.setTextColor(C_BLACK, C_YELLOW);
  else                      M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.print(" NO ");

  M5.Display.setCursor(145, 88);
  if (wifiDeleteChoiceYes) M5.Display.setTextColor(C_BLACK, C_YELLOW);
  else                     M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.print(" YES ");
}

void drawWifiSelect() {
  clearScreen();
  int w = M5.Display.width();

  drawHazardStripe(0, 9);
  M5.Display.drawRect(5, 14, w - 10, 116, C_AMBER);

  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(10, 20);
  M5.Display.print("WIFI SELECT");

  M5.Display.setTextSize(1);

  uint8_t profileCount = wifiMaintProfileCount();
  uint8_t optionCount = profileCount + 1;
  int preferred = wifiMaintPreferredProfile();

  for (uint8_t option = 0; option < optionCount; ++option) {
    int y = 46 + option * 12;
    bool selected = option == wifiSelectIndex;
    bool preferredHere = (option == 0) ? (preferred < 0)
                                       : (preferred == (int)option - 1);

    if (selected) {
      M5.Display.fillRect(10, y - 2, w - 20, 11, C_YELLOW);
      M5.Display.setTextColor(C_BLACK, C_YELLOW);
    } else {
      M5.Display.setTextColor(C_WHITE, C_BLACK);
    }

    M5.Display.setCursor(15, y);
    M5.Display.print(selected ? "> " : "  ");

    String label = (option == 0) ? String("AUTO")
                                 : wifiMaintProfileSsid(option - 1);
    if (label.length() > 25) {
      label = label.substring(0, 22) + "...";
    }
    M5.Display.print(label);
    if (preferredHere) M5.Display.print(" *");
  }

  M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.setCursor(10, 121);
  M5.Display.print("*=PREFERRED  PUSH=CONNECT  HOLD=BACK");
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
  M5.Display.print("FW          : v1.10.9");

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
    M5.Display.print("FW v1.10.9  LATEST ");
    M5.Display.print(wifiMaintLatestVersion());
  } else if (wifiMaintMode() == WifiMaintMode::MAINT_AP &&
             wifiMaintLastFailure().length() > 0) {
    String failed = wifiMaintLastFailureSsid();
    if (failed.length() > 24) failed = failed.substring(0, 21) + "...";
    M5.Display.print("FAIL : ");
    M5.Display.print(failed);
  } else {
    M5.Display.print("AP   : DinMeter-Setup");
  }

  if (wifiMaintMode() == WifiMaintMode::MAINT_AP &&
      !wifiMaintUpdating() && wifiMaintLastFailure().length() > 0) {
    String why = wifiMaintLastFailure();
    if (why.length() > 31) why = why.substring(0, 28) + "...";
    M5.Display.setTextColor(C_RED, C_BLACK);
    M5.Display.setCursor(10, 114);
    M5.Display.print(why);
    M5.Display.setTextColor(C_GREY, C_BLACK);
    M5.Display.setCursor(10, 126);
    M5.Display.print("AP DinMeter-Setup / HOLD=EXIT");
    return;
  }

  if (wifiMaintUpdating()) {
    M5.Display.setTextColor(C_GREEN, C_BLACK);
    M5.Display.setCursor(10, 114);
    M5.Display.printf("UPDATING... %d%%", wifiMaintProgress());
    M5.Display.setTextColor(C_GREY, C_BLACK);
    M5.Display.setCursor(10, 126);
    M5.Display.print("DO NOT POWER OFF");
    return;
  }

  M5.Display.setCursor(10, 114);
  if (wifiMaintMode() == WifiMaintMode::MAINT_STA) {
    if (wifiMaintGithubUpdateAvailable()) {
      M5.Display.setTextColor(C_GREEN, C_BLACK);
      M5.Display.print("PUSH = UPDATE ");
      M5.Display.print(wifiMaintLatestVersion());
    } else if (wifiMaintGithubBusy()) {
      M5.Display.setTextColor(C_YELLOW, C_BLACK);
      M5.Display.print("CHECKING GITHUB...");
    } else {
      M5.Display.setTextColor(C_GREY, C_BLACK);
      M5.Display.print("PUSH = CHECK GITHUB");
    }

    M5.Display.setTextColor(C_GREY, C_BLACK);
    M5.Display.setCursor(10, 126);
    M5.Display.print("HOLD = EXIT   WEB OTA READY");
  } else {
    M5.Display.setTextColor(C_GREY, C_BLACK);
    M5.Display.print("WEB OTA READY");
    M5.Display.setCursor(10, 126);
    M5.Display.print("PUSH/HOLD = EXIT");
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
  const int x0 = PRIMARY_UI_X_OFFSET;
  const int contentW = w - x0;
  const int cellW = contentW / 8;

  drawHazardStripe(0, 8, x0, contentW);

  // Top status
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setCursor(x0 + 6, 13);
  M5.Display.print(BANK_SHORT_NAMES[browseBank]);

  M5.Display.setCursor(x0 + 34, 13);
  M5.Display.setTextColor(C_WHITE, C_BLACK);
  M5.Display.printf("P%u", loadedSlot + 1);

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
  M5.Display.drawRect(x0 + 5, 27, contentW - 10, 30, C_AMBER);
  M5.Display.setTextColor(C_WHITE, C_BLACK);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(x0 + 10, 35);
  M5.Display.printf("%.18s", currentPreset.name);

  // 8Angle assignment labels - always visible in PERFORMANCE
  M5.Display.setTextSize(1);
  for (uint8_t i = 0; i < 8; ++i) {
    int x = x0 + i * cellW;

    if (i > 0) {
      M5.Display.drawLine(x, 65, x, 102, C_DARK);
    }

    M5.Display.setTextColor(C_AMBER, C_BLACK);
    M5.Display.setCursor(x + 2, 68);
    M5.Display.print(PERF_LABELS[i]);

    char v[8];
    formatParamValue(PAGE_PERF, i, v, sizeof(v));

    if (i == 6) {
      M5.Display.setTextColor(currentPreset.glide ? C_GREEN : C_GREY, C_BLACK);
      M5.Display.setCursor(x + 4, 84);
      M5.Display.print(currentPreset.glide ? "ON" : "OFF");
    } else {
      M5.Display.setTextColor(C_WHITE, C_BLACK);
      M5.Display.setCursor(x + 3, 84);
      M5.Display.print(v);
    }
  }

  M5.Display.drawLine(x0, 105, w - 1, 105, C_AMBER);

  // Bottom status
  M5.Display.setTextColor(currentPreset.mono ? C_YELLOW : C_GREY, C_BLACK);
  M5.Display.setCursor(x0 + 6, 111);
  M5.Display.print(currentPreset.mono ? "MONO" : "POLY");

  M5.Display.setTextColor(currentPreset.glide ? C_GREEN : C_GREY, C_BLACK);
  M5.Display.setCursor(x0 + 48, 111);
  M5.Display.print(currentPreset.glide ? "PORT ON" : "PORT OFF");

  M5.Display.setTextColor(C_GREY, C_BLACK);
  M5.Display.setCursor(x0 + 115, 111);
  M5.Display.print("ENC=BANK");

  M5.Display.setCursor(x0 + 6, 123);
  M5.Display.print("BYTE=PRESET   HOLD ENC=SAVE");
}

const char** labelsForPage(uint8_t page) {
  if (page == PAGE_PERF) return PERF_LABELS;
  if (page == PAGE_FILTER_ENV) return FILTER_LABELS;
  if (page == PAGE_VOICE_ENV) return VOICE_ENV_LABELS;
  if (page == PAGE_OSC) return OSC_LABELS;
  if (page == PAGE_GM) return GM_LABELS;
  if (page == PAGE_ENV) return ENV_LABELS;
  if (page == PAGE_LFO) return LFO_LABELS;
  if (page == PAGE_ROUTE) return ROUTE_LABELS;
  return MOD_LABELS;
}

const char* envCurveLabel(uint8_t curve) {
  if (curve == ENV_CURVE_EXP) return "EXP";
  if (curve == ENV_CURVE_LOG) return "LOG";
  return "LIN";
}

const char* routeSourceLabel(uint8_t source) {
  switch (source) {
    case MODSRC_ENV1: return "E1";
    case MODSRC_ENV2: return "E2";
    case MODSRC_ENV3: return "E3";
    case MODSRC_LFO1: return "L1";
    case MODSRC_LFO2: return "L2";
    case MODSRC_LFO3: return "L3";
    default: return "---";
  }
}

const char* routeDestinationLabel(uint8_t destination) {
  switch (destination) {
    case MODDST_CUTOFF: return "CUT";
    case MODDST_PITCH: return "PIT";
    case MODDST_AMP: return "AMP";
    default: return "---";
  }
}

const char* lfoWaveLabel(uint8_t waveform) {
  switch (waveform) {
    case LFO_WAVE_SINE: return "SIN";
    case LFO_WAVE_TRIANGLE: return "TRI";
    case LFO_WAVE_SAW_UP: return "SAW+";
    case LFO_WAVE_SAW_DOWN: return "SAW-";
    case LFO_WAVE_SQUARE: return "SQR";
    case LFO_WAVE_RANDOM: return "RND";
    default: return "SIN";
  }
}

void formatParamValue(uint8_t page, uint8_t knob, char* out, size_t n) {
  if (page == PAGE_PERF || page == PAGE_FILTER_ENV ||
      page == PAGE_VOICE_ENV || page == PAGE_MOD) {
    snprintf(out, n, "%u", currentParamAs127(knob));
    return;
  }

  if (page == PAGE_GM) {
    switch (knob) {
      case 0: snprintf(out, n, "%03u", currentGmLayer.program + 1); return;
      case 1: snprintf(out, n, "%u", currentGmLayer.level); return;
      case 2: snprintf(out, n, "%+d", currentGmLayer.transpose / 12); return;
      case 3: snprintf(out, n, "%u", currentGmLayer.pan); return;
      default: snprintf(out, n, "-"); return;
    }
  }

  if (page == PAGE_LFO) {
    const LfoState& lfo = currentPreset.lfo[selectedLfo];
    switch (knob) {
      case 0: snprintf(out, n, "%s", lfoWaveLabel(lfo.waveform)); return;
      case 1: snprintf(out, n, "%u", lfo.rate); return;
      case 2: snprintf(out, n, "%u", lfo.delay); return;
      case 3: snprintf(out, n, "%u", lfo.fade); return;
      case 4: snprintf(out, n, "%s", lfo.retrigger ? "ON" : "--"); return;
      case 5: snprintf(out, n, "%u", lfo.phase); return;
      default: snprintf(out, n, "-"); return;
    }
  }

  if (page == PAGE_ENV) {
    const EnvelopeState& env = currentPreset.env[selectedEnv];
    switch (knob) {
      case 0: snprintf(out, n, "%u", env.attack); return;
      case 1: snprintf(out, n, "%u", env.decay); return;
      case 2: snprintf(out, n, "%u", env.sustain); return;
      case 3: snprintf(out, n, "%u", env.release); return;
      case 4: snprintf(out, n, "%s", envCurveLabel(env.curve)); return;
      case 5: snprintf(out, n, "%s", env.retrigger ? "ON" : "--"); return;
      default: snprintf(out, n, "-"); return;
    }
  }

  if (page == PAGE_ROUTE) {
    const ModRoute& route = currentPreset.routes[selectedRoute];
    switch (knob) {
      case 0: snprintf(out, n, "%s", routeSourceLabel(route.source)); return;
      case 1: snprintf(out, n, "%s", routeDestinationLabel(route.destination)); return;
      case 2: snprintf(out, n, "%+d", route.amount); return;
      case 3: snprintf(out, n, "%s", route.enabled ? "ON" : "--"); return;
      default: snprintf(out, n, "-"); return;
    }
  }

  if (page == PAGE_OSC) {
    uint8_t oi = selectedOsc;
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
  const int x0 = PRIMARY_UI_X_OFFSET;
  const int contentW = w - x0;
  const int tabW = contentW / PAGE_COUNT;

  for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
    int x = x0 + i * tabW;
    int width = (i == PAGE_COUNT - 1) ? (w - x) : (tabW - 1);

    if (i == configPage) {
      M5.Display.fillRect(x, 0, width, 16, C_YELLOW);
      M5.Display.setTextColor(C_BLACK, C_YELLOW);
    } else {
      M5.Display.drawRect(x, 0, width, 16, C_DARK);
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
  const int x0 = PRIMARY_UI_X_OFFSET;
  const int contentW = w - x0;

  drawPageTabs();

  M5.Display.setTextColor(C_YELLOW, C_BLACK);
  M5.Display.setTextSize(1);
  M5.Display.setCursor(x0 + 5, 20);

  if (configPage == PAGE_OSC) {
    M5.Display.printf("CONFIG // OSC %u/3", selectedOsc + 1);
  } else if (configPage == PAGE_GM) {
    M5.Display.printf("GM %03u %.11s",
                      currentGmLayer.program + 1,
                      gmProgramName(currentGmLayer.program));
  } else if (configPage == PAGE_ENV) {
    M5.Display.printf("CONFIG // ENV %u/%u", selectedEnv + 1, ENV_COUNT);
  } else if (configPage == PAGE_LFO) {
    M5.Display.printf("CONFIG // LFO %u/%u", selectedLfo + 1, LFO_COUNT);
  } else if (configPage == PAGE_ROUTE) {
    M5.Display.printf("CONFIG // ROUTE %02u/%02u",
                      selectedRoute + 1, MOD_ROUTE_COUNT);
  } else {
    M5.Display.printf("CONFIG // %s", PAGE_NAMES[configPage]);
  }

  drawHazardStripe(34, 6, x0, contentW);

  const char** labels = labelsForPage(configPage);
  const int cellW = contentW / 8;

  // 8Angle row
  M5.Display.setTextSize(1);
  for (uint8_t i = 0; i < 8; ++i) {
    int x = x0 + i * cellW;
    M5.Display.setTextColor(C_AMBER, C_BLACK);
    M5.Display.setCursor(x + 2, 44);
    M5.Display.print(labels[i]);

    char v[8];
    formatParamValue(configPage, i, v, sizeof(v));
    M5.Display.setTextColor(C_WHITE, C_BLACK);
    M5.Display.setCursor(x + 2, 58);
    M5.Display.print(v);
  }

  M5.Display.drawLine(x0, 76, w - 1, 76, C_AMBER);

  // ByteButton row
  bool btnStates[8] = {
    (bool)currentPreset.mono,
    (bool)currentPreset.glide,
    (bool)currentPreset.legato,
    (bool)currentPreset.soft,
    (bool)currentPreset.reverbEnabled,
    (bool)currentPreset.vibratoEnabled,
    false, // PANIC is an action, not a state
    configAuditionEnabled
  };

  for (uint8_t i = 0; i < 8; ++i) {
    int x = x0 + i * cellW;

    M5.Display.setTextColor(C_AMBER, C_BLACK);
    M5.Display.setCursor(x + 2, 83);
    M5.Display.print(CONFIG_BUTTON_LABELS[i]);

    if (i == 6) {
      M5.Display.setTextColor(C_RED, C_BLACK);
      M5.Display.setCursor(x + 5, 99);
      M5.Display.print("!!");
    } else {
      M5.Display.setTextColor(btnStates[i] ? C_GREEN : C_GREY, C_BLACK);
      M5.Display.setCursor(x + 8, 99);
      M5.Display.print(btnStates[i] ? "ON" : "--");
    }
  }

  if (modified) {
    M5.Display.setTextColor(C_RED, C_BLACK);
    M5.Display.setCursor(x0 + 5, 117);
    M5.Display.print("* MODIFIED");
  } else {
    M5.Display.setTextColor(C_GREY, C_BLACK);
    M5.Display.setCursor(x0 + 5, 117);
    if (configPage == PAGE_VOICE_ENV) {
      M5.Display.print("PUSH=NEUTRAL  ENC=PAGE");
    } else if (configPage == PAGE_OSC || configPage == PAGE_ENV ||
               configPage == PAGE_LFO || configPage == PAGE_ROUTE) {
      M5.Display.print("PUSH=NEXT   ENC=PAGE");
    } else {
      M5.Display.print("ENC=PAGE   HOLD=SAVE");
    }
  }

  drawHazardStripe(128, 7, x0, contentW);
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
  } else if (textEditorActive) {
    drawTextEditor();
  } else if (wifiScanListActive) {
    drawWifiScanList();
  } else if (wifiDeleteConfirm) {
    drawWifiDeleteConfirm();
  } else if (wifiDeleteListActive) {
    drawWifiDeleteList();
  } else if (wifiSetupMenuActive) {
    drawWifiSetupMenu();
  } else if (wifiSelectActive) {
    drawWifiSelect();
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
  loadGmLayerData(currentPresetIndex(), currentGmLayer);

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

  snprintf(overlayTitle, sizeof(overlayTitle), "DIN SYNTH v1.10.9");
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

  // CONFIG can generate its own audition notes when no keyboard is attached.
  updateConfigAudition();

  // DinMeter-owned time-domain modulators run independently of the slower UI
  // polling. The MIDI output side is rate-limited inside each engine.
  updateSoftwareGlide();
  updateModulationEngine();

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
  Version: v1.10.9
  END
  ======================================================================
*/
