/**
 * NAM Model Performance Benchmark
 *
 * Measures DSP performance of NAM model processing comparing:
 * - Standard NAM (amp_nam) using upstream library
 * - Optimized NAM (amp_nam_optimized) using SIMD-accelerated implementation
 *
 * Reports samples/second throughput, real-time capability factor, and speedup.
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dsp/SignalGraphExecutor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/simd/SimdMath.h"
#include "dsp/simd/Benchmark.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

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

// Effect type identifiers
constexpr const char* kAmpTypeStandard = "amp_nam";
constexpr const char* kAmpTypeOptimized = "amp_nam_optimized";

// ============================================================================
// Benchmark Configuration
// ============================================================================

struct BenchmarkConfig
{
  double sampleRate = 48000.0;
  int blockSize = 512;
  int warmupBlocks = 100;
  int measureBlocks = 1000;
};

// ============================================================================
// Benchmark Results
// ============================================================================

struct BenchmarkResult
{
  std::string name;
  std::string modelName;
  double sampleRate;
  int blockSize;
  
  double totalTimeMs;
  int totalBlocks;
  long long totalSamples;
  
  double samplesPerSecond;
  double blocksPerSecond;
  double realTimeFactor;  // > 1.0 means faster than realtime
  double averageBlockTimeUs;
  double minBlockTimeUs;
  double maxBlockTimeUs;
  double stdDevBlockTimeUs;
  
  bool meetsRealTime;
};

void PrintBenchmarkResult(const BenchmarkResult& r)
{
  std::cout << "\n--- " << r.name << " ---\n";
  std::cout << "Model: " << r.modelName << "\n";
  std::cout << "Sample Rate: " << r.sampleRate << " Hz, Block Size: " << r.blockSize << "\n";
  std::cout << "Processed: " << r.totalBlocks << " blocks (" << r.totalSamples << " samples)\n";
  std::cout << "Total Time: " << std::fixed << std::setprecision(2) << r.totalTimeMs << " ms\n";
  std::cout << "\nPerformance:\n";
  std::cout << "  Samples/second: " << std::fixed << std::setprecision(0) << r.samplesPerSecond << "\n";
  std::cout << "  Blocks/second:  " << std::fixed << std::setprecision(1) << r.blocksPerSecond << "\n";
  std::cout << "  Real-time factor: " << std::fixed << std::setprecision(2) << r.realTimeFactor << "x\n";
  std::cout << "\nBlock Timing:\n";
  std::cout << "  Average: " << std::fixed << std::setprecision(2) << r.averageBlockTimeUs << " µs\n";
  std::cout << "  Min:     " << std::fixed << std::setprecision(2) << r.minBlockTimeUs << " µs\n";
  std::cout << "  Max:     " << std::fixed << std::setprecision(2) << r.maxBlockTimeUs << " µs\n";
  std::cout << "  StdDev:  " << std::fixed << std::setprecision(2) << r.stdDevBlockTimeUs << " µs\n";
  std::cout << "\nReal-time capable: " << (r.meetsRealTime ? "YES ✓" : "NO ✗") << "\n";
}

void PrintSpeedup(const BenchmarkResult& baseline, const BenchmarkResult& optimized)
{
  double speedup = optimized.samplesPerSecond / baseline.samplesPerSecond;
  double blockSpeedup = baseline.averageBlockTimeUs / optimized.averageBlockTimeUs;
  
  std::cout << "\n========================================\n";
  std::cout << "SPEEDUP: " << baseline.name << " -> " << optimized.name << "\n";
  std::cout << "========================================\n";
  std::cout << "Throughput improvement: " << std::fixed << std::setprecision(2) << speedup << "x\n";
  std::cout << "Block time improvement: " << std::fixed << std::setprecision(2) << blockSpeedup << "x\n";
  
  double baselineHeadroom = (baseline.realTimeFactor - 1.0) * 100.0;
  double optimizedHeadroom = (optimized.realTimeFactor - 1.0) * 100.0;
  std::cout << "Baseline headroom:  " << std::fixed << std::setprecision(1) << baselineHeadroom << "%\n";
  std::cout << "Optimized headroom: " << std::fixed << std::setprecision(1) << optimizedHeadroom << "%\n";
}

// ============================================================================
// Test Signal Generation
// ============================================================================

void GenerateSineWave(std::vector<float>& buffer, double frequency, double sampleRate, double amplitude)
{
  for (std::size_t i = 0; i < buffer.size(); ++i)
  {
    buffer[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / sampleRate));
  }
}

void GenerateGuitarLikeSignal(std::vector<float>& buffer, double sampleRate)
{
  // Generate a guitar-like signal with multiple harmonics
  std::random_device rd;
  std::mt19937 gen(42); // Fixed seed for reproducibility
  std::uniform_real_distribution<float> noise(-0.01f, 0.01f);
  
  for (std::size_t i = 0; i < buffer.size(); ++i)
  {
    double t = static_cast<double>(i) / sampleRate;
    // Fundamental (low E, ~82 Hz) + harmonics
    double signal = 0.0;
    signal += 0.5 * std::sin(2.0 * kPi * 82.0 * t);
    signal += 0.3 * std::sin(2.0 * kPi * 164.0 * t);
    signal += 0.15 * std::sin(2.0 * kPi * 246.0 * t);
    signal += 0.08 * std::sin(2.0 * kPi * 328.0 * t);
    signal += noise(gen);  // Add slight noise
    buffer[i] = static_cast<float>(signal * 0.3);  // Keep amplitude reasonable
  }
}

// ============================================================================
// Direct NAM Model Benchmark
// ============================================================================

BenchmarkResult BenchmarkDirectNAM(const fs::path& modelPath, const std::string& modelName, const BenchmarkConfig& config)
{
  BenchmarkResult result;
  result.name = "Direct NAM Library";
  result.modelName = modelName;
  result.sampleRate = config.sampleRate;
  result.blockSize = config.blockSize;
  
  // Load model
  auto model = nam::get_dsp(modelPath);
  if (!model)
  {
    std::cerr << "ERROR: Failed to load model: " << modelPath << "\n";
    return result;
  }
  
  model->ResetAndPrewarm(config.sampleRate, config.blockSize);
  
  const int inputChannels = model->NumInputChannels();
  const int outputChannels = model->NumOutputChannels();
  if (inputChannels <= 0 || outputChannels <= 0)
  {
    std::cerr << "ERROR: Model has invalid channel counts (in=" << inputChannels
              << ", out=" << outputChannels << ") for: " << modelPath << "\n";
    return result;
  }

  // Allocate buffers (NAM uses NAM_SAMPLE which is double by default)
  std::vector<std::vector<NAM_SAMPLE>> inputBuffers(static_cast<size_t>(inputChannels));
  std::vector<std::vector<NAM_SAMPLE>> outputBuffers(static_cast<size_t>(outputChannels));
  for (int ch = 0; ch < inputChannels; ++ch)
    inputBuffers[static_cast<size_t>(ch)].resize(static_cast<size_t>(config.blockSize));
  for (int ch = 0; ch < outputChannels; ++ch)
    outputBuffers[static_cast<size_t>(ch)].resize(static_cast<size_t>(config.blockSize));

  std::vector<NAM_SAMPLE*> inputPtrs(static_cast<size_t>(inputChannels));
  std::vector<NAM_SAMPLE*> outputPtrs(static_cast<size_t>(outputChannels));
  for (int ch = 0; ch < inputChannels; ++ch)
    inputPtrs[static_cast<size_t>(ch)] = inputBuffers[static_cast<size_t>(ch)].data();
  for (int ch = 0; ch < outputChannels; ++ch)
    outputPtrs[static_cast<size_t>(ch)] = outputBuffers[static_cast<size_t>(ch)].data();
  
  // Generate input signal
  std::vector<float> floatInput(static_cast<size_t>(config.blockSize));
  GenerateGuitarLikeSignal(floatInput, config.sampleRate);
  for (int i = 0; i < config.blockSize; ++i)
  {
    const NAM_SAMPLE sampleValue = static_cast<NAM_SAMPLE>(floatInput[i]);
    for (int ch = 0; ch < inputChannels; ++ch)
      inputBuffers[static_cast<size_t>(ch)][static_cast<size_t>(i)] = sampleValue;
  }
  
  // Warmup
  for (int i = 0; i < config.warmupBlocks; ++i)
  {
    model->process(inputPtrs.data(), outputPtrs.data(), config.blockSize);
  }
  
  // Measure
  std::vector<double> blockTimesUs;
  blockTimesUs.reserve(static_cast<size_t>(config.measureBlocks));
  
  auto overallStart = std::chrono::high_resolution_clock::now();
  
  for (int block = 0; block < config.measureBlocks; ++block)
  {
    auto blockStart = std::chrono::high_resolution_clock::now();
    model->process(inputPtrs.data(), outputPtrs.data(), config.blockSize);
    auto blockEnd = std::chrono::high_resolution_clock::now();
    
    auto blockDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(blockEnd - blockStart);
    blockTimesUs.push_back(static_cast<double>(blockDuration.count()) / 1000.0);
  }
  
  auto overallEnd = std::chrono::high_resolution_clock::now();
  auto totalDuration = std::chrono::duration_cast<std::chrono::microseconds>(overallEnd - overallStart);
  
  // Calculate statistics
  result.totalBlocks = config.measureBlocks;
  result.totalSamples = static_cast<long long>(config.measureBlocks) * config.blockSize;
  result.totalTimeMs = static_cast<double>(totalDuration.count()) / 1000.0;
  
  result.samplesPerSecond = static_cast<double>(result.totalSamples) / (result.totalTimeMs / 1000.0);
  result.blocksPerSecond = static_cast<double>(result.totalBlocks) / (result.totalTimeMs / 1000.0);
  result.realTimeFactor = result.samplesPerSecond / config.sampleRate;
  
  result.averageBlockTimeUs = std::accumulate(blockTimesUs.begin(), blockTimesUs.end(), 0.0) / blockTimesUs.size();
  result.minBlockTimeUs = *std::min_element(blockTimesUs.begin(), blockTimesUs.end());
  result.maxBlockTimeUs = *std::max_element(blockTimesUs.begin(), blockTimesUs.end());
  
  double sumSquares = 0.0;
  for (double t : blockTimesUs)
  {
    double diff = t - result.averageBlockTimeUs;
    sumSquares += diff * diff;
  }
  result.stdDevBlockTimeUs = std::sqrt(sumSquares / blockTimesUs.size());
  
  // Check if it meets real-time requirements
  // Block time budget = blockSize / sampleRate * 1e6 µs
  double blockBudgetUs = (static_cast<double>(config.blockSize) / config.sampleRate) * 1e6;
  result.meetsRealTime = result.maxBlockTimeUs < blockBudgetUs;
  
  return result;
}

// ============================================================================
// Graph-based NAM Benchmark (via SignalGraphExecutor)
// ============================================================================

guitarfx::Preset MakeNamPreset(const fs::path& modelPath, const char* ampType)
{
  guitarfx::Preset preset;
  preset.id = "benchmark";
  preset.name = "benchmark";
  preset.version = 2;

  guitarfx::GraphNode input;
  input.id = "input";
  input.type = guitarfx::kNodeTypeInput;
  input.category = "routing";

  guitarfx::GraphNode amp;
  amp.id = "amp";
  amp.type = ampType;
  amp.category = "amp";
  amp.enabled = true;
  guitarfx::ResourceRef ref;
  ref.resourceType = "nam";
  ref.filePath = modelPath;
  amp.resources.push_back(ref);

  guitarfx::GraphNode output;
  output.id = "output";
  output.type = guitarfx::kNodeTypeOutput;
  output.category = "routing";

  preset.graph.nodes = {input, amp, output};
  preset.graph.edges = {
    {input.id, amp.id, 0, 0, 1.0},
    {amp.id, output.id, 0, 0, 1.0}
  };

  return preset;
}

BenchmarkResult BenchmarkGraphNAM(
  const fs::path& resourcesDir,
  const fs::path& modelPath,
  const std::string& modelName,
  const char* ampType,
  const BenchmarkConfig& config)
{
  BenchmarkResult result;
  result.name = std::string(ampType) + " (via SignalGraphExecutor)";
  result.modelName = modelName;
  result.sampleRate = config.sampleRate;
  result.blockSize = config.blockSize;
  
  // Set up resources and executor
  guitarfx::RegisterAllEffects();
  auto resourceLibrary = std::make_unique<guitarfx::ResourceLibrary>();
  resourceLibrary->LoadFromDirectory(resourcesDir);
  
  auto executor = std::make_unique<guitarfx::SignalGraphExecutor>();
  executor->SetResourceLibrary(resourceLibrary.get());
  
  auto preset = MakeNamPreset(modelPath, ampType);
  executor->SetGraph(preset.graph);
  executor->Prepare(config.sampleRate, config.blockSize);
  
  // Allocate buffers
  std::vector<float> inputL(static_cast<size_t>(config.blockSize));
  std::vector<float> inputR(static_cast<size_t>(config.blockSize));
  std::vector<float> outputL(static_cast<size_t>(config.blockSize));
  std::vector<float> outputR(static_cast<size_t>(config.blockSize));
  
  GenerateGuitarLikeSignal(inputL, config.sampleRate);
  inputR = inputL;  // Mono signal duplicated
  
  float* inputs[2] = {inputL.data(), inputR.data()};
  float* outputs[2] = {outputL.data(), outputR.data()};
  
  // Warmup
  for (int i = 0; i < config.warmupBlocks; ++i)
  {
    executor->Process(inputs, outputs, config.blockSize);
  }
  
  // Measure
  std::vector<double> blockTimesUs;
  blockTimesUs.reserve(static_cast<size_t>(config.measureBlocks));
  
  auto overallStart = std::chrono::high_resolution_clock::now();
  
  for (int block = 0; block < config.measureBlocks; ++block)
  {
    auto blockStart = std::chrono::high_resolution_clock::now();
    executor->Process(inputs, outputs, config.blockSize);
    auto blockEnd = std::chrono::high_resolution_clock::now();
    
    auto blockDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(blockEnd - blockStart);
    blockTimesUs.push_back(static_cast<double>(blockDuration.count()) / 1000.0);
  }
  
  auto overallEnd = std::chrono::high_resolution_clock::now();
  auto totalDuration = std::chrono::duration_cast<std::chrono::microseconds>(overallEnd - overallStart);
  
  // Calculate statistics
  result.totalBlocks = config.measureBlocks;
  result.totalSamples = static_cast<long long>(config.measureBlocks) * config.blockSize;
  result.totalTimeMs = static_cast<double>(totalDuration.count()) / 1000.0;
  
  result.samplesPerSecond = static_cast<double>(result.totalSamples) / (result.totalTimeMs / 1000.0);
  result.blocksPerSecond = static_cast<double>(result.totalBlocks) / (result.totalTimeMs / 1000.0);
  result.realTimeFactor = result.samplesPerSecond / config.sampleRate;
  
  result.averageBlockTimeUs = std::accumulate(blockTimesUs.begin(), blockTimesUs.end(), 0.0) / blockTimesUs.size();
  result.minBlockTimeUs = *std::min_element(blockTimesUs.begin(), blockTimesUs.end());
  result.maxBlockTimeUs = *std::max_element(blockTimesUs.begin(), blockTimesUs.end());
  
  double sumSquares = 0.0;
  for (double t : blockTimesUs)
  {
    double diff = t - result.averageBlockTimeUs;
    sumSquares += diff * diff;
  }
  result.stdDevBlockTimeUs = std::sqrt(sumSquares / blockTimesUs.size());
  
  double blockBudgetUs = (static_cast<double>(config.blockSize) / config.sampleRate) * 1e6;
  result.meetsRealTime = result.maxBlockTimeUs < blockBudgetUs;
  
  return result;
}

// ============================================================================
// Multi-Configuration Benchmark
// ============================================================================

void RunMultiConfigBenchmark(
  const fs::path& resourcesDir,
  const fs::path& modelPath,
  const std::string& modelName)
{
  std::cout << "\n";
  std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
  std::cout << "║     NAM MODEL PERFORMANCE BENCHMARK - MULTI-CONFIGURATION        ║\n";
  std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";
  std::cout << "\nModel: " << modelName << "\n";
  std::cout << "Path: " << modelPath.filename().string() << "\n";
  
  // Print SIMD level
  std::cout << "SIMD Level: ";
  switch (guitarfx::simd::GetSimdLevel())
  {
    case guitarfx::simd::SimdLevel::AVX2: std::cout << "AVX2"; break;
    case guitarfx::simd::SimdLevel::AVX: std::cout << "AVX"; break;
    case guitarfx::simd::SimdLevel::SSE4: std::cout << "SSE4"; break;
    case guitarfx::simd::SimdLevel::SSE2: std::cout << "SSE2"; break;
    default: std::cout << "Scalar"; break;
  }
  std::cout << "\n";
  
  // Test configurations
  struct TestConfig
  {
    double sampleRate;
    int blockSize;
    const char* label;
  };
  
  std::vector<TestConfig> configs = {
    {44100.0, 64, "44.1kHz / 64 samples (low latency)"},
    {44100.0, 128, "44.1kHz / 128 samples"},
    {44100.0, 256, "44.1kHz / 256 samples"},
    {48000.0, 128, "48kHz / 128 samples"},
    {48000.0, 256, "48kHz / 256 samples"},
    {48000.0, 512, "48kHz / 512 samples"},
    {96000.0, 256, "96kHz / 256 samples (high quality)"},
  };
  
  std::cout << "\n┌─────────────────────────────────────┬──────────────┬──────────────┬──────────┐\n";
  std::cout << "│ Configuration                       │ Standard NAM │ Optimized    │ Speedup  │\n";
  std::cout << "│                                     │ RT Factor    │ RT Factor    │          │\n";
  std::cout << "├─────────────────────────────────────┼──────────────┼──────────────┼──────────┤\n";
  
  for (const auto& tc : configs)
  {
    BenchmarkConfig cfg;
    cfg.sampleRate = tc.sampleRate;
    cfg.blockSize = tc.blockSize;
    cfg.warmupBlocks = 50;
    cfg.measureBlocks = 500;
    
    auto standardResult = BenchmarkGraphNAM(resourcesDir, modelPath, modelName, kAmpTypeStandard, cfg);
    auto optimizedResult = BenchmarkGraphNAM(resourcesDir, modelPath, modelName, kAmpTypeOptimized, cfg);
    
    double speedup = optimizedResult.samplesPerSecond / standardResult.samplesPerSecond;
    
    std::cout << "│ " << std::left << std::setw(35) << tc.label << " │ ";
    std::cout << std::right << std::setw(10) << std::fixed << std::setprecision(1) << standardResult.realTimeFactor << "x │ ";
    std::cout << std::setw(10) << optimizedResult.realTimeFactor << "x │ ";
    std::cout << std::setw(6) << std::setprecision(2) << speedup << "x  │\n";
  }
  
  std::cout << "└─────────────────────────────────────┴──────────────┴──────────────┴──────────┘\n";
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

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║           NAM MODEL DSP PERFORMANCE BENCHMARK                    ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\nResources: " << resourcesDir.string() << "\n";

    // Load model library
    const auto audioModelsJson = nlohmann::json::parse(
      std::ifstream(dataDir / "audiofx-models.json"));

    if (!audioModelsJson.is_array() || audioModelsJson.empty())
    {
      std::cerr << "ERROR: No models found in audiofx-models.json\n";
      return 1;
    }

    // Use first model for detailed benchmark
    const auto& firstModel = audioModelsJson[0];
    const fs::path modelPath = resourcesDir / firstModel.value("filePath", "");
    const std::string modelName = firstModel.value("title", "Unknown");

    if (!fs::exists(modelPath))
    {
      std::cerr << "ERROR: Model file not found: " << modelPath << "\n";
      return 1;
    }

    // Run activation function micro-benchmarks first
    guitarfx::benchmark::RunAllBenchmarks();

    // Configure benchmark
    BenchmarkConfig config;
    config.sampleRate = 48000.0;
    config.blockSize = 512;
    config.warmupBlocks = 100;
    config.measureBlocks = 1000;

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║              FULL MODEL PROCESSING BENCHMARK                     ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";
    std::cout << "\nModel: " << modelName << "\n";
    std::cout << "Sample Rate: " << config.sampleRate << " Hz\n";
    std::cout << "Block Size: " << config.blockSize << " samples\n";
    std::cout << "Warmup: " << config.warmupBlocks << " blocks\n";
    std::cout << "Measure: " << config.measureBlocks << " blocks\n";

    // Benchmark direct NAM library
    auto directResult = BenchmarkDirectNAM(modelPath, modelName, config);
    PrintBenchmarkResult(directResult);

    // Benchmark standard NAM effect
    auto standardResult = BenchmarkGraphNAM(resourcesDir, modelPath, modelName, kAmpTypeStandard, config);
    PrintBenchmarkResult(standardResult);

    // Benchmark optimized NAM effect
    auto optimizedResult = BenchmarkGraphNAM(resourcesDir, modelPath, modelName, kAmpTypeOptimized, config);
    PrintBenchmarkResult(optimizedResult);

    // Print speedup comparison
    PrintSpeedup(standardResult, optimizedResult);

    // Run multi-configuration benchmark
    RunMultiConfigBenchmark(resourcesDir, modelPath, modelName);

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                    BENCHMARK COMPLETE                            ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════════╝\n";

    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Fatal error: " << ex.what() << "\n";
    return 1;
  }
}
