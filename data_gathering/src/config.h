#ifndef CONFIG_H
#define CONFIG_H

// WiFi Configuration
#define WIFI_SSID "Vodafone-mango"
#define WIFI_PASSWORD "Mangoblu2020"
#define SERVER_IP "192.168.1.11"
#define SERVER_PORT 5010

// NTP Configuration
#define NTP_SERVER "pool.ntp.org"
#define TZ_OFFSET 2  // UTC+2 (ora legale italiana)

// Build the server URL
#define SERVER_URL "http://" SERVER_IP ":" #SERVER_PORT "/api/data/bulk"

// Button pins (set to -1 to disable)
#define BUTTON_PAUSE_PIN -1
#define BUTTON_SEND_PIN -1

#endif
