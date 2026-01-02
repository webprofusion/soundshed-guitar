#pragma once

#include <vector>
#include <memory>
#include <cstddef>
#include <complex>

namespace namguitar
{
  // High-performance convolver using PocketFFT for real-time audio IR processing
  class OptimizedConvolver
  {
  public:
    OptimizedConvolver();
    ~OptimizedConvolver();

    // Initialize with impulse response
    void SetImpulse(const std::vector<float>& irSamples, int blockSize);

    // Process audio through convolution
    void Process(const std::vector<double>& input, std::vector<double>& output);

    // Reset internal state
    void Reset();

    [[nodiscard]] bool IsInitialized() const noexcept { return mInitialized; }
    [[nodiscard]] bool IsUsingFFT() const noexcept { return mUseFFT; }

  private:
    // PocketFFT-based convolution
    void InitializeFFT(const std::vector<float>& irSamples, int blockSize);
    void ProcessFFT(const std::vector<double>& input, std::vector<double>& output);

    // Fallback simple convolution for very small IRs
    void ProcessDirect(const std::vector<double>& input, std::vector<double>& output);

    bool mInitialized = false;
    bool mUseFFT = false;
    
    std::vector<float> mImpulse;
    std::vector<double> mOverlapBuffer;
    int mBlockSize = 0;
    size_t mFFTSize = 0;

    // PocketFFT buffers
    std::vector<std::complex<double>> mInputBuffer;
    std::vector<std::complex<double>> mOutputBuffer;
    std::vector<std::complex<double>> mIRFreqDomain;  // Pre-computed IR in frequency domain
  };

} // namespace namguitar
