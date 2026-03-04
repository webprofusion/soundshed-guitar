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
#include "dsp/EffectGuids.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/AutoArpEffect.h"

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
  std::vector<float> outputL(kTestBlockSize, 0.0f);
  std::vector<float> outputR(kTestBlockSize, 0.0f);

  const int blocksToProcess = (effectType == guitarfx::EffectGuids::kSynthSaw) ? 8 : 1;

  // Generate a phase-continuous 440 Hz sine across all blocks so that
  // pitch-tracking effects (YIN) do not see a phase discontinuity at each
  // block boundary which would corrupt the autocorrelation.
  const int totalSamples = kTestBlockSize * blocksToProcess;
  std::vector<float> inputL(static_cast<size_t>(totalSamples));
  std::vector<float> inputR(static_cast<size_t>(totalSamples));
  GenerateSineWave(inputL, 440.0, 0.5);
  GenerateSineWave(inputR, 440.0, 0.5);

  float* outputs[2] = {outputL.data(), outputR.data()};

  try
  {
    for (int block = 0; block < blocksToProcess; ++block)
    {
      std::fill(outputL.begin(), outputL.end(), 0.0f);
      std::fill(outputR.begin(), outputR.end(), 0.0f);
      float* blkInputs[2] = {
        inputL.data() + block * kTestBlockSize,
        inputR.data() + block * kTestBlockSize
      };
      effect->Process(blkInputs, outputs, kTestBlockSize);
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

// ════════════════════════════════════════════════════════════════════
// Auto-Arpeggiator specific tests
// ════════════════════════════════════════════════════════════════════

namespace
{

bool TestAutoArpSpecific()
{
  std::cout << "\n--- AutoArpEffect Specific Tests ---\n";
  int passed = 0, failed = 0;

  auto makeArp = []() -> std::unique_ptr<guitarfx::AutoArpEffect> {
    auto e = std::make_unique<guitarfx::AutoArpEffect>();
    e->Prepare(kTestSampleRate, kTestBlockSize);
    return e;
  };

  std::vector<float> inL(kTestBlockSize), inR(kTestBlockSize);
  std::vector<float> outL(kTestBlockSize, 0.f), outR(kTestBlockSize, 0.f);
  GenerateSineWave(inL, 440.0, 0.5);
  GenerateSineWave(inR, 440.0, 0.5);
  float* ins[2]  = {inL.data(), inR.data()};
  float* outs[2] = {outL.data(), outR.data()};

  // Test 1: SetParam round-trip for timing params
  {
    auto e = makeArp();
    e->SetParam("bpm",      130.0);
    e->SetParam("stepRate", 2.0);
    e->SetParam("numSteps", 3.0);
    const bool ok = (e->GetParam("bpm") == 130.0) &&
                    (e->GetParam("stepRate") == 2.0) &&
                    (e->GetParam("numSteps") == 3.0);
    std::cout << "  SetParam round-trip (bpm/stepRate/numSteps): " << (ok ? "PASS" : "FAIL") << "\n";
    ok ? ++passed : ++failed;
  }

  // Test 2: Process produces non-zero output (step 0 = 0 st, bypass path)
  {
    auto e = makeArp();
    std::fill(outL.begin(), outL.end(), 0.f);
    std::fill(outR.begin(), outR.end(), 0.f);
    e->Process(ins, outs, kTestBlockSize);
    const auto analysis = AnalyzeSignal(outL);
    const bool ok = !analysis.isAllZeros && !analysis.hasNaN && !analysis.hasInf;
    std::cout << "  Process non-zero output (bypass path):       " << (ok ? "PASS" : "FAIL") << "\n";
    ok ? ++passed : ++failed;
  }

  // Test 3: Reset clears phase — two separate instances started at same time
  //         should produce identical output right after Reset().
  {
    auto e = makeArp();
    e->SetParam("bpm", 90.0);
    // Process a few blocks to advance phase
    for (int b = 0; b < 4; ++b)
      e->Process(ins, outs, kTestBlockSize);
    e->Reset();
    // After reset, replaying the same input should match a freshly-prepared instance
    auto fresh = makeArp();
    fresh->SetParam("bpm", 90.0);

    std::vector<float> out1(kTestBlockSize, 0.f), out1r(kTestBlockSize, 0.f);
    std::vector<float> out2(kTestBlockSize, 0.f), out2r(kTestBlockSize, 0.f);
    float* o1[2] = {out1.data(), out1r.data()};
    float* o2[2] = {out2.data(), out2r.data()};
    e->Process(ins, o1, kTestBlockSize);
    fresh->Process(ins, o2, kTestBlockSize);

    bool match = true;
    for (int i = 0; i < kTestBlockSize && match; ++i)
      if (std::abs(out1[static_cast<size_t>(i)] - out2[static_cast<size_t>(i)]) > 1e-5f)
        match = false;
    std::cout << "  Reset restores initial state:                " << (match ? "PASS" : "FAIL") << "\n";
    match ? ++passed : ++failed;
  }

  // Test 4: Custom pattern — changing step0 changes behaviour
  {
    auto e = makeArp();
    e->SetParam("pattern", 4.0);   // Custom
    e->SetParam("step0",   0.0);   // Root = bypass stretch
    e->SetParam("numSteps", 2.0);
    e->SetParam("step1",   0.0);   // All bypass
    e->SetParam("gate",    1.0);   // Gate fully open
    e->SetParam("attack",  0.0);
    e->SetParam("mix",     1.0);
    std::fill(outL.begin(), outL.end(), 0.f);
    std::fill(outR.begin(), outR.end(), 0.f);
    e->Process(ins, outs, kTestBlockSize);
    const auto analysis = AnalyzeSignal(outL);
    const bool ok = !analysis.isAllZeros;
    std::cout << "  Custom pattern step0=0 produces output:      " << (ok ? "PASS" : "FAIL") << "\n";
    ok ? ++passed : ++failed;
  }

  // Test 5: Switching back to predefined pattern
  {
    auto e = makeArp();
    e->SetParam("pattern", 4.0);  // Custom
    e->SetParam("pattern", 0.0);  // Back to Major Triad — should not crash
    std::fill(outL.begin(), outL.end(), 0.f);
    std::fill(outR.begin(), outR.end(), 0.f);
    e->Process(ins, outs, kTestBlockSize);
    const auto analysis = AnalyzeSignal(outL);
    const bool ok = !analysis.hasNaN && !analysis.hasInf;
    std::cout << "  Switch back to predefined pattern (no NaN):  " << (ok ? "PASS" : "FAIL") << "\n";
    ok ? ++passed : ++failed;
  }

  // Test 6: BPM injection via SetParam("bpm") at multiple BPMs
  {
    bool allOk = true;
    for (const double bpm : {60.0, 120.0, 180.0, 300.0})
    {
      auto e = makeArp();
      e->SetParam("bpm", bpm);
      std::fill(outL.begin(), outL.end(), 0.f);
      std::fill(outR.begin(), outR.end(), 0.f);
      e->Process(ins, outs, kTestBlockSize);
      const auto analysis = AnalyzeSignal(outL);
      if (analysis.hasNaN || analysis.hasInf)
        allOk = false;
    }
    std::cout << "  BPM range 60-300 no NaN/Inf:                 " << (allOk ? "PASS" : "FAIL") << "\n";
    allOk ? ++passed : ++failed;
  }

  std::cout << "AutoArp specific: " << passed << "/" << (passed + failed) << " passed.\n";
  return failed == 0;
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

  // AutoArpeggiator-specific behavioural tests
  if (!TestAutoArpSpecific())
    return 1;

  return 0;
}
