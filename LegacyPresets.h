#pragma once
#include <Arduino.h>

struct LegacyOscDef {
  uint8_t bank;
  uint8_t program;
  uint8_t volume;
  int8_t transpose;
  int8_t detune;
};

struct LegacyPatchDef {
  const char* name;
  LegacyOscDef osc1;
  LegacyOscDef osc2;
  LegacyOscDef osc3;
  uint8_t cutoff;
  uint8_t resonance;
  uint8_t attack;
  uint8_t decay;
  uint8_t release;
  uint8_t chorusProgram;
  uint8_t chorusSend;
  uint8_t spatialVolume;
  uint8_t spatialDelay;
};

static constexpr LegacyOscDef LEGACY_OFF_OSC = {0, 0, 0, 0, 0};
static constexpr uint8_t LEGACY_PATCH_COUNT = 60;

static const LegacyPatchDef LEGACY_PATCHES[LEGACY_PATCH_COUNT] = {
  {"00 PIANO", {0,0,100,0,0}, LEGACY_OFF_OSC, LEGACY_OFF_OSC, 64,64,64,64,64, 2,0, 0,29},
  {"01 SH Square+Sub", {127,47,100,0,0}, {127,47,24,-12,0}, LEGACY_OFF_OSC, 48,72,65,68,54, 2,8, 10,12},
  {"02 SH Square+Saw", {127,47,100,0,0}, {127,44,32,0,3}, LEGACY_OFF_OSC, 50,82,64,64,48, 2,6, 10,10},
  {"03 SH Square+Ocarina", {127,47,100,0,0}, {0,79,28,0,4}, LEGACY_OFF_OSC, 52,70,65,68,55, 2,5, 12,12},
  {"04 SH Resonant Saw", {127,44,100,0,0}, LEGACY_OFF_OSC, LEGACY_OFF_OSC, 42,102,64,60,44, 2,0, 7,8},
  {"05 SH Heavy Sub", {127,47,100,0,0}, {127,47,36,-12,0}, LEGACY_OFF_OSC, 46,68,65,70,58, 2,5, 16,15},
  {"06 SH Ocarina+Saw", {0,79,100,0,0}, {127,44,24,0,4}, LEGACY_OFF_OSC, 54,76,65,68,54, 2,5, 12,12},
  {"07 SH Harmonics+Square", {0,31,100,0,0}, {127,47,26,0,3}, LEGACY_OFF_OSC, 54,68,69,68,58, 2,5, 12,11},
  {"08 SH Thick ShortDelay", {127,47,100,0,0}, {127,44,55,0,5}, LEGACY_OFF_OSC, 46,78,65,66,52, 6,26, 8,10},
  {"09 SH Round 3OSC", {127,47,100,0,0}, {0,79,30,0,4}, {127,47,18,-12,0}, 46,72,66,70,58, 2,8, 18,16},

  {"10 PIANO", {0,0,100,0,0}, LEGACY_OFF_OSC, LEGACY_OFF_OSC, 64,64,64,64,64, 2,0, 0,29},
  {"11 MOOG Saw x2", {127,44,100,0,0}, {127,44,78,0,5}, LEGACY_OFF_OSC, 48,55,64,72,64, 2,4, 8,10},
  {"12 MOOG Saw+Square", {127,44,100,0,0}, {127,47,60,0,4}, LEGACY_OFF_OSC, 44,60,64,76,68, 2,5, 10,10},
  {"13 MOOG Saw+Sub", {127,44,100,0,0}, {127,47,30,-12,0}, LEGACY_OFF_OSC, 42,58,64,78,70, 2,4, 9,10},
  {"14 MOOG Round Ocarina", {127,44,100,0,0}, {0,79,38,0,3}, LEGACY_OFF_OSC, 40,54,65,78,72, 2,6, 10,12},
  {"15 MOOG Triple", {127,44,100,0,0}, {127,47,52,0,5}, {127,47,20,-12,0}, 42,60,64,80,72, 2,8, 14,14},
  {"16 MOOG Dark", {127,44,100,0,0}, {127,44,68,0,4}, LEGACY_OFF_OSC, 32,48,65,82,76, 2,5, 8,10},
  {"17 MOOG Resonant", {127,44,100,0,0}, {127,47,48,0,3}, LEGACY_OFF_OSC, 44,90,64,76,68, 2,5, 8,10},
  {"18 MOOG ShortDelay", {127,44,100,0,0}, {127,44,72,0,5}, LEGACY_OFF_OSC, 44,56,64,78,70, 6,24, 5,8},
  {"19 MOOG Big", {127,44,100,0,0}, {127,47,64,0,6}, {0,79,24,-12,0}, 38,58,65,82,78, 6,16, 15,15},

  {"20 PIANO", {0,0,100,0,0}, LEGACY_OFF_OSC, LEGACY_OFF_OSC, 64,64,64,64,64, 2,0, 0,29},
  {"21 PROPHET Saw+Saw", {127,44,100,0,0}, {127,44,76,0,7}, LEGACY_OFF_OSC, 72,56,64,68,62, 2,15, 18,14},
  {"22 PROPHET Saw+Square", {127,44,100,0,0}, {127,47,64,0,6}, LEGACY_OFF_OSC, 74,60,64,68,64, 2,16, 20,15},
  {"23 PROPHET Brass Hybrid", {0,62,100,0,0}, {127,44,45,0,5}, LEGACY_OFF_OSC, 72,58,65,72,68, 2,18, 20,16},
  {"24 PROPHET Bright", {127,44,100,0,0}, {127,47,60,0,8}, LEGACY_OFF_OSC, 92,68,63,62,54, 2,12, 16,12},
  {"25 PROPHET Wide", {127,44,100,0,-4}, {127,44,82,0,6}, LEGACY_OFF_OSC, 76,58,64,70,66, 2,22, 30,20},
  {"26 PROPHET Soft", {127,47,100,0,0}, {0,79,34,0,5}, LEGACY_OFF_OSC, 58,50,68,76,76, 2,14, 22,18},
  {"27 PROPHET ShortDelay", {127,44,100,0,0}, {127,47,58,0,6}, LEGACY_OFF_OSC, 72,58,64,68,64, 6,30, 12,10},
  {"28 PROPHET 3OSC", {127,44,100,0,0}, {127,47,55,0,6}, {0,79,22,12,0}, 70,58,64,70,68, 2,18, 24,18},
  {"29 PROPHET Lush", {127,44,100,0,-4}, {127,47,72,0,7}, {0,79,20,0,0}, 68,56,67,74,76, 6,18, 34,22},

  {"30 PIANO", {0,0,100,0,0}, LEGACY_OFF_OSC, LEGACY_OFF_OSC, 64,64,64,64,64, 2,0, 0,29},
  {"31 KIEFER Ocarina", {0,79,100,0,0}, {127,47,24,0,4}, LEGACY_OFF_OSC, 58,48,66,68,62, 2,8, 14,12},
  {"32 KIEFER Whistle", {0,78,100,0,0}, {0,79,28,0,4}, LEGACY_OFF_OSC, 58,46,66,70,64, 2,8, 16,12},
  {"33 KIEFER Voice+Ocarina", {0,85,100,0,0}, {0,79,32,0,4}, LEGACY_OFF_OSC, 56,48,67,70,66, 2,10, 18,14},
  {"34 KIEFER Harmonics", {0,31,100,0,0}, {0,79,26,0,3}, LEGACY_OFF_OSC, 54,46,69,70,66, 2,8, 14,12},
  {"35 KIEFER Clear", {0,79,100,0,0}, {127,44,20,0,3}, LEGACY_OFF_OSC, 68,50,64,64,54, 2,6, 12,10},
  {"36 KIEFER Round", {0,79,100,0,0}, {127,47,30,0,4}, LEGACY_OFF_OSC, 48,44,68,74,72, 2,9, 18,14},
  {"37 KIEFER ShortDelay", {0,79,100,0,0}, {127,47,26,0,4}, LEGACY_OFF_OSC, 56,46,66,70,64, 6,24, 8,8},
  {"38 KIEFER Wide", {0,79,100,0,-3}, {0,79,68,0,5}, LEGACY_OFF_OSC, 56,46,66,72,68, 2,12, 28,18},
  {"39 KIEFER Soft Triple", {0,79,100,0,0}, {127,47,28,0,4}, {0,73,18,0,-3}, 52,46,68,74,72, 6,14, 22,16},

  {"40 PIANO", {0,0,100,0,0}, LEGACY_OFF_OSC, LEGACY_OFF_OSC, 64,64,64,64,64, 2,0, 0,29},
  {"41 ORIGINAL Ocarina+Clarinet", {0,79,100,0,0}, {0,71,25,0,4}, LEGACY_OFF_OSC, 58,56,66,70,64, 2,8, 14,12},
  {"42 ORIGINAL Ocarina+Harmonics", {0,79,100,0,0}, {0,31,28,0,3}, LEGACY_OFF_OSC, 52,48,68,72,68, 2,8, 16,12},
  {"43 ORIGINAL Whistle+Square", {0,78,100,0,0}, {127,47,34,0,5}, LEGACY_OFF_OSC, 62,54,65,68,60, 2,10, 18,14},
  {"44 ORIGINAL Flute+Saw", {0,73,100,0,0}, {127,44,26,0,4}, LEGACY_OFF_OSC, 58,52,67,70,64, 2,8, 14,12},
  {"45 ORIGINAL Recorder+Square", {0,74,100,0,0}, {127,47,28,0,4}, LEGACY_OFF_OSC, 56,54,67,70,66, 2,8, 16,12},
  {"46 ORIGINAL Harmonics+Ocarina", {0,31,100,0,0}, {0,79,32,0,3}, LEGACY_OFF_OSC, 48,48,69,76,72, 2,8, 14,12},
  {"47 ORIGINAL Clarinet Dark", {0,71,100,0,0}, {0,79,20,0,4}, LEGACY_OFF_OSC, 42,56,68,74,70, 2,8, 14,12},
  {"48 ORIGINAL Sine-ish Wide", {0,79,100,0,-3}, {0,78,65,0,5}, LEGACY_OFF_OSC, 54,44,67,72,68, 2,12, 30,20},
  {"49 ORIGINAL Huge", {0,79,100,0,0}, {127,44,30,0,5}, {127,47,22,-12,0}, 46,54,68,78,76, 6,18, 24,18},

  {"50 PIANO", {0,0,100,0,0}, LEGACY_OFF_OSC, LEGACY_OFF_OSC, 64,64,64,64,64, 2,0, 0,29},
  {"51 DUB RAW", {0,31,100,0,0}, LEGACY_OFF_OSC, LEGACY_OFF_OSC, 64,64,64,64,64, 2,0, 0,29},
  {"52 DUB Attack Trim", {0,31,100,0,0}, LEGACY_OFF_OSC, LEGACY_OFF_OSC, 52,52,69,72,74, 2,0, 0,29},
  {"53 DUB MicroLayer", {0,31,100,0,0}, {0,31,42,0,1}, LEGACY_OFF_OSC, 50,50,69,74,76, 2,0, 0,29},
  {"54 DUB Light Spatial", {0,31,100,0,0}, {0,31,45,0,1}, LEGACY_OFF_OSC, 48,50,69,76,78, 2,0, 8,6},
  {"55 DUB Spatial More", {0,31,100,0,0}, {0,31,42,0,1}, LEGACY_OFF_OSC, 46,48,69,78,80, 2,0, 14,8},
  {"56 DUB Sub", {0,31,100,0,0}, {0,31,25,-12,0}, LEGACY_OFF_OSC, 44,48,70,80,82, 2,0, 5,5},
  {"57 DUB Ocarina Sub", {0,31,100,0,0}, {0,79,22,-12,0}, LEGACY_OFF_OSC, 44,46,70,80,84, 2,0, 6,5},
  {"58 DUB ShortDelay Tiny", {0,31,100,0,0}, {0,31,35,0,1}, LEGACY_OFF_OSC, 48,48,69,76,78, 6,10, 0,29},
  {"59 DUB Deep", {0,31,100,0,0}, {0,31,28,-12,0}, {0,79,14,-12,0}, 38,46,70,84,88, 2,0, 8,6},
};
