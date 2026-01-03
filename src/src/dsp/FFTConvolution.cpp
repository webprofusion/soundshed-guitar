#include "FFTConvolution.h"
#include <cmath>
#include <stdexcept>

// KFR headers for DFT
#include <kfr/dft.hpp>

namespace namguitar
{
  FFTConvolution::FFTConvolution() = default;

  FFTConvolution::~FFTConvolution() = default;

  FFTConvolution::FFTConvolution(FFTConvolution&&) noexcept = default;

  FFTConvolution& FFTConvolution::operator=(FFTConvolution&&) noexcept = default;

  size_t FFTConvolution::NextPowerOf2(size_t n)
  {
    size_t power = 1;
    while (power < n)
    {
      power *= 2;
    }
    return power;
  }

  void FFTConvolution::SetImpulse(const std::vector<float>& irSamples, int blockSize)
  {
    if (irSamples.empty() || blockSize <= 0)
    {
      mImpulse.clear();
      mOverlapBuffer.clear();
      mIRFFT.clear();
      mIRPartitions.clear();
      mInputHistory.clear();
      mUsePartitionedConvolution = false;
      mDFTPlan.reset();
      return;
    }

    mImpulse = irSamples;
    mBlockSize = blockSize;
    
    // FFT size must be >= blockSize + IR length - 1 for linear convolution
    const size_t minFFTSize = blockSize + irSamples.size() - 1;
    mFFTSize = NextPowerOf2(minFFTSize);

    // Initialize overlap buffer (size = IR length - 1)
    mOverlapBuffer.assign(irSamples.size() - 1, 0.0);

    // Create KFR DFT plan
    mDFTPlan = std::make_unique<kfr::dft_plan<double>>(mFFTSize);
    
    // Allocate working buffers
    mInputBuffer.resize(mFFTSize);
    mOutputBuffer.resize(mFFTSize);
    mTempBuffer.resize(mDFTPlan->temp_size);

    // OPTIMIZATION: For large IRs (>1024 samples), use partitioned convolution
    // This splits the IR into segments and processes them independently
    // reducing per-frame computation overhead
    constexpr size_t kPartitionThreshold = 1024;
    if (irSamples.size() > kPartitionThreshold)
    {
      // Use partitioned convolution for large IRs
      mPartitionSize = blockSize;  // Process one block at a time
      mNumPartitions = (irSamples.size() + mPartitionSize - 1) / mPartitionSize;
      
      mIRPartitions.resize(mNumPartitions);
      mInputHistory.resize(mNumPartitions);
      mUsePartitionedConvolution = true;

      // Pre-compute FFT for each IR partition
      size_t partitionFFTSize = NextPowerOf2(2 * mPartitionSize);
      kfr::dft_plan<double> partitionPlan(partitionFFTSize);
      std::vector<uint8_t> partitionTemp(partitionPlan.temp_size);
      
      for (size_t p = 0; p < mNumPartitions; ++p)
      {
        std::vector<std::complex<double>> partition(partitionFFTSize, std::complex<double>(0.0, 0.0));
        
        // Copy IR partition
        size_t start = p * mPartitionSize;
        size_t end = std::min(start + mPartitionSize, irSamples.size());
        for (size_t i = start; i < end; ++i)
        {
          partition[i - start] = std::complex<double>(irSamples[i], 0.0);
        }
        
        // Pre-compute FFT of this partition using KFR
        mIRPartitions[p].resize(partitionFFTSize);
        partitionPlan.execute(mIRPartitions[p].data(), partition.data(), partitionTemp.data(), false);
        
        // Initialize history for this partition
        mInputHistory[p].assign(mPartitionSize, 0.0);
      }
    }
    else
    {
      // For small IRs, pre-compute IR FFT once instead of every frame
      std::vector<std::complex<double>> irTimeDomain(mFFTSize, std::complex<double>(0.0, 0.0));
      for (size_t i = 0; i < irSamples.size(); ++i)
      {
        irTimeDomain[i] = std::complex<double>(irSamples[i], 0.0);
      }
      
      // Forward FFT of IR using KFR
      mIRFFT.resize(mFFTSize);
      mDFTPlan->execute(mIRFFT.data(), irTimeDomain.data(), mTempBuffer.data(), false);
      
      mUsePartitionedConvolution = false;
      mIRPartitions.clear();
      mInputHistory.clear();
    }
  }

  void FFTConvolution::Process(const std::vector<double>& input, std::vector<double>& output)
  {
    if (mImpulse.empty() || input.empty() || !mDFTPlan)
    {
      output = input;
      return;
    }

    if (mUsePartitionedConvolution)
    {
      // Partitioned convolution: process each IR partition separately
      output.resize(input.size(), 0.0);
      size_t partitionFFTSize = NextPowerOf2(2 * mPartitionSize);
      
      kfr::dft_plan<double> partitionPlan(partitionFFTSize);
      std::vector<uint8_t> partitionTemp(partitionPlan.temp_size);
      std::vector<std::complex<double>> inputFFT(partitionFFTSize);
      std::vector<std::complex<double>> resultFFT(partitionFFTSize);
      std::vector<std::complex<double>> inputTimeDomain(partitionFFTSize);
      
      for (size_t p = 0; p < mNumPartitions; ++p)
      {
        // Prepare input in time domain (zero-padded)
        std::fill(inputTimeDomain.begin(), inputTimeDomain.end(), std::complex<double>(0.0, 0.0));
        for (size_t i = 0; i < input.size() && i < partitionFFTSize; ++i)
        {
          inputTimeDomain[i] = std::complex<double>(input[i], 0.0);
        }
        
        // Forward FFT of input using KFR
        partitionPlan.execute(inputFFT.data(), inputTimeDomain.data(), partitionTemp.data(), false);
        
        // Multiply by pre-computed IR partition FFT
        for (size_t i = 0; i < partitionFFTSize; ++i)
        {
          resultFFT[i] = inputFFT[i] * mIRPartitions[p][i];
        }
        
        // Inverse FFT using KFR
        partitionPlan.execute(inputTimeDomain.data(), resultFFT.data(), partitionTemp.data(), true);
        
        // Add to output (with overlap handling)
        for (size_t i = 0; i < input.size(); ++i)
        {
          output[i] += inputTimeDomain[i].real() / static_cast<double>(partitionFFTSize);
        }
      }
    }
    else
    {
      // Standard overlap-add with pre-computed IR FFT
      const size_t inputSize = input.size();
      const size_t irSize = mImpulse.size();
      
      // Prepare input in time domain (zero-padded)
      std::fill(mInputBuffer.begin(), mInputBuffer.end(), std::complex<double>(0.0, 0.0));
      for (size_t i = 0; i < inputSize; ++i)
      {
        mInputBuffer[i] = std::complex<double>(input[i], 0.0);
      }

      // Forward FFT on input using KFR
      mDFTPlan->execute(mOutputBuffer.data(), mInputBuffer.data(), mTempBuffer.data(), false);

      // Multiply in frequency domain by pre-computed IR FFT
      for (size_t i = 0; i < mFFTSize; ++i)
      {
        mOutputBuffer[i] *= mIRFFT[i];
      }

      // Inverse FFT using KFR
      mDFTPlan->execute(mInputBuffer.data(), mOutputBuffer.data(), mTempBuffer.data(), true);

      // Extract real part and apply overlap-add
      output.resize(inputSize);
      const size_t overlapSize = mOverlapBuffer.size();
      const double scale = 1.0 / static_cast<double>(mFFTSize);

      for (size_t i = 0; i < inputSize; ++i)
      {
        double sample = mInputBuffer[i].real() * scale;
        
        // Add overlap from previous block
        if (i < overlapSize)
        {
          sample += mOverlapBuffer[i];
        }
        
        output[i] = sample;
      }

      // Save overlap for next block
      const size_t outputSize = inputSize + irSize - 1;
      for (size_t i = 0; i < overlapSize; ++i)
      {
        const size_t srcIdx = inputSize + i;
        if (srcIdx < outputSize && srcIdx < mInputBuffer.size())
        {
          mOverlapBuffer[i] = mInputBuffer[srcIdx].real() * scale;
        }
        else
        {
          mOverlapBuffer[i] = 0.0;
        }
      }
    }
  }

  void FFTConvolution::Reset()
  {
    std::fill(mOverlapBuffer.begin(), mOverlapBuffer.end(), 0.0);
  }

} // namespace namguitar
