#pragma once

#include <vector>
#include <complex>
#include <memory>
#include <cstddef>

namespace guitarfx
{
class SimdFFT;
}

namespace guitarfx
{
/**
 * Real-time convolver using Uniformly Partitioned Overlap-Save (UPOLS) algorithm.
 *
 * Optimized for low-latency real-time audio:
 * - Fixed-size FFT blocks regardless of IR length
 * - All buffers pre-allocated (zero allocations in audio thread)
 * - Partitioned convolution for O(B log B) per-block complexity
 * - Latency = partition size (typically 256-512 samples)
 *
 * Optional non-uniform (Gardner-style) partitioning, enabled via
 * SetLowLatencyMode(true) before SetImpulse(). In this mode the IR head is
 * convolved with small partitions (low latency) while the tail uses larger
 * partitions for efficiency. Latency drops to the small base block size
 * (e.g. 128) at the cost of some extra CPU. See RealtimeConvolver.cpp.
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
     * Enable/disable non-uniform (low-latency) partitioning. Must be called
     * BEFORE SetImpulse() to take effect; the mode is captured at SetImpulse()
     * time alongside the partition layout. Defaults to false (uniform).
     */
    void SetLowLatencyMode(bool enabled) noexcept
    {
        mLowLatencyMode = enabled;
    }

    [[nodiscard]] bool IsLowLatencyMode() const noexcept
    {
        return mLowLatencyMode;
    }

    /**
     * Process audio samples through the convolver.
     * ZERO heap allocations in this path - all buffers pre-allocated.
     */
    void Process(const float* input, float* output, int numSamples);

    /**
     * Reset internal state (clears all buffers).
     */
    void Reset();

    [[nodiscard]] bool IsInitialized() const noexcept
    {
        return mInitialized;
    }

    [[nodiscard]] int GetLatency() const noexcept
    {
        return mUseDirectConvolution ? 0 : static_cast<int>(mPartitionSize);
    }

    [[nodiscard]] size_t GetNumPartitions() const noexcept
    {
        return mNumPartitions;
    }

  private:
    void ProcessBlock();
    void ProcessDirect(const float* input, float* output, int numSamples);

    // Builds a uniformly-partitioned overlap-save engine with an explicit
    // partition size (power of two). Used both for the default uniform path and
    // for each stage of the non-uniform engine.
    bool BuildUniform(const std::vector<float>& irSamples, size_t partitionSize);

    // Builds and processes the non-uniform (Gardner-style) engine.
    bool BuildNonUniform(const std::vector<float>& irSamples, int blockSize);
    void ProcessNonUniform(const float* input, float* output, int numSamples);

    bool mInitialized = false;
    bool mUseDirectConvolution = false; // For short IRs, use direct FIR convolution (no latency)
    bool mLowLatencyMode = false;       // Requested at SetImpulse time (non-uniform partitioning)
    bool mIsNonUniform = false;         // True when the non-uniform engine is active

    // Direct convolution (for short IRs)
    std::vector<float> mDirectIR;
    std::vector<float> mDirectHistory; // Ring buffer for FIR filter state
    size_t mDirectHistoryPos = 0;
    static constexpr size_t kDirectConvolutionThreshold = 64; // Use direct convolution for IRs <= this length

    // Partition configuration
    size_t mPartitionSize = 0;
    size_t mFFTSize = 0;
    size_t mNumPartitions = 0;

    // IR in frequency domain (pre-computed at SetImpulse time)
    std::vector<std::vector<std::complex<float>>> mIRPartitionsFFT;

    // Input delay line (frequency domain) - circular buffer
    std::vector<std::vector<std::complex<float>>> mInputFFTDelayLine;
    size_t mDelayLineIndex = 0;

    // Input/output sample buffering
    std::vector<float> mInputBuffer;
    std::vector<float> mOutputBuffer;
    std::vector<float> mPreviousInputBlock; // Previous block for overlap-save
    size_t mInputBufferPos = 0;
    size_t mOutputBufferReadPos = 0;

    // Pre-allocated working buffers (NO allocations in ProcessBlock)
    std::vector<std::complex<float>> mFFTInputBuffer;  // Input to FFT
    std::vector<std::complex<float>> mFFTOutputBuffer; // Output from FFT
    std::vector<std::complex<float>> mAccumulator;     // Freq domain accumulator

    // FFT plan
    std::unique_ptr<SimdFFT> mFFT;

    // --- Non-uniform (Gardner-style) engine ---
    // mNonUniformStages[0] is the low-latency head segment (no extra delay).
    // Every other stage's contribution is delayed by exactly mBaseBlock samples
    // (a single shared delay line), which time-aligns all stages so their sum
    // equals the full convolution delayed by mBaseBlock. See RealtimeConvolver.cpp.
    std::vector<std::unique_ptr<RealtimeConvolver>> mNonUniformStages;
    size_t mBaseBlock = 0;              // latency of the non-uniform engine (samples)
    std::vector<float> mNuHeadScratch;  // output of stage 0 (no delay)
    std::vector<float> mNuRestScratch;  // accumulated output of stages 1..n-1
    std::vector<float> mNuStageScratch; // per-stage temporary
    std::vector<float> mNuDelayLine;    // delay ring (length mBaseBlock) for stages 1..n-1
    size_t mNuDelayPos = 0;
    size_t mNuScratchSize = 0; // max chunk size for the scratch buffers
};

} // namespace guitarfx
