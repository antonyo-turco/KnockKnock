#include "hub.h"
#include "config.h"
#include "serial_bridge.h"
#include "cloud_task.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>
#include "cJSON.h"
#include "secure_store.h"

static const char *TAG = "HUB";

#define MAX_DEVICES 10

typedef struct {
    uint8_t mac[6];
    char name[32];
    bool active;
} hub_device_t;

typedef struct {
    int count;
    hub_device_t devices[MAX_DEVICES];
} hub_device_registry_t;

static hub_device_registry_t s_registry = {0};

static void load_registry(void) {
    size_t len = sizeof(s_registry);
    esp_err_t err = secure_store_read_blob("device_list", &s_registry, &len);
    if (err != ESP_OK) {
        s_registry.count = 0;
        ESP_LOGW(TAG, "No device registry found in NVS, starting fresh");
    } else {
        ESP_LOGI(TAG, "Loaded %d devices from registry", s_registry.count);
    }
}

static void save_registry(void) {
    esp_err_t err = secure_store_write_blob("device_list", &s_registry, sizeof(s_registry));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save device registry: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Device registry saved to NVS");
    }
}

static bool parse_mac_address(const char *mac_str, uint8_t *mac_out) {
    int values[6];
    if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) == 6) {
        for (int i = 0; i < 6; ++i) {
            mac_out[i] = (uint8_t)values[i];
        }
        return true;
    }
    return false;
}

static void hub_register_device(const uint8_t *mac, const char *name, bool active) {
    // Check if already in registry
    for (int i = 0; i < s_registry.count; ++i) {
        if (memcmp(s_registry.devices[i].mac, mac, 6) == 0) {
            // Update fields
            if (name && strlen(name) > 0) {
                strncpy(s_registry.devices[i].name, name, sizeof(s_registry.devices[i].name) - 1);
            }
            s_registry.devices[i].active = active;
            save_registry();
            return;
        }
    }
    
    // Add new device
    if (s_registry.count < MAX_DEVICES) {
        hub_device_t *d = &s_registry.devices[s_registry.count];
        memcpy(d->mac, mac, 6);
        d->active = active;
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
            // Shift remaining devices
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

static void buzzer_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << BUZZER_PIN),
        .pull_down_en = 0,
        .pull_up_en = 0
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

static void on_gateway_msg(uint8_t msg_type, const uint8_t *payload, size_t len) {
    switch (msg_type) {
        case GW_TO_HUB_PAIR_NOTIF: {
            if (len >= sizeof(sb_pair_notif_payload_t)) {
                const sb_pair_notif_payload_t *p = (const sb_pair_notif_payload_t *)payload;
                ESP_LOGI(TAG, "Sensor pair / info request from %02X:%02X:%02X:%02X:%02X:%02X",
                         p->sensor_mac[0], p->sensor_mac[1], p->sensor_mac[2],
                         p->sensor_mac[3], p->sensor_mac[4], p->sensor_mac[5]);
                
                // Register device in hub database
                hub_register_device(p->sensor_mac, NULL, true);

                // Send INFO_RESP
                sb_info_resp_payload_t resp = {0};
                memcpy(resp.sensor_mac, p->sensor_mac, 6);
                
                struct timeval tv_now;
                gettimeofday(&tv_now, NULL);
                resp.timestamp = tv_now.tv_sec;
                resp.do_ml_training = 0;
                resp.ml_duration_ms = 0;
                resp.do_reset = 0;

                serial_bridge_send(HUB_TO_GW_INFO_RESP, (const uint8_t *)&resp, sizeof(resp));
            }
            break;
        }
        case GW_TO_HUB_ALARM: {
            if (len >= sizeof(sb_alarm_payload_t)) {
                const sb_alarm_payload_t *p = (const sb_alarm_payload_t *)payload;
                ESP_LOGW(TAG, "Alarm code %d from %02X:%02X:%02X:%02X:%02X:%02X",
                         p->alarm_code,
                         p->sensor_mac[0], p->sensor_mac[1], p->sensor_mac[2],
                         p->sensor_mac[3], p->sensor_mac[4], p->sensor_mac[5]);
                
                activate_buzzer();
                cloud_publish_alarm(p->sensor_mac, p->alarm_code);
            }
            break;
        }
        case GW_TO_HUB_STATUS: {
            if (len >= sizeof(sb_status_payload_t)) {
                const sb_status_payload_t *p = (const sb_status_payload_t *)payload;
                ESP_LOGI(TAG, "Gateway Status: Uptime %lu s, Peers: %d", (unsigned long)p->uptime_s, p->num_peers);
            }
            break;
        }
        default:
            ESP_LOGW(TAG, "Unknown message type from gateway: 0x%02X", msg_type);
            break;
    }
}

void hub_start(void) {
    ESP_LOGI(TAG, "Starting Hub logic");
    buzzer_init();
    
    // Load device registry from secure NVS
    load_registry();
    
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

void hub_handle_mqtt_command(const char *data, int len) {
    if (!data || len <= 0) return;

    // Safely copy and null-terminate the JSON payload
    char *buf = malloc(len + 1);
    if (!buf) return;
    memcpy(buf, data, len);
    buf[len] = '\0';

    ESP_LOGI(TAG, "Parsing MQTT command: %s", buf);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON command");
        return;
    }

    cJSON *type_item = cJSON_GetObjectItem(root, "type");
    if (!type_item || !cJSON_IsString(type_item)) {
        ESP_LOGE(TAG, "No valid 'type' key in command");
        cJSON_Delete(root);
        return;
    }

    const char *type = type_item->valuestring;
    ESP_LOGI(TAG, "Command type: %s", type);

    if (strcmp(type, "DEVICE_LIST_REQUEST") == 0) {
        cJSON *resp_root = cJSON_CreateObject();
        cJSON_AddStringToObject(resp_root, "type", "DEVICE_LIST_RESPONSE");
        cJSON *devices_arr = cJSON_CreateArray();
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
            cJSON_AddItemToArray(devices_arr, item);
        }
        cJSON_AddItemToObject(resp_root, "devices", devices_arr);
        char *json_str = cJSON_PrintUnformatted(resp_root);
        if (json_str) {
            cloud_publish_response(json_str);
            free(json_str);
        }
        cJSON_Delete(resp_root);
    } 
    else if (strcmp(type, "ADD_DEVICE") == 0) {
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (payload) {
            cJSON *mac_item = cJSON_GetObjectItem(payload, "mac_address");
            cJSON *name_item = cJSON_GetObjectItem(payload, "device_name");
            if (mac_item && cJSON_IsString(mac_item)) {
                const char *mac_str = mac_item->valuestring;
                const char *name_str = (name_item && cJSON_IsString(name_item)) ? name_item->valuestring : "";
                
                uint8_t mac[6];
                bool ok = parse_mac_address(mac_str, mac);
                if (ok) {
                    hub_register_device(mac, name_str, false); // Add as inactive until first contact
                    
                    // Respond with DeviceAck
                    cJSON *ack = cJSON_CreateObject();
                    cJSON_AddStringToObject(ack, "type", "DEVICE_ACK");
                    cJSON_AddStringToObject(ack, "action", "ADD_DEVICE");
                    cJSON_AddStringToObject(ack, "mac", mac_str);
                    cJSON_AddBoolToObject(ack, "success", true);
                    cJSON_AddStringToObject(ack, "message", "Device registered in Hub list.");
                    char *json_str = cJSON_PrintUnformatted(ack);
                    if (json_str) {
                        cloud_publish_response(json_str);
                        free(json_str);
                    }
                    cJSON_Delete(ack);
                }
            }
        }
    } 
    else if (strcmp(type, "REMOVE_DEVICE") == 0) {
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (payload) {
            cJSON *mac_item = cJSON_GetObjectItem(payload, "mac_address");
            if (mac_item && cJSON_IsString(mac_item)) {
                const char *mac_str = mac_item->valuestring;
                uint8_t mac[6];
                bool ok = parse_mac_address(mac_str, mac);
                if (ok) {
                    bool removed = hub_remove_device(mac);
                    
                    if (removed) {
                        // Send unpair command to Gateway over Serial Bridge
                        sb_unpair_payload_t unpair;
                        memcpy(unpair.sensor_mac, mac, 6);
                        serial_bridge_send(HUB_TO_GW_UNPAIR, (const uint8_t *)&unpair, sizeof(unpair));
                    }

                    // Respond with DeviceAck
                    cJSON *ack = cJSON_CreateObject();
                    cJSON_AddStringToObject(ack, "type", "DEVICE_ACK");
                    cJSON_AddStringToObject(ack, "action", "REMOVE_DEVICE");
                    cJSON_AddStringToObject(ack, "mac", mac_str);
                    cJSON_AddBoolToObject(ack, "success", removed);
                    cJSON_AddStringToObject(ack, "message", removed ? "Device removed and un-paired." : "Device not found in Hub registry.");
                    char *json_str = cJSON_PrintUnformatted(ack);
                    if (json_str) {
                        cloud_publish_response(json_str);
                        free(json_str);
                    }
                    cJSON_Delete(ack);
                }
            }
        }
    }

    cJSON_Delete(root);
}
