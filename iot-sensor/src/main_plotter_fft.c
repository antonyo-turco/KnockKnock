/**
 * @file main_plotter_fft.c
 * @brief ADXL362 con FFT - Plot dati accelerometro e spettro frequenze
 * 
 * Legge l'accelerometro ADXL362, accumula N campioni, calcola la FFT
 * e stampa sia i dati raw che lo spettro di frequenze
 * 
 * Usa la libreria esp-dsp per l'FFT veloce
 */

#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "adxl362.h"
#include "fft_processor.h"

static const char *TAG = "PLOTTER_FFT";

/* =========================================================
 *  Pin definitions
 * ========================================================= */
#define MY_SPI_HOST   SPI3_HOST
#define MY_PIN_MOSI   GPIO_NUM_23
#define MY_PIN_MISO   GPIO_NUM_19
#define MY_PIN_SCLK   GPIO_NUM_18
#define MY_PIN_CS     GPIO_NUM_5
#define MY_SPI_CLOCK  1000000       /* 1 MHz */

/* =========================================================
 *  Configurazione
 * ========================================================= */
#define SAMPLING_RATE_MS  10   /* 10ms = 100Hz */
#define SAMPLING_FREQ_HZ  100  /* Frequenza di campionamento (100Hz) */
#define PLOT_MODE         0    /* 0=raw, 1=FFT, 2=alternato */

/* Buffer per raccogliere campioni e calcolare FFT */
static float signal_buffer[FFT_PROCESSOR_SAMPLES];
static float fft_magnitude[FFT_PROCESSOR_SAMPLES / 2];
static uint32_t buffer_index = 0;

/* =========================================================
 *  Inizializzazione del sensore
 * ========================================================= */
static adxl362_handle_t sensor_init(void)
{
    adxl362_handle_t sensor = NULL;

    adxl362_pins_t pins = {
        .spi_host     = MY_SPI_HOST,
        .pin_mosi     = MY_PIN_MOSI,
        .pin_miso     = MY_PIN_MISO,
        .pin_sclk     = MY_PIN_SCLK,
        .pin_cs       = MY_PIN_CS,
        .spi_clock_hz = MY_SPI_CLOCK,
    };

    esp_err_t ret = adxl362_init(&sensor, &pins);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADXL362 init failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    adxl362_set_range(sensor, ADXL362_RANGE_2G);
    adxl362_set_odr(sensor, ADXL362_ODR_100_HZ);
    adxl362_start_measurement(sensor);
    
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "ADXL362 inizializzato");
    ESP_LOGI(TAG, "Modalità plot: %d (0=raw, 1=FFT, 2=alternato)", PLOT_MODE);
    
    return sensor;
}

/* =========================================================
 *  Calcola la magnitudo dall'asse Z
 * ========================================================= */
static void process_fft(void)
{
    /* Calcola la FFT */
    fft_processor_compute_magnitude(signal_buffer, fft_magnitude);

    /* Stampa le prime 50 componenti di frequenza (fino a 5kHz @ 100Hz sampling) */
    printf("FFT:");
    for (int i = 0; i < 50; i++) {
        printf("%.1f,", fft_magnitude[i]);
    }
    printf("\n");
    fflush(stdout);
}

/* =========================================================
 *  Task principale
 * ========================================================= */
void plotter_task(void *pvParameter)
{
    adxl362_handle_t sensor = (adxl362_handle_t)pvParameter;
    uint32_t fft_cycle = 0;

    if (!sensor) {
        ESP_LOGE(TAG, "Sensore non inizializzato!");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Task plotter avviato");

    while (1) {
        adxl362_data_mg_t data;
        esp_err_t ret = adxl362_read_mg(sensor, &data);

        if (ret == ESP_OK) {
            if (PLOT_MODE == 0) {
                /* Modalità raw: stampa i dati X,Y,Z continuamente */
                printf("%.1f,%.1f,%.1f\n", data.x_mg, data.y_mg, data.z_mg);
                fflush(stdout);

            } else if (PLOT_MODE == 1) {
                /* Modalità FFT: accumula campioni sull'asse Z e calcola FFT */
                signal_buffer[buffer_index++] = data.z_mg;

                if (buffer_index >= FFT_PROCESSOR_SAMPLES) {
                    process_fft();
                    buffer_index = 0;
                    fft_cycle++;
                    ESP_LOGI(TAG, "FFT ciclo #%lu completato", fft_cycle);
                }

            } else if (PLOT_MODE == 2) {
                /* Modalità alternata: raw per 1024 samples, poi FFT */
                signal_buffer[buffer_index++] = data.z_mg;

                if (buffer_index < FFT_PROCESSOR_SAMPLES) {
                    printf("%.1f,%.1f,%.1f\n", data.x_mg, data.y_mg, data.z_mg);
                    fflush(stdout);
                } else {
                    process_fft();
                    buffer_index = 0;
                    fft_cycle++;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPLING_RATE_MS));
    }
}

/* =========================================================
 *  main
 * ========================================================= */
void app_main(void)
{
    ESP_LOGI(TAG, "=== ADXL362 con FFT ===");
    
    /* Inizializza la FFT */
    esp_err_t ret = fft_processor_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FFT init fallito: %s", esp_err_to_name(ret));
    }

    adxl362_handle_t sensor = sensor_init();
    
    if (!sensor) {
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    xTaskCreate(
        plotter_task,
        "plotter_fft_task",
        4096,                   /* Stack size più grande per FFT */
        (void *)sensor,
        1,
        NULL
    );
}
