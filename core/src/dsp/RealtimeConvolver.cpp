#include "RealtimeConvolver.h"
#include "FiniteCheck.h"
#include "RealFFT.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace guitarfx
{
namespace
{
constexpr size_t NextPowerOf2(size_t n)
{
    size_t power = 1;

    while (power < n)
    {
        power *= 2;
    }

    return power;
}

// Tail partition size for the non-uniform engine. That engine has no latency to keep
// down, so the tail is free to use a partition far larger than the host
// block -- and it has to be, because a partitioned convolution's per-sample cost is
// proportional to the partition COUNT. A 4.7 s plate reverb split at 256 samples is
// ~880 partitions and streams 28 KB of spectra per output sample, which is DRAM-bound;
// at 4096 it is ~55 partitions and 1.8 KB. Size the tail so the count stays near the
// target rather than tying it to the host block, which has nothing to do with the IR.
constexpr size_t kTargetTailPartitions = 64;

size_t TailPartitionForIR(size_t irLength)
{
    return std::clamp(NextPowerOf2(irLength / kTargetTailPartitions), size_t{256}, size_t{4096});
}

// FIR head length of the zero-latency engine: 256 taps, or 512 once the IR is long enough for a
// 512-sample tail partition. Measured on 480-40,000-sample IRs (docs/plans/zero-latency-ir-cab.md),
// a vectorised FIR of this length costs less than the 64- and 128-sample FFT partitions it
// replaces; of the heads tried (64 to 512), 256 was cheapest up to ~8,000 samples and 512 from
// ~24,000. The switch falls at 16,448 samples, where the tail partition reaches 512.
constexpr size_t kMaxFirHead = 512;

size_t ZeroLatencyHeadForIR(size_t irLength)
{
    return std::min(TailPartitionForIR(irLength), kMaxFirHead);
}

// The same guard ProcessBlock() puts on every FFT output sample, so a bad sample cannot reach
// the recursive effects downstream whichever engine runs.
float SanitizeOutput(float sample)
{
    return IsFinite(sample) ? std::clamp(sample, -100.0f, 100.0f) : 0.0f;
}

// The smallest tail partition for which the non-uniform engine spreads its stages' FFTs (see
// BuildNonUniform). An IR with a shorter tail (up to 32,768 samples, so every cab IR) keeps the
// layout it always had: its largest FFT is 1024 points, cheap enough to run at a boundary.
// Spreading only the stages from 1024 samples on, and leaving a 512 stage at its boundary, cost
// the same on average and left that stage's burst every 512 samples in the p90.
constexpr size_t kSpreadMinTailPartition = 1024;

// How much each spread stage's partition grows on the last: 16 takes the 256 first stage straight
// to a tail of up to 4096. Fewer, longer stages trade FFTs for multiply-accumulates, which
// vectorise better. Measured 2026-10-19 on 0.8-6 s IRs at 64-1024-sample blocks: doubling (the
// boundary layout's growth) cost 10-20% more than the boundary layout on x64 and 20-25% more on a
// Galaxy S21's A78; 16 costs 0-25% less than it on x64 and 0-23% less on the A78. 4 is between.
constexpr size_t kSpreadStageGrowth = 16;
} // namespace

RealtimeConvolver::RealtimeConvolver() = default;

RealtimeConvolver::~RealtimeConvolver() = default;

RealtimeConvolver::RealtimeConvolver(RealtimeConvolver&&) noexcept = default;

RealtimeConvolver& RealtimeConvolver::operator=(RealtimeConvolver&&) noexcept = default;

bool RealtimeConvolver::SetImpulse(const std::vector<float>& irSamples)
{
    mInitialized = false;
    mUseDirectConvolution = false;
    mIsNonUniform = false;
    mSpread = false;
    mNonUniformStages.clear();

    if (irSamples.empty())
    {
        mIRPartitionsFFT.clear();
        mInputFFTDelayLine.clear();
        mFFT.reset();
        mFirReversed.clear();
        mFirHistory.clear();
        return false;
    }

    // An IR no longer than the FIR head runs on the FIR alone, a longer one on the FIR plus
    // partitions that need no delay. Neither owes anything to the host block.
    if (irSamples.size() <= ZeroLatencyHeadForIR(irSamples.size()))
    {
        BuildFir(irSamples.data(), irSamples.size());
        mUseDirectConvolution = true;
        mInitialized = true;
        return true;
    }

    return BuildNonUniform(irSamples);
}

bool RealtimeConvolver::BuildUniform(const std::vector<float>& irSamples, size_t partitionSize, bool spread)
{
    mInitialized = false;
    mUseDirectConvolution = false;
    mIsNonUniform = false;
    mSpread = false;

    // The real FFT needs at least 4 points.
    if (irSamples.empty() || partitionSize < 2)
    {
        return false;
    }

    mPartitionSize = partitionSize;

    // FFT size is 2x partition for linear convolution. The input and the IR are real, so each
    // spectrum is half that many packed bins (RealFFT.h).
    mFFTSize = mPartitionSize * 2;
    mNumBins = mFFTSize / 2;

    // Calculate partitions needed
    mNumPartitions = (irSamples.size() + mPartitionSize - 1) / mPartitionSize;

    // Create FFT plan
    try
    {
        mFFT = std::make_unique<RealFFT>(mFFTSize);
    }
    catch (...)
    {
        return false;
    }

    mAccumulator.resize(mNumBins);

    // Pre-compute FFT of each IR partition: the partition, then as many zeros.
    mIRPartitionsFFT.resize(mNumPartitions);
    std::vector<float> segment(mPartitionSize);
    const std::vector<float> zeros(mPartitionSize, 0.0f);

    for (size_t p = 0; p < mNumPartitions; ++p)
    {
        const size_t irStart = p * mPartitionSize;
        const size_t irEnd = std::min(irStart + mPartitionSize, irSamples.size());
        std::fill(segment.begin(), segment.end(), 0.0f);
        std::copy(irSamples.begin() + static_cast<std::ptrdiff_t>(irStart),
                  irSamples.begin() + static_cast<std::ptrdiff_t>(irEnd), segment.begin());

        mIRPartitionsFFT[p].resize(mNumBins);
        mFFT->Forward(mIRPartitionsFFT[p].data(), segment.data(), zeros.data());
    }

    // Allocate delay line for input FFTs
    mInputFFTDelayLine.resize(mNumPartitions);

    for (auto& fft : mInputFFTDelayLine)
    {
        fft.assign(mNumBins, std::complex<float>(0.0f, 0.0f));
    }

    mDelayLineIndex = 0;

    // Allocate I/O buffers
    mInputBuffer.assign(mPartitionSize, 0.0f);
    mOutputBuffer.assign(mPartitionSize, 0.0f);
    mInputBufferPos = 0;
    mOutputBufferReadPos = mPartitionSize; // Start empty

    // Previous input block for overlap-save (needed for proper convolution)
    mPreviousInputBlock.assign(mPartitionSize, 0.0f);

    // The accumulator is carried across the block now, so it starts cleared with
    // nothing scheduled: the first ProcessBlock() has no older frames to fold in.
    std::fill(mAccumulator.begin(), mAccumulator.end(), std::complex<float>(0.0f, 0.0f));
    mPreAccumAnchor = 0;
    mPreAccumPartition = mNumPartitions;

    if (spread && !InitSpread())
    {
        return false;
    }

    mInitialized = true;
    return true;
}

// Zero-latency non-uniform (Gardner-style) partitioned convolution.
//
// The IR is split into contiguous segments. The first, h[0, head), runs as a
// direct-form FIR. After it come segments whose partition size grows
// geometrically from `head` up to the tail partition, each an independent
// single-block uniform convolver, and then the long tail as one multi-partition
// uniform engine.
//
// For a segment covering IR samples starting at offset `off` with partition P,
// its single-block convolver produces (h_seg * x)[n - P]. The output needs
// (h_seg * x)[n - off], so the stage wants an extra delay of off - P. The layout
// makes P == off for every segment (each starts where the last ended, at twice
// its offset, and the tail starts at exactly its own partition size), so no stage
// needs any delay, and the FIR plus all the stages is the full convolution with
// no latency. Only the head could not be done this way, since it would need P == 0.
//
// P == off leaves a stage no slack: the block that closes at a boundary is wanted
// from that same sample, so its forward and inverse FFTs run in one callback. With
// a 4096-sample tail, every 4096 samples all the stages close together, and that
// callback cost several times the median (2026-10, IR Reverb on a phone: p90 3-4x,
// max 7x). So once the tail partition reaches kSpreadMinTailPartition, every stage
// after the FIR is a spread engine instead: two partitions late, so it starts at
// twice its partition (off == 2P), and running to twice the next stage's partition
// so that one does too. A 3 s IR with a 4096 tail is then the FIR to 512, one
// 256-sample stage of 30 partitions to 8192, and the tail from there.
//
// Until 2026-10 the head was an FFT stage at a base block of 64-256 samples
// following the host block, and every other stage went through a shared delay
// line of that length, so the engine ran that many samples late. The FIR costs
// less than those small partitions did.
bool RealtimeConvolver::BuildNonUniform(const std::vector<float>& irSamples)
{
    mInitialized = false;
    mUseDirectConvolution = false;
    mIsNonUniform = false;
    mNonUniformStages.clear();

    const size_t n = irSamples.size();
    const size_t head = ZeroLatencyHeadForIR(n);
    // Never below `head`, which is how ZeroLatencyHeadForIR picks it: the layout relies on
    // the tail starting at exactly its own partition size, which the doubling from `head`
    // can only reach from underneath. Both are powers of two.
    const size_t maxPartition = TailPartitionForIR(n);

    // Build a uniform stage covering irSamples[off, off+len).
    auto addStage = [&](size_t off, size_t len, size_t partition, bool spread) -> bool {
        auto stage = std::make_unique<RealtimeConvolver>();
        std::vector<float> slice(irSamples.begin() + static_cast<std::ptrdiff_t>(off),
                                 irSamples.begin() + static_cast<std::ptrdiff_t>(std::min(off + len, n)));

        if (!stage->BuildUniform(slice, partition, spread))
        {
            return false;
        }

        mNonUniformStages.push_back(std::move(stage));
        return true;
    };

    // Head segment: the direct-form FIR, no latency.
    BuildFir(irSamples.data(), head);

    const bool spread = maxPartition >= kSpreadMinTailPartition;
    size_t off = head;

    if (spread)
    {
        // Spread segments, off == 2P, each running to twice the next one's partition so that
        // one starts at off == 2P too, until the partition reaches maxPartition.
        size_t partition = head / 2;

        while (off < n && partition < maxPartition)
        {
            const size_t nextPartition = std::min(partition * kSpreadStageGrowth, maxPartition);

            if (!addStage(off, 2 * nextPartition - off, partition, true))
            {
                return false;
            }

            off = 2 * nextPartition;
            partition = nextPartition;
        }
    }
    else
    {
        // Geometrically growing segments until the partition reaches maxPartition.
        while (off < n && off < maxPartition)
        {
            const size_t partition = off; // P == off: the stage's own latency is its offset

            if (!addStage(off, partition, partition, false))
            {
                return false;
            }

            off += partition;
        }
    }

    // Efficient tail: one multi-partition uniform engine for the remainder.
    if (off < n)
    {
        // off == maxPartition here, or 2 * maxPartition when spread, so the tail engine's own
        // latency is its offset and it needs no delay either.
        if (!addStage(off, n - off, maxPartition, spread))
        {
            return false;
        }
    }

    // Aggregate reporting + scratch buffers.
    mNumPartitions = 0;

    for (const auto& stage : mNonUniformStages)
    {
        mNumPartitions += stage->GetNumPartitions();
    }

    mNuRestScratch.assign(kFirChunk, 0.0f);
    mNuStageScratch.assign(kFirChunk, 0.0f);

    mIsNonUniform = true;
    mInitialized = true;
    return true;
}

void RealtimeConvolver::BuildFir(const float* coefficients, size_t count)
{
    mFirReversed.assign(count, 0.0f);

    for (size_t k = 0; k < count; ++k)
    {
        mFirReversed[count - 1 - k] = coefficients[k];
    }

    mFirHistory.assign(count - 1 + kFirChunk, 0.0f);
}

void RealtimeConvolver::ProcessFir(const float* input, float* output, size_t numSamples)
{
    const size_t taps = mFirReversed.size();
    const float* coefficients = mFirReversed.data();
    float* history = mFirHistory.data();
    size_t done = 0;

    while (done < numSamples)
    {
        const size_t chunk = std::min(numSamples - done, kFirChunk);
        const float* in = input + done;
        float* out = output + done;

        // The chunk goes in behind the history before any output is written, so the output may
        // be the input buffer. A non-finite sample goes in as silence: in the history it would
        // poison the next `taps` outputs.
        float* fresh = history + taps - 1;

        for (size_t i = 0; i < chunk; ++i)
        {
            fresh[i] = IsFinite(in[i]) ? in[i] : 0.0f;
        }

        // y[i] = sum_k h[k] x[i - k], which with the coefficients reversed is the dot product of
        // them with history[i, i + taps). Four outputs share each coefficient load; the compiler
        // vectorises each sum over k (Release builds are fast-math, so it may reorder them).
        size_t i = 0;

        for (; i + 4 <= chunk; i += 4)
        {
            const float* x = history + i;
            float s0 = 0.0f;
            float s1 = 0.0f;
            float s2 = 0.0f;
            float s3 = 0.0f;

            for (size_t k = 0; k < taps; ++k)
            {
                const float c = coefficients[k];
                s0 += c * x[k];
                s1 += c * x[k + 1];
                s2 += c * x[k + 2];
                s3 += c * x[k + 3];
            }

            out[i] = s0;
            out[i + 1] = s1;
            out[i + 2] = s2;
            out[i + 3] = s3;
        }

        for (; i < chunk; ++i)
        {
            const float* x = history + i;
            float s = 0.0f;

            for (size_t k = 0; k < taps; ++k)
            {
                s += coefficients[k] * x[k];
            }

            out[i] = s;
        }

        // Keep the newest taps - 1 samples as the next chunk's history.
        std::memmove(history, history + chunk, (taps - 1) * sizeof(float));
        done += chunk;
    }
}

// Fold IR partitions into the accumulator up to (but not including) `target`.
//
// Partition p pairs the IR's p-th slice with the input frame from p blocks back, so
// every partition except p == 0 uses a frame that has been sitting in the delay line
// since before the current block started. Only p == 0 has to wait for the boundary.
// Process() therefore walks this forward a slice at a time as the block fills, which
// turns "one 880-multiply burst every partition" into the same work spread evenly over
// the callbacks that produce it. With a 64-sample host block against a multi-second IR
// the burst was several times the callback deadline; the average was never the problem.
void RealtimeConvolver::AccumulatePartitionsUpTo(size_t target)
{
    const size_t limit = std::min(target, mNumPartitions);

    while (mPreAccumPartition < limit)
    {
        const size_t p = mPreAccumPartition;

        // Measured from the newest frame as of this block's start, partition p's frame
        // is (p - 1) slots older.
        const size_t delayIdx = (mPreAccumAnchor + mNumPartitions - (p - 1)) % mNumPartitions;
        RealFFT::MultiplyAccumulate(mAccumulator.data(), mInputFFTDelayLine[delayIdx].data(),
                                    mIRPartitionsFFT[p].data(), mNumBins);
        ++mPreAccumPartition;
    }
}

void RealtimeConvolver::ProcessBlock()
{
    // Everything but partition 0 should already be in the accumulator. Finish whatever
    // the schedule has not reached: the very first block, or a host feeding us fewer
    // samples than it takes to walk the whole partition list.
    AccumulatePartitionsUpTo(mNumPartitions);

    // Forward FFT of [previous samples | current samples], the overlap-save frame, straight into
    // the delay line. This is the slot the accumulation pass never reads, so the frame it
    // replaces (the oldest) is already spent.
    auto& currentSlot = mInputFFTDelayLine[mDelayLineIndex];
    mFFT->Forward(currentSlot.data(), mPreviousInputBlock.data(), mInputBuffer.data());

    // Save current input for next block
    std::copy(mInputBuffer.begin(), mInputBuffer.end(), mPreviousInputBlock.begin());

    // Partition 0 is the only one that needs the frame that just closed.
    RealFFT::MultiplyAccumulate(mAccumulator.data(), currentSlot.data(), mIRPartitionsFFT[0].data(), mNumBins);

    // Inverse FFT
    mFFT->Inverse(mAccumulator.data());
    const float* result = mFFT->GetResult();

    // Overlap-Save output extraction:
    // The first N samples contain circular convolution artifacts - DISCARD them
    // The last N samples are the valid linear convolution result - KEEP them
    const float scale = 1.0f / static_cast<float>(mFFTSize);

    for (size_t i = 0; i < mPartitionSize; ++i)
    {
        // Keep only the SECOND half of the IFFT output (valid linear convolution)
        float sample = result[mPartitionSize + i] * scale;

        // Safety: clamp to reasonable audio range to prevent numeric instability
        if (!IsFinite(sample))
        {
            sample = 0.0f;
        }
        else
        {
            sample = std::clamp(sample, -100.0f, 100.0f);
        }

        mOutputBuffer[i] = sample;
    }

    // Open the next block's accumulation against the frame just stored, and hand the
    // write cursor on to the slot that frame will displace.
    SimdFFT::ClearBuffer(mAccumulator.data(), mNumBins);
    mPreAccumAnchor = mDelayLineIndex;
    mPreAccumPartition = 1;
    mDelayLineIndex = (mDelayLineIndex + 1) % mNumPartitions;

    mOutputBufferReadPos = 0;
}

void RealtimeConvolver::Process(const float* input, float* output, int numSamples)
{
    if (!mInitialized || !input || !output || numSamples <= 0)
    {
        if (output && numSamples > 0)
        {
            std::memset(output, 0, static_cast<size_t>(numSamples) * sizeof(float));
        }

        return;
    }

    // Use direct convolution for short IRs (zero latency)
    if (mUseDirectConvolution)
    {
        ProcessFir(input, output, static_cast<size_t>(numSamples));

        for (int k = 0; k < numSamples; ++k)
        {
            output[k] = SanitizeOutput(output[k]);
        }

        return;
    }

    // Zero-latency non-uniform (Gardner-style) engine.
    if (mIsNonUniform)
    {
        ProcessNonUniform(input, output, numSamples);
        return;
    }

    int i = 0;

    while (i < numSamples)
    {
        // If output buffer exhausted, process next block
        if (mOutputBufferReadPos >= mPartitionSize)
        {
            if (mSpread)
            {
                ProcessSpreadBoundary();
            }
            else
            {
                ProcessBlock();
            }
        }

        // Output available samples
        // Key insight: We SYNCHRONIZE input/output buffer positions
        // When we read sample N from output, we write sample N to input for next block
        while (mOutputBufferReadPos < mPartitionSize && i < numSamples)
        {
            const size_t bufferPos = mOutputBufferReadPos;
            output[i] = mOutputBuffer[bufferPos];
            mInputBuffer[bufferPos] = input[i];
            ++mOutputBufferReadPos;
            ++i;
        }

        // Keep the accumulation as far through the partition list as the block is
        // through its samples, so the cost lands evenly instead of all at the boundary.
        if (mSpread)
        {
            // 64-bit: a long IR's job cost times a partition overflows a 32-bit size_t.
            RunSpreadJobTo(
                static_cast<size_t>(static_cast<std::uint64_t>(mJobCost) * mOutputBufferReadPos / mPartitionSize));
        }
        else if (mNumPartitions > 1)
        {
            AccumulatePartitionsUpTo(1 + ((mNumPartitions - 1) * mOutputBufferReadPos) / mPartitionSize);
        }
    }
}

void RealtimeConvolver::ProcessNonUniform(const float* input, float* output, int numSamples)
{
    int processed = 0;

    while (processed < numSamples)
    {
        const size_t chunk = std::min(static_cast<size_t>(numSamples - processed), kFirChunk);
        const float* in = input + processed;
        float* out = output + processed;

        // The stages read the input before the FIR writes any output, so the output may be the
        // input buffer.
        std::fill_n(mNuRestScratch.data(), chunk, 0.0f);

        for (auto& stage : mNonUniformStages)
        {
            stage->Process(in, mNuStageScratch.data(), static_cast<int>(chunk));

            for (size_t k = 0; k < chunk; ++k)
            {
                mNuRestScratch[k] += mNuStageScratch[k];
            }
        }

        // Every stage is already time-aligned (see BuildNonUniform), so they add straight onto
        // the head.
        ProcessFir(in, out, chunk);

        for (size_t k = 0; k < chunk; ++k)
        {
            out[k] = SanitizeOutput(out[k] + mNuRestScratch[k]);
        }

        processed += static_cast<int>(chunk);
    }
}

void RealtimeConvolver::Reset()
{
    if (!mInitialized)
    {
        return;
    }

    // Reset direct convolution state
    if (mUseDirectConvolution)
    {
        std::fill(mFirHistory.begin(), mFirHistory.end(), 0.0f);
        return;
    }

    // Reset non-uniform engine state
    if (mIsNonUniform)
    {
        for (auto& stage : mNonUniformStages)
        {
            if (stage)
            {
                stage->Reset();
            }
        }

        std::fill(mFirHistory.begin(), mFirHistory.end(), 0.0f);
        std::fill(mNuRestScratch.begin(), mNuRestScratch.end(), 0.0f);
        std::fill(mNuStageScratch.begin(), mNuStageScratch.end(), 0.0f);
        return;
    }

    // Clear delay line
    for (auto& fft : mInputFFTDelayLine)
    {
        std::fill(fft.begin(), fft.end(), std::complex<float>(0.0f, 0.0f));
    }

    mDelayLineIndex = 0;

    // Drop the part-built accumulation along with the frames it was built from.
    std::fill(mAccumulator.begin(), mAccumulator.end(), std::complex<float>(0.0f, 0.0f));
    mPreAccumAnchor = 0;
    mPreAccumPartition = mNumPartitions;

    // Clear buffers
    std::fill(mInputBuffer.begin(), mInputBuffer.end(), 0.0f);
    std::fill(mOutputBuffer.begin(), mOutputBuffer.end(), 0.0f);
    std::fill(mPreviousInputBlock.begin(), mPreviousInputBlock.end(), 0.0f);
    mInputBufferPos = 0;
    mOutputBufferReadPos = mPartitionSize;

    // Drop the block in flight too, so the first boundary after the reset plays silence.
    if (mSpread)
    {
        ResetSpread();
    }
}

bool RealtimeConvolver::CopyStateFrom(const RealtimeConvolver& source) noexcept
{
    if (&source == this || !LayoutMatches(source))
    {
        return false;
    }

    CopyStateUnchecked(source);
    return true;
}

size_t RealtimeConvolver::HistorySpan() const noexcept
{
    if (!mInitialized)
    {
        return 0;
    }

    // The FIR keeps its last taps of input, plus a chunk being gathered.
    size_t span = mFirHistory.size();

    if (mIsNonUniform)
    {
        // Every stage hears the whole input; the history is as old as the oldest any of them keeps.
        for (const auto& stage : mNonUniformStages)
        {
            if (stage)
            {
                span = std::max(span, stage->HistorySpan());
            }
        }

        return span;
    }

    if (mUseDirectConvolution)
    {
        return span;
    }

    // A uniform engine: one frame of input per partition in the delay line, each two partitions
    // wide, plus the block filling and, for the spread engine, the block in flight.
    return std::max(span, (mNumPartitions + 4) * mPartitionSize);
}

bool RealtimeConvolver::LayoutMatches(const RealtimeConvolver& other) const noexcept
{
    if (mInitialized != other.mInitialized || mUseDirectConvolution != other.mUseDirectConvolution ||
        mIsNonUniform != other.mIsNonUniform || mSpread != other.mSpread || mPartitionSize != other.mPartitionSize ||
        mNumPartitions != other.mNumPartitions || mFirHistory.size() != other.mFirHistory.size() ||
        mInputFFTDelayLine.size() != other.mInputFFTDelayLine.size() ||
        mInputBuffer.size() != other.mInputBuffer.size() || mOutputBuffer.size() != other.mOutputBuffer.size() ||
        mPreviousInputBlock.size() != other.mPreviousInputBlock.size() ||
        mAccumulator.size() != other.mAccumulator.size() ||
        mSpreadCurrentBlock.size() != other.mSpreadCurrentBlock.size() ||
        mSpreadOutput.size() != other.mSpreadOutput.size() || mNuRestScratch.size() != other.mNuRestScratch.size() ||
        mNuStageScratch.size() != other.mNuStageScratch.size() ||
        mNonUniformStages.size() != other.mNonUniformStages.size())
    {
        return false;
    }

    for (size_t i = 0; i < mInputFFTDelayLine.size(); ++i)
    {
        if (mInputFFTDelayLine[i].size() != other.mInputFFTDelayLine[i].size())
        {
            return false;
        }
    }

    for (size_t i = 0; i < mNonUniformStages.size(); ++i)
    {
        const auto& mine = mNonUniformStages[i];
        const auto& theirs = other.mNonUniformStages[i];

        if (static_cast<bool>(mine) != static_cast<bool>(theirs) || (mine && !mine->LayoutMatches(*theirs)))
        {
            return false;
        }
    }

    return true;
}

void RealtimeConvolver::CopyStateUnchecked(const RealtimeConvolver& source) noexcept
{
    // Every member Process() changes; the impulse and the layout are the same on both sides.
    std::copy(source.mFirHistory.begin(), source.mFirHistory.end(), mFirHistory.begin());

    for (size_t i = 0; i < mInputFFTDelayLine.size(); ++i)
    {
        std::copy(source.mInputFFTDelayLine[i].begin(), source.mInputFFTDelayLine[i].end(),
                  mInputFFTDelayLine[i].begin());
    }

    mDelayLineIndex = source.mDelayLineIndex;
    std::copy(source.mInputBuffer.begin(), source.mInputBuffer.end(), mInputBuffer.begin());
    std::copy(source.mOutputBuffer.begin(), source.mOutputBuffer.end(), mOutputBuffer.begin());
    std::copy(source.mPreviousInputBlock.begin(), source.mPreviousInputBlock.end(), mPreviousInputBlock.begin());
    mInputBufferPos = source.mInputBufferPos;
    mOutputBufferReadPos = source.mOutputBufferReadPos;
    std::copy(source.mAccumulator.begin(), source.mAccumulator.end(), mAccumulator.begin());
    mPreAccumAnchor = source.mPreAccumAnchor;
    mPreAccumPartition = source.mPreAccumPartition;
    std::copy(source.mSpreadCurrentBlock.begin(), source.mSpreadCurrentBlock.end(), mSpreadCurrentBlock.begin());
    std::copy(source.mSpreadOutput.begin(), source.mSpreadOutput.end(), mSpreadOutput.begin());
    mJobSlot = source.mJobSlot;
    mJobPhase = source.mJobPhase;
    mJobPos = source.mJobPos;
    mJobDone = source.mJobDone;
    mJobCost = source.mJobCost;
    std::copy(source.mNuRestScratch.begin(), source.mNuRestScratch.end(), mNuRestScratch.begin());
    std::copy(source.mNuStageScratch.begin(), source.mNuStageScratch.end(), mNuStageScratch.begin());

    for (size_t i = 0; i < mNonUniformStages.size(); ++i)
    {
        if (mNonUniformStages[i])
        {
            mNonUniformStages[i]->CopyStateUnchecked(*source.mNonUniformStages[i]);
        }
    }
}
} // namespace guitarfx
