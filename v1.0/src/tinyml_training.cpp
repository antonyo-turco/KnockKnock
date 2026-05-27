#include "tinyml_training.h"
#include <math.h>
#include <string.h>
#include <float.h>
#include <Preferences.h>
#include "config.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Seeding buffer  (static — lives only during training, never saved to NVS)
// ─────────────────────────────────────────────────────────────────────────────

static float    s_seed_buf[SEED_BUFFER_SIZE][FEATURE_DIM];
static uint32_t s_seed_count   = 0;
static bool     s_seeding_done = false;

// ─────────────────────────────────────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

static float safe_sqrt(float x) {
    return (x > 0.0f) ? sqrtf(x) : 0.0f;
}

// Welford single-pass online mean + variance.
// Caller must increment n BEFORE calling.
static void welford_update(float &mean, float &M2, uint32_t n, float x) {
    float delta = x - mean;
    mean       += delta / (float)n;
    M2         += delta * (x - mean);
}

// Map InferenceFeatures → raw float vector (13 dimensions, no spectral flux)
static void features_to_raw(const InferenceFeatures &f, float out[FEATURE_DIM]) {
    out[0]  = f.impact_score;
    out[1]  = f.m_p99;
    out[2]  = f.x_p99;
    out[3]  = f.y_p99;
    out[4]  = f.z_p99;
    out[5]  = f.m_jerk_max;
    out[6]  = f.x_jerk_max;
    out[7]  = f.y_jerk_max;
    out[8]  = f.z_jerk_max;
    out[9]  = f.m_band_20_40;
    out[10] = f.x_band_20_40;
    out[11] = f.y_band_20_40;
    out[12] = f.z_band_20_40;
}

static void normalise(const KMeansModel &model,
                      const float raw[FEATURE_DIM],
                      float out[FEATURE_DIM]) {
    for (int d = 0; d < FEATURE_DIM; ++d) {
        float s = (model.norm_std[d] > 1e-9f) ? model.norm_std[d] : 1.0f;
        out[d] = (raw[d] - model.norm_mean[d]) / s;
    }
}

static float euclidean_dist(const float a[], const float b[], int dim) {
    float acc = 0.0f;
    for (int i = 0; i < dim; ++i) {
        float d = a[i] - b[i];
        acc += d * d;
    }
    return safe_sqrt(acc);
}

static int nearest_centroid_idx(const KMeansModel &model, const float norm_fv[]) {
    int   best      = 0;
    float best_dist = euclidean_dist(norm_fv, model.centroids[0], FEATURE_DIM);
    for (int k = 1; k < KMEANS_K; ++k) {
        float d = euclidean_dist(norm_fv, model.centroids[k], FEATURE_DIM);
        if (d < best_dist) { best_dist = d; best = k; }
    }
    return best;
}

// ─────────────────────────────────────────────────────────────────────────────
//  K-means++ seeding  (deterministic furthest-point variant)
//
//  C0 = first buffered sample.
//  Ck = sample with maximum min-distance to already-chosen centroids.
//  No RNG needed — fully reproducible on embedded hardware.
// ─────────────────────────────────────────────────────────────────────────────

static void kmeans_pp_seed(KMeansModel &model) {
    memcpy(model.centroids[0], s_seed_buf[0], sizeof(float) * FEATURE_DIM);
    model.centroid_counts[0] = 1;

    for (int k = 1; k < KMEANS_K; ++k) {
        float best_min_dist = -1.0f;
        int   best_idx      = 0;

        for (int i = 0; i < (int)s_seed_count; ++i) {
            // Min distance from s_seed_buf[i] to any centroid chosen so far
            float min_d = FLT_MAX;
            for (int j = 0; j < k; ++j) {
                float d = euclidean_dist(s_seed_buf[i], model.centroids[j], FEATURE_DIM);
                if (d < min_d) min_d = d;
            }
            if (min_d > best_min_dist) {
                best_min_dist = min_d;
                best_idx      = i;
            }
        }

        memcpy(model.centroids[k], s_seed_buf[best_idx], sizeof(float) * FEATURE_DIM);
        model.centroid_counts[k] = 1;
    }

    model.initialised = true;
    Serial.printf("[TRAINING] k++ seeding complete on %d samples — %d centroids placed.\n",
                  (int)s_seed_count, KMEANS_K);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────────────────────

void training_init(KMeansModel &model) {
    memset(&model, 0, sizeof(KMeansModel));
    for (int d = 0; d < FEATURE_DIM; ++d) {
        model.norm_std[d] = 1.0f;
    }
    s_seed_count   = 0;
    s_seeding_done = false;
}

void training_update(KMeansModel &model, const InferenceFeatures &features) {
    float raw[FEATURE_DIM];
    features_to_raw(features, raw);

    // ── 1. Update normalisation stats ────────────────────────────────────────
    model.norm_n++;
    for (int d = 0; d < FEATURE_DIM; ++d) {
        welford_update(model.norm_mean[d], model.norm_M2[d], model.norm_n, raw[d]);
        if (model.norm_n > 1) {
            float s = safe_sqrt(model.norm_M2[d] / (float)(model.norm_n - 1));
            model.norm_std[d] = (s > 1e-9f) ? s : 1.0f;
        }
    }
    model.total_samples++;

    // ── 2. Normalise ─────────────────────────────────────────────────────────
    float norm_fv[FEATURE_DIM];
    normalise(model, raw, norm_fv);

    // ── 3. Fill seeding buffer → trigger k++ when full ───────────────────────
    if (!s_seeding_done) {
        if (s_seed_count < (uint32_t)SEED_BUFFER_SIZE) {
            memcpy(s_seed_buf[s_seed_count], norm_fv, sizeof(float) * FEATURE_DIM);
            s_seed_count++;
        }
        if (s_seed_count == (uint32_t)SEED_BUFFER_SIZE) {
            kmeans_pp_seed(model);
            s_seeding_done = true;
        }
        return;
    }

    // ── 4. Online centroid update  c ← c + (x − c) / n ──────────────────────
    int k = nearest_centroid_idx(model, norm_fv);
    model.centroid_counts[k]++;
    uint32_t cnt = model.centroid_counts[k];
    for (int d = 0; d < FEATURE_DIM; ++d) {
        model.centroids[k][d] += (norm_fv[d] - model.centroids[k][d]) / (float)cnt;
    }

    // ── 5. Distance stats for threshold ──────────────────────────────────────
    float dist = euclidean_dist(norm_fv, model.centroids[k], FEATURE_DIM);

    model.dist_n[k]++;
    welford_update(model.dist_mean[k], model.dist_M2[k], model.dist_n[k], dist);

    if (dist > model.dist_max[k]) {
        model.dist_max[k] = dist;
    }
}

void training_finalize(KMeansModel &model) {
    Serial.println("[TRAINING] Finalising model...");

    // Freeze normalisation std
    for (int d = 0; d < FEATURE_DIM; ++d) {
        if (model.norm_n > 1) {
            float s = safe_sqrt(model.norm_M2[d] / (float)(model.norm_n - 1));
            model.norm_std[d] = (s > 1e-9f) ? s : 1.0f;
        }
    }

    // Per-cluster threshold:
    //   thr_stat = mean_dist + SIGMA_MULT * sigma   ← covers typical spread
    //   thr_max  = dist_max  * MAX_DIST_MARGIN      ← covers every training sample
    //   threshold = max(thr_stat, thr_max, MIN_THRESHOLD)
    for (int k = 0; k < KMEANS_K; ++k) {
        float sigma    = (model.dist_n[k] > 1)
                         ? safe_sqrt(model.dist_M2[k] / (float)(model.dist_n[k] - 1))
                         : 0.0f;
        float thr_stat = model.dist_mean[k] + SIGMA_MULT * sigma;
        float thr_max  = model.dist_max[k]  * MAX_DIST_MARGIN;
        float thr      = fmaxf(thr_stat, thr_max);
        model.dist_threshold[k] = fmaxf(thr, MIN_THRESHOLD);
    }

    model.finalised = true;
    training_print_model(model);
}

// ─────────────────────────────────────────────────────────────────────────────
//  NVS persistence
// ─────────────────────────────────────────────────────────────────────────────

static const char NVS_NS[]  = "antitheft";
static const char NVS_KEY[] = "kmeans";

bool training_save(const KMeansModel &model) {
    if (!model.finalised) {
        Serial.println("[NVS] Cannot save: model not finalised.");
        return false;
    }
    Preferences prefs;
    if (!prefs.begin(NVS_NS, false)) {
        Serial.println("[NVS] ERROR: cannot open namespace.");
        return false;
    }
    size_t w = prefs.putBytes(NVS_KEY, &model, sizeof(KMeansModel));
    prefs.end();
    bool ok = (w == sizeof(KMeansModel));
    Serial.printf("[NVS] %s (%u/%u bytes).\n",
                  ok ? "Saved OK" : "SAVE FAILED",
                  (unsigned)w, (unsigned)sizeof(KMeansModel));
    return ok;
}

bool training_load(KMeansModel &model) {
    Preferences prefs;
    if (!prefs.begin(NVS_NS, true)) {
        Serial.println("[NVS] Cannot open namespace.");
        return false;
    }
    size_t stored = prefs.getBytesLength(NVS_KEY);
    bool ok = false;
    if (stored == sizeof(KMeansModel)) {
        prefs.getBytes(NVS_KEY, &model, sizeof(KMeansModel));
        ok = model.finalised;
    }
    prefs.end();
    Serial.printf("[NVS] %s (stored=%u, expected=%u).\n",
                  ok ? "Model loaded" : "No valid model",
                  (unsigned)stored, (unsigned)sizeof(KMeansModel));
    return ok;
}

void training_erase_nvs() {
    Preferences prefs;
    if (prefs.begin(NVS_NS, false)) {
        prefs.remove(NVS_KEY);
        prefs.end();
        Serial.println("[NVS] Model erased.");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Inference
// ─────────────────────────────────────────────────────────────────────────────

bool training_is_baseline(const KMeansModel &model,
                           const InferenceFeatures &features,
                           float *out_distance) {
    if (!model.finalised) {
        if (out_distance) *out_distance = 0.0f;
        return true;
    }

    float raw[FEATURE_DIM];
    features_to_raw(features, raw);

    float norm_fv[FEATURE_DIM];
    normalise(model, raw, norm_fv);

    int   k    = nearest_centroid_idx(model, norm_fv);
    float dist = euclidean_dist(norm_fv, model.centroids[k], FEATURE_DIM);

    if (out_distance) *out_distance = dist;
    return dist <= model.dist_threshold[k];
}

// ─────────────────────────────────────────────────────────────────────────────
//  Debug print
// ─────────────────────────────────────────────────────────────────────────────

static const char* FEAT_NAMES[FEATURE_DIM] = {
    "impact_score",
    "m_p99",  "x_p99",  "y_p99",  "z_p99",
    "m_jerk", "x_jerk", "y_jerk", "z_jerk",
    "m_band", "x_band", "y_band", "z_band"
};

void training_print_model(const KMeansModel &model) {
    Serial.println(F("\n╔══════════════════════════════════════════════╗"));
    Serial.println(F("║     K-Means++ Model  (13-dim)                ║"));
    Serial.println(F("╚══════════════════════════════════════════════╝"));
    Serial.printf("  K=%d  feat_dim=%d  total_samples=%lu\n",
                  KMEANS_K, FEATURE_DIM, (unsigned long)model.total_samples);

    Serial.println(F("\n  ── Normalisation ──────────────────────────────"));
    for (int d = 0; d < FEATURE_DIM; ++d) {
        Serial.printf("  [%2d] %-14s  mean=%10.4f  std=%10.4f\n",
                      d, FEAT_NAMES[d], model.norm_mean[d], model.norm_std[d]);
    }

    Serial.println(F("\n  ── Centroids ──────────────────────────────────"));
    for (int k = 0; k < KMEANS_K; ++k) {
        Serial.printf("  C%d  n=%-6lu  dist_mean=%.4f  dist_max=%.4f  thr=%.4f\n",
                      k,
                      (unsigned long)model.centroid_counts[k],
                      model.dist_mean[k],
                      model.dist_max[k],
                      model.dist_threshold[k]);
    }
    Serial.println(F("══════════════════════════════════════════════\n"));
}
