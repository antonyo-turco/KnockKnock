#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <time.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "config.h"
#include "feature_extraction.h"
#include "tinyml_training.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Phase durations
// ─────────────────────────────────────────────────────────────────────────────

// Production
#define EXPLORING_DURATION_MS  (24UL * 3600UL * 1000UL)
#define TRAINING_DURATION_MS   (24UL * 3600UL * 1000UL)

// Test (15 min)
// #define EXPLORING_DURATION_MS  (15UL * 60UL * 1000UL)
// #define TRAINING_DURATION_MS   (15UL * 60UL * 1000UL)

// Quick bench (2 min)
// #define EXPLORING_DURATION_MS    ( 2UL * 60UL * 1000UL)
// #define TRAINING_DURATION_MS     ( 2UL * 60UL * 1000UL)

// ─────────────────────────────────────────────────────────────────────────────
//  Hardware
// ─────────────────────────────────────────────────────────────────────────────

#define SIGNAL_GAIN          1.0f
#define OLED_WIDTH           128
#define OLED_HEIGHT           64
#define OLED_RESET            -1
#define OLED_REFRESH_WINDOWS   1

#define PIN_CS            4
#define PIN_MISO          3
#define PIN_MOSI          2
#define PIN_SCK           1
#define SPI_CLOCK_FREQ    1000000

#define ADXL362_CMD_READ_REG   0x0B
#define ADXL362_CMD_WRITE_REG  0x0A
#define ADXL362_REG_DEVID_AD   0x00
#define ADXL362_REG_DEVID_MST  0x01
#define ADXL362_REG_XDATA_L    0x0E
#define ADXL362_REG_YDATA_L    0x10
#define ADXL362_REG_ZDATA_L    0x12
#define ADXL362_REG_FILTER_CTL 0x2C
#define ADXL362_REG_POWER_CTL  0x2D

// ─────────────────────────────────────────────────────────────────────────────
//  LED
// ─────────────────────────────────────────────────────────────────────────────

static const uint8_t ALERT_LED_PIN = LED_BUILTIN;

enum class LedPattern : uint8_t {
    OFF, PULSE, SLOW_BLINK, FAST_BLINK, ON
};

static LedPattern    g_led_pattern = LedPattern::OFF;
static unsigned long g_led_last_ms = 0;
static int           g_led_step    = 0;
static bool          g_led_state   = false;

static void led_update() {
    unsigned long now = millis();
    switch (g_led_pattern) {
        case LedPattern::OFF:        digitalWrite(ALERT_LED_PIN, LOW);  break;
        case LedPattern::ON:         digitalWrite(ALERT_LED_PIN, HIGH); break;
        case LedPattern::SLOW_BLINK:
            if (now - g_led_last_ms >= 800) {
                g_led_state = !g_led_state;
                digitalWrite(ALERT_LED_PIN, g_led_state ? HIGH : LOW);
                g_led_last_ms = now;
            }
            break;
        case LedPattern::FAST_BLINK:
            if (now - g_led_last_ms >= 120) {
                g_led_state = !g_led_state;
                digitalWrite(ALERT_LED_PIN, g_led_state ? HIGH : LOW);
                g_led_last_ms = now;
            }
            break;
        case LedPattern::PULSE: {
            static const uint16_t dur[] = {100, 100, 100, 2700};
            static const bool     st[]  = {true, false, true, false};
            if (now - g_led_last_ms >= dur[g_led_step]) {
                g_led_step  = (g_led_step + 1) % 4;
                g_led_state = st[g_led_step];
                digitalWrite(ALERT_LED_PIN, g_led_state ? HIGH : LOW);
                g_led_last_ms = now;
            }
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Types & globals
// ─────────────────────────────────────────────────────────────────────────────

struct Sample { int16_t x, y, z; };
enum class SystemMode : uint8_t { EXPLORING, TRAINING, INFERENCE };

static Sample        g_window[SAMPLE_COUNT];
static int           g_window_index   = 0;
static uint32_t      g_window_seq     = 0;
static unsigned long g_last_sample_ms = 0;
static unsigned long g_phase_start_ms = 0;

static KMeansModel   g_model;
static SystemMode    g_mode;

// Current time features — updated at every NTP sync and at boot
static float         g_time_sin       = 0.0f;
static float         g_time_cos       = 1.0f;  // default: midnight
static bool          g_time_valid     = false;

// NTP sync scheduling
static unsigned long g_last_ntp_sync_ms = 0;
static bool          g_ntp_sync_pending = false;

static Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
static bool oled_ok = false;

// ─────────────────────────────────────────────────────────────────────────────
//  NTP sync
//
//  Called between two windows — may block for up to
//  WIFI_TIMEOUT_MS + NTP_TIMEOUT_MS (~18 s worst case).
//  During this time sampling is paused; at most one window is skipped.
// ─────────────────────────────────────────────────────────────────────────────

static void oled_show_message(const char *line1, const char *line2 = nullptr);

static void ntp_sync() {
    Serial.println("[NTP] Starting time sync...");
    oled_show_message("Time sync...", "Connecting WiFi");

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
        delay(200);
        led_update();
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[NTP] WiFi timeout — sync skipped.");
        oled_show_message("Time sync", "WiFi failed");
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(500);
        return;
    }

    Serial.println("[NTP] WiFi connected. Fetching time...");
    oled_show_message("Time sync...", "NTP request");

    configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);

    // Wait for valid time
    struct tm timeinfo;
    t0 = millis();
    bool got_time = false;
    while (millis() - t0 < NTP_TIMEOUT_MS) {
        if (getLocalTime(&timeinfo)) { got_time = true; break; }
        delay(200);
        led_update();
    }

    // Disconnect WiFi immediately to save power
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);

    if (!got_time) {
        Serial.println("[NTP] NTP timeout — keeping previous time.");
        oled_show_message("Time sync", "NTP failed");
        delay(500);
        return;
    }

    // Update global time features
    g_time_valid = get_time_features(g_time_sin, g_time_cos);
    g_last_ntp_sync_ms = millis();
    g_ntp_sync_pending = false;

    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &timeinfo);
    Serial.printf("[NTP] Time synced: %s  sin=%.3f  cos=%.3f\n",
                  tbuf, g_time_sin, g_time_cos);

    char obuf[24];
    snprintf(obuf, sizeof(obuf), "Time: %s", tbuf);
    oled_show_message("Sync OK", obuf);
    delay(1000);
}

// ─────────────────────────────────────────────────────────────────────────────
//  OLED
// ─────────────────────────────────────────────────────────────────────────────

static void oled_progress_bar(int x, int y, int w, int h, uint8_t pct) {
    oled.drawRect(x, y, w, h, SSD1306_WHITE);
    int fill = (int)((w - 2) * pct / 100);
    if (fill > 0) oled.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
}

static void fmt_time(char *buf, size_t len, uint32_t secs) {
    if (secs >= 3600)
        snprintf(buf, len, "%luh%02lum",
                 (unsigned long)(secs / 3600),
                 (unsigned long)((secs % 3600) / 60));
    else
        snprintf(buf, len, "%lum%02lus",
                 (unsigned long)(secs / 60),
                 (unsigned long)(secs % 60));
}

static void oled_show_message(const char *line1, const char *line2) {
    if (!oled_ok) return;
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);  oled.print(line1);
    if (line2) { oled.setCursor(0, 16); oled.print(line2); }
    oled.display();
}

static void oled_show_exploring() {
    if (!oled_ok) return;
    uint32_t elapsed = millis() - g_phase_start_ms;
    if (elapsed > EXPLORING_DURATION_MS) elapsed = EXPLORING_DURATION_MS;
    uint8_t pct = (uint8_t)(elapsed * 100 / EXPLORING_DURATION_MS);
    char tbuf[12];
    fmt_time(tbuf, sizeof(tbuf), (EXPLORING_DURATION_MS - elapsed) / 1000);

    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(2); oled.setCursor(0, 0);  oled.print(F("EXPLORE"));
    oled.setTextSize(1); oled.setCursor(0, 18); oled.printf("%u%%  rem:%s", pct, tbuf);
    oled_progress_bar(0, 30, 128, 10, pct);
    oled.setCursor(0, 44); oled.printf("novel:%u%%  n=%lu",
                                        exploring_novelty_pct(),
                                        (unsigned long)g_model.total_samples);
    // Show current time if valid
    if (g_time_valid) {
        struct tm ti;
        if (getLocalTime(&ti)) {
            oled.setCursor(0, 54);
            oled.printf("%02d:%02d", ti.tm_hour, ti.tm_min);
        }
    }
    oled.display();
}

static void oled_show_training() {
    if (!oled_ok) return;
    uint32_t elapsed = millis() - g_phase_start_ms;
    if (elapsed > TRAINING_DURATION_MS) elapsed = TRAINING_DURATION_MS;
    uint8_t pct = (uint8_t)(elapsed * 100 / TRAINING_DURATION_MS);
    char tbuf[12];
    fmt_time(tbuf, sizeof(tbuf), (TRAINING_DURATION_MS - elapsed) / 1000);

    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(2); oled.setCursor(0, 0);  oled.print(F("TRAINING"));
    oled.setTextSize(1); oled.setCursor(0, 18); oled.printf("%u%%  rem:%s", pct, tbuf);
    oled_progress_bar(0, 30, 128, 10, pct);
    oled.setCursor(0, 44); oled.printf("n=%lu  k=%d",
                                        (unsigned long)g_model.total_samples, KMEANS_K);
    if (g_time_valid) {
        struct tm ti;
        if (getLocalTime(&ti)) {
            oled.setCursor(0, 54);
            oled.printf("%02d:%02d", ti.tm_hour, ti.tm_min);
        }
    }
    oled.display();
}

static void oled_show_inference(bool baseline, float dist,
                                 float impact, float p99, int cluster) {
    if (!oled_ok) return;
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);

    if (baseline) {
        oled.setTextSize(2); oled.setCursor(0, 0);  oled.print(F("BASELINE"));
        oled.setTextSize(1);
        oled.setCursor(0, 20); oled.printf("dist  : %.4f", dist);
        oled.setCursor(0, 32); oled.printf("impact: %.4f", impact);
        oled.setCursor(0, 44); oled.printf("m_p99 : %.2f",  p99);
        oled.setCursor(0, 56); oled.printf("cluster: C%d", cluster);
    } else {
        oled.fillRect(0, 0, OLED_WIDTH, OLED_HEIGHT, SSD1306_WHITE);
        oled.setTextColor(SSD1306_BLACK);
        oled.setTextSize(2); oled.setCursor(4, 2);  oled.print(F("DEVIATION"));
        oled.setTextSize(1);
        oled.setCursor(4, 22); oled.printf("dist  : %.4f", dist);
        oled.setCursor(4, 34); oled.printf("impact: %.4f", impact);
        oled.setCursor(4, 46); oled.printf("cluster: C%d", cluster);
    }
    oled.display();
}

// ─────────────────────────────────────────────────────────────────────────────
//  ADXL362
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t adxl362_read_reg(uint8_t reg) {
    SPI.beginTransaction(SPISettings(SPI_CLOCK_FREQ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    SPI.transfer(ADXL362_CMD_READ_REG); SPI.transfer(reg);
    uint8_t v = SPI.transfer(0x00);
    digitalWrite(PIN_CS, HIGH); SPI.endTransaction();
    return v;
}

static void adxl362_write_reg(uint8_t reg, uint8_t val) {
    SPI.beginTransaction(SPISettings(SPI_CLOCK_FREQ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    SPI.transfer(ADXL362_CMD_WRITE_REG); SPI.transfer(reg); SPI.transfer(val);
    digitalWrite(PIN_CS, HIGH); SPI.endTransaction();
}

static int16_t adxl362_read_axis(uint8_t lsb_reg) {
    uint8_t lsb = adxl362_read_reg(lsb_reg);
    uint8_t msb = adxl362_read_reg(lsb_reg + 1);
    return static_cast<int16_t>((static_cast<int16_t>(msb) << 8) | lsb);
}

static void init_sensor() {
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    pinMode(PIN_CS, OUTPUT);
    digitalWrite(PIN_CS, HIGH);
    delay(100);
    uint8_t id_ad  = adxl362_read_reg(ADXL362_REG_DEVID_AD);
    uint8_t id_mst = adxl362_read_reg(ADXL362_REG_DEVID_MST);
    Serial.printf("ADXL362 DevID: 0x%02X 0x%02X\n", id_ad, id_mst);
    if (id_ad != 0xAD || id_mst != 0x1D) {
        Serial.println("ERROR: ADXL362 not detected!");
        oled_show_message("ERROR:", "ADXL362 not found");
    }
    adxl362_write_reg(ADXL362_REG_FILTER_CTL, 0x13);
    adxl362_write_reg(ADXL362_REG_POWER_CTL,  0x02);
    delay(100);
}

static bool read_sample(Sample &s) {
    s.x = static_cast<int16_t>(adxl362_read_axis(ADXL362_REG_XDATA_L) * SIGNAL_GAIN);
    s.y = static_cast<int16_t>(adxl362_read_axis(ADXL362_REG_YDATA_L) * SIGNAL_GAIN);
    s.z = static_cast<int16_t>(adxl362_read_axis(ADXL362_REG_ZDATA_L) * SIGNAL_GAIN);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Phase transitions
// ─────────────────────────────────────────────────────────────────────────────

static void transition_to_training() {
    g_led_pattern = LedPattern::ON; led_update();
    oled_show_message("Exploring done!", "Running k++...");
    bool ok = exploring_finalize(g_model);
    if (!ok) Serial.println("[SYSTEM] WARNING: novelty buffer sparse.");
    g_phase_start_ms = millis();
    g_mode           = SystemMode::TRAINING;
    g_led_pattern    = LedPattern::SLOW_BLINK;
    Serial.printf("[SYSTEM] EXPLORING done -> TRAINING for %lu s.\n",
                  TRAINING_DURATION_MS / 1000UL);
    oled_show_message("TRAINING", "started..."); delay(1500);
}

static void transition_to_inference() {
    g_led_pattern = LedPattern::ON; led_update();
    oled_show_message("Training done!", "Finalising...");
    training_finalize(g_model);
    training_save(g_model);
    g_mode        = SystemMode::INFERENCE;
    g_led_pattern = LedPattern::OFF;
    Serial.println("[SYSTEM] TRAINING done -> INFERENCE mode.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Window processing
// ─────────────────────────────────────────────────────────────────────────────

static void process_window() {
    static int16_t xb[SAMPLE_COUNT], yb[SAMPLE_COUNT], zb[SAMPLE_COUNT];
    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        xb[i] = g_window[i].x;
        yb[i] = g_window[i].y;
        zb[i] = g_window[i].z;
    }

    // Update time features from RTC every window (cheap — no network involved)
    get_time_features(g_time_sin, g_time_cos);

    InferenceFeatures feat = compute_features(
        xb, yb, zb, SAMPLE_COUNT, SAMPLING_RATE_HZ,
        g_time_sin, g_time_cos);

    switch (g_mode) {
        case SystemMode::EXPLORING:
            exploring_update(g_model, feat);
            if (g_window_seq % OLED_REFRESH_WINDOWS == 0) oled_show_exploring();
            if (g_window_seq % 150 == 0)
                Serial.printf("[EXPLORE] win=%lu  n=%lu  novel=%u%%\n",
                              (unsigned long)g_window_seq,
                              (unsigned long)g_model.total_samples,
                              exploring_novelty_pct());
            if (millis() - g_phase_start_ms >= EXPLORING_DURATION_MS)
                transition_to_training();
            break;

        case SystemMode::TRAINING:
            training_update(g_model, feat);
            if (g_window_seq % OLED_REFRESH_WINDOWS == 0) oled_show_training();
            if (g_window_seq % 150 == 0) {
                uint32_t elapsed = millis() - g_phase_start_ms;
                Serial.printf("[TRAIN] win=%lu  n=%lu  %lu%%\n",
                              (unsigned long)g_window_seq,
                              (unsigned long)g_model.total_samples,
                              elapsed * 100 / TRAINING_DURATION_MS);
            }
            if (millis() - g_phase_start_ms >= TRAINING_DURATION_MS)
                transition_to_inference();
            break;

        case SystemMode::INFERENCE: {
            float dist    = 0.0f;
            int   cluster = -1;
            bool  ok      = training_is_baseline(g_model, feat, &dist, &cluster);
            g_led_pattern = ok ? LedPattern::OFF : LedPattern::FAST_BLINK;
            if (g_window_seq % OLED_REFRESH_WINDOWS == 0)
                oled_show_inference(ok, dist, feat.impact_score, feat.m_p99, cluster);
            Serial.printf("[INF] win=%lu  impact=%.4f  m_p99=%.2f  dist=%.4f  C%d  %s\n",
                          (unsigned long)g_window_seq,
                          feat.impact_score, feat.m_p99, dist, cluster,
                          ok ? "BASELINE" : "*** DEVIATION ***");
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println(F("\n=== Anti-theft TinyML firmware ==="));

    pinMode(ALERT_LED_PIN, OUTPUT);
    g_led_pattern = LedPattern::OFF;

    // OLED (Heltec V4)
    pinMode(Vext, OUTPUT); digitalWrite(Vext, LOW);
    Wire.begin(SDA_OLED, SCL_OLED);
    pinMode(RST_OLED, OUTPUT);
    digitalWrite(RST_OLED, LOW); delay(50);
    digitalWrite(RST_OLED, HIGH); delay(50);
    oled_ok = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    if (oled_ok) { oled.clearDisplay(); oled.display(); Serial.println("[OLED] OK."); }
    else          { Serial.println("[OLED] Init failed."); }

    oled_show_message("Booting...", "Init sensor");
    init_sensor();

    // First NTP sync at boot
    ntp_sync();
    g_last_ntp_sync_ms = millis();

    // Try to load a saved model
    if (training_load(g_model)) {
        g_mode        = SystemMode::INFERENCE;
        g_led_pattern = LedPattern::OFF;
        Serial.println("[SYSTEM] Model loaded -> INFERENCE mode.");
        oled_show_message("Model loaded", "INFERENCE mode"); delay(1500);
        oled_show_inference(true, 0.0f, 0.0f, 0.0f, -1);
    } else {
        training_init(g_model);
        g_phase_start_ms = millis();
        g_mode           = SystemMode::EXPLORING;
        g_led_pattern    = LedPattern::PULSE;
        Serial.printf("[SYSTEM] No model -> EXPLORING for %lu s.\n",
                      EXPLORING_DURATION_MS / 1000UL);
        oled_show_message("No model found", "EXPLORING..."); delay(1500);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Loop
// ─────────────────────────────────────────────────────────────────────────────

void loop() {
    led_update();

    // ── NTP sync check — runs between windows, never mid-window ──────────────
    // Set the pending flag when the interval elapses; the actual sync happens
    // only when a complete window has just been processed (window_index == 0).
    if (millis() - g_last_ntp_sync_ms >= TIME_SYNC_INTERVAL_MS) {
        g_ntp_sync_pending = true;
    }
    if (g_ntp_sync_pending && g_window_index == 0) {
        ntp_sync();
        // Reset sample timer so we don't accumulate drift from sync duration
        g_last_sample_ms = millis();
    }

    // ── Sample collection ────────────────────────────────────────────────────
    unsigned long now = millis();
    if (now - g_last_sample_ms < (unsigned long)SAMPLE_PERIOD_MS) return;
    g_last_sample_ms = now;

    Sample s{};
    if (!read_sample(s)) return;
    g_window[g_window_index++] = s;

    if (g_window_index >= SAMPLE_COUNT) {
        process_window();
        g_window_index = 0;
        g_window_seq++;
    }
}
