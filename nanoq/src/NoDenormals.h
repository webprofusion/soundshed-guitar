#pragma once

// Flush-to-zero for the duration of a scope: the audio thread's guard against denormals.
//
// Recursive filters and reverb tails decay into the subnormal range, where x86 arithmetic runs
// tens of times slower and a host's validator flags the output. Setting FTZ and DAZ for the
// block, and putting the thread's floating point state back after, is what JUCE's
// ScopedNoDenormals does for the JUCE products.

#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
 #include <xmmintrin.h>
#elif defined(_M_ARM64) && defined(_MSC_VER)
 #include <intrin.h>
#endif

namespace nanoq
{
class ScopedNoDenormals
{
public:
    ScopedNoDenormals() noexcept
    {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
        mPrevious = _mm_getcsr();
        _mm_setcsr(mPrevious | 0x8040u); // FTZ (bit 15) and DAZ (bit 6)
#elif defined(__aarch64__)
        unsigned long long fpcr;
        asm volatile("mrs %0, fpcr" : "=r"(fpcr));
        mPrevious = fpcr;
        asm volatile("msr fpcr, %0" : : "r"(fpcr | (1ull << 24))); // FZ
#elif defined(_M_ARM64)
        mPrevious = static_cast<unsigned long long>(_ReadStatusReg(0x5A20)); // ARM64_FPCR
        _WriteStatusReg(0x5A20, static_cast<__int64>(mPrevious | (1ull << 24)));
#endif
    }

    ~ScopedNoDenormals()
    {
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
        _mm_setcsr(mPrevious);
#elif defined(__aarch64__)
        asm volatile("msr fpcr, %0" : : "r"(mPrevious));
#elif defined(_M_ARM64)
        _WriteStatusReg(0x5A20, static_cast<__int64>(mPrevious));
#endif
    }

    ScopedNoDenormals(const ScopedNoDenormals&) = delete;
    ScopedNoDenormals& operator=(const ScopedNoDenormals&) = delete;

private:
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86)
    unsigned int mPrevious = 0;
#else
    unsigned long long mPrevious = 0;
#endif
};
} // namespace nanoq
