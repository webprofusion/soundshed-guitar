/**
 * NAM Model Diagnostic Tests
 * 
 * This test file provides detailed diagnostics for verifying that NAM model
 * processing is working correctly. It tests:
 * 
 * 1. Model loading and initialization
 * 2. Direct model processing (bypassing SignalGraphExecutor)
 * 3. SignalGraphExecutor processing pipeline
 * 4. Signal integrity through each stage
 * 
 * Run with various test signals to diagnose garbled output issues.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dsp/SignalGraphExecutor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

// Force factory registration
#include "NAM/wavenet.h"
#include "NAM/lstm.h"
#include "NAM/convnet.h"

namespace
{
[[maybe_unused]] volatile auto force_wavenet = &nam::wavenet::Factory;
[[maybe_unused]] volatile auto force_lstm = &nam::lstm::Factory;
[[maybe_unused]] volatile auto force_convnet = &nam::convnet::Factory;
} // namespace

namespace fs = std::filesystem;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kTestSampleRate = 48000.0;
constexpr int kTestBlockSize = 512;

// ============================================================================
// Test Signal Generators
// ============================================================================

// Template version for all buffer types
template<typename T>
void GenerateSineWaveT(std::vector<T>& buffer, double frequency, double sampleRate, double amplitude)
{
  for (std::size_t i = 0; i < buffer.size(); ++i)
  {
    buffer[i] = static_cast<T>(amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / sampleRate));
  }
}

// Overloads for specific types to avoid ambiguity
// Note: NAM_SAMPLE is double, so we only need NAM_SAMPLE and float overloads
void GenerateSineWave(std::vector<NAM_SAMPLE>& buffer, double frequency, double sampleRate, double amplitude = 0.5)
{
  GenerateSineWaveT(buffer, frequency, sampleRate, amplitude);
}

void GenerateSineWave(std::vector<float>& buffer, double frequency, double sampleRate, double amplitude = 0.5)
{
  GenerateSineWaveT(buffer, frequency, sampleRate, amplitude);
}

void GenerateImpulse(std::vector<NAM_SAMPLE>& buffer, double amplitude = 0.8)
{
  std::fill(buffer.begin(), buffer.end(), static_cast<NAM_SAMPLE>(0.0));
  if (!buffer.empty())
  {
    buffer[0] = static_cast<NAM_SAMPLE>(amplitude);
  }
}

void GenerateDCSignal(std::vector<NAM_SAMPLE>& buffer, double dcLevel = 0.1)
{
  std::fill(buffer.begin(), buffer.end(), static_cast<NAM_SAMPLE>(dcLevel));
}

void GenerateRamp(std::vector<NAM_SAMPLE>& buffer, double startVal = -0.5, double endVal = 0.5)
{
  if (buffer.size() < 2) return;
  const double step = (endVal - startVal) / static_cast<double>(buffer.size() - 1);
  for (std::size_t i = 0; i < buffer.size(); ++i)
  {
    buffer[i] = static_cast<NAM_SAMPLE>(startVal + step * static_cast<double>(i));
  }
}

// ============================================================================
// Signal Analysis
// ============================================================================

struct SignalStats
{
  double min = 0.0;
  double max = 0.0;
  double mean = 0.0;
  double rms = 0.0;
  double peak = 0.0;
  bool hasNaN = false;
  bool hasInf = false;
  bool isAllZeros = true;
  bool isConstant = true;
  int numNaN = 0;
  int numInf = 0;
  int numZeros = 0;
};

template<typename T>
SignalStats AnalyzeBuffer(const std::vector<T>& buffer)
{
  SignalStats stats;
  if (buffer.empty()) return stats;

  stats.min = static_cast<double>(buffer[0]);
  stats.max = static_cast<double>(buffer[0]);
  
  double sum = 0.0;
  double sumSquares = 0.0;
  const double firstVal = static_cast<double>(buffer[0]);

  for (const auto& sample : buffer)
  {
    const double val = static_cast<double>(sample);
    
    if (std::isnan(val))
    {
      stats.hasNaN = true;
      stats.numNaN++;
      continue;
    }
    if (std::isinf(val))
    {
      stats.hasInf = true;
      stats.numInf++;
      continue;
    }

    if (val != 0.0) stats.isAllZeros = false;
    if (val != firstVal) stats.isConstant = false;
    if (std::abs(val) < 1e-10) stats.numZeros++;

    stats.min = std::min(stats.min, val);
    stats.max = std::max(stats.max, val);
    sum += val;
    sumSquares += val * val;
  }

  stats.mean = sum / static_cast<double>(buffer.size());
  stats.rms = std::sqrt(sumSquares / static_cast<double>(buffer.size()));
  stats.peak = std::max(std::abs(stats.min), std::abs(stats.max));

  return stats;
}

void PrintSignalStats(const std::string& label, const SignalStats& stats)
{
  std::cout << "  " << label << ":\n";
  std::cout << "    Range: [" << std::fixed << std::setprecision(6) << stats.min 
            << ", " << stats.max << "]\n";
  std::cout << "    Mean: " << stats.mean << ", RMS: " << stats.rms 
            << ", Peak: " << stats.peak << "\n";
  if (stats.hasNaN) std::cout << "    WARNING: Contains " << stats.numNaN << " NaN values!\n";
  if (stats.hasInf) std::cout << "    WARNING: Contains " << stats.numInf << " Inf values!\n";
  if (stats.isAllZeros) std::cout << "    WARNING: All zeros!\n";
  if (stats.isConstant && !stats.isAllZeros) std::cout << "    WARNING: Constant value (DC)!\n";
}

// Print first N samples for inspection
template<typename T>
void PrintSamples(const std::string& label, const std::vector<T>& buffer, int count = 20)
{
  std::cout << "  " << label << " (first " << count << " samples): ";
  const int n = std::min(count, static_cast<int>(buffer.size()));
  for (int i = 0; i < n; ++i)
  {
    std::cout << std::fixed << std::setprecision(4) << buffer[i];
    if (i < n - 1) std::cout << ", ";
  }
  std::cout << "\n";
}

// ============================================================================
// Graph helper
// ============================================================================

// Effect types to test
constexpr const char* kAmpTypeStandard = "amp_nam";
constexpr const char* kAmpTypeOptimized = "amp_nam_optimized";

guitarfx::Preset MakeNamGraphPreset(const fs::path& modelPath, const fs::path& irPath, const char* ampType = kAmpTypeStandard)
{
  guitarfx::Preset preset;
  preset.id = "nam-diagnostic";
  preset.name = "nam-diagnostic";
  preset.version = 2;

  guitarfx::GraphNode input;
  input.id = "input";
  input.type = guitarfx::kNodeTypeInput;
  input.category = "routing";

  guitarfx::GraphNode amp;
  amp.id = "amp";
  amp.type = ampType;
  amp.category = "amp";
  amp.enabled = fs::exists(modelPath);
  if (amp.enabled)
  {
    guitarfx::ResourceRef ref;
    ref.resourceType = "nam";
    ref.filePath = modelPath;
    amp.resources.push_back(ref);
  }

  guitarfx::GraphNode cab;
  cab.id = "cab";
  cab.type = "cab_ir";
  cab.category = "cab";
  cab.enabled = fs::exists(irPath);
  cab.params["mix"] = 1.0;
  cab.params["outputGain"] = 0.0;
  if (cab.enabled)
  {
    guitarfx::ResourceRef ref;
    ref.resourceType = "ir";
    ref.filePath = irPath;
    cab.resources.push_back(ref);
  }

  guitarfx::GraphNode output;
  output.id = "output";
  output.type = guitarfx::kNodeTypeOutput;
  output.category = "routing";

  preset.graph.nodes = {input, amp, cab, output};
  preset.graph.edges = {
    {input.id, amp.id, 0, 0, 1.0},
    {amp.id, cab.id, 0, 0, 1.0},
    {cab.id, output.id, 0, 0, 1.0}
  };

  return preset;
}

class GraphHarness
{
public:
  GraphHarness(const fs::path& resourcesDir,
               const fs::path& modelPath,
               const fs::path& irPath,
               double sampleRate,
               int blockSize,
               const char* ampType = kAmpTypeStandard)
  {
    guitarfx::RegisterAllEffects();
    mResourceLibrary = std::make_unique<guitarfx::ResourceLibrary>();
    mResourceLibrary->LoadFromDirectory(resourcesDir);
    mExecutor = std::make_unique<guitarfx::SignalGraphExecutor>();
    mExecutor->SetResourceLibrary(mResourceLibrary.get());
    mPreset = MakeNamGraphPreset(modelPath, irPath, ampType);
    mExecutor->SetInputTrim(mPreset.global.inputTrim);
    mExecutor->SetOutputTrim(mPreset.global.outputTrim);
    mExecutor->SetGraph(mPreset.graph);
    mExecutor->Prepare(sampleRate, blockSize);
  }

  void Process(float** inputs, float** outputs, int numSamples)
  {
    mExecutor->Process(inputs, outputs, numSamples);
  }

  void SetInputTrim(double db) { mExecutor->SetInputTrim(db); }
  void SetOutputTrim(double db) { mExecutor->SetOutputTrim(db); }
  void SetDrive(double value) { mExecutor->SetNodeParam("amp", "drive", value); }
  void SetTone(double value) { mExecutor->SetNodeParam("amp", "tone", value); }
  void SetMix(double value) { mExecutor->SetNodeParam("cab", "mix", value); }

private:
  guitarfx::Preset mPreset;
  std::unique_ptr<guitarfx::ResourceLibrary> mResourceLibrary;
  std::unique_ptr<guitarfx::SignalGraphExecutor> mExecutor;
};

// ============================================================================
// Test: Direct NAM Model Processing
// ============================================================================

bool TestDirectModelProcessing(const fs::path& modelPath)
{
  std::cout << "\n=== Test: Direct NAM Model Processing ===\n";
  std::cout << "Model: " << modelPath.filename().string() << "\n\n";

  // Load model directly using NAM library
  std::unique_ptr<nam::DSP> model;
  try
  {
    model = nam::get_dsp(modelPath);
    if (!model)
    {
      std::cerr << "ERROR: Failed to load model (returned nullptr)\n";
      return false;
    }
    std::cout << "Model loaded successfully\n";
  }
  catch (const std::exception& ex)
  {
    std::cerr << "ERROR: Exception loading model: " << ex.what() << "\n";
    return false;
  }

  // Initialize model
  try
  {
    model->ResetAndPrewarm(kTestSampleRate, kTestBlockSize);
    std::cout << "Model initialized at " << kTestSampleRate << " Hz, block size " << kTestBlockSize << "\n";
  }
  catch (const std::exception& ex)
  {
    std::cerr << "ERROR: Exception initializing model: " << ex.what() << "\n";
    return false;
  }

  // Test with various signals
  std::vector<NAM_SAMPLE> input(kTestBlockSize);
  std::vector<NAM_SAMPLE> output(kTestBlockSize);

  // Test 1: Sine wave
  std::cout << "\n--- Test 1: 440 Hz Sine Wave (amplitude 0.3) ---\n";
  GenerateSineWave(input, 440.0, kTestSampleRate, 0.3);
  PrintSignalStats("Input", AnalyzeBuffer(input));
  PrintSamples("Input", input);

  NAM_SAMPLE* inputPtrs[1] = {input.data()};
  NAM_SAMPLE* outputPtrs[1] = {output.data()};

  try
  {
    model->process(inputPtrs, outputPtrs, kTestBlockSize);
  }
  catch (const std::exception& ex)
  {
    std::cerr << "ERROR: Exception during processing: " << ex.what() << "\n";
    return false;
  }

  PrintSignalStats("Output", AnalyzeBuffer(output));
  PrintSamples("Output", output);

  // Test 2: Impulse
  std::cout << "\n--- Test 2: Impulse (amplitude 0.5) ---\n";
  GenerateImpulse(input, 0.5);
  PrintSignalStats("Input", AnalyzeBuffer(input));
  PrintSamples("Input", input);

  model->process(inputPtrs, outputPtrs, kTestBlockSize);
  PrintSignalStats("Output", AnalyzeBuffer(output));
  PrintSamples("Output", output);

  // Test 3: Silence (should produce near-silence)
  std::cout << "\n--- Test 3: Silence ---\n";
  std::fill(input.begin(), input.end(), static_cast<NAM_SAMPLE>(0.0));
  PrintSignalStats("Input", AnalyzeBuffer(input));

  model->process(inputPtrs, outputPtrs, kTestBlockSize);
  PrintSignalStats("Output", AnalyzeBuffer(output));
  PrintSamples("Output", output, 10);

  // Test 4: Low frequency sine (bass note E2 ~82Hz)
  std::cout << "\n--- Test 4: 82 Hz Sine Wave (Low E string) ---\n";
  GenerateSineWave(input, 82.0, kTestSampleRate, 0.4);
  PrintSignalStats("Input", AnalyzeBuffer(input));

  model->process(inputPtrs, outputPtrs, kTestBlockSize);
  PrintSignalStats("Output", AnalyzeBuffer(output));

  // Test 5: Multiple consecutive blocks (check for state issues)
  std::cout << "\n--- Test 5: Multiple Blocks (10 blocks, check stability) ---\n";
  GenerateSineWave(input, 440.0, kTestSampleRate, 0.3);
  
  bool allBlocksOk = true;
  for (int block = 0; block < 10; ++block)
  {
    model->process(inputPtrs, outputPtrs, kTestBlockSize);
    auto stats = AnalyzeBuffer(output);
    
    if (stats.hasNaN || stats.hasInf)
    {
      std::cerr << "Block " << block << ": FAILED - NaN/Inf detected\n";
      allBlocksOk = false;
      break;
    }
    
    if (stats.isAllZeros)
    {
      std::cerr << "Block " << block << ": FAILED - Output is all zeros\n";
      allBlocksOk = false;
      break;
    }

    if (stats.peak > 5.0)
    {
      std::cerr << "Block " << block << ": WARNING - High peak: " << stats.peak << "\n";
    }
  }
  
  if (allBlocksOk)
  {
    std::cout << "All 10 blocks processed without errors\n";
  }

  return true;
}

// ============================================================================
// Test: SignalGraphExecutor Processing Pipeline
// ============================================================================

bool TestDSPManagerPipeline(const fs::path& resourcesDir, const fs::path& modelPath, const fs::path& irPath, const char* ampType)
{
  std::cout << "\n=== Test: SignalGraphExecutor Processing Pipeline (" << ampType << ") ===\n";
  std::cout << "Model: " << modelPath.filename().string() << "\n";
  std::cout << "IR: " << (irPath.empty() ? "(none)" : irPath.filename().string()) << "\n\n";

  GraphHarness dsp(resourcesDir, modelPath, irPath, kTestSampleRate, kTestBlockSize, ampType);
  std::cout << "Graph DSP prepared at " << kTestSampleRate << " Hz\n";

  // Set neutral DSP parameters
  dsp.SetInputTrim(0.0);
  dsp.SetOutputTrim(0.0);
  dsp.SetDrive(0.5);
  dsp.SetTone(0.0);
  dsp.SetMix(1.0);
  std::cout << "DSP parameters set to neutral values\n";

  // Create stereo buffers
  std::vector<float> inputL(kTestBlockSize);
  std::vector<float> inputR(kTestBlockSize);
  std::vector<float> outputL(kTestBlockSize);
  std::vector<float> outputR(kTestBlockSize);

  float* inputs[2] = {inputL.data(), inputR.data()};
  float* outputs[2] = {outputL.data(), outputR.data()};

  // Test 1: Sine wave
  std::cout << "\n--- Test 1: 440 Hz Sine Wave through DSP Manager ---\n";
  GenerateSineWave(inputL, 440.0, kTestSampleRate, 0.3);
  GenerateSineWave(inputR, 440.0, kTestSampleRate, 0.3);
  
  PrintSignalStats("Input L", AnalyzeBuffer(inputL));
  PrintSamples("Input L", inputL);

  dsp.Process(inputs, outputs, kTestBlockSize);

  PrintSignalStats("Output L", AnalyzeBuffer(outputL));
  PrintSignalStats("Output R", AnalyzeBuffer(outputR));
  PrintSamples("Output L", outputL);

  // Test 2: Check if output differs from input
  std::cout << "\n--- Test 2: Verify Processing Changed Signal ---\n";
  bool signalChanged = false;
  for (int i = 0; i < kTestBlockSize; ++i)
  {
    if (std::abs(outputL[i] - inputL[i]) > 1e-6)
    {
      signalChanged = true;
      break;
    }
  }
  std::cout << "Signal changed by processing: " << (signalChanged ? "YES" : "NO (possible passthrough!)") << "\n";

  // Test 3: Stability over multiple blocks
  std::cout << "\n--- Test 3: 20 Block Stability Test ---\n";
  bool stable = true;
  for (int block = 0; block < 20; ++block)
  {
    // Regenerate input each block with slightly different phase
    for (std::size_t i = 0; i < inputL.size(); ++i)
    {
      const double phase = 2.0 * kPi * 440.0 * (block * kTestBlockSize + i) / kTestSampleRate;
      inputL[i] = inputR[i] = static_cast<float>(0.3 * std::sin(phase));
    }

    dsp.Process(inputs, outputs, kTestBlockSize);

    auto stats = AnalyzeBuffer(outputL);
    if (stats.hasNaN || stats.hasInf)
    {
      std::cerr << "Block " << block << ": NaN/Inf detected!\n";
      stable = false;
      break;
    }
    if (stats.peak > 10.0)
    {
      std::cerr << "Block " << block << ": Excessive peak " << stats.peak << "\n";
      stable = false;
    }
  }
  std::cout << "Stability test: " << (stable ? "PASSED" : "FAILED") << "\n";

  // Test 4: Bypass comparison (set mix to 0 for dry signal)
  std::cout << "\n--- Test 4: Dry/Wet Mix Comparison ---\n";
  
  // Generate fresh input
  GenerateSineWave(inputL, 440.0, kTestSampleRate, 0.3);
  GenerateSineWave(inputR, 440.0, kTestSampleRate, 0.3);
  
  // Test with mix = 0 (fully dry)
  dsp.SetMix(0.0);
  dsp.Process(inputs, outputs, kTestBlockSize);
  auto dryStats = AnalyzeBuffer(outputL);
  std::cout << "Mix=0 (dry): Peak=" << dryStats.peak << ", RMS=" << dryStats.rms << "\n";

  // Reset input and test with mix = 1 (fully wet)
  GenerateSineWave(inputL, 440.0, kTestSampleRate, 0.3);
  GenerateSineWave(inputR, 440.0, kTestSampleRate, 0.3);
  dsp.SetMix(1.0);
  dsp.Process(inputs, outputs, kTestBlockSize);
  auto wetStats = AnalyzeBuffer(outputL);
  std::cout << "Mix=1 (wet): Peak=" << wetStats.peak << ", RMS=" << wetStats.rms << "\n";

  return true;
}

// ============================================================================
// Test: Compare Direct vs Manager Processing
// ============================================================================

bool TestDirectVsManager(const fs::path& resourcesDir, const fs::path& modelPath, const char* ampType)
{
  std::cout << "\n=== Test: Direct Model vs DSP Manager Comparison (" << ampType << ") ===\n";
  std::cout << "This test compares NAM model output when called directly vs through SignalGraphExecutor\n\n";

  // Load model directly
  auto directModel = nam::get_dsp(modelPath);
  if (!directModel)
  {
    std::cerr << "ERROR: Failed to load model for direct test\n";
    return false;
  }
  directModel->ResetAndPrewarm(kTestSampleRate, kTestBlockSize);

  // Set up graph-based DSP manager
  GraphHarness dsp(resourcesDir, modelPath, {}, kTestSampleRate, kTestBlockSize, ampType);

  // Neutral settings for DSP manager (to minimize processing changes)
  dsp.SetInputTrim(0.0);
  dsp.SetOutputTrim(0.0);
  dsp.SetDrive(0.0);  // No drive
  dsp.SetTone(0.0);
  dsp.SetMix(1.0);

  // Generate identical test signal
  std::vector<NAM_SAMPLE> directInput(kTestBlockSize);
  std::vector<NAM_SAMPLE> directOutput(kTestBlockSize);
  
  std::vector<float> managerInputL(kTestBlockSize);
  std::vector<float> managerInputR(kTestBlockSize);
  std::vector<float> managerOutputL(kTestBlockSize);
  std::vector<float> managerOutputR(kTestBlockSize);

  GenerateSineWave(directInput, 440.0, kTestSampleRate, 0.3);
  GenerateSineWave(managerInputL, 440.0, kTestSampleRate, 0.3);
  GenerateSineWave(managerInputR, 440.0, kTestSampleRate, 0.3);

  // Process directly
  NAM_SAMPLE* directInputPtrs[1] = {directInput.data()};
  NAM_SAMPLE* directOutputPtrs[1] = {directOutput.data()};
  directModel->process(directInputPtrs, directOutputPtrs, kTestBlockSize);

  // Process through manager
  float* inputs[2] = {managerInputL.data(), managerInputR.data()};
  float* outputs[2] = {managerOutputL.data(), managerOutputR.data()};
  dsp.Process(inputs, outputs, kTestBlockSize);

  // Compare results
  auto directStats = AnalyzeBuffer(directOutput);
  auto managerStats = AnalyzeBuffer(managerOutputL);

  std::cout << "Direct NAM model:\n";
  PrintSignalStats("Output", directStats);
  PrintSamples("Output", directOutput);

  std::cout << "\nDSP Manager (neutral settings):\n";
  PrintSignalStats("Output L", managerStats);
  PrintSamples("Output L", managerOutputL);

  // Calculate difference
  std::cout << "\n--- Signal Difference Analysis ---\n";
  double maxDiff = 0.0;
  double sumDiffSquares = 0.0;
  for (int i = 0; i < kTestBlockSize; ++i)
  {
    const double diff = std::abs(static_cast<double>(directOutput[i]) - static_cast<double>(managerOutputL[i]));
    maxDiff = std::max(maxDiff, diff);
    sumDiffSquares += diff * diff;
  }
  const double rmsDiff = std::sqrt(sumDiffSquares / kTestBlockSize);
  
  std::cout << "Max difference: " << std::scientific << maxDiff << "\n";
  std::cout << "RMS difference: " << rmsDiff << "\n";
  std::cout << std::fixed;

  if (rmsDiff < 0.01)
  {
    std::cout << "Signals are very similar (likely just rounding/processing order differences)\n";
  }
  else if (rmsDiff < 0.1)
  {
    std::cout << "Signals differ moderately (DSP chain is adding coloration)\n";
  }
  else
  {
    std::cout << "WARNING: Signals differ significantly - check DSP pipeline!\n";
  }

  return true;
}

// ============================================================================
// Test: Different Models Produce Different Output
// ============================================================================

bool TestModelDifferentiation(const fs::path& resourcesDir, const nlohmann::json& audioModelsJson, const char* ampType)
{
  std::cout << "\n=== Test: Different Models Produce Different Output (" << ampType << ") ===\n";

  if (!audioModelsJson.is_array() || audioModelsJson.size() < 2)
  {
    std::cerr << "ERROR: Need at least two models in audiofx-models.json\n";
    return false;
  }

  // Pick first model, and then the first different model with a distinct file path
  const auto& modelA = audioModelsJson.front();
  const auto* modelBPtr = static_cast<const nlohmann::json*>(nullptr);
  for (const auto& m : audioModelsJson)
  {
    if (m.value("filePath", std::string{}) != modelA.value("filePath", std::string{}))
    {
      modelBPtr = &m;
      break;
    }
  }

  if (!modelBPtr)
  {
    std::cerr << "ERROR: Could not find a second distinct model entry\n";
    return false;
  }

  const auto& modelB = *modelBPtr;

  const fs::path pathA = resourcesDir / modelA.value("filePath", "");
  const fs::path pathB = resourcesDir / modelB.value("filePath", "");

  if (!fs::exists(pathA) || !fs::exists(pathB))
  {
    std::cerr << "ERROR: Model files missing: " << pathA << " or " << pathB << "\n";
    return false;
  }

  auto renderModel = [&resourcesDir, ampType](const fs::path& modelPath) -> std::vector<double>
  {
    GraphHarness dsp(resourcesDir, modelPath, {}, kTestSampleRate, kTestBlockSize, ampType);

    dsp.SetInputTrim(0.0);
    dsp.SetOutputTrim(0.0);
    dsp.SetDrive(0.0);
    dsp.SetTone(0.0);
    dsp.SetMix(1.0);

    std::vector<float> inL(kTestBlockSize), inR(kTestBlockSize);
    std::vector<float> outL(kTestBlockSize), outR(kTestBlockSize);
    GenerateSineWave(inL, 440.0, kTestSampleRate, 0.3);
    GenerateSineWave(inR, 440.0, kTestSampleRate, 0.3);

    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};

    // Process a couple of blocks to let the model settle
    dsp.Process(inputs, outputs, kTestBlockSize);
    dsp.Process(inputs, outputs, kTestBlockSize);

    return std::vector<double>(outL.begin(), outL.end());
  };

  const auto outA = renderModel(pathA);
  const auto outB = renderModel(pathB);

  if (outA.empty() || outB.empty())
  {
    std::cerr << "ERROR: Failed to render one or both models\n";
    return false;
  }

  auto statsA = AnalyzeBuffer(outA);
  auto statsB = AnalyzeBuffer(outB);

  if (statsA.isAllZeros || statsB.isAllZeros)
  {
    std::cerr << "ERROR: One of the model outputs is silent\n";
    return false;
  }

  double sumSq = 0.0;
  double maxDiff = 0.0;
  for (std::size_t i = 0; i < std::min(outA.size(), outB.size()); ++i)
  {
    const double diff = std::abs(outA[i] - outB[i]);
    sumSq += diff * diff;
    maxDiff = std::max(maxDiff, diff);
  }
  const double rmsDiff = std::sqrt(sumSq / static_cast<double>(std::min(outA.size(), outB.size())));

  std::cout << "Model A: " << modelA.value("title", "(unknown)") << "\n";
  PrintSignalStats("Output", statsA);
  std::cout << "Model B: " << modelB.value("title", "(unknown)") << "\n";
  PrintSignalStats("Output", statsB);
  std::cout << "RMS difference: " << rmsDiff << ", Max difference: " << maxDiff << "\n";

  const bool different = rmsDiff > 1e-3;
  std::cout << (different ? "Models produce distinct output\n" : "WARNING: Models produced nearly identical output\n");

  return different;
}

// ============================================================================
// Test: Standard vs Optimized NAM Comparison
// ============================================================================

bool TestStandardVsOptimized(const fs::path& resourcesDir, const fs::path& modelPath)
{
  std::cout << "\n=== Test: Standard NAM vs Optimized NAM Comparison ===\n";
  std::cout << "This test verifies that amp_nam and amp_nam_optimized produce similar output\n";
  std::cout << "Model: " << modelPath.filename().string() << "\n\n";

  auto renderWithAmpType = [&resourcesDir, &modelPath](const char* ampType) -> std::vector<double>
  {
    GraphHarness dsp(resourcesDir, modelPath, {}, kTestSampleRate, kTestBlockSize, ampType);

    dsp.SetInputTrim(0.0);
    dsp.SetOutputTrim(0.0);
    dsp.SetDrive(0.0);
    dsp.SetTone(0.0);
    dsp.SetMix(1.0);

    std::vector<float> inL(kTestBlockSize), inR(kTestBlockSize);
    std::vector<float> outL(kTestBlockSize), outR(kTestBlockSize);
    GenerateSineWave(inL, 440.0, kTestSampleRate, 0.3);
    GenerateSineWave(inR, 440.0, kTestSampleRate, 0.3);

    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};

    // Process a few blocks to let model settle
    for (int i = 0; i < 5; ++i)
    {
      dsp.Process(inputs, outputs, kTestBlockSize);
    }

    return std::vector<double>(outL.begin(), outL.end());
  };

  const auto standardOutput = renderWithAmpType(kAmpTypeStandard);
  const auto optimizedOutput = renderWithAmpType(kAmpTypeOptimized);

  if (standardOutput.empty() || optimizedOutput.empty())
  {
    std::cerr << "ERROR: Failed to render one or both amp types\n";
    return false;
  }

  auto standardStats = AnalyzeBuffer(standardOutput);
  auto optimizedStats = AnalyzeBuffer(optimizedOutput);

  std::cout << "Standard NAM (amp_nam):\n";
  PrintSignalStats("Output", standardStats);
  PrintSamples("Output", standardOutput);

  std::cout << "\nOptimized NAM (amp_nam_optimized):\n";
  PrintSignalStats("Output", optimizedStats);
  PrintSamples("Output", optimizedOutput);

  // Check for errors in either output
  if (standardStats.hasNaN || standardStats.hasInf)
  {
    std::cerr << "ERROR: Standard NAM produced NaN/Inf!\n";
    return false;
  }
  if (optimizedStats.hasNaN || optimizedStats.hasInf)
  {
    std::cerr << "ERROR: Optimized NAM produced NaN/Inf!\n";
    return false;
  }
  if (standardStats.isAllZeros)
  {
    std::cerr << "ERROR: Standard NAM produced all zeros!\n";
    return false;
  }
  if (optimizedStats.isAllZeros)
  {
    std::cerr << "ERROR: Optimized NAM produced all zeros!\n";
    return false;
  }

  // Calculate difference between outputs
  double sumSq = 0.0;
  double maxDiff = 0.0;
  for (std::size_t i = 0; i < std::min(standardOutput.size(), optimizedOutput.size()); ++i)
  {
    const double diff = std::abs(standardOutput[i] - optimizedOutput[i]);
    sumSq += diff * diff;
    maxDiff = std::max(maxDiff, diff);
  }
  const double rmsDiff = std::sqrt(sumSq / static_cast<double>(std::min(standardOutput.size(), optimizedOutput.size())));

  std::cout << "\n--- Difference Analysis ---\n";
  std::cout << "Max difference: " << std::scientific << maxDiff << "\n";
  std::cout << "RMS difference: " << rmsDiff << "\n";
  std::cout << std::fixed;

  // Calculate correlation coefficient
  const double meanStd = standardStats.mean;
  const double meanOpt = optimizedStats.mean;
  double sumXY = 0.0, sumX2 = 0.0, sumY2 = 0.0;
  for (std::size_t i = 0; i < standardOutput.size(); ++i)
  {
    const double x = standardOutput[i] - meanStd;
    const double y = optimizedOutput[i] - meanOpt;
    sumXY += x * y;
    sumX2 += x * x;
    sumY2 += y * y;
  }
  const double correlation = (sumX2 > 0 && sumY2 > 0) ? sumXY / std::sqrt(sumX2 * sumY2) : 0.0;
  std::cout << "Correlation: " << std::setprecision(6) << correlation << "\n";

  // The optimized version may use different precision (float vs double) and different
  // activation implementations, so we expect some difference but correlation should be high
  constexpr double kMinCorrelation = 0.95;  // High correlation expected
  constexpr double kMaxRmsDiff = 0.5;       // Allow some difference due to implementation

  if (correlation < kMinCorrelation)
  {
    std::cerr << "WARNING: Low correlation between standard and optimized (" << correlation << ")\n";
    std::cerr << "This may indicate an issue with the optimized implementation.\n";
    return false;
  }

  if (rmsDiff > kMaxRmsDiff)
  {
    std::cerr << "WARNING: High RMS difference (" << rmsDiff << ") between standard and optimized\n";
    // This is a warning, not a failure, as some difference is expected
  }

  std::cout << "\nConclusion: Standard and Optimized NAM produce ";
  if (rmsDiff < 0.001)
  {
    std::cout << "nearly identical output (excellent match)\n";
  }
  else if (rmsDiff < 0.1)
  {
    std::cout << "similar output (good match, minor precision differences)\n";
  }
  else if (correlation > kMinCorrelation)
  {
    std::cout << "correlated output (acceptable, implementation differences expected)\n";
  }
  else
  {
    std::cout << "different output (may need investigation)\n";
  }

  return true;
}

} // namespace

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[])
{
#ifndef GUITARFX_TEST_RESOURCES_DIR
#error "GUITARFX_TEST_RESOURCES_DIR must be defined"
#endif

  try
  {
    const fs::path resourcesDir = fs::path(GUITARFX_TEST_RESOURCES_DIR);
    const fs::path dataDir = resourcesDir / "data";

    std::cout << "NAM Model Diagnostic Tests\n";
    std::cout << "==========================\n";
    std::cout << "Resources: " << resourcesDir.string() << "\n\n";

    // Load model library to find a test model
    const auto audioModelsJson = nlohmann::json::parse(
      std::ifstream(dataDir / "audiofx-models.json"));
    
    if (!audioModelsJson.is_array() || audioModelsJson.empty())
    {
      std::cerr << "ERROR: No models found in audiofx-models.json\n";
      return 1;
    }

    // Use the first model for testing
    const auto& firstModel = audioModelsJson[0];
    const fs::path modelPath = resourcesDir / firstModel.value("filePath", "");
    const std::string modelName = firstModel.value("title", "Unknown");

    std::cout << "Using test model: " << modelName << "\n";
    std::cout << "Path: " << modelPath.string() << "\n";

    if (!fs::exists(modelPath))
    {
      std::cerr << "ERROR: Model file not found!\n";
      return 1;
    }

    // Load IR library for optional IR testing
    fs::path irPath;
    try
    {
      const auto irLibraryJson = nlohmann::json::parse(
        std::ifstream(dataDir / "ir-library.json"));
      if (irLibraryJson.is_array() && !irLibraryJson.empty())
      {
        irPath = resourcesDir / irLibraryJson[0].value("filePath", "");
      }
    }
    catch (...) { /* IR is optional */ }

    // Run diagnostic tests
    bool allPassed = true;

    // Test direct NAM model processing (library level, no effect type)
    allPassed &= TestDirectModelProcessing(modelPath);

    // Test Standard NAM (amp_nam)
    std::cout << "\n\n########################################\n";
    std::cout << "# TESTING STANDARD NAM (amp_nam)       #\n";
    std::cout << "########################################\n";
    allPassed &= TestDSPManagerPipeline(resourcesDir, modelPath, irPath, kAmpTypeStandard);
    allPassed &= TestDirectVsManager(resourcesDir, modelPath, kAmpTypeStandard);
    allPassed &= TestModelDifferentiation(resourcesDir, audioModelsJson, kAmpTypeStandard);

    // Test Optimized NAM (amp_nam_optimized)
    std::cout << "\n\n########################################\n";
    std::cout << "# TESTING OPTIMIZED NAM (amp_nam_optimized) #\n";
    std::cout << "########################################\n";
    allPassed &= TestDSPManagerPipeline(resourcesDir, modelPath, irPath, kAmpTypeOptimized);
    allPassed &= TestDirectVsManager(resourcesDir, modelPath, kAmpTypeOptimized);
    allPassed &= TestModelDifferentiation(resourcesDir, audioModelsJson, kAmpTypeOptimized);

    // Compare Standard vs Optimized
    std::cout << "\n\n########################################\n";
    std::cout << "# COMPARING STANDARD vs OPTIMIZED      #\n";
    std::cout << "########################################\n";
    allPassed &= TestStandardVsOptimized(resourcesDir, modelPath);

    std::cout << "\n==========================\n";
    std::cout << "Diagnostic tests " << (allPassed ? "COMPLETED" : "HAD FAILURES") << "\n";

    return allPassed ? 0 : 1;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Fatal error: " << ex.what() << "\n";
    return 1;
  }
}
