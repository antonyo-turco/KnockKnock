/**
 * @file gateway.c
 * @brief Gateway orchestrator implementation.
 *
 * Data flow:
 *
 *  Sensor  --[ESP-NOW]--> gateway (espnow_manager recv_cb)
 *                              |
 *                     [internal queue]
 *                              |
 *                       gateway_task  ----[serial_bridge]--> Hub
 *
 *  Hub  --[serial_bridge recv_cb]--> gateway_task ----[ESP-NOW]--> Sensor
 */

#include "gateway.h"
#include "espnow_manager.h"
#include "serial_bridge.h"
#include "secure_store.h"

#include "esp_log.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include <string.h>

static const char *TAG = "GATEWAY";

/* -------------------------------------------------------------------------- */
/*  UART pin configuration (adjust to your hardware)                          */
/* -------------------------------------------------------------------------- */

#define GW_UART_PORT   1
#define GW_UART_TX_PIN 7
#define GW_UART_RX_PIN 6
#define GW_UART_BAUD   921600  /* High baud for low-latency relay. */

/* -------------------------------------------------------------------------- */
/*  Internal event queue                                                       */
/* -------------------------------------------------------------------------- */

/** Event types processed by the gateway task. */
typedef enum {
    GW_EVT_ESPNOW_RECV, /**< Packet received from a sensor over ESP-NOW.   */
    GW_EVT_SERIAL_RECV, /**< Frame received from the Hub over serial.       */
} gw_event_type_t;

/** Maximum payload size carried in a gateway event. */
#define GW_EVT_MAX_PAYLOAD 240u

/** An event posted to the gateway queue. */
typedef struct {
    gw_event_type_t type;
    uint8_t         src_mac[6];                /**< Sender MAC (ESP-NOW events).  */
    uint8_t         msg_type;                  /**< Protocol message type byte.   */
    size_t          payload_len;
    uint8_t         payload[GW_EVT_MAX_PAYLOAD];
} gw_event_t;

#define GW_QUEUE_DEPTH   20u
#define GW_TASK_STACK    6144u
#define GW_TASK_PRIORITY 6u
#define STATUS_PERIOD_S  30u   /**< Heartbeat interval sent to Hub (seconds). */

static QueueHandle_t s_gw_queue      = NULL;
static TaskHandle_t  s_gw_task_handle = NULL;

/* -------------------------------------------------------------------------- */
/*  ESP-NOW → Hub forwarding logic                                             */
/* -------------------------------------------------------------------------- */

/**
 * @brief Handle a packet from a sensor and forward it to the Hub.
 */
static void handle_espnow_event(const gw_event_t *evt)
{
    const gw_espnow_packet_t *pkt = (const gw_espnow_packet_t *)evt->payload;

    switch (pkt->type) {
    case MSG_TYPE_PAIR: {
        ESP_LOGI(TAG, "Received MSG_TYPE_PAIR from %02X:%02X:%02X:%02X:%02X:%02X",
                 evt->src_mac[0], evt->src_mac[1], evt->src_mac[2],
                 evt->src_mac[3], evt->src_mac[4], evt->src_mac[5]);
        /* Accept the sensor as a new encrypted peer. */
        esp_err_t err = espnow_manager_pair_peer(evt->src_mac);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to pair peer.");
            break;
        }

        /* Notify the Hub of the new pairing. */
        sb_pair_notif_payload_t notif;
        memcpy(notif.sensor_mac, evt->src_mac, 6);
        serial_bridge_send(GW_TO_HUB_PAIR_NOTIF,
                           (const uint8_t *)&notif, sizeof(notif));
        break;
    }

    case MSG_TYPE_ALARM: {
        sb_alarm_payload_t alarm;
        memcpy(alarm.sensor_mac, evt->src_mac, 6);
        alarm.alarm_code = pkt->payload.alarm_code;

        ESP_LOGI(TAG, "Alarm code=0x%02X from %02X:%02X:%02X:%02X:%02X:%02X",
                 alarm.alarm_code,
                 evt->src_mac[0], evt->src_mac[1], evt->src_mac[2],
                 evt->src_mac[3], evt->src_mac[4], evt->src_mac[5]);

        serial_bridge_send(GW_TO_HUB_ALARM,
                           (const uint8_t *)&alarm, sizeof(alarm));
        break;
    }

    case MSG_TYPE_INFO_REQ: {
        /*
         * The sensor is asking for information.  The gateway is a pure relay:
         * forward the request to the Hub and wait for a HUB_TO_GW_INFO_RESP
         * event which will carry the sensor MAC to route the answer back.
         *
         * To let the Hub know who is asking, we send the sensor MAC embedded
         * in the request.  We reuse sb_pair_notif_payload_t (just the MAC)
         * since it has the right layout.
         */
        sb_pair_notif_payload_t req;
        memcpy(req.sensor_mac, evt->src_mac, 6);
        serial_bridge_send(GW_TO_HUB_INFO_REQ,
                           (const uint8_t *)&req, sizeof(req));
        ESP_LOGD(TAG, "INFO_REQ relayed to Hub for sensor %02X:%02X:%02X:%02X:%02X:%02X",
                 evt->src_mac[0], evt->src_mac[1], evt->src_mac[2],
                 evt->src_mac[3], evt->src_mac[4], evt->src_mac[5]);
        break;
    }

    case MSG_TYPE_PAIR_ACK: {
        ESP_LOGI(TAG, "Received MSG_TYPE_PAIR_ACK from %02X:%02X:%02X:%02X:%02X:%02X",
                 evt->src_mac[0], evt->src_mac[1], evt->src_mac[2],
                 evt->src_mac[3], evt->src_mac[4], evt->src_mac[5]);
                 
        /* Upgrade peer to encrypted! */
        espnow_manager_add_peer(evt->src_mac);

        /* Forward success to the Hub. */
        sb_pair_success_payload_t succ;
        memcpy(succ.sensor_mac, evt->src_mac, 6);
        serial_bridge_send(GW_TO_HUB_PAIR_SUCCESS,
                           (const uint8_t *)&succ, sizeof(succ));
        break;
    }

    default:
        ESP_LOGW(TAG, "Unknown ESP-NOW message type: 0x%02X", pkt->type);
        break;
    }
}

/* -------------------------------------------------------------------------- */
/*  Hub → Sensor forwarding logic                                              */
/* -------------------------------------------------------------------------- */

/**
 * @brief Handle a frame from the Hub and forward it to the correct sensor.
 */
static void handle_serial_event(const gw_event_t *evt)
{
    switch (evt->msg_type) {
    case HUB_TO_GW_INFO_RESP: {
        if (evt->payload_len < sizeof(sb_info_resp_payload_t)) {
            ESP_LOGW(TAG, "INFO_RESP payload too short (%u bytes).",
                     (unsigned)evt->payload_len);
            break;
        }

        const sb_info_resp_payload_t *resp =
            (const sb_info_resp_payload_t *)evt->payload;

        /* Build the ESP-NOW response packet to forward to the sensor. */
        gw_espnow_packet_t pkt;
        pkt.type                           = MSG_TYPE_INFO_RESP;
        pkt.payload.info_resp.timestamp    = resp->timestamp;
        pkt.payload.info_resp.do_ml_training = resp->do_ml_training;
        pkt.payload.info_resp.ml_duration_ms = resp->ml_duration_ms;
        pkt.payload.info_resp.do_reset       = resp->do_reset;

        bool ok = espnow_manager_send(resp->sensor_mac, &pkt,
                                       sizeof(pkt.type) +
                                       sizeof(pkt.payload.info_resp),
                                       200);
        if (!ok) {
            ESP_LOGE(TAG, "Failed to forward INFO_RESP to sensor.");
        } else {
            ESP_LOGI(TAG, "INFO_RESP forwarded to %02X:%02X:%02X:%02X:%02X:%02X",
                     resp->sensor_mac[0], resp->sensor_mac[1], resp->sensor_mac[2],
                     resp->sensor_mac[3], resp->sensor_mac[4], resp->sensor_mac[5]);
        }
        break;
    }

    case HUB_TO_GW_UNPAIR: {
        if (evt->payload_len < sizeof(sb_unpair_payload_t)) break;

        const sb_unpair_payload_t *up = (const sb_unpair_payload_t *)evt->payload;
        esp_now_del_peer(up->sensor_mac);
        ESP_LOGI(TAG, "Sensor %02X:%02X:%02X:%02X:%02X:%02X un-paired by Hub.",
                 up->sensor_mac[0], up->sensor_mac[1], up->sensor_mac[2],
                 up->sensor_mac[3], up->sensor_mac[4], up->sensor_mac[5]);
        break;
    }

    case HUB_TO_GW_ADD_PEER: {
        if (evt->payload_len < sizeof(sb_add_peer_payload_t)) break;

        const sb_add_peer_payload_t *ap = (const sb_add_peer_payload_t *)evt->payload;
        esp_err_t err = espnow_manager_add_peer(ap->sensor_mac);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Sensor %02X:%02X:%02X:%02X:%02X:%02X registered as peer.",
                     ap->sensor_mac[0], ap->sensor_mac[1], ap->sensor_mac[2],
                     ap->sensor_mac[3], ap->sensor_mac[4], ap->sensor_mac[5]);
        }
        break;
    }

    case HUB_TO_GW_REKEY: {
        if (evt->payload_len < sizeof(sb_rekey_payload_t)) break;

        const sb_rekey_payload_t *rk = (const sb_rekey_payload_t *)evt->payload;

        /* Persist the new PMK/LMK to secure NVS. */
        char pmk_str[17], lmk_str[17];
        memcpy(pmk_str, rk->new_pmk, 16); pmk_str[16] = '\0';
        memcpy(lmk_str, rk->new_lmk, 16); lmk_str[16] = '\0';

        /* Keys will be picked up on next boot by espnow_manager_init(). */
        secure_store_write_string("esp_now_pmk", pmk_str);
        secure_store_write_string("esp_now_lmk", lmk_str);

        ESP_LOGW(TAG, "New ESP-NOW keys received and stored. Reboot required.");
        break;
    }

    case HUB_TO_GW_START_PAIRING: {
        if (evt->payload_len < sizeof(sb_start_pairing_payload_t)) break;

        const sb_start_pairing_payload_t *sp = (const sb_start_pairing_payload_t *)evt->payload;
        ESP_LOGI(TAG, "Instructed to start pairing with sensor %02X:%02X:%02X:%02X:%02X:%02X",
                 sp->sensor_mac[0], sp->sensor_mac[1], sp->sensor_mac[2],
                 sp->sensor_mac[3], sp->sensor_mac[4], sp->sensor_mac[5]);
                 
        esp_err_t err = espnow_manager_send_pairing_req(sp->sensor_mac);
        if (err != ESP_OK) {
             ESP_LOGE(TAG, "Failed to send pairing request to sensor.");
        }
        break;
    }

    default:
        ESP_LOGW(TAG, "Unknown Hub message type: 0x%02X", evt->msg_type);
        break;
    }
}

/* -------------------------------------------------------------------------- */
/*  Gateway main task                                                          */
/* -------------------------------------------------------------------------- */

/**
 * @brief Gateway task: drains the event queue and handles events.
 *        Also sends a periodic status heartbeat to the Hub.
 */
static void gateway_task(void *arg)
{
    gw_event_t evt;
    TickType_t last_status_tick = xTaskGetTickCount();

    while (true) {
        /* Wait for an event with a timeout equal to the heartbeat period. */
        if (xQueueReceive(s_gw_queue, &evt,
                          pdMS_TO_TICKS(STATUS_PERIOD_S * 1000UL)) == pdTRUE) {
            switch (evt.type) {
            case GW_EVT_ESPNOW_RECV:
                handle_espnow_event(&evt);
                break;
            case GW_EVT_SERIAL_RECV:
                handle_serial_event(&evt);
                break;
            default:
                break;
            }
        }

        /* Periodic heartbeat to Hub. */
        TickType_t now = xTaskGetTickCount();
        if ((now - last_status_tick) >= pdMS_TO_TICKS(STATUS_PERIOD_S * 1000UL)) {
            last_status_tick = now;

            esp_now_peer_num_t peer_num = {0};
            esp_now_get_peer_num(&peer_num);

            sb_status_payload_t status = {
                .num_peers = (uint8_t)peer_num.total_num,
                .uptime_s  = (uint32_t)(esp_timer_get_time() / 1000000ULL),
            };
            serial_bridge_send(GW_TO_HUB_STATUS,
                               (const uint8_t *)&status, sizeof(status));
            ESP_LOGD(TAG, "Status heartbeat: %d peers, uptime %lu s.",
                     status.num_peers, (unsigned long)status.uptime_s);
        }
    }

    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------- */
/*  Callbacks from ESP-NOW manager and serial bridge                           */
/* -------------------------------------------------------------------------- */

/**
 * @brief ESP-NOW receive callback (ISR context – post to queue only).
 */
static void on_espnow_recv(const uint8_t            *src_mac,
                           const gw_espnow_packet_t *packet,
                           int                       len)
{
    if (len <= 0 || (size_t)len > GW_EVT_MAX_PAYLOAD) return;

    gw_event_t evt = {0};
    evt.type        = GW_EVT_ESPNOW_RECV;
    evt.payload_len = (size_t)len;
    memcpy(evt.src_mac, src_mac, 6);
    memcpy(evt.payload, packet, (size_t)len);

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_gw_queue, &evt, &woken);
    if (woken) portYIELD_FROM_ISR();
}

/**
 * @brief Serial bridge receive callback (task context).
 */
static void on_serial_recv(uint8_t msg_type, const uint8_t *payload, size_t len)
{
    if (len > GW_EVT_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "Serial frame payload too large (%u bytes), dropped.", (unsigned)len);
        return;
    }

    gw_event_t evt = {0};
    evt.type        = GW_EVT_SERIAL_RECV;
    evt.msg_type    = msg_type;
    evt.payload_len = len;
    if (payload && len > 0) {
        memcpy(evt.payload, payload, len);
    }

    if (xQueueSend(s_gw_queue, &evt, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGW(TAG, "Gateway queue full – serial frame dropped.");
    }
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                 */
/* -------------------------------------------------------------------------- */

esp_err_t gateway_start(void)
{
    /* ------------------------------------------------------------------ */
    /* 1. Create the internal event queue.                                 */
    /* ------------------------------------------------------------------ */
    s_gw_queue = xQueueCreate(GW_QUEUE_DEPTH, sizeof(gw_event_t));
    if (s_gw_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create gateway queue.");
        return ESP_ERR_NO_MEM;
    }

    /* ------------------------------------------------------------------ */
    /* 2. Initialise ESP-NOW manager.                                      */
    /* ------------------------------------------------------------------ */
    esp_err_t err = espnow_manager_init(on_espnow_recv);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "espnow_manager_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ------------------------------------------------------------------ */
    /* 3. Initialise serial bridge to Hub.                                 */
    /* ------------------------------------------------------------------ */
    serial_bridge_config_t sb_cfg = {
        .uart_port = GW_UART_PORT,
        .tx_pin    = GW_UART_TX_PIN,
        .rx_pin    = GW_UART_RX_PIN,
        .baud_rate = GW_UART_BAUD,
        .recv_cb   = on_serial_recv,
    };
    err = serial_bridge_init(&sb_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "serial_bridge_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ------------------------------------------------------------------ */
    /* 4. Start the gateway orchestration task.                            */
    /* ------------------------------------------------------------------ */
    BaseType_t task_ok = xTaskCreate(gateway_task, "gateway",
                                      GW_TASK_STACK, NULL,
                                      GW_TASK_PRIORITY, &s_gw_task_handle);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create gateway task.");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Gateway started successfully.");
    return ESP_OK;
}
