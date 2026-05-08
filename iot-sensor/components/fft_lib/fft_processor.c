#include "fft_processor.h"
#include "esp_dsp.h"
#include "esp_log.h"
#include <math.h>

static const char *TAG = "FFT_PROCESSOR";

// Array statico interno per i calcoli complessi (Parte Reale e Immaginaria)
static float complex_workspace[FFT_PROCESSOR_SAMPLES * 2]; 

esp_err_t fft_processor_init(void) {
    esp_err_t ret = dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Errore durante l'inizializzazione di dsps_fft2r_init_fc32");
    }
    return ret;
}

void fft_processor_compute_magnitude(float *input_signal, float *output_magnitude) {
    // 1. Popolamento dell'array di lavoro (Segnale reale, Immaginario a 0)
    for (int i = 0; i < FFT_PROCESSOR_SAMPLES; i++) {
        complex_workspace[i * 2] = input_signal[i];
        complex_workspace[i * 2 + 1] = 0.0f;
    }

    // 2. Applicazione della finestra di Hann per ridurre il leakage spettrale
    dsps_wind_hann_f32(input_signal, FFT_PROCESSOR_SAMPLES);

    // 3. Esecuzione della trasformata di Fourier Veloce (Radix-2)
    dsps_fft2r_fc32(complex_workspace, FFT_PROCESSOR_SAMPLES);

    // 4. Riordino dei bit (operazione necessaria per l'algoritmo FFT di esp-dsp)
    dsps_bit_rev_fc32(complex_workspace, FFT_PROCESSOR_SAMPLES);

    // 5. Estrazione della magnitudo logaritmica (in dB) per le frequenze positive
    for (int i = 0; i < FFT_PROCESSOR_SAMPLES / 2; i++) {
        float real = complex_workspace[i * 2];
        float imag = complex_workspace[i * 2 + 1];
        float power = (real * real + imag * imag) / FFT_PROCESSOR_SAMPLES;
        
        // Evita il logaritmo di zero per prevenire errori matematici (NaN/Inf)
        if (power < 1e-12f) {
            output_magnitude[i] = -120.0f; // Valore minimo di clamping (dB)
        } else {
            output_magnitude[i] = 10.0f * log10f(power);
        }
    }
}