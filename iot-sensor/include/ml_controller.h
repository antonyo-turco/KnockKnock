
#include "tinyml_baseline_model.h"
#include "tinyml_training.h"
#include "feature_extraction.h" 





// ––––––––––––––––––––––––––––––––––––––––––––––––––––––––
//Global variables
// ––––––––––––––––––––––––––––––––––––––––––––––––––––––––
struct Sample { int16_t x, y, z; };
enum class SystemMode : uint8_t { EXPLORING, TRAINING, INFERENCE };
static Sample        g_window[SAMPLE_COUNT];
static int           g_window_index    = 0;
static uint32_t      g_window_seq      = 0;
static unsigned long g_last_sample_ms  = 0;
static unsigned long g_phase_start_ms  = 0;

static KMeansModel   g_model;
static SystemMode    g_mode;

// Current time features — updated at every NTP sync and at boot
static float         g_time_sin       = 0.0f;
static float         g_time_cos       = 1.0f;  // default: midnight
static bool          g_time_valid     = false;




// ─────────────────────────────────────────────────────────────────────────────
//  Phase transitions
// ─────────────────────────────────────────────────────────────────────────────

static void transition_to_training() {


    bool ok = exploring_finalize(g_model);
    if (!ok) {
        printf("[SYSTEM] WARNING: novelty buffer sparse — model may need longer exploring.");
    }

    g_phase_start_ms = millis();
    g_mode = SystemMode::TRAINING;
    printf("[SYSTEM] EXPLORING complete -> TRAINING for %lu s.\n",
           (unsigned long)TRAINING_DURATION_MS / 1000UL);
    delay(1500);
}

static void transition_to_inference() {


    training_finalize(g_model);
    training_save(g_model);

    g_mode = SystemMode::INFERENCE;
    Serial.println("[SYSTEM] TRAINING complete -> INFERENCE mode.");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Window processing
// ─────────────────────────────────────────────────────────────────────────────

static void process_window() {
    static int16_t xb[SAMPLE_COUNT], yb[SAMPLE_COUNT], zb[SAMPLE_COUNT];
    for (int i = 0; i < SAMPLE_COUNT; ++i) {
        xb[i] = g_window[i].x;
        yb[i] = g_window[i].y;
        zb[i] = g_window[i].z;
    }

    InferenceFeatures feat = compute_features(
        xb, yb, zb, SAMPLE_COUNT, SAMPLING_RATE_HZ,
        g_time_sin, g_time_cos);

    switch (g_mode) {

        // ── EXPLORING ────────────────────────────────────────────────────────
        case SystemMode::EXPLORING:
            exploring_update(g_model, feat);


            if (g_window_seq % 150 == 0)
                Serial.printf("[EXPLORE] win=%lu  n=%lu  novel=%u%%\n",
                              (unsigned long)g_window_seq,
                              (unsigned long)g_model.total_samples,
                              exploring_novelty_pct());

            if (millis() - g_phase_start_ms >= EXPLORING_DURATION_MS)
                transition_to_training();
            break;

        // ── TRAINING ─────────────────────────────────────────────────────────
        case SystemMode::TRAINING:
            training_update(g_model, feat);


            if (g_window_seq % 150 == 0) {
                uint32_t elapsed = millis() - g_phase_start_ms;
                Serial.printf("[TRAIN] win=%lu  n=%lu  %lu%%\n",
                              (unsigned long)g_window_seq,
                              (unsigned long)g_model.total_samples,
                              elapsed * 100 / TRAINING_DURATION_MS);
            }

            if (millis() - g_phase_start_ms >= TRAINING_DURATION_MS)
                transition_to_inference();
            break;

        // ── INFERENCE ────────────────────────────────────────────────────────
        case SystemMode::INFERENCE: {
            float dist    = 0.0f;
            int   cluster = -1;
            bool  ok      = training_is_baseline(g_model, feat, &dist, &cluster);

            Serial.printf("[INF] win=%lu  impact=%.4f  m_p99=%.2f  dist=%.4f  C%d  %s\n",
                        (unsigned long)g_window_seq,
                        feat.impact_score, feat.m_p99, dist,
                        cluster,
                        ok ? "BASELINE" : "*** DEVIATION ***");


            Serial.printf("[INF] win=%lu  impact=%.4f  m_p99=%.2f  dist=%.4f  %s\n",
                          (unsigned long)g_window_seq,
                          feat.impact_score, feat.m_p99, dist,
                          ok ? "BASELINE" : "*** DEVIATION ***");
            break;
        }
    }
}