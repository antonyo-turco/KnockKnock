#include "feature_extraction.h"
#include <math.h>
#include <arduinoFFT.h>

static float safe_float(float value) {
    if (isnan(value) || isinf(value)) {
        return 0.0f;
    }
    return value;
}

static float percentile_99(const float values[], int size) {
    if (size <= 0) {
        return 0.0f;
    }

    float sorted[size];
    for (int i = 0; i < size; ++i) {
        sorted[i] = fabsf(values[i]);
    }

    for (int i = 0; i < size - 1; ++i) {
        for (int j = i + 1; j < size; ++j) {
            if (sorted[i] > sorted[j]) {
                float tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }

    int index = (int)(size * 0.99f);
    if (index >= size) {
        index = size - 1;
    }
    return sorted[index];
}

static float bandpower(const double freqs[], const double power[], int size, float low, float high) {
    double result = 0.0;
    for (int i = 1; i < size; ++i) {
        if (freqs[i] >= low && freqs[i] < high) {
            result += (power[i] + power[i - 1]) * (freqs[i] - freqs[i - 1]) * 0.5;
        }
    }
    return safe_float((float)result);
}

static float spectral_flux(const float signal[], int size, float sampling_rate_hz) {
    const int fft_size = 256;
    if (size < fft_size) {
        return 0.0f;
    }

    double prev_real[fft_size];
    double prev_imag[fft_size];
    double curr_real[fft_size];
    double curr_imag[fft_size];

    int start_prev = 0;
    int start_curr = size - fft_size;

    for (int i = 0; i < fft_size; ++i) {
        prev_real[i] = signal[start_prev + i];
        prev_imag[i] = 0.0;
        curr_real[i] = signal[start_curr + i];
        curr_imag[i] = 0.0;
    }

    ArduinoFFT<double> FFTprev = ArduinoFFT<double>(prev_real, prev_imag, fft_size, sampling_rate_hz);
    FFTprev.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    FFTprev.compute(FFT_FORWARD);
    FFTprev.complexToMagnitude();

    ArduinoFFT<double> FFTcurr = ArduinoFFT<double>(curr_real, curr_imag, fft_size, sampling_rate_hz);
    FFTcurr.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    FFTcurr.compute(FFT_FORWARD);
    FFTcurr.complexToMagnitude();

    double prev_sum = 0.0;
    double curr_sum = 0.0;
    for (int i = 0; i < fft_size / 2; ++i) {
        prev_sum += prev_real[i];
        curr_sum += curr_real[i];
    }

    if (prev_sum <= 1e-12 || curr_sum <= 1e-12) {
        return 0.0f;
    }

    double flux = 0.0;
    for (int i = 0; i < fft_size / 2; ++i) {
        double p1 = prev_real[i] / prev_sum;
        double p2 = curr_real[i] / curr_sum;
        double diff = p2 - p1;
        flux += diff * diff;
    }

    return safe_float((float)sqrt(flux / (fft_size / 2)));
}

AxisFeatures compute_features(const float x_values[], const float y_values[], const float z_values[], int size, float sampling_rate_hz) {
    AxisFeatures features{};

    if (size <= 0) {
        features.impact_score = 0.0f;
        features.event_flag = 0;
        return features;
    }

    const int fft_size = 512;
    double vReal[fft_size];
    double vImag[fft_size];
    float magnitude[size];

    float m_rms_acc = 0.0f;
    float m_jerk_max = 0.0f;

    for (int i = 0; i < size; ++i) {
        float mx = x_values[i];
        float my = y_values[i];
        float mz = z_values[i];
        float m = sqrtf(mx * mx + my * my + mz * mz);
        magnitude[i] = m;
        m_rms_acc += m * m;
    }

    for (int i = 1; i < size; ++i) {
        float jerk = fabsf(magnitude[i] - magnitude[i - 1]);
        if (jerk > m_jerk_max) {
            m_jerk_max = jerk;
        }
    }

    float m_rms = sqrtf(m_rms_acc / size);
    float m_p99 = percentile_99(magnitude, size);

    for (int i = 0; i < fft_size; ++i) {
        if (i < size) {
            vReal[i] = magnitude[i];
        } else {
            vReal[i] = 0.0;
        }
        vImag[i] = 0.0;
    }

    ArduinoFFT<double> FFT = ArduinoFFT<double>(vReal, vImag, fft_size, sampling_rate_hz);
    FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    FFT.compute(FFT_FORWARD);
    FFT.complexToMagnitude();

    double total_power = 0.0;
    for (int i = 0; i < fft_size / 2; ++i) {
        total_power += vReal[i];
    }

    double freqs[fft_size / 2];
    for (int i = 0; i < fft_size / 2; ++i) {
        freqs[i] = ((double)i * sampling_rate_hz) / fft_size;
    }

    float band_20_40 = bandpower(freqs, vReal, fft_size / 2, 20.0f, 40.0f);
    float flux = spectral_flux(magnitude, size, sampling_rate_hz);

    float p99_norm = fminf(1.0f, m_p99 / (m_rms * 8.0f + 1e-9f));
    float jerk_norm = fminf(1.0f, m_jerk_max / (m_rms * 12.0f + 1e-9f));
    float band_norm = fminf(1.0f, band_20_40 / (total_power * 0.3f + 1e-9f));
    float flux_norm = fminf(1.0f, flux / 0.25f);

    features.impact_score = safe_float(0.35f * p99_norm + 0.30f * jerk_norm + 0.20f * band_norm + 0.15f * flux_norm);
    features.event_flag = (features.impact_score >= 0.60f) ? 1 : 0;

    return features;
}
