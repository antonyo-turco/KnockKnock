// ─────────────────────────────────────────────────────────────────────────────
//  data_collector_wifi.ino
//  Raccolta dati ADXL362 → TCP WiFi → PC — VERSIONE PULITA SENZA BLOCCHI
//
//  Formato CSV: millis,x,y,z
//  ODR: 400 Hz (2500 µs per campione)
//  Hardware: Heltec ESP32-C3 V4 + ADXL362
// ─────────────────────────────────────────────────────────────────────────────

#include <SPI.h>
#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── Configurazione ────────────────────────────────────────────────────────────

#define WIFI_SSID      "Vodafone-mango"
#define WIFI_PASSWORD  "Mangoblu2020"
#define SERVER_IP      "192.168.1.8"
#define SERVER_PORT    9876

// ── Costanti hardware ─────────────────────────────────────────────────────────

static constexpr int  PIN_CS           = 4;
static constexpr int  PIN_MISO         = 3;
static constexpr int  PIN_MOSI         = 2;
static constexpr int  PIN_SCK          = 1;
static constexpr long SPI_FREQ         = 8000000;
static constexpr long SAMPLE_PERIOD_US = 2500;    // 400 Hz

// ADXL362
static constexpr uint8_t CMD_WRITE         = 0x0A;
static constexpr uint8_t CMD_READ          = 0x0B;
static constexpr uint8_t REG_SOFT_RESET    = 0x1F;
static constexpr uint8_t REG_FILTER_CTL    = 0x2C;
static constexpr uint8_t REG_POWER_CTL     = 0x2D;
static constexpr uint8_t REG_XDATA_L       = 0x0E;
static constexpr uint8_t FILTER_CTL_400HZ  = 0b00000101;
static constexpr uint8_t POWER_CTL_MEASURE = 0x02;

// OLED
static constexpr int OLED_W   = 128;
static constexpr int OLED_H   = 64;
static constexpr int OLED_RST = 21;

// Buffer TCP
static constexpr int TX_BUF_SIZE = 2048;

// ── Stato globale ─────────────────────────────────────────────────────────────

static WiFiClient    client;
static Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, OLED_RST);

static uint32_t s_samples_sent  = 0;
static uint32_t s_samples_lost  = 0;
static uint32_t s_start_ms      = 0;

static char s_tx_buf[TX_BUF_SIZE];
static int  s_tx_pos = 0;

// ── ADXL362 ──────────────────────────────────────────────────────────────────

static void adxl_write(uint8_t reg, uint8_t val) {
    digitalWrite(PIN_CS, LOW);
    SPI.transfer(CMD_WRITE);
    SPI.transfer(reg);
    SPI.transfer(val);
    digitalWrite(PIN_CS, HIGH);
}

static void adxl_read_xyz(int16_t &x, int16_t &y, int16_t &z) {
    uint8_t buf[6];
    digitalWrite(PIN_CS, LOW);
    SPI.transfer(CMD_READ);
    SPI.transfer(REG_XDATA_L);
    for (int i = 0; i < 6; ++i) buf[i] = SPI.transfer(0x00);
    digitalWrite(PIN_CS, HIGH);
    x = (int16_t)((buf[1] << 8) | buf[0]);
    y = (int16_t)((buf[3] << 8) | buf[2]);
    z = (int16_t)((buf[5] << 8) | buf[4]);
}

static void adxl_init() {
    pinMode(PIN_CS, OUTPUT);
    digitalWrite(PIN_CS, HIGH);
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    SPI.beginTransaction(SPISettings(SPI_FREQ, MSBFIRST, SPI_MODE0));
    adxl_write(REG_SOFT_RESET, 0x52);
    delay(10);
    adxl_write(REG_FILTER_CTL, FILTER_CTL_400HZ);
    delay(2);
    adxl_write(REG_POWER_CTL, POWER_CTL_MEASURE);
    delay(10);
}

// ── OLED ──────────────────────────────────────────────────────────────────────

static void oled_init() {
    pinMode(Vext, OUTPUT);
    digitalWrite(Vext, LOW);
    delay(50);
    display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    display.clearDisplay();
    display.display();
}

static void oled_msg(const char* line1, const char* line2 = nullptr) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(line1);
    if (line2) display.println(line2);
    display.display();
}

// ── WiFi ──────────────────────────────────────────────────────────────────────

static bool wifi_connect() {
    oled_msg("WiFi: Connettendo...");
    Serial.print("WiFi: ");
    Serial.println(WIFI_SSID);
    
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
        delay(300);
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        oled_msg("WiFi TIMEOUT");
        Serial.println("WiFi TIMEOUT");
        return false;
    }
    
    String ip = WiFi.localIP().toString();
    oled_msg("WiFi OK", ip.c_str());
    Serial.print("WiFi OK — IP: ");
    Serial.println(ip);
    delay(800);
    return true;
}

// ── TCP ───────────────────────────────────────────────────────────────────────

static bool tcp_connect_setup() {
    oled_msg("TCP: Connettendo...");
    Serial.printf("TCP: %s:%d\n", SERVER_IP, SERVER_PORT);
    
    if (!client.connect(SERVER_IP, SERVER_PORT)) {
        oled_msg("TCP FALLITA");
        Serial.println("TCP FALLITA");
        return false;
    }
    
    oled_msg("TCP OK - Streaming");
    Serial.println("TCP OK - Inizio streaming");
    delay(500);
    return true;
}

// ── Flush buffer TCP NON-BLOCCANTE ────────────────────────────────────────────

static void flush_tx_buf() {
    if (s_tx_pos == 0) return;
    if (!client.connected()) return;
    
    size_t written = client.write((const uint8_t*)s_tx_buf, s_tx_pos);
    if (written > 0) {
        memmove(s_tx_buf, s_tx_buf + written, s_tx_pos - written);
        s_tx_pos -= written;
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n\n=== STARTUP ===");

    oled_init();
    adxl_init();

    if (!wifi_connect()) {
        Serial.println("HALT");
        while (true) delay(1000);
    }

    if (!tcp_connect_setup()) {
        Serial.println("HALT");
        while (true) delay(1000);
    }

    s_start_ms = millis();
    Serial.println("OK - Streaming");
}

// ── Loop SEMPLIFICATO ─────────────────────────────────────────────────────────

void loop() {
    static uint32_t next_us      = micros();
    static uint32_t last_flush_ms = 0;
    static uint32_t last_hz_ms    = 0;
    static uint32_t hz_count      = 0;

    // ── Timing preciso 400 Hz ─────────────────────────────────────────────────
    uint32_t now_us = micros();
    if ((int32_t)(now_us - next_us) < 0) return;
    next_us += SAMPLE_PERIOD_US;

    // ── Lettura sensore ───────────────────────────────────────────────────────
    int16_t x, y, z;
    adxl_read_xyz(x, y, z);
    uint32_t ts = millis();

    // ── Accumula nel buffer ───────────────────────────────────────────────────
    int needed = snprintf(s_tx_buf + s_tx_pos, TX_BUF_SIZE - s_tx_pos,
                          "%lu,%d,%d,%d\n", (unsigned long)ts, (int)x, (int)y, (int)z);
    if (needed > 0 && s_tx_pos + needed < TX_BUF_SIZE) {
        s_tx_pos += needed;
        s_samples_sent++;
        hz_count++;
    } else {
        s_samples_lost++;
    }

    // ── Flush ogni 200ms (NON-BLOCCANTE) ──────────────────────────────────────
    if (ts - last_flush_ms >= 200) {
        flush_tx_buf();
        last_flush_ms = ts;
    }

    // ── Stampa statistiche ogni secondo ───────────────────────────────────────
    if (ts - last_hz_ms >= 1000) {
        Serial.printf("[%lus] sent=%lu lost=%lu hz=%lu\n",
                      (unsigned long)((ts - s_start_ms) / 1000),
                      (unsigned long)s_samples_sent,
                      (unsigned long)s_samples_lost,
                      (unsigned long)hz_count);
        hz_count = 0;
        last_hz_ms = ts;
    }
}
