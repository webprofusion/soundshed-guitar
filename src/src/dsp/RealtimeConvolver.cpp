#include "RealtimeConvolver.h"
#include "SimdFFT.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace guitarfx
{
  namespace
  {
    constexpr size_t NextPowerOf2(size_t n)
    {
      size_t power = 1;
      while (power < n)
        power *= 2;
      return power;
    }
  }

  RealtimeConvolver::RealtimeConvolver() = default;

  RealtimeConvolver::~RealtimeConvolver() = default;

  RealtimeConvolver::RealtimeConvolver(RealtimeConvolver &&) noexcept = default;

  RealtimeConvolver &RealtimeConvolver::operator=(RealtimeConvolver &&) noexcept = default;

  bool RealtimeConvolver::SetImpulse(const std::vector<float> &irSamples, int blockSize)
  {
    mInitialized = false;
    mUseDirectConvolution = false;

    if (irSamples.empty() || blockSize <= 0)
    {
      mIRPartitionsFFT.clear();
      mInputFFTDelayLine.clear();
      mFFT.reset();
      mDirectIR.clear();
      mDirectHistory.clear();
      return false;
    }

    // For very short IRs, use direct convolution (zero latency)
    if (irSamples.size() <= kDirectConvolutionThreshold)
    {
      mDirectIR = irSamples;
      mDirectHistory.assign(irSamples.size(), 0.0);
      mDirectHistoryPos = 0;
      mUseDirectConvolution = true;
      mInitialized = true;
      return true;
    }

    // Use a larger partition size for better efficiency with long IRs
    mPartitionSize = NextPowerOf2(static_cast<size_t>(std::max(blockSize, 256)));
    mPartitionSize = std::clamp(mPartitionSize, size_t{256}, size_t{2048});

    // FFT size is 2x partition for linear convolution
    mFFTSize = mPartitionSize * 2;

    // Calculate partitions needed
    mNumPartitions = (irSamples.size() + mPartitionSize - 1) / mPartitionSize;

    // Create FFT plan
    try
    {
      mFFT = std::make_unique<SimdFFT>(mFFTSize);
    }
    catch (...)
    {
      return false;
    }

    // Allocate all working buffers ONCE
    mFFTInputBuffer.resize(mFFTSize);
    mFFTOutputBuffer.resize(mFFTSize);
    mAccumulator.resize(mFFTSize);

    // Pre-compute FFT of each IR partition
    mIRPartitionsFFT.resize(mNumPartitions);

    for (size_t p = 0; p < mNumPartitions; ++p)
    {
      // Clear input buffer
      std::fill(mFFTInputBuffer.begin(), mFFTInputBuffer.end(), std::complex<double>(0.0, 0.0));

      // Copy IR partition into FIRST half
      const size_t irStart = p * mPartitionSize;
      const size_t irEnd = std::min(irStart + mPartitionSize, irSamples.size());

      for (size_t i = irStart; i < irEnd; ++i)
      {
        mFFTInputBuffer[i - irStart] = std::complex<double>(irSamples[i], 0.0);
      }

      // Compute and store FFT
      mIRPartitionsFFT[p].resize(mFFTSize);
      mFFT->Forward(mIRPartitionsFFT[p].data(), mFFTInputBuffer.data());
    }

    // Allocate delay line for input FFTs
    mInputFFTDelayLine.resize(mNumPartitions);
    for (auto &fft : mInputFFTDelayLine)
    {
      fft.assign(mFFTSize, std::complex<double>(0.0, 0.0));
    }
    mDelayLineIndex = 0;

    // Allocate I/O buffers
    mInputBuffer.assign(mPartitionSize, 0.0);
    mOutputBuffer.assign(mPartitionSize, 0.0);
    mInputBufferPos = 0;
    mOutputBufferReadPos = mPartitionSize; // Start empty

    // Previous input block for overlap-save (needed for proper convolution)
    mPreviousInputBlock.assign(mPartitionSize, 0.0);

    mInitialized = true;
    return true;
  }

  void RealtimeConvolver::ProcessDirect(const double *input, double *output, int numSamples)
  {
    const size_t irLen = mDirectIR.size();

    for (int i = 0; i < numSamples; ++i)
    {
      // Store new sample in history
      mDirectHistory[mDirectHistoryPos] = input[i];

      // Direct FIR convolution: output[n] = sum(input[n-k] * ir[k])
      double sum = 0.0;
      for (size_t k = 0; k < irLen; ++k)
      {
        // Calculate index into circular history buffer
        size_t histIdx = (mDirectHistoryPos + irLen - k) % irLen;
        sum += mDirectHistory[histIdx] * static_cast<double>(mDirectIR[k]);
      }
      output[i] = sum;

      // Advance history position
      mDirectHistoryPos = (mDirectHistoryPos + 1) % irLen;
    }
  }

  void RealtimeConvolver::ProcessBlock()
  {
    // Prepare FFT input: [previous samples | current samples]
    // This is the correct overlap-save arrangement for linear convolution
    for (size_t i = 0; i < mPartitionSize; ++i)
    {
      mFFTInputBuffer[i] = std::complex<double>(mPreviousInputBlock[i], 0.0);
      mFFTInputBuffer[mPartitionSize + i] = std::complex<double>(mInputBuffer[i], 0.0);
    }

    // Save current input for next block
    std::copy(mInputBuffer.begin(), mInputBuffer.end(), mPreviousInputBlock.begin());

    // Forward FFT of input block
    mFFT->Forward(mFFTOutputBuffer.data(), mFFTInputBuffer.data());

    // Store in delay line
    auto &currentSlot = mInputFFTDelayLine[mDelayLineIndex];
    std::copy(mFFTOutputBuffer.begin(), mFFTOutputBuffer.end(), currentSlot.begin());

    // Clear accumulator using SIMD
    SimdFFT::ClearBuffer(mAccumulator.data(), mFFTSize);

    // Accumulate contributions from all IR partitions using SIMD
    for (size_t p = 0; p < mNumPartitions; ++p)
    {
      const size_t delayIdx = (mDelayLineIndex + mNumPartitions - p) % mNumPartitions;
      const auto &inputFFT = mInputFFTDelayLine[delayIdx];
      const auto &irFFT = mIRPartitionsFFT[p];

      // SIMD complex multiply-accumulate (the hot path)
      SimdFFT::ComplexMultiplyAccumulate(mAccumulator.data(), inputFFT.data(), irFFT.data(), mFFTSize);
    }

    // Advance delay line write position
    mDelayLineIndex = (mDelayLineIndex + 1) % mNumPartitions;

    // Inverse FFT
    mFFT->Inverse(mFFTInputBuffer.data(), mAccumulator.data());

    // Overlap-Save output extraction:
    // The first N samples contain circular convolution artifacts - DISCARD them
    // The last N samples are the valid linear convolution result - KEEP them
    const double scale = 1.0 / static_cast<double>(mFFTSize);

    for (size_t i = 0; i < mPartitionSize; ++i)
    {
      // Keep only the SECOND half of the IFFT output (valid linear convolution)
      double sample = mFFTInputBuffer[mPartitionSize + i].real() * scale;

      // Safety: clamp to reasonable audio range to prevent numeric instability
      // This protects against FFT/accumulator overflow in edge cases
      if (std::isnan(sample) || std::isinf(sample))
      {
        sample = 0.0;
      }
      else
      {
        // Clamp to ±100.0 (extremely loud but still within double precision range)
        sample = std::clamp(sample, -100.0, 100.0);
      }

      mOutputBuffer[i] = sample;
    }

    mOutputBufferReadPos = 0;
  }

  void RealtimeConvolver::Process(const double *input, double *output, int numSamples)
  {
    if (!mInitialized || !input || !output || numSamples <= 0)
    {
      if (output && numSamples > 0)
      {
        std::memset(output, 0, numSamples * sizeof(double));
      }
      return;
    }

    // Use direct convolution for short IRs (zero latency)
    if (mUseDirectConvolution)
    {
      ProcessDirect(input, output, numSamples);
      return;
    }

    int i = 0;
    while (i < numSamples)
    {
      // If output buffer exhausted, process next block
      if (mOutputBufferReadPos >= mPartitionSize)
      {
        ProcessBlock();
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
      std::fill(mDirectHistory.begin(), mDirectHistory.end(), 0.0);
      mDirectHistoryPos = 0;
      return;
    }

    // Clear delay line
    for (auto &fft : mInputFFTDelayLine)
    {
      std::fill(fft.begin(), fft.end(), std::complex<double>(0.0, 0.0));
    }
    mDelayLineIndex = 0;

    // Clear buffers
    std::fill(mInputBuffer.begin(), mInputBuffer.end(), 0.0);
    std::fill(mOutputBuffer.begin(), mOutputBuffer.end(), 0.0);
    std::fill(mPreviousInputBlock.begin(), mPreviousInputBlock.end(), 0.0);
    mInputBufferPos = 0;
    mOutputBufferReadPos = mPartitionSize;
  }

} // namespace guitarfx
