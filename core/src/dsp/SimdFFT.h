#pragma once

/**
 * SIMD radix-2 complex FFT: SSE3 on x86, NEON on arm64, scalar elsewhere.
 *
 * - Pre-computed twiddle factors, interleaved [re, im] per stage
 * - The first stage (twiddle 1) is a plain add/subtract pass
 * - NEON de-interleaves with vld2q: four butterflies or multiply-accumulates per step
 * - Aligned memory allocations
 *
 * RealFFT.h builds a real-input transform of twice the size on one of these.
 */

#include <algorithm>
#include <vector>
#include <complex>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>

// Windows ARM64EC first: MSVC defines _M_X64 there too, but the code runs on an arm64 CPU.
#if defined(_M_ARM64EC)
    #define GUITARFX_ARCH_ARM64 1
#elif defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
    #define GUITARFX_ARCH_X86 1
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define GUITARFX_ARCH_ARM64 1
#endif

#if defined(GUITARFX_ARCH_X86) && defined(_MSC_VER)
    #include <intrin.h>
    #include <malloc.h>
#elif defined(GUITARFX_ARCH_X86)
    #include <x86intrin.h>
    #include <cstdlib>
#elif defined(GUITARFX_ARCH_ARM64)
    #include <arm_neon.h>
    #if defined(_MSC_VER)
        #include <malloc.h>
    #else
        #include <cstdlib>
    #endif
#else
    #include <cstdlib>
#endif

#ifndef M_PI
    #define M_PI 3.14159265358979323846
#endif

namespace guitarfx
{
// Memory alignment for SIMD operations
constexpr size_t SIMD_ALIGN = 32; // AVX alignment (works for SSE2 too)

/**
 * SIMD-optimized radix-2 split-radix FFT
 *
 * Uses interleaved real/imaginary format for better SIMD utilization.
 * Data layout: [re0, im0, re1, im1, re2, im2, ...]
 */
class SimdFFT
{
  public:
    explicit SimdFFT(size_t size) : mSize(size), mLog2Size(0), mWorkBuffer(nullptr)
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
        : mSize(other.mSize), mLog2Size(other.mLog2Size), mWorkBuffer(other.mWorkBuffer),
          mTwiddleSizes(std::move(other.mTwiddleSizes)), mTwiddleBuffers(std::move(other.mTwiddleBuffers)),
          mBitReversed(std::move(other.mBitReversed))
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
    void Forward(std::complex<float>* output, const std::complex<float>* input) const
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
            output[i] = std::complex<float>(mWorkBuffer[i * 2], mWorkBuffer[i * 2 + 1]);
        }
    }

    /**
     * Inverse FFT: frequency domain -> time domain
     * Note: Output is NOT scaled - caller must scale by 1/N
     */
    void Inverse(std::complex<float>* output, const std::complex<float>* input) const
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
            output[i] = std::complex<float>(mWorkBuffer[i * 2], mWorkBuffer[i * 2 + 1]);
        }
    }

    // --- Stepped transform ---
    // One transform taken a slice at a time, so its cost can be spread over several audio
    // callbacks: LoadBitReversed over [0, size), then RunButterflies over every stage from 1 to
    // GetNumStages() in order, each over [0, size / 2), then read GetResult(). The arithmetic is
    // Forward()/Inverse()'s exactly (unscaled), as long as butterfly ranges start and end on even
    // indices. Shares the work buffer with Forward()/Inverse(), so one transform at a time.

    void LoadBitReversed(const std::complex<float>* input, size_t begin, size_t end) const
    {
        for (size_t i = begin; i < end; ++i)
        {
            const size_t ri = mBitReversed[i];
            mWorkBuffer[i * 2] = input[ri].real();
            mWorkBuffer[i * 2 + 1] = input[ri].imag();
        }
    }

    void RunButterflies(size_t stage, size_t begin, size_t end, bool inverse) const
    {
        ButterflyRange(mWorkBuffer, stage, begin, end, inverse);
    }

    /// The transform's output, valid once the last stage has run.
    [[nodiscard]] const std::complex<float>* GetResult() const noexcept
    {
        return reinterpret_cast<const std::complex<float>*>(mWorkBuffer);
    }

    [[nodiscard]] size_t GetNumStages() const noexcept
    {
        return mLog2Size;
    }

    /**
     * SIMD-optimized complex multiply-accumulate for convolution
     * acc[k] += a[k] * b[k] for all k
     * This is the hottest path in partitioned convolution.
     */
    static void ComplexMultiplyAccumulate(std::complex<float>* acc, const std::complex<float>* a,
                                          const std::complex<float>* b, size_t count)
    {
#if defined(GUITARFX_ARCH_X86)
        // SSE3 path: process 4 complex<float> per iteration (2x __m128).
        // Layout: [re0, im0, re1, im1] per register.
        // Complex multiply: (ar+ai*j)(br+bi*j) = (ar*br-ai*bi) + (ar*bi+ai*br)*j
        // Using MOVELDUP/MOVEHDUP (SSE3) + ADDSUB (SSE3).

        const float* pa = reinterpret_cast<const float*>(a);
        const float* pb = reinterpret_cast<const float*>(b);
        float* pacc = reinterpret_cast<float*>(acc);

        size_t i = 0;

        // Process 4 complex floats at a time (2x __m128)
        for (; i + 3 < count; i += 4)
        {
            __m128 a0 = _mm_loadu_ps(pa + i * 2);     // [a0r,a0i,a1r,a1i]
            __m128 a1 = _mm_loadu_ps(pa + i * 2 + 4); // [a2r,a2i,a3r,a3i]
            __m128 b0 = _mm_loadu_ps(pb + i * 2);
            __m128 b1 = _mm_loadu_ps(pb + i * 2 + 4);

            __m128 ar0 = _mm_moveldup_ps(a0);                             // [a0r,a0r,a1r,a1r]
            __m128 ai0 = _mm_movehdup_ps(a0);                             // [a0i,a0i,a1i,a1i]
            __m128 bs0 = _mm_shuffle_ps(b0, b0, _MM_SHUFFLE(2, 3, 0, 1)); // [b0i,b0r,b1i,b1r]
            __m128 r0 = _mm_addsub_ps(_mm_mul_ps(ar0, b0), _mm_mul_ps(ai0, bs0));

            __m128 ar1 = _mm_moveldup_ps(a1);
            __m128 ai1 = _mm_movehdup_ps(a1);
            __m128 bs1 = _mm_shuffle_ps(b1, b1, _MM_SHUFFLE(2, 3, 0, 1));
            __m128 r1 = _mm_addsub_ps(_mm_mul_ps(ar1, b1), _mm_mul_ps(ai1, bs1));

            _mm_storeu_ps(pacc + i * 2, _mm_add_ps(_mm_loadu_ps(pacc + i * 2), r0));
            _mm_storeu_ps(pacc + i * 2 + 4, _mm_add_ps(_mm_loadu_ps(pacc + i * 2 + 4), r1));
        }

        // Handle remaining pairs (2 complex floats)
        for (; i + 1 < count; i += 2)
        {
            __m128 av = _mm_loadu_ps(pa + i * 2); // [a0r,a0i,a1r,a1i]
            __m128 bv = _mm_loadu_ps(pb + i * 2);
            __m128 ar = _mm_moveldup_ps(av);
            __m128 ai = _mm_movehdup_ps(av);
            __m128 bs = _mm_shuffle_ps(bv, bv, _MM_SHUFFLE(2, 3, 0, 1));
            __m128 r = _mm_addsub_ps(_mm_mul_ps(ar, bv), _mm_mul_ps(ai, bs));
            _mm_storeu_ps(pacc + i * 2, _mm_add_ps(_mm_loadu_ps(pacc + i * 2), r));
        }

        // Scalar remainder
        for (; i < count; ++i)
        {
            acc[i] += a[i] * b[i];
        }

#elif defined(GUITARFX_ARCH_ARM64)
        // NEON: vld2q splits 4 complex<float> into [re x4] and [im x4], so each product is four
        // fused multiply-adds with no shuffles. Two such groups per iteration.
        const float* pa = reinterpret_cast<const float*>(a);
        const float* pb = reinterpret_cast<const float*>(b);
        float* pacc = reinterpret_cast<float*>(acc);

        size_t i = 0;

        for (; i + 7 < count; i += 8)
        {
            const float32x4x2_t a0 = vld2q_f32(pa + i * 2);
            const float32x4x2_t b0 = vld2q_f32(pb + i * 2);
            const float32x4x2_t a1 = vld2q_f32(pa + i * 2 + 8);
            const float32x4x2_t b1 = vld2q_f32(pb + i * 2 + 8);
            float32x4x2_t c0 = vld2q_f32(pacc + i * 2);
            float32x4x2_t c1 = vld2q_f32(pacc + i * 2 + 8);

            c0.val[0] = vfmsq_f32(vfmaq_f32(c0.val[0], a0.val[0], b0.val[0]), a0.val[1], b0.val[1]);
            c0.val[1] = vfmaq_f32(vfmaq_f32(c0.val[1], a0.val[0], b0.val[1]), a0.val[1], b0.val[0]);
            c1.val[0] = vfmsq_f32(vfmaq_f32(c1.val[0], a1.val[0], b1.val[0]), a1.val[1], b1.val[1]);
            c1.val[1] = vfmaq_f32(vfmaq_f32(c1.val[1], a1.val[0], b1.val[1]), a1.val[1], b1.val[0]);

            vst2q_f32(pacc + i * 2, c0);
            vst2q_f32(pacc + i * 2 + 8, c1);
        }

        for (; i + 3 < count; i += 4)
        {
            const float32x4x2_t av = vld2q_f32(pa + i * 2);
            const float32x4x2_t bv = vld2q_f32(pb + i * 2);
            float32x4x2_t cv = vld2q_f32(pacc + i * 2);
            cv.val[0] = vfmsq_f32(vfmaq_f32(cv.val[0], av.val[0], bv.val[0]), av.val[1], bv.val[1]);
            cv.val[1] = vfmaq_f32(vfmaq_f32(cv.val[1], av.val[0], bv.val[1]), av.val[1], bv.val[0]);
            vst2q_f32(pacc + i * 2, cv);
        }

        for (; i < count; ++i)
        {
            acc[i] += a[i] * b[i];
        }

#else

        for (size_t i = 0; i < count; ++i)
        {
            acc[i] += a[i] * b[i];
        }

#endif
    }

    /**
     * SIMD-optimized buffer clear
     */
    static void ClearBuffer(std::complex<float>* buffer, size_t count)
    {
        float* p = reinterpret_cast<float*>(buffer);
        const size_t floatCount = count * 2;

#if defined(GUITARFX_ARCH_X86)
        const __m128 zero = _mm_setzero_ps();

        size_t i = 0;

        for (; i + 16 <= floatCount; i += 16)
        {
            _mm_storeu_ps(p + i, zero);
            _mm_storeu_ps(p + i + 4, zero);
            _mm_storeu_ps(p + i + 8, zero);
            _mm_storeu_ps(p + i + 12, zero);
        }

        for (; i < floatCount; ++i)
        {
            p[i] = 0.0f;
        }

#else

        for (size_t i = 0; i < floatCount; ++i)
        {
            p[i] = 0.0f;
        }

#endif
    }

    [[nodiscard]] size_t GetSize() const noexcept
    {
        return mSize;
    }

  private:
    // Reads and writes the work buffer in bit-reversed order directly, so its input and output
    // passes need no extra copy.
    friend class RealFFT;

    static float* AllocateAligned(size_t count)
    {
#ifdef _MSC_VER
        return static_cast<float*>(_aligned_malloc(count * sizeof(float), SIMD_ALIGN));
#else
        void* ptr = nullptr;

        if (posix_memalign(&ptr, SIMD_ALIGN, count * sizeof(float)) != 0)
        {
            return nullptr;
        }

        return static_cast<float*>(ptr);
#endif
    }

    static void FreeAligned(float* ptr)
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

            const float angleStep = static_cast<float>(-2.0 * M_PI / static_cast<double>(m));

            for (size_t j = 0; j < halfM; ++j)
            {
                const float angle = angleStep * static_cast<float>(j);
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

    void FFTCore(float* data, bool inverse) const
    {
        // Cooley-Tukey iterative FFT with SIMD butterflies (float/SSE3)
        for (size_t stage = 1; stage <= mLog2Size; ++stage)
        {
            ButterflyRange(data, stage, 0, mSize / 2, inverse);
        }
    }

    // Butterflies [begin, end) of one stage, numbered in the order FFTCore runs them: group by
    // group (each m = 2^stage points wide), and within a group by twiddle index j.
    void ButterflyRange(float* data, size_t stage, size_t begin, size_t end, bool inverse) const
    {
        if (stage == 1)
        {
            FirstStageRange(data, begin, end);
            return;
        }

        const size_t m = size_t{1} << stage;
        const size_t halfM = m >> 1;
        const float* twiddles = mTwiddleBuffers[stage - 1];
        const float invSign = inverse ? -1.0f : 1.0f;

        // Butterfly b is twiddle j = b % halfM of the group starting at point k = (b / halfM) * m.
        size_t k = (begin >> (stage - 1)) << stage;
        size_t jStart = begin & (halfM - 1);

        for (size_t b = begin; b < end; k += m, jStart = 0)
        {
            size_t j = jStart;
            const size_t jEnd = std::min(halfM, j + (end - b));
            b += jEnd - j;

#if defined(GUITARFX_ARCH_X86)

            // SSE3: process 2 butterflies per iteration using one __m128 per pair.
            // idx0 and idx1=idx0+2 are adjacent; idx0h and idx1h=idx0h+2 are adjacent,
            // so we can load/store 2 complex floats in a single 128-bit op.
            for (; j + 1 < jEnd; j += 2)
            {
                const size_t idx0 = (k + j) * 2;
                const size_t idx0h = (k + j + halfM) * 2;

                // Load twiddles for butterfly j and j+1: [wr0,wi0,wr1,wi1]
                __m128 tw = _mm_loadu_ps(twiddles + j * 2);

                // Negate imaginary parts of twiddle for inverse FFT: [wr0,-wi0,wr1,-wi1]
                if (inverse)
                {
                    const __m128 signMask = _mm_set_ps(-1.0f, 1.0f, -1.0f, 1.0f);
                    tw = _mm_mul_ps(tw, signMask);
                }

                // Load t = [tr0,ti0,tr1,ti1] and u = [ur0,ui0,ur1,ui1]
                __m128 t = _mm_loadu_ps(data + idx0h);
                __m128 u = _mm_loadu_ps(data + idx0);

                // Complex multiply t * tw for both pairs using MOVELDUP/MOVEHDUP (SSE3)
                __m128 tr = _mm_moveldup_ps(t);                                   // [tr0,tr0,tr1,tr1]
                __m128 ti = _mm_movehdup_ps(t);                                   // [ti0,ti0,ti1,ti1]
                __m128 tw_swap = _mm_shuffle_ps(tw, tw, _MM_SHUFFLE(2, 3, 0, 1)); // [wi0,wr0,wi1,wr1]
                __m128 twt = _mm_addsub_ps(_mm_mul_ps(tr, tw), _mm_mul_ps(ti, tw_swap));

                // Butterfly: lo = u+twt, hi = u-twt
                _mm_storeu_ps(data + idx0, _mm_add_ps(u, twt));
                _mm_storeu_ps(data + idx0h, _mm_sub_ps(u, twt));
            }

#elif defined(GUITARFX_ARCH_ARM64)

            // NEON: 4 butterflies per iteration, de-interleaved by vld2q into [re x4] and [im x4]
            // so the twiddle multiply is plain fused multiply-adds. Needs halfM >= 4.
            const float32x4_t sign = vdupq_n_f32(invSign);

            for (; j + 3 < jEnd; j += 4)
            {
                const size_t idx = (k + j) * 2;
                const size_t idxh = (k + j + halfM) * 2;

                const float32x4x2_t w = vld2q_f32(twiddles + j * 2);
                const float32x4_t wr = w.val[0];
                const float32x4_t wi = vmulq_f32(w.val[1], sign);
                const float32x4x2_t t = vld2q_f32(data + idxh);
                float32x4x2_t u = vld2q_f32(data + idx);

                const float32x4_t twr = vfmsq_f32(vmulq_f32(t.val[0], wr), t.val[1], wi);
                const float32x4_t twi = vfmaq_f32(vmulq_f32(t.val[0], wi), t.val[1], wr);

                float32x4x2_t h;
                h.val[0] = vsubq_f32(u.val[0], twr);
                h.val[1] = vsubq_f32(u.val[1], twi);
                u.val[0] = vaddq_f32(u.val[0], twr);
                u.val[1] = vaddq_f32(u.val[1], twi);
                vst2q_f32(data + idx, u);
                vst2q_f32(data + idxh, h);
            }

            // The second stage (halfM == 2): both butterflies of a group in one interleaved register.
            static constexpr float kAlternate[4] = {-1.0f, 1.0f, -1.0f, 1.0f};
            static constexpr float kConjugate[4] = {1.0f, -1.0f, 1.0f, -1.0f};

            for (; j + 1 < jEnd; j += 2)
            {
                const size_t idx = (k + j) * 2;
                const size_t idxh = (k + j + halfM) * 2;

                float32x4_t tw = vld1q_f32(twiddles + j * 2); // [wr0, wi0, wr1, wi1]

                if (inverse)
                {
                    tw = vmulq_f32(tw, vld1q_f32(kConjugate));
                }

                const float32x4_t t = vld1q_f32(data + idxh);
                const float32x4_t u = vld1q_f32(data + idx);
                const float32x4_t tr = vtrn1q_f32(t, t);    // [tr0, tr0, tr1, tr1]
                const float32x4_t ti = vtrn2q_f32(t, t);    // [ti0, ti0, ti1, ti1]
                const float32x4_t twSwap = vrev64q_f32(tw); // [wi0, wr0, wi1, wr1]

                // [tr*wr - ti*wi, tr*wi + ti*wr] for both
                const float32x4_t twt = vfmaq_f32(vmulq_f32(tr, tw), vmulq_f32(ti, vld1q_f32(kAlternate)), twSwap);
                vst1q_f32(data + idx, vaddq_f32(u, twt));
                vst1q_f32(data + idxh, vsubq_f32(u, twt));
            }

#endif

            // The rest: every butterfly without SIMD, else an odd range end
            for (; j < jEnd; ++j)
            {
                const size_t idx = (k + j) * 2;
                const size_t idxh = (k + j + halfM) * 2;

                const float wr = twiddles[j * 2];
                const float wi = twiddles[j * 2 + 1] * invSign;

                const float ur = data[idx];
                const float ui = data[idx + 1];
                const float tr = data[idxh];
                const float ti = data[idxh + 1];

                const float twr = tr * wr - ti * wi;
                const float twi = tr * wi + ti * wr;

                data[idx] = ur + twr;
                data[idx + 1] = ui + twi;
                data[idxh] = ur - twr;
                data[idxh + 1] = ui - twi;
            }
        }
    }

    // The first stage's butterflies [begin, end): its twiddle is 1 both ways, so butterfly b is
    // the sum and difference of points 2b and 2b + 1, with no multiply.
    static void FirstStageRange(float* data, size_t begin, size_t end)
    {
        size_t b = begin;

#if defined(GUITARFX_ARCH_X86)

        for (; b + 1 < end; b += 2)
        {
            const __m128 a = _mm_loadu_ps(data + b * 4);     // [p0, p1]
            const __m128 c = _mm_loadu_ps(data + b * 4 + 4); // [p2, p3]
            const __m128 u = _mm_movelh_ps(a, c);            // [p0, p2]
            const __m128 t = _mm_movehl_ps(c, a);            // [p1, p3]
            const __m128 lo = _mm_add_ps(u, t);
            const __m128 hi = _mm_sub_ps(u, t);
            _mm_storeu_ps(data + b * 4, _mm_movelh_ps(lo, hi));
            _mm_storeu_ps(data + b * 4 + 4, _mm_movehl_ps(hi, lo));
        }

#elif defined(GUITARFX_ARCH_ARM64)

        for (; b + 1 < end; b += 2)
        {
            const float32x4_t a = vld1q_f32(data + b * 4);     // [p0, p1]
            const float32x4_t c = vld1q_f32(data + b * 4 + 4); // [p2, p3]
            const float32x4_t u = vcombine_f32(vget_low_f32(a), vget_low_f32(c));
            const float32x4_t t = vcombine_f32(vget_high_f32(a), vget_high_f32(c));
            const float32x4_t lo = vaddq_f32(u, t);
            const float32x4_t hi = vsubq_f32(u, t);
            vst1q_f32(data + b * 4, vcombine_f32(vget_low_f32(lo), vget_low_f32(hi)));
            vst1q_f32(data + b * 4 + 4, vcombine_f32(vget_high_f32(lo), vget_high_f32(hi)));
        }

#endif

        for (; b < end; ++b)
        {
            float* p = data + b * 4;
            const float ur = p[0];
            const float ui = p[1];
            const float tr = p[2];
            const float ti = p[3];
            p[0] = ur + tr;
            p[1] = ui + ti;
            p[2] = ur - tr;
            p[3] = ui - ti;
        }
    }

    size_t mSize;
    size_t mLog2Size;
    mutable float* mWorkBuffer;
    std::vector<size_t> mTwiddleSizes;
    std::vector<float*> mTwiddleBuffers;
    std::vector<size_t> mBitReversed;
};
} // namespace guitarfx
