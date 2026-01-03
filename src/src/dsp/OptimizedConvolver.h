#pragma once

#include <vector>
#include <memory>
#include <cstddef>
#include <complex>

// KFR DFT forward declarations
namespace kfr { template<typename T> class dft_plan; }

namespace namguitar
{
  // High-performance convolver using KFR for real-time audio IR processing
  class OptimizedConvolver
  {
  public:
    OptimizedConvolver();
    ~OptimizedConvolver();

    // Non-copyable, movable
    OptimizedConvolver(const OptimizedConvolver&) = delete;
    OptimizedConvolver& operator=(const OptimizedConvolver&) = delete;
    OptimizedConvolver(OptimizedConvolver&&) noexcept;
    OptimizedConvolver& operator=(OptimizedConvolver&&) noexcept;

    // Initialize with impulse response
    void SetImpulse(const std::vector<float>& irSamples, int blockSize);

    // Process audio through convolution
    void Process(const std::vector<double>& input, std::vector<double>& output);

    // Reset internal state
    void Reset();

    [[nodiscard]] bool IsInitialized() const noexcept { return mInitialized; }
    [[nodiscard]] bool IsUsingFFT() const noexcept { return mUseFFT; }

  private:
    // KFR-based convolution
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

    // KFR DFT plan
    std::unique_ptr<kfr::dft_plan<double>> mDFTPlan;

    // KFR buffers
    std::vector<std::complex<double>> mInputBuffer;
    std::vector<std::complex<double>> mOutputBuffer;
    std::vector<std::complex<double>> mIRFreqDomain;  // Pre-computed IR in frequency domain
    std::vector<uint8_t> mTempBuffer;  // Temporary buffer for KFR
  };

} // namespace namguitar
