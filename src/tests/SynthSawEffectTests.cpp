/**
 * @file SynthSawEffectTests.cpp
 * @brief Tests for the SynthSawEffect pitch detection and synthesis
 *
 * This test validates that the SynthSawEffect can:
 * 1. Accurately detect pitch from sine wave input
 * 2. Track pitch changes quickly
 * 3. Handle edge cases (silence, noise, out-of-range frequencies)
 * 4. Generate sawtooth output at the correct frequency
 */

#include <cmath>
#include <iostream>
#include <vector>
#include <string>
#include <iomanip>
#include <chrono>

#include "dsp/EffectRegistry.h"
#include "dsp/EffectProcessor.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/SynthSawEffect.h"

namespace
{

constexpr double kTestSampleRate = 48000.0;
constexpr int kTestBlockSize = 512;
constexpr double kPi = 3.14159265358979323846;

// Test frequencies covering guitar range (low E to high E)
constexpr double kTestFrequencies[] = {
  82.41,   // E2 (low E string)
  110.0,   // A2
  146.83,  // D3
  196.0,   // G3
  246.94,  // B3
  329.63,  // E4 (high E string open)
  440.0,   // A4 (concert A)
  659.26,  // E5 (12th fret high E)
  880.0,   // A5
  1318.5   // E6 (24th fret high E)
};

struct TestResult
{
  bool passed = false;
  std::string message;
  double detectedFreq = 0.0;
  double expectedFreq = 0.0;
  double errorCents = 0.0;
  int samplesUntilLock = 0;
};

// Generate a sine wave at a specific frequency
void GenerateSineWave(std::vector<float>& buffer, double frequency, 
                      double amplitude = 0.5, double startPhase = 0.0)
{
  for (size_t i = 0; i < buffer.size(); ++i)
  {
    double phase = startPhase + 2.0 * kPi * frequency * static_cast<double>(i) / kTestSampleRate;
    buffer[i] = static_cast<float>(amplitude * std::sin(phase));
  }
}

// Generate silence
void GenerateSilence(std::vector<float>& buffer)
{
  std::fill(buffer.begin(), buffer.end(), 0.0f);
}

// Generate white noise
void GenerateNoise(std::vector<float>& buffer, double amplitude = 0.1)
{
  for (size_t i = 0; i < buffer.size(); ++i)
  {
    // Simple pseudo-random noise
    buffer[i] = static_cast<float>(amplitude * (2.0 * (rand() / static_cast<double>(RAND_MAX)) - 1.0));
  }
}

// Calculate error in cents between two frequencies
double FrequencyErrorCents(double detected, double expected)
{
  if (expected <= 0.0 || detected <= 0.0)
    return 9999.0;
  return 1200.0 * std::log2(detected / expected);
}

// Analyze output signal for fundamental frequency using zero-crossing
double EstimateOutputFrequency(const std::vector<float>& buffer, double sampleRate)
{
  int crossings = 0;
  for (size_t i = 1; i < buffer.size(); ++i)
  {
    if ((buffer[i - 1] < 0.0f && buffer[i] >= 0.0f) ||
        (buffer[i - 1] >= 0.0f && buffer[i] < 0.0f))
    {
      crossings++;
    }
  }
  
  // Frequency = crossings / 2 / duration
  double duration = static_cast<double>(buffer.size()) / sampleRate;
  return crossings / 2.0 / duration;
}

// Check if signal has valid audio (not silent, no NaN/Inf)
bool IsValidAudio(const std::vector<float>& buffer)
{
  double sumSquares = 0.0;
  for (const auto& sample : buffer)
  {
    if (std::isnan(sample) || std::isinf(sample))
      return false;
    sumSquares += sample * sample;
  }
  double rms = std::sqrt(sumSquares / buffer.size());
  return rms > 1e-6; // Not silent
}

/**
 * Test pitch detection accuracy for a single frequency
 */
TestResult TestPitchDetection(guitarfx::SynthSawEffect& effect, double targetFreq)
{
  TestResult result;
  result.expectedFreq = targetFreq;

  // Process enough audio to allow pitch detection to stabilize
  // At 48kHz with 512-sample blocks, we need multiple blocks
  constexpr int kWarmupBlocks = 20;  // ~213ms warmup
  constexpr int kTestBlocks = 10;    // ~107ms test

  std::vector<float> inputL(kTestBlockSize);
  std::vector<float> inputR(kTestBlockSize);
  std::vector<float> outputL(kTestBlockSize);
  std::vector<float> outputR(kTestBlockSize);

  float* inputs[2] = {inputL.data(), inputR.data()};
  float* outputs[2] = {outputL.data(), outputR.data()};

  effect.Reset();

  double phase = 0.0;
  double phaseInc = 2.0 * kPi * targetFreq / kTestSampleRate;
  int samplesProcessed = 0;
  int lockSample = -1;

  // Process warmup + test blocks
  for (int block = 0; block < kWarmupBlocks + kTestBlocks; ++block)
  {
    // Generate continuous sine wave
    for (int i = 0; i < kTestBlockSize; ++i)
    {
      inputL[i] = static_cast<float>(0.5 * std::sin(phase));
      inputR[i] = inputL[i];
      phase += phaseInc;
      if (phase >= 2.0 * kPi)
        phase -= 2.0 * kPi;
    }

    effect.Process(inputs, outputs, kTestBlockSize);
    samplesProcessed += kTestBlockSize;

    // Check if pitch locked during warmup
    if (lockSample < 0 && effect.GetPitchConfidence() > 0.7f)
    {
      double error = std::abs(FrequencyErrorCents(effect.GetDetectedFrequency(), targetFreq));
      if (error < 50.0) // Within 50 cents = locked
      {
        lockSample = samplesProcessed;
      }
    }
  }

  result.detectedFreq = effect.GetDetectedFrequency();
  result.errorCents = FrequencyErrorCents(result.detectedFreq, targetFreq);
  result.samplesUntilLock = lockSample > 0 ? lockSample : -1;

  // Pass criteria: within 15 cents of target frequency
  if (std::abs(result.errorCents) < 15.0)
  {
    result.passed = true;
    result.message = "PASS";
  }
  else
  {
    result.passed = false;
    result.message = "FAIL: Error " + std::to_string(result.errorCents) + " cents";
  }

  return result;
}

/**
 * Test pitch tracking speed (note change)
 */
TestResult TestPitchTrackingSpeed(guitarfx::SynthSawEffect& effect)
{
  TestResult result;
  
  constexpr double kStartFreq = 220.0;  // A3
  constexpr double kEndFreq = 440.0;    // A4 (octave up)
  constexpr int kSteadyBlocks = 15;     // Blocks at each pitch
  
  std::vector<float> inputL(kTestBlockSize);
  std::vector<float> inputR(kTestBlockSize);
  std::vector<float> outputL(kTestBlockSize);
  std::vector<float> outputR(kTestBlockSize);

  float* inputs[2] = {inputL.data(), inputR.data()};
  float* outputs[2] = {outputL.data(), outputR.data()};

  effect.Reset();

  double phase = 0.0;
  double currentFreq = kStartFreq;
  double phaseInc = 2.0 * kPi * currentFreq / kTestSampleRate;

  // Establish first pitch
  for (int block = 0; block < kSteadyBlocks; ++block)
  {
    for (int i = 0; i < kTestBlockSize; ++i)
    {
      inputL[i] = static_cast<float>(0.5 * std::sin(phase));
      inputR[i] = inputL[i];
      phase += phaseInc;
    }
    effect.Process(inputs, outputs, kTestBlockSize);
  }

  // Switch to second pitch and measure tracking time
  currentFreq = kEndFreq;
  phaseInc = 2.0 * kPi * currentFreq / kTestSampleRate;
  phase = 0.0; // Reset phase for new note

  int samplesUntilTracked = -1;
  int sampleCount = 0;

  // Debug: track detected frequency each block
  std::cout << "    Debug: ";

  for (int block = 0; block < kSteadyBlocks; ++block)
  {
    for (int i = 0; i < kTestBlockSize; ++i)
    {
      inputL[i] = static_cast<float>(0.5 * std::sin(phase));
      inputR[i] = inputL[i];
      phase += phaseInc;
    }
    effect.Process(inputs, outputs, kTestBlockSize);
    sampleCount += kTestBlockSize;

    // Debug output
    if (block < 5)
    {
      std::cout << std::fixed << std::setprecision(0) << effect.GetDetectedFrequency() << " ";
    }

    if (samplesUntilTracked < 0)
    {
      double error = std::abs(FrequencyErrorCents(effect.GetDetectedFrequency(), kEndFreq));
      if (error < 30.0) // Within 30 cents
      {
        samplesUntilTracked = sampleCount;
      }
    }
  }
  std::cout << "Hz\n";

  result.samplesUntilLock = samplesUntilTracked;
  result.detectedFreq = effect.GetDetectedFrequency();
  result.expectedFreq = kEndFreq;
  result.errorCents = FrequencyErrorCents(result.detectedFreq, kEndFreq);

  // Pass criteria: track new pitch within 100ms (~4800 samples at 48kHz)
  if (samplesUntilTracked > 0 && samplesUntilTracked < 4800)
  {
    result.passed = true;
    double trackingMs = 1000.0 * samplesUntilTracked / kTestSampleRate;
    result.message = "PASS: Tracked in " + std::to_string(trackingMs) + " ms";
  }
  else if (samplesUntilTracked < 0)
  {
    result.passed = false;
    result.message = "FAIL: Did not track new pitch";
  }
  else
  {
    result.passed = false;
    double trackingMs = 1000.0 * samplesUntilTracked / kTestSampleRate;
    result.message = "FAIL: Tracking too slow (" + std::to_string(trackingMs) + " ms)";
  }

  return result;
}

/**
 * Test silence handling (should not produce output)
 */
TestResult TestSilenceHandling(guitarfx::SynthSawEffect& effect)
{
  TestResult result;

  std::vector<float> inputL(kTestBlockSize, 0.0f);
  std::vector<float> inputR(kTestBlockSize, 0.0f);
  std::vector<float> outputL(kTestBlockSize);
  std::vector<float> outputR(kTestBlockSize);

  float* inputs[2] = {inputL.data(), inputR.data()};
  float* outputs[2] = {outputL.data(), outputR.data()};

  effect.Reset();

  // Process several blocks of silence
  for (int block = 0; block < 10; ++block)
  {
    effect.Process(inputs, outputs, kTestBlockSize);
  }

  // Check output is silent (or near-silent)
  double sumSquares = 0.0;
  for (const auto& sample : outputL)
  {
    if (std::isnan(sample) || std::isinf(sample))
    {
      result.passed = false;
      result.message = "FAIL: NaN or Inf in output";
      return result;
    }
    sumSquares += sample * sample;
  }
  
  double rms = std::sqrt(sumSquares / outputL.size());

  if (rms < 0.001) // Should be nearly silent
  {
    result.passed = true;
    result.message = "PASS: Output silent on silent input";
  }
  else
  {
    result.passed = false;
    result.message = "FAIL: Output not silent (RMS=" + std::to_string(rms) + ")";
  }

  return result;
}

/**
 * Test noise handling (should not produce pitched output)
 */
TestResult TestNoiseHandling(guitarfx::SynthSawEffect& effect)
{
  TestResult result;

  std::vector<float> inputL(kTestBlockSize);
  std::vector<float> inputR(kTestBlockSize);
  std::vector<float> outputL(kTestBlockSize);
  std::vector<float> outputR(kTestBlockSize);

  float* inputs[2] = {inputL.data(), inputR.data()};
  float* outputs[2] = {outputL.data(), outputR.data()};

  effect.Reset();
  srand(12345); // Deterministic noise

  // Process noise
  for (int block = 0; block < 10; ++block)
  {
    GenerateNoise(inputL, 0.3);
    inputR = inputL;
    effect.Process(inputs, outputs, kTestBlockSize);
  }

  // Check confidence is low
  if (effect.GetPitchConfidence() < 0.5f)
  {
    result.passed = true;
    result.message = "PASS: Low confidence on noise input";
  }
  else
  {
    result.passed = false;
    result.message = "FAIL: High confidence on noise (conf=" + 
                     std::to_string(effect.GetPitchConfidence()) + ")";
  }

  return result;
}

/**
 * Test output frequency matches detected pitch
 */
TestResult TestOutputFrequency(guitarfx::SynthSawEffect& effect)
{
  TestResult result;
  constexpr double kTargetFreq = 440.0;

  std::vector<float> inputL(kTestBlockSize);
  std::vector<float> inputR(kTestBlockSize);
  std::vector<float> outputL(kTestBlockSize);
  std::vector<float> outputR(kTestBlockSize);

  float* inputs[2] = {inputL.data(), inputR.data()};
  float* outputs[2] = {outputL.data(), outputR.data()};

  effect.Reset();
  effect.SetParam("mix", 1.0); // Full wet

  double phase = 0.0;
  double phaseInc = 2.0 * kPi * kTargetFreq / kTestSampleRate;

  // Warmup to lock pitch
  for (int block = 0; block < 30; ++block)
  {
    for (int i = 0; i < kTestBlockSize; ++i)
    {
      inputL[i] = static_cast<float>(0.5 * std::sin(phase));
      inputR[i] = inputL[i];
      phase += phaseInc;
    }
    effect.Process(inputs, outputs, kTestBlockSize);
  }

  // Analyze output frequency
  double outputFreq = EstimateOutputFrequency(outputL, kTestSampleRate);
  double detectedFreq = effect.GetDetectedFrequency();

  result.detectedFreq = detectedFreq;
  result.expectedFreq = kTargetFreq;

  // Output frequency should match detected frequency (within 5%)
  double freqError = std::abs(outputFreq - detectedFreq) / detectedFreq;
  
  if (freqError < 0.05 && std::abs(FrequencyErrorCents(detectedFreq, kTargetFreq)) < 20.0)
  {
    result.passed = true;
    result.message = "PASS: Output freq matches detected freq";
  }
  else
  {
    result.passed = false;
    result.message = "FAIL: Output freq mismatch (output=" + std::to_string(outputFreq) + 
                     ", detected=" + std::to_string(detectedFreq) + ")";
  }

  return result;
}

/**
 * Test octave shift parameter
 */
TestResult TestOctaveShift(guitarfx::SynthSawEffect& effect)
{
  TestResult result;
  constexpr double kTargetFreq = 220.0; // A3

  std::vector<float> inputL(kTestBlockSize);
  std::vector<float> inputR(kTestBlockSize);
  std::vector<float> outputL(kTestBlockSize);
  std::vector<float> outputR(kTestBlockSize);

  float* inputs[2] = {inputL.data(), inputR.data()};
  float* outputs[2] = {outputL.data(), outputR.data()};

  effect.Reset();
  effect.SetParam("mix", 1.0);
  effect.SetParam("octaveShift", 1.0); // Shift up one octave

  double phase = 0.0;
  double phaseInc = 2.0 * kPi * kTargetFreq / kTestSampleRate;

  // Process to lock pitch
  for (int block = 0; block < 30; ++block)
  {
    for (int i = 0; i < kTestBlockSize; ++i)
    {
      inputL[i] = static_cast<float>(0.5 * std::sin(phase));
      inputR[i] = inputL[i];
      phase += phaseInc;
    }
    effect.Process(inputs, outputs, kTestBlockSize);
  }

  // Output should be at 440 Hz (one octave up from 220)
  double outputFreq = EstimateOutputFrequency(outputL, kTestSampleRate);
  double expectedOutputFreq = kTargetFreq * 2.0; // 440 Hz

  double freqError = std::abs(outputFreq - expectedOutputFreq) / expectedOutputFreq;

  if (freqError < 0.10) // Within 10%
  {
    result.passed = true;
    result.message = "PASS: Octave shift working (output=" + std::to_string(outputFreq) + " Hz)";
  }
  else
  {
    result.passed = false;
    result.message = "FAIL: Octave shift not working (output=" + std::to_string(outputFreq) + 
                     " Hz, expected=" + std::to_string(expectedOutputFreq) + " Hz)";
  }

  return result;
}

} // anonymous namespace

int main()
{
  std::cout << "========================================\n";
  std::cout << "SynthSaw Effect Tests\n";
  std::cout << "========================================\n\n";

  // Register all effects
  guitarfx::RegisterAllEffects();

  // Create effect instance
  auto effect = std::make_unique<guitarfx::SynthSawEffect>();
  effect->Prepare(kTestSampleRate, kTestBlockSize);

  int passed = 0;
  int failed = 0;

  // Test 1: Pitch detection accuracy across guitar range
  std::cout << "--- Pitch Detection Accuracy Tests ---\n";
  for (double freq : kTestFrequencies)
  {
    effect->Reset();
    auto result = TestPitchDetection(*effect, freq);
    
    std::cout << std::fixed << std::setprecision(1);
    std::cout << "  " << std::setw(7) << freq << " Hz: ";
    std::cout << result.message;
    if (result.passed)
    {
      std::cout << " (detected=" << result.detectedFreq << " Hz, error=" 
                << std::setprecision(1) << result.errorCents << " cents)";
      passed++;
    }
    else
    {
      std::cout << " (detected=" << result.detectedFreq << " Hz)";
      failed++;
    }
    std::cout << "\n";
  }
  std::cout << "\n";

  // Test 2: Pitch tracking speed
  std::cout << "--- Pitch Tracking Speed Test ---\n";
  effect->Reset();
  {
    auto result = TestPitchTrackingSpeed(*effect);
    std::cout << "  Octave jump (220->440 Hz): " << result.message << "\n";
    if (result.passed) passed++; else failed++;
  }
  std::cout << "\n";

  // Test 3: Silence handling
  std::cout << "--- Edge Case Tests ---\n";
  effect->Reset();
  {
    auto result = TestSilenceHandling(*effect);
    std::cout << "  Silence input: " << result.message << "\n";
    if (result.passed) passed++; else failed++;
  }

  // Test 4: Noise handling
  effect->Reset();
  {
    auto result = TestNoiseHandling(*effect);
    std::cout << "  Noise input: " << result.message << "\n";
    if (result.passed) passed++; else failed++;
  }
  std::cout << "\n";

  // Test 5: Output frequency verification
  std::cout << "--- Output Verification Tests ---\n";
  effect->Reset();
  {
    auto result = TestOutputFrequency(*effect);
    std::cout << "  Output frequency match: " << result.message << "\n";
    if (result.passed) passed++; else failed++;
  }

  // Test 6: Octave shift
  effect->Reset();
  {
    auto result = TestOctaveShift(*effect);
    std::cout << "  Octave shift: " << result.message << "\n";
    if (result.passed) passed++; else failed++;
  }
  std::cout << "\n";

  // Summary
  std::cout << "========================================\n";
  std::cout << "Results: " << passed << " passed, " << failed << " failed\n";
  std::cout << "========================================\n";

  return failed > 0 ? 1 : 0;
}
