#pragma once
#include <Arduino.h>

enum class WifiMaintMode : uint8_t {
  OFF = 0,
  MAINT_STA,
  MAINT_AP,
  SETUP_AP
};

void wifiMaintInit();
void wifiMaintStartMaintenance();
void wifiMaintStartSetup();
void wifiMaintStop();
void wifiMaintLoop();

bool wifiMaintActive();
bool wifiMaintUpdating();
bool wifiMaintHasCredentials();
bool wifiMaintStaConnected();
WifiMaintMode wifiMaintMode();

String wifiMaintIp();
String wifiMaintModeText();
String wifiMaintSavedSsid();
int wifiMaintProgress();
String wifiMaintStatus();

// Saved Wi-Fi profile selection for the physical SYSTEM MENU.
// Profiles are exposed as compact ordinals 0..count-1; -1 means AUTO.
uint8_t wifiMaintProfileCount();
String wifiMaintProfileSsid(uint8_t ordinal);
int wifiMaintPreferredProfile();
bool wifiMaintSelectProfile(int ordinal);

// GitHub Release self-update (available only in HOME WIFI maintenance mode).
void wifiMaintCheckLatestRelease();
void wifiMaintStartGithubUpdate();
bool wifiMaintGithubUpdateAvailable();
bool wifiMaintGithubBusy();
String wifiMaintLatestVersion();
String wifiMaintGithubStatus();
