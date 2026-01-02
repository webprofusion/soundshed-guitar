#pragma once

#include <vector>
#include <complex>
#include <algorithm>
#include <cmath>

namespace namguitar
{
  // FFT-based convolution using partitioned overlap-add method
  // This provides O(N log N) complexity with reduced computation overhead
  // by pre-computing IR FFT and using multiple partitions for large IRs
  class FFTConvolution
  {
  public:
    FFTConvolution() = default;
    ~FFTConvolution() = default;

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
    // Simple FFT implementation using radix-2 Cooley-Tukey algorithm
    void FFT(std::vector<std::complex<double>>& data, bool inverse);
    
    // Get next power of 2
    static size_t NextPowerOf2(size_t n);

    std::vector<float> mImpulse;
    std::vector<double> mOverlapBuffer;  // Stores overlap from previous block
    int mBlockSize = 0;
    size_t mFFTSize = 0;
    
    // Optimization: pre-computed IR FFT to avoid recomputing each frame
    std::vector<std::complex<double>> mIRFFT;
    
    // Partitioned convolution for large IRs
    bool mUsePartitionedConvolution = false;
    std::vector<std::vector<std::complex<double>>> mIRPartitions;  // Pre-computed FFT partitions
    std::vector<std::vector<double>> mInputHistory;  // History for each partition
    std::size_t mPartitionSize = 0;
    std::size_t mNumPartitions = 0;
  };

} // namespace namguitar
