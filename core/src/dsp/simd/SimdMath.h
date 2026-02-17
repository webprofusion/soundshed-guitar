#pragma once

/**
 * SIMD-optimized math functions for neural network inference.
 * Provides AVX2, SSE, and scalar fallbacks for activation functions.
 */

#include <cmath>
#include <cstdint>

// Architecture and platform detection
#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
  #define GUITARFX_ARCH_X86 1
#endif

#if defined(GUITARFX_ARCH_X86)
  #if defined(_MSC_VER)
    #include <intrin.h>
    #define GUITARFX_MSVC 1
  #elif defined(__GNUC__) || defined(__clang__)
    #include <x86intrin.h>
    #define GUITARFX_GCC_CLANG 1
  #endif
#endif

// SIMD feature detection
#if defined(GUITARFX_ARCH_X86) && (defined(__AVX2__) || (defined(_MSC_VER) && defined(__AVX2__)))
  #define GUITARFX_HAS_AVX2 1
#endif

#if defined(GUITARFX_ARCH_X86) && (defined(__AVX__) || (defined(_MSC_VER) && defined(__AVX__)))
  #define GUITARFX_HAS_AVX 1
#endif

#if defined(GUITARFX_ARCH_X86) && (defined(__SSE4_1__) || defined(__SSE4_2__) || (defined(_MSC_VER) && (defined(__SSE4_1__) || defined(__SSE4_2__))))
  #define GUITARFX_HAS_SSE4 1
#endif

#if defined(GUITARFX_ARCH_X86) && (defined(__SSE2__) || (defined(_MSC_VER) && defined(_M_X64)))
  #define GUITARFX_HAS_SSE2 1
#endif

// Force inline hint
#if defined(_MSC_VER)
  #define GUITARFX_FORCE_INLINE __forceinline
#else
  #define GUITARFX_FORCE_INLINE inline __attribute__((always_inline))
#endif

namespace guitarfx
{
namespace simd
{

// ============================================================================
// Runtime SIMD Detection
// ============================================================================

enum class SimdLevel
{
  Scalar = 0,
  SSE2 = 1,
  SSE4 = 2,
  AVX = 3,
  AVX2 = 4,
  AVX512 = 5
};

inline SimdLevel DetectSimdLevel()
{
#if defined(GUITARFX_MSVC)
  int cpuInfo[4];
  __cpuid(cpuInfo, 0);
  int nIds = cpuInfo[0];

  if (nIds >= 7)
  {
    __cpuidex(cpuInfo, 7, 0);
    if (cpuInfo[1] & (1 << 5)) // AVX2
      return SimdLevel::AVX2;
  }

  if (nIds >= 1)
  {
    __cpuid(cpuInfo, 1);
    if (cpuInfo[2] & (1 << 28)) // AVX
      return SimdLevel::AVX;
    if (cpuInfo[2] & (1 << 19)) // SSE4.1
      return SimdLevel::SSE4;
    if (cpuInfo[3] & (1 << 26)) // SSE2
      return SimdLevel::SSE2;
  }
#elif defined(GUITARFX_GCC_CLANG)
  // GCC/Clang typically have these as compile-time defines
  #if defined(__AVX2__)
    return SimdLevel::AVX2;
  #elif defined(__AVX__)
    return SimdLevel::AVX;
  #elif defined(__SSE4_1__)
    return SimdLevel::SSE4;
  #elif defined(__SSE2__)
    return SimdLevel::SSE2;
  #endif
#endif
  return SimdLevel::Scalar;
}

// Cache the detected level
inline SimdLevel GetSimdLevel()
{
  static SimdLevel level = DetectSimdLevel();
  return level;
}

// ============================================================================
// Scalar Math Functions (Baseline)
// ============================================================================

namespace scalar
{

GUITARFX_FORCE_INLINE float FastTanh(float x)
{
  // Pade approximant - accurate for |x| < 4, clamps otherwise
  const float ax = std::abs(x);
  const float x2 = x * x;

  return (x * (2.45550750702956f + 2.45550750702956f * ax + (0.893229853513558f + 0.821226666969744f * ax) * x2)
          / (2.44506634652299f + (2.44506634652299f + x2) * std::abs(x + 0.814642734961073f * x * ax)));
}

GUITARFX_FORCE_INLINE float FastSigmoid(float x)
{
  return 0.5f * (FastTanh(x * 0.5f) + 1.0f);
}

GUITARFX_FORCE_INLINE float FastExp(float x)
{
  // Fast exp approximation using bit manipulation
  // Valid for x in [-87, 88] approximately
  x = 1.0f + x / 256.0f;
  x *= x; x *= x; x *= x; x *= x;
  x *= x; x *= x; x *= x; x *= x;
  return x;
}

GUITARFX_FORCE_INLINE float ReLU(float x)
{
  return x > 0.0f ? x : 0.0f;
}

GUITARFX_FORCE_INLINE float LeakyReLU(float x, float alpha = 0.01f)
{
  return x > 0.0f ? x : alpha * x;
}

GUITARFX_FORCE_INLINE float HardTanh(float x)
{
  return x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
}

// Apply activation to array (scalar fallback)
inline void ApplyTanh(float* data, long size)
{
  for (long i = 0; i < size; ++i)
    data[i] = FastTanh(data[i]);
}

inline void ApplySigmoid(float* data, long size)
{
  for (long i = 0; i < size; ++i)
    data[i] = FastSigmoid(data[i]);
}

inline void ApplyReLU(float* data, long size)
{
  for (long i = 0; i < size; ++i)
    data[i] = ReLU(data[i]);
}

} // namespace scalar

// ============================================================================
// SSE2 Implementations
// ============================================================================

#ifdef GUITARFX_HAS_SSE2

namespace sse2
{

// SSE2 fast tanh using polynomial approximation
GUITARFX_FORCE_INLINE __m128 FastTanh(__m128 x)
{
  // Clamp input to prevent overflow
  const __m128 limit = _mm_set1_ps(4.0f);
  x = _mm_max_ps(_mm_min_ps(x, limit), _mm_sub_ps(_mm_setzero_ps(), limit));

  // Compute x^2
  __m128 x2 = _mm_mul_ps(x, x);

  // Numerator: x * (27 + x^2)
  __m128 num = _mm_add_ps(_mm_set1_ps(27.0f), x2);
  num = _mm_mul_ps(x, num);

  // Denominator: 27 + 9*x^2
  __m128 den = _mm_add_ps(_mm_set1_ps(27.0f), _mm_mul_ps(_mm_set1_ps(9.0f), x2));

  // Divide
  return _mm_div_ps(num, den);
}

GUITARFX_FORCE_INLINE __m128 FastSigmoid(__m128 x)
{
  // sigmoid(x) = 0.5 * (tanh(x/2) + 1)
  __m128 half = _mm_set1_ps(0.5f);
  __m128 one = _mm_set1_ps(1.0f);
  return _mm_mul_ps(half, _mm_add_ps(FastTanh(_mm_mul_ps(x, half)), one));
}

GUITARFX_FORCE_INLINE __m128 ReLU(__m128 x)
{
  return _mm_max_ps(x, _mm_setzero_ps());
}

GUITARFX_FORCE_INLINE __m128 LeakyReLU(__m128 x, __m128 alpha)
{
  __m128 pos = _mm_max_ps(x, _mm_setzero_ps());
  __m128 neg = _mm_mul_ps(alpha, _mm_min_ps(x, _mm_setzero_ps()));
  return _mm_add_ps(pos, neg);
}

inline void ApplyTanh(float* data, long size)
{
  long i = 0;

  // Process 4 floats at a time
  for (; i + 4 <= size; i += 4)
  {
    __m128 v = _mm_loadu_ps(data + i);
    v = FastTanh(v);
    _mm_storeu_ps(data + i, v);
  }

  // Handle remainder
  for (; i < size; ++i)
    data[i] = scalar::FastTanh(data[i]);
}

inline void ApplySigmoid(float* data, long size)
{
  long i = 0;

  for (; i + 4 <= size; i += 4)
  {
    __m128 v = _mm_loadu_ps(data + i);
    v = FastSigmoid(v);
    _mm_storeu_ps(data + i, v);
  }

  for (; i < size; ++i)
    data[i] = scalar::FastSigmoid(data[i]);
}

inline void ApplyReLU(float* data, long size)
{
  long i = 0;
  __m128 zero = _mm_setzero_ps();

  for (; i + 4 <= size; i += 4)
  {
    __m128 v = _mm_loadu_ps(data + i);
    v = _mm_max_ps(v, zero);
    _mm_storeu_ps(data + i, v);
  }

  for (; i < size; ++i)
    data[i] = scalar::ReLU(data[i]);
}

// Fused gated activation: output = tanh(x) * sigmoid(gate)
inline void ApplyGatedActivation(float* data, float* gate, long size)
{
  long i = 0;

  for (; i + 4 <= size; i += 4)
  {
    __m128 x = _mm_loadu_ps(data + i);
    __m128 g = _mm_loadu_ps(gate + i);

    x = FastTanh(x);
    g = FastSigmoid(g);

    __m128 result = _mm_mul_ps(x, g);
    _mm_storeu_ps(data + i, result);
  }

  for (; i < size; ++i)
    data[i] = scalar::FastTanh(data[i]) * scalar::FastSigmoid(gate[i]);
}

} // namespace sse2

#endif // GUITARFX_HAS_SSE2

// ============================================================================
// AVX/AVX2 Implementations
// ============================================================================

#ifdef GUITARFX_HAS_AVX

namespace avx
{

GUITARFX_FORCE_INLINE __m256 FastTanh(__m256 x)
{
  // Clamp input
  const __m256 limit = _mm256_set1_ps(4.0f);
  x = _mm256_max_ps(_mm256_min_ps(x, limit), _mm256_sub_ps(_mm256_setzero_ps(), limit));

  // x^2
  __m256 x2 = _mm256_mul_ps(x, x);

  // Numerator: x * (27 + x^2)
  __m256 num = _mm256_add_ps(_mm256_set1_ps(27.0f), x2);
  num = _mm256_mul_ps(x, num);

  // Denominator: 27 + 9*x^2
  __m256 den = _mm256_add_ps(_mm256_set1_ps(27.0f), _mm256_mul_ps(_mm256_set1_ps(9.0f), x2));

  return _mm256_div_ps(num, den);
}

GUITARFX_FORCE_INLINE __m256 FastSigmoid(__m256 x)
{
  __m256 half = _mm256_set1_ps(0.5f);
  __m256 one = _mm256_set1_ps(1.0f);
  return _mm256_mul_ps(half, _mm256_add_ps(FastTanh(_mm256_mul_ps(x, half)), one));
}

GUITARFX_FORCE_INLINE __m256 ReLU(__m256 x)
{
  return _mm256_max_ps(x, _mm256_setzero_ps());
}

inline void ApplyTanh(float* data, long size)
{
  long i = 0;

  // Process 8 floats at a time with AVX
  for (; i + 8 <= size; i += 8)
  {
    __m256 v = _mm256_loadu_ps(data + i);
    v = FastTanh(v);
    _mm256_storeu_ps(data + i, v);
  }

  // Handle remainder with SSE
#ifdef GUITARFX_HAS_SSE2
  for (; i + 4 <= size; i += 4)
  {
    __m128 v = _mm_loadu_ps(data + i);
    v = sse2::FastTanh(v);
    _mm_storeu_ps(data + i, v);
  }
#endif

  // Scalar remainder
  for (; i < size; ++i)
    data[i] = scalar::FastTanh(data[i]);
}

inline void ApplySigmoid(float* data, long size)
{
  long i = 0;

  for (; i + 8 <= size; i += 8)
  {
    __m256 v = _mm256_loadu_ps(data + i);
    v = FastSigmoid(v);
    _mm256_storeu_ps(data + i, v);
  }

#ifdef GUITARFX_HAS_SSE2
  for (; i + 4 <= size; i += 4)
  {
    __m128 v = _mm_loadu_ps(data + i);
    v = sse2::FastSigmoid(v);
    _mm_storeu_ps(data + i, v);
  }
#endif

  for (; i < size; ++i)
    data[i] = scalar::FastSigmoid(data[i]);
}

inline void ApplyReLU(float* data, long size)
{
  long i = 0;
  __m256 zero = _mm256_setzero_ps();

  for (; i + 8 <= size; i += 8)
  {
    __m256 v = _mm256_loadu_ps(data + i);
    v = _mm256_max_ps(v, zero);
    _mm256_storeu_ps(data + i, v);
  }

#ifdef GUITARFX_HAS_SSE2
  for (; i + 4 <= size; i += 4)
  {
    __m128 v = _mm_loadu_ps(data + i);
    v = _mm_max_ps(v, _mm_setzero_ps());
    _mm_storeu_ps(data + i, v);
  }
#endif

  for (; i < size; ++i)
    data[i] = scalar::ReLU(data[i]);
}

// Fused gated activation for WaveNet: output = tanh(x) * sigmoid(gate)
inline void ApplyGatedActivation(float* data, float* gate, long size)
{
  long i = 0;

  for (; i + 8 <= size; i += 8)
  {
    __m256 x = _mm256_loadu_ps(data + i);
    __m256 g = _mm256_loadu_ps(gate + i);

    x = FastTanh(x);
    g = FastSigmoid(g);

    __m256 result = _mm256_mul_ps(x, g);
    _mm256_storeu_ps(data + i, result);
  }

#ifdef GUITARFX_HAS_SSE2
  for (; i + 4 <= size; i += 4)
  {
    __m128 x = _mm_loadu_ps(data + i);
    __m128 g = _mm_loadu_ps(gate + i);

    x = sse2::FastTanh(x);
    g = sse2::FastSigmoid(g);

    __m128 result = _mm_mul_ps(x, g);
    _mm_storeu_ps(data + i, result);
  }
#endif

  for (; i < size; ++i)
    data[i] = scalar::FastTanh(data[i]) * scalar::FastSigmoid(gate[i]);
}

// Fused gated activation in-place on interleaved data (channels split in half)
// First half: tanh activation
// Second half: sigmoid gate
// Result stored in first half: tanh(first) * sigmoid(second)
inline void ApplyGatedActivationInterleaved(float* data, long channels, long numFrames)
{
  const long halfChannels = channels / 2;

  for (long frame = 0; frame < numFrames; ++frame)
  {
    float* frameData = data + frame * channels;
    float* gateData = frameData + halfChannels;

    long i = 0;

    for (; i + 8 <= halfChannels; i += 8)
    {
      __m256 x = _mm256_loadu_ps(frameData + i);
      __m256 g = _mm256_loadu_ps(gateData + i);

      x = FastTanh(x);
      g = FastSigmoid(g);

      _mm256_storeu_ps(frameData + i, _mm256_mul_ps(x, g));
    }

#ifdef GUITARFX_HAS_SSE2
    for (; i + 4 <= halfChannels; i += 4)
    {
      __m128 x = _mm_loadu_ps(frameData + i);
      __m128 g = _mm_loadu_ps(gateData + i);

      x = sse2::FastTanh(x);
      g = sse2::FastSigmoid(g);

      _mm_storeu_ps(frameData + i, _mm_mul_ps(x, g));
    }
#endif

    for (; i < halfChannels; ++i)
      frameData[i] = scalar::FastTanh(frameData[i]) * scalar::FastSigmoid(gateData[i]);
  }
}

} // namespace avx

#endif // GUITARFX_HAS_AVX

// ============================================================================
// Dispatcher - Auto-selects best implementation at runtime
// ============================================================================

inline void ApplyTanh(float* data, long size)
{
#ifdef GUITARFX_HAS_AVX
  if (GetSimdLevel() >= SimdLevel::AVX)
  {
    avx::ApplyTanh(data, size);
    return;
  }
#endif
#ifdef GUITARFX_HAS_SSE2
  if (GetSimdLevel() >= SimdLevel::SSE2)
  {
    sse2::ApplyTanh(data, size);
    return;
  }
#endif
  scalar::ApplyTanh(data, size);
}

inline void ApplySigmoid(float* data, long size)
{
#ifdef GUITARFX_HAS_AVX
  if (GetSimdLevel() >= SimdLevel::AVX)
  {
    avx::ApplySigmoid(data, size);
    return;
  }
#endif
#ifdef GUITARFX_HAS_SSE2
  if (GetSimdLevel() >= SimdLevel::SSE2)
  {
    sse2::ApplySigmoid(data, size);
    return;
  }
#endif
  scalar::ApplySigmoid(data, size);
}

inline void ApplyReLU(float* data, long size)
{
#ifdef GUITARFX_HAS_AVX
  if (GetSimdLevel() >= SimdLevel::AVX)
  {
    avx::ApplyReLU(data, size);
    return;
  }
#endif
#ifdef GUITARFX_HAS_SSE2
  if (GetSimdLevel() >= SimdLevel::SSE2)
  {
    sse2::ApplyReLU(data, size);
    return;
  }
#endif
  scalar::ApplyReLU(data, size);
}

inline void ApplyGatedActivation(float* data, float* gate, long size)
{
#ifdef GUITARFX_HAS_AVX
  if (GetSimdLevel() >= SimdLevel::AVX)
  {
    avx::ApplyGatedActivation(data, gate, size);
    return;
  }
#endif
#ifdef GUITARFX_HAS_SSE2
  if (GetSimdLevel() >= SimdLevel::SSE2)
  {
    sse2::ApplyGatedActivation(data, gate, size);
    return;
  }
#endif
  // Scalar fallback
  for (long i = 0; i < size; ++i)
    data[i] = scalar::FastTanh(data[i]) * scalar::FastSigmoid(gate[i]);
}

inline void ApplyGatedActivationInterleaved(float* data, long channels, long numFrames)
{
#ifdef GUITARFX_HAS_AVX
  if (GetSimdLevel() >= SimdLevel::AVX)
  {
    avx::ApplyGatedActivationInterleaved(data, channels, numFrames);
    return;
  }
#endif
  // Fallback implementation
  const long halfChannels = channels / 2;
  for (long frame = 0; frame < numFrames; ++frame)
  {
    float* frameData = data + frame * channels;
    float* gateData = frameData + halfChannels;
    for (long i = 0; i < halfChannels; ++i)
      frameData[i] = scalar::FastTanh(frameData[i]) * scalar::FastSigmoid(gateData[i]);
  }
}

} // namespace simd
} // namespace guitarfx
