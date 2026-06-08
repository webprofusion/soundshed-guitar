#pragma once

/*
  BlockSincResampler algorithm summary
  ====================================

  Purpose
  - Convert one non-interleaved mono block from source sample-rate domain to
    target sample-rate domain with fixed output frame count.
  - Support three runtime quality modes with identical API:
    1) Linear              : minimal CPU, lowest quality
    2) HighPerformance     : windowed-sinc with fewer taps
    3) Highest             : windowed-sinc with wider kernel

  High-level flow
  1) Prepare(sourceRate, targetRate, maxInputFrames, quality)
     - Stores rates and limits.
     - Computes max output frame capacity for worst-case ratio (+guard).
     - For sinc modes, precomputes a fractional-phase kernel table so the
       real-time loop avoids per-sample trig/window evaluation.

  2) ProcessFixedOutput(input, inputFrames, output, outputFrames)
     - Fast passthrough when rates match (or frame counts match).
     - Uses linear interpolation if quality == Linear.
     - Uses table-driven windowed-sinc convolution otherwise.

  Sinc path details (table-driven)
  - Kernel shape: sinc(distance * cutoff) * Blackman(distance)
  - Kernel support: tap offsets in [-halfTaps, +halfTaps]
  - Fractional-phase discretization: kKernelPhaseCount rows in [0, 1)
  - Each row is normalized at build time so runtime avoids coefficient-sum
    normalization/division.
  - Per output sample:
    a) Map output index to source position
       sourcePos = (outIndex + 0.5) * (inputFrames / outputFrames) - 0.5
    b) Split into center integer index + fractional phase
    c) Select nearest precomputed kernel row for the phase
    d) Convolve taps against clamped source samples

  Edge handling
  - Out-of-range reads are clamped to [0, inputFrames - 1].
  - In passthrough mode, any extra output frames are padded using last input
    sample.

  Complexity and tradeoffs
  - Linear mode: O(outputFrames)
  - Sinc mode  : O(outputFrames * tapCount)
  - Precomputation cost is paid in Prepare() and amortized across blocks.
  - Design favors deterministic, allocation-free hot-loop behavior after table
    build, while preserving a simple fixed-block API for DSP call sites.
*/

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace guitarfx
{
  enum class SampleRateConversionQuality
  {
    Linear,
    HighPerformance,
    Highest
  };

  class BlockSincResampler
  {
  public:
    // Configure one resampling lane for repeated fixed-output block conversion.
    // This precomputes phase kernels for sinc modes so the real-time loop only
    // performs table lookup + MAC, not trig/window evaluation.
    void Prepare(double sourceRate,
                 double targetRate,
                 int maxInputFrames,
                 SampleRateConversionQuality quality = SampleRateConversionQuality::Highest)
    {
      mSourceRate = sourceRate;
      mTargetRate = targetRate;
      mMaxInputFrames = std::max(0, maxInputFrames);
      mMaxOutputFrames = ComputeMaxOutputFrameCount(mMaxInputFrames, mSourceRate, mTargetRate);
      mQuality = quality;

      const int halfTaps = GetHalfTapsForQuality();
      const double cutoff = std::min(mTargetRate / std::max(mSourceRate, 1.0e-12), 1.0);
      EnsureKernelTable(halfTaps, cutoff);
    }

    [[nodiscard]] int GetMaxOutputFrames() const noexcept { return mMaxOutputFrames; }

    [[nodiscard]] static int ComputeOutputFrameCount(int inputFrames, double sourceRate, double targetRate)
    {
      if (inputFrames <= 0 || sourceRate <= 0.0 || targetRate <= 0.0)
        return 0;

      const double scaledFrames = static_cast<double>(inputFrames) * targetRate / sourceRate;
      return std::max(1, static_cast<int>(std::llround(scaledFrames)));
    }

    [[nodiscard]] static int ComputeMaxOutputFrameCount(int maxInputFrames, double sourceRate, double targetRate)
    {
      if (maxInputFrames <= 0 || sourceRate <= 0.0 || targetRate <= 0.0)
        return 0;

      const double scaledFrames = static_cast<double>(maxInputFrames) * targetRate / sourceRate;
      return std::max(1, static_cast<int>(std::ceil(scaledFrames)) + kGuardFrames);
    }

    template <typename InputSample, typename OutputSample>
    int ProcessFixedOutput(const InputSample *input,
                           int inputFrames,
                           OutputSample *output,
                           int outputFrames) const
    {
      if (!input || !output || inputFrames <= 0 || outputFrames <= 0)
        return 0;

      if (inputFrames == outputFrames || RatesMatch(mSourceRate, mTargetRate))
      {
        // Fast pass-through path used when SRC is effectively disabled.
        const int framesToCopy = std::min(inputFrames, outputFrames);
        for (int sampleIndex = 0; sampleIndex < framesToCopy; ++sampleIndex)
          output[sampleIndex] = static_cast<OutputSample>(input[sampleIndex]);
        for (int sampleIndex = framesToCopy; sampleIndex < outputFrames; ++sampleIndex)
          output[sampleIndex] = static_cast<OutputSample>(input[inputFrames - 1]);
        return outputFrames;
      }

      if (mQuality == SampleRateConversionQuality::Linear)
        return ProcessLinear(input, inputFrames, output, outputFrames);

      return ProcessSinc(input, inputFrames, output, outputFrames);
    }

  private:
    // Highest quality uses a wide sinc kernel; high-performance uses a narrow
    // kernel with lower CPU cost.
    static constexpr int kHalfTaps = 64;
    static constexpr int kHighPerformanceHalfTaps = 12;
    static constexpr int kGuardFrames = 8;
    static constexpr double kPi = 3.14159265358979323846;
    // Number of fractional positions in [0, 1) with precomputed coefficient rows.
    static constexpr int kKernelPhaseCount = 1024;

    double mSourceRate = 44100.0;
    double mTargetRate = 44100.0;
    int mMaxInputFrames = 0;
    int mMaxOutputFrames = 0;
    SampleRateConversionQuality mQuality = SampleRateConversionQuality::Highest;
    mutable int mKernelHalfTaps = -1;
    mutable double mKernelCutoff = -1.0;
    mutable std::vector<double> mKernelTable;

    [[nodiscard]] static bool RatesMatch(double sourceRate, double targetRate)
    {
      return std::abs(sourceRate - targetRate) < 1.0e-9;
    }

    [[nodiscard]] int GetHalfTapsForQuality() const
    {
      if (mQuality == SampleRateConversionQuality::HighPerformance)
        return kHighPerformanceHalfTaps;
      return kHalfTaps;
    }

    [[nodiscard]] static double Sinc(double value)
    {
      if (std::abs(value) < 1.0e-8)
        return 1.0;

      const double angle = kPi * value;
      return std::sin(angle) / angle;
    }

    [[nodiscard]] static double Blackman(double distance, int halfTaps)
    {
      const double normalizedDistance = std::abs(distance) / static_cast<double>(halfTaps);
      if (normalizedDistance >= 1.0)
        return 0.0;

      return 0.42 + 0.5 * std::cos(kPi * normalizedDistance)
        + 0.08 * std::cos(2.0 * kPi * normalizedDistance);
    }

    void EnsureKernelTable(int halfTaps, double cutoff) const
    {
      if (halfTaps <= 0)
        return;

      // If setup didn't change, keep the existing table to avoid rebuild cost.
      if (mKernelHalfTaps == halfTaps
          && std::abs(mKernelCutoff - cutoff) <= std::numeric_limits<double>::epsilon())
      {
        return;
      }

      const int tapCount = 2 * halfTaps + 1;
      mKernelTable.assign(static_cast<std::size_t>(kKernelPhaseCount) * static_cast<std::size_t>(tapCount), 0.0);

      for (int phaseIndex = 0; phaseIndex < kKernelPhaseCount; ++phaseIndex)
      {
        // Precompute one tap row for a fractional offset in [0, 1).
        const double phase = static_cast<double>(phaseIndex) / static_cast<double>(kKernelPhaseCount);
        double coeffSum = 0.0;

        for (int tapOffset = -halfTaps; tapOffset <= halfTaps; ++tapOffset)
        {
          const double distance = phase - static_cast<double>(tapOffset);
          const double coefficient = cutoff * Sinc(distance * cutoff) * Blackman(distance, halfTaps);
          const std::size_t tableIndex = static_cast<std::size_t>(phaseIndex) * static_cast<std::size_t>(tapCount)
            + static_cast<std::size_t>(tapOffset + halfTaps);
          mKernelTable[tableIndex] = coefficient;
          coeffSum += coefficient;
        }

        if (std::abs(coeffSum) > 1.0e-12)
        {
          // Normalize each row once so runtime path can skip coefficient-sum division.
          const std::size_t rowStart = static_cast<std::size_t>(phaseIndex) * static_cast<std::size_t>(tapCount);
          for (int tapIndex = 0; tapIndex < tapCount; ++tapIndex)
            mKernelTable[rowStart + static_cast<std::size_t>(tapIndex)] /= coeffSum;
        }
      }

      mKernelHalfTaps = halfTaps;
      mKernelCutoff = cutoff;
    }

    template <typename InputSample>
    [[nodiscard]] static double ReadClampedSample(const InputSample *input, int inputFrames, int sampleIndex)
    {
      const int clampedIndex = std::clamp(sampleIndex, 0, inputFrames - 1);
      return static_cast<double>(input[clampedIndex]);
    }

    template <typename InputSample, typename OutputSample>
    int ProcessLinear(const InputSample *input,
                      int inputFrames,
                      OutputSample *output,
                      int outputFrames) const
    {
      const double sourceStep = static_cast<double>(inputFrames) / static_cast<double>(outputFrames);
      for (int outputIndex = 0; outputIndex < outputFrames; ++outputIndex)
      {
        const double sourcePosition = (static_cast<double>(outputIndex) + 0.5) * sourceStep - 0.5;
        const int lowerIndex = static_cast<int>(std::floor(sourcePosition));
        const int upperIndex = lowerIndex + 1;
        const double fraction = sourcePosition - static_cast<double>(lowerIndex);
        const double lowerSample = ReadClampedSample(input, inputFrames, lowerIndex);
        const double upperSample = ReadClampedSample(input, inputFrames, upperIndex);
        output[outputIndex] = static_cast<OutputSample>(lowerSample + (upperSample - lowerSample) * fraction);
      }
      return outputFrames;
    }

    template <typename InputSample, typename OutputSample>
    int ProcessSinc(const InputSample *input,
                    int inputFrames,
                    OutputSample *output,
                    int outputFrames) const
    {
      const int halfTaps = GetHalfTapsForQuality();
      const int tapCount = 2 * halfTaps + 1;
      const double sourceStep = static_cast<double>(inputFrames) / static_cast<double>(outputFrames);
      const double cutoff = std::min(static_cast<double>(outputFrames) / static_cast<double>(inputFrames), 1.0);
      EnsureKernelTable(halfTaps, cutoff);

      for (int outputIndex = 0; outputIndex < outputFrames; ++outputIndex)
      {
        // Map one output sample to a fractional source position.
        const double sourcePosition = (static_cast<double>(outputIndex) + 0.5) * sourceStep - 0.5;
        const int centerIndex = static_cast<int>(std::floor(sourcePosition));
        const double fractional = sourcePosition - static_cast<double>(centerIndex);
        int phaseIndex = static_cast<int>(fractional * static_cast<double>(kKernelPhaseCount));
        phaseIndex = std::clamp(phaseIndex, 0, kKernelPhaseCount - 1);
        const std::size_t kernelRow = static_cast<std::size_t>(phaseIndex) * static_cast<std::size_t>(tapCount);
        double weightedSum = 0.0;

        for (int tapOffset = -halfTaps; tapOffset <= halfTaps; ++tapOffset)
        {
          const int sourceIndex = centerIndex + tapOffset;
          const double coefficient = mKernelTable[kernelRow + static_cast<std::size_t>(tapOffset + halfTaps)];
          weightedSum += ReadClampedSample(input, inputFrames, sourceIndex) * coefficient;
        }

        output[outputIndex] = static_cast<OutputSample>(weightedSum);
      }

      return outputFrames;
    }
  };
}