#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

// OLED — usa direttamente le costanti dal pins_arduino.h della V4
// SDA_OLED=17, SCL_OLED=18, RST_OLED=21, Vext=36
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// ADC — Scegli modalità di campionamento
#define SAMPLING_MODE_NORMAL 0
#define SAMPLING_MODE_MAX    1
#define SAMPLING_MODE        SAMPLING_MODE_NORMAL  // Cambia qui per passare a modalità NORMAL (256 Hz)

#define ADC_PIN 7
#define ADC_WINDOW_SEC 2

#if SAMPLING_MODE == SAMPLING_MODE_NORMAL
  #define ADC_SAMPLE_RATE 256  // Hz
#else
  #define ADC_SAMPLE_RATE 10000  // Hz — massima frequenza su ESP32-S3
#endif

static const bool ENABLE_DISPLAY = true;   // false = disable OLED and external Vext power
static const bool ENABLE_RED_LED = false;  // false = keep built-in red LED off
static const uint8_t BUTTON_PIN = 0;      // Heltec Boot / user button on V4
static const uint32_t SAMPLING_RATES[] = {256, 10000};
static const uint8_t SAMPLING_RATE_COUNT = sizeof(SAMPLING_RATES) / sizeof(SAMPLING_RATES[0]);
uint8_t currentSamplingIndex = (ADC_SAMPLE_RATE == SAMPLING_RATES[1]) ? 1 : 0;
uint32_t currentSamplingRate = SAMPLING_RATES[currentSamplingIndex];
uint8_t displayAddress = 0x3C;
bool displayEnabled = ENABLE_DISPLAY;

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, RST_OLED);

// --- Global variables ---
float lastAdcAvg   = 0;
uint32_t lastAdcCount = 0;
String wifiStatus  = "WiFi: ...";
String mqttStatus  = "MQTT: ...";

// ADC accumulator
volatile uint32_t adcSum = 0;
volatile uint32_t adcCount = 0;
hw_timer_t *adcTimer = NULL;

// --- Network ---
const char* WIFI_SSID = "Vodafone-mango";
const char* WIFI_PASS = "Mangoblu2020";

// --- ThingsBoard MQTT ---
const char* TB_SERVER   = "mqtt.eu.thingsboard.cloud";
const int   TB_PORT     = 1883;
const char* TB_USERNAME = "ukjhfn2eodqzzlvbjfar";
const char* TB_PASSWORD = "hjojtvvi5sg9xyqudhn4";
const char* CLIENT_ID = "esp32_client";

const char* TELEMETRY_TOPIC  = "v1/devices/me/telemetry";
const char* ATTRIBUTES_TOPIC = "v1/devices/me/attributes";

// --- Clients ---
WiFiClient   espClient;
PubSubClient client(espClient);

long lastMsg = 0;
const int PUBLISH_INTERVAL = 5000;  // 5 secondi per allineare con ADC window

// ADC Timer interrupt
void IRAM_ATTR onAdcTimer() {
    uint16_t val = analogRead(ADC_PIN);
    adcSum += val;
    adcCount++;
}

// --- Prototypes ---
void setup_wifi();
void reconnect_mqtt();
void publishTelemetry();
void displayDataScreen();
void displayStatusScreen();

// =========================================================================
// SETUP
// =========================================================================
void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n[DEBUG] Starting setup...");

  // 1. Alimenta i periferici esterni tramite Vext (OLED, LoRa, ecc.)
  pinMode(Vext, OUTPUT);
  digitalWrite(Vext, ENABLE_DISPLAY ? LOW : HIGH);   // LOW = ON su Heltec
  pinMode(LED, OUTPUT);
  digitalWrite(LED, ENABLE_RED_LED ? HIGH : LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  delay(100);

  if (ENABLE_DISPLAY) {
    // 2. I2C con i pin corretti della V4
    Wire.begin(SDA_OLED, SCL_OLED);  // SDA=17, SCL=18
    
    // Debug: scansiona indirizzi I2C disponibili
    Serial.println("[DEBUG] I2C Scan...");
    for (uint8_t i = 0x01; i < 0x7F; i++) {
      Wire.beginTransmission(i);
      if (Wire.endTransmission() == 0) {
        Serial.printf("[DEBUG] Device found at 0x%02X\n", i);
      }
    }

    // 3. Reset hardware OLED
    pinMode(RST_OLED, OUTPUT);
    digitalWrite(RST_OLED, LOW);
    delay(50);
    digitalWrite(RST_OLED, HIGH);
    delay(50);

    // 4. Inizializza display — prova indirizzi comuni
    bool displayFound = false;
    uint8_t displayAddresses[] = {0x3C, 0x3D};  // Indirizzi comuni per SSD1306
    
    for (uint8_t addr : displayAddresses) {
      if (display.begin(SSD1306_SWITCHCAPVCC, addr)) {
        Serial.printf("[DEBUG] OLED found at 0x%02X\n", addr);
        displayFound = true;
        displayAddress = addr;
        break;
      }
    }
    
    if (!displayFound) {
      Serial.println("[ERROR] SSD1306 not found — Verifica:");
      Serial.println("  - Collegamento SDA (GPIO17) e SCL (GPIO18)");
      Serial.println("  - Alimentazione Vext");
      Serial.println("  - Cavo I2C");
      while (true) { delay(1000); }
    }
    Serial.println("[DEBUG] OLED OK");

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.println("MQTT SENDER");
    display.println("WiFi connecting...");
    display.display();
  } else {
    displayEnabled = false;
    Serial.println("[INFO] OLED disabled in code.");
  }

  // 5. WiFi
  setup_wifi();

  // 6. ADC setup
  pinMode(ADC_PIN, INPUT);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
  
  adcTimer = timerBegin(1, 80, true);
  timerAttachInterrupt(adcTimer, &onAdcTimer, true);
  timerAlarmWrite(adcTimer, 1000000UL / currentSamplingRate, true);
  timerAlarmEnable(adcTimer);

  // 7. MQTT broker
  client.setServer(TB_SERVER, TB_PORT);

  Serial.println("[DEBUG] Setup complete!");
}

// =========================================================================
// LOOP
// =========================================================================
void loop() {
  long now = millis();

  // Reconnect non-bloccante: solo se WiFi è su
  if (WiFi.status() == WL_CONNECTED && !client.connected()) {
    reconnect_mqtt();
  }

  if (client.connected()) {
    client.loop();
  }

  static int lastButtonState = HIGH;
  static long buttonDownTime = 0;
  static bool longPressHandled = false;
  int buttonState = digitalRead(BUTTON_PIN);

  if (buttonState == LOW && lastButtonState == HIGH) {
    buttonDownTime = now;
    longPressHandled = false;
  }

  if (buttonState == LOW && lastButtonState == LOW && !longPressHandled) {
    if (now - buttonDownTime >= 1000) {
      displayEnabled = !displayEnabled;
      if (displayEnabled) {
        digitalWrite(Vext, LOW);
        delay(50);
        digitalWrite(RST_OLED, LOW);
        delay(50);
        digitalWrite(RST_OLED, HIGH);
        delay(50);
        display.begin(SSD1306_SWITCHCAPVCC, displayAddress);
        display.setTextColor(SSD1306_WHITE);
        display.setTextSize(1);
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("Display ON");
        display.display();
      } else {
        if (ENABLE_DISPLAY) {
          display.ssd1306_command(SSD1306_DISPLAYOFF);
        }
        digitalWrite(Vext, HIGH);
      }
      Serial.printf("[BUTTON] Display %s\n", displayEnabled ? "ON" : "OFF");
      longPressHandled = true;
    }
  }

  if (buttonState == HIGH && lastButtonState == LOW) {
    if (!longPressHandled) {
      currentSamplingIndex = (currentSamplingIndex + 1) % SAMPLING_RATE_COUNT;
      currentSamplingRate = SAMPLING_RATES[currentSamplingIndex];
      timerAlarmWrite(adcTimer, 1000000UL / currentSamplingRate, true);
      Serial.printf("[BUTTON] Sampling rate changed to %u Hz\n", currentSamplingRate);
      if (displayEnabled) {
        display.clearDisplay();
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println("Button pressed");
        display.print("Freq now: ");
        display.print(currentSamplingRate);
        display.println(" Hz");
        display.display();
      }
      delay(200);
    }
  }
  lastButtonState = buttonState;

  if (now - lastMsg > PUBLISH_INTERVAL) {
    lastMsg = now;
    if (client.connected()) {
      publishTelemetry();
    }
  }

  static long lastDisplayUpdate = 0;
  if (now - lastDisplayUpdate > 2000) {
    lastDisplayUpdate = now;
    wifiStatus = (WiFi.status() == WL_CONNECTED) ? "WiFi: OK" : "WiFi: NO";
    mqttStatus = client.connected()              ? "MQTT: OK" : "MQTT: NO";
    displayDataScreen();
  }
}

// =========================================================================
// WIFI
// =========================================================================
void setup_wifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    yield();   // cede il controllo ai task interni ESP-IDF
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Connected — IP: ");
    Serial.println(WiFi.localIP());
    if (displayEnabled) {
      display.clearDisplay();
      display.setCursor(0, 0);
      display.println("WiFi OK!");
      display.println(WiFi.localIP().toString());
      display.display();
      delay(1000);
    }
  } else {
    Serial.println("[WiFi] Connection failed");
  }
}

// =========================================================================
// MQTT — non bloccante, un tentativo ogni 5 secondi
// =========================================================================
void reconnect_mqtt() {
  static long lastAttempt = 0;
  long now = millis();
  if (now - lastAttempt < 5000) return;
  lastAttempt = now;

  Serial.print("[MQTT] Connecting...");
  if (client.connect(CLIENT_ID, TB_USERNAME, TB_PASSWORD)) {
    Serial.println(" OK");
    client.publish(ATTRIBUTES_TOPIC, "{\"firmware_version\":\"1.0.0\"}");
  } else {
    Serial.print(" FAILED, state=");
    Serial.println(client.state());
  }
}

// =========================================================================
// TELEMETRY
// =========================================================================
void publishTelemetry() {
  // Calcola media ADC dall'ultimo campionamento (5 secondi)
  if (adcCount > 0) {
    lastAdcAvg = (float)adcSum / (float)adcCount;
    lastAdcCount = adcCount;
  }

  String payload = "{\"adc_avg\":" + String(lastAdcAvg, 1) +
                   ",\"adc_samples\":" + String(lastAdcCount) + "}";

  if (client.publish(TELEMETRY_TOPIC, payload.c_str())) {
    Serial.print("[TELEMETRY] Published: ");
    Serial.println(payload);
  } else {
    Serial.println("[TELEMETRY] Publish failed");
  }

  // Reset accumulatore ADC per la prossima finestra
  adcSum = 0;
  adcCount = 0;
}

// =========================================================================
// DISPLAY
// =========================================================================
void displayDataScreen() {
  if (!ENABLE_DISPLAY || !displayEnabled) {
    return;
  }

  display.clearDisplay();
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println(wifiStatus);
  display.println(mqttStatus);

  display.drawLine(0, 22, 127, 22, SSD1306_WHITE);

  display.setCursor(0, 28);
  display.print("Freq:  ");
  display.print(currentSamplingRate);
  display.println(" Hz");

  display.setCursor(0, 38);
  display.print("Samples: ");
  display.println(lastAdcCount);

  display.setCursor(0, 48);
  display.print("Avg:   ");
  display.print(lastAdcAvg, 0);

  display.setCursor(0, 56);
  display.print("Up: ");
  display.print(millis() / 1000);
  display.print("s");

  display.display();
}

void displayStatusScreen() {
  if (!ENABLE_DISPLAY || !displayEnabled) {
    return;
  }

  display.clearDisplay();
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println("System Status");
  display.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  display.setCursor(0, 14);
  display.print("WiFi: ");
  display.println(WiFi.status() == WL_CONNECTED ? "OK" : "FAIL");

  display.setCursor(0, 24);
  display.print("MQTT: ");
  display.println(client.connected() ? "OK" : "FAIL");

  display.setCursor(0, 34);
  display.print("IP: ");
  display.println(WiFi.status() == WL_CONNECTED
                  ? WiFi.localIP().toString() : "N/A");

  display.setCursor(0, 44);
  display.print("Up: ");
  display.print(millis() / 1000);
  display.print("s");

  display.display();
}