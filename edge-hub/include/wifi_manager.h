#pragma once
#include <Arduino.h>

void wifi_init(const char *ssid, const char *password);
bool wifi_is_connected();
