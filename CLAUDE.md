# CLAUDE.md

## Project overview

PlatformIO repository for an ESP32-S3 on a custom PCB. The board drives four addressable LED strips using FastLED. The light patterns respond to audio from a microphone on the same PCB.

Current state: the firmware still generates artificial noise signals in place of microphone input. Replacing the noise source with live microphone data is the next major step.

Long term: the ESP32-S3 hosts a web page that selects which lighting algorithm runs on the PCB.

Interim: while the ESP32 web UI is in development, the same page is hosted on the developer's laptop so it can be tested without the board.

## Repository layout

- `src/`: ESP32 firmware. Contains the LED rendering, the signal source (currently noise, later microphone), and the algorithm selection logic.
- `data/`: Web UI files meant to be served from the ESP32's filesystem. Uploaded with `pio run -t uploadfs`.
- `server/`: Development web server that runs on the laptop and serves the same UI from `data/` (or a copy of it) for testing.
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

- Microphone: on the PCB. I2S, DMM-4026-B-I2S-EB-R, pins:
SCK 15
WS  16
SD  18
- Pin assignments should live in one header file. Do not hardcode pin numbers elsewhere.

## Firmware conventions

- Build system: PlatformIO. [Fill in: framework, Arduino or ESP-IDF, and the environment name from `platformio.ini`.]
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
- Laptop dev server: [command to run: 'cd server node server.js']

## Working with this repo

- The owner is building this to learn electrical engineering and firmware, and to strengthen their portfolio. When making a change, briefly explain the reasoning behind hardware-related choices (for example power budget and current draw, decoupling, level shifting for LED data lines, and microphone signal conditioning), not just the code.
- Do not change pin assignments or power-related code without asking first, because they depend on the physical PCB.
- Do not modify files in `tools/` unless asked. They are working references.
- Keep the laptop server and the ESP32 web UI in sync, so the same page works in both places.
- Do not commit or push unless asked.

## Open items

- Replace artificial noise signals with microphone input.
- Build the ESP32-hosted web page that selects the active algorithm.
- Define how the web UI talks to the firmware (for example HTTP endpoints or WebSocket) and keep it the same for the laptop server and the ESP32.
