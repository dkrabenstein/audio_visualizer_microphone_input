// pins.h -- every GPIO assignment for the custom PCB lives here.
// Do not hardcode pin numbers anywhere else. Changing these requires the
// matching PCB change.

#pragma once

// LED data lines (WS2812B, through the level shifter on the PCB)
#define PIN_LED0 4
#define PIN_LED1 5
#define PIN_LED2 6
#define PIN_LED3 7

// DMM-4026-B-I2S-EB-R microphone (I2S, ESP32 is clock master)
#define PIN_I2S_SCK 15   // bit clock  -> mic SCK
#define PIN_I2S_WS  16   // word select -> mic WS
#define PIN_I2S_SD  18   // data in    <- mic SD
