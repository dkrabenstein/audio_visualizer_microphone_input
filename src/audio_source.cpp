// audio_source.cpp -- I2S microphone -> FFT -> 8 normalised bands.
//
// Signal chain:
//   DMM-4026-B (24-bit I2S, left slot) -> 1024-pt Hann-windowed FFT (50% overlap)
//   -> 8 log-spaced bands -> dB -> adaptive 0..1 mapping -> attack/decay smoothing

#include "audio_source.h"
#include "pins.h"

#include <Arduino.h>
#include <Preferences.h>
#include <driver/i2s.h>
#include <math.h>
#include <string.h>

// ---------------------------------------------------------------- Config
#define I2S_PORT      I2S_NUM_0
#define SAMPLE_RATE   48000     // same rate as the working tools/ mic sketches
#define FFT_N         1024      // bin width = 48000/1024 = 46.9 Hz
#define HOP_N         512       // new samples per FFT -> ~94 spectra/second
#define DMA_BUF_LEN   256
#define DMA_BUF_COUNT 8         // ~42 ms of headroom

// Band edges as FFT bin indices (bin k = k * 46.9 Hz). Roughly one band per
// octave-ish: 47-141, 141-234, 234-422, 422-750, 750-1.4k, 1.4-2.8k, 2.8-6.1k, 6.1-14k Hz
static const uint16_t BAND_EDGES[9] = { 1, 3, 5, 9, 16, 30, 60, 130, 300 };

// Level mapping, in dBFS (0 dB = full-scale sine).
// The top of the window follows the loudest band (fast up, slow down) so quiet
// rooms and loud rooms both use the full 0..1 range. The bottom of the window is a
// per-band noise floor table (tunable, see AudioTuning).
#define MIN_RANGE_DB  (12.0f)   // smallest ceiling-to-floor span, avoids divide-by-~0
#define CEIL_FALL_DB  (0.02f)   // dB the ceiling drops per spectrum (~2 dB/s)

// Per-band noise floor: a fixed table (AudioTuning::floorDb) set from the web UI,
// the "calibrate" button, and saved to flash. There is no automatic tracking, so the
// same sound always maps to the same output. A band reads 0 until it exceeds
// floor + marginDb.
#define CALIB_SPECTRA    282        // ~3 s of spectra averaged by "calibrate"
#define TUNE_VERSION     2          // bump when AudioTuning changes (old saved blobs are ignored)

// Defaults: measured in a quiet room (fan only, after the 90 Hz high-pass).
static const AudioTuning TUNE_DEFAULT = {
    { -78, -80, -84, -86, -85, -89, -92, -95 },  // floorDb
    5.0f,                                        // marginDb
    -55.0f                                       // minCeilDb
};
// DC removal. MEMS mics output a small constant offset; with the Hann window it leaks
// into FFT bin 1 and pins band 0 near full scale. A one-pole running average
// (corner ~7 Hz at 48 kHz, well below the lowest band) is subtracted per sample.
#define DC_ALPHA      0.001f    // diagnostic DC tracker only
#define HPF_HZ        90.0f     // 2nd-order high-pass corner (below band 0 start of ~94 Hz)
#define ATTACK        0.60f     // smoothing toward a higher value (per spectrum)
#define DECAY         0.90f     // multiplier when the value falls

// ---------------------------------------------------------------- State
static AudioSourceKind g_kind = AUDIO_SRC_MIC;
static AudioFeatures   g_features = {};
static portMUX_TYPE    g_featMux = portMUX_INITIALIZER_UNLOCKED;

static int32_t g_i2sBuf[DMA_BUF_LEN];
static float   g_ring[FFT_N];         // most recent FFT_N samples, oldest first
static float   g_re[FFT_N], g_im[FFT_N];
static float   g_window[FFT_N];
static float   g_cos[FFT_N / 2], g_sin[FFT_N / 2];
static uint16_t g_bitrev[FFT_N];

static AudioTuning g_tune = TUNE_DEFAULT;      // guarded by g_featMux
static float g_rawDb[8] = { -120, -120, -120, -120, -120, -120, -120, -120 };  // guarded by g_featMux
static volatile int g_calibLeft = 0;           // >0 while calibrating (set by the web handler)
static float g_ceilDb = -55.0f;
static float g_smooth[8] = {0};
static float g_dc = 0.0f;           // running DC estimate (full-scale units)
static bool  g_dcSeeded = false;

// ---------------------------------------------------------------- High-pass
// 2nd-order Butterworth high-pass (12 dB/oct), transposed direct form II.
// Replaces the old one-pole DC remover: removes DC and infrasonic/rumble energy
// that leaked into FFT bins 1-2 and inflated band 0.
static float g_hpB0, g_hpB1, g_hpB2, g_hpA1, g_hpA2;
static float g_hpZ1 = 0.0f, g_hpZ2 = 0.0f;

static void hp_init() {
    const float w0 = 2.0f * PI * HPF_HZ / SAMPLE_RATE;
    const float c = cosf(w0), alpha = sinf(w0) / (2.0f * 0.70710678f);
    const float a0 = 1.0f + alpha;
    g_hpB0 = (1.0f + c) * 0.5f / a0;
    g_hpB1 = -(1.0f + c) / a0;
    g_hpB2 = g_hpB0;
    g_hpA1 = -2.0f * c / a0;
    g_hpA2 = (1.0f - alpha) / a0;
}

static inline float hp_process(float x) {
    const float y = g_hpB0 * x + g_hpZ1;
    g_hpZ1 = g_hpB1 * x - g_hpA1 * y + g_hpZ2;
    g_hpZ2 = g_hpB2 * x - g_hpA2 * y;
    return y;
}

// ---------------------------------------------------------------- FFT
static void fft_init() {
    const int bits = 10;  // log2(FFT_N)
    for (int i = 0; i < FFT_N; i++) {
        // Hann window: removes the spectral leakage a hard-edged block would cause.
        g_window[i] = 0.5f - 0.5f * cosf(2.0f * PI * i / (FFT_N - 1));
        uint16_t r = 0;
        for (int b = 0; b < bits; b++) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        g_bitrev[i] = r;
    }
    for (int i = 0; i < FFT_N / 2; i++) {
        g_cos[i] = cosf(2.0f * PI * i / FFT_N);
        g_sin[i] = -sinf(2.0f * PI * i / FFT_N);
    }
}

// In-place iterative radix-2 FFT on g_re/g_im (input already bit-reversed).
static void fft_run() {
    for (int len = 2; len <= FFT_N; len <<= 1) {
        const int half = len >> 1;
        const int step = FFT_N / len;
        for (int i = 0; i < FFT_N; i += len) {
            for (int j = 0, k = 0; j < half; j++, k += step) {
                const int a = i + j, b = a + half;
                const float tr = g_re[b] * g_cos[k] - g_im[b] * g_sin[k];
                const float ti = g_re[b] * g_sin[k] + g_im[b] * g_cos[k];
                g_re[b] = g_re[a] - tr;  g_im[b] = g_im[a] - ti;
                g_re[a] += tr;           g_im[a] += ti;
            }
        }
    }
}

// ---------------------------------------------------------------- Analysis
static void analyse_block() {
    // Window + bit-reverse copy.
    for (int i = 0; i < FFT_N; i++) {
        g_re[g_bitrev[i]] = g_ring[i] * g_window[i];
        g_im[g_bitrev[i]] = 0.0f;
    }
    fft_run();

    // Scale so a full-scale sine reads 1.0 in its bin (Hann coherent gain = 0.5).
    const float norm = 4.0f / FFT_N;

    float db[8];
    float loudest = -120.0f;
    for (int b = 0; b < 8; b++) {
        float peak = 0.0f;
        for (int k = BAND_EDGES[b]; k < BAND_EDGES[b + 1]; k++) {
            const float m = sqrtf(g_re[k] * g_re[k] + g_im[k] * g_im[k]) * norm;
            if (m > peak) peak = m;
        }
        db[b] = 20.0f * log10f(peak + 1e-9f);
        if (db[b] > loudest) loudest = db[b];
    }

    // Once a second: loudest band in dBFS. Near -120 means the mic delivers
    // silence (wiring/clock problem); around -60..-30 means real sound arrives.
    static uint32_t lastDbgMs = 0;
    if (millis() - lastDbgMs >= 1000) {
        lastDbgMs = millis();
        Serial.printf("mic: loudest band %.1f dBFS, ceil %.1f dB\n", loudest, g_ceilDb);
    }

#ifdef MIC_DEBUG_BANDS
    {
        static uint32_t lastBandMs = 0;
        static float acc[8]; static float accBin[12]; static int accN = 0;
        for (int b = 0; b < 8; b++) acc[b] += db[b];
        for (int k = 0; k < 12; k++)
            accBin[k] += 20.0f * log10f(sqrtf(g_re[k]*g_re[k] + g_im[k]*g_im[k]) * norm + 1e-9f);
        accN++;
        if (millis() - lastBandMs >= 2000) {
            lastBandMs = millis();
            Serial.printf("BANDS dBFS:");
            for (int b = 0; b < 8; b++) Serial.printf(" %.1f", acc[b] / accN);
            Serial.printf("\nBINS0-11 dBFS:");
            for (int k = 0; k < 12; k++) Serial.printf(" %.1f", accBin[k] / accN);
            Serial.printf("\nFLOORS:");
            for (int b = 0; b < 8; b++) Serial.printf(" %.1f", g_tune.floorDb[b]);
            Serial.printf("\nOUT (0..1):");
            for (int b = 0; b < 8; b++) Serial.printf(" %.2f", g_smooth[b]);
            Serial.printf("\nDC=%.6f\n", g_dc);
            for (int b = 0; b < 8; b++) acc[b] = 0;
            for (int k = 0; k < 12; k++) accBin[k] = 0;
            accN = 0;
        }
    }
#endif

    // Snapshot the tunable settings (the web handler writes them from another task).
    AudioTuning t;
    portENTER_CRITICAL(&g_featMux);
    t = g_tune;
    portEXIT_CRITICAL(&g_featMux);

    // Calibrate: average ~3 s of the current (quiet) room and make it the floor table.
    static float calSum[8];
    static int   calN = 0;
    if (g_calibLeft > 0) {
        if (g_calibLeft == CALIB_SPECTRA) { for (int b = 0; b < 8; b++) calSum[b] = 0; calN = 0; }
        for (int b = 0; b < 8; b++) calSum[b] += db[b];
        calN++;
        if (--g_calibLeft == 0) {
            portENTER_CRITICAL(&g_featMux);
            for (int b = 0; b < 8; b++) g_tune.floorDb[b] = t.floorDb[b] = calSum[b] / calN;
            portEXIT_CRITICAL(&g_featMux);
        }
    }

    // Adaptive ceiling: jump up to a loud band immediately, drift down slowly.
    if (loudest > g_ceilDb) g_ceilDb = loudest;
    else g_ceilDb -= CEIL_FALL_DB;
    if (g_ceilDb < t.minCeilDb) g_ceilDb = t.minCeilDb;

    AudioFeatures f;
    float sum = 0.0f;
    for (int b = 0; b < 8; b++) {
        const float bottom = t.floorDb[b] + t.marginDb;
        float span = g_ceilDb - bottom;
        if (span < MIN_RANGE_DB) span = MIN_RANGE_DB;
        float v = (db[b] - bottom) / span;
        v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        if (v > g_smooth[b]) g_smooth[b] += (v - g_smooth[b]) * ATTACK;
        else                 g_smooth[b] *= DECAY;
        f.bands[b] = g_smooth[b];
        sum += g_smooth[b];
    }
    f.level = sum / 8.0f;
    f.bass  = (f.bands[0] + f.bands[1]) / 2.0f;
    f.mid   = (f.bands[2] + f.bands[3] + f.bands[4]) / 3.0f;
    f.treb  = (f.bands[5] + f.bands[6] + f.bands[7]) / 3.0f;

    portENTER_CRITICAL(&g_featMux);
    g_features = f;
    for (int b = 0; b < 8; b++) g_rawDb[b] = db[b];
    portEXIT_CRITICAL(&g_featMux);
}

// ---------------------------------------------------------------- Tuning API
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void audio_tuning_defaults(AudioTuning *out) { *out = TUNE_DEFAULT; }

void audio_tuning_get(AudioTuning *out, float rawDb[8], bool *calibrating) {
    portENTER_CRITICAL(&g_featMux);
    if (out) *out = g_tune;
    if (rawDb) for (int b = 0; b < 8; b++) rawDb[b] = g_rawDb[b];
    portEXIT_CRITICAL(&g_featMux);
    if (calibrating) *calibrating = g_calibLeft > 0;
}

void audio_tuning_set(const AudioTuning &in) {
    AudioTuning t = in;
    for (int b = 0; b < 8; b++) t.floorDb[b] = clampf(t.floorDb[b], -120.0f, -20.0f);
    t.marginDb   = clampf(t.marginDb,   0.0f,   20.0f);
    t.minCeilDb  = clampf(t.minCeilDb,  -80.0f, -20.0f);
    portENTER_CRITICAL(&g_featMux);
    g_tune = t;
    portEXIT_CRITICAL(&g_featMux);
}

void audio_tuning_calibrate() { g_calibLeft = CALIB_SPECTRA; }

// Saved in NVS (flash) as one versioned blob. Call save only on an explicit user
// action -- not per slider tick -- to spare the flash.
struct TuneBlob { uint8_t version; AudioTuning t; };

bool audio_tuning_save() {
    TuneBlob blob; blob.version = TUNE_VERSION;
    audio_tuning_get(&blob.t, nullptr, nullptr);
    Preferences p;
    if (!p.begin("mic", false)) return false;
    const size_t n = p.putBytes("tune", &blob, sizeof(blob));
    p.end();
    return n == sizeof(blob);
}

static void audio_tuning_load() {
    Preferences p;
    if (!p.begin("mic", true)) return;
    TuneBlob blob;
    if (p.getBytesLength("tune") == sizeof(blob) &&
        p.getBytes("tune", &blob, sizeof(blob)) == sizeof(blob) &&
        blob.version == TUNE_VERSION) {
        audio_tuning_set(blob.t);
    }
    p.end();
}

void audio_tuning_reset() {
    audio_tuning_set(TUNE_DEFAULT);
    Preferences p;
    if (p.begin("mic", false)) { p.remove("tune"); p.end(); }
}

// ---------------------------------------------------------------- Capture task
static void micTask(void *) {
    static float hopBuf[HOP_N];
    int fill = 0;
    for (;;) {
        size_t bytesRead = 0;
        if (i2s_read(I2S_PORT, g_i2sBuf, sizeof(g_i2sBuf), &bytesRead, portMAX_DELAY) != ESP_OK) {
            vTaskDelay(1);
            continue;
        }
        const int n = (int)(bytesRead / sizeof(int32_t));

        for (int i = 0; i < n; i++) {
            // 24-bit sample sits left-justified in the 32-bit slot -> >>8, then to -1..1.
            float s = (float)(g_i2sBuf[i] >> 8) * (1.0f / 8388608.0f);

            // Track raw DC for diagnostics only (the high-pass below does the removal).
            if (!g_dcSeeded) {
                g_dc = s; g_dcSeeded = true;
                // Settle the high-pass on the initial offset so boot has no step transient
                // that would park the loudness ceiling high for seconds.
                for (int i = 0; i < 4000; i++) hp_process(s);
            }
            g_dc += (s - g_dc) * DC_ALPHA;
            s = hp_process(s);

            // Collect HOP_N new samples, then slide the ring and run one FFT
            // (50% overlap with the previous window).
            hopBuf[fill++] = s;
            if (fill == HOP_N) {
                memmove(g_ring, g_ring + HOP_N, (FFT_N - HOP_N) * sizeof(float));
                memcpy(g_ring + (FFT_N - HOP_N), hopBuf, HOP_N * sizeof(float));
                fill = 0;
                analyse_block();
            }
        }
    }
}

// ---------------------------------------------------------------- Public API
bool audio_source_begin(AudioSourceKind kind) {
    g_kind = kind;
    if (kind != AUDIO_SRC_MIC) return true;

    fft_init();
    hp_init();
    audio_tuning_load();   // saved web-UI settings, if any

    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
    cfg.sample_rate          = SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;  // mic L/R pin tied to GND
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = DMA_BUF_COUNT;
    cfg.dma_buf_len          = DMA_BUF_LEN;
    cfg.use_apll             = false;

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = PIN_I2S_SCK;
    pins.ws_io_num    = PIN_I2S_WS;
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num  = PIN_I2S_SD;

    if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) return false;
    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) return false;

    // Core 0 shares with Wi-Fi/HTTP (priority 2); the mic must not drop DMA
    // buffers, so it runs slightly higher. Render stays alone on Core 1.
    xTaskCreatePinnedToCore(micTask, "mic_task", 8192, nullptr, 4, nullptr, 0);
    return true;
}

AudioSourceKind audio_source_kind() { return g_kind; }

void audio_source_get(AudioFeatures *out) {
    portENTER_CRITICAL(&g_featMux);
    *out = g_features;
    portEXIT_CRITICAL(&g_featMux);
}

void audio_source_push_udp(const AudioFeatures &f) {
    if (g_kind != AUDIO_SRC_UDP) return;
    portENTER_CRITICAL(&g_featMux);
    g_features = f;
    portEXIT_CRITICAL(&g_featMux);
}
