#include "hub.h"
#include "config.h"
#include "serial_bridge.h"
#include "cloud_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"
#include "secure_store.h"

static const char *TAG = "HUB";

#define MAX_DEVICES      10
#define REGISTRY_VERSION 2

typedef struct {
    uint8_t  mac[6];
    char     name[32];
    bool     active;
    bool     train_pending;
    uint32_t train_duration_ms;
} hub_device_t;

typedef struct {
    uint8_t      version;
    int          count;
    hub_device_t devices[MAX_DEVICES];
} hub_device_registry_t;

static hub_device_registry_t s_registry  = {0};
static bool                  s_alarm_enabled = true;

typedef struct {
    uint8_t msg_type;
    uint8_t payload[240];
    size_t  len;
} hub_event_t;

static QueueHandle_t s_hub_queue = NULL;

/* -------------------------------------------------------------------------- */
/*  Registry helpers                                                           */
/* -------------------------------------------------------------------------- */

static void load_registry(void) {
    size_t len = sizeof(s_registry);
    esp_err_t err = secure_store_read_blob("device_list", &s_registry, &len);
    if (err != ESP_OK || s_registry.version != REGISTRY_VERSION) {
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "Registry version mismatch (got %d, want %d), clearing",
                     s_registry.version, REGISTRY_VERSION);
        } else {
            ESP_LOGW(TAG, "No device registry found, starting fresh");
        }
        memset(&s_registry, 0, sizeof(s_registry));
        s_registry.version = REGISTRY_VERSION;
    } else {
        ESP_LOGI(TAG, "Loaded %d devices from registry (v%d)",
                 s_registry.count, s_registry.version);
    }
}

static void save_registry(void) {
    esp_err_t err = secure_store_write_blob("device_list", &s_registry, sizeof(s_registry));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save device registry: %s", esp_err_to_name(err));
    }
}

static hub_device_t *hub_find_device(const uint8_t *mac) {
    for (int i = 0; i < s_registry.count; ++i) {
        if (memcmp(s_registry.devices[i].mac, mac, 6) == 0) {
            return &s_registry.devices[i];
        }
    }
    return NULL;
}

static bool parse_mac_address(const char *mac_str, uint8_t *mac_out) {
    int values[6];
    if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) == 6) {
        for (int i = 0; i < 6; ++i) mac_out[i] = (uint8_t)values[i];
        return true;
    }
    return false;
}

static void hub_register_device(const uint8_t *mac, const char *name, bool active) {
    for (int i = 0; i < s_registry.count; ++i) {
        if (memcmp(s_registry.devices[i].mac, mac, 6) == 0) {
            if (name && strlen(name) > 0) {
                strncpy(s_registry.devices[i].name, name,
                        sizeof(s_registry.devices[i].name) - 1);
            }
            s_registry.devices[i].active = active;
            save_registry();
            return;
        }
    }
    if (s_registry.count < MAX_DEVICES) {
        hub_device_t *d = &s_registry.devices[s_registry.count];
        memcpy(d->mac, mac, 6);
        d->active = active;
        d->train_pending = false;
        d->train_duration_ms = 0;
        if (name && strlen(name) > 0) {
            strncpy(d->name, name, sizeof(d->name) - 1);
        } else {
            snprintf(d->name, sizeof(d->name), "Sensor %02X:%02X", mac[4], mac[5]);
        }
        s_registry.count++;
        save_registry();
    } else {
        ESP_LOGE(TAG, "Registry full, cannot register device");
    }
}

static bool hub_remove_device(const uint8_t *mac) {
    for (int i = 0; i < s_registry.count; ++i) {
        if (memcmp(s_registry.devices[i].mac, mac, 6) == 0) {
            for (int j = i; j < s_registry.count - 1; ++j) {
                s_registry.devices[j] = s_registry.devices[j + 1];
            }
            s_registry.count--;
            save_registry();
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/*  Buzzer                                                                     */
/* -------------------------------------------------------------------------- */

static void buzzer_init(void) {
    gpio_config_t io_conf = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << BUZZER_PIN),
        .pull_down_en = 0,
        .pull_up_en   = 0,
    };
    gpio_config(&io_conf);
    gpio_set_level(BUZZER_PIN, 0);
}

static void activate_buzzer(void) {
    ESP_LOGI(TAG, "Activating buzzer!");
    gpio_set_level(BUZZER_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(BUZZER_PIN, 0);
}

static void hub_publish_device_list_to_cloud(void) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "DEVICE_LIST_RESPONSE");
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s_registry.count; ++i) {
        cJSON *item = cJSON_CreateObject();
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_registry.devices[i].mac[0], s_registry.devices[i].mac[1],
                 s_registry.devices[i].mac[2], s_registry.devices[i].mac[3],
                 s_registry.devices[i].mac[4], s_registry.devices[i].mac[5]);
        cJSON_AddStringToObject(item, "mac", mac_str);
        cJSON_AddStringToObject(item, "name", s_registry.devices[i].name);
        cJSON_AddBoolToObject(item, "active", s_registry.devices[i].active);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(resp, "devices", arr);
    char *s = cJSON_PrintUnformatted(resp);
    if (s) {
        cloud_publish_response(s);
        free(s);
    }
    cJSON_Delete(resp);
}

/* -------------------------------------------------------------------------- */
/*  Serial bridge message handler (Gateway → Hub)                             */
/* -------------------------------------------------------------------------- */

static void process_gateway_msg(uint8_t msg_type, const uint8_t *payload, size_t len) {
    switch (msg_type) {

    case GW_TO_HUB_PAIR_NOTIF: {
        if (len < sizeof(sb_pair_notif_payload_t)) break;
        const sb_pair_notif_payload_t *p = (const sb_pair_notif_payload_t *)payload;
        ESP_LOGI(TAG, "Sensor pair / info request from %02X:%02X:%02X:%02X:%02X:%02X",
                 p->sensor_mac[0], p->sensor_mac[1], p->sensor_mac[2],
                 p->sensor_mac[3], p->sensor_mac[4], p->sensor_mac[5]);

        hub_register_device(p->sensor_mac, NULL, true);
        hub_publish_device_list_to_cloud();

        sb_info_resp_payload_t resp = {0};
        memcpy(resp.sensor_mac, p->sensor_mac, 6);

        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        resp.timestamp = tv_now.tv_sec;
        resp.do_reset  = 0;

        /* Deliver pending training command if one was queued via MQTT. */
        hub_device_t *dev = hub_find_device(p->sensor_mac);
        if (dev && dev->train_pending) {
            resp.do_ml_training  = 1;
            resp.ml_duration_ms  = dev->train_duration_ms;
            dev->train_pending   = false;
            dev->train_duration_ms = 0;
            save_registry();
            ESP_LOGI(TAG, "Delivering pending training command (%lu ms)",
                     (unsigned long)resp.ml_duration_ms);
        } else {
            resp.do_ml_training = 0;
            resp.ml_duration_ms = 0;
        }

        serial_bridge_send(HUB_TO_GW_INFO_RESP, (const uint8_t *)&resp, sizeof(resp));
        break;
    }

    case GW_TO_HUB_PAIR_SUCCESS: {
        if (len < sizeof(sb_pair_success_payload_t)) break;
        const sb_pair_success_payload_t *p = (const sb_pair_success_payload_t *)payload;
        ESP_LOGI(TAG, "Sensor pairing successful (cloud-initiated) for %02X:%02X:%02X:%02X:%02X:%02X",
                 p->sensor_mac[0], p->sensor_mac[1], p->sensor_mac[2],
                 p->sensor_mac[3], p->sensor_mac[4], p->sensor_mac[5]);

        hub_device_t *dev = hub_find_device(p->sensor_mac);
        if (dev) {
            dev->active = true;
            save_registry();
            hub_publish_device_list_to_cloud();
        }
        break;
    }

    case GW_TO_HUB_ALARM: {
        if (len < sizeof(sb_alarm_payload_t)) break;
        const sb_alarm_payload_t *p = (const sb_alarm_payload_t *)payload;
        ESP_LOGW(TAG, "Alarm code %d from %02X:%02X:%02X:%02X:%02X:%02X",
                 p->alarm_code,
                 p->sensor_mac[0], p->sensor_mac[1], p->sensor_mac[2],
                 p->sensor_mac[3], p->sensor_mac[4], p->sensor_mac[5]);

        hub_device_t *dev = hub_find_device(p->sensor_mac);
        if (dev) {
            if (!dev->active) {
                dev->active = true;
                save_registry();
                hub_publish_device_list_to_cloud();
            }
        }

        if (s_alarm_enabled) {
            activate_buzzer();
            cloud_publish_alarm(p->sensor_mac, p->alarm_code);
        } else {
            ESP_LOGI(TAG, "Alarm suppressed (alarm disabled via cloud)");
        }
        break;
    }

    case GW_TO_HUB_STATUS: {
        if (len < sizeof(sb_status_payload_t)) break;
        const sb_status_payload_t *p = (const sb_status_payload_t *)payload;
        ESP_LOGI(TAG, "Gateway status: uptime %lu s, peers %d",
                 (unsigned long)p->uptime_s, p->num_peers);

        // Risincronizza tutti i peer attivi/registrati al Gateway
        for (int i = 0; i < s_registry.count; ++i) {
            sb_add_peer_payload_t add_peer;
            memcpy(add_peer.sensor_mac, s_registry.devices[i].mac, 6);
            serial_bridge_send(HUB_TO_GW_ADD_PEER, (const uint8_t *)&add_peer, sizeof(add_peer));
        }
        break;
    }

    case GW_TO_HUB_INFO_REQ: {
        if (len < sizeof(sb_pair_notif_payload_t)) break;
        const sb_pair_notif_payload_t *p = (const sb_pair_notif_payload_t *)payload;
        ESP_LOGI(TAG, "Info request from %02X:%02X:%02X:%02X:%02X:%02X",
                 p->sensor_mac[0], p->sensor_mac[1], p->sensor_mac[2],
                 p->sensor_mac[3], p->sensor_mac[4], p->sensor_mac[5]);

        /* Only respond with info — do NOT re-register the device. */
        hub_device_t *dev = hub_find_device(p->sensor_mac);
        if (dev) {
            if (!dev->active) {
                dev->active = true;
                save_registry();
                hub_publish_device_list_to_cloud();
            }
        }

        sb_info_resp_payload_t resp = {0};
        memcpy(resp.sensor_mac, p->sensor_mac, 6);

        struct timeval tv_now;
        gettimeofday(&tv_now, NULL);
        resp.timestamp = tv_now.tv_sec;
        resp.do_reset  = 0;

        /* Deliver pending training command if one was queued via MQTT. */
        dev = hub_find_device(p->sensor_mac);
        if (dev && dev->train_pending) {
            resp.do_ml_training  = 1;
            resp.ml_duration_ms  = dev->train_duration_ms;
            dev->train_pending   = false;
            dev->train_duration_ms = 0;
            save_registry();
            ESP_LOGI(TAG, "Delivering pending training command (%lu ms)",
                     (unsigned long)resp.ml_duration_ms);
        } else {
            resp.do_ml_training = 0;
            resp.ml_duration_ms = 0;
        }

        serial_bridge_send(HUB_TO_GW_INFO_RESP, (const uint8_t *)&resp, sizeof(resp));
        break;
    }

    default:
        ESP_LOGW(TAG, "Unknown message type from gateway: 0x%02X", msg_type);
        break;
    }
}

static void on_gateway_msg(uint8_t msg_type, const uint8_t *payload, size_t len) {
    if (!s_hub_queue) return;

    hub_event_t ev;
    ev.msg_type = msg_type;
    ev.len = (len > sizeof(ev.payload)) ? sizeof(ev.payload) : len;
    if (payload && ev.len > 0) {
        memcpy(ev.payload, payload, ev.len);
    }

    if (xQueueSend(s_hub_queue, &ev, 0) != pdPASS) {
        ESP_LOGE(TAG, "Hub event queue full! Dropped message type 0x%02X", msg_type);
    }
}

static void hub_task(void *pvParameters) {
    hub_event_t ev;
    ESP_LOGI(TAG, "Hub task started on Core %d", xPortGetCoreID());
    while (1) {
        if (xQueueReceive(s_hub_queue, &ev, pdMS_TO_TICKS(1000)) == pdTRUE) {
            process_gateway_msg(ev.msg_type, ev.payload, ev.len);
        } else {
            // Timeout every 1 second: retry pairing for inactive devices
            for (int i = 0; i < s_registry.count; ++i) {
                if (!s_registry.devices[i].active) {
                    ESP_LOGI(TAG, "Retrying pairing for inactive device %02X:%02X:%02X:%02X:%02X:%02X...",
                             s_registry.devices[i].mac[0], s_registry.devices[i].mac[1],
                             s_registry.devices[i].mac[2], s_registry.devices[i].mac[3],
                             s_registry.devices[i].mac[4], s_registry.devices[i].mac[5]);
                    sb_start_pairing_payload_t start_pairing;
                    memcpy(start_pairing.sensor_mac, s_registry.devices[i].mac, 6);
                    serial_bridge_send(HUB_TO_GW_START_PAIRING, (const uint8_t *)&start_pairing, sizeof(start_pairing));
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  MQTT command handler (Cloud → Hub)                                        */
/* -------------------------------------------------------------------------- */

void hub_handle_mqtt_command(const char *data, int len) {
    if (!data || len <= 0) return;

    char *buf = malloc(len + 1);
    if (!buf) return;
    memcpy(buf, data, len);
    buf[len] = '\0';

    ESP_LOGI(TAG, "MQTT command: %s", buf);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { ESP_LOGE(TAG, "Failed to parse JSON command"); return; }

    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (!type_item || !cJSON_IsString(type_item)) {
        ESP_LOGE(TAG, "No valid 'type' in command");
        cJSON_Delete(root);
        return;
    }
    const char *type = type_item->valuestring;

    /* ------------------------------------------------------------------ */
    /* DEVICE_LIST_REQUEST                                                  */
    /* ------------------------------------------------------------------ */
    if (strcmp(type, "DEVICE_LIST_REQUEST") == 0) {
        hub_publish_device_list_to_cloud();
    }

    /* ------------------------------------------------------------------ */
    /* ADD_DEVICE                                                           */
    /* ------------------------------------------------------------------ */
    else if (strcmp(type, "ADD_DEVICE") == 0) {
        cJSON *pl = cJSON_GetObjectItem(root, "payload");
        if (pl) {
            cJSON *mac_item  = cJSON_GetObjectItem(pl, "mac_address");
            cJSON *name_item = cJSON_GetObjectItem(pl, "device_name");
            if (mac_item && cJSON_IsString(mac_item)) {
                uint8_t mac[6];
                bool ok = parse_mac_address(mac_item->valuestring, mac);
                if (ok) {
                    const char *name = (name_item && cJSON_IsString(name_item))
                                       ? name_item->valuestring : "";
                    hub_register_device(mac, name, false); // Register as inactive initially
                    hub_publish_device_list_to_cloud();
                    
                    sb_start_pairing_payload_t start_pairing;
                    memcpy(start_pairing.sensor_mac, mac, 6);
                    serial_bridge_send(HUB_TO_GW_START_PAIRING, (const uint8_t *)&start_pairing, sizeof(start_pairing));

                    cJSON *ack = cJSON_CreateObject();
                    cJSON_AddStringToObject(ack, "type", "DEVICE_ACK");
                    cJSON_AddStringToObject(ack, "action", "ADD_DEVICE");
                    cJSON_AddStringToObject(ack, "mac", mac_item->valuestring);
                    cJSON_AddBoolToObject(ack, "success", true);
                    cJSON_AddStringToObject(ack, "message", "Device registered.");
                    char *s = cJSON_PrintUnformatted(ack);
                    if (s) { cloud_publish_response(s); free(s); }
                    cJSON_Delete(ack);
                }
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /* REMOVE_DEVICE                                                        */
    /* ------------------------------------------------------------------ */
    else if (strcmp(type, "REMOVE_DEVICE") == 0) {
        cJSON *pl = cJSON_GetObjectItem(root, "payload");
        if (pl) {
            cJSON *mac_item = cJSON_GetObjectItem(pl, "mac_address");
            if (mac_item && cJSON_IsString(mac_item)) {
                uint8_t mac[6];
                bool ok = parse_mac_address(mac_item->valuestring, mac);
                if (ok) {
                    bool removed = hub_remove_device(mac);
                    if (removed) {
                        sb_unpair_payload_t unpair;
                        memcpy(unpair.sensor_mac, mac, 6);
                        serial_bridge_send(HUB_TO_GW_UNPAIR,
                                           (const uint8_t *)&unpair, sizeof(unpair));
                    }
                    cJSON *ack = cJSON_CreateObject();
                    cJSON_AddStringToObject(ack, "type", "DEVICE_ACK");
                    cJSON_AddStringToObject(ack, "action", "REMOVE_DEVICE");
                    cJSON_AddStringToObject(ack, "mac", mac_item->valuestring);
                    cJSON_AddBoolToObject(ack, "success", removed);
                    cJSON_AddStringToObject(ack, "message",
                        removed ? "Device removed." : "Device not found.");
                    char *s = cJSON_PrintUnformatted(ack);
                    if (s) { cloud_publish_response(s); free(s); }
                    cJSON_Delete(ack);
                }
            }
        }
    }

    /* ------------------------------------------------------------------ */
    /* ALARM_ENABLE / ALARM_DISABLE                                         */
    /* ------------------------------------------------------------------ */
    else if (strcmp(type, "ALARM_ENABLE") == 0 || strcmp(type, "ALARM_DISABLE") == 0) {
        s_alarm_enabled = (strcmp(type, "ALARM_ENABLE") == 0);
        ESP_LOGI(TAG, "Alarms %s", s_alarm_enabled ? "enabled" : "disabled");
        cJSON *ack = cJSON_CreateObject();
        cJSON_AddStringToObject(ack, "type", "ALARM_ACK");
        cJSON_AddBoolToObject(ack, "alarm_enabled", s_alarm_enabled);
        char *s = cJSON_PrintUnformatted(ack);
        if (s) { cloud_publish_response(s); free(s); }
        cJSON_Delete(ack);
    }

    /* ------------------------------------------------------------------ */
    /* START_TRAINING                                                        */
    /* ------------------------------------------------------------------ */
    else if (strcmp(type, "START_TRAINING") == 0) {
        cJSON *pl       = cJSON_GetObjectItem(root, "payload");
        cJSON *mac_item = pl ? cJSON_GetObjectItem(pl, "mac_address") : NULL;
        cJSON *dur_item = pl ? cJSON_GetObjectItem(pl, "duration_ms")  : NULL;

        if (mac_item && cJSON_IsString(mac_item) &&
            dur_item && cJSON_IsNumber(dur_item)) {
            uint8_t mac[6];
            bool found = false;
            if (parse_mac_address(mac_item->valuestring, mac)) {
                hub_device_t *dev = hub_find_device(mac);
                if (dev) {
                    dev->train_pending     = true;
                    dev->train_duration_ms = (uint32_t)dur_item->valuedouble;
                    save_registry();
                    found = true;
                    ESP_LOGI(TAG, "Training queued for %s (%lu ms)",
                             mac_item->valuestring,
                             (unsigned long)dev->train_duration_ms);
                } else {
                    ESP_LOGW(TAG, "START_TRAINING: sensor not in registry");
                }
            }
            cJSON *ack = cJSON_CreateObject();
            cJSON_AddStringToObject(ack, "type", "TRAINING_ACK");
            cJSON_AddStringToObject(ack, "mac", mac_item->valuestring);
            cJSON_AddBoolToObject(ack, "queued", found);
            if (!found) {
                cJSON_AddStringToObject(ack, "reason", "device not found");
            }
            char *s = cJSON_PrintUnformatted(ack);
            if (s) { cloud_publish_response(s); free(s); }
            cJSON_Delete(ack);
        }
    }

    else {
        ESP_LOGW(TAG, "Unknown command type: %s", type);
    }

    cJSON_Delete(root);
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                 */
/* -------------------------------------------------------------------------- */

void hub_start(void) {
    ESP_LOGI(TAG, "Starting Hub logic");
    buzzer_init();
    load_registry();

    s_hub_queue = xQueueCreate(10, sizeof(hub_event_t));
    if (!s_hub_queue) {
        ESP_LOGE(TAG, "Failed to create Hub event queue!");
        return;
    }

    // Pin hub_task to Core 1 (APP CPU)
    xTaskCreatePinnedToCore(hub_task, "hub_task", 4096, NULL, 3, NULL, 1);

    serial_bridge_config_t sb_cfg = {
        .uart_port = HUB_UART_PORT,
        .tx_pin    = HUB_UART_TX_PIN,
        .rx_pin    = HUB_UART_RX_PIN,
        .baud_rate = HUB_UART_BAUD,
        .recv_cb   = on_gateway_msg,
    };
    ESP_ERROR_CHECK(serial_bridge_init(&sb_cfg));
    ESP_LOGI(TAG, "Hub started");
}
