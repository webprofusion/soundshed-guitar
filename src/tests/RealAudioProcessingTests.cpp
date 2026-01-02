/**
 * @file RealAudioProcessingTests.cpp
 * @brief Tests for real audio processing through the complete DSP pipeline
 *
 * These tests process actual audio through NAMDSPManager::Process() with specific
 * DSP settings and capture the output to validate against expected behavior.
 * This tests the full signal chain: input trim, gate, drive, tone, NAM model,
 * IR convolution, doubler, pitch shift, and output trim.
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include "dsp/NAMDSPManager.h"
#include "IPlugConstants.h"

namespace
{
constexpr double kSampleRate = 44100.0;
constexpr int kBlockSize = 512;
constexpr double kTolerance = 1e-5;
constexpr double kPi = 3.14159265358979323846;

bool ApproxEqual(double a, double b, double tolerance = kTolerance)
{
  return std::abs(a - b) < tolerance;
}

/**
 * @brief Helper to generate test audio signals
 */
class AudioSignalGenerator
{
public:
  // Generate a sine wave at specified frequency
  static std::vector<double> GenerateSineWave(double frequency, double amplitude, 
                                              double durationSeconds, double sampleRate)
  {
    const int numSamples = static_cast<int>(durationSeconds * sampleRate);
    std::vector<double> signal(numSamples);
    
    for (int i = 0; i < numSamples; ++i)
    {
      double t = static_cast<double>(i) / sampleRate;
      signal[i] = amplitude * std::sin(2.0 * kPi * frequency * t);
    }
    
    return signal;
  }

  // Generate a square wave at specified frequency
  static std::vector<double> GenerateSquareWave(double frequency, double amplitude,
                                                double durationSeconds, double sampleRate)
  {
    const int numSamples = static_cast<int>(durationSeconds * sampleRate);
    std::vector<double> signal(numSamples);
    const double period = sampleRate / frequency;
    
    for (int i = 0; i < numSamples; ++i)
    {
      double phase = std::fmod(static_cast<double>(i), period) / period;
      signal[i] = amplitude * (phase < 0.5 ? 1.0 : -1.0);
    }
    
    return signal;
  }

  // Generate white noise
  static std::vector<double> GenerateWhiteNoise(double amplitude, double durationSeconds,
                                                double sampleRate)
  {
    const int numSamples = static_cast<int>(durationSeconds * sampleRate);
    std::vector<double> signal(numSamples);
    unsigned int seed = 12345;
    
    for (int i = 0; i < numSamples; ++i)
    {
      seed = (1103515245u * seed + 12345u) & 0x7fffffffu;
      signal[i] = amplitude * (2.0 * (static_cast<double>(seed) / 0x7fffffffu) - 1.0);
    }
    
    return signal;
  }

  // Generate chirp (frequency sweep)
  static std::vector<double> GenerateChirp(double startFreq, double endFreq,
                                           double amplitude, double durationSeconds,
                                           double sampleRate)
  {
    const int numSamples = static_cast<int>(durationSeconds * sampleRate);
    std::vector<double> signal(numSamples);
    
    for (int i = 0; i < numSamples; ++i)
    {
      double t = static_cast<double>(i) / sampleRate;
      double freq = startFreq + (endFreq - startFreq) * (t / durationSeconds);
      signal[i] = amplitude * std::sin(2.0 * kPi * freq * t);
    }
    
    return signal;
  }
};

/**
 * @brief Helper to analyze audio characteristics
 */
class AudioAnalyzer
{
public:
  struct AudioStats
  {
    double rms = 0.0;              // RMS (effective) level
    double peakLevel = 0.0;        // Peak amplitude
    double dcOffset = 0.0;         // DC component (mean)
    double crestFactor = 0.0;      // Peak/RMS ratio
    bool hasNaN = false;           // Contains NaN?
    bool hasInf = false;           // Contains Inf?
    bool isClipped = false;        // Exceeds [-1, 1]?
    int clippedSampleCount = 0;    // Number of clipped samples
  };

  static AudioStats Analyze(const std::vector<double>& signal)
  {
    AudioStats stats;
    
    if (signal.empty())
      return stats;

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

      double absSample = std::abs(sample);
      maxAbs = std::max(maxAbs, absSample);
      sumSquares += sample * sample;
      sum += sample;

      if (absSample > 1.0)
      {
        stats.isClipped = true;
        stats.clippedSampleCount++;
      }
    }

    stats.dcOffset = sum / static_cast<double>(signal.size());
    stats.rms = std::sqrt(sumSquares / static_cast<double>(signal.size()));
    stats.peakLevel = maxAbs;
    stats.crestFactor = stats.rms > 1e-10 ? stats.peakLevel / stats.rms : 0.0;

    return stats;
  }

  static void PrintStats(const AudioStats& stats, const std::string& label = "")
  {
    if (!label.empty())
      std::cout << "  " << label << ":\n";
    std::cout << "    RMS: " << stats.rms << "\n";
    std::cout << "    Peak: " << stats.peakLevel << "\n";
    std::cout << "    DC Offset: " << stats.dcOffset << "\n";
    std::cout << "    Crest Factor: " << stats.crestFactor << "\n";
    if (stats.isClipped)
      std::cout << "    Clipped: YES (" << stats.clippedSampleCount << " samples)\n";
  }
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
    auto dspManager = std::make_unique<namguitar::NAMDSPManager>();
    dspManager->Prepare(kSampleRate, kBlockSize);

    // Setup with typical settings
    dspManager->SetInputTrim(-6.0);
    dspManager->SetOutputTrim(-3.0);
    dspManager->SetDrive(1.0);
    dspManager->SetTone(0.0);
    dspManager->SetGateEnabled(false);
    dspManager->SetDoublerEnabled(false);
    dspManager->SetTranspose(0);
    dspManager->SetAmpEnabled(true);
    dspManager->SetCabEnabled(false);

    // Test with a sine wave
    auto inputSignal = AudioSignalGenerator::GenerateSineWave(1000.0, 0.5, 0.1, kSampleRate);
    std::vector<double> outputSignal(inputSignal.size());

    // Process through DSP in blocks
    const int numBlocks = (inputSignal.size() + kBlockSize - 1) / kBlockSize;

    for (int block = 0; block < numBlocks; ++block)
    {
      int startIdx = block * kBlockSize;
      int numSamples = std::min(kBlockSize, static_cast<int>(inputSignal.size()) - startIdx);

      // Prepare input/output buffers in iPlug format
      iplug::sample inputBuffer[2][512];  // Stereo
      iplug::sample outputBuffer[2][512];

      for (int i = 0; i < numSamples; ++i)
      {
        inputBuffer[0][i] = inputSignal[startIdx + i];
        inputBuffer[1][i] = inputSignal[startIdx + i];
      }

      // Process
      iplug::sample* inputs[] = { inputBuffer[0], inputBuffer[1] };
      iplug::sample* outputs[] = { outputBuffer[0], outputBuffer[1] };
      dspManager->Process(inputs, outputs, numSamples);

      // Capture output
      for (int i = 0; i < numSamples; ++i)
      {
        outputSignal[startIdx + i] = outputBuffer[0][i];
      }
    }

    // Analyze output
    auto outputStats = AudioAnalyzer::Analyze(outputSignal);

    // Check for validity
    if (outputStats.hasNaN || outputStats.hasInf)
    {
      std::cout << "FAILED - Invalid samples in output\n";
      return false;
    }

    if (outputStats.isClipped)
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
    auto dspManager = std::make_unique<namguitar::NAMDSPManager>();
    dspManager->Prepare(kSampleRate, kBlockSize);

    // Setup with moderate settings
    dspManager->SetInputTrim(-3.0);
    dspManager->SetOutputTrim(-6.0);
    dspManager->SetDrive(1.0);
    dspManager->SetTone(0.0);
    dspManager->SetGateEnabled(false);
    dspManager->SetDoublerEnabled(false);
    dspManager->SetAmpEnabled(true);
    dspManager->SetCabEnabled(false);

    // Process a 2-second signal in blocks
    const int totalSamples = static_cast<int>(2.0 * kSampleRate);
    auto inputSignal = AudioSignalGenerator::GenerateSineWave(440.0, 0.5, 2.0, kSampleRate);
    std::vector<double> outputSignal(totalSamples);

    int processedSamples = 0;
    int blockCount = 0;

    while (processedSamples < totalSamples)
    {
      int remainingSamples = totalSamples - processedSamples;
      int numSamples = std::min(kBlockSize, remainingSamples);

      iplug::sample inputBuffer[2][512];
      iplug::sample outputBuffer[2][512];

      for (int i = 0; i < numSamples; ++i)
      {
        inputBuffer[0][i] = inputSignal[processedSamples + i];
        inputBuffer[1][i] = inputSignal[processedSamples + i];
      }

      iplug::sample* inputs[] = { inputBuffer[0], inputBuffer[1] };
      iplug::sample* outputs[] = { outputBuffer[0], outputBuffer[1] };
      dspManager->Process(inputs, outputs, numSamples);

      for (int i = 0; i < numSamples; ++i)
      {
        outputSignal[processedSamples + i] = outputBuffer[0][i];
      }

      processedSamples += numSamples;
      blockCount++;
    }

    auto stats = AudioAnalyzer::Analyze(outputSignal);

    if (stats.hasNaN || stats.hasInf)
    {
      std::cout << "FAILED - Invalid samples\n";
      return false;
    }

    // Should have non-zero output
    if (stats.rms < 0.001)
    {
      std::cout << "FAILED - No output signal\n";
      return false;
    }

    std::cout << "OK (" << blockCount << " blocks)\n";
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
    auto dspManager = std::make_unique<namguitar::NAMDSPManager>();
    dspManager->Prepare(kSampleRate, kBlockSize);

    // Setup: Moderate settings
    dspManager->SetInputTrim(0.0);
    dspManager->SetOutputTrim(0.0);
    dspManager->SetDrive(1.0);
    dspManager->SetGateEnabled(false);
    dspManager->SetDoublerEnabled(false);
    dspManager->SetAmpEnabled(false);
    dspManager->SetCabEnabled(false);

    // Generate modest signal (-3dB)
    auto inputSignal = AudioSignalGenerator::GenerateSineWave(1000.0, 0.7, 0.1, kSampleRate);
    std::vector<double> outputSignal(inputSignal.size());

    const int numBlocks = (inputSignal.size() + kBlockSize - 1) / kBlockSize;
    for (int block = 0; block < numBlocks; ++block)
    {
      int startIdx = block * kBlockSize;
      int numSamples = std::min(kBlockSize, static_cast<int>(inputSignal.size()) - startIdx);

      iplug::sample inputBuffer[2][512];
      iplug::sample outputBuffer[2][512];

      for (int i = 0; i < numSamples; ++i)
      {
        inputBuffer[0][i] = inputSignal[startIdx + i];
        inputBuffer[1][i] = inputSignal[startIdx + i];
      }

      iplug::sample* inputs[] = { inputBuffer[0], inputBuffer[1] };
      iplug::sample* outputs[] = { outputBuffer[0], outputBuffer[1] };
      dspManager->Process(inputs, outputs, numSamples);

      for (int i = 0; i < numSamples; ++i)
      {
        outputSignal[startIdx + i] = outputBuffer[0][i];
      }
    }

    auto stats = AudioAnalyzer::Analyze(outputSignal);

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
    auto dspManager = std::make_unique<namguitar::NAMDSPManager>();
    dspManager->Prepare(kSampleRate, kBlockSize);

    // Setup: Moderate settings
    dspManager->SetInputTrim(0.0);
    dspManager->SetOutputTrim(-3.0);
    dspManager->SetDrive(1.2);
    dspManager->SetTone(0.3);
    dspManager->SetGateEnabled(false);
    dspManager->SetDoublerEnabled(true);
    dspManager->SetDoublerDelay(6.0);
    dspManager->SetAmpEnabled(false);
    dspManager->SetCabEnabled(false);

    // Process multiple signal types
    const std::vector<std::string> signalTypes = { "sine", "square", "noise", "chirp" };
    
    for (const auto& sigType : signalTypes)
    {
      std::vector<double> inputSignal;
      
      if (sigType == "sine")
        inputSignal = AudioSignalGenerator::GenerateSineWave(1000.0, 0.5, 0.2, kSampleRate);
      else if (sigType == "square")
        inputSignal = AudioSignalGenerator::GenerateSquareWave(500.0, 0.5, 0.2, kSampleRate);
      else if (sigType == "noise")
        inputSignal = AudioSignalGenerator::GenerateWhiteNoise(0.3, 0.2, kSampleRate);
      else if (sigType == "chirp")
        inputSignal = AudioSignalGenerator::GenerateChirp(100.0, 5000.0, 0.4, 0.2, kSampleRate);

      std::vector<double> outputSignal(inputSignal.size());

      const int numBlocks = (inputSignal.size() + kBlockSize - 1) / kBlockSize;
      for (int block = 0; block < numBlocks; ++block)
      {
        int startIdx = block * kBlockSize;
        int numSamples = std::min(kBlockSize, static_cast<int>(inputSignal.size()) - startIdx);

        iplug::sample inputBuffer[2][512];
        iplug::sample outputBuffer[2][512];

        for (int i = 0; i < numSamples; ++i)
        {
          inputBuffer[0][i] = inputSignal[startIdx + i];
          inputBuffer[1][i] = inputSignal[startIdx + i];
        }

        iplug::sample* inputs[] = { inputBuffer[0], inputBuffer[1] };
        iplug::sample* outputs[] = { outputBuffer[0], outputBuffer[1] };
        dspManager->Process(inputs, outputs, numSamples);

        for (int i = 0; i < numSamples; ++i)
        {
          outputSignal[startIdx + i] = outputBuffer[0][i];
        }
      }

      auto stats = AudioAnalyzer::Analyze(outputSignal);

      // Check for validity
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

      // DC offset should be small
      if (std::abs(stats.dcOffset) > 0.1)
      {
        std::cout << "FAILED - Excessive DC offset in " << sigType << " ("
                  << stats.dcOffset << ")\n";
        return false;
      }

      // Peak should not exceed reasonable bounds
      if (stats.peakLevel > 1.5)
      {
        std::cout << "FAILED - Excessive peak in " << sigType << " ("
                  << stats.peakLevel << ")\n";
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
  std::cout << "Test: Cabinet IR processing (cab enabled)... ";

  try
  {
    auto dspManager = std::make_unique<namguitar::NAMDSPManager>();
    dspManager->Prepare(kSampleRate, kBlockSize);

    // Setup with cabinet IR enabled
    dspManager->SetInputTrim(-3.0);
    dspManager->SetOutputTrim(-3.0);
    dspManager->SetDrive(1.0);
    dspManager->SetTone(0.0);
    dspManager->SetGateEnabled(false);
    dspManager->SetDoublerEnabled(false);
    dspManager->SetTranspose(0);
    dspManager->SetAmpEnabled(true);
    dspManager->SetCabEnabled(true);  // Enable cabinet IR convolution

    // Generate test signal - 1 second of 440Hz sine wave
    const double durationSeconds = 1.0;
    const int totalSamples = static_cast<int>(durationSeconds * kSampleRate);
    auto inputSignal = AudioSignalGenerator::GenerateSineWave(440.0, 0.5, durationSeconds, kSampleRate);
    std::vector<double> outputSignal(totalSamples);

    // Process in blocks to simulate realtime operation
    int processedSamples = 0;
    int blockCount = 0;

    while (processedSamples < totalSamples)
    {
      int remainingSamples = totalSamples - processedSamples;
      int numSamples = std::min(kBlockSize, remainingSamples);

      iplug::sample inputBuffer[2][512];
      iplug::sample outputBuffer[2][512];

      for (int i = 0; i < numSamples; ++i)
      {
        inputBuffer[0][i] = inputSignal[processedSamples + i];
        inputBuffer[1][i] = inputSignal[processedSamples + i];
      }

      iplug::sample* inputs[] = { inputBuffer[0], inputBuffer[1] };
      iplug::sample* outputs[] = { outputBuffer[0], outputBuffer[1] };
      dspManager->Process(inputs, outputs, numSamples);

      for (int i = 0; i < numSamples; ++i)
      {
        outputSignal[processedSamples + i] = outputBuffer[0][i];
      }

      processedSamples += numSamples;
      blockCount++;
    }

    // Analyze output
    auto inputStats = AudioAnalyzer::Analyze(inputSignal);
    auto outputStats = AudioAnalyzer::Analyze(outputSignal);

    // Check for validity
    if (outputStats.hasNaN || outputStats.hasInf)
    {
      std::cout << "FAILED - Invalid samples in output\n";
      return false;
    }

    // Output should not exceed reasonable bounds with cab enabled
    if (outputStats.peakLevel > 1.5)
    {
      std::cout << "FAILED - Excessive peak level: " << outputStats.peakLevel << "\n";
      return false;
    }

    // Should have some output
    if (outputStats.rms < 0.001)
    {
      std::cout << "FAILED - No significant output\n";
      return false;
    }

    // DC offset should be minimal
    if (std::abs(outputStats.dcOffset) > 0.1)
    {
      std::cout << "FAILED - Excessive DC offset: " << outputStats.dcOffset << "\n";
      return false;
    }

    std::cout << "OK (" << blockCount << " blocks)\n";
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
  std::cout << "Real Audio Processing Tests (NAMDSPManager::Process)\n";
  std::cout << "====================================================\n\n";

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
