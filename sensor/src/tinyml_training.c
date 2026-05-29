/*
 * tinyml_training.c
 *
 * Classificatore k-means one-class per anomaly detection su ESP32-C3 (ESP-IDF).
 * Il modello impara la distribuzione NORMALE del segnale durante il training
 * e durante l'inferenza segnala tutto ciò che se ne discosta.
 *
 * Tre fasi sequenziali:
 *
 *  1. EXPLORING  – raccoglie le statistiche di normalizzazione (media/std
 *                  di ogni feature via Welford) e popola un buffer di campioni
 *                  "novel" (diversi tra loro) per il seeding k++.
 *                  Al termine, congela la normalizzazione e piazza i K centroidi.
 *
 *  2. TRAINING   – aggiorna iterativamente i centroidi con il nuovo dato
 *                  (k-means online) e raccoglie le statistiche di distanza
 *                  (mean/std/max) per ogni centroide.
 *
 *  3. INFERENCE  – normalizza il vettore di feature, trova il centroide più
 *                  vicino e confronta la distanza con la soglia di quel centroide.
 *                  dist > threshold → anomalia.
 *
 * Persistenza: il modello viene salvato in NVS (namespace "antitheft", chiave
 * "kmeans") come blob grezzo. La compatibilità è verificata tramite sizeof().
 */

#include "tinyml_training.h"
#include "esp_log.h"
#include "feature_extraction.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <float.h>
#include <math.h>
#include <string.h>

static const char *TAG_ML = "TINYML";

/* Buffer globale dei campioni "novel" raccolti durante Exploring.
 * Usato dal seeding k++ per posizionare i centroidi iniziali
 * in punti massimamente distanti tra loro nello spazio delle feature. */
static float s_nov_buf[NOVELTY_BUFFER_SIZE][FEATURE_DIM];
static int s_nov_count = 0;

static float safe_sqrt(float x) { return (x > 0.0f) ? sqrtf(x) : 0.0f; }

/*
 * Algoritmo di Welford per calcolo incrementale di media e varianza.
 * Più stabile numericamente della formula (Σx²/n - μ²) per valori grandi.
 * M2 accumula la somma delle deviazioni al quadrato dalla media corrente;
 * la varianza campionaria è M2/(n-1).
 */
static void welford_update(float *mean, float *M2, uint32_t n, float x) {
  float delta = x - *mean;
  *mean += delta / (float)n;   /* aggiorna media in-place */
  *M2 += delta * (x - *mean); /* aggiorna M2 con la deviazione AGGIORNATA */
}

/*
 * Serializza il struct InferenceFeatures in un array float[FEATURE_DIM].
 * Mappa fissa — deve restare identica tra training e inferenza.
 * Indici: [0] impact, [1-4] p99, [5-8] jerk, [9-12] b20_40,
 *         [13-16] b40_100, [17-20] b1_5, [21-24] b5_20,
 *         [25-27] zcr, [28-34] x_top7, [35-41] y_top7, [42-48] z_top7,
 *         [49] time_sin, [50] time_cos.
 */
static void features_to_raw(const InferenceFeatures *f,
                            float out[FEATURE_DIM]) {
  out[0] = f->impact_score;
  out[1] = f->m_p99;        out[2] = f->x_p99;        out[3] = f->y_p99;        out[4] = f->z_p99;
  out[5] = f->m_jerk_max;   out[6] = f->x_jerk_max;   out[7] = f->y_jerk_max;   out[8] = f->z_jerk_max;
  out[9]  = f->m_band_20_40;  out[10] = f->x_band_20_40;  out[11] = f->y_band_20_40;  out[12] = f->z_band_20_40;
  out[13] = f->m_band_40_100; out[14] = f->x_band_40_100; out[15] = f->y_band_40_100; out[16] = f->z_band_40_100;
  out[17] = f->m_band_1_5;    out[18] = f->x_band_1_5;    out[19] = f->y_band_1_5;    out[20] = f->z_band_1_5;
  out[21] = f->m_band_5_20;   out[22] = f->x_band_5_20;   out[23] = f->y_band_5_20;   out[24] = f->z_band_5_20;
  out[25] = f->x_zcr; out[26] = f->y_zcr; out[27] = f->z_zcr;
  for (int i = 0; i < TOP7_COUNT; ++i) {
    out[28 + i] = f->x_top7_freq[i];
    out[35 + i] = f->y_top7_freq[i];
    out[42 + i] = f->z_top7_freq[i];
  }
  out[49] = f->time_sin;
  out[50] = f->time_cos;
}

/*
 * Z-score normalization: porta ogni feature a media≈0, std≈1.
 * Indispensabile per la distanza euclidea in k-means: senza normalizzazione
 * feature con scale diverse (es. band power ~24000 vs ZCR ~0.004)
 * dominerebbero completamente il calcolo della distanza.
 * La normalizzazione è congelata al termine di Exploring e mai più modificata.
 */
static void normalise(const KMeansModel *model, const float raw[FEATURE_DIM],
                      float out[FEATURE_DIM]) {
  for (int d = 0; d < FEATURE_DIM; ++d) {
    float s = (model->norm_std[d] > 1e-9f) ? model->norm_std[d] : 1.0f;
    out[d] = (raw[d] - model->norm_mean[d]) / s;
  }
}

/*
 * Distanza euclidea tra due vettori in FEATURE_DIM dimensioni.
 * Metrica centrale del classificatore: misura quanto un campione
 * si discosta dal centroide più vicino nello spazio normalizzato.
 */
static float euclidean_dist(const float a[], const float b[], int dim) {
  float acc = 0.0f;
  for (int i = 0; i < dim; ++i) {
    float d = a[i] - b[i];
    acc += d * d;
  }
  return safe_sqrt(acc);
}

/* Restituisce l'indice del centroide con distanza minima dal punto nv. */
static int nearest_centroid_idx(const KMeansModel *model, const float nv[]) {
  int best = 0;
  float bd = euclidean_dist(nv, model->centroids[0], FEATURE_DIM);
  for (int k = 1; k < KMEANS_K; ++k) {
    float d = euclidean_dist(nv, model->centroids[k], FEATURE_DIM);
    if (d < bd) { bd = d; best = k; }
  }
  return best;
}

/* ─── Novelty buffer ─────────────────────────────────────────────────────── */

/*
 * Trova il campione nel buffer novel più vicino a query.
 * Usato da novelty_try_insert per decidere se il nuovo punto è abbastanza
 * diverso da quelli già memorizzati.
 */
static int novelty_nearest(const float query[], float *out_dist) {
  int best_idx = 0;
  float best_dist = euclidean_dist(query, s_nov_buf[0], FEATURE_DIM);
  for (int i = 1; i < s_nov_count; ++i) {
    float d = euclidean_dist(query, s_nov_buf[i], FEATURE_DIM);
    if (d < best_dist) { best_dist = d; best_idx = i; }
  }
  *out_dist = best_dist;
  return best_idx;
}

/*
 * Inserisce un campione nel buffer novel solo se è abbastanza diverso
 * da tutti quelli già presenti (distanza > NOVELTY_THRESHOLD).
 * Obiettivo: raccogliere una rappresentazione DIVERSIFICATA della distribuzione
 * normale, non tanti campioni simili dello stesso stato.
 * Quando il buffer è pieno, il nuovo campione rimpiazza il più vicino
 * (rinnova i "bordi" della distribuzione con dati più recenti).
 */
static bool novelty_try_insert(const float norm_fv[]) {
  if (s_nov_count < KMEANS_K) {
    /* Primi K campioni accettati incondizionatamente (necessari per k++ seeding) */
    memcpy(s_nov_buf[s_nov_count++], norm_fv, sizeof(float) * FEATURE_DIM);
    return true;
  }
  float near_dist;
  int near_idx = novelty_nearest(norm_fv, &near_dist);
  if (near_dist < NOVELTY_THRESHOLD) return false; /* troppo simile a un campione esistente */
  if (s_nov_count < NOVELTY_BUFFER_SIZE) {
    memcpy(s_nov_buf[s_nov_count++], norm_fv, sizeof(float) * FEATURE_DIM);
  } else {
    memcpy(s_nov_buf[near_idx], norm_fv, sizeof(float) * FEATURE_DIM); /* rimpiazza il più vicino */
  }
  return true;
}

/*
 * K-Means++ seeding: piazza i K centroidi iniziali massimizzando la distanza
 * reciproca tra di loro, usando i campioni novel come pool.
 *
 * Algoritmo:
 *   1. Il primo centroide è il primo campione del buffer.
 *   2. Per ogni centroide successivo k: scegli il campione del buffer
 *      la cui distanza MINIMA dai centroidi già piazzati è MASSIMA.
 *      (max-of-min → il punto più "isolato" rispetto ai centroidi esistenti)
 *
 * Questo garantisce che i centroidi coprano regioni diverse dello spazio
 * delle feature, riducendo il rischio di k-means degenere con centroidi
 * sovrapposti (problema del k-means con init casuale).
 */
static void kmeans_pp_seed(KMeansModel *model) {
  memcpy(model->centroids[0], s_nov_buf[0], sizeof(float) * FEATURE_DIM);
  model->centroid_counts[0] = 1;
  for (int k = 1; k < KMEANS_K; ++k) {
    float best_min_dist = -1.0f;
    int best_idx = 0;
    for (int i = 0; i < s_nov_count; ++i) {
      /* Distanza minima dal punto i a tutti i centroidi già piazzati */
      float min_d = FLT_MAX;
      for (int j = 0; j < k; ++j) {
        float d = euclidean_dist(s_nov_buf[i], model->centroids[j], FEATURE_DIM);
        if (d < min_d) min_d = d;
      }
      if (min_d > best_min_dist) { best_min_dist = min_d; best_idx = i; }
    }
    memcpy(model->centroids[k], s_nov_buf[best_idx], sizeof(float) * FEATURE_DIM);
    model->centroid_counts[k] = 1;
  }
  model->initialised = true;
  ESP_LOGI(TAG_ML, "[EXPLORING] k++ seeding done on %d novel samples — %d centroids placed.",
           s_nov_count, KMEANS_K);
}

/* ─── API pubblica ─────────────────────────────────────────────────────────── */

void training_init(KMeansModel *model) {
  memset(model, 0, sizeof(KMeansModel));
  for (int d = 0; d < FEATURE_DIM; ++d)
    model->norm_std[d] = 1.0f; /* evita divisione per 0 prima dei primi campioni */
  s_nov_count = 0;
}

/*
 * FASE EXPLORING — chiamata per ogni finestra.
 * Aggiorna le statistiche di normalizzazione (Welford per ogni feature)
 * e tenta di inserire il campione normalizzato nel buffer novel.
 * I primi 5 campioni vengono saltati per il novelty buffer:
 * la normalizzazione è ancora instabile con meno di 5 punti.
 */
void exploring_update(KMeansModel *model, const InferenceFeatures *features) {
  float raw[FEATURE_DIM];
  features_to_raw(features, raw);

  model->norm_n++;
  model->total_samples++;
  for (int d = 0; d < FEATURE_DIM; ++d) {
    welford_update(&(model->norm_mean[d]), &(model->norm_M2[d]), model->norm_n, raw[d]);
    if (model->norm_n > 1) {
      float s = safe_sqrt(model->norm_M2[d] / (float)(model->norm_n - 1));
      model->norm_std[d] = (s > 1e-9f) ? s : 1.0f;
    }
  }

  if (model->norm_n >= 5) {
    float norm_fv[FEATURE_DIM];
    normalise(model, raw, norm_fv);
    novelty_try_insert(norm_fv);
  }
}

/*
 * Fine della fase EXPLORING.
 * 1. Congela definitivamente le statistiche di normalizzazione.
 * 2. Esegue il k++ seeding sui campioni novel.
 * 3. Calcola suggested_threshold_mg dal jerk osservato durante Exploring
 *    (usato per calibrare la soglia hardware dell'ADXL362).
 *
 * Nota: questa versione usa mean(m_p99) + 3σ per la soglia hardware.
 * La versione sensor-standalone usa mean(m_jerk) + 2σ.
 * Con il training lungo (24h) questa versione è più accurata perché
 * m_p99 ha più campioni per stimare la distribuzione.
 */
bool exploring_finalize(KMeansModel *model) {
  ESP_LOGI(TAG_ML, "[EXPLORING] Finalising — %d novel samples in buffer.", s_nov_count);
  if (s_nov_count < MIN_NOVELTY_FOR_SEEDING) {
    ESP_LOGW(TAG_ML, "[EXPLORING] WARNING: only %d samples (min %d).",
             s_nov_count, MIN_NOVELTY_FOR_SEEDING);
  }
  /* Ricalcola std finale con tutti i dati accumulati */
  for (int d = 0; d < FEATURE_DIM; ++d) {
    if (model->norm_n > 1) {
      float s = safe_sqrt(model->norm_M2[d] / (float)(model->norm_n - 1));
      model->norm_std[d] = (s > 1e-9f) ? s : 1.0f;
    }
  }
  kmeans_pp_seed(model);

  /* mean(m_p99) + 3σ: copre il 99.7% dell'ampiezza normale.
   * m_p99 (indice 1) include la componente DC della gravità, quindi il
   * valore calibrato rappresenta il picco assoluto del segnale a riposo. */
  model->suggested_threshold_mg = model->norm_mean[1] + 3.0f * model->norm_std[1];
  ESP_LOGI(TAG_ML,
           "[EXPLORING] Adaptive threshold: mean(m_p99)=%.1f  std=%.1f"
           "  -> mean+3sigma = %.1f mg  (over %lu windows)",
           model->norm_mean[1], model->norm_std[1],
           model->suggested_threshold_mg,
           (unsigned long)model->total_samples);

  return s_nov_count >= MIN_NOVELTY_FOR_SEEDING;
}

uint8_t exploring_novelty_pct(void) {
  return (uint8_t)((s_nov_count * 100) / NOVELTY_BUFFER_SIZE);
}

/*
 * FASE TRAINING — chiamata per ogni finestra.
 * Implementa k-means online (Hartigan-Wong semplificato):
 *   1. Normalizza il vettore di feature.
 *   2. Trova il centroide più vicino (nearest_centroid_idx).
 *   3. Aggiorna il centroide con media mobile incrementale:
 *        C_k ← C_k + (x - C_k) / n_k
 *      Equivalente alla media di tutti i campioni assegnati senza memorizzarli.
 *   4. Aggiorna le statistiche di distanza (Welford su dist) e il massimo.
 *      Queste statistiche servono a calcolare la soglia in training_finalize.
 */
void training_update(KMeansModel *model, const InferenceFeatures *features) {
  float raw[FEATURE_DIM];
  features_to_raw(features, raw);
  model->total_samples++;

  float norm_fv[FEATURE_DIM];
  normalise(model, raw, norm_fv);

  int k = nearest_centroid_idx(model, norm_fv);
  model->centroid_counts[k]++;
  uint32_t cnt = model->centroid_counts[k];
  /* Media mobile: C_k ← C_k + (x - C_k) / n */
  for (int d = 0; d < FEATURE_DIM; ++d)
    model->centroids[k][d] += (norm_fv[d] - model->centroids[k][d]) / (float)cnt;

  /* Distanza dal centroide aggiornato (non da quello pre-aggiornamento) */
  float dist = euclidean_dist(norm_fv, model->centroids[k], FEATURE_DIM);
  model->dist_n[k]++;
  welford_update(&(model->dist_mean[k]), &(model->dist_M2[k]), model->dist_n[k], dist);
  if (dist > model->dist_max[k]) model->dist_max[k] = dist;
}

/*
 * Fine della fase TRAINING — calcola la soglia di anomalia per ogni centroide.
 *
 * La soglia è il massimo tra:
 *   thr_stat = mean_dist + SIGMA_MULT * sigma_dist  (statistica: copre SIGMA_MULT deviazioni standard)
 *   thr_max  = max_dist  * MAX_DIST_MARGIN          (sicurezza: non escludere il peggior campione visto)
 *   MIN_THRESHOLD                                    (floor: evita soglie collassate a zero)
 *
 * Durante l'inferenza, un campione con dist > thr è anomalo.
 * La soglia è per-centroide: cluster con più variabilità hanno soglie più alte.
 */
void training_finalize(KMeansModel *model) {
  ESP_LOGI(TAG_ML, "[TRAINING] Finalising model...");
  for (int k = 0; k < KMEANS_K; ++k) {
    float sigma = (model->dist_n[k] > 1)
        ? safe_sqrt(model->dist_M2[k] / (float)(model->dist_n[k] - 1))
        : 0.0f;
    float thr_stat = model->dist_mean[k] + SIGMA_MULT * sigma;
    float thr_max  = model->dist_max[k] * MAX_DIST_MARGIN;
    model->dist_threshold[k] = fmaxf(fmaxf(thr_stat, thr_max), MIN_THRESHOLD);
  }
  model->finalised = true;
  training_print_model(model);
}

/* ─── NVS persistence ───────────────────────────────────────────────────────── */

static const char NVS_NS[]  = "antitheft";
static const char NVS_KEY[] = "kmeans";

/* Salva l'intero struct KMeansModel in NVS come blob grezzo.
 * Il controllo sizeof() in training_load() garantisce compatibilità:
 * se K o FEATURE_DIM cambiano, la dimensione non corrisponde e il modello
 * viene rifiutato, forzando un nuovo training. */
bool training_save(const KMeansModel *model) {
  if (!model->finalised) {
    ESP_LOGW(TAG_ML, "[NVS] Cannot save: not finalised.");
    return false;
  }
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG_ML, "[NVS] ERROR: cannot open namespace.");
    return false;
  }
  err = nvs_set_blob(nvs, NVS_KEY, model, sizeof(KMeansModel));
  if (err == ESP_OK) err = nvs_commit(nvs);
  nvs_close(nvs);
  bool ok = (err == ESP_OK);
  ESP_LOGI(TAG_ML, "[NVS] %s (%u bytes).", ok ? "Saved OK" : "SAVE FAILED",
           (unsigned)sizeof(KMeansModel));
  return ok;
}

bool training_load(KMeansModel *model) {
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    ESP_LOGW(TAG_ML, "[NVS] Cannot open namespace (may not exist yet).");
    return false;
  }
  size_t stored_len = 0;
  err = nvs_get_blob(nvs, NVS_KEY, NULL, &stored_len);
  bool ok = false;
  if (err == ESP_OK && stored_len == sizeof(KMeansModel)) {
    err = nvs_get_blob(nvs, NVS_KEY, model, &stored_len);
    if (err == ESP_OK) ok = model->finalised;
  }
  nvs_close(nvs);
  ESP_LOGI(TAG_ML, "[NVS] %s (stored=%u, expected=%u).",
           ok ? "Model loaded" : "No valid model",
           (unsigned)stored_len, (unsigned)sizeof(KMeansModel));
  return ok;
}

void training_erase_nvs(void) {
  nvs_handle_t nvs;
  if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
    nvs_erase_key(nvs, NVS_KEY);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG_ML, "[NVS] Model erased.");
  }
}

/* ─── Inferenza ─────────────────────────────────────────────────────────────── */

/*
 * Classifica una finestra come baseline o anomalia.
 *
 * Pipeline:
 *   1. features_to_raw()      → float[51]
 *   2. normalise()            → z-score con statistiche congelate da Exploring
 *   3. nearest_centroid_idx() → trova il cluster più vicino
 *   4. euclidean_dist()       → distanza dal centroide
 *   5. dist <= threshold[k]   → baseline (true) o anomalia (false)
 *
 * Restituisce true = baseline (normale), false = anomalia.
 * out_distance e out_cluster sono opzionali (passare NULL per ignorarli).
 */
bool training_is_baseline(const KMeansModel *model,
                          const InferenceFeatures *features,
                          float *out_distance, int *out_cluster) {
  if (!model->finalised) {
    /* Modello non pronto: considera tutto baseline per sicurezza */
    if (out_distance) *out_distance = 0.0f;
    if (out_cluster)  *out_cluster  = -1;
    return true;
  }
  float raw[FEATURE_DIM];
  features_to_raw(features, raw);
  float norm_fv[FEATURE_DIM];
  normalise(model, raw, norm_fv);
  int k = nearest_centroid_idx(model, norm_fv);
  float dist = euclidean_dist(norm_fv, model->centroids[k], FEATURE_DIM);
  if (out_distance) *out_distance = dist;
  if (out_cluster)  *out_cluster  = k;
  return dist <= model->dist_threshold[k];
}

/* ─── Debug ──────────────────────────────────────────────────────────────────── */

/* 51 nomi — devono restare sincronizzati con features_to_raw(). */
static const char *FEAT_NAMES[FEATURE_DIM] = {
    /* [0]     */ "impact_score",
    /* [1- 4]  */ "m_p99",    "x_p99",    "y_p99",    "z_p99",
    /* [5- 8]  */ "m_jerk",   "x_jerk",   "y_jerk",   "z_jerk",
    /* [9-12]  */ "m_b2040",  "x_b2040",  "y_b2040",  "z_b2040",
    /* [13-16] */ "m_b40100", "x_b40100", "y_b40100", "z_b40100",
    /* [17-20] */ "m_b1_5",   "x_b1_5",   "y_b1_5",   "z_b1_5",
    /* [21-24] */ "m_b5_20",  "x_b5_20",  "y_b5_20",  "z_b5_20",
    /* [25-27] */ "x_zcr",    "y_zcr",    "z_zcr",
    /* [28-34] */ "x_f0", "x_f1", "x_f2", "x_f3", "x_f4", "x_f5", "x_f6",
    /* [35-41] */ "y_f0", "y_f1", "y_f2", "y_f3", "y_f4", "y_f5", "y_f6",
    /* [42-48] */ "z_f0", "z_f1", "z_f2", "z_f3", "z_f4", "z_f5", "z_f6",
    /* [49-50] */ "time_sin", "time_cos",
};

void training_print_model(const KMeansModel *model) {
  ESP_LOGI(TAG_ML, "K-Means++ model: K=%d  feat_dim=%d  samples=%lu",
           KMEANS_K, FEATURE_DIM, (unsigned long)model->total_samples);
  ESP_LOGI(TAG_ML, "Normalisation:");
  for (int d = 0; d < FEATURE_DIM; ++d)
    ESP_LOGI(TAG_ML, "  [%2d] %-12s  mean=%10.4f  std=%9.4f", d, FEAT_NAMES[d],
             model->norm_mean[d], model->norm_std[d]);
  ESP_LOGI(TAG_ML, "Centroids:");
  for (int k = 0; k < KMEANS_K; ++k)
    ESP_LOGI(TAG_ML, "  C%d  n=%-6lu  dist_mean=%.4f  dist_max=%.4f  thr=%.4f",
             k, (unsigned long)model->centroid_counts[k], model->dist_mean[k],
             model->dist_max[k], model->dist_threshold[k]);
}
