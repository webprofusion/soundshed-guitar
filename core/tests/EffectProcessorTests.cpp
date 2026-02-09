/**
 * @file EffectProcessorTests.cpp
 * @brief Tests for individual effect processors with default settings
 *
 * This test validates that each registered effect processor can:
 * 1. Be created from the registry
 * 2. Be prepared with valid sample rate and block size
 * 3. Process audio without producing NaN, Inf, or silence
 */

#include <cmath>
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>

#include "dsp/EffectRegistry.h"
#include "dsp/EffectProcessor.h"
#include "dsp/effects/BuiltinEffects.h"

namespace
{

constexpr double kTestSampleRate = 48000.0;
constexpr int kTestBlockSize = 512;
constexpr double kPi = 3.14159265358979323846;

// Generate a simple sine wave for testing
void GenerateSineWave(std::vector<float>& buffer, double frequency, double amplitude = 0.5)
{
  for (size_t i = 0; i < buffer.size(); ++i)
  {
    double phase = 2.0 * kPi * frequency * static_cast<double>(i) / kTestSampleRate;
    buffer[i] = static_cast<float>(amplitude * std::sin(phase));
  }
}

// Analyze signal for validity
struct SignalAnalysis
{
  bool hasNaN = false;
  bool hasInf = false;
  bool isAllZeros = false;
  bool isAllSameValue = false;
  double peakValue = 0.0;
  double rmsValue = 0.0;
};

SignalAnalysis AnalyzeSignal(const std::vector<float>& buffer)
{
  SignalAnalysis result;
  
  if (buffer.empty())
    return result;

  double sumSquares = 0.0;
  double peak = 0.0;
  float firstValue = buffer[0];
  bool allSame = true;
  bool allZero = true;

  for (const auto& sample : buffer)
  {
    if (std::isnan(sample))
    {
      result.hasNaN = true;
      return result; // Early exit on NaN
    }
    
    if (std::isinf(sample))
    {
      result.hasInf = true;
      return result; // Early exit on Inf
    }

    double absSample = std::abs(sample);
    if (absSample > peak)
      peak = absSample;
    
    sumSquares += sample * sample;
    
    if (absSample > 1e-10)
      allZero = false;
    
    if (std::abs(sample - firstValue) > 1e-10)
      allSame = false;
  }

  result.peakValue = peak;
  result.rmsValue = std::sqrt(sumSquares / buffer.size());
  result.isAllZeros = allZero;
  result.isAllSameValue = allSame;

  return result;
}

bool TestEffectProcessor(const std::string& effectType)
{
  auto& registry = guitarfx::EffectRegistry::Instance();
  
  // Create effect
  auto effect = registry.Create(effectType);
  if (!effect)
  {
    std::cout << "  ERROR: Failed to create effect\n";
    return false;
  }

  // Prepare effect
  effect->Prepare(kTestSampleRate, kTestBlockSize);
  effect->Reset();

  // Create test buffers
  std::vector<float> inputL(kTestBlockSize);
  std::vector<float> inputR(kTestBlockSize);
  std::vector<float> outputL(kTestBlockSize, 0.0f);
  std::vector<float> outputR(kTestBlockSize, 0.0f);

  // Generate 440Hz test tone
  GenerateSineWave(inputL, 440.0, 0.5);
  GenerateSineWave(inputR, 440.0, 0.5);

  // Process audio (allow pitch-tracking effects to settle)
  float* inputs[2] = {inputL.data(), inputR.data()};
  float* outputs[2] = {outputL.data(), outputR.data()};

  const int blocksToProcess = (effectType == "synth_saw") ? 8 : 1;
  try
  {
    for (int block = 0; block < blocksToProcess; ++block)
    {
      std::fill(outputL.begin(), outputL.end(), 0.0f);
      std::fill(outputR.begin(), outputR.end(), 0.0f);
      effect->Process(inputs, outputs, kTestBlockSize);
    }
  }
  catch (const std::exception& ex)
  {
    std::cout << "  ERROR: Process threw exception: " << ex.what() << "\n";
    return false;
  }

  // Analyze output
  auto inputAnalysis = AnalyzeSignal(inputL);
  auto outputAnalysis = AnalyzeSignal(outputL);

  if (outputAnalysis.hasNaN)
  {
    std::cout << "  FAIL: Output contains NaN\n";
    return false;
  }

  if (outputAnalysis.hasInf)
  {
    std::cout << "  FAIL: Output contains Inf\n";
    return false;
  }

  if (outputAnalysis.isAllZeros)
  {
    std::cout << "  FAIL: Output is all zeros (no signal)\n";
    return false;
  }

  if (outputAnalysis.peakValue > 100.0)
  {
    std::cout << "  FAIL: Output peak excessively high (" << outputAnalysis.peakValue << ")\n";
    return false;
  }

  // Success - output is valid
  std::cout << "  PASS (peak=" << std::fixed << std::setprecision(3) 
            << outputAnalysis.peakValue << ", rms=" << outputAnalysis.rmsValue << ")\n";
  return true;
}

} // anonymous namespace

int main()
{
  std::cout << "========================================\n";
  std::cout << "Effect Processor Tests\n";
  std::cout << "========================================\n\n";

  // Register all effects before testing
  guitarfx::RegisterAllEffects();

  auto& registry = guitarfx::EffectRegistry::Instance();
  auto allTypes = registry.GetAllTypes();

  if (allTypes.empty())
  {
    std::cerr << "ERROR: No effects registered!\n";
    return 1;
  }

  std::cout << "Testing " << allTypes.size() << " registered effects...\n\n";

  int passed = 0;
  int failed = 0;
  std::vector<std::string> failedEffects;

  for (const auto& info : allTypes)
  {
    std::cout << std::left << std::setw(30) << info.displayName << " (" << info.type << ")";
    
    if (TestEffectProcessor(info.type))
    {
      ++passed;
    }
    else
    {
      ++failed;
      failedEffects.push_back(info.type);
    }
  }

  std::cout << "\n========================================\n";
  std::cout << "Results: " << passed << "/" << allTypes.size() << " effects passed\n";
  
  if (failed > 0)
  {
    std::cout << "\nFailed effects:\n";
    for (const auto& name : failedEffects)
    {
      std::cout << "  - " << name << "\n";
    }
    std::cout << "\n";
    return 1;
  }

  std::cout << "\nAll effects PASSED.\n";
  return 0;
}
