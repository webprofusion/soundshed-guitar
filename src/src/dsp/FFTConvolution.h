#pragma once

#include <vector>
#include <complex>
#include <algorithm>
#include <cmath>
#include <memory>

// KFR DFT forward declarations
namespace kfr { template<typename T> class dft_plan; }

namespace namguitar
{
  // FFT-based convolution using partitioned overlap-add method
  // This provides O(N log N) complexity with reduced computation overhead
  // by pre-computing IR FFT and using multiple partitions for large IRs
  // Uses KFR library for high-performance FFT
  class FFTConvolution
  {
  public:
    FFTConvolution();
    ~FFTConvolution();

    // Non-copyable, movable
    FFTConvolution(const FFTConvolution&) = delete;
    FFTConvolution& operator=(const FFTConvolution&) = delete;
    FFTConvolution(FFTConvolution&&) noexcept;
    FFTConvolution& operator=(FFTConvolution&&) noexcept;

    // Initialize with impulse response
    // irSamples: the impulse response to convolve with
    // blockSize: expected processing block size for optimization
    void SetImpulse(const std::vector<float>& irSamples, int blockSize);

    // Process audio through convolution
    // input: input samples
    // output: output samples (will be resized if needed)
    void Process(const std::vector<double>& input, std::vector<double>& output);

    // Reset internal state (history buffers)
    void Reset();

    [[nodiscard]] bool IsInitialized() const noexcept { return !mImpulse.empty(); }
    [[nodiscard]] bool IsOptimized() const noexcept { return mUsePartitionedConvolution; }

  private:
    std::vector<float> mImpulse;
    std::vector<double> mOverlapBuffer;  // Stores overlap from previous block
    int mBlockSize = 0;
    size_t mFFTSize = 0;
    
    // KFR DFT plan for efficient FFT computation
    std::unique_ptr<kfr::dft_plan<double>> mDFTPlan;
    
    // Pre-computed IR in frequency domain
    std::vector<std::complex<double>> mIRFFT;
    
    // Working buffers for FFT processing
    std::vector<std::complex<double>> mInputBuffer;
    std::vector<std::complex<double>> mOutputBuffer;
    std::vector<uint8_t> mTempBuffer;  // Temporary buffer for KFR
    
    // Partitioned convolution for large IRs
    bool mUsePartitionedConvolution = false;
    std::vector<std::vector<std::complex<double>>> mIRPartitions;  // Pre-computed FFT partitions
    std::vector<std::vector<double>> mInputHistory;  // History for each partition
    std::size_t mPartitionSize = 0;
    std::size_t mNumPartitions = 0;
    
    // Get next power of 2
    static size_t NextPowerOf2(size_t n);
  };

} // namespace namguitar
