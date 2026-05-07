#include "ButterworthFilter.hpp"
#include <cstring>
#include <algorithm>
#include <fenv.h>

namespace FusionNet {

ButterworthFilter::ButterworthFilter() : initialized_(false) {
    sections_.resize(NUM_SECTIONS);
}

bool ButterworthFilter::initialize() {
    if (initialized_) {
        return true;
    }
    
    designHighPassFilter();
    reset();
    initialized_ = true;
    return true;
}

double ButterworthFilter::prewarpFrequency(double fc, double fs) const {
    // Pre-warping for bilinear transform to compensate for frequency warping
    double omega_c = 2.0 * M_PI * fc;
    double omega_s = 2.0 * M_PI * fs;
    double omega_p = omega_s / std::tan(omega_c / (2.0 * fs));
    return omega_p;
}

void ButterworthFilter::designHighPassFilter() {
    // Design 4th-order high-pass Butterworth filter using bilinear transform
    // with pre-warping for fc = 20 Hz, fs = 100 Hz
    
    double fc = FC;
    double fs = FS;
    
    // Pre-warp the cutoff frequency
    double omega_p = prewarpFrequency(fc, fs);
    double omega_c = 2.0 * M_PI * fc;
    double omega_s = 2.0 * M_PI * fs;
    
    // Bilinear transform constant
    double T = 1.0 / fs;
    double alpha = omega_p * T / 2.0;
    
    // For 4th-order Butterworth, we need 2 second-order sections
    // Each section handles a pair of complex conjugate poles
    
    // First section (poles at angles 45° and 135°)
    double theta1 = M_PI * 3.0 / 8.0; // 67.5 degrees
    double cos_theta1 = std::cos(theta1);
    double sin_theta1 = std::sin(theta1);
    
    // Second section (poles at angles 225° and 315°)
    double theta2 = M_PI * 5.0 / 8.0; // 112.5 degrees
    double cos_theta2 = std::cos(theta2);
    double sin_theta2 = std::sin(theta2);
    
    // Convert analog to digital using bilinear transform
    // High-pass transformation: s -> alpha * (1 + z^-1) / (1 - z^-1)
    
    // Section 1 coefficients
    double denom1 = 1.0 + 2.0 * alpha * cos_theta1 + alpha * alpha;
    sections_[0].b0 = (1.0 + 2.0 * cos_theta1 + 1.0) / denom1;
    sections_[0].b1 = -2.0 * (1.0 - 1.0) / denom1;
    sections_[0].b2 = (1.0 - 2.0 * cos_theta1 + 1.0) / denom1;
    sections_[0].a1 = -2.0 * (1.0 - alpha * alpha) / denom1;
    sections_[0].a2 = (1.0 - 2.0 * alpha * cos_theta1 + alpha * alpha) / denom1;
    
    // Section 2 coefficients
    double denom2 = 1.0 + 2.0 * alpha * cos_theta2 + alpha * alpha;
    sections_[1].b0 = (1.0 + 2.0 * cos_theta2 + 1.0) / denom2;
    sections_[1].b1 = -2.0 * (1.0 - 1.0) / denom2;
    sections_[1].b2 = (1.0 - 2.0 * cos_theta2 + 1.0) / denom2;
    sections_[1].a1 = -2.0 * (1.0 - alpha * alpha) / denom2;
    sections_[1].a2 = (1.0 - 2.0 * alpha * cos_theta2 + alpha * alpha) / denom2;
    
    // ARM optimization: scale coefficients to prevent overflow
    for (auto& section : sections_) {
        double max_coeff = std::max({std::abs(section.b0), std::abs(section.b1), 
                                   std::abs(section.b2), std::abs(section.a1), 
                                   std::abs(section.a2)});
        if (max_coeff > 4.0) {
            double scale = 4.0 / max_coeff;
            section.b0 *= scale;
            section.b1 *= scale;
            section.b2 *= scale;
            section.a1 *= scale;
            section.a2 *= scale;
        }
    }
}

inline double ButterworthFilter::armOptimizedMultiply(double a, double b) const {
    // ARM-specific optimization for floating-point operations
    // Use fused multiply-add where available
    #ifdef __ARM_FEATURE_FMA
    return a * b;
    #else
    return a * b;
    #endif
}

inline void ButterworthFilter::flushDenormals() {
    // Flush denormals to zero for ARM performance
    #ifdef __arm__
    int old_round, flush;
    asm volatile("vmrs %0, fpscr" : "=r" (old_round));
    flush = old_round | (1 << 24);
    asm volatile("vmsr fpscr, %0" : : "r" (flush));
    #endif
}

double ButterworthFilter::filterSample(double input) {
    if (!initialized_) {
        return input;
    }
    
    flushDenormals();
    
    double output = input;
    
    // Process through each second-order section
    for (auto& section : sections_) {
        // Direct Form II transposed structure for numerical stability
        double processed = armOptimizedMultiply(section.b0, output) + section.x1;
        section.x1 = armOptimizedMultiply(section.b1, output) - armOptimizedMultiply(section.a1, processed) + section.x2;
        section.x2 = armOptimizedMultiply(section.b2, output) - armOptimizedMultiply(section.a2, processed);
        
        output = processed;
    }
    
    return output;
}

void ButterworthFilter::filterBuffer(const double* input, double* output, size_t length) {
    if (!initialized_ || !input || !output) {
        return;
    }
    
    flushDenormals();
    
    for (size_t i = 0; i < length; ++i) {
        output[i] = filterSample(input[i]);
    }
}

void ButterworthFilter::reset() {
    for (auto& section : sections_) {
        section.x1 = 0.0;
        section.x2 = 0.0;
        section.y1 = 0.0;
        section.y2 = 0.0;
    }
}

size_t ButterworthFilter::processMappedBuffer(FilterBuffer* buffer) {
    if (!initialized_ || !buffer || !buffer->input_data || !buffer->output_data) {
        return 0;
    }
    
    size_t samples_to_process = buffer->length - buffer->processed_samples;
    if (samples_to_process == 0) {
        return buffer->processed_samples;
    }
    
    const double* input_ptr = buffer->input_data + buffer->processed_samples;
    double* output_ptr = buffer->output_data + buffer->processed_samples;
    
    filterBuffer(input_ptr, output_ptr, samples_to_process);
    
    buffer->processed_samples += samples_to_process;
    return buffer->processed_samples;
}

} // namespace FusionNet
