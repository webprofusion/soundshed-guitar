#include "OptimizedConvolver.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// PocketFFT - header-only FFT library (C++ version)
#include "pocketfft_hdronly.h"

namespace namguitar
{
  namespace
  {
    size_t NextPowerOf2(size_t n)
    {
      size_t power = 1;
      while (power < n)
        power *= 2;
      return power;
    }
  }

  OptimizedConvolver::OptimizedConvolver() = default;

  OptimizedConvolver::~OptimizedConvolver() = default;

  void OptimizedConvolver::SetImpulse(const std::vector<float>& irSamples, int blockSize)
  {
    if (irSamples.empty() || blockSize <= 0)
    {
      mInitialized = false;
      mImpulse.clear();
      mOverlapBuffer.clear();
      return;
    }

    mImpulse = irSamples;
    mBlockSize = blockSize;

    // Use FFT convolution for all but the smallest IRs
    if (irSamples.size() > 64)
    {
      try
      {
        InitializeFFT(irSamples, blockSize);
        mUseFFT = true;
        mInitialized = true;
        return;
      }
      catch (...)
      {
        mUseFFT = false;
      }
    }

    // Fallback: use direct time-domain convolution for very small IRs
    mUseFFT = false;
    mInitialized = true;
  }

  void OptimizedConvolver::InitializeFFT(const std::vector<float>& irSamples, int blockSize)
  {
    const size_t minFFTSize = blockSize + irSamples.size() - 1;
    mFFTSize = NextPowerOf2(minFFTSize);

    // Allocate buffers
    mInputBuffer.resize(mFFTSize);
    mOutputBuffer.resize(mFFTSize);
    mIRFreqDomain.resize(mFFTSize);

    // Prepare IR in time domain (zero-padded)
    std::vector<std::complex<double>> irTimeDomain(mFFTSize, std::complex<double>(0.0, 0.0));
    for (size_t i = 0; i < irSamples.size(); ++i)
    {
      irTimeDomain[i] = std::complex<double>(static_cast<double>(irSamples[i]), 0.0);
    }

    // Pre-compute IR FFT using PocketFFT
    pocketfft::shape_t shape = { mFFTSize };
    pocketfft::stride_t stride = { sizeof(std::complex<double>) };
    pocketfft::shape_t axes = { 0 };

    mIRFreqDomain = irTimeDomain;
    pocketfft::c2c(shape, stride, stride, axes, pocketfft::FORWARD,
                   irTimeDomain.data(), mIRFreqDomain.data(), 1.0);

    // Initialize overlap buffer
    mOverlapBuffer.assign(irSamples.size() - 1, 0.0);
  }

  void OptimizedConvolver::ProcessFFT(const std::vector<double>& input, std::vector<double>& output)
  {
    const size_t inputSize = input.size();
    const size_t irSize = mImpulse.size();

    // Zero-pad input and prepare for FFT
    std::fill(mInputBuffer.begin(), mInputBuffer.end(), std::complex<double>(0.0, 0.0));
    for (size_t i = 0; i < inputSize; ++i)
    {
      mInputBuffer[i] = std::complex<double>(input[i], 0.0);
    }

    // Forward FFT using PocketFFT
    pocketfft::shape_t shape = { mFFTSize };
    pocketfft::stride_t stride = { sizeof(std::complex<double>) };
    pocketfft::shape_t axes = { 0 };

    pocketfft::c2c(shape, stride, stride, axes, pocketfft::FORWARD,
                   mInputBuffer.data(), mOutputBuffer.data(), 1.0);

    // Multiply in frequency domain
    for (size_t i = 0; i < mFFTSize; ++i)
    {
      mOutputBuffer[i] *= mIRFreqDomain[i];
    }

    // Inverse FFT
    pocketfft::c2c(shape, stride, stride, axes, pocketfft::BACKWARD,
                   mOutputBuffer.data(), mInputBuffer.data(), 1.0 / static_cast<double>(mFFTSize));

    // Extract results with overlap-add
    output.resize(inputSize);
    const size_t overlapSize = mOverlapBuffer.size();

    for (size_t i = 0; i < inputSize; ++i)
    {
      double sample = mInputBuffer[i].real();

      // Add overlap from previous block
      if (i < overlapSize)
      {
        sample += mOverlapBuffer[i];
      }

      output[i] = sample;
    }

    // Save overlap for next block
    for (size_t i = 0; i < overlapSize; ++i)
    {
      const size_t srcIdx = inputSize + i;
      if (srcIdx < mFFTSize)
      {
        mOverlapBuffer[i] = mInputBuffer[srcIdx].real();
      }
      else
      {
        mOverlapBuffer[i] = 0.0;
      }
    }
  }

  void OptimizedConvolver::ProcessDirect(const std::vector<double>& input, std::vector<double>& output)
  {
    // Simple time-domain convolution for small IRs
    const size_t inputSize = input.size();
    const size_t irSize = mImpulse.size();
    output.resize(inputSize);

    for (size_t n = 0; n < inputSize; ++n)
    {
      double sum = 0.0;
      const size_t limit = std::min(n + 1, irSize);

      for (size_t k = 0; k < limit; ++k)
      {
        sum += input[n - k] * mImpulse[k];
      }

      output[n] = sum;
    }
  }

  void OptimizedConvolver::Process(const std::vector<double>& input, std::vector<double>& output)
  {
    if (!mInitialized || mImpulse.empty() || input.empty())
    {
      output = input;
      return;
    }

    if (mUseFFT)
    {
      ProcessFFT(input, output);
      return;
    }

    // Fallback to direct convolution
    ProcessDirect(input, output);
  }

  void OptimizedConvolver::Reset()
  {
    std::fill(mOverlapBuffer.begin(), mOverlapBuffer.end(), 0.0);
  }

} // namespace namguitar
