#ifndef FEATURE_EXTRACTION_H
#define FEATURE_EXTRACTION_H

#include <Arduino.h>

// Struttura per contenere i dati di un campione
struct Measurement {
    int16_t x;
    int16_t y;
    int16_t z;
};

// Struttura per contenere le feature calcolate
struct AxisFeatures {
    float impact_score;
    int event_flag;
    // Aggiungi qui altre feature se necessario
    // Esempio:
    // float x_rms;
    // float y_rms;
    // float z_rms;
    // float m_dominant_freq;
};

/**
 * @brief Calcola le feature da un batch di dati dell'accelerometro.
 *
 * @param x_values Array dei valori dell'asse X.
 * @param y_values Array dei valori dell'asse Y.
 * @param z_values Array dei valori dell'asse Z.
 * @param size La dimensione degli array (numero di campioni).
 * @param sampling_rate_hz La frequenza di campionamento in Hz.
 * @return Una struttura AxisFeatures con le feature calcolate.
 */
AxisFeatures compute_features(const float x_values[], const float y_values[], const float z_values[], int size, float sampling_rate_hz);

#endif // FEATURE_EXTRACTION_H
