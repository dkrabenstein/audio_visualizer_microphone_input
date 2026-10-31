#include <Arduino.h>
#include <driver/i2s.h>

// Pins match audio_visualizer_mic.kicad_sch:
//   Mic SCK  -> GPIO15
//   Mic WS   -> GPIO16
//   Mic DATA -> GPIO18
#define I2S_SCK     15
#define I2S_WS      16
#define I2S_SD      18

#define I2S_PORT    I2S_NUM_0
#define BUFFER_LEN  128
#define SAMPLE_RATE 48000

static int32_t samples[BUFFER_LEN];
static float dcEstimate = 0.0f;  // running DC offset (in 24-bit units)

static void checkErr(esp_err_t err, const char *what) {
    if (err != ESP_OK) {
        Serial.printf("ERROR: %s failed (%d)\n", what, (int)err);
        while (true) delay(1000);
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate          = SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;  // mic L/R pin is tied to GND
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 4;
    cfg.dma_buf_len          = BUFFER_LEN;
    cfg.use_apll             = false;

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;  // no MCLK (otherwise 0 = GPIO0)
    pins.bck_io_num   = I2S_SCK;
    pins.ws_io_num    = I2S_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num  = I2S_SD;

    checkErr(i2s_driver_install(I2S_PORT, &cfg, 0, NULL), "i2s_driver_install");
    checkErr(i2s_set_pin(I2S_PORT, &pins), "i2s_set_pin");

    Serial.println("I2S Mic Test Ready. Speak or tap the mic.");
}

void loop() {
    size_t bytesRead = 0;
    esp_err_t err = i2s_read(I2S_PORT, samples, sizeof(samples), &bytesRead, portMAX_DELAY);
    if (err != ESP_OK) {
        Serial.printf("i2s_read error (%d)\n", (int)err);
        return;
    }

    int frames = bytesRead / sizeof(int32_t);
    if (frames <= 0) return;

    int32_t peak = 0;
    for (int i = 0; i < frames; i++) {
        // Mic data is 24-bit, left-justified in the 32-bit slot
        int32_t val = samples[i] >> 8;

        // Track and remove DC offset so a quiet room reads near zero
        dcEstimate += (val - dcEstimate) * 0.001f;
        int32_t ac = abs(val - (int32_t)dcEstimate);

        if (ac > peak) peak = ac;
    }

    // Open Tools -> Serial Plotter for a real-time graph
    Serial.println(peak);
}
