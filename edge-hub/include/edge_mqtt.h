#pragma once
#include "message_types.h"

void mqtt_init(const char *broker_ip, uint16_t port);
void mqtt_loop();   // call every loop() iteration
void mqtt_publish_payload(const knockknock_payload_t *payload);
void mqtt_publish_heartbeat();
bool mqtt_is_connected();
