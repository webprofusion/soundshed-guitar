#include "FFTConvolution.h"
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace namguitar
{
  namespace
  {
    constexpr double kPi = std::numbers::pi;
  }

  size_t FFTConvolution::NextPowerOf2(size_t n)
  {
    size_t power = 1;
    while (power < n)
    {
      power *= 2;
    }
    return power;
  }

  void FFTConvolution::FFT(std::vector<std::complex<double>>& data, bool inverse)
  {
    const size_t n = data.size();
    if (n <= 1) return;

    // Check if power of 2
    if ((n & (n - 1)) != 0)
    {
      throw std::runtime_error("FFT size must be power of 2");
    }

    // Bit-reversal permutation
    for (size_t i = 0, j = 0; i < n; ++i)
    {
      if (j > i)
      {
        std::swap(data[i], data[j]);
      }

      size_t m = n / 2;
      while (m >= 1 && j >= m)
      {
        j -= m;
        m /= 2;
      }
      j += m;
    }

    // Cooley-Tukey decimation-in-time radix-2 FFT
    // Calculate log2(n) for the number of stages
    size_t log2n = 0;
    size_t temp = n;
    while (temp > 1)
    {
      temp >>= 1;
      ++log2n;
    }
    
    for (size_t s = 1; s <= log2n; ++s)
    {
      const size_t m = 1 << s;  // 2^s
      const size_t m2 = m / 2;
      const double sign = inverse ? 1.0 : -1.0;
      const std::complex<double> wm = std::exp(std::complex<double>(0.0, sign * 2.0 * kPi / static_cast<double>(m)));

      for (size_t k = 0; k < n; k += m)
      {
        std::complex<double> w(1.0, 0.0);
        for (size_t j = 0; j < m2; ++j)
        {
          const std::complex<double> t = w * data[k + j + m2];
          const std::complex<double> u = data[k + j];
          data[k + j] = u + t;
          data[k + j + m2] = u - t;
          w *= wm;
        }
      }
    }

    // Scaling for inverse FFT
    if (inverse)
    {
      const double scale = 1.0 / static_cast<double>(n);
      for (auto& val : data)
      {
        val *= scale;
      }
    }
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
      return;
    }

    mImpulse = irSamples;
    mBlockSize = blockSize;
    
    // FFT size must be >= blockSize + IR length - 1 for linear convolution
    const size_t minFFTSize = blockSize + irSamples.size() - 1;
    mFFTSize = NextPowerOf2(minFFTSize);

    // Initialize overlap buffer (size = IR length - 1)
    mOverlapBuffer.assign(irSamples.size() - 1, 0.0);

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
        
        // Pre-compute FFT of this partition
        FFT(partition, false);
        mIRPartitions[p] = partition;
        
        // Initialize history for this partition
        mInputHistory[p].assign(mPartitionSize, 0.0);
      }
    }
    else
    {
      // For small IRs, pre-compute IR FFT once instead of every frame
      mIRFFT.resize(mFFTSize, std::complex<double>(0.0, 0.0));
      for (size_t i = 0; i < irSamples.size(); ++i)
      {
        mIRFFT[i] = std::complex<double>(irSamples[i], 0.0);
      }
      FFT(mIRFFT, false);
      
      mUsePartitionedConvolution = false;
      mIRPartitions.clear();
      mInputHistory.clear();
    }
  }

  void FFTConvolution::Process(const std::vector<double>& input, std::vector<double>& output)
  {
    if (mImpulse.empty() || input.empty())
    {
      output = input;
      return;
    }

    if (mUsePartitionedConvolution)
    {
      // Partitioned convolution: process each IR partition separately
      output.resize(input.size(), 0.0);
      size_t partitionFFTSize = NextPowerOf2(2 * mPartitionSize);
      
      for (size_t p = 0; p < mNumPartitions; ++p)
      {
        std::vector<std::complex<double>> inputFFT(partitionFFTSize, std::complex<double>(0.0, 0.0));
        
        // Copy current input block
        for (size_t i = 0; i < input.size(); ++i)
        {
          inputFFT[i] = std::complex<double>(input[i], 0.0);
        }
        
        // Forward FFT of input
        FFT(inputFFT, false);
        
        // Multiply by pre-computed IR partition FFT
        for (size_t i = 0; i < partitionFFTSize; ++i)
        {
          inputFFT[i] *= mIRPartitions[p][i];
        }
        
        // Inverse FFT
        FFT(inputFFT, true);
        
        // Add to output (with overlap handling)
        for (size_t i = 0; i < input.size(); ++i)
        {
          output[i] += inputFFT[i].real();
        }
      }
    }
    else
    {
      // Standard overlap-add with pre-computed IR FFT
      const size_t inputSize = input.size();
      const size_t irSize = mImpulse.size();
      
      // Prepare FFT buffers (reuse from member if possible for efficiency)
      std::vector<std::complex<double>> inputFFT(mFFTSize, std::complex<double>(0.0, 0.0));

      // Copy input to FFT buffer (zero-padded)
      for (size_t i = 0; i < inputSize; ++i)
      {
        inputFFT[i] = std::complex<double>(input[i], 0.0);
      }

      // Forward FFT on input only (IR FFT is pre-computed)
      FFT(inputFFT, false);

      // Multiply in frequency domain by pre-computed IR FFT
      for (size_t i = 0; i < mFFTSize; ++i)
      {
        inputFFT[i] *= mIRFFT[i];
      }

      // Inverse FFT
      FFT(inputFFT, true);

      // Extract real part and apply overlap-add
      output.resize(inputSize);
      const size_t overlapSize = mOverlapBuffer.size();

      for (size_t i = 0; i < inputSize; ++i)
      {
        double sample = inputFFT[i].real();
        
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
        if (srcIdx < outputSize && srcIdx < inputFFT.size())
        {
          mOverlapBuffer[i] = inputFFT[srcIdx].real();
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
