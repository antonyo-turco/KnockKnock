/**
 * @file device_list.cpp
 * @brief Device list implementation for KnockKnock edge hub (Arduino).
 *
 * Implements device list management functions declared in message_types.h.
 * Uses ArduinoJson (v7.x) for JSON building and parsing.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <Arduino.h>
#include <ArduinoJson.h>

#include "message_types.h"

static const char *TAG = "DEVLIST";

/* =========================================================
 *  ArduinoJson document size helper
 * ========================================================= */
/* MAX_JSON_MSG_LEN is 512, so a StaticJsonDocument<1024> gives plenty
 * of headroom for the JSON object model before serialization. */
#define JSON_DOC_SIZE  1024

/* =========================================================
 *  Device list operations
 * ========================================================= */

void device_list_init(device_list_t *list)
{
    if (list == NULL) {
        Serial.printf("[%s] device_list_init: NULL list pointer\n", TAG);
        return;
    }
    memset(list, 0, sizeof(device_list_t));
    list->count = 0;
}

bool device_list_add(device_list_t *list, const char *mac, const char *name)
{
    if (list == NULL || mac == NULL || name == NULL) {
        Serial.printf("[%s] device_list_add: NULL argument\n", TAG);
        return false;
    }

    /* Check for duplicate */
    if (device_list_find(list, mac) >= 0) {
        Serial.printf("[%s] Device with MAC %s already exists in list\n", TAG, mac);
        return false;
    }

    /* Check if list is full */
    if (list->count >= MAX_DEVICES) {
        Serial.printf("[%s] Device list is full (max %d devices)\n", TAG, MAX_DEVICES);
        return false;
    }

    /* Add the new device */
    device_entry_t *entry = &list->devices[list->count];
    strncpy(entry->mac_address, mac, MAX_MAC_ADDRESS_LEN - 1);
    entry->mac_address[MAX_MAC_ADDRESS_LEN - 1] = '\0';
    strncpy(entry->device_name, name, MAX_DEVICE_NAME_LEN - 1);
    entry->device_name[MAX_DEVICE_NAME_LEN - 1] = '\0';
    entry->active = true;

    list->count++;
    Serial.printf("[%s] Device added: %s (%s), total: %d\n", TAG, mac, name, list->count);
    return true;
}

bool device_list_remove(device_list_t *list, const char *mac)
{
    if (list == NULL || mac == NULL) {
        Serial.printf("[%s] device_list_remove: NULL argument\n", TAG);
        return false;
    }

    int idx = device_list_find(list, mac);
    if (idx < 0) {
        Serial.printf("[%s] Device with MAC %s not found for removal\n", TAG, mac);
        return false;
    }

    /* Shift remaining entries down */
    for (int i = idx; i < list->count - 1; i++) {
        memcpy(&list->devices[i], &list->devices[i + 1], sizeof(device_entry_t));
    }

    /* Clear the last (now unused) slot */
    memset(&list->devices[list->count - 1], 0, sizeof(device_entry_t));
    list->count--;

    Serial.printf("[%s] Device removed: %s, total: %d\n", TAG, mac, list->count);
    return true;
}

int device_list_find(device_list_t *list, const char *mac)
{
    if (list == NULL || mac == NULL) {
        return -1;
    }

    for (int i = 0; i < list->count; i++) {
        if (strncasecmp(list->devices[i].mac_address, mac, MAX_MAC_ADDRESS_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

/* =========================================================
 *  JSON builders (ArduinoJson v7.x)
 * ========================================================= */

int build_add_device_json(const char *mac, const char *name, char *buf, size_t buf_size)
{
    if (mac == NULL || name == NULL || buf == NULL || buf_size == 0) {
        Serial.printf("[%s] build_add_device_json: NULL argument or zero buffer\n", TAG);
        return 0;
    }

    StaticJsonDocument<JSON_DOC_SIZE> doc;
    doc["type"] = MSG_TYPE_ADD_DEVICE;
    doc["mac"]  = mac;
    doc["name"] = name;

    size_t json_len = serializeJson(doc, buf, buf_size);
    if (json_len == 0) {
        Serial.printf("[%s] build_add_device_json: serialization failed (buf too small?)\n", TAG);
        return 0;
    }

    return (int)json_len;
}

int build_device_list_json(const device_list_t *list, char *buf, size_t buf_size)
{
    if (list == NULL || buf == NULL || buf_size == 0) {
        Serial.printf("[%s] build_device_list_json: NULL argument or zero buffer\n", TAG);
        return 0;
    }

    StaticJsonDocument<JSON_DOC_SIZE> doc;
    doc["type"] = MSG_TYPE_DEVICE_LIST_RESP;

    JsonArray devices = doc["devices"].to<JsonArray>();

    for (int i = 0; i < list->count; i++) {
        JsonObject dev = devices.add<JsonObject>();
        dev["mac"]    = list->devices[i].mac_address;
        dev["name"]   = list->devices[i].device_name;
        dev["active"] = list->devices[i].active;
    }

    size_t json_len = serializeJson(doc, buf, buf_size);
    if (json_len == 0) {
        Serial.printf("[%s] build_device_list_json: serialization failed (buf too small?)\n", TAG);
        return 0;
    }

    return (int)json_len;
}

int build_ack_json(const char *action, const char *mac, bool success,
                   const char *message, char *buf, size_t buf_size)
{
    if (action == NULL || mac == NULL || message == NULL ||
        buf == NULL || buf_size == 0) {
        Serial.printf("[%s] build_ack_json: NULL argument or zero buffer\n", TAG);
        return 0;
    }

    StaticJsonDocument<JSON_DOC_SIZE> doc;
    doc["type"]    = MSG_TYPE_DEVICE_ACK;
    doc["action"]  = action;
    doc["mac"]     = mac;
    doc["success"] = success;
    doc["message"] = message;

    size_t json_len = serializeJson(doc, buf, buf_size);
    if (json_len == 0) {
        Serial.printf("[%s] build_ack_json: serialization failed (buf too small?)\n", TAG);
        return 0;
    }

    return (int)json_len;
}

/* =========================================================
 *  JSON parser (ArduinoJson v7.x)
 * ========================================================= */

char* parse_message_type(const char *json, char *mac_out, size_t mac_size,
                         char *name_out, size_t name_size)
{
    if (json == NULL) {
        Serial.printf("[%s] parse_message_type: NULL JSON string\n", TAG);
        return NULL;
    }

    /* Initialize outputs */
    if (mac_out != NULL && mac_size > 0) {
        mac_out[0] = '\0';
    }
    if (name_out != NULL && name_size > 0) {
        name_out[0] = '\0';
    }

    StaticJsonDocument<JSON_DOC_SIZE> doc;
    DeserializationError err = deserializeJson(doc, json);

    if (err) {
        Serial.printf("[%s] JSON parse error: %s\n", TAG, err.c_str());
        return NULL;
    }

    /* Extract type field */
    const char *type_str = doc["type"];
    if (type_str == NULL) {
        Serial.printf("[%s] JSON missing 'type' field\n", TAG);
        return NULL;
    }

    char *result = strdup(type_str);
    if (result == NULL) {
        Serial.printf("[%s] Failed to allocate memory for message type\n", TAG);
        return NULL;
    }

    /*
     * Determine where to look for mac/name fields.
     *
     * Cloud format: {"type":"...","payload":{"mac_address":"...","device_name":"..."}}
     * Edge format:  {"type":"...","mac":"...","name":"..."}
     *
     * If a "payload" sub-object is present, use it; otherwise use the root object.
     */
    JsonObject data = doc["payload"];
    if (data.isNull()) {
        data = doc.as<JsonObject>();  /* flat edge format */
    }

    /* Extract MAC address: try "mac_address" first, then "mac" */
    if (mac_out != NULL && mac_size > 0) {
        const char *mac_str = data["mac_address"];
        if (mac_str == NULL) {
            mac_str = data["mac"];
        }
        if (mac_str != NULL) {
            strncpy(mac_out, mac_str, mac_size - 1);
            mac_out[mac_size - 1] = '\0';
        }
    }

    /* Extract device name: try "device_name" first, then "name" */
    if (name_out != NULL && name_size > 0) {
        const char *name_str = data["device_name"];
        if (name_str == NULL) {
            name_str = data["name"];
        }
        if (name_str != NULL) {
            strncpy(name_out, name_str, name_size - 1);
            name_out[name_size - 1] = '\0';
        }
    }

    return result;
}
