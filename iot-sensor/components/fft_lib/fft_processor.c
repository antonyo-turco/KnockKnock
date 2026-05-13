#include "fft_processor.h"
#include "esp_dsp.h"
#include "esp_log.h"
#include <math.h>
#include <stdlib.h>

static const char *TAG = "FFT_PROCESSOR";

// Array interno per i calcoli complessi (Parte Reale e Immaginaria)
static float *complex_workspace = NULL; 
static int max_allocated_samples = 0;

esp_err_t fft_processor_init(int max_samples) {
    esp_err_t ret = dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Errore durante l'inizializzazione di dsps_fft2r_init_fc32");
        return ret;
    }

    if (complex_workspace != NULL) {
        free(complex_workspace);
    }

    complex_workspace = (float *)malloc(max_samples * 2 * sizeof(float));
    if (complex_workspace == NULL) {
        ESP_LOGE(TAG, "Errore di allocazione per complex_workspace");
        return ESP_ERR_NO_MEM;
    }
    max_allocated_samples = max_samples;

    return ESP_OK;
}

void fft_processor_compute_magnitude(float *input_signal, float *output_magnitude, int samples) {
    if (complex_workspace == NULL || samples > max_allocated_samples) {
        ESP_LOGE(TAG, "Workspace non inizializzato o numero di campioni troppo elevato");
        return;
    }

    // 1. Popolamento dell'array di lavoro (Segnale reale, Immaginario a 0)
    for (int i = 0; i < samples; i++) {
        complex_workspace[i * 2] = input_signal[i];
        complex_workspace[i * 2 + 1] = 0.0f;
    }

    // 2. Applicazione della finestra di Hann per ridurre il leakage spettrale
    dsps_wind_hann_f32(input_signal, samples);

    // 3. Esecuzione della trasformata di Fourier Veloce (Radix-2)
    dsps_fft2r_fc32(complex_workspace, samples);

    // 4. Riordino dei bit (operazione necessaria per l'algoritmo FFT di esp-dsp)
    dsps_bit_rev_fc32(complex_workspace, samples);

    // 5. Estrazione della magnitudo logaritmica (in dB) per le frequenze positive
    for (int i = 0; i < samples / 2; i++) {
        float real = complex_workspace[i * 2];
        float imag = complex_workspace[i * 2 + 1];
        float power = (real * real + imag * imag) / samples;
        
        // Evita il logaritmo di zero per prevenire errori matematici (NaN/Inf)
        if (power < 1e-12f) {
            output_magnitude[i] = -120.0f; // Valore minimo di clamping (dB)
        } else {
            output_magnitude[i] = 10.0f * log10f(power);
        }
    }
}