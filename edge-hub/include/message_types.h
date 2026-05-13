#pragma once
#include <stdint.h>

#define PAYLOAD_VERSION   1
#define IF_N_FEATURES     9

/* event_type values */
#define EVENT_TELEMETRY   0
#define EVENT_ALARM       1

typedef struct __attribute__((packed)) {
    uint8_t  payload_version;   /* always PAYLOAD_VERSION              */
    uint8_t  node_id;           /* 0-255, set in config.h              */
    uint8_t  event_type;        /* EVENT_TELEMETRY or EVENT_ALARM      */
    uint8_t  _pad;              /* explicit padding — do not remove    */
    uint32_t timestamp_ms;      /* ms since boot                       */
    float    band_energy[5];    /* normalized, sum == 1.0f             */
    float    spectral_centroid; /* Hz                                  */
    float    spectral_entropy;  /* normalized 0-1                      */
    float    crest_factor;      /* dimensionless                       */
    float    rms_amplitude;     /* normalized 0-1                      */
    float    anomaly_score;     /* 0-1; populated after Stage 3        */
} knockknock_payload_t;         /* total: 4 + 4 + 20 + 4*4 = 44 bytes */

/* Sanity check — will fail at compile time if struct drifts */
_Static_assert(sizeof(knockknock_payload_t) == 44,
               "knockknock_payload_t size mismatch — check padding");
