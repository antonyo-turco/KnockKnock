#include "tinyml_training.h"
#include <math.h>
#include <string.h>
#include <float.h>
#include <Preferences.h>

// ─────────────────────────────────────────────────────────────────────────────
//  Novelty buffer  (static — not saved to NVS)
//
//  Stores normalised feature vectors collected during EXPLORING.
//  Novelty criterion: Euclidean distance to nearest existing entry > NOVELTY_THRESHOLD.
//  When full and a novel sample arrives, the existing entry most similar to the
//  new sample is replaced — this keeps the buffer maximally diverse.
// ─────────────────────────────────────────────────────────────────────────────

static float s_nov_buf[NOVELTY_BUFFER_SIZE][FEATURE_DIM];
static int   s_nov_count = 0;

// ─────────────────────────────────────────────────────────────────────────────
//  Internal helpers
// ─────────────────────────────────────────────────────────────────────────────

static float safe_sqrt(float x) { return (x > 0.0f) ? sqrtf(x) : 0.0f; }

// Welford online mean + variance.  Caller increments n before calling.
static void welford_update(float &mean, float &M2, uint32_t n, float x) {
    float delta = x - mean;
    mean += delta / (float)n;
    M2   += delta * (x - mean);
}

static void features_to_raw(const InferenceFeatures &f, float out[FEATURE_DIM]) {
    out[0]  = f.impact_score;
    out[1]  = f.m_p99;   out[2]  = f.x_p99;   out[3]  = f.y_p99;   out[4]  = f.z_p99;
    out[5]  = f.m_jerk_max; out[6]  = f.x_jerk_max; out[7]  = f.y_jerk_max; out[8]  = f.z_jerk_max;
    out[9]  = f.m_band_20_40; out[10] = f.x_band_20_40; out[11] = f.y_band_20_40; out[12] = f.z_band_20_40;
    out[13] = f.m_band_1_5;   out[14] = f.x_band_1_5;   out[15] = f.y_band_1_5;   out[16] = f.z_band_1_5;
    out[17] = f.m_band_5_20;  out[18] = f.x_band_5_20;  out[19] = f.y_band_5_20;  out[20] = f.z_band_5_20;
    out[21] = f.x_zcr;  out[22] = f.y_zcr;  out[23] = f.z_zcr;
    for (int i = 0; i < TOP7_COUNT; ++i) {
        out[24 + i] = f.x_top7_freq[i];
        out[31 + i] = f.y_top7_freq[i];
        out[38 + i] = f.z_top7_freq[i];
    }
}

static void normalise(const KMeansModel &model,
                      const float raw[FEATURE_DIM], float out[FEATURE_DIM]) {
    for (int d = 0; d < FEATURE_DIM; ++d) {
        float s = (model.norm_std[d] > 1e-9f) ? model.norm_std[d] : 1.0f;
        out[d] = (raw[d] - model.norm_mean[d]) / s;
    }
}

static float euclidean_dist(const float a[], const float b[], int dim) {
    float acc = 0.0f;
    for (int i = 0; i < dim; ++i) { float d = a[i] - b[i]; acc += d * d; }
    return safe_sqrt(acc);
}

static int nearest_centroid_idx(const KMeansModel &model, const float nv[]) {
    int   best = 0;
    float bd   = euclidean_dist(nv, model.centroids[0], FEATURE_DIM);
    for (int k = 1; k < KMEANS_K; ++k) {
        float d = euclidean_dist(nv, model.centroids[k], FEATURE_DIM);
        if (d < bd) { bd = d; best = k; }
    }
    return best;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Novelty buffer helpers
// ─────────────────────────────────────────────────────────────────────────────

// Returns index of the buffer entry nearest to query, and its distance.
static int novelty_nearest(const float query[FEATURE_DIM], float &out_dist) {
    int   best_idx  = 0;
    float best_dist = euclidean_dist(query, s_nov_buf[0], FEATURE_DIM);
    for (int i = 1; i < s_nov_count; ++i) {
        float d = euclidean_dist(query, s_nov_buf[i], FEATURE_DIM);
        if (d < best_dist) { best_dist = d; best_idx = i; }
    }
    out_dist = best_dist;
    return best_idx;
}

// Try to insert norm_fv into the novelty buffer.
// Returns true if the sample was added or replaced an existing entry.
static bool novelty_try_insert(const float norm_fv[FEATURE_DIM]) {
    // Always fill until we have at least KMEANS_K entries
    if (s_nov_count < KMEANS_K) {
        memcpy(s_nov_buf[s_nov_count++], norm_fv, sizeof(float) * FEATURE_DIM);
        return true;
    }

    float near_dist;
    int   near_idx = novelty_nearest(norm_fv, near_dist);

    if (near_dist < NOVELTY_THRESHOLD) {
        // Sample is too similar to an existing entry — discard
        return false;
    }

    if (s_nov_count < NOVELTY_BUFFER_SIZE) {
        // Buffer not full — just add
        memcpy(s_nov_buf[s_nov_count++], norm_fv, sizeof(float) * FEATURE_DIM);
    } else {
        // Buffer full — replace the entry most similar to the new sample.
        // This consolidates the densest region and keeps maximal diversity.
        memcpy(s_nov_buf[near_idx], norm_fv, sizeof(float) * FEATURE_DIM);
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  K-means++ seeding  (deterministic furthest-point variant)
//  Operates on s_nov_buf[0..s_nov_count-1].
// ─────────────────────────────────────────────────────────────────────────────

static void kmeans_pp_seed(KMeansModel &model) {
    // C0 — first buffer entry
    memcpy(model.centroids[0], s_nov_buf[0], sizeof(float) * FEATURE_DIM);
    model.centroid_counts[0] = 1;

    for (int k = 1; k < KMEANS_K; ++k) {
        float best_min_dist = -1.0f;
        int   best_idx      = 0;
        for (int i = 0; i < s_nov_count; ++i) {
            float min_d = FLT_MAX;
            for (int j = 0; j < k; ++j) {
                float d = euclidean_dist(s_nov_buf[i], model.centroids[j], FEATURE_DIM);
                if (d < min_d) min_d = d;
            }
            if (min_d > best_min_dist) { best_min_dist = min_d; best_idx = i; }
        }
        memcpy(model.centroids[k], s_nov_buf[best_idx], sizeof(float) * FEATURE_DIM);
        model.centroid_counts[k] = 1;
    }

    model.initialised = true;
    Serial.printf("[EXPLORING] k++ seeding done on %d novel samples — %d centroids placed.\n",
                  s_nov_count, KMEANS_K);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public API — init
// ─────────────────────────────────────────────────────────────────────────────

void training_init(KMeansModel &model) {
    memset(&model, 0, sizeof(KMeansModel));
    for (int d = 0; d < FEATURE_DIM; ++d) model.norm_std[d] = 1.0f;
    s_nov_count = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Phase 1: EXPLORING
// ─────────────────────────────────────────────────────────────────────────────

void exploring_update(KMeansModel &model, const InferenceFeatures &features) {
    float raw[FEATURE_DIM];
    features_to_raw(features, raw);

    // Update live normalisation
    model.norm_n++;
    model.total_samples++;
    for (int d = 0; d < FEATURE_DIM; ++d) {
        welford_update(model.norm_mean[d], model.norm_M2[d], model.norm_n, raw[d]);
        if (model.norm_n > 1) {
            float s = safe_sqrt(model.norm_M2[d] / (float)(model.norm_n - 1));
            model.norm_std[d] = (s > 1e-9f) ? s : 1.0f;
        }
    }

    // Normalise and try to insert into novelty buffer
    // Skip first 30 samples — Welford needs a warm-up period for stable std
    if (model.norm_n >= 30) {
        float norm_fv[FEATURE_DIM];
        normalise(model, raw, norm_fv);
        novelty_try_insert(norm_fv);
    }
}

bool exploring_finalize(KMeansModel &model) {
    Serial.printf("[EXPLORING] Finalising — %d novel samples in buffer.\n", s_nov_count);

    if (s_nov_count < MIN_NOVELTY_FOR_SEEDING) {
        Serial.printf("[EXPLORING] WARNING: only %d samples (min %d). "
                      "Model quality may be poor.\n",
                      s_nov_count, MIN_NOVELTY_FOR_SEEDING);
    }

    // Freeze normalisation
    for (int d = 0; d < FEATURE_DIM; ++d) {
        if (model.norm_n > 1) {
            float s = safe_sqrt(model.norm_M2[d] / (float)(model.norm_n - 1));
            model.norm_std[d] = (s > 1e-9f) ? s : 1.0f;
        }
    }

    // Run k++ on novelty buffer
    kmeans_pp_seed(model);
    return s_nov_count >= MIN_NOVELTY_FOR_SEEDING;
}

uint8_t exploring_novelty_pct() {
    return (uint8_t)((s_nov_count * 100) / NOVELTY_BUFFER_SIZE);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Phase 2: TRAINING
// ─────────────────────────────────────────────────────────────────────────────

void training_update(KMeansModel &model, const InferenceFeatures &features) {
    float raw[FEATURE_DIM];
    features_to_raw(features, raw);
    model.total_samples++;

    // Use frozen normalisation — do NOT update Welford here
    float norm_fv[FEATURE_DIM];
    normalise(model, raw, norm_fv);

    // Online centroid update: c += (x - c) / n
    int k = nearest_centroid_idx(model, norm_fv);
    model.centroid_counts[k]++;
    uint32_t cnt = model.centroid_counts[k];
    for (int d = 0; d < FEATURE_DIM; ++d) {
        model.centroids[k][d] += (norm_fv[d] - model.centroids[k][d]) / (float)cnt;
    }

    // Distance statistics for threshold computation
    float dist = euclidean_dist(norm_fv, model.centroids[k], FEATURE_DIM);
    model.dist_n[k]++;
    welford_update(model.dist_mean[k], model.dist_M2[k], model.dist_n[k], dist);
    if (dist > model.dist_max[k]) model.dist_max[k] = dist;
}

void training_finalize(KMeansModel &model) {
    Serial.println("[TRAINING] Finalising model...");

    // threshold = max(mean + 3σ,  dist_max * 1.1,  MIN_THRESHOLD)
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
                            float *out_distance,
                            int *out_cluster) {
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
    if (out_cluster)  *out_cluster  = k;        // ← riga aggiunta
    return dist <= model.dist_threshold[k];
}

// ─────────────────────────────────────────────────────────────────────────────
//  Debug print
// ─────────────────────────────────────────────────────────────────────────────

static const char* FEAT_NAMES[FEATURE_DIM] = {
    "impact_score",
    "m_p99",   "x_p99",   "y_p99",   "z_p99",
    "m_jerk",  "x_jerk",  "y_jerk",  "z_jerk",
    "m_b2040", "x_b2040", "y_b2040", "z_b2040",
    "m_b1_5",  "x_b1_5",  "y_b1_5",  "z_b1_5",
    "m_b5_20", "x_b5_20", "y_b5_20", "z_b5_20",
    "x_zcr",   "y_zcr",   "z_zcr",
    "x_f0","x_f1","x_f2","x_f3","x_f4","x_f5","x_f6",
    "y_f0","y_f1","y_f2","y_f3","y_f4","y_f5","y_f6",
    "z_f0","z_f1","z_f2","z_f3","z_f4","z_f5","z_f6"
};

void training_print_model(const KMeansModel &model) {
    Serial.println(F("\n╔══════════════════════════════════════════════╗"));
    Serial.println(F("║     K-Means++ Model  (45-dim)                ║"));
    Serial.println(F("╚══════════════════════════════════════════════╝"));
    Serial.printf("  K=%d  feat_dim=%d  total_samples=%lu\n",
                  KMEANS_K, FEATURE_DIM, (unsigned long)model.total_samples);

    Serial.println(F("\n  ── Normalisation ─────────────────────────────"));
    for (int d = 0; d < FEATURE_DIM; ++d) {
        Serial.printf("  [%2d] %-12s  mean=%10.4f  std=%9.4f\n",
                      d, FEAT_NAMES[d], model.norm_mean[d], model.norm_std[d]);
    }

    Serial.println(F("\n  ── Centroids ─────────────────────────────────"));
    for (int k = 0; k < KMEANS_K; ++k) {
        Serial.printf("  C%d  n=%-6lu  dist_mean=%.4f  dist_max=%.4f  thr=%.4f\n",
                      k, (unsigned long)model.centroid_counts[k],
                      model.dist_mean[k], model.dist_max[k], model.dist_threshold[k]);
    }
    Serial.println(F("══════════════════════════════════════════════\n"));
}
