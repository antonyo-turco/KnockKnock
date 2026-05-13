#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <arduinoFFT.h>
#include "accel_driver.h"

// ============================================================================
// CONFIGURATION
// ============================================================================
#define SAMPLING_FREQ_HZ 100
#define SAMPLING_PERIOD_US (1000000 / SAMPLING_FREQ_HZ)  // 10000 µs = 10 ms
#define FFT_WINDOW_SAMPLES 512  // MUST BE POWER OF 2 for ArduinoFFT

// ============================================================================
// GLOBAL STATE: Double Buffering for Real and Imaginary
// ============================================================================
AccelDriver accel;

// Static allocation: 4 buffers (2 for real, 2 for imaginary)
double vReal0[FFT_WINDOW_SAMPLES], vImag0[FFT_WINDOW_SAMPLES];
double vReal1[FFT_WINDOW_SAMPLES], vImag1[FFT_WINDOW_SAMPLES];

// Control pointers (sampling fills these, FFT processes these)
double *fillReal = vReal0;
double *fillImag = vImag0;
double *procReal = vReal1;
double *procImag = vImag1;

volatile int fillIndex = 0;

// FreeRTOS synchronization
SemaphoreHandle_t xFFTReady = NULL;      // Signals FFT task: buffer is full
SemaphoreHandle_t xFFTFinished = NULL;   // Signals Sampler: FFT is done

// Parameters (adjustable via Serial)
volatile float sensitivityRange = 4.0f;
volatile int fftCounter = 0;

// ============================================================================
// UTILITY: Parse Serial Commands
// ============================================================================
void handleSerialCommand(String cmd) {
    cmd.trim();
    
    if (cmd.startsWith("#SET_SENS")) {
        float range = atof(cmd.substring(9).c_str());
        if (range == 2 || range == 4 || range == 8 || range == 16) {
            accel.setSensitivityRange((uint8_t)range);
            sensitivityRange = range;
        } else {
            Serial.println("ERROR: Range must be 2, 4, 8, or 16 g");
        }
    }
    else if (cmd.startsWith("#GET_STATUS")) {
        Serial.printf("=== STATUS ===\n");
        Serial.printf("Sensitivity: ±%.0f g\n", sensitivityRange);
        Serial.printf("Sampling: %d Hz\n", SAMPLING_FREQ_HZ);
        Serial.printf("Nyquist Freq: %.1f Hz\n", SAMPLING_FREQ_HZ / 2.0f);
        Serial.printf("FFT Window: %d samples = %.1f sec\n", FFT_WINDOW_SAMPLES, (float)FFT_WINDOW_SAMPLES / SAMPLING_FREQ_HZ);
        Serial.printf("FFT Results: %d windows processed\n", fftCounter);
    }
}

// ============================================================================
// TASK: Accelerometer Sampler (Core 1, Priority 5)
// ============================================================================
void TaskSampler(void *pvParameters) {
    int64_t next_waketime = esp_timer_get_time();
    
    Serial.println("TaskSampler: Starting...");
    
    // ========================================================================
    // HIGH-PASS FILTER (IIR 1st order)
    // Removes DC/gravity (0 Hz), passes vibrations
    // fc = 10 Hz cutoff frequency, fs = 100 Hz sampling frequency
    // alpha = 2*pi*fc / (2*pi*fc + fs) = 62.83 / 690.83 ≈ 0.091
    // ========================================================================
    static double prev_input = 0.0;
    static double prev_output = 0.0;
    const double alpha = 0.091;  // IIR coefficient @ 10 Hz cutoff
    
    // Initially, signal that FFT is "finished" (so first swap can happen)
    xSemaphoreGive(xFFTFinished);
    
    while (1) {
        // Read accelerometer sample
        AccelSample sample = accel.readSample();
        
        // Calculate magnitude (sqrt(x² + y² + z²))
        float magnitude = sqrt((float)sample.x * sample.x + 
                              (float)sample.y * sample.y + 
                              (float)sample.z * sample.z);
        
        // ====================================================================
        // Apply HIGH-PASS FILTER to remove DC (gravity)
        // y[n] = alpha * (y[n-1] + x[n] - x[n-1])
        // ====================================================================
        double filtered = alpha * (prev_output + magnitude - prev_input);
        prev_input = magnitude;
        prev_output = filtered;
        
        // Add filtered data to buffer
        fillReal[fillIndex] = filtered;
        fillImag[fillIndex] = 0.0;
        
        fillIndex++;
        
        // Check if buffer is full
        if (fillIndex >= FFT_WINDOW_SAMPLES) {
            // Wait for FFT task to finish processing previous buffer
            // This is the critical handshake
            if (xSemaphoreTake(xFFTFinished, portMAX_DELAY) == pdTRUE) {
                // Swap buffers
                double *tempReal = fillReal;
                double *tempImag = fillImag;
                fillReal = procReal;
                fillImag = procImag;
                procReal = tempReal;
                procImag = tempImag;
                
                fillIndex = 0;
                
                // Signal FFT task: new data ready
                xSemaphoreGive(xFFTReady);
            }
        }
        
        // Precise timing: sleep until next sample
        next_waketime += SAMPLING_PERIOD_US;
        int64_t sleep_time = next_waketime - esp_timer_get_time();
        if (sleep_time > 0) {
            delayMicroseconds(sleep_time);
        } else {
            // If we're behind, just yield and move on
            vTaskDelay(1);
        }
    }
}

// ============================================================================
// TASK: FFT Analyzer (Core 0, Priority 1)
// ============================================================================
void TaskFFT(void *pvParameters) {
    Serial.println("TaskFFT: Starting...");
    
    while (1) {
        // Wait for sampler to signal buffer full
        if (xSemaphoreTake(xFFTReady, portMAX_DELAY) == pdTRUE) {
            // Compute FFT on procReal/procImag
            ArduinoFFT<double> fft(procReal, procImag, FFT_WINDOW_SAMPLES, SAMPLING_FREQ_HZ);
            
            // Apply Hamming window
            fft.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
            
            // Compute FFT
            fft.compute(FFT_FORWARD);
            
            // Convert complex to magnitude
            fft.complexToMagnitude();
            
            // Find top 5 frequencies
            double max1 = 0, max2 = 0, max3 = 0, max4 = 0, max5 = 0;
            int bin1 = 0, bin2 = 0, bin3 = 0, bin4 = 0, bin5 = 0;
            int nyquist = FFT_WINDOW_SAMPLES / 2;
            
            for (int i = 1; i < nyquist; i++) {
                double mag = procReal[i];
                
                if (mag > max1) {
                    max5 = max4; bin5 = bin4;
                    max4 = max3; bin4 = bin3;
                    max3 = max2; bin3 = bin2;
                    max2 = max1; bin2 = bin1;
                    max1 = mag; bin1 = i;
                } else if (mag > max2) {
                    max5 = max4; bin5 = bin4;
                    max4 = max3; bin4 = bin3;
                    max3 = max2; bin3 = bin2;
                    max2 = mag; bin2 = i;
                } else if (mag > max3) {
                    max5 = max4; bin5 = bin4;
                    max4 = max3; bin4 = bin3;
                    max3 = mag; bin3 = i;
                } else if (mag > max4) {
                    max5 = max4; bin5 = bin4;
                    max4 = mag; bin4 = i;
                } else if (mag > max5) {
                    max5 = mag; bin5 = i;
                }
            }
            
            // Convert bins to frequencies
            float f1 = (float)bin1 * SAMPLING_FREQ_HZ / FFT_WINDOW_SAMPLES;
            float f2 = (float)bin2 * SAMPLING_FREQ_HZ / FFT_WINDOW_SAMPLES;
            float f3 = (float)bin3 * SAMPLING_FREQ_HZ / FFT_WINDOW_SAMPLES;
            float f4 = (float)bin4 * SAMPLING_FREQ_HZ / FFT_WINDOW_SAMPLES;
            float f5 = (float)bin5 * SAMPLING_FREQ_HZ / FFT_WINDOW_SAMPLES;
            
            // Print FFT results (gravity removed via high-pass filter!)
            Serial.printf("FFT_RES: %.2f,%.1f %.2f,%.1f %.2f,%.1f %.2f,%.1f %.2f,%.1f\n", 
                         f1, (float)max1, f2, (float)max2, f3, (float)max3, 
                         f4, (float)max4, f5, (float)max5);
            
            fftCounter++;
            
            // Signal sampler: FFT finished, ready for next buffer swap
            xSemaphoreGive(xFFTFinished);
        }
    }
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n=== ACCELEROMETER SENSIBILITY TEST ===");
    Serial.printf("Sampling @ %d Hz (Nyquist: %.1f Hz)\n", SAMPLING_FREQ_HZ, SAMPLING_FREQ_HZ / 2.0f);
    Serial.println("High-pass filtered @ 10 Hz (removes gravity)");
    Serial.printf("FFT window: %d samples = %.1f sec\n", FFT_WINDOW_SAMPLES, (float)FFT_WINDOW_SAMPLES / SAMPLING_FREQ_HZ);
    Serial.println("FFT top 5 frequencies every window");
    Serial.println("Commands: #SET_SENS <2|4|8|16> | #GET_STATUS\n");
    
    // Initialize accelerometer
    if (!accel.initialize()) {
        Serial.println("ERROR: Accelerometer initialization failed!");
        while (1) delay(1000);
    }
    
    // Create semaphores
    xFFTReady = xSemaphoreCreateBinary();
    xFFTFinished = xSemaphoreCreateBinary();
    
    if (xFFTReady == NULL || xFFTFinished == NULL) {
        Serial.println("ERROR: Could not create semaphores!");
        while (1) delay(1000);
    }
    
    // Initialize imaginary arrays to zero
    for (int i = 0; i < FFT_WINDOW_SAMPLES; i++) {
        vImag0[i] = 0.0;
        vImag1[i] = 0.0;
    }
    
    // Create FreeRTOS tasks with adequate stack size
    // TaskSampler on Core 1 (high priority 5)
    BaseType_t ret1 = xTaskCreatePinnedToCore(
        TaskSampler,
        "Sampler",
        4096,
        NULL,
        5,
        NULL,
        1
    );
    
    // TaskFFT on Core 0 (low priority 1) - increased stack for FFT
    BaseType_t ret2 = xTaskCreatePinnedToCore(
        TaskFFT,
        "FFT",
        16384,  // Increased stack for FFT computations
        NULL,
        1,
        NULL,
        0
    );
    
    if (ret1 != pdPASS || ret2 != pdPASS) {
        Serial.println("ERROR: Could not create tasks!");
        while (1) delay(1000);
    }
    
    Serial.println("✓ Tasks started\n");
}

// ============================================================================
// MAIN LOOP: Serial command handler (non-blocking)
// ============================================================================
void loop() {
    // Check for incoming Serial commands
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        handleSerialCommand(cmd);
    }
    
    delay(10);
}
