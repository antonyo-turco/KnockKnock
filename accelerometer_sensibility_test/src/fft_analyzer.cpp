#include "fft_analyzer.h"

FFTAnalyzer::FFTAnalyzer() : sampleCount(0) {
    fillReal = vReal[0];
    fillImag = vImag[0];
    procReal = vReal[1];
    procImag = vImag[1];
    
    // Initialize imaginary parts to zero
    for (int i = 0; i < FFT_SAMPLES; i++) {
        fillImag[i] = 0.0;
        procImag[i] = 0.0;
    }
}

void FFTAnalyzer::addSample(float value) {
    if (sampleCount < FFT_SAMPLES) {
        fillReal[sampleCount] = value;
        sampleCount++;
    }
}

bool FFTAnalyzer::isBufferFull() {
    return sampleCount >= FFT_SAMPLES;
}

int FFTAnalyzer::getSampleCount() {
    return sampleCount;
}

void FFTAnalyzer::swapBuffers() {
    double *tempReal = fillReal;
    double *tempImag = fillImag;
    fillReal = procReal;
    fillImag = procImag;
    procReal = tempReal;
    procImag = tempImag;
    sampleCount = 0;
}

FFTResult FFTAnalyzer::computeFFT() {
    // Buffer must be full before we compute
    if (sampleCount < FFT_SAMPLES) {
        FFTResult empty = {0, 0, 0, 0, 0, 0};
        return empty;
    }
    
    // Swap buffers: procReal/procImag now contain data to analyze
    swapBuffers();
    
    // Create FFT object and compute
    ArduinoFFT<double> fft = ArduinoFFT<double>(procReal, procImag, FFT_SAMPLES, FFT_SAMPLING_FREQ);
    
    // Apply Hamming window
    fft.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
    
    // Compute FFT
    fft.compute(FFT_FORWARD);
    
    // Convert complex to magnitude
    fft.complexToMagnitude();
    
    // Now procReal contains magnitudes, get top 3
    FFTResult result = getTop3Frequencies(procReal);
    
    return result;
}

void FFTAnalyzer::resetBuffer() {
    sampleCount = 0;
    for (int i = 0; i < FFT_SAMPLES; i++) {
        fillImag[i] = 0.0;
    }
}

float FFTAnalyzer::getMagnitudeAtBin(uint16_t bin) {
    if (bin >= FFT_SAMPLES / 2) return 0.0;
    return (float)procReal[bin];
}

FFTResult FFTAnalyzer::getTop3Frequencies(double *magnitudes) {
    FFTResult result = {0, 0, 0, 0, 0, 0};
    
    // Find top 3 frequencies (skip DC component at bin 0)
    // Only search up to Nyquist frequency (FFT_SAMPLES / 2)
    int nyquist = FFT_SAMPLES / 2;
    
    double max1 = 0, max2 = 0, max3 = 0;
    int bin1 = 0, bin2 = 0, bin3 = 0;
    
    for (int i = 1; i < nyquist; i++) {
        double mag = magnitudes[i];
        
        if (mag > max1) {
            max3 = max2; bin3 = bin2;
            max2 = max1; bin2 = bin1;
            max1 = mag; bin1 = i;
        } else if (mag > max2) {
            max3 = max2; bin3 = bin2;
            max2 = mag; bin2 = i;
        } else if (mag > max3) {
            max3 = mag; bin3 = i;
        }
    }
    
    // Convert bins to frequencies
    result.f1 = (float)bin1 * FFT_SAMPLING_FREQ / FFT_SAMPLES;
    result.mag1 = (float)max1;
    
    result.f2 = (float)bin2 * FFT_SAMPLING_FREQ / FFT_SAMPLES;
    result.mag2 = (float)max2;
    
    result.f3 = (float)bin3 * FFT_SAMPLING_FREQ / FFT_SAMPLES;
    result.mag3 = (float)max3;
    
    return result;
}
