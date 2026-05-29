/*
 * sliding_window.c
 *
 * Buffer circolare con overlap per inferenza continua senza perdita di campioni.
 * Versione ESP-IDF per ESP32-C3 — usa due task FreeRTOS separati:
 *   sensor_sampler_task : scrive campioni nel buffer, segnala con semaphore
 *   ml_processor_task   : attende il semaphore, estrae la finestra, fa inferenza
 * Questo file implementa solo la struttura dati — la gestione dei task è in main.c.
 *
 * Struttura:
 *   buffer circolare da CIRCULAR_BUFFER_SIZE campioni
 *   └─ contiene sempre gli ultimi N campioni in ordine cronologico
 *
 * Meccanismo sliding window:
 *   - Ogni nuovo campione viene scritto nel buffer circolare.
 *   - Ogni STEP_SAMPLES (50) campioni nuovi, viene estratta una finestra
 *     di WINDOW_SAMPLES (256) campioni → compute_features + inferenza.
 *   - La finestra avanza di 50 campioni per volta, con overlap di 206.
 *   - Questo permette di rilevare un evento che inizia in qualsiasi punto
 *     del tempo, non solo all'inizio di una finestra.
 *
 *                     ◄─────── WINDOW_SAMPLES (256) ────────►
 *   buffer: [... 206 campioni vecchi | 50 nuovi campioni ...]
 *                     ◄─ riusati ──►◄─── nuovi ───►
 *
 * Ring buffer dei voti:
 *   Ogni finestra produce un voto 0 (baseline) o 1 (anomalia).
 *   Gli ultimi VOTE_WINDOW_N (20) voti sono tenuti in un ring buffer.
 *   L'allarme scatta quando vote_count >= VOTE_THRESHOLD.
 *   Questo filtra i transienti casuali: richiede anomalia sostenuta
 *   per almeno VOTE_THRESHOLD finestre nelle ultime 5 secondi.
 *
 * Differenza rispetto a sensor-standalone: non c'è il warmup di 2×WINDOW_SAMPLES.
 * Il task sampler e il task ML girano su core separati (0 e 1) quindi
 * il campionamento continua durante la feature extraction — non c'è il
 * problema dello zero-padding che affligge sensor-standalone (single-threaded).
 */

#include "sliding_window.h"
#include "config.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "SLIDING_WINDOW";

void sliding_window_init(SlidingWindowState *state, const KMeansModel *model) {
    if (!state || !model) return;

    memset(state, 0, sizeof(SlidingWindowState));
    state->model = model;
    state->write_idx = 0;
    state->new_sample_count = 0;
    state->vote_idx = 0;
    state->vote_count = 0;
    state->ready = false;

    ESP_LOGI(TAG, "Initialized. Buffer size: %d samples/axis, voting window: %d",
             CIRCULAR_BUFFER_SIZE, VOTE_WINDOW_N);
}

/*
 * Aggiunge un campione al buffer circolare e segnala quando una nuova
 * finestra è pronta per l'inferenza.
 *
 * Il buffer circolare usa write_idx % CIRCULAR_BUFFER_SIZE come indice
 * di scrittura. Il campionamento continua anche mentre ml_processor_task
 * processa la finestra precedente (grazie ai task separati su core diversi)
 * — per questo il guard su state->ready è importante: senza di esso,
 * ogni campione oltre il 50° chiamerebbe xSemaphoreGive ripetutamente,
 * accumulando segnali nel semaphore e causando invocazioni spurie del task ML.
 *
 * Restituisce true quando STEP_SAMPLES nuovi campioni sono pronti.
 */
bool sliding_window_add_sample(SlidingWindowState *state, int16_t x, int16_t y, int16_t z) {
    if (!state) return false;

    /* Scrivi sempre nel buffer circolare — il campionamento non si interrompe
     * mentre il task ML è in esecuzione. */
    int idx = state->write_idx % CIRCULAR_BUFFER_SIZE;
    state->buffer_x[idx] = x;
    state->buffer_y[idx] = y;
    state->buffer_z[idx] = z;
    state->write_idx++;

    /* Non contare nuovi campioni finché sliding_window_step() non ha consumato
     * il segnale precedente (guard contro semaphore flood). */
    if (state->ready) {
        return false;
    }

    state->new_sample_count++;
    if (state->new_sample_count >= STEP_SAMPLES) {
        state->ready = true;
        return true;
    }
    return false;
}

/*
 * Estrae la finestra corrente dal buffer circolare, calcola le 51 feature,
 * esegue l'inferenza k-means e aggiorna il ring buffer dei voti.
 *
 * La finestra estratta contiene gli ultimi WINDOW_SAMPLES campioni:
 *   start = (write_idx - WINDOW_SAMPLES) mod CIRCULAR_BUFFER_SIZE
 * I campioni vengono copiati in ordine cronologico in window_x/y/z
 * (la copia risolve il wrap-around del buffer circolare).
 *
 * Restituisce: 1 = anomalia, 0 = baseline, -1 = errore.
 */
int sliding_window_step(SlidingWindowState *state, float time_sin, float time_cos) {
    if (!state || !state->ready || !state->model) {
        return -1;
    }

    /* Estrai gli ultimi WINDOW_SAMPLES campioni in ordine cronologico */
    static int16_t window_x[WINDOW_SAMPLES];
    static int16_t window_y[WINDOW_SAMPLES];
    static int16_t window_z[WINDOW_SAMPLES];

    int start_idx = (state->write_idx - WINDOW_SAMPLES + CIRCULAR_BUFFER_SIZE) % CIRCULAR_BUFFER_SIZE;
    for (int i = 0; i < WINDOW_SAMPLES; i++) {
        int src_idx = (start_idx + i) % CIRCULAR_BUFFER_SIZE;
        window_x[i] = state->buffer_x[src_idx];
        window_y[i] = state->buffer_y[src_idx];
        window_z[i] = state->buffer_z[src_idx];
    }

    /* Estrae le 51 feature e classifica */
    InferenceFeatures feat = compute_features(
        window_x, window_y, window_z,
        WINDOW_SAMPLES, SAMPLING_RATE_HZ,
        time_sin, time_cos
    );

    float distance = 0.0f;
    int cluster = 0;
    bool is_baseline = training_is_baseline(state->model, &feat, &distance, &cluster);
    uint8_t vote = is_baseline ? 0 : 1;

    /* Aggiorna il ring buffer dei voti: rimuove il voto più vecchio,
     * inserisce quello nuovo, aggiorna il contatore. */
    state->vote_count -= state->vote_ring[state->vote_idx]; /* rimuovi voto uscente */
    state->vote_ring[state->vote_idx] = vote;               /* inserisci voto entrante */
    state->vote_count += vote;
    state->vote_idx = (state->vote_idx + 1) % VOTE_WINDOW_N; /* avanza puntatore circolare */

    /* Salva per il log e il task principale */
    state->last_features = feat;
    state->last_distance = distance;
    state->last_cluster  = cluster;

    /* Reset contatore: pronto per accumulare il prossimo step */
    state->new_sample_count = 0;
    state->ready = false;

    ESP_LOGD(TAG, "[VOTE] is_baseline=%d, dist=%.3f, cluster=%d, vote_count=%d/%d",
             is_baseline, distance, cluster, state->vote_count, VOTE_WINDOW_N);

    return vote;
}

int sliding_window_get_vote_count(const SlidingWindowState *state) {
    if (!state) return 0;
    return state->vote_count;
}

/*
 * Reset completo: azzera buffer campioni e ring dei voti.
 * Chiamato dopo un allarme prima di tornare in deep sleep,
 * o dopo un re-training richiesto dall'hub.
 * In questa versione (dual-task) non c'è il problema dello zero-padding
 * di sensor-standalone: il task sampler riempie il buffer quasi subito
 * dopo il reset prima che il task ML elabori la prima finestra.
 */
void sliding_window_reset(SlidingWindowState *state) {
    if (!state) return;

    memset(state->buffer_x, 0, sizeof(state->buffer_x));
    memset(state->buffer_y, 0, sizeof(state->buffer_y));
    memset(state->buffer_z, 0, sizeof(state->buffer_z));
    memset(state->vote_ring, 0, sizeof(state->vote_ring));

    state->write_idx = 0;
    state->new_sample_count = 0;
    state->vote_idx = 0;
    state->vote_count = 0;
    state->ready = false;

    ESP_LOGI(TAG, "Reset complete");
}
