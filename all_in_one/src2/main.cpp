#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "feature_extraction.h"
#include "tinyml_training.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Training duration
//  Comment/uncomment the line you want to use.
// ─────────────────────────────────────────────────────────────────────────────

// #define TRAINING_DURATION_MS  (24UL * 3600UL * 1000UL)   // 24 h — production
// #define TRAINING_DURATION_MS     (15UL * 60UL  * 1000UL)   // 15 min — testing
#define TRAINING_DURATION_MS  ( 2UL * 60UL  * 1000UL)   //  2 min — quick bench

// ─────────────────────────────────────────────────────────────────────────────
//  Sampling
// ─────────────────────────────────────────────────────────────────────────────

#define SAMPLE_COUNT      200
#define SAMPLING_RATE_HZ  100.0f
#define SAMPLE_PERIOD_MS  10
#define SIGNAL_GAIN       1.0f

// ─────────────────────────────────────────────────────────────────────────────
//  OLED  — Adafruit SSD1306, 128×64, I2C
//  Adjust OLED_SDA / OLED_SCL if your wiring differs.
// ─────────────────────────────────────────────────────────────────────────────

#define OLED_WIDTH    128
#define OLED_HEIGHT    64
#define OLED_RESET    -1     // -1 = share Arduino reset pin

// How often (in completed windows) the OLED is refreshed.
// Keeps I2C traffic low — display doesn't need to update every 2 s.
#define OLED_REFRESH_WINDOWS  1

static Adafruit_SSD1306 oled(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
static bool oled_ok = false;

// ─────────────────────────────────────────────────────────────────────────────
//  SPI / ADXL362 pins & registers
// ─────────────────────────────────────────────────────────────────────────────

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
//  LED — patterns are handled non-blocking via a state machine in loop()
// ─────────────────────────────────────────────────────────────────────────────

static const uint8_t ALERT_LED_PIN = LED_BUILTIN;

enum class LedPattern : uint8_t {
    OFF,           // inference  — baseline confirmed
    SLOW_BLINK,    // training   — 1 Hz heartbeat
    FAST_BLINK,    // inference  — DEVIATION detected
    ON             // solid on   — training just finished (brief)
};

static LedPattern    g_led_pattern   = LedPattern::OFF;
static unsigned long g_led_last_ms   = 0;
static bool          g_led_state     = false;

static void led_update() {
    unsigned long now = millis();
    switch (g_led_pattern) {
        case LedPattern::OFF:
            digitalWrite(ALERT_LED_PIN, LOW);
            break;
        case LedPattern::ON:
            digitalWrite(ALERT_LED_PIN, HIGH);
            break;
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
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Types & globals
// ─────────────────────────────────────────────────────────────────────────────

struct Sample { int16_t x, y, z; };

enum class SystemMode : uint8_t { TRAINING, INFERENCE };

static Sample      g_window[SAMPLE_COUNT];
static int         g_window_index  = 0;
static uint32_t    g_window_seq    = 0;
static unsigned long g_last_sample_ms = 0;

static KMeansModel g_model;
static SystemMode  g_mode;
static uint32_t    g_training_start_ms = 0;

// Last inference result — kept so the OLED refresh (every N windows)
// always shows up-to-date values without re-computing.
static float       g_last_dist         = 0.0f;
static bool        g_last_is_baseline  = true;
static float       g_last_impact       = 0.0f;
static float       g_last_p99          = 0.0f;

// ─────────────────────────────────────────────────────────────────────────────
//  OLED rendering helpers
// ─────────────────────────────────────────────────────────────────────────────

// Draw a progress bar.  x,y = top-left corner; w,h = size; pct in [0,100].
static void oled_progress_bar(int x, int y, int w, int h, uint8_t pct) {
    oled.drawRect(x, y, w, h, SSD1306_WHITE);
    int fill = (int)((w - 2) * pct / 100);
    if (fill > 0) {
        oled.fillRect(x + 1, y + 1, fill, h - 2, SSD1306_WHITE);
    }
}

// Format seconds → "1h23m" or "12m34s"
static void fmt_time(char *buf, size_t len, uint32_t secs) {
    if (secs >= 3600) {
        snprintf(buf, len, "%luh%02lum", (unsigned long)(secs / 3600),
                 (unsigned long)((secs % 3600) / 60));
    } else {
        snprintf(buf, len, "%lum%02lus", (unsigned long)(secs / 60),
                 (unsigned long)(secs % 60));
    }
}

static void oled_show_training() {
    if (!oled_ok) return;

    uint32_t elapsed_ms  = millis() - g_training_start_ms;
    if (elapsed_ms > TRAINING_DURATION_MS) elapsed_ms = TRAINING_DURATION_MS;
    uint8_t  pct         = (uint8_t)(elapsed_ms * 100 / TRAINING_DURATION_MS);
    uint32_t remaining_s = (TRAINING_DURATION_MS - elapsed_ms) / 1000;

    char time_buf[12];
    fmt_time(time_buf, sizeof(time_buf), remaining_s);

    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);

    // ── Row 0: title ─────────────────────────────────────────────────────────
    oled.setTextSize(2);
    oled.setCursor(0, 0);
    oled.print(F("TRAINING"));

    // ── Row 1: percentage ────────────────────────────────────────────────────
    oled.setTextSize(1);
    oled.setCursor(0, 18);
    oled.printf("%u%%  rem: %s", pct, time_buf);

    // ── Row 2: progress bar ──────────────────────────────────────────────────
    oled_progress_bar(0, 30, 128, 10, pct);

    // ── Row 3: sample counter + cluster info ─────────────────────────────────
    oled.setCursor(0, 44);
    oled.printf("n=%lu  k=%d", (unsigned long)g_model.total_samples, KMEANS_K);

    oled.setCursor(0, 54);
    oled.printf("win #%lu", (unsigned long)g_window_seq);

    oled.display();
}

static void oled_show_inference(bool is_baseline, float dist,
                                 float impact, float m_p99) {
    if (!oled_ok) return;

    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);

    if (is_baseline) {
        // ── BASELINE screen ───────────────────────────────────────────────────
        oled.setTextSize(2);
        oled.setCursor(0, 0);
        oled.print(F("BASELINE"));

        oled.setTextSize(1);
        oled.setCursor(0, 20);
        oled.printf("dist  : %.4f", dist);
        oled.setCursor(0, 32);
        oled.printf("impact: %.4f", impact);
        oled.setCursor(0, 44);
        oled.printf("m_p99 : %.2f", m_p99);
        oled.setCursor(0, 56);
        oled.printf("win #%lu", (unsigned long)g_window_seq);

    } else {
        // ── DEVIATION screen (inverted for maximum visibility) ────────────────
        oled.fillRect(0, 0, OLED_WIDTH, OLED_HEIGHT, SSD1306_WHITE);
        oled.setTextColor(SSD1306_BLACK);

        oled.setTextSize(2);
        oled.setCursor(4, 2);
        oled.print(F("DEVIATION"));

        oled.setTextSize(1);
        oled.setCursor(4, 22);
        oled.printf("dist  : %.4f", dist);
        oled.setCursor(4, 34);
        oled.printf("impact: %.4f", impact);
        oled.setCursor(4, 46);
        oled.printf("m_p99 : %.2f", m_p99);
    }

    oled.display();
}

static void oled_show_boot_message(const char *msg) {
    if (!oled_ok) return;
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(0, 0);
    oled.print(msg);
    oled.display();
}

// ─────────────────────────────────────────────────────────────────────────────
//  ADXL362 helpers
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t adxl362_read_register(uint8_t reg) {
    SPI.beginTransaction(SPISettings(SPI_CLOCK_FREQ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    SPI.transfer(ADXL362_CMD_READ_REG);
    SPI.transfer(reg);
    uint8_t v = SPI.transfer(0x00);
    digitalWrite(PIN_CS, HIGH);
    SPI.endTransaction();
    return v;
}

static void adxl362_write_register(uint8_t reg, uint8_t val) {
    SPI.beginTransaction(SPISettings(SPI_CLOCK_FREQ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    SPI.transfer(ADXL362_CMD_WRITE_REG);
    SPI.transfer(reg);
    SPI.transfer(val);
    digitalWrite(PIN_CS, HIGH);
    SPI.endTransaction();
}

static int16_t adxl362_read_axis(uint8_t lsb_reg) {
    uint8_t lsb = adxl362_read_register(lsb_reg);
    uint8_t msb = adxl362_read_register(lsb_reg + 1);
    return static_cast<int16_t>((static_cast<int16_t>(msb) << 8) | lsb);
}

static void init_sensor() {
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    pinMode(PIN_CS, OUTPUT);
    digitalWrite(PIN_CS, HIGH);
    delay(100);

    uint8_t id_ad  = adxl362_read_register(ADXL362_REG_DEVID_AD);
    uint8_t id_mst = adxl362_read_register(ADXL362_REG_DEVID_MST);
    Serial.printf("ADXL362 DevID: 0x%02X 0x%02X\n", id_ad, id_mst);
    if (id_ad != 0xAD || id_mst != 0x1D) {
        Serial.println("ERROR: ADXL362 not detected! Check wiring.");
        oled_show_boot_message("ERROR:\nADXL362\nnot found!");
    }

    adxl362_write_register(ADXL362_REG_FILTER_CTL, 0x13);  // ±2g, 100 Hz ODR
    adxl362_write_register(ADXL362_REG_POWER_CTL,  0x02);  // measurement mode
    delay(100);
}

static bool read_sample(Sample &s) {
    s.x = static_cast<int16_t>(adxl362_read_axis(ADXL362_REG_XDATA_L) * SIGNAL_GAIN);
    s.y = static_cast<int16_t>(adxl362_read_axis(ADXL362_REG_YDATA_L) * SIGNAL_GAIN);
    s.z = static_cast<int16_t>(adxl362_read_axis(ADXL362_REG_ZDATA_L) * SIGNAL_GAIN);
    return true;
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

    InferenceFeatures feat = compute_features(
        xb, yb, zb, SAMPLE_COUNT, SAMPLING_RATE_HZ);

    // ── TRAINING ─────────────────────────────────────────────────────────────
    if (g_mode == SystemMode::TRAINING) {
        training_update(g_model, feat);

        // Refresh OLED every OLED_REFRESH_WINDOWS windows
        if (g_window_seq % OLED_REFRESH_WINDOWS == 0) {
            oled_show_training();
        }

        // Log to serial every 150 windows (~5 min at 2s/window)
        if (g_window_seq % 150 == 0) {
            uint32_t elapsed_ms  = millis() - g_training_start_ms;
            uint32_t pct         = elapsed_ms * 100 / TRAINING_DURATION_MS;
            Serial.printf("[TRAIN] win=%lu  n=%lu  %lu%%\n",
                          (unsigned long)g_window_seq,
                          (unsigned long)g_model.total_samples,
                          (unsigned long)pct);
        }

        // Training complete?
        if (millis() - g_training_start_ms >= TRAINING_DURATION_MS) {
            g_led_pattern = LedPattern::ON;
            led_update();

            oled_show_boot_message("Training done!\nFinalising...");
            training_finalize(g_model);
            training_save(g_model);

            g_mode = SystemMode::INFERENCE;
            g_led_pattern = LedPattern::OFF;
            Serial.println("[SYSTEM] Training complete -> INFERENCE mode.");
        }
        return;
    }

    // ── INFERENCE ────────────────────────────────────────────────────────────
    float dist = 0.0f;
    bool  ok   = training_is_baseline(g_model, feat, &dist);

    g_last_dist        = dist;
    g_last_is_baseline = ok;
    g_last_impact      = feat.impact_score;
    g_last_p99         = feat.m_p99;

    g_led_pattern = ok ? LedPattern::OFF : LedPattern::FAST_BLINK;

    if (g_window_seq % OLED_REFRESH_WINDOWS == 0) {
        oled_show_inference(ok, dist, feat.impact_score, feat.m_p99);
    }

    Serial.printf("[INF] win=%lu  impact=%.4f  m_p99=%.2f  dist=%.4f  %s\n",
                  (unsigned long)g_window_seq,
                  feat.impact_score, feat.m_p99, dist,
                  ok ? "BASELINE" : "*** DEVIATION ***");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Setup
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println(F("\n=== Anti-theft TinyML firmware ==="));

    // LED
    pinMode(ALERT_LED_PIN, OUTPUT);
    g_led_pattern = LedPattern::OFF;

    // OLED
    pinMode(Vext, OUTPUT);
    digitalWrite(Vext, LOW);
    Wire.begin(SDA_OLED, SCL_OLED);

    pinMode(RST_OLED, OUTPUT);
    digitalWrite(RST_OLED, LOW);
    delay(50);
    digitalWrite(RST_OLED, HIGH);
    delay(50);

    oled_ok = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);

    if (oled_ok) {
        oled.clearDisplay();
        oled.display();
        Serial.println("[OLED] OK.");
    } else {
        Serial.println("[OLED] OLED init failed.");
    }

    oled_show_boot_message("Booting...\nInit sensor");
    init_sensor();

    // Try to load a previously trained model
    // if (training_load(g_model)) {
    //     g_mode = SystemMode::INFERENCE;
    //     g_led_pattern = LedPattern::OFF;
    //     Serial.println("[SYSTEM] Model loaded -> INFERENCE mode.");
    //     oled_show_boot_message("Model loaded\nINFERENCE mode");
    //     delay(1500);
    //     oled_show_inference(true, 0.0f, 0.0f, 0.0f);
    // } else {
    //     training_init(g_model);
    //     g_training_start_ms = millis();
    //     g_mode = SystemMode::TRAINING;
    //     g_led_pattern = LedPattern::SLOW_BLINK;
    //     Serial.printf("[SYSTEM] No model -> TRAINING for %lu s.\n",
    //                   TRAINING_DURATION_MS / 1000UL);
    //     oled_show_boot_message("No model found\nStarting\nTRAINING...");
    //     delay(1500);
    // }

    training_init(g_model);
    g_training_start_ms = millis();
    g_mode = SystemMode::TRAINING;
    g_led_pattern = LedPattern::SLOW_BLINK;
    Serial.printf("[SYSTEM] No model -> TRAINING for %lu s.\n",
                    TRAINING_DURATION_MS / 1000UL);
    oled_show_boot_message("No model found\nStarting\nTRAINING...");
    delay(1500);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Loop
// ─────────────────────────────────────────────────────────────────────────────

void loop() {
    // Non-blocking LED update — must be called every iteration
    led_update();

    unsigned long now = millis();
    if (now - g_last_sample_ms < SAMPLE_PERIOD_MS) return;
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
