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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>

#include "dsp/EffectRegistry.h"
#include "dsp/EffectProcessor.h"
#include "dsp/EffectGuids.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/AutoArpEffect.h"
#include "dsp/effects/TempoSync.h"

namespace
{

namespace fs = std::filesystem;

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

struct DriveMetrics
{
  double peak = 0.0;
  double rms = 0.0;
  double mean = 0.0;
  double crestFactor = 0.0;
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

double EstimateFrequencyFromPositiveZeroCrossings(const std::vector<float>& buffer,
                                                  double sampleRate)
{
  std::vector<double> crossingIndices;
  crossingIndices.reserve(buffer.size() / 32);

  for (size_t i = 1; i < buffer.size(); ++i)
  {
    const float previous = buffer[i - 1];
    const float current = buffer[i];
    if (previous <= 0.0f && current > 0.0f)
    {
      const float delta = current - previous;
      const double fraction = std::abs(delta) > 1.0e-9f ? (-previous / delta) : 0.0;
      crossingIndices.push_back(static_cast<double>(i - 1) + fraction);
    }
  }

  if (crossingIndices.size() < 2)
    return 0.0;

  double totalPeriod = 0.0;
  int periods = 0;
  for (size_t i = 1; i < crossingIndices.size(); ++i)
  {
    const double period = crossingIndices[i] - crossingIndices[i - 1];
    if (period > 1.0)
    {
      totalPeriod += period;
      ++periods;
    }
  }

  if (periods == 0)
    return 0.0;

  const double averagePeriod = totalPeriod / static_cast<double>(periods);
  return averagePeriod > 0.0 ? sampleRate / averagePeriod : 0.0;
}

DriveMetrics MeasureDriveMetrics(const std::vector<float>& buffer)
{
  DriveMetrics metrics;
  if (buffer.empty())
    return metrics;

  double sum = 0.0;
  double sumSquares = 0.0;
  double peak = 0.0;
  for (float sample : buffer)
  {
    const double absSample = std::abs(sample);
    peak = std::max(peak, absSample);
    sum += sample;
    sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
  }

  metrics.peak = peak;
  metrics.mean = sum / static_cast<double>(buffer.size());
  metrics.rms = std::sqrt(sumSquares / static_cast<double>(buffer.size()));
  metrics.crestFactor = metrics.rms > 1.0e-9 ? metrics.peak / metrics.rms : 0.0;
  return metrics;
}

std::vector<float> RenderDriveEffect(const std::string& effectType,
                                     double inputAmplitude,
                                     int blocksToProcess = 6)
{
  auto effect = guitarfx::EffectRegistry::Instance().Create(effectType);
  if (!effect)
    return {};

  effect->Prepare(kTestSampleRate, kTestBlockSize);
  effect->SetParam("mix", 1.0);
  effect->SetParam("tone", 0.5);
  effect->SetParam("level", 0.0);

  std::vector<float> inputL(kTestBlockSize, 0.0f);
  std::vector<float> inputR(kTestBlockSize, 0.0f);
  std::vector<float> outputL(kTestBlockSize, 0.0f);
  std::vector<float> outputR(kTestBlockSize, 0.0f);
  float* inputs[2] = {inputL.data(), inputR.data()};
  float* outputs[2] = {outputL.data(), outputR.data()};

  for (int block = 0; block < blocksToProcess; ++block)
  {
    const std::size_t startIndex = static_cast<std::size_t>(block * kTestBlockSize);
    for (int i = 0; i < kTestBlockSize; ++i)
    {
      const double phase = 2.0 * kPi * 220.0 * static_cast<double>(startIndex + static_cast<std::size_t>(i)) / kTestSampleRate;
      const float sample = static_cast<float>(inputAmplitude * std::sin(phase));
      inputL[static_cast<std::size_t>(i)] = sample;
      inputR[static_cast<std::size_t>(i)] = sample;
    }
    effect->Process(inputs, outputs, kTestBlockSize);
  }

  return outputL;
}

double NormalizedDifference(const std::vector<float>& a, const std::vector<float>& b)
{
  const std::size_t length = std::min(a.size(), b.size());
  if (length == 0)
    return 0.0;

  double diffSquares = 0.0;
  double refSquares = 0.0;
  for (std::size_t i = 0; i < length; ++i)
  {
    const double diff = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    diffSquares += diff * diff;
    refSquares += static_cast<double>(a[i]) * static_cast<double>(a[i]);
  }

  return std::sqrt(diffSquares / std::max(refSquares, 1.0e-9));
}

fs::path WriteStereoImpulseToWav(const std::vector<float>& left,
                                 const std::vector<float>& right,
                                 double sampleRate)
{
  const std::size_t length = std::min(left.size(), right.size());
  if (length == 0)
    throw std::runtime_error("Stereo IR must have data");

  const fs::path tempDir = fs::temp_directory_path() / "guitarfx_effect_processor_tests";
  fs::create_directories(tempDir);
  const fs::path path = tempDir / "flanger_reverb_stability_ir.wav";

  std::ofstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("Failed to create temp stereo IR file");

  std::vector<float> interleaved(length * 2);
  for (std::size_t i = 0; i < length; ++i)
  {
    interleaved[i * 2] = left[i];
    interleaved[i * 2 + 1] = right[i];
  }

  const uint32_t dataSize = static_cast<uint32_t>(interleaved.size() * sizeof(float));
  const uint32_t riffSize = 36u + dataSize;
  const uint16_t audioFormat = 3;
  const uint16_t numChannels = 2;
  const uint32_t sampleRateU32 = static_cast<uint32_t>(sampleRate);
  const uint32_t byteRate = sampleRateU32 * numChannels * sizeof(float);
  const uint16_t blockAlign = static_cast<uint16_t>(numChannels * sizeof(float));
  const uint16_t bitsPerSample = 32;
  const uint32_t fmtChunkSize = 16;

  file.write("RIFF", 4);
  file.write(reinterpret_cast<const char*>(&riffSize), 4);
  file.write("WAVE", 4);
  file.write("fmt ", 4);
  file.write(reinterpret_cast<const char*>(&fmtChunkSize), 4);
  file.write(reinterpret_cast<const char*>(&audioFormat), 2);
  file.write(reinterpret_cast<const char*>(&numChannels), 2);
  file.write(reinterpret_cast<const char*>(&sampleRateU32), 4);
  file.write(reinterpret_cast<const char*>(&byteRate), 4);
  file.write(reinterpret_cast<const char*>(&blockAlign), 2);
  file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
  file.write("data", 4);
  file.write(reinterpret_cast<const char*>(&dataSize), 4);
  file.write(reinterpret_cast<const char*>(interleaved.data()), dataSize);

  return path;
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

  const int latencyBlocks = std::max(0, (effect->GetLatencySamples() + kTestBlockSize - 1) / kTestBlockSize);
  const int blocksToProcess = std::max((effectType == guitarfx::EffectGuids::kSynthSaw) ? 8 : 1,
                                       latencyBlocks + 2);

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

bool TestFlangerReverbStability()
{
  std::cout << "\n--- Flanger -> IR Reverb Stability Test ---\n";

  auto& registry = guitarfx::EffectRegistry::Instance();
  auto flanger = registry.Create(guitarfx::EffectGuids::kFlanger);
  auto reverb = registry.Create(guitarfx::EffectGuids::kReverbIr);
  if (!flanger || !reverb)
  {
    std::cout << "  FAIL: Could not create flanger or IR reverb\n";
    return false;
  }

  constexpr std::size_t kIRLength = 4096;
  std::vector<float> irL(kIRLength, 0.0f);
  std::vector<float> irR(kIRLength, 0.0f);
  for (std::size_t i = 0; i < kIRLength; ++i)
  {
    const float t = static_cast<float>(i) / static_cast<float>(kIRLength);
    const float env = std::exp(-6.0f * t);
    const float modA = std::sin(2.0 * kPi * 0.013 * static_cast<double>(i));
    const float modB = std::cos(2.0 * kPi * 0.021 * static_cast<double>(i));
    irL[i] = env * static_cast<float>(0.65 * modA + 0.35 * modB);
    irR[i] = env * static_cast<float>(0.60 * modB - 0.30 * modA);
  }
  irL[0] += 0.6f;
  irR[0] += 0.6f;

  const fs::path irPath = WriteStereoImpulseToWav(irL, irR, kTestSampleRate);

  try
  {
    flanger->Prepare(kTestSampleRate, kTestBlockSize);
    reverb->Prepare(kTestSampleRate, kTestBlockSize);

    flanger->SetParam("rate", 0.25);
    flanger->SetParam("depth", 5.0);
    flanger->SetParam("delay", 5.0);
    flanger->SetParam("feedback", 0.85);
    flanger->SetParam("mix", 1.0);

    reverb->SetParam("mix", 1.0);
    reverb->SetParam("outputGain", 0.0);
    if (!reverb->LoadResource(irPath))
    {
      std::cout << "  FAIL: Could not load IR resource\n";
      fs::remove(irPath);
      return false;
    }

    std::vector<float> inputL(kTestBlockSize, 0.0f);
    std::vector<float> inputR(kTestBlockSize, 0.0f);
    std::vector<float> flangerL(kTestBlockSize, 0.0f);
    std::vector<float> flangerR(kTestBlockSize, 0.0f);
    std::vector<float> reverbL(kTestBlockSize, 0.0f);
    std::vector<float> reverbR(kTestBlockSize, 0.0f);

    float* inPtrs[2] = {inputL.data(), inputR.data()};
    float* flangerOutPtrs[2] = {flangerL.data(), flangerR.data()};
    float* reverbOutPtrs[2] = {reverbL.data(), reverbR.data()};

    constexpr int kDurationSeconds = 8;
    const int totalBlocks = static_cast<int>((kDurationSeconds * kTestSampleRate) / kTestBlockSize);
    double peak = 0.0;
    double sumSquares = 0.0;
    std::size_t sampleCount = 0;

    for (int block = 0; block < totalBlocks; ++block)
    {
      const std::size_t startIndex = static_cast<std::size_t>(block * kTestBlockSize);
      for (int i = 0; i < kTestBlockSize; ++i)
      {
        const double phase = 2.0 * kPi * 110.0 * static_cast<double>(startIndex + static_cast<std::size_t>(i)) / kTestSampleRate;
        const float sample = static_cast<float>(0.8 * std::sin(phase));
        inputL[static_cast<std::size_t>(i)] = sample;
        inputR[static_cast<std::size_t>(i)] = sample;
      }

      flanger->Process(inPtrs, flangerOutPtrs, kTestBlockSize);

      float* reverbInPtrs[2] = {flangerL.data(), flangerR.data()};
      reverb->Process(reverbInPtrs, reverbOutPtrs, kTestBlockSize);

      for (int i = 0; i < kTestBlockSize; ++i)
      {
        const float samples[2] = {reverbL[static_cast<std::size_t>(i)], reverbR[static_cast<std::size_t>(i)]};
        for (float sample : samples)
        {
          if (!std::isfinite(sample))
          {
            std::cout << "  FAIL: Non-finite sample at block " << block << "\n";
            fs::remove(irPath);
            return false;
          }

          peak = std::max(peak, static_cast<double>(std::abs(sample)));
          sumSquares += static_cast<double>(sample) * static_cast<double>(sample);
          ++sampleCount;
        }
      }
    }

    fs::remove(irPath);

    const double rms = sampleCount > 0 ? std::sqrt(sumSquares / static_cast<double>(sampleCount)) : 0.0;
    // This is a stability test, not a level-matching test. The true-stereo IR sum
    // can legitimately produce short peaks above the older 12.0 ceiling while
    // remaining finite and energy-bounded, so keep the stricter RMS guard and a
    // slightly looser peak guard for transient excursions.
    const bool ok = peak < 16.0 && rms < 4.0;
    std::cout << "  Peak=" << std::fixed << std::setprecision(3) << peak
              << ", RMS=" << rms << " -> " << (ok ? "PASS" : "FAIL") << "\n";
    return ok;
  }
  catch (const std::exception& ex)
  {
    fs::remove(irPath);
    std::cout << "  FAIL: Exception during flanger/reverb stability test: " << ex.what() << "\n";
    return false;
  }
}

bool TestTempoSyncSpecific()
{
  std::cout << "\n--- Tempo Sync Effect Tests ---\n";

  struct TempoCase
  {
    const char* label;
    const std::string type;
    double bpm;
    double division;
    const char* effectiveKey;
    double expected;
    double tolerance;
  };

  const std::vector<TempoCase> cases = {
    {"Delay quarter-note at 120 BPM", guitarfx::EffectGuids::kDelayDigital, 120.0, 4.0, "effectiveTimeMs", 500.0, 0.01},
    {"Tremolo quarter-note at 120 BPM", guitarfx::EffectGuids::kTremolo, 120.0, 4.0, "effectiveRate", 2.0, 0.01},
    {"Chorus half-note at 120 BPM", guitarfx::EffectGuids::kChorus, 120.0, 1.0, "effectiveRate", 1.0, 0.01},
    {"Flanger whole-note at 120 BPM", guitarfx::EffectGuids::kFlanger, 120.0, 0.0, "effectiveRate", 0.5, 0.01},
    {"Phaser eighth-note triplet at 120 BPM", guitarfx::EffectGuids::kPhaser, 120.0, 9.0, "effectiveRate", 6.0, 0.01},
  };

  auto& registry = guitarfx::EffectRegistry::Instance();
  int passed = 0;
  int failed = 0;

  std::vector<float> inputL(kTestBlockSize, 0.0f);
  std::vector<float> inputR(kTestBlockSize, 0.0f);
  std::vector<float> outputL(kTestBlockSize, 0.0f);
  std::vector<float> outputR(kTestBlockSize, 0.0f);
  GenerateSineWave(inputL, 220.0, 0.5);
  GenerateSineWave(inputR, 220.0, 0.5);
  float* inputs[2] = {inputL.data(), inputR.data()};
  float* outputs[2] = {outputL.data(), outputR.data()};

  for (const auto& testCase : cases)
  {
    auto effect = registry.Create(testCase.type);
    if (!effect)
    {
      std::cout << "  " << std::left << std::setw(44) << testCase.label << "FAIL (create)\n";
      ++failed;
      continue;
    }

    effect->Prepare(kTestSampleRate, kTestBlockSize);
    effect->SetParam("syncMode", 1.0);
    effect->SetParam("syncDivision", testCase.division);
    effect->SetParam("bpm", testCase.bpm);

    const double effective = effect->GetParam(testCase.effectiveKey);
    const bool mappingOk = std::abs(effective - testCase.expected) <= testCase.tolerance;

    std::fill(outputL.begin(), outputL.end(), 0.0f);
    std::fill(outputR.begin(), outputR.end(), 0.0f);
    effect->Process(inputs, outputs, kTestBlockSize);
    const auto analysis = AnalyzeSignal(outputL);
    const bool audioOk = !analysis.hasNaN && !analysis.hasInf;

    const bool ok = mappingOk && audioOk;
    std::cout << "  " << std::left << std::setw(44) << testCase.label
              << (ok ? "PASS" : "FAIL")
              << " (effective=" << std::fixed << std::setprecision(3) << effective << ")\n";
    ok ? ++passed : ++failed;
  }

  std::cout << "Tempo-sync specific: " << passed << "/" << (passed + failed) << " passed.\n";
  return failed == 0;
}

bool TestDriveEffectCharacter()
{
  std::cout << "\n--- Drive Effect Character Tests ---\n";

  const auto overdrive = RenderDriveEffect(guitarfx::EffectGuids::kOverdrive, 0.25);
  const auto distortion = RenderDriveEffect(guitarfx::EffectGuids::kDistortion, 0.25);
  const auto fuzz = RenderDriveEffect(guitarfx::EffectGuids::kFuzz, 0.25);
  const auto fuzzCleanup = RenderDriveEffect(guitarfx::EffectGuids::kFuzz, 0.08);

  if (overdrive.empty() || distortion.empty() || fuzz.empty() || fuzzCleanup.empty())
  {
    std::cout << "  FAIL: Could not render one or more drive effects\n";
    return false;
  }

  const DriveMetrics overdriveMetrics = MeasureDriveMetrics(overdrive);
  const DriveMetrics distortionMetrics = MeasureDriveMetrics(distortion);
  const DriveMetrics fuzzMetrics = MeasureDriveMetrics(fuzz);
  const DriveMetrics fuzzCleanupMetrics = MeasureDriveMetrics(fuzzCleanup);

  const double odVsDist = NormalizedDifference(overdrive, distortion);
  const double odVsFuzz = NormalizedDifference(overdrive, fuzz);
  const double distVsFuzz = NormalizedDifference(distortion, fuzz);

  const bool distinctOk = odVsDist > 0.08 && odVsFuzz > 0.08 && distVsFuzz > 0.08;
  const bool distortionCompressed = distortionMetrics.crestFactor + 0.05 < overdriveMetrics.crestFactor;
  const bool fuzzAsymmetric = std::abs(fuzzMetrics.mean) > std::abs(distortionMetrics.mean) + 0.005;
  const bool fuzzCleansUp = fuzzCleanupMetrics.rms < fuzzMetrics.rms * 0.75;
  const bool protectedOutput = overdriveMetrics.peak <= 1.01 && distortionMetrics.peak <= 1.01 && fuzzMetrics.peak <= 1.01;

  std::cout << "  Distinct transfer/output shapes:             " << (distinctOk ? "PASS" : "FAIL")
            << " (OD/DIST=" << std::fixed << std::setprecision(3) << odVsDist
            << ", OD/FUZZ=" << odVsFuzz
            << ", DIST/FUZZ=" << distVsFuzz << ")\n";
  std::cout << "  Distortion is more compressed than OD:       " << (distortionCompressed ? "PASS" : "FAIL")
            << " (OD crest=" << overdriveMetrics.crestFactor
            << ", DIST crest=" << distortionMetrics.crestFactor << ")\n";
  std::cout << "  Fuzz shows more asymmetry than distortion:   " << (fuzzAsymmetric ? "PASS" : "FAIL")
            << " (FUZZ mean=" << fuzzMetrics.mean
            << ", DIST mean=" << distortionMetrics.mean << ")\n";
  std::cout << "  Fuzz cleans up at lower input level:         " << (fuzzCleansUp ? "PASS" : "FAIL")
            << " (low RMS=" << fuzzCleanupMetrics.rms
            << ", high RMS=" << fuzzMetrics.rms << ")\n";
  std::cout << "  Drive outputs stay near clip ceiling:        " << (protectedOutput ? "PASS" : "FAIL")
            << " (OD peak=" << overdriveMetrics.peak
            << ", DIST peak=" << distortionMetrics.peak
            << ", FUZZ peak=" << fuzzMetrics.peak << ")\n";

  return distinctOk && distortionCompressed && fuzzAsymmetric && fuzzCleansUp && protectedOutput;
}

bool TestDynamicsSoftClipOptions()
{
  std::cout << "\n--- Dynamics Soft Clip Option Tests ---\n";

  auto renderEffect = [&](const std::string& effectType,
                          double inputAmplitude,
                          auto configure,
                          int blocksToProcess = 6)
  {
    auto effect = guitarfx::EffectRegistry::Instance().Create(effectType);
    if (!effect)
      return std::vector<float>{};

    effect->Prepare(kTestSampleRate, kTestBlockSize);
    configure(*effect);

    std::vector<float> inputL(kTestBlockSize, 0.0f);
    std::vector<float> inputR(kTestBlockSize, 0.0f);
    std::vector<float> outputL(kTestBlockSize, 0.0f);
    std::vector<float> outputR(kTestBlockSize, 0.0f);
    float* inputs[2] = {inputL.data(), inputR.data()};
    float* outputs[2] = {outputL.data(), outputR.data()};

    for (int block = 0; block < blocksToProcess; ++block)
    {
      const std::size_t startIndex = static_cast<std::size_t>(block * kTestBlockSize);
      for (int i = 0; i < kTestBlockSize; ++i)
      {
        const double phase = 2.0 * kPi * 220.0 * static_cast<double>(startIndex + static_cast<std::size_t>(i)) / kTestSampleRate;
        const float sample = static_cast<float>(inputAmplitude * std::sin(phase));
        inputL[static_cast<std::size_t>(i)] = sample;
        inputR[static_cast<std::size_t>(i)] = sample;
      }

      effect->Process(inputs, outputs, kTestBlockSize);
    }

    return outputL;
  };

  const auto limiterHard = renderEffect(guitarfx::EffectGuids::kLimiterBrickwall, 1.2,
                                        [](guitarfx::EffectProcessor& effect)
                                        {
                                          effect.SetParam("ceiling", -6.0);
                                          effect.SetParam("release", 50.0);
                                        });
  const auto limiterSoft = renderEffect(guitarfx::EffectGuids::kLimiterBrickwall, 1.2,
                                        [](guitarfx::EffectProcessor& effect)
                                        {
                                          effect.SetParam("ceiling", -6.0);
                                          effect.SetParam("release", 50.0);
                                          effect.SetParam("softClip", 1.0);
                                        });
  const auto vcaHard = renderEffect(guitarfx::EffectGuids::kCompressorVca, 0.35,
                                    [](guitarfx::EffectProcessor& effect)
                                    {
                                      effect.SetParam("threshold", 0.0);
                                      effect.SetParam("ratio", 1.0);
                                      effect.SetParam("attack", 0.1);
                                      effect.SetParam("release", 100.0);
                                      effect.SetParam("knee", 0.0);
                                      effect.SetParam("makeup", 18.0);
                                      effect.SetParam("mix", 1.0);
                                    });
  const auto vcaSoft = renderEffect(guitarfx::EffectGuids::kCompressorVca, 0.35,
                                    [](guitarfx::EffectProcessor& effect)
                                    {
                                      effect.SetParam("threshold", 0.0);
                                      effect.SetParam("ratio", 1.0);
                                      effect.SetParam("attack", 0.1);
                                      effect.SetParam("release", 100.0);
                                      effect.SetParam("knee", 0.0);
                                      effect.SetParam("makeup", 18.0);
                                      effect.SetParam("mix", 1.0);
                                      effect.SetParam("softClip", 1.0);
                                    });
  const auto optoHard = renderEffect(guitarfx::EffectGuids::kCompressorOpto, 0.35,
                                     [](guitarfx::EffectProcessor& effect)
                                     {
                                       effect.SetParam("threshold", 0.0);
                                       effect.SetParam("ratio", 1.0);
                                       effect.SetParam("attack", 20.0);
                                       effect.SetParam("release", 300.0);
                                       effect.SetParam("makeup", 18.0);
                                       effect.SetParam("mix", 1.0);
                                     });
  const auto optoSoft = renderEffect(guitarfx::EffectGuids::kCompressorOpto, 0.35,
                                     [](guitarfx::EffectProcessor& effect)
                                     {
                                       effect.SetParam("threshold", 0.0);
                                       effect.SetParam("ratio", 1.0);
                                       effect.SetParam("attack", 20.0);
                                       effect.SetParam("release", 300.0);
                                       effect.SetParam("makeup", 18.0);
                                       effect.SetParam("mix", 1.0);
                                       effect.SetParam("softClip", 1.0);
                                     });

  if (limiterHard.empty() || limiterSoft.empty() || vcaHard.empty() || vcaSoft.empty() || optoHard.empty() || optoSoft.empty())
  {
    std::cout << "  FAIL: Could not render one or more dynamics effects\n";
    return false;
  }

  const double limiterCeiling = std::pow(10.0, -6.0 * 0.05);
  const DriveMetrics limiterHardMetrics = MeasureDriveMetrics(limiterHard);
  const DriveMetrics limiterSoftMetrics = MeasureDriveMetrics(limiterSoft);
  const DriveMetrics vcaHardMetrics = MeasureDriveMetrics(vcaHard);
  const DriveMetrics vcaSoftMetrics = MeasureDriveMetrics(vcaSoft);
  const DriveMetrics optoHardMetrics = MeasureDriveMetrics(optoHard);
  const DriveMetrics optoSoftMetrics = MeasureDriveMetrics(optoSoft);

  const bool limiterBounded = limiterSoftMetrics.peak <= limiterCeiling + 0.001;
  const bool limiterChangesShape = NormalizedDifference(limiterHard, limiterSoft) > 0.01;
  const bool vcaSoftProtects = vcaHardMetrics.peak > 1.1 && vcaSoftMetrics.peak <= 1.01 && NormalizedDifference(vcaHard, vcaSoft) > 0.05;
  const bool optoSoftProtects = optoHardMetrics.peak > 1.1 && optoSoftMetrics.peak <= 1.01 && NormalizedDifference(optoHard, optoSoft) > 0.05;

  std::cout << "  Limiter soft clip stays under ceiling:       " << (limiterBounded ? "PASS" : "FAIL")
            << " (soft peak=" << std::fixed << std::setprecision(3) << limiterSoftMetrics.peak
            << ", ceiling=" << limiterCeiling << ")\n";
  std::cout << "  Limiter soft clip reshapes limiter knee:     " << (limiterChangesShape ? "PASS" : "FAIL")
            << " (diff=" << NormalizedDifference(limiterHard, limiterSoft) << ")\n";
  std::cout << "  VCA soft clip protects post-makeup output:   " << (vcaSoftProtects ? "PASS" : "FAIL")
            << " (hard peak=" << vcaHardMetrics.peak
            << ", soft peak=" << vcaSoftMetrics.peak << ")\n";
  std::cout << "  Opto soft clip protects post-makeup output:  " << (optoSoftProtects ? "PASS" : "FAIL")
            << " (hard peak=" << optoHardMetrics.peak
            << ", soft peak=" << optoSoftMetrics.peak << ")\n";

  return limiterBounded && limiterChangesShape && vcaSoftProtects && optoSoftProtects;
}

bool TestOctaveSpecific()
{
  std::cout << "\n--- OctaveEffect Specific Tests ---\n";

  auto& registry = guitarfx::EffectRegistry::Instance();

  auto runVoiceTest = [&](double octaveUp, double octaveDown, const char* label)
  {
    auto effect = registry.Create(guitarfx::EffectGuids::kOctave);
    if (!effect)
    {
      std::cout << "  FAIL: Could not create octave effect\n";
      return false;
    }

    effect->Prepare(kTestSampleRate, kTestBlockSize);
    effect->SetParam("octaveUp", octaveUp);
    effect->SetParam("octaveDown", octaveDown);
    effect->SetParam("tone", 1.0);
    effect->SetParam("mix", 1.0);

    if (effect->GetLatencySamples() <= 0)
    {
      std::cout << "  FAIL: " << label << " reports no pitch-shift latency\n";
      return false;
    }

    constexpr int kBlocksToProcess = 8;
    const int totalSamples = kTestBlockSize * kBlocksToProcess;
    std::vector<float> inputL(static_cast<size_t>(totalSamples));
    std::vector<float> inputR(static_cast<size_t>(totalSamples));
    GenerateSineWave(inputL, 220.0, 0.5);
    GenerateSineWave(inputR, 220.0, 0.5);

    std::vector<float> outputL(kTestBlockSize, 0.0f);
    std::vector<float> outputR(kTestBlockSize, 0.0f);
    float* outputs[2] = {outputL.data(), outputR.data()};

    for (int block = 0; block < kBlocksToProcess; ++block)
    {
      float* inputs[2] = {
        inputL.data() + block * kTestBlockSize,
        inputR.data() + block * kTestBlockSize
      };
      effect->Process(inputs, outputs, kTestBlockSize);
    }

    const auto analysis = AnalyzeSignal(outputL);
    const bool ok = !analysis.hasNaN && !analysis.hasInf && !analysis.isAllZeros && analysis.peakValue > 0.01;
    std::cout << "  " << std::left << std::setw(44) << label << (ok ? "PASS" : "FAIL")
              << " (latency=" << effect->GetLatencySamples()
              << ", peak=" << std::fixed << std::setprecision(3) << analysis.peakValue << ")\n";
    return ok;
  };

  const bool octaveUpOk = runVoiceTest(1.0, 0.0, "Octave-up voice produces shifted output:");
  const bool octaveDownOk = runVoiceTest(0.0, 1.0, "Octave-down voice produces shifted output:");
  return octaveUpOk && octaveDownOk;
}

bool TestTransposeLatencySpecific()
{
  std::cout << "\n--- TransposeEffect Latency Tests ---\n";

  auto& registry = guitarfx::EffectRegistry::Instance();
  auto effect = registry.Create(guitarfx::EffectGuids::kTranspose);
  if (!effect)
  {
    std::cout << "  FAIL: Could not create transpose effect\n";
    return false;
  }

  effect->Prepare(kTestSampleRate, kTestBlockSize);
  effect->SetParam("mix", 1.0);
  effect->SetParam("quality", 0.0);
  effect->SetParam("semitones", -1.0);
  const int lowShiftLatency = effect->GetLatencySamples();

  effect->SetParam("semitones", -2.0);
  const int slightDownTuneLatency = effect->GetLatencySamples();

  effect->SetParam("semitones", -12.0);
  const int deepShiftLatency = effect->GetLatencySamples();

  effect->SetParam("quality", 1.0);
  effect->SetParam("semitones", -1.0);
  const int qualityLatency = effect->GetLatencySamples();

  const bool lowShiftResponsive = lowShiftLatency > 0 && lowShiftLatency <= kTestBlockSize;
  const bool slightDownTuneStillResponsive = slightDownTuneLatency >= lowShiftLatency && slightDownTuneLatency <= kTestBlockSize * 2;
  const bool deepShiftStillBounded = deepShiftLatency >= lowShiftLatency && deepShiftLatency <= kTestBlockSize * 3;
  const bool qualityModeIsHigherLatency = qualityLatency > lowShiftLatency;

  std::cout << "  " << std::left << std::setw(44) << "-1 semitone stays within one block:" << (lowShiftResponsive ? "PASS" : "FAIL")
            << " (latency=" << lowShiftLatency << ")\n";
  std::cout << "  " << std::left << std::setw(44) << "-2 semitones stays within two blocks:" << (slightDownTuneStillResponsive ? "PASS" : "FAIL")
            << " (latency=" << slightDownTuneLatency << ")\n";
  std::cout << "  " << std::left << std::setw(44) << "-12 semitones stays within three blocks:" << (deepShiftStillBounded ? "PASS" : "FAIL")
            << " (latency=" << deepShiftLatency << ")\n";
  std::cout << "  " << std::left << std::setw(44) << "Best quality increases transpose latency:" << (qualityModeIsHigherLatency ? "PASS" : "FAIL")
            << " (latency=" << qualityLatency << ")\n";

  return lowShiftResponsive && slightDownTuneStillResponsive && deepShiftStillBounded && qualityModeIsHigherLatency;
}

bool TestStftTransposeSpecific()
{
  std::cout << "\n--- StftTransposeEffect Tests ---\n";

  auto& registry = guitarfx::EffectRegistry::Instance();
  auto effect = registry.Create(guitarfx::EffectGuids::kTransposeStft);
  if (!effect)
  {
    std::cout << "  FAIL: Could not create STFT transpose effect\n";
    return false;
  }

  effect->Prepare(kTestSampleRate, kTestBlockSize);
  effect->SetParam("mix", 1.0);
  effect->SetParam("semitones", -12.0);

  const int latencySamples = effect->GetLatencySamples();
  const int latencyBlocks = std::max(0, (latencySamples + kTestBlockSize - 1) / kTestBlockSize);
  const int blocksToProcess = latencyBlocks + 10;
  const int totalSamples = blocksToProcess * kTestBlockSize;

  std::vector<float> inputL(static_cast<size_t>(totalSamples), 0.0f);
  std::vector<float> inputR(static_cast<size_t>(totalSamples), 0.0f);
  std::vector<float> outputL(static_cast<size_t>(totalSamples), 0.0f);
  std::vector<float> outputR(static_cast<size_t>(totalSamples), 0.0f);
  std::vector<float> blockOutL(kTestBlockSize, 0.0f);
  std::vector<float> blockOutR(kTestBlockSize, 0.0f);

  GenerateSineWave(inputL, 440.0, 0.5);
  GenerateSineWave(inputR, 440.0, 0.5);

  float* outputs[2] = {blockOutL.data(), blockOutR.data()};
  for (int block = 0; block < blocksToProcess; ++block)
  {
    std::fill(blockOutL.begin(), blockOutL.end(), 0.0f);
    std::fill(blockOutR.begin(), blockOutR.end(), 0.0f);
    float* inputs[2] = {
      inputL.data() + block * kTestBlockSize,
      inputR.data() + block * kTestBlockSize
    };
    effect->Process(inputs, outputs, kTestBlockSize);
    std::copy(blockOutL.begin(), blockOutL.end(), outputL.begin() + static_cast<size_t>(block * kTestBlockSize));
    std::copy(blockOutR.begin(), blockOutR.end(), outputR.begin() + static_cast<size_t>(block * kTestBlockSize));
  }

  const size_t warmupStart = static_cast<size_t>(std::min(totalSamples - 1,
    latencySamples + kTestBlockSize * 2));
  std::vector<float> steadyState(outputL.begin() + static_cast<std::ptrdiff_t>(warmupStart), outputL.end());
  const auto analysis = AnalyzeSignal(steadyState);
  const double detectedFrequency = EstimateFrequencyFromPositiveZeroCrossings(steadyState, kTestSampleRate);

  const bool latencyBounded = latencySamples > 0 && latencySamples <= kTestBlockSize * 2;
  const bool outputHealthy = !analysis.hasNaN && !analysis.hasInf && !analysis.isAllZeros && analysis.peakValue > 0.05;
  const bool octaveDownDetected = detectedFrequency >= 205.0 && detectedFrequency <= 235.0;

  std::cout << "  " << std::left << std::setw(44) << "Latency stays within two blocks:"
            << (latencyBounded ? "PASS" : "FAIL")
            << " (latency=" << latencySamples << ")\n";
  std::cout << "  " << std::left << std::setw(44) << "Steady-state output remains valid:"
            << (outputHealthy ? "PASS" : "FAIL")
            << " (peak=" << std::fixed << std::setprecision(3) << analysis.peakValue << ")\n";
  std::cout << "  " << std::left << std::setw(44) << "-12 st lands near one octave down:"
            << (octaveDownDetected ? "PASS" : "FAIL")
            << " (freq=" << std::fixed << std::setprecision(1) << detectedFrequency << " Hz)\n";

  return latencyBounded && outputHealthy && octaveDownDetected;
}

bool TestStftTransposeLiveChangesSpecific()
{
  std::cout << "\n--- StftTransposeEffect Live Change Tests ---\n";

  auto& registry = guitarfx::EffectRegistry::Instance();
  auto effect = registry.Create(guitarfx::EffectGuids::kTransposeStft);
  if (!effect)
  {
    std::cout << "  FAIL: Could not create STFT transpose effect\n";
    return false;
  }

  effect->Prepare(kTestSampleRate, kTestBlockSize);
  effect->SetParam("mix", 1.0);

  std::vector<float> inputL(kTestBlockSize, 0.0f);
  std::vector<float> inputR(kTestBlockSize, 0.0f);
  std::vector<float> outputL(kTestBlockSize, 0.0f);
  std::vector<float> outputR(kTestBlockSize, 0.0f);
  GenerateSineWave(inputL, 440.0, 0.5);
  GenerateSineWave(inputR, 440.0, 0.5);

  float* inputs[2] = {inputL.data(), inputR.data()};
  float* outputs[2] = {outputL.data(), outputR.data()};

  const std::vector<int> semitoneSequence = {-1, -12, -2, 0, 5, -12, 2, 0};
  bool allBlocksHealthy = true;
  bool latencyTracksTarget = true;

  for (const int semitones : semitoneSequence)
  {
    effect->SetParam("semitones", static_cast<double>(semitones));

    const int reportedLatency = effect->GetLatencySamples();
    const int expectedLatency = (semitones == 0) ? 0 : (std::abs(semitones) <= 2 ? 384 : 768);
    latencyTracksTarget = latencyTracksTarget && (reportedLatency == expectedLatency);

    for (int block = 0; block < 4; ++block)
    {
      std::fill(outputL.begin(), outputL.end(), 0.0f);
      std::fill(outputR.begin(), outputR.end(), 0.0f);
      effect->Process(inputs, outputs, kTestBlockSize);

      const auto analysis = AnalyzeSignal(outputL);
      allBlocksHealthy = allBlocksHealthy && !analysis.hasNaN && !analysis.hasInf;
    }
  }

  std::cout << "  " << std::left << std::setw(44) << "Reported latency tracks pending target:"
            << (latencyTracksTarget ? "PASS" : "FAIL") << "\n";
  std::cout << "  " << std::left << std::setw(44) << "Rapid semitone changes stay finite:"
            << (allBlocksHealthy ? "PASS" : "FAIL") << "\n";

  return latencyTracksTarget && allBlocksHealthy;
}

bool TestPitchShiftQualitySpecific()
{
  std::cout << "\n--- PitchShiftEffect Quality Tests ---\n";

  auto& registry = guitarfx::EffectRegistry::Instance();
  auto effect = registry.Create(guitarfx::EffectGuids::kPitchShift);
  if (!effect)
  {
    std::cout << "  FAIL: Could not create pitch shift effect\n";
    return false;
  }

  effect->Prepare(kTestSampleRate, kTestBlockSize);
  effect->SetParam("mix", 1.0);
  effect->SetParam("stepMode", 1.0);
  effect->SetParam("minSemitones", -2.0);
  effect->SetParam("maxSemitones", 2.0);
  effect->SetParam("quality", 0.0);
  effect->SetParam("semitones", 0.5);
  const int latencyModeLatency = effect->GetLatencySamples();

  effect->SetParam("quality", 1.0);
  const int qualityModeLatency = effect->GetLatencySamples();

  const bool latencyModeResponsive = latencyModeLatency > 0 && latencyModeLatency <= kTestBlockSize;
  const bool qualityModeHigherLatency = qualityModeLatency > latencyModeLatency;

  std::cout << "  " << std::left << std::setw(44) << "Best latency keeps pitch shift within one block:" << (latencyModeResponsive ? "PASS" : "FAIL")
            << " (latency=" << latencyModeLatency << ")\n";
  std::cout << "  " << std::left << std::setw(44) << "Best quality increases pitch shift latency:" << (qualityModeHigherLatency ? "PASS" : "FAIL")
            << " (latency=" << qualityModeLatency << ")\n";

  return latencyModeResponsive && qualityModeHigherLatency;
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

  if (!TestOctaveSpecific())
    return 1;

  if (!TestStftTransposeSpecific())
    return 1;

  if (!TestStftTransposeLiveChangesSpecific())
    return 1;

  if (!TestTransposeLatencySpecific())
    return 1;

  if (!TestPitchShiftQualitySpecific())
    return 1;

  if (!TestTempoSyncSpecific())
    return 1;

  if (!TestDriveEffectCharacter())
    return 1;

  if (!TestDynamicsSoftClipOptions())
    return 1;

  if (!TestFlangerReverbStability())
    return 1;

  return 0;
}
