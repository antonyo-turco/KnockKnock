#include "feature_extraction.h"
#include <math.h>
#include <ArduinoFFT.h>

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

static float safe_float(float value) {
    return (isnan(value) || isinf(value)) ? 0.0f : value;
}

// 99th percentile of |values[0..size-1]|.
// Uses a fixed-size static buffer — size must be <= SAMPLE_COUNT.
static float percentile_99(const float values[], int size) {
    if (size <= 0) return 0.0f;

    // Static buffer — avoids VLA stack allocation on every call.
    static float sorted[SAMPLE_COUNT];
    for (int i = 0; i < size; ++i) {
        sorted[i] = fabsf(values[i]);
    }

    // Insertion sort — O(n²) but n is always 256, ~32k comparisons, fast enough.
    for (int i = 1; i < size; ++i) {
        float key = sorted[i];
        int   j   = i - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            --j;
        }
        sorted[j + 1] = key;
    }

    int index = (int)(size * 0.99f);
    if (index >= size) index = size - 1;
    return sorted[index];
}

// Trapezoidal band-power integration over [low, high) Hz.
// freqs[] and power[] must be the FFT frequency and magnitude arrays
// (FFT_SIZE/2 elements each).
static float bandpower(const float freqs[], const float power[],
                       int size, float low, float high) {
    float result = 0.0f;
    for (int i = 1; i < size; ++i) {
        if (freqs[i] >= low && freqs[i] < high) {
            result += (power[i] + power[i - 1]) * (freqs[i] - freqs[i - 1]) * 0.5f;
        }
    }
    return safe_float(result);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Per-axis (or magnitude) metric computation
//
//  Computes p99, jerk_max, and 20-40 Hz band power for one signal vector.
//  signal[] has exactly SAMPLE_COUNT == FFT_SIZE elements — no zero-padding.
// ─────────────────────────────────────────────────────────────────────────────

static void compute_signal_metrics(const float signal[], int size, float sampling_rate_hz,
                                   float &p99_out, float &jerk_max_out, float &band20_40_out) {
    // ── p99 ──────────────────────────────────────────────────────────────────
    p99_out = percentile_99(signal, size);

    // ── jerk_max  (max absolute sample-to-sample delta) ──────────────────────
    jerk_max_out = 0.0f;
    for (int i = 1; i < size; ++i) {
        float jerk = fabsf(signal[i] - signal[i - 1]);
        if (jerk > jerk_max_out) jerk_max_out = jerk;
    }

    // ── FFT → band power 20-40 Hz ────────────────────────────────────────────
    // Static buffers: FFT_SIZE floats each — shared across calls (single-threaded).
    static float vReal[FFT_SIZE];
    static float vImag[FFT_SIZE];

    // size == FFT_SIZE, so no zero-padding needed — copy directly.
    for (int i = 0; i < FFT_SIZE; ++i) {
        vReal[i] = (i < size) ? signal[i] : 0.0f;
        vImag[i] = 0.0f;
    }

    ArduinoFFT<float> FFT(vReal, vImag, FFT_SIZE, sampling_rate_hz);
    FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    FFT.compute(FFT_FORWARD);
    FFT.complexToMagnitude();

    // Frequency axis — static, computed once (values never change).
    static float freqs[FFT_SIZE / 2];
    static bool  freqs_ready = false;
    if (!freqs_ready) {
        for (int i = 0; i < FFT_SIZE / 2; ++i) {
            freqs[i] = ((float)i * sampling_rate_hz) / (float)FFT_SIZE;
        }
        freqs_ready = true;
    }

    band20_40_out = bandpower(freqs, vReal, FFT_SIZE / 2, 20.0f, 40.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public API
// ─────────────────────────────────────────────────────────────────────────────

InferenceFeatures compute_features(
    const int16_t x_values[],
    const int16_t y_values[],
    const int16_t z_values[],
    int size,
    float sampling_rate_hz)
{
    InferenceFeatures features{};
    if (size <= 0) return features;

    // ── Convert raw int16 axes to float; compute magnitude ───────────────────
    // Static buffers — avoid stack VLAs.
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

    for (int i = 1; i < size; ++i) {
        float jerk = fabsf(mag_m[i] - mag_m[i - 1]);
        if (jerk > m_jerk_max) m_jerk_max = jerk;
    }

    float m_rms = sqrtf(m_rms_acc / (float)size);
    float m_p99 = percentile_99(mag_m, size);

    // ── Magnitude FFT features ───────────────────────────────────────────────
    float dummy_p99, dummy_jerk, m_band_20_40;
    compute_signal_metrics(mag_m, size, sampling_rate_hz,
                           dummy_p99, dummy_jerk, m_band_20_40);

    // ── impact_score — normalised composite in [0, 1] ────────────────────────
    // Weights: p99=0.40, jerk=0.35, band=0.25  (sum = 1.0)
    // Each sub-score is clamped to [0,1] relative to a multiple of RMS
    // so that the score is independent of absolute signal amplitude scale.
    float p99_norm  = fminf(1.0f, m_p99       / (m_rms *  8.0f + 1e-9f));
    float jerk_norm = fminf(1.0f, m_jerk_max  / (m_rms * 12.0f + 1e-9f));

    // Band power is normalised by total spectral power rather than RMS.
    // Recompute total power from the magnitude FFT already done inside
    // compute_signal_metrics — we need it here, so do a lightweight pass.
    // (The FFT buffers are static and still hold the magnitude values.)
    // To avoid re-running the FFT we instead normalise band by a fixed
    // heuristic: band / (m_rms² * size * 0.3) — consistent across windows.
    float band_norm = fminf(1.0f, m_band_20_40 / (m_rms * m_rms * (float)size * 0.3f + 1e-9f));

    features.impact_score = safe_float(
        0.40f * p99_norm + 0.35f * jerk_norm + 0.25f * band_norm);

    features.m_p99        = safe_float(m_p99);
    features.m_jerk_max   = safe_float(m_jerk_max);
    features.m_band_20_40 = safe_float(m_band_20_40);

    // ── Per-axis metrics ─────────────────────────────────────────────────────
    float x_p99, y_p99, z_p99;
    float x_jerk, y_jerk, z_jerk;
    float x_band, y_band, z_band;

    compute_signal_metrics(mag_x, size, sampling_rate_hz, x_p99, x_jerk, x_band);
    compute_signal_metrics(mag_y, size, sampling_rate_hz, y_p99, y_jerk, y_band);
    compute_signal_metrics(mag_z, size, sampling_rate_hz, z_p99, z_jerk, z_band);

    features.x_p99        = safe_float(x_p99);
    features.y_p99        = safe_float(y_p99);
    features.z_p99        = safe_float(z_p99);
    features.x_jerk_max   = safe_float(x_jerk);
    features.y_jerk_max   = safe_float(y_jerk);
    features.z_jerk_max   = safe_float(z_jerk);
    features.x_band_20_40 = safe_float(x_band);
    features.y_band_20_40 = safe_float(y_band);
    features.z_band_20_40 = safe_float(z_band);

    return features;
}
