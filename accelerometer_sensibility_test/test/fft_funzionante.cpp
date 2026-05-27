#include <Arduino.h>
#include <arduinoFFT.h>

#define ADC_PIN 7 // V4 ADC pin range on GPIO1-7
#define SAMPLES 128
#define SAMPLING_FREQUENCY 128

static double vReal0[SAMPLES], vImag0[SAMPLES];
static double vReal1[SAMPLES], vImag1[SAMPLES];
double *fillReal = vReal0, *fillImag = vImag0;
double *procReal = vReal0, *procImag = vImag0;

volatile int sampleIdx = 0;
SemaphoreHandle_t xBufferSemaphore;
hw_timer_t *timer = NULL;

void displayDataScreen();

void IRAM_ATTR onTimer() {
    fillReal[sampleIdx] = (double)analogRead(ADC_PIN);
    // For serial plotter
    Serial.printf(">fillReal:%f\r\n", fillReal[sampleIdx]);

    fillImag[sampleIdx] = 0.0;
    sampleIdx++;

    if (sampleIdx >= SAMPLES) {
        sampleIdx = 0;
        procReal = fillReal;
        procImag = fillImag;
        fillReal = (fillReal == vReal0) ? vReal1 : vReal0;
        fillImag = (fillImag == vImag0) ? vImag1 : vImag0;

        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        xSemaphoreGiveFromISR(xBufferSemaphore, &xHigherPriorityTaskWoken);
        if (xHigherPriorityTaskWoken) portYIELD_FROM_ISR();
    }
}

void TaskFFT(void *pvParameters) {
    ArduinoFFT<double> FFT = ArduinoFFT<double>(procReal, procImag, SAMPLES, SAMPLING_FREQUENCY);

    while (true) {
        if (xSemaphoreTake(xBufferSemaphore, portMAX_DELAY) == pdTRUE) {
            double mean = 0;
            for (int i = 0; i < SAMPLES; i++) {
                mean += procReal[i];
            }
            mean /= SAMPLES;
            for (int i = 0; i < SAMPLES; i++) {
                procReal[i] -= mean;
            }

            FFT = ArduinoFFT<double>(procReal, procImag, SAMPLES, SAMPLING_FREQUENCY);
            FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
            FFT.compute(FFT_FORWARD);
            FFT.complexToMagnitude();

            double peak = FFT.majorPeak();
            Serial.printf(">Max frequency:%.1f\r\n", peak);
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("FFT receiver starting...");
    Serial.print("Using ADC pin: ");
    Serial.println(ADC_PIN);
    Serial.printf("Samples=%d, Fs=%d Hz\n", SAMPLES, SAMPLING_FREQUENCY);

    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    xBufferSemaphore = xSemaphoreCreateBinary();

    timer = timerBegin(0, 80, true);
    timerAttachInterrupt(timer, &onTimer, true);
    timerAlarmWrite(timer, 1000000 / SAMPLING_FREQUENCY, true);
    timerAlarmEnable(timer);

    xTaskCreatePinnedToCore(TaskFFT, "FFT", 10000, NULL, 1, NULL, 0);
}

void loop() {
    vTaskDelay(portMAX_DELAY);
}
