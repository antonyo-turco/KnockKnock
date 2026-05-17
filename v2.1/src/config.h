#ifndef CONFIG_H
#define CONFIG_H

// ─────────────────────────────────────────────────────────────────────────────
//  Sampling & FFT
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int   SAMPLE_COUNT     = 256;
static constexpr int   FFT_SIZE         = 256;
static constexpr float SAMPLING_RATE_HZ = 100.0f;
static constexpr int   SAMPLE_PERIOD_MS = 10;

// ─────────────────────────────────────────────────────────────────────────────
//  Novelty buffer
// ─────────────────────────────────────────────────────────────────────────────

static constexpr int   NOVELTY_BUFFER_SIZE = 200;
static constexpr float NOVELTY_THRESHOLD   = 1.5f;

// ─────────────────────────────────────────────────────────────────────────────
//  WiFi — used only for NTP time sync
// ─────────────────────────────────────────────────────────────────────────────

#define WIFI_SSID      "Vodafone-mango"
#define WIFI_PASSWORD  "Mangoblu2020"

// Maximum time to wait for WiFi connection before giving up
static constexpr uint32_t WIFI_TIMEOUT_MS  = 10000;

// Maximum time to wait for NTP response
static constexpr uint32_t NTP_TIMEOUT_MS   = 8000;

// ─────────────────────────────────────────────────────────────────────────────
//  NTP & timezone
//
//  GMT_OFFSET_SEC:  seconds east of UTC  (Italy standard = 3600, DST = 7200)
//  DST_OFFSET_SEC:  daylight saving offset added on top (3600 or 0)
//
//  The firmware does NOT handle automatic DST transitions — update manually
//  if needed, or use a full timezone library.
// ─────────────────────────────────────────────────────────────────────────────

static constexpr long  GMT_OFFSET_SEC = 3600;    // UTC+1 (Italy standard time)
static constexpr int   DST_OFFSET_SEC = 3600;    // +1h DST (set 0 in winter)
#define NTP_SERVER  "pool.ntp.org"

// How often to resync time from NTP (milliseconds).
// Default: every 24h.  The sync interrupts between two windows.
static constexpr uint32_t TIME_SYNC_INTERVAL_MS = 24UL * 3600UL * 1000UL;

#endif // CONFIG_H
