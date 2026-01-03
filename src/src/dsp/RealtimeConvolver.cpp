#include "RealtimeConvolver.h"
#include "SimdFFT.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace namguitar
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

  RealtimeConvolver::RealtimeConvolver(RealtimeConvolver&&) noexcept = default;

  RealtimeConvolver& RealtimeConvolver::operator=(RealtimeConvolver&&) noexcept = default;

  bool RealtimeConvolver::SetImpulse(const std::vector<float>& irSamples, int blockSize)
  {
    mInitialized = false;
    
    if (irSamples.empty() || blockSize <= 0)
    {
      mIRPartitionsFFT.clear();
      mInputFFTDelayLine.clear();
      mFFT.reset();
      return false;
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
    for (auto& fft : mInputFFTDelayLine)
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
    
    // Overlap buffer
    mOverlapBuffer.assign(mPartitionSize, 0.0);
    
    mInitialized = true;
    return true;
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
    auto& currentSlot = mInputFFTDelayLine[mDelayLineIndex];
    std::copy(mFFTOutputBuffer.begin(), mFFTOutputBuffer.end(), currentSlot.begin());
    
    // Clear accumulator using SIMD
    SimdFFT::ClearBuffer(mAccumulator.data(), mFFTSize);
    
    // Accumulate contributions from all IR partitions using SIMD
    for (size_t p = 0; p < mNumPartitions; ++p)
    {
      const size_t delayIdx = (mDelayLineIndex + mNumPartitions - p) % mNumPartitions;
      const auto& inputFFT = mInputFFTDelayLine[delayIdx];
      const auto& irFFT = mIRPartitionsFFT[p];
      
      // SIMD complex multiply-accumulate (the hot path)
      SimdFFT::ComplexMultiplyAccumulate(mAccumulator.data(), inputFFT.data(), irFFT.data(), mFFTSize);
    }
    
    // Advance delay line write position
    mDelayLineIndex = (mDelayLineIndex + 1) % mNumPartitions;
    
    // Inverse FFT
    mFFT->Inverse(mFFTInputBuffer.data(), mAccumulator.data());
    
    // Scale and extract output with overlap-add
    const double scale = 1.0 / static_cast<double>(mFFTSize);
    
    for (size_t i = 0; i < mPartitionSize; ++i)
    {
      // First half: add to overlap from previous block
      mOutputBuffer[i] = mOverlapBuffer[i] + mFFTInputBuffer[i].real() * scale;
      // Second half becomes next overlap
      mOverlapBuffer[i] = mFFTInputBuffer[mPartitionSize + i].real() * scale;
    }
    
    mOutputBufferReadPos = 0;
  }

  void RealtimeConvolver::Process(const double* input, double* output, int numSamples)
  {
    if (!mInitialized || !input || !output || numSamples <= 0)
    {
      if (output && numSamples > 0)
      {
        std::memset(output, 0, numSamples * sizeof(double));
      }
      return;
    }

    int i = 0;
    while (i < numSamples)
    {
      // Output available samples
      while (mOutputBufferReadPos < mPartitionSize && i < numSamples)
      {
        output[i] = mOutputBuffer[mOutputBufferReadPos];
        mInputBuffer[mOutputBufferReadPos] = input[i];
        ++mOutputBufferReadPos;
        ++i;
      }
      
      // If output buffer exhausted, process next block
      if (mOutputBufferReadPos >= mPartitionSize && i < numSamples)
      {
        ProcessBlock();
      }
    }
  }

  void RealtimeConvolver::Reset()
  {
    if (!mInitialized)
    {
      return;
    }
    
    // Clear delay line
    for (auto& fft : mInputFFTDelayLine)
    {
      std::fill(fft.begin(), fft.end(), std::complex<double>(0.0, 0.0));
    }
    mDelayLineIndex = 0;
    
    // Clear buffers
    std::fill(mInputBuffer.begin(), mInputBuffer.end(), 0.0);
    std::fill(mOutputBuffer.begin(), mOutputBuffer.end(), 0.0);
    std::fill(mOverlapBuffer.begin(), mOverlapBuffer.end(), 0.0);
    std::fill(mPreviousInputBlock.begin(), mPreviousInputBlock.end(), 0.0);
    mInputBufferPos = 0;
    mOutputBufferReadPos = mPartitionSize;
  }

} // namespace namguitar
