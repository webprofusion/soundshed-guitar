#pragma once

#include <vector>
#include <complex>
#include <memory>
#include <cstddef>

namespace guitarfx
{
class RealFFT;
}

namespace guitarfx
{
/**
 * Real-time convolver with no latency: non-uniform (Gardner-style) partitioning.
 *
 * The first 256 or 512 IR samples run as a direct-form FIR, and the rest as
 * uniformly partitioned overlap-save (UPOLS) FFT stages whose partitions grow with
 * their offset into the IR, so the output has no latency at all. An IR no longer
 * than the FIR runs on the FIR alone. All buffers are allocated by SetImpulse(),
 * none on the audio thread. See RealtimeConvolver.cpp.
 *
 * Each FFT stage spreads a block's partition multiplies across the samples of
 * that block rather than doing them all at the block boundary, so per-callback
 * cost stays flat even when the host block is much smaller than the partition.
 * A long IR's larger stages start at twice their partition, which leaves them a
 * partition of slack, and spend it spreading their FFTs too: each one computes a
 * block's output over the samples of the block after it.
 *
 * The input and the IR are real, so every stage runs real-input FFTs (RealFFT.h):
 * half a complex FFT's work, and half the spectrum bins to multiply and store.
 *
 * Until 2026-10 a Low Latency switch could run the whole IR as one uniform engine
 * instead, 256 to 2048 samples late. It saved little CPU on short IRs and cost more
 * on long ones (docs/plans/zero-latency-ir-cab.md), so it is gone.
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
     * Initialize with impulse response. The layout depends on the IR alone, not the host block,
     * and Process() takes blocks of any size.
     * @param irSamples The impulse response samples
     * @return true if initialization succeeded
     */
    bool SetImpulse(const std::vector<float>& irSamples);

    /**
     * Process audio samples through the convolver.
     * ZERO heap allocations in this path - all buffers pre-allocated.
     */
    void Process(const float* input, float* output, int numSamples);

    /**
     * Clears the history (input, output and delay-line buffers) and keeps the impulse, so
     * IsInitialized() stays true and the next Process() still convolves. To drop the impulse,
     * assign a default-constructed convolver.
     */
    void Reset();

    [[nodiscard]] bool IsInitialized() const noexcept
    {
        return mInitialized;
    }

    /**
     * Takes over `source`'s history, so this convolver carries on exactly as if it had processed
     * everything `source` did. For two convolvers built from the same impulse with the same
     * settings, as a mono IR's left and right are: the right can sit out while both would hear the
     * same input, and catch up here before it is needed again. Copies into storage already
     * allocated, so it is safe on the audio thread. False, with nothing changed, if the two are
     * not laid out alike.
     */
    bool CopyStateFrom(const RealtimeConvolver& source) noexcept;

    /// How many of the most recent input samples the history depends on, at most: two convolvers
    /// with the same impulse that have been fed the same input for this long hold the same
    /// history, bit for bit. 0 before an impulse is set.
    [[nodiscard]] size_t HistorySpan() const noexcept;

    [[nodiscard]] size_t GetNumPartitions() const noexcept
    {
        return mNumPartitions;
    }

  private:
    void ProcessBlock();

    [[nodiscard]] bool LayoutMatches(const RealtimeConvolver& other) const noexcept;
    void CopyStateUnchecked(const RealtimeConvolver& source) noexcept;

    // Direct-form FIR over `count` coefficients: the whole IR when it is short enough, or the
    // zero-latency engine's head. ProcessFir writes (not adds) its output.
    void BuildFir(const float* coefficients, size_t count);
    void ProcessFir(const float* input, float* output, size_t numSamples);

    // Folds IR partitions [mPreAccumPartition, target) into mAccumulator for the block
    // that is still filling. Called a slice at a time from Process() so a long IR costs
    // the same on every callback. See RealtimeConvolver.cpp.
    void AccumulatePartitionsUpTo(size_t target);

    // Builds a uniformly-partitioned overlap-save engine with an explicit partition size (power
    // of two), a partition late. Each FFT stage of the non-uniform engine is one of these.
    // `spread` makes it two partitions late, and computes each block's output over the samples
    // of the next block instead of all at once at its boundary.
    bool BuildUniform(const std::vector<float>& irSamples, size_t partitionSize, bool spread = false);

    // The spread engine (RealtimeConvolverSpread.cpp): at each boundary it finishes the block in
    // flight, plays it, and starts on the block that just closed; RunSpreadJobTo() moves that
    // work on as samples arrive. InitSpread() finishes BuildUniform for it.
    bool InitSpread();
    void ResetSpread();
    void ProcessSpreadBoundary();
    void RunSpreadJobTo(size_t target);
    void RunSpreadPhase(size_t phase, size_t begin, size_t end);

    // Builds and processes the zero-latency non-uniform (Gardner-style) engine.
    bool BuildNonUniform(const std::vector<float>& irSamples);
    void ProcessNonUniform(const float* input, float* output, int numSamples);

    bool mInitialized = false;
    bool mUseDirectConvolution = false; // The whole IR runs on the FIR
    bool mIsNonUniform = false;         // True when the zero-latency non-uniform engine is active

    // Direct-form FIR: the whole IR when it is no longer than the head, else the head of the
    // non-uniform engine.
    std::vector<float> mFirReversed; // h[count - 1] .. h[0], so each output is one dot product
    std::vector<float> mFirHistory;  // count - 1 samples of history, then one chunk of input
    static constexpr size_t kFirChunk = 256;

    // Partition configuration
    size_t mPartitionSize = 0;
    size_t mFFTSize = 0; // real FFT size, twice the partition
    size_t mNumBins = 0; // complex bins in a packed real spectrum (RealFFT.h): mFFTSize / 2
    size_t mNumPartitions = 0;

    // IR in frequency domain (pre-computed at SetImpulse time), packed real spectra
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

    // Freq domain accumulator, pre-allocated (NO allocations in ProcessBlock)
    std::vector<std::complex<float>> mAccumulator;

    // Accumulation schedule. mAccumulator is built up across the block rather than in
    // one burst at the block boundary: mPreAccumAnchor is the delay-line slot holding
    // the newest input frame at the start of the current block, and mPreAccumPartition
    // is the next IR partition still to be folded in.
    size_t mPreAccumAnchor = 0;
    size_t mPreAccumPartition = 0;

    // Spread engine (BuildUniform with `spread`). The block in flight is the frame
    // [mPreviousInputBlock | mSpreadCurrentBlock], stored in delay-line slot mJobSlot; its output
    // goes to mSpreadOutput, which becomes mOutputBuffer at the next boundary. mJobPhase and
    // mJobPos say how far through its phases (see RealtimeConvolver.cpp) the work is, and
    // mJobDone how many of its mJobCost cost units that is.
    bool mSpread = false;
    std::vector<float> mSpreadCurrentBlock;
    std::vector<float> mSpreadOutput;
    size_t mJobSlot = 0;
    size_t mJobPhase = 0;
    size_t mJobPos = 0;
    size_t mJobDone = 0;
    size_t mJobCost = 0;

    // FFT plan
    std::unique_ptr<RealFFT> mFFT;

    // --- Zero-latency non-uniform (Gardner-style) engine ---
    // The FIR above covers the IR's head. Each stage here is a uniform engine for the IR
    // segment starting at the offset equal to its own partition, so its block latency is
    // exactly the delay that segment needs, and the stages add straight into the FIR's
    // output. See RealtimeConvolver.cpp.
    std::vector<std::unique_ptr<RealtimeConvolver>> mNonUniformStages;
    std::vector<float> mNuRestScratch;  // accumulated output of the stages
    std::vector<float> mNuStageScratch; // per-stage temporary
};
} // namespace guitarfx
