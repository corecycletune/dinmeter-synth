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
