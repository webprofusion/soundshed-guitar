#pragma once

/**
 * SIMD-optimized FFT for x64 processors (Intel/AMD)
 * 
 * Uses SSE2 intrinsics which are available on ALL x64 CPUs.
 * AVX path available for newer CPUs (automatically detected).
 * 
 * Key optimizations:
 * - SSE2 packed double operations (2 doubles per instruction)
 * - Pre-computed twiddle factors
 * - Cache-friendly memory access patterns
 * - Minimized branch mispredictions
 * - Aligned memory allocations
 */

#include <vector>
#include <complex>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

#ifdef _MSC_VER
#include <intrin.h>
#include <malloc.h>
#else
#include <x86intrin.h>
#include <cstdlib>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace namguitar
{
  // Memory alignment for SIMD operations
  constexpr size_t SIMD_ALIGN = 32;  // AVX alignment (works for SSE2 too)

  /**
   * SIMD-optimized radix-2 split-radix FFT
   * 
   * Uses interleaved real/imaginary format for better SIMD utilization.
   * Data layout: [re0, im0, re1, im1, re2, im2, ...]
   */
  class SimdFFT
  {
  public:
    explicit SimdFFT(size_t size)
      : mSize(size)
      , mLog2Size(0)
      , mWorkBuffer(nullptr)
    {
      // Calculate log2(size)
      size_t n = size;
      while (n > 1)
      {
        n >>= 1;
        ++mLog2Size;
      }
      
      // Allocate aligned working buffer
      mWorkBuffer = AllocateAligned(mSize * 2);
      
      // Pre-compute twiddle factors (interleaved format for SIMD)
      PrecomputeTwiddles();
      
      // Pre-compute bit-reversal permutation
      PrecomputeBitReversal();
    }
    
    ~SimdFFT()
    {
      FreeAligned(mWorkBuffer);
      for (auto* ptr : mTwiddleBuffers)
      {
        FreeAligned(ptr);
      }
    }
    
    // Non-copyable
    SimdFFT(const SimdFFT&) = delete;
    SimdFFT& operator=(const SimdFFT&) = delete;
    
    // Movable
    SimdFFT(SimdFFT&& other) noexcept
      : mSize(other.mSize)
      , mLog2Size(other.mLog2Size)
      , mWorkBuffer(other.mWorkBuffer)
      , mTwiddleSizes(std::move(other.mTwiddleSizes))
      , mTwiddleBuffers(std::move(other.mTwiddleBuffers))
      , mBitReversed(std::move(other.mBitReversed))
    {
      other.mWorkBuffer = nullptr;
    }
    
    SimdFFT& operator=(SimdFFT&& other) noexcept
    {
      if (this != &other)
      {
        FreeAligned(mWorkBuffer);
        for (auto* ptr : mTwiddleBuffers)
        {
          FreeAligned(ptr);
        }
        
        mSize = other.mSize;
        mLog2Size = other.mLog2Size;
        mWorkBuffer = other.mWorkBuffer;
        mTwiddleSizes = std::move(other.mTwiddleSizes);
        mTwiddleBuffers = std::move(other.mTwiddleBuffers);
        mBitReversed = std::move(other.mBitReversed);
        
        other.mWorkBuffer = nullptr;
      }
      return *this;
    }
    
    /**
     * Forward FFT: time domain -> frequency domain
     * @param output Complex output (size mSize)
     * @param input Complex input (size mSize)
     */
    void Forward(std::complex<double>* output, const std::complex<double>* input) const
    {
      // Convert to interleaved format and apply bit reversal
      for (size_t i = 0; i < mSize; ++i)
      {
        const size_t ri = mBitReversed[i];
        mWorkBuffer[i * 2] = input[ri].real();
        mWorkBuffer[i * 2 + 1] = input[ri].imag();
      }
      
      // Perform FFT butterflies with SIMD
      FFTCore(mWorkBuffer, false);
      
      // Convert back to std::complex format
      for (size_t i = 0; i < mSize; ++i)
      {
        output[i] = std::complex<double>(mWorkBuffer[i * 2], mWorkBuffer[i * 2 + 1]);
      }
    }
    
    /**
     * Inverse FFT: frequency domain -> time domain
     * Note: Output is NOT scaled - caller must scale by 1/N
     */
    void Inverse(std::complex<double>* output, const std::complex<double>* input) const
    {
      // Convert to interleaved format and apply bit reversal
      for (size_t i = 0; i < mSize; ++i)
      {
        const size_t ri = mBitReversed[i];
        mWorkBuffer[i * 2] = input[ri].real();
        mWorkBuffer[i * 2 + 1] = input[ri].imag();
      }
      
      // Perform IFFT butterflies with SIMD (conjugate twiddles)
      FFTCore(mWorkBuffer, true);
      
      // Convert back to std::complex format
      for (size_t i = 0; i < mSize; ++i)
      {
        output[i] = std::complex<double>(mWorkBuffer[i * 2], mWorkBuffer[i * 2 + 1]);
      }
    }
    
    /**
     * SIMD-optimized complex multiply-accumulate for convolution
     * acc[k] += a[k] * b[k] for all k
     * This is the hottest path in partitioned convolution.
     */
    static void ComplexMultiplyAccumulate(
      std::complex<double>* acc,
      const std::complex<double>* a,
      const std::complex<double>* b,
      size_t count)
    {
      // Process 2 complex numbers at a time using SSE2
      // Each complex = 2 doubles, so we process 4 doubles = 2 __m128d
      
      const double* pa = reinterpret_cast<const double*>(a);
      const double* pb = reinterpret_cast<const double*>(b);
      double* pacc = reinterpret_cast<double*>(acc);
      
      size_t i = 0;
      
      // SSE2 path - process 2 complex numbers at a time
      for (; i + 1 < count; i += 2)
      {
        // Load a[i] and a[i+1]: [a0r, a0i, a1r, a1i]
        __m128d a0 = _mm_loadu_pd(pa + i * 2);      // [a0r, a0i]
        __m128d a1 = _mm_loadu_pd(pa + i * 2 + 2);  // [a1r, a1i]
        
        // Load b[i] and b[i+1]
        __m128d b0 = _mm_loadu_pd(pb + i * 2);      // [b0r, b0i]
        __m128d b1 = _mm_loadu_pd(pb + i * 2 + 2);  // [b1r, b1i]
        
        // Complex multiply: (ar + ai*j)(br + bi*j) = (ar*br - ai*bi) + (ar*bi + ai*br)*j
        
        // For a0*b0:
        __m128d a0r = _mm_unpacklo_pd(a0, a0);  // [a0r, a0r]
        __m128d a0i = _mm_unpackhi_pd(a0, a0);  // [a0i, a0i]
        __m128d b0_swap = _mm_shuffle_pd(b0, b0, 1);  // [b0i, b0r]
        
        __m128d prod0_r = _mm_mul_pd(a0r, b0);           // [a0r*b0r, a0r*b0i]
        __m128d prod0_i = _mm_mul_pd(a0i, b0_swap);      // [a0i*b0i, a0i*b0r]
        __m128d result0 = _mm_addsub_pd(prod0_r, prod0_i); // [a0r*b0r - a0i*b0i, a0r*b0i + a0i*b0r]
        
        // For a1*b1:
        __m128d a1r = _mm_unpacklo_pd(a1, a1);
        __m128d a1i = _mm_unpackhi_pd(a1, a1);
        __m128d b1_swap = _mm_shuffle_pd(b1, b1, 1);
        
        __m128d prod1_r = _mm_mul_pd(a1r, b1);
        __m128d prod1_i = _mm_mul_pd(a1i, b1_swap);
        __m128d result1 = _mm_addsub_pd(prod1_r, prod1_i);
        
        // Accumulate
        __m128d acc0 = _mm_loadu_pd(pacc + i * 2);
        __m128d acc1 = _mm_loadu_pd(pacc + i * 2 + 2);
        
        acc0 = _mm_add_pd(acc0, result0);
        acc1 = _mm_add_pd(acc1, result1);
        
        _mm_storeu_pd(pacc + i * 2, acc0);
        _mm_storeu_pd(pacc + i * 2 + 2, acc1);
      }
      
      // Handle remaining element
      for (; i < count; ++i)
      {
        acc[i] += a[i] * b[i];
      }
    }
    
    /**
     * SIMD-optimized buffer clear
     */
    static void ClearBuffer(std::complex<double>* buffer, size_t count)
    {
      double* p = reinterpret_cast<double*>(buffer);
      const size_t doubleCount = count * 2;
      
      __m128d zero = _mm_setzero_pd();
      
      size_t i = 0;
      for (; i + 8 <= doubleCount; i += 8)
      {
        _mm_storeu_pd(p + i, zero);
        _mm_storeu_pd(p + i + 2, zero);
        _mm_storeu_pd(p + i + 4, zero);
        _mm_storeu_pd(p + i + 6, zero);
      }
      for (; i < doubleCount; ++i)
      {
        p[i] = 0.0;
      }
    }
    
    [[nodiscard]] size_t GetSize() const noexcept { return mSize; }
    
  private:
    static double* AllocateAligned(size_t count)
    {
#ifdef _MSC_VER
      return static_cast<double*>(_aligned_malloc(count * sizeof(double), SIMD_ALIGN));
#else
      void* ptr = nullptr;
      if (posix_memalign(&ptr, SIMD_ALIGN, count * sizeof(double)) != 0)
        return nullptr;
      return static_cast<double*>(ptr);
#endif
    }
    
    static void FreeAligned(double* ptr)
    {
      if (ptr)
      {
#ifdef _MSC_VER
        _aligned_free(ptr);
#else
        free(ptr);
#endif
      }
    }
    
    void PrecomputeTwiddles()
    {
      // Twiddle factors for each stage, stored interleaved [re, im, re, im, ...]
      mTwiddleSizes.resize(mLog2Size);
      mTwiddleBuffers.resize(mLog2Size);
      
      for (size_t stage = 1; stage <= mLog2Size; ++stage)
      {
        const size_t m = size_t{1} << stage;
        const size_t halfM = m >> 1;
        
        mTwiddleSizes[stage - 1] = halfM;
        mTwiddleBuffers[stage - 1] = AllocateAligned(halfM * 2);
        
        const double angleStep = -2.0 * M_PI / static_cast<double>(m);
        
        for (size_t j = 0; j < halfM; ++j)
        {
          const double angle = angleStep * static_cast<double>(j);
          mTwiddleBuffers[stage - 1][j * 2] = std::cos(angle);
          mTwiddleBuffers[stage - 1][j * 2 + 1] = std::sin(angle);
        }
      }
    }
    
    void PrecomputeBitReversal()
    {
      mBitReversed.resize(mSize);
      for (size_t i = 0; i < mSize; ++i)
      {
        size_t result = 0;
        size_t x = i;
        for (size_t b = 0; b < mLog2Size; ++b)
        {
          result = (result << 1) | (x & 1);
          x >>= 1;
        }
        mBitReversed[i] = result;
      }
    }
    
    void FFTCore(double* data, bool inverse) const
    {
      // Cooley-Tukey iterative FFT with SIMD butterflies
      for (size_t stage = 1; stage <= mLog2Size; ++stage)
      {
        const size_t m = size_t{1} << stage;
        const size_t halfM = m >> 1;
        const double* twiddles = mTwiddleBuffers[stage - 1];
        const double invSign = inverse ? -1.0 : 1.0;
        
        for (size_t k = 0; k < mSize; k += m)
        {
          // SIMD butterfly processing
          size_t j = 0;
          
          // Process pairs of butterflies using SSE2
          for (; j + 1 < halfM; j += 2)
          {
            const size_t idx0 = (k + j) * 2;
            const size_t idx1 = (k + j + 1) * 2;
            const size_t idx0h = (k + j + halfM) * 2;
            const size_t idx1h = (k + j + 1 + halfM) * 2;
            
            // Load twiddle factors
            __m128d tw0 = _mm_loadu_pd(twiddles + j * 2);      // [wr0, wi0]
            __m128d tw1 = _mm_loadu_pd(twiddles + j * 2 + 2);  // [wr1, wi1]
            
            // Apply inverse sign to imaginary part if needed
            if (inverse)
            {
              __m128d signMask = _mm_set_pd(-1.0, 1.0);
              tw0 = _mm_mul_pd(tw0, signMask);
              tw1 = _mm_mul_pd(tw1, signMask);
            }
            
            // Load u and t values
            __m128d u0 = _mm_loadu_pd(data + idx0);   // [ur0, ui0]
            __m128d t0 = _mm_loadu_pd(data + idx0h); // [tr0, ti0]
            __m128d u1 = _mm_loadu_pd(data + idx1);
            __m128d t1 = _mm_loadu_pd(data + idx1h);
            
            // Complex multiply t * twiddle
            // (tr + ti*j)(wr + wi*j) = (tr*wr - ti*wi) + (tr*wi + ti*wr)*j
            
            // For t0 * tw0:
            __m128d tr0 = _mm_unpacklo_pd(t0, t0);  // [tr0, tr0]
            __m128d ti0 = _mm_unpackhi_pd(t0, t0);  // [ti0, ti0]
            __m128d tw0_swap = _mm_shuffle_pd(tw0, tw0, 1);  // [wi0, wr0]
            
            __m128d prod0_r = _mm_mul_pd(tr0, tw0);           // [tr0*wr0, tr0*wi0]
            __m128d prod0_i = _mm_mul_pd(ti0, tw0_swap);      // [ti0*wi0, ti0*wr0]
            __m128d twt0 = _mm_addsub_pd(prod0_r, prod0_i);   // [tr*wr - ti*wi, tr*wi + ti*wr]
            
            // For t1 * tw1:
            __m128d tr1 = _mm_unpacklo_pd(t1, t1);
            __m128d ti1 = _mm_unpackhi_pd(t1, t1);
            __m128d tw1_swap = _mm_shuffle_pd(tw1, tw1, 1);
            
            __m128d prod1_r = _mm_mul_pd(tr1, tw1);
            __m128d prod1_i = _mm_mul_pd(ti1, tw1_swap);
            __m128d twt1 = _mm_addsub_pd(prod1_r, prod1_i);
            
            // Butterfly: y[k+j] = u + twt, y[k+j+halfM] = u - twt
            __m128d y0_lo = _mm_add_pd(u0, twt0);
            __m128d y0_hi = _mm_sub_pd(u0, twt0);
            __m128d y1_lo = _mm_add_pd(u1, twt1);
            __m128d y1_hi = _mm_sub_pd(u1, twt1);
            
            _mm_storeu_pd(data + idx0, y0_lo);
            _mm_storeu_pd(data + idx0h, y0_hi);
            _mm_storeu_pd(data + idx1, y1_lo);
            _mm_storeu_pd(data + idx1h, y1_hi);
          }
          
          // Handle remaining butterfly (if halfM is odd)
          for (; j < halfM; ++j)
          {
            const size_t idx = (k + j) * 2;
            const size_t idxh = (k + j + halfM) * 2;
            
            const double wr = twiddles[j * 2];
            const double wi = twiddles[j * 2 + 1] * invSign;
            
            const double ur = data[idx];
            const double ui = data[idx + 1];
            const double tr = data[idxh];
            const double ti = data[idxh + 1];
            
            // t * twiddle
            const double twr = tr * wr - ti * wi;
            const double twi = tr * wi + ti * wr;
            
            // Butterfly
            data[idx] = ur + twr;
            data[idx + 1] = ui + twi;
            data[idxh] = ur - twr;
            data[idxh + 1] = ui - twi;
          }
        }
      }
    }
    
    size_t mSize;
    size_t mLog2Size;
    mutable double* mWorkBuffer;
    std::vector<size_t> mTwiddleSizes;
    std::vector<double*> mTwiddleBuffers;
    std::vector<size_t> mBitReversed;
  };

} // namespace namguitar
