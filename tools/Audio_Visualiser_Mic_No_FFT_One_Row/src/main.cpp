#include <Arduino.h>
#include <driver/i2s.h>
#include <FastLED.h>   // Install "FastLED" from the Arduino Library Manager

// ---------------- Pins ----------------
// Mic (per audio_visualizer_mic.kicad_sch)
#define I2S_SCK     15
#define I2S_WS      16
#define I2S_SD      18

// LED data out (goes through the 74AHCT125 level shifter)
#define LED_PIN     4

// ---------------- LED config ----------------
#define NUM_LEDS    180
#define LED_TYPE    WS2812B
#define COLOR_ORDER GRB

// 180 LEDs at full white can pull ~10.8 A. FastLED scales brightness to stay
// under this limit. Set it to what your 5V supply can actually deliver.
#define LED_VOLTS         5
#define LED_MAX_MILLIAMPS 4000

// ---------------- Audio config ----------------
#define I2S_PORT    I2S_NUM_0
#define BUFFER_LEN  128
#define SAMPLE_RATE 48000

// ---------------- Tuning ----------------
#define BEAT_RATIO       1.5f   // peak must exceed slow average by this factor to count as a beat
#define BEAT_MIN_MS      100    // minimum time between beats
#define FLASH_DECAY      0.85f  // per-frame flash fade (lower = faster fade)
#define LEVEL_DECAY      0.90f  // per-frame envelope fall-off
#define MIN_CEILING      20000.0f  // loudest-level floor so silence isn't auto-gained to full scale
#define NOISE_GATE       0.05f  // normalized level below this is treated as silence

static int32_t samples[BUFFER_LEN];
static CRGB leds[NUM_LEDS];

static float dcEstimate = 0.0f;  // running DC offset (24-bit units)
static float envelope   = 0.0f;  // smoothed loudness
static float avgLevel   = 0.0f;  // slow average for beat detection
static float ceilLevel  = MIN_CEILING;  // adaptive "loud" reference
static float flash      = 0.0f;  // 0..255 beat flash brightness
static uint8_t hue      = 0;
static uint32_t lastBeat = 0;

static void checkErr(esp_err_t err, const char *what) {
    if (err != ESP_OK) {
        Serial.printf("ERROR: %s failed (%d)\n", what, (int)err);
        while (true) delay(1000);
    }
}

static void setupMic() {
    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate          = SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;  // mic L/R pin tied to GND
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 8;   // extra headroom while FastLED.show() blocks
    cfg.dma_buf_len          = BUFFER_LEN;
    cfg.use_apll             = false;

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = I2S_SCK;
    pins.ws_io_num    = I2S_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num  = I2S_SD;

    checkErr(i2s_driver_install(I2S_PORT, &cfg, 0, NULL), "i2s_driver_install");
    checkErr(i2s_set_pin(I2S_PORT, &pins), "i2s_set_pin");
}

// Returns DC-removed peak of one audio chunk, or -1 on failure.
static int32_t readPeak() {
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_PORT, samples, sizeof(samples), &bytesRead, portMAX_DELAY);
    if (err != ESP_OK) return -1;

    int frames = bytesRead / sizeof(int32_t);
    if (frames <= 0) return -1;

    int32_t peak = 0;
    for (int i = 0; i < frames; i++) {
        int32_t val = samples[i] >> 8;  // 24-bit data, left-justified in 32-bit slot
        dcEstimate += (val - dcEstimate) * 0.001f;
        int32_t ac = abs(val - (int32_t)dcEstimate);
        if (ac > peak) peak = ac;
    }
    return peak;
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    setupMic();

    FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
    FastLED.setMaxPowerInVoltsAndMilliamps(LED_VOLTS, LED_MAX_MILLIAMPS);
    FastLED.setBrightness(255);
    FastLED.clear(true);

    Serial.println("Music LEDs ready.");
}

void loop() {
    int32_t peakI = readPeak();
    if (peakI < 0) return;
    float peak = (float)peakI;

    // Envelope: fast attack, slow decay
    if (peak > envelope) envelope = peak;
    else envelope *= LEVEL_DECAY;

    // Adaptive ceiling: tracks recent loud peaks, drifts down slowly
    if (envelope > ceilLevel) ceilLevel = envelope;
    else ceilLevel -= (ceilLevel - MIN_CEILING) * 0.0005f;

    float level = constrain(envelope / ceilLevel, 0.0f, 1.0f);
    if (level < NOISE_GATE) level = 0.0f;

    // Beat detection against a slow average
    uint32_t now = millis();
    bool beat = (peak > avgLevel * BEAT_RATIO) && (level > 0.2f) && (now - lastBeat > BEAT_MIN_MS);
    avgLevel += (peak - avgLevel) * 0.02f;

    if (beat) {
        lastBeat = now;
        flash = 255.0f;
        hue += 32;  // jump to a new color on each beat
    } else {
        flash *= FLASH_DECAY;
    }

    // Brightness: beat flash on top of a level-driven base glow
    uint8_t base = (uint8_t)(level * level * 120.0f);
    uint8_t val  = (uint8_t)max((float)base, flash);

    fill_solid(leds, NUM_LEDS, CHSV(hue, 255, val));
    FastLED.show();
}
