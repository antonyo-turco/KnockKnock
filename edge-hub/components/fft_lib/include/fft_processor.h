#ifndef FFT_PROCESSOR_H
#define FFT_PROCESSOR_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FFT_PROCESSOR_SAMPLES 1024 

/**
 * @brief Inizializza il modulo processore FFT e alloca le risorse necessarie.
 * 
 * @return ESP_OK se l'inizializzazione ha successo, altrimenti un codice di errore.
 */
esp_err_t fft_processor_init(void);

/**
 * @brief Elabora un segnale nel dominio del tempo ed estrae le magnitudo delle frequenze.
 * 
 * @param[in]  input_signal     Array contenente il segnale di input (dimensione: FFT_PROCESSOR_SAMPLES).
 * @param[out] output_magnitude Array dove verranno salvate le magnitudo (dimensione: FFT_PROCESSOR_SAMPLES / 2).
 */
void fft_processor_compute_magnitude(float *input_signal, float *output_magnitude);

#ifdef __cplusplus
}
#endif

#endif // FFT_PROCESSOR_H