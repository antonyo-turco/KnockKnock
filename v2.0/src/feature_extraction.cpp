#include "feature_extraction.h"
#include <math.h>
#include <float.h>
#include <ArduinoFFT.h>

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

static float safe_float(float v) {
    return (isnan(v) || isinf(v)) ? 0.0f : v;
}

// 99th percentile of |values[0..size-1]|.
// Uses insertion sort on a static buffer — O(n²), fine for n=256.
static float percentile_99(const float values[], int size) {
    if (size <= 0) return 0.0f;
    static float sorted[SAMPLE_COUNT];
    for (int i = 0; i < size; ++i) sorted[i] = fabsf(values[i]);
    for (int i = 1; i < size; ++i) {
        float key = sorted[i];
        int   j   = i - 1;
        while (j >= 0 && sorted[j] > key) { sorted[j + 1] = sorted[j]; --j; }
        sorted[j + 1] = key;
    }
    int idx = (int)(size * 0.99f);
    if (idx >= size) idx = size - 1;
    return sorted[idx];
}

// Zero-crossing rate: fraction of consecutive pairs that change sign.
static float compute_zcr(const float signal[], int size) {
    if (size < 2) return 0.0f;
    int crossings = 0;
    for (int i = 1; i < size; ++i) {
        if ((signal[i] >= 0.0f) != (signal[i - 1] >= 0.0f)) ++crossings;
    }
    return (float)crossings / (float)(size - 1);
}

// Trapezoidal band-power integration over [low, high) Hz.
static float bandpower(const float freqs[], const float power[],
                       int n, float low, float high) {
    float result = 0.0f;
    for (int i = 1; i < n; ++i) {
        if (freqs[i] >= low && freqs[i] < high) {
            result += (power[i] + power[i - 1]) * (freqs[i] - freqs[i - 1]) * 0.5f;
        }
    }
    return safe_float(result);
}

// Find the TOP7_COUNT bins (excluding DC bin 0) with highest magnitude,
// convert to Hz, and return them sorted ascending by frequency.
static void top7_frequencies(const float magnitudes[], int n_bins,
                              float sampling_rate_hz, float out[TOP7_COUNT]) {
    // Initialise with first TOP7_COUNT non-DC bins
    int   top_idx[TOP7_COUNT];
    float top_mag[TOP7_COUNT];
    int   start = (n_bins > TOP7_COUNT) ? TOP7_COUNT : n_bins - 1;
    for (int i = 0; i < TOP7_COUNT; ++i) {
        top_idx[i] = i + 1;                // skip bin 0 (DC)
        top_mag[i] = (i + 1 < n_bins) ? magnitudes[i + 1] : 0.0f;
    }
    // Keep top_mag sorted descending for efficient updates
    for (int i = 0; i < TOP7_COUNT - 1; ++i) {
        for (int j = i + 1; j < TOP7_COUNT; ++j) {
            if (top_mag[j] > top_mag[i]) {
                float tm = top_mag[i]; top_mag[i] = top_mag[j]; top_mag[j] = tm;
                int   ti = top_idx[i]; top_idx[i] = top_idx[j]; top_idx[j] = ti;
            }
        }
    }
    // Scan remaining bins
    for (int i = start + 1; i < n_bins; ++i) {
        if (magnitudes[i] > top_mag[TOP7_COUNT - 1]) {
            top_mag[TOP7_COUNT - 1] = magnitudes[i];
            top_idx[TOP7_COUNT - 1] = i;
            // Bubble up to maintain descending order
            for (int j = TOP7_COUNT - 2; j >= 0; --j) {
                if (top_mag[j + 1] > top_mag[j]) {
                    float tm = top_mag[j]; top_mag[j] = top_mag[j+1]; top_mag[j+1] = tm;
                    int   ti = top_idx[j]; top_idx[j] = top_idx[j+1]; top_idx[j+1] = ti;
                } else break;
            }
        }
    }
    // Convert to Hz
    float bin_hz = sampling_rate_hz / (float)(n_bins * 2);  // n_bins = FFT_SIZE/2
    for (int i = 0; i < TOP7_COUNT; ++i) {
        out[i] = top_idx[i] * bin_hz;
    }
    // Sort ascending by frequency (simple insertion sort on 7 elements)
    for (int i = 1; i < TOP7_COUNT; ++i) {
        float key = out[i];
        int   j   = i - 1;
        while (j >= 0 && out[j] > key) { out[j + 1] = out[j]; --j; }
        out[j + 1] = key;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Core per-signal metric computation
//
//  Computes all FFT-based and time-domain metrics for one signal vector of
//  exactly SAMPLE_COUNT == FFT_SIZE elements (no zero-padding).
//
//  Outputs: p99, jerk_max, band_1_5, band_5_20, band_20_40, top7_freq[7]
//  ZCR is computed separately (time-domain only, no FFT needed).
// ─────────────────────────────────────────────────────────────────────────────

static void compute_signal_metrics(
    const float signal[], int size, float sampling_rate_hz,
    float &p99_out, float &jerk_max_out,
    float &band_1_5_out, float &band_5_20_out, float &band_20_40_out,
    float top7_freq_out[TOP7_COUNT])
{
    // ── Time-domain metrics ──────────────────────────────────────────────────
    p99_out      = percentile_99(signal, size);
    jerk_max_out = 0.0f;
    for (int i = 1; i < size; ++i) {
        float j = fabsf(signal[i] - signal[i - 1]);
        if (j > jerk_max_out) jerk_max_out = j;
    }

    // ── FFT ─────────────────────────────────────────────────────────────────
    // Static buffers shared across calls (single-threaded, sequential calls OK)
    static float vReal[FFT_SIZE];
    static float vImag[FFT_SIZE];

    for (int i = 0; i < FFT_SIZE; ++i) {
        vReal[i] = (i < size) ? signal[i] : 0.0f;
        vImag[i] = 0.0f;
    }

    ArduinoFFT<float> FFT(vReal, vImag, FFT_SIZE, sampling_rate_hz);
    FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    FFT.compute(FFT_FORWARD);
    FFT.complexToMagnitude();

    // Frequency axis — computed once, cached forever
    static float freqs[FFT_SIZE / 2];
    static bool  freqs_ready = false;
    if (!freqs_ready) {
        for (int i = 0; i < FFT_SIZE / 2; ++i) {
            freqs[i] = ((float)i * sampling_rate_hz) / (float)FFT_SIZE;
        }
        freqs_ready = true;
    }

    const int n_bins = FFT_SIZE / 2;
    band_1_5_out   = bandpower(freqs, vReal, n_bins,  1.0f,  5.0f);
    band_5_20_out  = bandpower(freqs, vReal, n_bins,  5.0f, 20.0f);
    band_20_40_out = bandpower(freqs, vReal, n_bins, 20.0f, 40.0f);

    top7_frequencies(vReal, n_bins, sampling_rate_hz, top7_freq_out);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────────────────────

InferenceFeatures compute_features(
    const int16_t x_values[],
    const int16_t y_values[],
    const int16_t z_values[],
    int   size,
    float sampling_rate_hz)
{
    InferenceFeatures feat{};
    if (size <= 0) return feat;

    // ── Convert to float; compute magnitude vector ───────────────────────────
    static float mag_m[SAMPLE_COUNT];
    static float mag_x[SAMPLE_COUNT];
    static float mag_y[SAMPLE_COUNT];
    static float mag_z[SAMPLE_COUNT];

    float m_rms_acc  = 0.0f;
    float m_jerk_max = 0.0f;

    for (int i = 0; i < size; ++i) {
        float fx = (float)x_values[i];
        float fy = (float)y_values[i];
        float fz = (float)z_values[i];
        float m  = sqrtf(fx * fx + fy * fy + fz * fz);
        mag_m[i] = m;
        mag_x[i] = fx;
        mag_y[i] = fy;
        mag_z[i] = fz;
        m_rms_acc += m * m;
    }
    float m_rms = sqrtf(m_rms_acc / (float)size);

    for (int i = 1; i < size; ++i) {
        float j = fabsf(mag_m[i] - mag_m[i - 1]);
        if (j > m_jerk_max) m_jerk_max = j;
    }

    // ── Magnitude metrics ────────────────────────────────────────────────────
    float m_p99, m_jerk, m_b15, m_b520, m_b2040;
    float m_top7[TOP7_COUNT];
    compute_signal_metrics(mag_m, size, sampling_rate_hz,
                           m_p99, m_jerk, m_b15, m_b520, m_b2040, m_top7);
    // Use the pre-computed m_jerk_max (identical to m_jerk, kept for clarity)

    // ── Per-axis metrics ─────────────────────────────────────────────────────
    float x_p99, x_jerk, x_b15, x_b520, x_b2040;  float x_top7[TOP7_COUNT];
    float y_p99, y_jerk, y_b15, y_b520, y_b2040;  float y_top7[TOP7_COUNT];
    float z_p99, z_jerk, z_b15, z_b520, z_b2040;  float z_top7[TOP7_COUNT];

    compute_signal_metrics(mag_x, size, sampling_rate_hz,
                           x_p99, x_jerk, x_b15, x_b520, x_b2040, x_top7);
    compute_signal_metrics(mag_y, size, sampling_rate_hz,
                           y_p99, y_jerk, y_b15, y_b520, y_b2040, y_top7);
    compute_signal_metrics(mag_z, size, sampling_rate_hz,
                           z_p99, z_jerk, z_b15, z_b520, z_b2040, z_top7);

    // ── ZCR (time-domain, no FFT needed) ─────────────────────────────────────
    float x_zcr = compute_zcr(mag_x, size);
    float y_zcr = compute_zcr(mag_y, size);
    float z_zcr = compute_zcr(mag_z, size);

    // ── impact_score — normalised composite in [0, 1] ────────────────────────
    // Based on magnitude: p99 (0.40), jerk (0.35), band_20_40 (0.25)
    float p99_norm  = fminf(1.0f, m_p99      / (m_rms *  8.0f + 1e-9f));
    float jerk_norm = fminf(1.0f, m_jerk_max / (m_rms * 12.0f + 1e-9f));
    float band_norm = fminf(1.0f, m_b2040    / (m_rms * m_rms * (float)size * 0.3f + 1e-9f));
    feat.impact_score = safe_float(0.40f * p99_norm + 0.35f * jerk_norm + 0.25f * band_norm);

    // ── Fill struct ───────────────────────────────────────────────────────────
    feat.m_p99  = safe_float(m_p99);
    feat.x_p99  = safe_float(x_p99);
    feat.y_p99  = safe_float(y_p99);
    feat.z_p99  = safe_float(z_p99);

    feat.m_jerk_max = safe_float(m_jerk_max);
    feat.x_jerk_max = safe_float(x_jerk);
    feat.y_jerk_max = safe_float(y_jerk);
    feat.z_jerk_max = safe_float(z_jerk);

    feat.m_band_20_40 = safe_float(m_b2040);
    feat.x_band_20_40 = safe_float(x_b2040);
    feat.y_band_20_40 = safe_float(y_b2040);
    feat.z_band_20_40 = safe_float(z_b2040);

    feat.m_band_1_5 = safe_float(m_b15);
    feat.x_band_1_5 = safe_float(x_b15);
    feat.y_band_1_5 = safe_float(y_b15);
    feat.z_band_1_5 = safe_float(z_b15);

    feat.m_band_5_20 = safe_float(m_b520);
    feat.x_band_5_20 = safe_float(x_b520);
    feat.y_band_5_20 = safe_float(y_b520);
    feat.z_band_5_20 = safe_float(z_b520);

    feat.x_zcr = safe_float(x_zcr);
    feat.y_zcr = safe_float(y_zcr);
    feat.z_zcr = safe_float(z_zcr);

    for (int i = 0; i < TOP7_COUNT; ++i) {
        feat.x_top7_freq[i] = safe_float(x_top7[i]);
        feat.y_top7_freq[i] = safe_float(y_top7[i]);
        feat.z_top7_freq[i] = safe_float(z_top7[i]);
    }

    return feat;
}
