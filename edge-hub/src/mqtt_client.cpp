#include "edge_mqtt.h"
#include <PubSubClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>

static WiFiClient   s_wifi_client;
static PubSubClient s_mqtt(s_wifi_client);
static const char  *s_broker_ip;
static uint16_t     s_port;
static uint32_t     s_last_reconnect_ms = 0;

void mqtt_init(const char *broker_ip, uint16_t port)
{
    s_broker_ip = broker_ip;
    s_port      = port;
    s_mqtt.setServer(broker_ip, port);
    s_mqtt.setBufferSize(512);
}

static void reconnect()
{
    if (s_mqtt.connect("knockknock-edge")) {
        Serial.println("[mqtt] Connected to broker");
    } else {
        Serial.printf("[mqtt] Connect failed, rc=%d — retry in 5s\n",
                      s_mqtt.state());
    }
}

void mqtt_loop()
{
    if (!s_mqtt.connected()) {
        uint32_t now = millis();
        if (now - s_last_reconnect_ms > 5000) {
            s_last_reconnect_ms = now;
            reconnect();
        }
    }
    s_mqtt.loop();
}

void mqtt_publish_payload(const knockknock_payload_t *p)
{
    const char *topic = (p->event_type == EVENT_ALARM)
        ? "knockknock/alarm"
        : "knockknock/telemetry";

    /* Serialize to JSON for InfluxDB/Grafana readability */
    StaticJsonDocument<512> doc;
    doc["node_id"]           = p->node_id;
    doc["event_type"]        = p->event_type;
    doc["timestamp_ms"]      = p->timestamp_ms;
    doc["band_energy_0"]     = p->band_energy[0];
    doc["band_energy_1"]     = p->band_energy[1];
    doc["band_energy_2"]     = p->band_energy[2];
    doc["band_energy_3"]     = p->band_energy[3];
    doc["band_energy_4"]     = p->band_energy[4];
    doc["spectral_centroid"] = p->spectral_centroid;
    doc["spectral_entropy"]  = p->spectral_entropy;
    doc["crest_factor"]      = p->crest_factor;
    doc["rms_amplitude"]     = p->rms_amplitude;
    doc["anomaly_score"]     = p->anomaly_score;

    char buf[512];
    size_t n = serializeJson(doc, buf, sizeof(buf));
    s_mqtt.publish(topic, buf, n);
}

void mqtt_publish_heartbeat()
{
    s_mqtt.publish("knockknock/status", "{\"status\":\"ok\"}", false);
}

bool mqtt_is_connected() { return s_mqtt.connected(); }
