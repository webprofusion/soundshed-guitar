#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

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
    static constexpr int kHalfTaps = 64;
    static constexpr int kHighPerformanceHalfTaps = 12;
    static constexpr int kGuardFrames = 8;
    static constexpr double kPi = 3.14159265358979323846;

    double mSourceRate = 44100.0;
    double mTargetRate = 44100.0;
    int mMaxInputFrames = 0;
    int mMaxOutputFrames = 0;
    SampleRateConversionQuality mQuality = SampleRateConversionQuality::Highest;

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
      const double sourceStep = static_cast<double>(inputFrames) / static_cast<double>(outputFrames);
      const double cutoff = std::min(static_cast<double>(outputFrames) / static_cast<double>(inputFrames), 1.0);

      for (int outputIndex = 0; outputIndex < outputFrames; ++outputIndex)
      {
        const double sourcePosition = (static_cast<double>(outputIndex) + 0.5) * sourceStep - 0.5;
        const int centerIndex = static_cast<int>(std::floor(sourcePosition));
        double weightedSum = 0.0;
        double coefficientSum = 0.0;

        for (int tapOffset = -halfTaps; tapOffset <= halfTaps; ++tapOffset)
        {
          const int sourceIndex = centerIndex + tapOffset;
          const double distance = sourcePosition - static_cast<double>(sourceIndex);
          const double coefficient = cutoff * Sinc(distance * cutoff) * Blackman(distance, halfTaps);
          weightedSum += ReadClampedSample(input, inputFrames, sourceIndex) * coefficient;
          coefficientSum += coefficient;
        }

        if (std::abs(coefficientSum) > 1.0e-12)
          weightedSum /= coefficientSum;
        output[outputIndex] = static_cast<OutputSample>(weightedSum);
      }

      return outputFrames;
    }
  };
}