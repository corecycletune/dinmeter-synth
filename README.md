# DinMeter Synth

[![Build DinMeter firmware](https://github.com/corecycletune/dinmeter-synth/actions/workflows/build-firmware.yml/badge.svg)](https://github.com/corecycletune/dinmeter-synth/actions/workflows/build-firmware.yml)

A standalone MIDI synthesizer/controller built around the **M5Stack Din Meter v1.1** and **SAM2695** sound module.

This repository contains the firmware and build automation for a compact hardware synthesizer project. The project combines embedded C++/Arduino development, USB MIDI host handling, physical controls, preset management, Wi-Fi maintenance, and OTA firmware updates.

## Highlights

- M5Stack Din Meter / M5StampS3 based controller
- USB MIDI keyboard host input
- SAM2695 MIDI sound module
- M5Stack 8Angle and ByteButton physical controls
- Multi-oscillator-style control over multiple MIDI channels
- Preset storage using ESP32 Preferences / NVS
- PERFORMANCE and CONFIG user interfaces
- Wi-Fi maintenance mode
- Browser-based OTA firmware update
- Firmware version checking before OTA apply
- GitHub Actions automated firmware compilation
- Build artifacts suitable for OTA deployment

## Hardware

- M5Stack Din Meter v1.1
- M5Stack Unit Hub
- M5Stack ByteButton U192
- M5Stack 8Angle U154
- M5Stack MIDI Unit U187 / SAM2695
- USB MIDI keyboard

## Repository structure

- `DinMeter_Synth_v1.ino` — Arduino entry point
- `SynthApp.cpp/.h` — main synthesizer/controller application
- `WifiMaintenance.cpp/.h` — Wi-Fi, web maintenance UI, and OTA update logic
- `LegacyPresets.h` — preset data
- `README_FIRST.txt` — detailed hardware, behavior, and version notes
- `.github/workflows/build-firmware.yml` — automated firmware build

## Firmware update concept

Development flow:

```text
Source change
   ↓
GitHub
   ↓
GitHub Actions build
   ↓
Firmware .bin artifact
   ↓
DinMeter MAINTENANCE mode
   ↓
Web OTA verification
   ↓
REBOOT & APPLY
```

The device stores Wi-Fi credentials in ESP32 NVS rather than hard-coding them into the repository.

## Project status

Active personal hardware/software project. The firmware architecture and update workflow are still evolving.

## License

This project is source-available for **noncommercial use** under the **PolyForm Noncommercial License 1.0.0**.

Commercial use is not granted by the public license. Separate permission from the copyright holder is required for commercial use.

See [LICENSE](LICENSE) for details.

Third-party libraries and dependencies remain subject to their own licenses.
