#ifndef FFT_PROCESSOR_H
#define FFT_PROCESSOR_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inizializza il modulo processore FFT e alloca le risorse necessarie.
 * 
 * @param[in] max_samples Numero massimo di campioni previsti (deve essere potenza di 2).
 * @return ESP_OK se l'inizializzazione ha successo, altrimenti un codice di errore.
 */
esp_err_t fft_processor_init(int max_samples);

/**
 * @brief Elabora un segnale nel dominio del tempo ed estrae la magnitudo lineare delle frequenze.
 * 
 * @param[in]  input_signal     Array contenente il segnale di input (dimensione: samples). ATTENZIONE: verrà modificato dalla funzione di windowing.
 * @param[out] output_magnitude Array dove verranno salvate le magnitudo lineari (dimensione: samples / 2).
 * @param[in]  samples          Numero di campioni (es. 1024, potenza di 2).
 */
void fft_processor_compute_magnitude(float *input_signal, float *output_magnitude, int samples);

#ifdef __cplusplus
}
#endif

#endif // FFT_PROCESSOR_H