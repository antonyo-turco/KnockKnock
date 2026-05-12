/* Updated main.cpp: use proper Heltec pins, power Vext, init I2C with SDA_OLED/SCL_OLED, probe SSD1306 addresses */
#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <sys/time.h>
#include "config.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// OLED via Adafruit SSD1306 (I2C)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Runtime control
bool sending_enabled = true;
unsigned long last_oled_update = 0;
unsigned long oled_update_interval = 1000; // ms

// Use board variant pin names for Heltec V4
#define PIN_CS   4
#define PIN_MISO 3
#define PIN_MOSI 2
#define PIN_SCK  1
#define SPI_CLOCK_FREQ   1000000

#define SAMPLING_RATE_HZ   100
#define SAMPLE_PERIOD_MS   10
#define BUFFER_SIZE        6000
#define BATCH_SIZE         500
#define JSON_BUFFER_SIZE   70000
#define SIGNAL_GAIN        1.0f

struct Measurement { int16_t x; int16_t y; int16_t z; };
Measurement buffer[BUFFER_SIZE];
int buffer_index = 0;
uint32_t batch_seq = 0;

#define ADXL362_CMD_READ_REG    0x0B
#define ADXL362_CMD_WRITE_REG   0x0A
#define ADXL362_REG_DEVID_AD    0x00
#define ADXL362_REG_DEVID_MST   0x01
#define ADXL362_REG_XDATA_L     0x0E
#define ADXL362_REG_XDATA_H     0x0F
#define ADXL362_REG_YDATA_L     0x10
#define ADXL362_REG_YDATA_H     0x11
#define ADXL362_REG_ZDATA_L     0x12
#define ADXL362_REG_ZDATA_H     0x13
#define ADXL362_REG_FILTER_CTL  0x2C
#define ADXL362_REG_POWER_CTL   0x2D

uint8_t adxl362_read_register(uint8_t reg_addr) {
    SPI.beginTransaction(SPISettings(SPI_CLOCK_FREQ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    SPI.transfer(ADXL362_CMD_READ_REG);
    SPI.transfer(reg_addr);
    uint8_t value = SPI.transfer(0x00);
    digitalWrite(PIN_CS, HIGH);
    SPI.endTransaction();
    return value;
}
void adxl362_write_register(uint8_t reg_addr, uint8_t value) {
    SPI.beginTransaction(SPISettings(SPI_CLOCK_FREQ, MSBFIRST, SPI_MODE0));
    digitalWrite(PIN_CS, LOW);
    SPI.transfer(ADXL362_CMD_WRITE_REG);
    SPI.transfer(reg_addr);
    SPI.transfer(value);
    digitalWrite(PIN_CS, HIGH);
    SPI.endTransaction();
}
int16_t adxl362_read_axis(uint8_t lsb_reg) {
    uint8_t lsb = adxl362_read_register(lsb_reg);
    uint8_t msb = adxl362_read_register(lsb_reg + 1);
    return ((int16_t)msb << 8) | lsb;
}

void wifi_connect(){
    Serial.print("Connecting to WiFi: ");
    Serial.println(WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int attempts=0;
    while (WiFi.status()!=WL_CONNECTED && attempts<30){ delay(500); Serial.print("."); attempts++; }
    if (WiFi.status()==WL_CONNECTED){ Serial.println("\n✓ WiFi connected!"); Serial.print("IP: "); Serial.println(WiFi.localIP()); }
    else { Serial.println("\n✗ WiFi connection failed!"); }
}

bool send_bulk_data(){
    if (WiFi.status() != WL_CONNECTED){ 
        Serial.println("✗ WiFi not connected!"); 
        return false; 
    }
    if (buffer_index < BATCH_SIZE){ 
        Serial.printf("Buffer not full yet: %d/%d\n", buffer_index, BATCH_SIZE); 
        return true; 
    }
    Serial.printf("\n=== Sending batch #%lu ===\n", (unsigned long)batch_seq);
    Serial.printf("Samples: %d\n", buffer_index);
    static char json_buffer[JSON_BUFFER_SIZE];
    char* pos=json_buffer; size_t remaining=JSON_BUFFER_SIZE;
    int n = snprintf(pos, remaining, "{\"device_id\":\"esp32-heltec-v4\",\"batch_seq\":%lu,\"sampling_rate_hz\":%d,\"batch_size\":%d,\"measurements\":[", (unsigned long)batch_seq, SAMPLING_RATE_HZ, BATCH_SIZE);
    
    if (n<=0 || (size_t)n>=remaining){ 
        Serial.println("✗ JSON header overflow"); 
        return false; 
    }
    pos += n; 
    remaining -= (size_t)n;
    for (int i=0;i<buffer_index;i++){
        n = snprintf(pos, remaining, "%s{\"x\":%d,\"y\":%d,\"z\":%d}", (i>0)?",":"", buffer[i].x, buffer[i].y, buffer[i].z);
        if (n<=0 || (size_t)n>=remaining){ 
            Serial.println("✗ JSON body overflow"); 
            return false; 
        }
        pos += n; 
        remaining -= (size_t)n;
    }
    n = snprintf(pos, remaining, "]}"); 
    if (n<=0 || (size_t)n>=remaining){ 
        Serial.println("✗ JSON footer overflow"); 
        return false; 
    }
    int total_size = (int)(pos - json_buffer + n);
    Serial.printf("JSON size: %d bytes\n", total_size);
    HTTPClient http; String url = "http://" + String(SERVER_IP) + ":" + String(SERVER_PORT) + "/api/data/bulk"; http.begin(url); http.addHeader("Content-Type","application/json"); http.setConnectTimeout(5000); http.setTimeout(20000);
    int httpResponseCode = http.POST((uint8_t*)json_buffer, total_size);
    if (httpResponseCode==201){ 
        Serial.printf("✓ Server OK: %d measurements received\n", buffer_index); 
        String response = http.getString(); 
        Serial.println("Response: "+response); 
        buffer_index=0; 
        batch_seq++; 
        http.end(); 
        return true; 
    }
    Serial.printf("✗ Server error: %d\n", httpResponseCode); 
    String response = http.getString(); 
    Serial.println("Response: "+response); 
    http.end(); 
    return false;
}

void updateOled(){
    if (millis()-last_oled_update<oled_update_interval) return; last_oled_update=millis();
    display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0,0);
    char buf[64]; String ip = WiFi.localIP().toString(); snprintf(buf,sizeof(buf),"%s", ip.c_str()); display.println(buf);
    snprintf(buf,sizeof(buf),"Batch: %lu", (unsigned long)batch_seq); display.println(buf);
    snprintf(buf,sizeof(buf),"Samples: %d", buffer_index); display.println(buf);
    snprintf(buf,sizeof(buf),"Sending: %s", sending_enabled?"ON":"OFF"); display.println(buf);
    display.display();
}

void setup(){
    Serial.begin(115200); delay(500);
    Serial.println("\n=== ADXL362 + Bulk WiFi (fixed 500-sample batches) ==="); Serial.printf("Sampling: %d Hz\n", SAMPLING_RATE_HZ); Serial.printf("Batch size: %d samples\n", BATCH_SIZE);

    // Power external peripherals (OLED, sensors) via Vext
    pinMode(Vext, OUTPUT);
    digitalWrite(Vext, LOW); // LOW = ON for Heltec V4

    // Init I2C with correct pins for Heltec V4
    Wire.begin(SDA_OLED, SCL_OLED);

    // Reset OLED hardware
    pinMode(RST_OLED, OUTPUT);
    digitalWrite(RST_OLED, LOW); delay(50); digitalWrite(RST_OLED, HIGH); delay(50);

    // Try addresses 0x3C and 0x3D
    bool dispOk=false; uint8_t addrs[]={0x3C,0x3D};
    for (uint8_t a: addrs){ if (display.begin(SSD1306_SWITCHCAPVCC, a)){ Serial.printf("SSD1306 found at 0x%02X\n", a); dispOk=true; break; } }
    if (!dispOk){ Serial.println("SSD1306 allocation failed"); }
    else { display.clearDisplay(); display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.setCursor(0,0); display.println("Booting..."); display.display(); }

    wifi_connect();

    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS);
    pinMode(PIN_CS, OUTPUT); digitalWrite(PIN_CS, HIGH);
    delay(100);

    uint8_t devid_ad = adxl362_read_register(ADXL362_REG_DEVID_AD);
    uint8_t devid_mst = adxl362_read_register(ADXL362_REG_DEVID_MST);
    Serial.printf("ADXL362 DevID: 0x%02X 0x%02X\n", devid_ad, devid_mst);
    if (devid_ad != 0xAD || devid_mst != 0x1D) { Serial.println("ERROR: ADXL362 not found!"); }
    adxl362_write_register(ADXL362_REG_FILTER_CTL, 0x13); // 2g, 100Hz, default HBW
    adxl362_write_register(ADXL362_REG_POWER_CTL, 0x02);
    delay(100);
    Serial.println("Ready to accumulate data...\n");
}

void loop(){
    int16_t x_raw = adxl362_read_axis(ADXL362_REG_XDATA_L);
    int16_t y_raw = adxl362_read_axis(ADXL362_REG_YDATA_L);
    int16_t z_raw = adxl362_read_axis(ADXL362_REG_ZDATA_L);
    int16_t x_scaled = (int16_t)(x_raw * SIGNAL_GAIN);
    int16_t y_scaled = (int16_t)(y_raw * SIGNAL_GAIN);
    int16_t z_scaled = (int16_t)(z_raw * SIGNAL_GAIN);
    if (buffer_index < BUFFER_SIZE) { buffer[buffer_index].x = x_scaled; buffer[buffer_index].y = y_scaled; buffer[buffer_index].z = z_scaled; buffer_index++; }
    if (sending_enabled && buffer_index >= BATCH_SIZE) { send_bulk_data(); }
    updateOled(); delay(SAMPLE_PERIOD_MS);
}
