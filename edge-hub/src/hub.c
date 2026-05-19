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

static const char *TAG = "HUB";

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
