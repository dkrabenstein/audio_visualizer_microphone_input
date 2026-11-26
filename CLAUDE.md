# CLAUDE.md

## Project overview

PlatformIO repository for an ESP32-S3 on a custom PCB. The board drives four addressable LED strips using FastLED. The light patterns respond to audio from a microphone on the same PCB.

Current state: the firmware still generates artificial noise signals in place of microphone input. Replacing the noise source with live microphone data is the next major step.

Long term: the ESP32-S3 hosts a web page that selects which lighting algorithm runs on the PCB.

Interim: while the ESP32 web UI is in development, the same page is hosted on the developer's laptop so it can be tested without the board.

## Repository layout

- `src/`: ESP32 firmware. Contains the LED rendering, the signal source (currently noise, later microphone), and the algorithm selection logic.
- `data/`: The web UI (`data/www/`), the single source of truth. Served from the ESP32's filesystem (upload with `pio run -t uploadfs`) and by the laptop server.
- `server/`: Development web server that runs on the laptop and serves `data/www/` directly (no second copy). It also streams simulated audio to the board and serves `/host.js`, which switches the page into laptop mode (shows the ESP32 connection panel).
- `tools/`: Standalone, working LED algorithms that do not depend on the website. Treat these as the reference implementations for the lighting behavior.
- `platformio.ini`: Build environments and library dependencies.

## Hardware

- MCU: ESP32-S3, on a custom PCB.
- LEDs: 4 addressable LED strips, driven with FastLED. WS2812B LEDs:
LED row 1: 180 LEDs, pin 4
LED row 1: 100 LEDs, pin 5
LED row 1: 180 LEDs, pin 6
LED row 1: 101 LEDs, pin 7
40A, 5V power supply

Strip lengths are configurable at runtime. A test configuration of {60, 1, 1, 1} LEDs (one short strip, the other three at one LED each) is used during development and is a valid configuration, not a misconfiguration.

- Microphone: on the PCB. I2S, DMM-4026-B-I2S-EB-R, pins:
SCK 15
WS  16
SD  18
- Pin assignments should live in one header file. Do not hardcode pin numbers elsewhere.

## Firmware conventions

- Build system: PlatformIO, Arduino framework. Environment `esp32-s3-devkitc-1-n32r16v` (USB upload). Environment `ota` (wireless upload, `pio run -e ota -t upload`; `upload_port` is a hardcoded IP, see the note in `platformio.ini`).
- The LED update loop must not block. Avoid `delay()` in the render path.
- Put the signal source behind one interface. Algorithms should receive audio features and not know whether they came from noise or from the microphone. Swapping noise for mic input should touch only the source module.
- Keep the noise generator available as a fallback or test source after mic input works.
- Algorithms should be selectable at runtime, so the web UI can choose one without reflashing.
- Port working code from `tools/` rather than rewriting algorithms from scratch, unless a change is needed for the new input or the web control.

## Build and run

- Build: `pio run`
- Flash firmware: `pio run -t upload`
- Flash web files: `pio run -t uploadfs`
- Serial monitor: `pio device monitor`
- Laptop dev server: `cd server && node server.js`

## Working with this repo

- The owner is building this to learn electrical engineering and firmware, and to strengthen their portfolio. When making a change, briefly explain the reasoning behind hardware-related choices (for example power budget and current draw, decoupling, level shifting for LED data lines, and microphone signal conditioning), not just the code.
- Do not change pin assignments or power-related code without asking first, because they depend on the physical PCB.
- Do not modify files in `tools/` unless asked. They are working references.
- Keep the laptop server and the ESP32 web UI in sync, so the same page works in both places.
- Do not commit or push unless asked.

## Open items

- Replace artificial noise signals with microphone input.
- Build the ESP32-hosted web page that selects the active algorithm.
