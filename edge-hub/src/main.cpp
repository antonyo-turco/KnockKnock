#include <Arduino.h>
#include "wifi_manager.h"
#include "edge_mqtt.h"

/* ── CONFIGURE THESE ──────────────────────────────────────── */
#define WIFI_SSID      "your_ssid"
#define WIFI_PASSWORD  "your_password"
#define MQTT_BROKER    "192.168.1.X"   /* laptop IP on local network */
#define MQTT_PORT      1883
/* ─────────────────────────────────────────────────────────── */

static uint32_t s_last_heartbeat_ms = 0;

void setup()
{
    Serial.begin(115200);
    delay(500);
    wifi_init(WIFI_SSID, WIFI_PASSWORD);
    mqtt_init(MQTT_BROKER, MQTT_PORT);
}

void loop()
{
    mqtt_loop();

    uint32_t now = millis();
    if (now - s_last_heartbeat_ms > 60000) {
        s_last_heartbeat_ms = now;
        if (mqtt_is_connected()) mqtt_publish_heartbeat();
    }
}
