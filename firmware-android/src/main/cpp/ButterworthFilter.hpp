#ifndef BUTTERWORTH_FILTER_HPP
#define BUTTERWORTH_FILTER_HPP

#include <vector>
#include <memory>
#include <cmath>

namespace FusionNet {

struct SecondOrderSection {
    double b0, b1, b2;
    double a1, a2;
    double x1, x2, y1, y2;
    
    SecondOrderSection() : b0(1.0), b1(0.0), b2(0.0), a1(0.0), a2(0.0), 
                          x1(0.0), x2(0.0), y1(0.0), y2(0.0) {}
};

class ButterworthFilter {
private:
    static constexpr int ORDER = 4;
    static constexpr int NUM_SECTIONS = ORDER / 2;
    static constexpr double FC = 20.0;  // Cut-off frequency Hz
    static constexpr double FS = 100.0; // Sampling frequency Hz
    
    std::vector<SecondOrderSection> sections_;
    bool initialized_;
    
    // ARM optimization: use fixed-point arithmetic for stability
    static constexpr double FIXED_POINT_SCALE = 65536.0;
    
    // Pre-warping for bilinear transform
    double prewarpFrequency(double fc, double fs) const;
    
    // Design 4th-order high-pass Butterworth filter
    void designHighPassFilter();
    
    // ARM-specific optimizations
    inline double armOptimizedMultiply(double a, double b) const;
    inline void flushDenormals();

public:
    ButterworthFilter();
    ~ButterworthFilter() = default;
    
    // Initialize filter coefficients
    bool initialize();
    
    // Process single sample
    double filterSample(double input);
    
    // Process buffer of samples
    void filterBuffer(const double* input, double* output, size_t length);
    
    // Reset filter state
    void reset();
    
    // Get filter specifications
    double getCutOffFrequency() const { return FC; }
    double getSamplingFrequency() const { return FS; }
    int getOrder() const { return ORDER; }
    
    // Memory-mapped buffer support for JNI
    struct FilterBuffer {
        double* input_data;
        double* output_data;
        size_t length;
        size_t processed_samples;
        
        FilterBuffer() : input_data(nullptr), output_data(nullptr), 
                        length(0), processed_samples(0) {}
    };
    
    // Process memory-mapped buffer
    size_t processMappedBuffer(FilterBuffer* buffer);
};

} // namespace FusionNet

#endif // BUTTERWORTH_FILTER_HPP
