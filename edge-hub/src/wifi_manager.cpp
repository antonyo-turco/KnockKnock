#include "wifi_manager.h"
#include <WiFi.h>

static const char *TAG = "wifi";

void wifi_init(const char *ssid, const char *password)
{
    WiFi.mode(WIFI_AP_STA);   // AP_STA required for ESP-NOW + Wi-Fi coexistence
    WiFi.begin(ssid, password);
    Serial.printf("[wifi] Connecting to %s", ssid);
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[wifi] Connected, IP: %s\n",
                      WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n[wifi] FAILED — check credentials in config");
    }
}

bool wifi_is_connected() { return WiFi.status() == WL_CONNECTED; }
