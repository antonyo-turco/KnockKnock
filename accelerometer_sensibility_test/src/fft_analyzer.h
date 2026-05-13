#ifndef FFT_ANALYZER_H
#define FFT_ANALYZER_H

#include <Arduino.h>
#include <arduinoFFT.h>

#define FFT_SAMPLES 1024
#define FFT_SAMPLING_FREQ 512

struct FFTResult {
    float f1, mag1;
    float f2, mag2;
    float f3, mag3;
};

class FFTAnalyzer {
public:
    FFTAnalyzer();
    
    // Add sample to buffer
    void addSample(float value);
    
    // Check if buffer is full
    bool isBufferFull();
    
    // Get number of samples in buffer
    int getSampleCount();
    
    // Compute FFT on accumulated buffer
    FFTResult computeFFT();
    
    // Reset buffer
    void resetBuffer();
    
    // Get raw magnitude at specific frequency bin
    float getMagnitudeAtBin(uint16_t bin);

private:
    double vReal[2][FFT_SAMPLES];
    double vImag[2][FFT_SAMPLES];
    
    double *fillReal;
    double *fillImag;
    double *procReal;
    double *procImag;
    
    int sampleCount;
    
    void swapBuffers();
    FFTResult getTop3Frequencies(double *magnitudes);
};

#endif
