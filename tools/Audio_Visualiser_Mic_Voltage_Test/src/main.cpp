#include <Arduino.h>
#include <FastLED.h>   // Install "FastLED" from the Arduino Library Manager
#include <WiFi.h>

// LED data out (goes through the 74AHCT125 level shifter)
#define LED_PIN_A     4
#define LED_PIN_B     5
#define LED_PIN_C     6
#define LED_PIN_D     7

// ---------------- LED config ----------------
#define NUM_LEDS_A    180
#define NUM_LEDS_B    120
#define NUM_LEDS_C    180
#define NUM_LEDS_D    120

#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB

// 180 LEDs at full white can pull ~10.8 A. FastLED scales brightness to stay
// under this limit. Set it to what your 5V supply can actually deliver.
#define LED_VOLTS         5
#define MAX_MILLIAMPS_PER_LINE 8000

// Helper: scale down an array ONLY if its current exceeds the limit
void limitPinCurrent(CRGB* strip, int numLeds, uint8_t volts, uint32_t max_mA) {
    // 1. Calculate estimated unscaled power for this specific array in mW
    uint32_t strip_mW = calculate_unscaled_power_mW(strip, numLeds);
    
    // 2. Convert mW to mA: I (mA) = P (mW) / V
    uint32_t estimated_mA = strip_mW / volts;

    // 3. If draw exceeds the line's safety threshold, dim only this strip
    if (estimated_mA > max_mA) {
        // FastLED 8-bit scale factor: (target / actual) * 255
        uint8_t scaleFactor = (uint8_t)((max_mA * 255ULL) / estimated_mA);
        
        // In-place non-destructive scaling across this array
        for (int i = 0; i < numLeds; i++) {
            strip[i].nscale8_video(scaleFactor);
        }
    }
}

static CRGB leds_A[NUM_LEDS_A];
static CRGB leds_B[NUM_LEDS_B];
static CRGB leds_C[NUM_LEDS_C];
static CRGB leds_D[NUM_LEDS_D];

void setup() {
    Serial.begin(115200);
    delay(1000);

    FastLED.addLeds<LED_TYPE, LED_PIN_A, COLOR_ORDER>(leds_A, NUM_LEDS_A);
    FastLED.addLeds<LED_TYPE, LED_PIN_B, COLOR_ORDER>(leds_B, NUM_LEDS_B);
    FastLED.addLeds<LED_TYPE, LED_PIN_C, COLOR_ORDER>(leds_C, NUM_LEDS_C);
    FastLED.addLeds<LED_TYPE, LED_PIN_D, COLOR_ORDER>(leds_D, NUM_LEDS_D);

    FastLED.setBrightness(77);
    FastLED.clear(true);

    Serial.println("Music LEDs ready.");
    Serial.println(WiFi.localIP());
}

void loop() {

    fill_solid(leds_A, NUM_LEDS_A, CRGB::White);
    fill_solid(leds_B, NUM_LEDS_B, CRGB::White);
    fill_solid(leds_C, NUM_LEDS_C, CRGB::White);
    fill_solid(leds_D, NUM_LEDS_D, CRGB::White);

    limitPinCurrent(leds_A, NUM_LEDS_A, LED_VOLTS, MAX_MILLIAMPS_PER_LINE);
    limitPinCurrent(leds_B, NUM_LEDS_B, LED_VOLTS, MAX_MILLIAMPS_PER_LINE);
    limitPinCurrent(leds_C, NUM_LEDS_C, LED_VOLTS, MAX_MILLIAMPS_PER_LINE);
    limitPinCurrent(leds_D, NUM_LEDS_D, LED_VOLTS, MAX_MILLIAMPS_PER_LINE);

    FastLED.show();
}
