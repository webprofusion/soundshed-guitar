/**
 * @file RealtimeConvolverTests.cpp
 * @brief Unit tests for the RealtimeConvolver, focused on the non-uniform
 *        (Gardner-style, low-latency) partitioning path enabled via
 *        SetLowLatencyMode(true).
 *
 * The tests verify that:
 *  - Low-latency mode reports the smaller base-block latency.
 *  - The non-uniform engine produces the same result as a direct reference
 *    convolution, delay-aligned by the reported latency.
 *  - The non-uniform output matches the uniform output once both are aligned
 *    by their respective latencies (so the toggle only changes latency/CPU,
 *    not the tone).
 *  - Reset() clears state so a subsequent run is identical to a fresh one.
 */

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <random>
#include <vector>

#include "dsp/RealtimeConvolver.h"

#ifndef M_PI
    #define M_PI 3.14159265358979323846
#endif

namespace
{
// Run a convolver over the input, feeding it in fixed-size blocks to mimic a
// real host. Returns an output vector the same length as the input.
std::vector<double> RunConvolver(guitarfx::RealtimeConvolver& conv, const std::vector<float>& input, int blockSize)
{
    std::vector<double> output(input.size(), 0.0);
    std::vector<float> inBlock(static_cast<std::size_t>(blockSize));
    std::vector<float> outBlock(static_cast<std::size_t>(blockSize));

    std::size_t pos = 0;

    while (pos < input.size())
    {
        const int n = static_cast<int>(std::min<std::size_t>(blockSize, input.size() - pos));

        for (int i = 0; i < n; ++i)
        {
            inBlock[static_cast<std::size_t>(i)] = input[pos + static_cast<std::size_t>(i)];
        }

        conv.Process(inBlock.data(), outBlock.data(), n);

        for (int i = 0; i < n; ++i)
        {
            output[pos + static_cast<std::size_t>(i)] = static_cast<double>(outBlock[static_cast<std::size_t>(i)]);
        }

        pos += static_cast<std::size_t>(n);
    }

    return output;
}

// Direct (time-domain) reference convolution: ref[n] = sum_k input[n-k]*ir[k].
std::vector<double> ReferenceConvolve(const std::vector<float>& input, const std::vector<float>& ir)
{
    std::vector<double> ref(input.size(), 0.0);
    const std::size_t irLen = ir.size();

    for (std::size_t n = 0; n < input.size(); ++n)
    {
        double acc = 0.0;
        const std::size_t kMax = std::min(n + 1, irLen);

        for (std::size_t k = 0; k < kMax; ++k)
        {
            acc += static_cast<double>(input[n - k]) * static_cast<double>(ir[k]);
        }

        ref[n] = acc;
    }

    return ref;
}

// Normalised RMS difference between two signals over [start, end).
double NormalizedRmsDiff(const std::vector<double>& a, const std::vector<double>& b, std::size_t start, std::size_t end)
{
    double diffSq = 0.0;
    double refSq = 0.0;
    std::size_t count = 0;

    for (std::size_t i = start; i < end && i < a.size() && i < b.size(); ++i)
    {
        const double d = a[i] - b[i];
        diffSq += d * d;
        refSq += b[i] * b[i];
        ++count;
    }

    if (count == 0 || refSq < 1e-20)
    {
        return 0.0;
    }

    return std::sqrt(diffSq / refSq);
}

// Build a decaying, oscillating impulse response of the requested length.
std::vector<float> MakeIR(std::size_t length)
{
    std::vector<float> ir(length, 0.0f);

    for (std::size_t i = 0; i < length; ++i)
    {
        const double t = static_cast<double>(i) / static_cast<double>(length);
        const double env = std::exp(-6.0 * t);
        ir[i] = static_cast<float>(env * std::sin(2.0 * M_PI * 60.0 * t));
    }

    ir[0] = 1.0f; // strong direct component
    return ir;
}

std::vector<float> MakeNoise(std::size_t length, unsigned seed)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    std::vector<float> out(length);

    for (auto& s : out)
    {
        s = dist(rng);
    }

    return out;
}

// ---------------------------------------------------------------------------

// Low-latency mode should report the smaller base-block latency, and the
// non-uniform engine should be active for an IR longer than the base block.
bool TestLowLatencyReportsSmallerLatency()
{
    std::cout << "Test: low-latency mode reports smaller latency... ";

    const std::vector<float> ir = MakeIR(4096);
    const int blockSize = 128;

    guitarfx::RealtimeConvolver uniform;
    uniform.SetLowLatencyMode(false);
    uniform.SetImpulse(ir, blockSize);

    guitarfx::RealtimeConvolver lowLat;
    lowLat.SetLowLatencyMode(true);
    lowLat.SetImpulse(ir, blockSize);

    const int uniformLatency = uniform.GetLatency();
    const int lowLatency = lowLat.GetLatency();

    if (!(lowLatency < uniformLatency))
    {
        std::cout << "FAILED (uniform=" << uniformLatency << ", lowLatency=" << lowLatency << ")\n";
        return false;
    }

    if (lowLatency != 128)
    {
        std::cout << "FAILED (expected base latency 128, got " << lowLatency << ")\n";
        return false;
    }

    std::cout << "OK (uniform=" << uniformLatency << ", lowLatency=" << lowLatency << ")\n";
    return true;
}

// The non-uniform engine, delay-aligned by its reported latency, must match a
// direct reference convolution.
bool TestNonUniformMatchesReference()
{
    std::cout << "Test: non-uniform output matches reference convolution... ";

    const std::vector<float> ir = MakeIR(4096);
    const int blockSize = 128;
    const std::vector<float> input = MakeNoise(16384, 1234u);

    guitarfx::RealtimeConvolver conv;
    conv.SetLowLatencyMode(true);

    if (!conv.SetImpulse(ir, blockSize))
    {
        std::cout << "FAILED (SetImpulse returned false)\n";
        return false;
    }

    const int latency = conv.GetLatency();
    const std::vector<double> got = RunConvolver(conv, input, blockSize);
    const std::vector<double> ref = ReferenceConvolve(input, ir);

    // Align: got[latency + k] should match ref[k].
    std::vector<double> aligned(ref.size(), 0.0);

    for (std::size_t k = 0; k + static_cast<std::size_t>(latency) < got.size(); ++k)
    {
        aligned[k] = got[k + static_cast<std::size_t>(latency)];
    }

    // Compare over a region clear of edge/transition effects.
    const std::size_t start = static_cast<std::size_t>(latency) + ir.size();
    const std::size_t end = ref.size() - static_cast<std::size_t>(latency);
    const double err = NormalizedRmsDiff(aligned, ref, start, end);

    if (!(err < 1e-3))
    {
        std::cout << "FAILED (normalized RMS error " << err << ")\n";
        return false;
    }

    std::cout << "OK (error " << err << ", latency " << latency << ")\n";
    return true;
}

// Toggling the mode must not change the tone: uniform and non-uniform outputs
// must match once aligned by their (different) latencies.
bool TestNonUniformMatchesUniform()
{
    std::cout << "Test: non-uniform output matches uniform (delay-aligned)... ";

    const std::vector<float> ir = MakeIR(6000); // > max partition to exercise the tail engine
    const int blockSize = 128;                  // base (128) < uniform partition (256) => non-uniform active
    const std::vector<float> input = MakeNoise(24000, 99u);

    guitarfx::RealtimeConvolver uniform;
    uniform.SetLowLatencyMode(false);
    uniform.SetImpulse(ir, blockSize);

    guitarfx::RealtimeConvolver lowLat;
    lowLat.SetLowLatencyMode(true);
    lowLat.SetImpulse(ir, blockSize);

    const int uLat = uniform.GetLatency();
    const int lLat = lowLat.GetLatency();

    const std::vector<double> uOut = RunConvolver(uniform, input, blockSize);
    const std::vector<double> lOut = RunConvolver(lowLat, input, blockSize);

    // Shift both to remove their latency, then compare on common indices.
    std::vector<double> uAligned(input.size(), 0.0);
    std::vector<double> lAligned(input.size(), 0.0);

    for (std::size_t k = 0; k + static_cast<std::size_t>(uLat) < uOut.size(); ++k)
    {
        uAligned[k] = uOut[k + static_cast<std::size_t>(uLat)];
    }

    for (std::size_t k = 0; k + static_cast<std::size_t>(lLat) < lOut.size(); ++k)
    {
        lAligned[k] = lOut[k + static_cast<std::size_t>(lLat)];
    }

    const std::size_t start = ir.size() + static_cast<std::size_t>(std::max(uLat, lLat));
    const std::size_t end = input.size() - static_cast<std::size_t>(std::max(uLat, lLat));
    const double err = NormalizedRmsDiff(lAligned, uAligned, start, end);

    if (!(err < 1e-3))
    {
        std::cout << "FAILED (normalized RMS error " << err << ", uLat=" << uLat << ", lLat=" << lLat << ")\n";
        return false;
    }

    std::cout << "OK (error " << err << ", uLat=" << uLat << ", lLat=" << lLat << ")\n";
    return true;
}

// Reset() must clear all internal state so a second run is identical to a
// fresh convolver's run.
bool TestResetClearsState()
{
    std::cout << "Test: reset clears non-uniform state... ";

    const std::vector<float> ir = MakeIR(4096);
    const int blockSize = 128;
    const std::vector<float> input = MakeNoise(8192, 7u);

    guitarfx::RealtimeConvolver conv;
    conv.SetLowLatencyMode(true);
    conv.SetImpulse(ir, blockSize);

    const std::vector<double> first = RunConvolver(conv, input, blockSize);

    conv.Reset();
    const std::vector<double> second = RunConvolver(conv, input, blockSize);

    const double err = NormalizedRmsDiff(first, second, ir.size(), input.size());

    if (!(err < 1e-9))
    {
        std::cout << "FAILED (post-reset run differs, error " << err << ")\n";
        return false;
    }

    std::cout << "OK\n";
    return true;
}
} // anonymous namespace

int main()
{
    std::cout << "RealtimeConvolver Non-Uniform Partitioning Tests\n";
    std::cout << "================================================\n\n";

    int passed = 0;
    int failed = 0;

    auto runTest = [&](bool (*test)()) {
        if (test())
        {
            ++passed;
        }
        else
        {
            ++failed;
        }
    };

    runTest(TestLowLatencyReportsSmallerLatency);
    runTest(TestNonUniformMatchesReference);
    runTest(TestNonUniformMatchesUniform);
    runTest(TestResetClearsState);

    std::cout << "\n================================================\n";
    std::cout << "Results: " << passed << "/" << (passed + failed) << " tests passed.\n";

    if (failed > 0)
    {
        std::cout << "\nSome tests FAILED.\n";
        return 1;
    }

    std::cout << "\nAll tests PASSED.\n";
    return 0;
}
