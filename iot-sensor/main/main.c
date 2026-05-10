/**
 * @file main_plotter.c
 * @brief ADXL362 continuous data reading and Serial Plotter output
 * 
 * Legge continuamente l'accelerometro ADXL362 su 3 assi (X, Y, Z)
 * e stampa i dati in formato CSV compatibile con Arduino Serial Plotter
 * 
 * Formato output:
 *   X,Y,Z
 *   123,456,789
 *   124,455,790
 *   ...
 *
 * PIN CONFIGURATION PER HELTEC V4 (pin 15-18 non saldati):
 *   MOSI -> GPIO 42
 *   MISO -> GPIO 40
 *   SCLK -> GPIO 39
 *   CS   -> GPIO 41
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "adxl362.h"

static const char *TAG = "PLOTTER";

/* =========================================================
 *  Pin definitions - HELTEC V4 con pin 15-18 non saldati
 * ========================================================= */
#define MY_SPI_HOST   SPI2_HOST        /* Heltec V4 usa SPI2 */
#define MY_PIN_MOSI   GPIO_NUM_42
#define MY_PIN_MISO   GPIO_NUM_40
#define MY_PIN_SCLK   GPIO_NUM_39
#define MY_PIN_CS     GPIO_NUM_41
#define MY_SPI_CLOCK  1000000         /* 1 MHz */

/* =========================================================
 *  Configurazione del sensore
 * ========================================================= */
#define SAMPLING_RATE_MS  10   /* Leggi ogni 10ms (100Hz) */
#define RANGE             ADXL362_RANGE_2G
#define ODR               ADXL362_ODR_100_HZ

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
        ESP_LOGE(TAG, "Verifica il collegamento:");
        ESP_LOGE(TAG, "  MOSI -> GPIO 42  |  MISO -> GPIO 40");
        ESP_LOGE(TAG, "  SCLK -> GPIO 39  |  CS   -> GPIO 41");
        ESP_LOGE(TAG, "  VDD  -> 3.3V     |  GND  -> GND");
        return NULL;
    }

    ESP_LOGI(TAG, "ADXL362 inicializzato correttamente");

    /* Configura il sensore */
    adxl362_set_range(sensor, RANGE);
    adxl362_set_odr(sensor, ODR);
    
    /* Avvia la misurazione */
    adxl362_start_measurement(sensor);
    
    /* Attendi che il sensore stabilizzi */
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(TAG, "Configurazione completata. Inizio acquisizione dati...");
    ESP_LOGI(TAG, "Apri Arduino IDE -> Strumenti -> Serial Plotter");
    
    return sensor;
}

/* =========================================================
 *  Task principale: leggi e stampa i dati
 * ========================================================= */
void plotter_task(void *pvParameter)
{
    adxl362_handle_t sensor = (adxl362_handle_t)pvParameter;
    
    if (!sensor) {
        ESP_LOGE(TAG, "Sensore non inizializzato!");
        vTaskDelete(NULL);
        return;
    }

    /* Stampa l'intestazione (opzionale, Serial Plotter lo ignora) */
    printf("X,Y,Z\n");
    fflush(stdout);

    while (1) {
        adxl362_data_mg_t data;
        esp_err_t ret = adxl362_read_mg(sensor, &data);

        if (ret == ESP_OK) {
            /* Stampa i dati in formato CSV per Serial Plotter
             * Usa virgola come separatore, newline come terminatore */
            printf("%.0f,%.0f,%.0f\n", data.x_mg, data.y_mg, data.z_mg);
            fflush(stdout);
        } else {
            ESP_LOGW(TAG, "Errore lettura sensore: %s", esp_err_to_name(ret));
        }

        /* Attendi prima della prossima lettura */
        vTaskDelay(pdMS_TO_TICKS(SAMPLING_RATE_MS));
    }
}

/* =========================================================
 *  Funzione principale
 * ========================================================= */
void app_main(void)
{
    ESP_LOGI(TAG, "=== ADXL362 Serial Plotter ===");
    ESP_LOGI(TAG, "HELTEC V4 - Pin config: MOSI=42, MISO=40, SCLK=39, CS=41");
    
    adxl362_handle_t sensor = sensor_init();
    
    if (!sensor) {
        /* Sensore non disponibile - resta in loop per non bootloop */
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /* Crea il task che legge e stampa i dati */
    xTaskCreate(
        plotter_task,           /* Funzione task */
        "plotter_task",         /* Nome task */
        2048,                   /* Stack size (byte) */
        (void *)sensor,         /* Argomento task */
        1,                      /* Priorità */
        NULL                    /* Handle */
    );

    /* Il loop principale di FreeRTOS prende il controllo da qui */
}
