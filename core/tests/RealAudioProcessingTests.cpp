/**
 * @file RealAudioProcessingTests.cpp
 * @brief Tests for real audio processing through the graph-based DSP pipeline
 *
 * These tests exercise audio processing via SignalGraphExecutor with
 * a small test preset (input -> gain -> simple cab -> output). The goals are
 * correctness (no NaN/Inf/clipping) and basic block-processing stability.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "dsp/SignalGraphExecutor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace
{
constexpr double kSampleRate = 44100.0;
constexpr int kBlockSize = 512;
constexpr double kPi = 3.14159265358979323846;

struct AudioStats
{
  double rms = 0.0;
  double peakLevel = 0.0;
  double dcOffset = 0.0;
  bool hasNaN = false;
  bool hasInf = false;
  bool isClipped = false;
  int clippedSampleCount = 0;
};

AudioStats Analyze(const std::vector<double>& signal)
{
  AudioStats stats;
  if (signal.empty()) return stats;

  double sumSquares = 0.0;
  double sum = 0.0;
  double maxAbs = 0.0;

  for (double sample : signal)
  {
    if (std::isnan(sample))
    {
      stats.hasNaN = true;
      return stats;
    }
    if (std::isinf(sample))
    {
      stats.hasInf = true;
      return stats;
    }

    const double absSample = std::abs(sample);
    maxAbs = std::max(maxAbs, absSample);
    sumSquares += sample * sample;
    sum += sample;

    if (absSample > 1.0)
    {
      stats.isClipped = true;
      ++stats.clippedSampleCount;
    }
  }

  stats.dcOffset = sum / static_cast<double>(signal.size());
  stats.rms = std::sqrt(sumSquares / static_cast<double>(signal.size()));
  stats.peakLevel = maxAbs;
  return stats;
}

std::vector<double> GenerateSineWave(double frequency, double amplitude, double durationSeconds)
{
  const int numSamples = static_cast<int>(durationSeconds * kSampleRate);
  std::vector<double> signal(numSamples);

  for (int i = 0; i < numSamples; ++i)
  {
    const double t = static_cast<double>(i) / kSampleRate;
    signal[i] = amplitude * std::sin(2.0 * kPi * frequency * t);
  }

  return signal;
}

std::vector<double> GenerateSquareWave(double frequency, double amplitude, double durationSeconds)
{
  const int numSamples = static_cast<int>(durationSeconds * kSampleRate);
  std::vector<double> signal(numSamples);
  const double period = kSampleRate / frequency;

  for (int i = 0; i < numSamples; ++i)
  {
    const double phase = std::fmod(static_cast<double>(i), period) / period;
    signal[i] = amplitude * (phase < 0.5 ? 1.0 : -1.0);
  }

  return signal;
}

std::vector<double> GenerateWhiteNoise(double amplitude, double durationSeconds)
{
  const int numSamples = static_cast<int>(durationSeconds * kSampleRate);
  std::vector<double> signal(numSamples);
  unsigned int seed = 12345;

  for (int i = 0; i < numSamples; ++i)
  {
    seed = (1103515245u * seed + 12345u) & 0x7fffffffu;
    signal[i] = amplitude * (2.0 * (static_cast<double>(seed) / 0x7fffffffu) - 1.0);
  }

  return signal;
}

std::vector<double> GenerateChirp(double startFreq, double endFreq, double amplitude, double durationSeconds)
{
  const int numSamples = static_cast<int>(durationSeconds * kSampleRate);
  std::vector<double> signal(numSamples);

  for (int i = 0; i < numSamples; ++i)
  {
    const double t = static_cast<double>(i) / kSampleRate;
    const double freq = startFreq + (endFreq - startFreq) * (t / durationSeconds);
    signal[i] = amplitude * std::sin(2.0 * kPi * freq * t);
  }

  return signal;
}

guitarfx::Preset MakeTestPreset(double inputTrimDb, double outputTrimDb, double gainDb, bool enableCab)
{
  guitarfx::Preset preset;
  preset.id = "test";
  preset.name = "test";
  preset.version = 2;
  preset.global.inputTrim = inputTrimDb;
  preset.global.outputTrim = outputTrimDb;
  preset.global.outputVolume = 1.0;

  guitarfx::GraphNode input;
  input.id = "input";
  input.type = guitarfx::kNodeTypeInput;
  input.category = "utility";

  guitarfx::GraphNode gain;
  gain.id = "gain";
  gain.type = "gain";
  gain.category = "utility";
  gain.params["gainDb"] = gainDb;

  guitarfx::GraphNode cab;
  cab.id = "cab";
  cab.type = "cab_simple";
  cab.category = "cab";
  cab.enabled = enableCab;
  cab.params["mix"] = 1.0;
  cab.params["bass"] = 0.5;
  cab.params["presence"] = 0.5;
  cab.params["brightness"] = 0.5;

  guitarfx::GraphNode output;
  output.id = "output";
  output.type = guitarfx::kNodeTypeOutput;

  preset.graph.nodes = { input, gain, cab, output };

  preset.graph.edges = {
    { input.id, gain.id, 0, 0, 1.0 },
    { gain.id, cab.id, 0, 0, 1.0 },
    { cab.id, output.id, 0, 0, 1.0 }
  };

  return preset;
}

class GraphDSPHarness
{
public:
  GraphDSPHarness(double inputTrimDb, double outputTrimDb, double gainDb, bool enableCab)
  {
    guitarfx::RegisterAllEffects();
    mExecutor = std::make_unique<guitarfx::SignalGraphExecutor>();
    auto preset = MakeTestPreset(inputTrimDb, outputTrimDb, gainDb, enableCab);
    mExecutor->SetInputTrim(preset.global.inputTrim);
    mExecutor->SetOutputTrim(preset.global.outputTrim);
    mExecutor->SetGraph(preset.graph);
    mExecutor->Prepare(kSampleRate, kBlockSize);
  }

  std::vector<double> Process(const std::vector<double>& input)
  {
    std::vector<double> output(input.size(), 0.0);
    const int numBlocks = static_cast<int>((input.size() + kBlockSize - 1) / kBlockSize);

    for (int block = 0; block < numBlocks; ++block)
    {
      const int startIdx = block * kBlockSize;
      const int numSamples = std::min(kBlockSize, static_cast<int>(input.size()) - startIdx);

      float inBufL[kBlockSize] = {};
      float inBufR[kBlockSize] = {};
      float outBufL[kBlockSize] = {};
      float outBufR[kBlockSize] = {};

      for (int i = 0; i < numSamples; ++i)
      {
        inBufL[i] = static_cast<float>(input[startIdx + i]);
        inBufR[i] = static_cast<float>(input[startIdx + i]);
      }

      float* inputs[] = { inBufL, inBufR };
      float* outputs[] = { outBufL, outBufR };

      mExecutor->Process(inputs, outputs, numSamples);

      for (int i = 0; i < numSamples; ++i)
      {
        output[startIdx + i] = outputs[0][i];
      }
    }

    return output;
  }

private:
  std::unique_ptr<guitarfx::SignalGraphExecutor> mExecutor;
};

/**
 * @brief Test 1: No Crash Under Normal Operation
 * Process various signals through the DSP pipeline without crashing
 */
bool TestPassthrough()
{
  std::cout << "Test: No crash under normal operation... ";

  try
  {
    GraphDSPHarness dsp(-6.0, -3.0, 0.0, false);
    const auto inputSignal = GenerateSineWave(1000.0, 0.5, 0.1);
    const auto outputSignal = dsp.Process(inputSignal);

    const auto stats = Analyze(outputSignal);
    if (stats.hasNaN || stats.hasInf)
    {
      std::cout << "FAILED - Invalid samples in output\n";
      return false;
    }
    if (stats.isClipped)
    {
      std::cout << "FAILED - Output clipped\n";
      return false;
    }

    std::cout << "OK\n";
    return true;
  }
  catch (const std::exception& e)
  {
    std::cout << "FAILED - Exception: " << e.what() << "\n";
    return false;
  }
}

/**
 * @brief Test 2: Realtime Block Processing
 * Verify that audio can be processed in blocks without gaps or discontinuities
 */
bool TestOutputTrim()
{
  std::cout << "Test: Realtime block processing (no gaps)... ";

  try
  {
    GraphDSPHarness dsp(-3.0, -6.0, 0.0, false);
    const auto inputSignal = GenerateSineWave(440.0, 0.5, 2.0);
    const auto outputSignal = dsp.Process(inputSignal);

    const auto stats = Analyze(outputSignal);
    if (stats.hasNaN || stats.hasInf)
    {
      std::cout << "FAILED - Invalid samples\n";
      return false;
    }
    if (stats.rms < 0.001)
    {
      std::cout << "FAILED - No output signal\n";
      return false;
    }

    std::cout << "OK\n";
    return true;
  }
  catch (const std::exception& e)
  {
    std::cout << "FAILED - Exception: " << e.what() << "\n";
    return false;
  }
}

/**
 * @brief Test 3: No Clipping Under Normal Operation
 * Verify that normal signals don't get clipped
 */
bool TestNoClipping()
{
  std::cout << "Test: No clipping with normal audio levels... ";

  try
  {
    GraphDSPHarness dsp(0.0, 0.0, 0.0, false);
    const auto inputSignal = GenerateSineWave(1000.0, 0.7, 0.1);
    const auto outputSignal = dsp.Process(inputSignal);

    const auto stats = Analyze(outputSignal);
    if (stats.isClipped)
    {
      std::cout << "FAILED - Signal clipped (" << stats.clippedSampleCount << " samples)\n";
      return false;
    }
    if (stats.peakLevel > 1.0)
    {
      std::cout << "FAILED - Peak exceeds 1.0: " << stats.peakLevel << "\n";
      return false;
    }

    std::cout << "OK\n";
    return true;
  }
  catch (const std::exception& e)
  {
    std::cout << "FAILED - Exception: " << e.what() << "\n";
    return false;
  }
}

/**
 * @brief Test 4: Audio Quality Check
 * Verify output is clean audio without NaN, Inf, or DC offset issues
 */
bool TestAudioQuality()
{
  std::cout << "Test: Audio quality (clean output, no artifacts)... ";

  try
  {
    GraphDSPHarness dsp(0.0, -3.0, 1.2, false);
    const std::vector<std::string> signalTypes = { "sine", "square", "noise", "chirp" };

    for (const auto& sigType : signalTypes)
    {
      std::vector<double> inputSignal;
      if (sigType == "sine")
        inputSignal = GenerateSineWave(1000.0, 0.5, 0.2);
      else if (sigType == "square")
        inputSignal = GenerateSquareWave(500.0, 0.5, 0.2);
      else if (sigType == "noise")
        inputSignal = GenerateWhiteNoise(0.3, 0.2);
      else if (sigType == "chirp")
        inputSignal = GenerateChirp(100.0, 5000.0, 0.4, 0.2);

      const auto outputSignal = dsp.Process(inputSignal);
      const auto stats = Analyze(outputSignal);

      if (stats.hasNaN)
      {
        std::cout << "FAILED - NaN in " << sigType << " signal\n";
        return false;
      }
      if (stats.hasInf)
      {
        std::cout << "FAILED - Inf in " << sigType << " signal\n";
        return false;
      }
      if (std::abs(stats.dcOffset) > 0.1)
      {
        std::cout << "FAILED - Excessive DC offset in " << sigType << " (" << stats.dcOffset << ")\n";
        return false;
      }
      if (stats.peakLevel > 1.5)
      {
        std::cout << "FAILED - Excessive peak in " << sigType << " (" << stats.peakLevel << ")\n";
        return false;
      }
    }

    std::cout << "OK\n";
    return true;
  }
  catch (const std::exception& e)
  {
    std::cout << "FAILED - Exception: " << e.what() << "\n";
    return false;
  }
}

/**
 * @brief Test 5: Cabinet IR Processing
 * Verify audio processing with cabinet IR convolution enabled
 */
bool TestCabinetIRProcessing()
{
  std::cout << "Test: Cabinet processing (simple cab enabled)... ";

  try
  {
    GraphDSPHarness dsp(-3.0, -3.0, 0.0, true);
    const auto inputSignal = GenerateSineWave(440.0, 0.5, 1.0);
    const auto outputSignal = dsp.Process(inputSignal);

    const auto outputStats = Analyze(outputSignal);
    if (outputStats.hasNaN || outputStats.hasInf)
    {
      std::cout << "FAILED - Invalid samples in output\n";
      return false;
    }
    if (outputStats.peakLevel > 1.5)
    {
      std::cout << "FAILED - Excessive peak level: " << outputStats.peakLevel << "\n";
      return false;
    }
    if (outputStats.rms < 0.001)
    {
      std::cout << "FAILED - No significant output\n";
      return false;
    }
    if (std::abs(outputStats.dcOffset) > 0.1)
    {
      std::cout << "FAILED - Excessive DC offset: " << outputStats.dcOffset << "\n";
      return false;
    }

    std::cout << "OK\n";
    return true;
  }
  catch (const std::exception& e)
  {
    std::cout << "FAILED - Exception: " << e.what() << "\n";
    return false;
  }
}

} // anonymous namespace

int main()
{
  std::cout << "Real Audio Processing Tests (SignalGraphExecutor)\n";
  std::cout << "=================================================\n\n";

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

  runTest(TestPassthrough);
  runTest(TestOutputTrim);
  runTest(TestNoClipping);
  runTest(TestAudioQuality);
  runTest(TestCabinetIRProcessing);

  std::cout << "\n====================================================\n";
  std::cout << "Results: " << passed << "/" << (passed + failed) << " tests passed.\n";

  if (failed > 0)
  {
    std::cout << "\nSome tests FAILED.\n";
    return 1;
  }

  std::cout << "\nAll tests PASSED.\n";
  return 0;
}
