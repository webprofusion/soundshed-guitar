#pragma once

// The 8-lane float vector the fused WaveNet (FusedWaveNet.h) is written in, one definition per
// instruction set, and the activations over it. Included by FusedWaveNet.cpp only.

#include <algorithm>
#include <cmath>
#include <cstring>

// The vector kernel. AVX2 needs FMA too: MSVC's /arch:AVX2 implies it, clang and gcc need it named
// (-march=x86-64-v3 does). Other x86-64 builds, the avx and sse2 tiers, get two SSE halves, which
// measured 1.2-2.7x the NAM core's own SSE build. arm64 gets two NEON halves with fused
// multiply-adds, always compiled and used where GUITARFX_FWN_NEON_ENABLED is defined (see
// IsAvailable()), which CMake defines by default; on Apple silicon it measured 1.7-2.5x the NAM
// core on standard and A2 full models. Elsewhere there is no kernel: IsAvailable() is false, and
// the plain C++ vector below only keeps FusedWaveNet.cpp compiling.
#if defined(__AVX2__) && (defined(__FMA__) || (defined(_MSC_VER) && !defined(__clang__)))
    #define GUITARFX_FWN_AVX2 1
    #include <immintrin.h>
#elif defined(_M_ARM64EC)
// Windows ARM64EC: MSVC defines _M_X64 there too, and would compile the SSE kernel through its
// emulated intrinsics. The code runs on an arm64 CPU, so it takes the NEON kernel.
    #define GUITARFX_FWN_NEON 1
    #include <arm_neon.h>
#elif defined(_M_X64) || defined(__x86_64__) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
// 32-bit x86 too, where the compiler has SSE2 (MSVC's default /arch:SSE2, _M_IX86_FP 2).
    #define GUITARFX_FWN_SSE 1
    #include <emmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
    #define GUITARFX_FWN_NEON 1
    #include <arm_neon.h>
#endif

#if defined(_MSC_VER)
    #define GUITARFX_FWN_INLINE __forceinline
#else
    #define GUITARFX_FWN_INLINE inline __attribute__((always_inline))
#endif

namespace guitarfx::fusedwavenet::simd
{
// ---------------------------------------------------------------- 8-lane float vector
#if defined(GUITARFX_FWN_AVX2)
struct V8
{
    __m256 v;
};

// Register tile: 4 outputs x 3 vectors of 8 frames. Measured 1-9% faster than 4 x 2 on every
// model shape, and it fills AVX2's 16 registers exactly. Every tile gives the same bits.
constexpr int kTileOutputs = 4;
constexpr int kTileVectors = 3;

GUITARFX_FWN_INLINE V8 Load(const float* p)
{
    return {_mm256_loadu_ps(p)};
}

GUITARFX_FWN_INLINE void Store(float* p, V8 a)
{
    _mm256_storeu_ps(p, a.v);
}

GUITARFX_FWN_INLINE V8 Set1(float x)
{
    return {_mm256_set1_ps(x)};
}

GUITARFX_FWN_INLINE V8 Broadcast(const float* p)
{
    return {_mm256_broadcast_ss(p)};
}

GUITARFX_FWN_INLINE V8 Fma(V8 a, V8 b, V8 c) // a * b + c
{
    return {_mm256_fmadd_ps(a.v, b.v, c.v)};
}

GUITARFX_FWN_INLINE V8 Add(V8 a, V8 b)
{
    return {_mm256_add_ps(a.v, b.v)};
}

GUITARFX_FWN_INLINE V8 Mul(V8 a, V8 b)
{
    return {_mm256_mul_ps(a.v, b.v)};
}

GUITARFX_FWN_INLINE V8 Div(V8 a, V8 b)
{
    return {_mm256_div_ps(a.v, b.v)};
}

GUITARFX_FWN_INLINE V8 Abs(V8 a)
{
    return {_mm256_andnot_ps(_mm256_set1_ps(-0.0f), a.v)};
}

GUITARFX_FWN_INLINE V8 Max(V8 a, V8 b)
{
    return {_mm256_max_ps(a.v, b.v)};
}
#elif defined(GUITARFX_FWN_SSE)
struct V8
{
    __m128 lo, hi;
};

// Each vector is two registers here, so a 4 x 2 tile already holds 16 accumulators.
constexpr int kTileOutputs = 4;
constexpr int kTileVectors = 2;

GUITARFX_FWN_INLINE V8 Load(const float* p)
{
    return {_mm_loadu_ps(p), _mm_loadu_ps(p + 4)};
}

GUITARFX_FWN_INLINE void Store(float* p, V8 a)
{
    _mm_storeu_ps(p, a.lo);
    _mm_storeu_ps(p + 4, a.hi);
}

GUITARFX_FWN_INLINE V8 Set1(float x)
{
    const __m128 s = _mm_set1_ps(x);
    return {s, s};
}

GUITARFX_FWN_INLINE V8 Broadcast(const float* p)
{
    const __m128 s = _mm_load1_ps(p);
    return {s, s};
}

GUITARFX_FWN_INLINE V8 Fma(V8 a, V8 b, V8 c) // a * b + c, unfused
{
    return {_mm_add_ps(_mm_mul_ps(a.lo, b.lo), c.lo), _mm_add_ps(_mm_mul_ps(a.hi, b.hi), c.hi)};
}

GUITARFX_FWN_INLINE V8 Add(V8 a, V8 b)
{
    return {_mm_add_ps(a.lo, b.lo), _mm_add_ps(a.hi, b.hi)};
}

GUITARFX_FWN_INLINE V8 Mul(V8 a, V8 b)
{
    return {_mm_mul_ps(a.lo, b.lo), _mm_mul_ps(a.hi, b.hi)};
}

GUITARFX_FWN_INLINE V8 Div(V8 a, V8 b)
{
    return {_mm_div_ps(a.lo, b.lo), _mm_div_ps(a.hi, b.hi)};
}

GUITARFX_FWN_INLINE V8 Abs(V8 a)
{
    const __m128 m = _mm_set1_ps(-0.0f);
    return {_mm_andnot_ps(m, a.lo), _mm_andnot_ps(m, a.hi)};
}

GUITARFX_FWN_INLINE V8 Max(V8 a, V8 b)
{
    return {_mm_max_ps(a.lo, b.lo), _mm_max_ps(a.hi, b.hi)};
}
#elif defined(GUITARFX_FWN_NEON)
struct V8
{
    float32x4_t lo, hi;
};

// 4 x 3 as on AVX2, but each vector is two of NEON's 32 registers: 24 accumulators, 6 input
// vectors and one register holding the four outputs' weights (FmaLane), so 31. A 4 x 4 tile would
// need 32 accumulators alone and spill. 24 independent multiply-add chains also cover a 4-cycle
// latency on 4 pipes (16 needed).
constexpr int kTileOutputs = 4;
constexpr int kTileVectors = 3;

GUITARFX_FWN_INLINE V8 Load(const float* p)
{
    return {vld1q_f32(p), vld1q_f32(p + 4)};
}

GUITARFX_FWN_INLINE void Store(float* p, V8 a)
{
    vst1q_f32(p, a.lo);
    vst1q_f32(p + 4, a.hi);
}

GUITARFX_FWN_INLINE V8 Set1(float x)
{
    const float32x4_t s = vdupq_n_f32(x);
    return {s, s};
}

GUITARFX_FWN_INLINE V8 Broadcast(const float* p)
{
    const float32x4_t s = vld1q_dup_f32(p);
    return {s, s};
}

GUITARFX_FWN_INLINE V8 Fma(V8 a, V8 b, V8 c) // a * b + c, fused as on AVX2
{
    return {vfmaq_f32(c.lo, a.lo, b.lo), vfmaq_f32(c.hi, a.hi, b.hi)};
}

// Fma(Broadcast(&w[L]), b, c), the same bits, from a register of four weights. With four
// broadcasts in flight GCC ran out of registers and kept three accumulators on the stack.
template <int L> GUITARFX_FWN_INLINE V8 FmaLane(float32x4_t w, V8 b, V8 c)
{
    return {vfmaq_laneq_f32(c.lo, b.lo, w, L), vfmaq_laneq_f32(c.hi, b.hi, w, L)};
}

GUITARFX_FWN_INLINE V8 Add(V8 a, V8 b)
{
    return {vaddq_f32(a.lo, b.lo), vaddq_f32(a.hi, b.hi)};
}

GUITARFX_FWN_INLINE V8 Mul(V8 a, V8 b)
{
    return {vmulq_f32(a.lo, b.lo), vmulq_f32(a.hi, b.hi)};
}

GUITARFX_FWN_INLINE V8 Div(V8 a, V8 b) // a true divide on AArch64, not a reciprocal estimate
{
    return {vdivq_f32(a.lo, b.lo), vdivq_f32(a.hi, b.hi)};
}

GUITARFX_FWN_INLINE V8 Abs(V8 a)
{
    return {vabsq_f32(a.lo), vabsq_f32(a.hi)};
}

GUITARFX_FWN_INLINE V8 Max(V8 a, V8 b)
{
    return {vmaxq_f32(a.lo, b.lo), vmaxq_f32(a.hi, b.hi)};
}
#else
// Compiles the file only. It measured 0.3x the NAM core, so IsAvailable() is false here and
// nothing builds a model on it.
struct V8
{
    float v[8];
};

constexpr int kTileOutputs = 4;
constexpr int kTileVectors = 2;

inline V8 Load(const float* p)
{
    V8 r;
    std::memcpy(r.v, p, sizeof(r.v));
    return r;
}

inline void Store(float* p, V8 a)
{
    std::memcpy(p, a.v, sizeof(a.v));
}

inline V8 Set1(float x)
{
    V8 r;
    std::fill_n(r.v, 8, x);
    return r;
}

inline V8 Broadcast(const float* p)
{
    return Set1(*p);
}

template <typename Op> inline V8 Lanes(V8 a, V8 b, Op op)
{
    V8 r;

    for (int i = 0; i < 8; ++i)
    {
        r.v[i] = op(a.v[i], b.v[i]);
    }

    return r;
}

inline V8 Fma(V8 a, V8 b, V8 c)
{
    return Lanes(Lanes(a, b, [](float x, float y) { return x * y; }), c, [](float x, float y) { return x + y; });
}

inline V8 Add(V8 a, V8 b)
{
    return Lanes(a, b, [](float x, float y) { return x + y; });
}

inline V8 Mul(V8 a, V8 b)
{
    return Lanes(a, b, [](float x, float y) { return x * y; });
}

inline V8 Div(V8 a, V8 b)
{
    return Lanes(a, b, [](float x, float y) { return x / y; });
}

inline V8 Abs(V8 a)
{
    return Lanes(a, a, [](float x, float) { return std::fabs(x); });
}

inline V8 Max(V8 a, V8 b)
{
    return Lanes(a, b, [](float x, float y) { return x > y ? x : y; });
}
#endif

// nam::activations::fast_tanh, lane-wise, with the same constants.
GUITARFX_FWN_INLINE V8 FastTanh(V8 x)
{
    const V8 ax = Abs(x);
    const V8 x2 = Mul(x, x);
    const V8 a = Set1(2.45550750702956f);
    const V8 num = Mul(x, Fma(Fma(Set1(0.821226666969744f), ax, Set1(0.893229853513558f)), x2, Fma(a, ax, a)));
    const V8 d = Set1(2.44506634652299f);
    const V8 den = Fma(Add(d, x2), Abs(Fma(Mul(Set1(0.814642734961073f), x), ax, x)), d);
    return Div(num, den);
}

GUITARFX_FWN_INLINE V8 LeakyRelu(V8 x, V8 slope) // slope in [0, 1)
{
    return Max(x, Mul(x, slope));
}
} // namespace guitarfx::fusedwavenet::simd
