DinMeter_Synth_v1_4
=================

Target hardware
---------------
- M5Stack Din Meter v1.1
- M5Stack Unit Hub
- M5Stack ByteButton U192
- M5Stack 8Angle U154
- M5Stack MIDI Unit U187 / SAM2695
- USB MIDI keyboard

Arduino board
-------------
M5StampS3 - M5Stack

Required installed libraries
----------------------------
1. M5Unified
2. M5Unit-8Angle
3. M5Unit-ByteButton
4. M5-SAM2695

USB MIDI Host library
---------------------
This sketch expects UsbMidi.h and its companion .cpp/.h files from:
  https://github.com/enudenki/esp32-usb-host-midi-library

Use the same source files that already worked in DinMeter_BringUp.
Copy the Omocha library src files into this sketch folder, OR install the
library in your Arduino libraries folder and make sure #include "UsbMidi.h"
resolves.

Files in this package
---------------------
- DinMeter_Synth_v1_4.ino   : Arduino entry point
- SynthApp.h              : app declarations
- SynthApp.cpp            : full V1 implementation
- LegacyPresets.h         : migrated 60-preset table from the prior firmware

Wiring
------
PORT A -> Unit Hub -> ByteButton + 8Angle
PORT B -> MIDI Unit
USB-C  -> USB MIDI keyboard

Logical controls
----------------
PERFORMANCE 8Angle left->right:
VOL | CUT | RES | ATK | REL | REV | VIB | GLIDE

8Angle toggle:
OFF = PERFORMANCE
ON  = CONFIG

ByteButton PERFORMANCE:
physical left->right CH7..CH0
software left->right P1..P8

CONFIG pages:
01 PERF
02 FILTER/ENV
03 OSC1
04 OSC2
05 OSC3
06 MOD

CONFIG ByteButton:
MON | GLD | LEG | SUS | SFT | REV | VIB | PANIC

Encoder:
PERFORMANCE rotate = BANK
CONFIG rotate      = PAGE
short press        = reserved MENU
long press         = SAVE confirmation

First-run adjustment points
---------------------------
1. If CONFIG switch direction is reversed:
   SynthApp.cpp:
     CONFIG_SWITCH_ACTIVE_LEVEL = true/false

2. If encoder direction is reversed:
     ENCODER_DIRECTION = +1 / -1

3. If an 8Angle knob moves backwards:
     ANGLE_REVERSE[8]

4. If encoder moves several pages per physical click:
     ENC_TRANSITIONS_PER_DETENT = 4
   Try 2 or 1 depending on the actual encoder.

Safety
------
The note manager detects an orphan NoteOff and triggers PANIC.
This is intended to recover from the observed case:
key held -> keyboard OCTAVE changed -> release sends a different note number.

Notes
-----
- Master VOL is not stored in presets.
- Program Change from the MIDI keyboard is ignored.
- The old 60 preset materials are migrated as defaults.
- Presets 61-64 are INIT slots.
- Saved user edits are stored in ESP32 Preferences/NVS.


v1.1 confirmed hardware corrections
-----------------------------------
1. 8Angle:
   fully LEFT  = 0
   fully RIGHT = 127
   All eight channels are reversed in software to match this UI direction.

2. 8Angle ADC/display jitter:
   +/-1 quantized count is ignored before parameter/UI update.
   This reduces screen flicker caused by ADC values hovering across a boundary.

3. ByteButton PERFORMANCE preset numbering:
   uses the printed physical channel numbers.
   physical RIGHT -> LEFT = preset 0..7.
   CONFIG-mode function order is unchanged (user-facing left -> right).


v1.2 PERFORMANCE GLIDE control
------------------------------
Din Meter orange encoder:
- rotate in PERFORMANCE = BANK
- short press in PERFORMANCE = GLIDE / PORTAMENTO ON/OFF
- long press = SAVE confirmation

GLIDE ON:
- automatically enables MONO
- keeps the current GLIDE TIME from the rightmost 8Angle knob

GLIDE OFF:
- disables portamento
- MONO remains as-is, allowing MONO without glide

CONFIG-mode ByteButton GLD remains available as the detailed/config-side
control for the same stored parameter.


v1.3 PERFORMANCE control change
--------------------------------
8Angle PERFORMANCE left -> right:
VOL | CUT | RES | ATK | REL | VIB | PORT | GLIDE TIME

- Reverb was removed from PERFORMANCE. Reverb is adjusted in CONFIG only.
- Knob #6 (index 5): VIBRATO DEPTH.
- Knob #7 (index 6): PORTAMENTO OFF/ON.
    left side  = OFF
    right side = ON
    hysteresis around the middle prevents chatter.
- Knob #8 (index 7): GLIDE / PORTAMENTO TIME.

The physical PORTAMENTO knob is authoritative in PERFORMANCE, like VOL:
it does not use pickup and does not mark the preset MODIFIED.
Its LED is green when ON and dim red when OFF.

Din Meter encoder short press no longer toggles GLIDE.
It is reserved again for the future menu.


v1.4 PERFORMANCE UI
-------------------
The PERFORMANCE screen now always shows the eight 8Angle assignments
left-to-right, matching the physical hardware:

VOL | CUT | RES | ATK | REL | VIB | PRT | GLD

The current value is shown under each label.
For PRT (Portamento), the display shows ON/OFF instead of a numeric value.

The preset name remains prominent above the control map.
Bottom status still shows MONO/POLY, Portamento state, BANK and SAVE hints.


Packaging fix:
- Sketch folder name is fixed to DinMeter_Synth_v1
- Main file name is fixed to DinMeter_Synth_v1.ino
- Internal code version remains v1.4

v1.5 compile fix
----------------
Added explicit forward declarations for:
- rawTo127(...)
- setParamPopup(...)
- formatParamValue(...)

Reason:
SynthApp.cpp is a normal C++ source file, so Arduino's automatic .ino
function-prototype generation does not apply inside it.


============================================================
v1.7 Wi-Fi / OTA Maintenance
============================================================

IMPORTANT
---------
This v1.7 build is based on the last known-good v1.5 boot/audio build.
It adds Wi-Fi/OTA and the requested PERFORMANCE Portamento pair:

  RIGHT = MONO + PORTAMENTO ON
  LEFT  = POLY + PORTAMENTO OFF

The earlier experimental live-CUTOFF controller build is NOT included here.
The earlier experimental Pitch-Bend glide build is also NOT included here.
Those bugs can be resumed after OTA is proven stable.

FIRST INSTALL
-------------
This version itself must be flashed once over USB.

After v1.7 is running, normal future firmware updates can be done over Wi-Fi.
USB then becomes emergency recovery only.

SYSTEM MENU
-----------
Long-press the Din Meter encoder:

  SAVE PRESET
  MAINTENANCE
  WIFI SETUP
  SYSTEM INFO
  EXIT

Turn encoder = select
Short press  = enter
Long press   = open/close SYSTEM MENU

WIFI SETUP
----------
SYSTEM MENU -> WIFI SETUP

Din Meter creates:

  SSID: DinMeter-Setup

Connect a phone/PC to it and open:

  http://192.168.4.1/

Enter home SSID/password and press SAVE WIFI.

Credentials are stored in ESP32 Preferences/NVS under namespace:
  dmsynth-wifi

They are NOT hard-coded in the source.

MAINTENANCE
-----------
SYSTEM MENU -> MAINTENANCE -> YES

If saved home Wi-Fi is available:
  Din Meter joins home Wi-Fi.
  PC does not need to change Wi-Fi.
  Browser:
    http://dinmeter.local/
  The screen also shows the numeric IP.

If home Wi-Fi is unavailable or not configured:
  Din Meter falls back automatically to:
    SSID: DinMeter-Setup
  Browser:
    http://192.168.4.1/

NORMAL PERFORMANCE / CONFIG
---------------------------
Wi-Fi is OFF.
Performance USB MIDI / UART / I2C processing stays isolated from Wi-Fi.

WEB OTA
-------
In MAINTENANCE mode open the browser page.

Compile a firmware .bin in Arduino IDE, then use:
  UPLOAD FIRMWARE

After a successful upload the Din Meter reboots automatically.

ARDUINO IDE OTA
---------------
In MAINTENANCE mode, Arduino IDE may show a network port named:
  DinMeter-Synth

Select that network port and upload normally.

If the network port does not appear, Web OTA still works using the IP/browser.

PARTITION NOTE
--------------
OTA requires an ESP32 partition scheme with OTA application slots.
The normal M5Stack/ESP32 board defaults generally provide this.

If Web/Arduino OTA reports insufficient update space, select an Arduino
"Partition Scheme" that includes OTA, flash once by USB, and then continue
wirelessly.

FILES ADDED IN v1.7
-------------------
  WifiMaintenance.h
  WifiMaintenance.cpp

These use only libraries included with the ESP32 Arduino core:
  WiFi
  WebServer
  Update
  ArduinoOTA
  ESPmDNS
  Preferences

No additional Library Manager install should normally be required.


v1.7 packaging note
-------------------
ArduinoOTA is explicitly stopped when leaving maintenance mode, so the user
can exit and re-enter MAINTENANCE during the same boot.
\n\nv1.7.1 maintenance screen flicker fix\n-------------------------------------\nv1.7 forced a full maintenance-screen redraw every 250 ms.\nBecause each redraw clears the display first, that appeared as constant flicker.\n\nv1.7.1 redraws the maintenance screen only when a visible item changes:\n- maintenance mode\n- status text\n- IP address\n- OTA progress\n- OTA updating state\n\nWhen OTA is idle the maintenance screen stays static.\n

v1.7.2 compile fix
------------------
Added the missing forward declaration:
  void resetMaintenanceUiCache();

Reason:
SynthApp.cpp is a normal C++ source file. Functions called before their
definition must be declared explicitly.


v1.7.3 Web OTA feedback improvement
-----------------------------------
The old Web OTA form used a normal HTML form submit. The browser showed almost
no immediate visual feedback while the firmware was uploading, so the
UPLOAD FIRMWARE button looked as if it had not been pressed.

v1.7.3 adds client-side upload feedback:
- selected firmware filename is shown as READY
- button immediately changes to UPLOADING...
- file selector/button are disabled during transfer
- message says DO NOT POWER OFF
- live browser upload percentage is shown
- progress bar is shown
- success changes to UPDATE COMPLETE - REBOOTING...
- HTTP/connection errors are shown clearly

This changes only the browser OTA user experience; firmware writing behavior
is unchanged.


v1.7.4 Web OTA hover cursor
---------------------------
Buttons now explicitly use a hand/pointer cursor on mouse hover.
Disabled upload buttons still use the waiting cursor.


v1.7.5 Web OTA return button
----------------------------
The UPDATE COMPLETE page now includes:

  BACK TO DINMETER

The button returns the browser to "/" on the same host/IP that served the
update page.

Important:
After a successful OTA update the Din Meter reboots into normal mode and
normal mode has Wi-Fi OFF. Therefore the button may not immediately connect.
If that happens, re-enter MAINTENANCE on the Din Meter and then press the
button again. The link itself keeps the original host/IP.


============================================================
v1.7.6 Sticky MAINTENANCE mode
============================================================

REQUESTED BEHAVIOR
------------------
Enter:
  Din Meter encoder long press
  -> SYSTEM MENU
  -> MAINTENANCE
  -> YES

While MAINTENANCE is active:
  OTA update
  -> firmware writes
  -> ESP restarts
  -> automatically reconnects Wi-Fi
  -> automatically returns to MAINTENANCE / OTA READY

Exit:
  press the Din Meter orange rotary encoder once
  -> clear maintenance resume flag
  -> Wi-Fi OFF
  -> return to normal PERFORMANCE / CONFIG

Power off:
  maintenance resume flag is cleared on the next real POWER-ON boot
  -> normal PERFORMANCE / CONFIG

HOW IT KNOWS
------------
v1.7.6 stores a small "maint_sticky" flag in ESP32 Preferences/NVS.

On software reboot / OTA:
  NVS survives -> flag remains ON -> maintenance resumes.

On a real power-on or brownout:
  reset reason is detected with esp_reset_reason()
  -> flag is cleared -> normal mode.

FIRST OTA INTO v1.7.6
---------------------
The previously-running v1.7.5 firmware cannot set the new flag before it
reboots. v1.7.6 therefore includes a ONE-TIME migration rule:

If:
  - this is the first v1.7.6 boot,
  - reset reason is software reset,
  - saved Wi-Fi credentials exist,

then it assumes the unit arrived via OTA and starts MAINTENANCE automatically.

After this migration runs once, normal sticky behavior is used.

IMPORTANT
---------
The earlier live-cutoff and manual Pitch-Bend glide experiments are still
NOT included in this Wi-Fi/OTA baseline. Those bugs remain for later.


============================================================
v1.7.7 Web OTA reboot-aware feedback
============================================================

FIXED:
The browser previously displayed:
  CONNECTION LOST DURING UPLOAD

even when the firmware had actually reached 100% and the Din Meter had
rebooted successfully. A successful OTA reboot naturally drops the TCP/Wi-Fi
connection, so that message was a false failure indication.

NEW BEHAVIOR:
- Browser still shows real upload progress.
- Once the whole HTTP request has been sent:
    FIRMWARE SENT - VERIFYING / REBOOT MAY DISCONNECT
- If the connection drops AFTER all firmware bytes were sent:
    this is treated as an expected reboot, NOT an error.
- The browser polls /health once per second.
- When the newly rebooted firmware is reachable again:
    DINMETER BACK ONLINE - v1.7.7
- The upload button becomes:
    BACK TO DINMETER

REAL FAILURE:
If the connection is lost BEFORE the browser has sent the complete firmware:
    CONNECTION LOST BEFORE FIRMWARE WAS SENT

OTHER:
- Maintenance web page now displays firmware version.
- Added /health JSON endpoint.
- Successful web OTA reboot delay increased from 1.5 s to 3 s to give the
  HTTP success response more time to leave the ESP32 TCP stack.

IMPORTANT FOR THE FIRST UPDATE INTO v1.7.7:
The web page used during that upload is still being served by the currently
installed v1.7.6 firmware. Therefore the v1.6/v1.7.6 old false
"CONNECTION LOST DURING UPLOAD" message can appear ONE LAST TIME while
installing v1.7.7. After v1.7.7 is installed, subsequent OTA updates use the
new reboot-aware UI.


============================================================
v1.7.9 EXPERIMENTAL USB FLASH
============================================================

BASE
----
Rebuilt directly from known-good v1.7.7.
The failed/experimental v1.7.8 USB-CDC maintenance boot path is NOT reused.

SYSTEM MENU
-----------
SAVE PRESET
MAINTENANCE
WIFI SETUP
USB FLASH
SYSTEM INFO
EXIT

USB FLASH
---------
SYSTEM MENU -> USB FLASH -> YES

The sketch calls the Arduino-ESP32 core API:

  usb_persist_restart(RESTART_BOOTLOADER);

On ESP32-S3, Arduino's own USB CDC implementation uses this mechanism to
request the native ROM USB download bootloader.

Expected behavior:
  1. synth is silenced
  2. screen shows USB FLASH
  3. USB is reset/reconfigured by the Arduino-ESP32 core
  4. ESP32-S3 restarts into ROM DOWNLOAD(USB/UART0) mode
  5. Windows re-detects the ESP32-S3 as a programming COM port
  6. Arduino IDE can upload over the wired USB cable
  7. after a successful flash/reset, the newly-written sketch boots normally

RECOVERY
--------
This is experimental because ESP32-S3 USB state transitions can vary with
USB topology/core versions.

If:
  - Windows never detects a programming port
  - the screen goes blank and the board appears stuck
  - Arduino IDE cannot upload

use the existing physical rear BOOT-button connection method and re-flash
the known-good firmware. The menu does not alter NVS or presets.

IMPORTANT
---------
Do not power-cycle during the actual flash write.
Keep the PC USB cable connected before selecting USB FLASH.


============================================================
v1.8.0 WEB OTA V2 - exact-size / verify-before-reboot
============================================================

BASE
----
v1.8.0 keeps the successful v1.7.9 USB FLASH recovery feature.
Normal synth/audio/control behavior is otherwise unchanged.

WHY WEB OTA V2
--------------
The previous browser updater could reach ~100% and then lose the connection
without giving enough evidence that the inactive OTA partition had been
fully written and activated.

V2 changes the update into three explicit phases:

PHASE 1 - PREPARE
-----------------
Browser sends the exact selected .bin file size to:

  /ota-begin?size=...

Din Meter checks:
  - file size is non-zero
  - an OTA app partition exists
  - selected file fits ESP.getFreeSketchSpace()

PHASE 2 - WRITE + VERIFY
------------------------
Din Meter calls:

  Update.begin(EXACT_FILE_SIZE, U_FLASH)

It counts the firmware bytes actually written to flash.

At upload end it requires:

  actual bytes == selected file size

and then calls:

  Update.end(false)

The 'false' is intentional:
an incomplete image is NOT accepted.

If verification succeeds the Din Meter DOES NOT reboot yet.
The browser must first display:

  VERIFIED ON DINMETER: x / x bytes

Then the button changes to:

  REBOOT & APPLY

PHASE 3 - APPLY
---------------
Only after REBOOT & APPLY is pressed does /reboot schedule ESP.restart().

The existing sticky MAINTENANCE flag survives that software restart, so the
new firmware should:
  - boot
  - reconnect to HOME WIFI
  - return to MAINTENANCE automatically
  - answer /health with the new firmware version

DIAGNOSTICS
-----------
New endpoint:

  /ota-status

returns:
  updating
  ready
  expected
  written
  errorCode
  error

The web updater now exposes server-side Update error strings rather than
treating a dropped browser connection as proof of success.

ARDUINO IDE OTA
---------------
Still retained, but ArduinoOTA.handle() is paused while Web OTA is writing
or waiting for reboot so the two OTA paths cannot compete.

RECOVERY
--------
SYSTEM MENU -> USB FLASH remains the reliable wired recovery path.
Rear BOOT button remains the final emergency recovery method.


============================================================
v1.8.2 BANK SELECT compile fix
============================================================

Rebuilt directly from v1.8.0 Web OTA V2.

BANK SELECT changes:
- overlay duration: 280 ms -> 1200 ms
- selected bank number is displayed at text size 4
- continued encoder movement refreshes the 1.2 s timeout
- all non-bank overlays remain unchanged

Compile fix:
The previous draft used getTextBounds(), which is not available on this
M5GFX display object. Banks are 1..8 (one digit), so v1.8.2 uses a fixed
center position instead of measuring the string.

Web OTA V2 and USB FLASH are retained unchanged.


============================================================
v1.8.3 BIN VERSION GUARD
============================================================

The exported file name remains DinMeter_Synth_v1.ino.bin, so Web OTA now
reads the selected local .bin BEFORE upload and displays:

  CURRENT   v1.8.3
  SELECTED  v1.x.x
  ACTION    UPGRADE / SAME VERSION / DOWNGRADE

v1.8.3+ firmware contains:
  DINMETER_FW_VERSION=v1.8.3

Older project builds are also checked for the legacy string:
  DIN SYNTH v...

Safety:
- unknown/non-DinMeter .bin -> upload blocked
- downgrade -> red warning + second confirmation
- same version -> REINSTALL
- newer version -> UPGRADE

Web OTA V2 verification and USB FLASH recovery remain unchanged.
