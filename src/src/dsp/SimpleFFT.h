#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace namguitar
{
  /**
   * Lightweight radix-2 Cooley-Tukey FFT implementation.
   * Optimized for guitar cabinet impulse response convolution.
   * 
   * Features:
   * - Power-of-2 sizes only (which partitioned convolution uses anyway)
   * - Pre-computed twiddle factors for zero runtime trig calls
   * - In-place transforms for memory efficiency
   * - No external dependencies
   */
  class SimpleFFT
  {
  public:
    explicit SimpleFFT(size_t size)
      : mSize(size)
      , mLog2Size(0)
    {
      // Calculate log2(size)
      size_t n = size;
      while (n > 1)
      {
        n >>= 1;
        ++mLog2Size;
      }
      
      // Pre-compute twiddle factors (e^(-2*pi*i*k/N) for forward FFT)
      mTwiddles.resize(mSize / 2);
      const double twoPiOverN = -2.0 * M_PI / static_cast<double>(mSize);
      for (size_t k = 0; k < mSize / 2; ++k)
      {
        const double angle = twoPiOverN * static_cast<double>(k);
        mTwiddles[k] = std::complex<double>(std::cos(angle), std::sin(angle));
      }
      
      // Pre-compute bit-reversal permutation
      mBitReversed.resize(mSize);
      for (size_t i = 0; i < mSize; ++i)
      {
        mBitReversed[i] = BitReverse(i, mLog2Size);
      }
    }
    
    /**
     * Forward FFT: time domain -> frequency domain
     * @param output Output buffer (must be size mSize)
     * @param input Input buffer (must be size mSize)
     */
    void Forward(std::complex<double>* output, const std::complex<double>* input) const
    {
      // Bit-reversal permutation copy
      for (size_t i = 0; i < mSize; ++i)
      {
        output[mBitReversed[i]] = input[i];
      }
      
      // Cooley-Tukey iterative FFT
      for (size_t stage = 1; stage <= mLog2Size; ++stage)
      {
        const size_t m = size_t{1} << stage;        // Butterfly group size
        const size_t halfM = m >> 1;                 // Half butterfly
        const size_t twiddleStep = mSize / m;        // Step through twiddle table
        
        for (size_t k = 0; k < mSize; k += m)
        {
          for (size_t j = 0; j < halfM; ++j)
          {
            const auto& twiddle = mTwiddles[j * twiddleStep];
            const auto t = twiddle * output[k + j + halfM];
            const auto u = output[k + j];
            output[k + j] = u + t;
            output[k + j + halfM] = u - t;
          }
        }
      }
    }
    
    /**
     * Inverse FFT: frequency domain -> time domain
     * Note: Output is NOT scaled by 1/N - caller must scale
     * @param output Output buffer (must be size mSize)
     * @param input Input buffer (must be size mSize)
     */
    void Inverse(std::complex<double>* output, const std::complex<double>* input) const
    {
      // Bit-reversal permutation copy
      for (size_t i = 0; i < mSize; ++i)
      {
        output[mBitReversed[i]] = input[i];
      }
      
      // Cooley-Tukey iterative IFFT (conjugate twiddles)
      for (size_t stage = 1; stage <= mLog2Size; ++stage)
      {
        const size_t m = size_t{1} << stage;
        const size_t halfM = m >> 1;
        const size_t twiddleStep = mSize / m;
        
        for (size_t k = 0; k < mSize; k += m)
        {
          for (size_t j = 0; j < halfM; ++j)
          {
            // Conjugate twiddle for inverse
            const auto twiddle = std::conj(mTwiddles[j * twiddleStep]);
            const auto t = twiddle * output[k + j + halfM];
            const auto u = output[k + j];
            output[k + j] = u + t;
            output[k + j + halfM] = u - t;
          }
        }
      }
    }
    
    [[nodiscard]] size_t GetSize() const noexcept { return mSize; }
    
  private:
    static size_t BitReverse(size_t x, size_t log2n)
    {
      size_t result = 0;
      for (size_t i = 0; i < log2n; ++i)
      {
        result = (result << 1) | (x & 1);
        x >>= 1;
      }
      return result;
    }
    
    size_t mSize;
    size_t mLog2Size;
    std::vector<std::complex<double>> mTwiddles;
    std::vector<size_t> mBitReversed;
  };

} // namespace namguitar
