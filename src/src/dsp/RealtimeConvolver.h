#pragma once

#include <vector>
#include <complex>
#include <memory>
#include <cstddef>

namespace namguitar { class SimpleFFT; }

namespace namguitar
{
  /**
   * Real-time convolver using Uniformly Partitioned Overlap-Save (UPOLS) algorithm.
   * 
   * Optimized for low-latency real-time audio:
   * - Fixed-size FFT blocks regardless of IR length
   * - All buffers pre-allocated (zero allocations in audio thread)
   * - Partitioned convolution for O(B log B) per-block complexity
   * - Latency = partition size (typically 256-512 samples)
   */
  class RealtimeConvolver
  {
  public:
    RealtimeConvolver();
    ~RealtimeConvolver();

    // Non-copyable, movable
    RealtimeConvolver(const RealtimeConvolver&) = delete;
    RealtimeConvolver& operator=(const RealtimeConvolver&) = delete;
    RealtimeConvolver(RealtimeConvolver&&) noexcept;
    RealtimeConvolver& operator=(RealtimeConvolver&&) noexcept;

    /**
     * Initialize with impulse response.
     * @param irSamples The impulse response samples
     * @param blockSize Expected processing block size (determines partition size)
     * @return true if initialization succeeded
     */
    bool SetImpulse(const std::vector<float>& irSamples, int blockSize);

    /**
     * Process audio samples through the convolver.
     * ZERO heap allocations in this path - all buffers pre-allocated.
     */
    void Process(const double* input, double* output, int numSamples);

    /**
     * Reset internal state (clears all buffers).
     */
    void Reset();

    [[nodiscard]] bool IsInitialized() const noexcept { return mInitialized; }
    [[nodiscard]] int GetLatency() const noexcept { return static_cast<int>(mPartitionSize); }
    [[nodiscard]] size_t GetNumPartitions() const noexcept { return mNumPartitions; }

  private:
    void ProcessBlock();

    bool mInitialized = false;
    
    // Partition configuration
    size_t mPartitionSize = 0;
    size_t mFFTSize = 0;
    size_t mNumPartitions = 0;
    
    // IR in frequency domain (pre-computed at SetImpulse time)
    std::vector<std::vector<std::complex<double>>> mIRPartitionsFFT;
    
    // Input delay line (frequency domain) - circular buffer
    std::vector<std::vector<std::complex<double>>> mInputFFTDelayLine;
    size_t mDelayLineIndex = 0;
    
    // Input/output sample buffering
    std::vector<double> mInputBuffer;
    std::vector<double> mOutputBuffer;
    size_t mInputBufferPos = 0;
    size_t mOutputBufferReadPos = 0;
    
    // Pre-allocated working buffers (NO allocations in ProcessBlock)
    std::vector<std::complex<double>> mFFTInputBuffer;   // Input to FFT
    std::vector<std::complex<double>> mFFTOutputBuffer;  // Output from FFT
    std::vector<std::complex<double>> mAccumulator;      // Freq domain accumulator
    
    // Overlap-add state
    std::vector<double> mOverlapBuffer;
    
    // FFT plan
    std::unique_ptr<SimpleFFT> mFFT;
  };

} // namespace namguitar
