/**
 * Sample Rate and Buffer Size Diagnostic Tests
 *
 * This test file diagnoses potential issues with:
 * 1. Sample rate mismatches between model initialization and processing
 * 2. Various buffer sizes (very small, typical, large)
 * 3. Dynamic buffer size changes during processing
 * 4. Sample rate transitions
 */

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dsp/SignalGraphExecutor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetTypes.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

// Force factory registration
namespace fs = std::filesystem;

namespace
{
  constexpr double kPi = 3.14159265358979323846;
  constexpr const char *kTestAmpNodeId = "amp";
  constexpr const char *kInputNodeId = "input";
  constexpr const char *kOutputNodeId = "output";

  // Common sample rates to test
  constexpr double kSampleRates[] = {44100.0, 48000.0, 88200.0, 96000.0};
  constexpr int kNumSampleRates = sizeof(kSampleRates) / sizeof(kSampleRates[0]);

  // Common buffer sizes to test
  constexpr int kBufferSizes[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
  constexpr int kNumBufferSizes = sizeof(kBufferSizes) / sizeof(kBufferSizes[0]);

  // ============================================================================
  // Signal Generation
  // ============================================================================

  void GenerateSineWave(std::vector<float> &buffer, double frequency, double sampleRate, double amplitude = 0.3)
  {
    for (std::size_t i = 0; i < buffer.size(); ++i)
    {
      buffer[i] = static_cast<float>(amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / sampleRate));
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
    int numNaN = 0;
    int numInf = 0;
  };

  template <typename T>
  SignalStats AnalyzeBuffer(const std::vector<T> &buffer)
  {
    SignalStats stats;
    if (buffer.empty())
      return stats;

    stats.min = static_cast<double>(buffer[0]);
    stats.max = static_cast<double>(buffer[0]);

    double sum = 0.0;
    double sumSquares = 0.0;

    for (const auto &sample : buffer)
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

      if (val != 0.0)
        stats.isAllZeros = false;
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

  // ============================================================================
  // Graph preset helpers
  // ============================================================================

  guitarfx::Preset MakeAmpPreset(const std::filesystem::path &modelPath,
                                 double inputTrim,
                                 double outputTrim,
                                 double drive)
  {
    guitarfx::Preset preset;
    preset.id = "sample-rate-buffer-test";
    preset.name = "sample-rate-buffer-test";
    preset.version = 2;
    preset.global.inputTrim = inputTrim;
    preset.global.outputTrim = outputTrim;
    preset.global.outputVolume = 1.0;

    guitarfx::GraphNode input;
    input.id = kInputNodeId;
    input.type = guitarfx::kNodeTypeInput;
    input.category = "utility";

    guitarfx::GraphNode amp;
    amp.id = kTestAmpNodeId;
    amp.type = "amp_nam";
    amp.category = "amp";
    amp.params["drive"] = drive;
    guitarfx::ResourceRef ref;
    ref.filePath = modelPath;
    amp.resources.push_back(ref);

    guitarfx::GraphNode output;
    output.id = kOutputNodeId;
    output.type = guitarfx::kNodeTypeOutput;
    output.category = "utility";

    preset.graph.nodes = {input, amp, output};
    preset.graph.edges = {
        {input.id, amp.id, 0, 0, 1.0},
        {amp.id, output.id, 0, 0, 1.0}};

    return preset;
  }

  class GraphHarness
  {
  public:
    GraphHarness(const std::filesystem::path &modelPath, double sampleRate, int blockSize,
                 double inputTrim, double outputTrim, double drive)
        : mSampleRate(sampleRate), mBlockSize(blockSize)
    {
      guitarfx::RegisterAllEffects();
      mExecutor = std::make_unique<guitarfx::SignalGraphExecutor>();
      auto preset = MakeAmpPreset(modelPath, inputTrim, outputTrim, drive);
      mExecutor->SetInputTrim(preset.global.inputTrim);
      mExecutor->SetOutputTrim(preset.global.outputTrim);
      mExecutor->SetGraph(preset.graph);
      mExecutor->Prepare(sampleRate, blockSize);
    }

    void Reprepare(double sampleRate, int blockSize)
    {
      mSampleRate = sampleRate;
      mBlockSize = blockSize;
      mExecutor->Prepare(sampleRate, blockSize);
    }

    void SetDrive(double drive)
    {
      mExecutor->SetNodeParam(kTestAmpNodeId, "drive", drive);
    }

    void SetTone(double tone)
    {
      mExecutor->SetNodeParam(kTestAmpNodeId, "tone", tone);
    }

    void Process(float **inputs, float **outputs, int numSamples)
    {
      mExecutor->Process(inputs, outputs, numSamples);
    }

  private:
    std::unique_ptr<guitarfx::SignalGraphExecutor> mExecutor;
    double mSampleRate;
    int mBlockSize;
  };

  void PrintStats(const std::string &label, const SignalStats &stats)
  {
    std::cout << "    " << label << ": Peak=" << std::fixed << std::setprecision(4) << stats.peak
              << ", RMS=" << stats.rms;
    if (stats.hasNaN)
      std::cout << " [NaN:" << stats.numNaN << "]";
    if (stats.hasInf)
      std::cout << " [Inf:" << stats.numInf << "]";
    if (stats.isAllZeros)
      std::cout << " [ZEROS]";
    std::cout << "\n";
  }

  // ============================================================================
  // Test: Various Sample Rates
  // ============================================================================

  bool TestSampleRates(const fs::path &modelPath)
  {
    std::cout << "\n=== Test: Sample Rate Variations ===\n";
    std::cout << "Testing model at different sample rates\n\n";

    bool allPassed = true;

    for (int i = 0; i < kNumSampleRates; ++i)
    {
      const double sampleRate = kSampleRates[i];
      const int blockSize = 512;

      std::cout << "--- Sample Rate: " << static_cast<int>(sampleRate) << " Hz ---\n";

      GraphHarness dsp(modelPath, sampleRate, blockSize, 0.0, 0.0, 0.5);

      // Generate a 440Hz test tone
      std::vector<float> inputL(blockSize), inputR(blockSize);
      std::vector<float> outputL(blockSize), outputR(blockSize);
      GenerateSineWave(inputL, 440.0, sampleRate, 0.3);
      GenerateSineWave(inputR, 440.0, sampleRate, 0.3);

      float *inputs[2] = {inputL.data(), inputR.data()};
      float *outputs[2] = {outputL.data(), outputR.data()};

      // Process multiple blocks to check stability
      bool stable = true;
      for (int block = 0; block < 20; ++block)
      {
        GenerateSineWave(inputL, 440.0, sampleRate, 0.3);
        GenerateSineWave(inputR, 440.0, sampleRate, 0.3);

        dsp.Process(inputs, outputs, blockSize);

        auto stats = AnalyzeBuffer(outputL);
        if (stats.hasNaN || stats.hasInf)
        {
          std::cerr << "  Block " << block << ": NaN/Inf detected!\n";
          stable = false;
          break;
        }
        if (stats.peak > 10.0)
        {
          std::cerr << "  Block " << block << ": Excessive peak: " << stats.peak << "\n";
          stable = false;
        }
      }

      auto finalStats = AnalyzeBuffer(outputL);
      PrintStats("Output", finalStats);

      if (stable)
      {
        std::cout << "  PASSED: Stable processing at " << static_cast<int>(sampleRate) << " Hz\n";
      }
      else
      {
        std::cout << "  FAILED: Unstable at " << static_cast<int>(sampleRate) << " Hz\n";
        allPassed = false;
      }
      std::cout << "\n";
    }

    return allPassed;
  }

  // ============================================================================
  // Test: Various Buffer Sizes
  // ============================================================================

  bool TestBufferSizes(const fs::path &modelPath)
  {
    std::cout << "\n=== Test: Buffer Size Variations ===\n";
    std::cout << "Testing model with different buffer sizes at 48kHz\n\n";

    const double sampleRate = 48000.0;
    bool allPassed = true;

    for (int i = 0; i < kNumBufferSizes; ++i)
    {
      const int blockSize = kBufferSizes[i];

      std::cout << "--- Buffer Size: " << blockSize << " samples ---\n";

      GraphHarness dsp(modelPath, sampleRate, blockSize, 0.0, 0.0, 0.5);
      dsp.SetTone(0.0);

      std::vector<float> inputL(blockSize), inputR(blockSize);
      std::vector<float> outputL(blockSize), outputR(blockSize);

      float *inputs[2] = {inputL.data(), inputR.data()};
      float *outputs[2] = {outputL.data(), outputR.data()};

      // Process many blocks to reach steady state
      const int numBlocks = std::max(100, 48000 / blockSize); // ~1 second of audio
      bool stable = true;

      for (int block = 0; block < numBlocks; ++block)
      {
        GenerateSineWave(inputL, 440.0, sampleRate, 0.3);
        GenerateSineWave(inputR, 440.0, sampleRate, 0.3);

        dsp.Process(inputs, outputs, blockSize);

        auto stats = AnalyzeBuffer(outputL);
        if (stats.hasNaN || stats.hasInf)
        {
          std::cerr << "  Block " << block << ": NaN/Inf detected!\n";
          stable = false;
          break;
        }
        if (stats.peak > 10.0)
        {
          std::cerr << "  Block " << block << ": Excessive peak: " << stats.peak << "\n";
          stable = false;
        }
      }

      auto finalStats = AnalyzeBuffer(outputL);
      PrintStats("Output", finalStats);

      // Calculate latency in ms
      const double latencyMs = static_cast<double>(blockSize) / sampleRate * 1000.0;
      std::cout << "  Latency: " << std::fixed << std::setprecision(2) << latencyMs << " ms\n";

      if (stable)
      {
        std::cout << "  PASSED\n";
      }
      else
      {
        std::cout << "  FAILED\n";
        allPassed = false;
      }
      std::cout << "\n";
    }

    return allPassed;
  }

  // ============================================================================
  // Test: Dynamic Buffer Size Changes
  // ============================================================================

  bool TestDynamicBufferChanges(const fs::path &modelPath)
  {
    std::cout << "\n=== Test: Dynamic Buffer Size Changes ===\n";
    std::cout << "Simulating buffer size changes during playback (like DAW changes)\n\n";

    const double sampleRate = 48000.0;
    bool allPassed = true;

    int currentBlockSize = 512;
    GraphHarness dsp(modelPath, sampleRate, currentBlockSize, 0.0, 0.0, 0.5);
    dsp.SetTone(0.0);

    // Simulate buffer size changes
    const int testSequence[] = {512, 256, 1024, 128, 512, 2048, 64, 512};
    const int numChanges = sizeof(testSequence) / sizeof(testSequence[0]);

    for (int i = 0; i < numChanges; ++i)
    {
      const int newBlockSize = testSequence[i];
      std::cout << "--- Changing buffer: " << currentBlockSize << " -> " << newBlockSize << " ---\n";

      // Re-prepare with new block size (simulating DAW buffer change)
      dsp.Reprepare(sampleRate, newBlockSize);
      currentBlockSize = newBlockSize;

      std::vector<float> inputL(currentBlockSize), inputR(currentBlockSize);
      std::vector<float> outputL(currentBlockSize), outputR(currentBlockSize);

      float *inputs[2] = {inputL.data(), inputR.data()};
      float *outputs[2] = {outputL.data(), outputR.data()};

      // Process a few blocks after the change
      bool stable = true;
      for (int block = 0; block < 10; ++block)
      {
        GenerateSineWave(inputL, 440.0, sampleRate, 0.3);
        GenerateSineWave(inputR, 440.0, sampleRate, 0.3);

        dsp.Process(inputs, outputs, currentBlockSize);

        auto stats = AnalyzeBuffer(outputL);
        if (stats.hasNaN || stats.hasInf)
        {
          std::cerr << "  Block " << block << " after change: NaN/Inf!\n";
          stable = false;
          break;
        }
        if (stats.peak > 10.0)
        {
          std::cerr << "  Block " << block << " after change: Excessive peak!\n";
          stable = false;
        }
      }

      auto finalStats = AnalyzeBuffer(outputL);
      PrintStats("Output", finalStats);

      if (!stable)
      {
        std::cout << "  FAILED after buffer change\n";
        allPassed = false;
      }
      else
      {
        std::cout << "  OK\n";
      }
    }

    std::cout << "\nDynamic buffer test: " << (allPassed ? "PASSED" : "FAILED") << "\n";
    return allPassed;
  }

  // ============================================================================
  // Test: Sample Rate Mismatch (Bug Scenario)
  // ============================================================================

  bool TestSampleRateMismatch(const fs::path &modelPath)
  {
    std::cout << "\n=== Test: Sample Rate Mismatch Scenario ===\n";
    std::cout << "Testing what happens if model is initialized at one rate but audio comes at another\n\n";

    bool allPassed = true;

    // Scenario 1: Model prepared at 48kHz, process with 44.1kHz signal
    {
      std::cout << "--- Scenario: Prepared at 48kHz, signal generated for 44.1kHz ---\n";

      GraphHarness dsp(modelPath, 48000.0, 512, 0.0, 0.0, 0.5);
      dsp.SetTone(0.0);

      std::vector<float> inputL(512), inputR(512);
      std::vector<float> outputL(512), outputR(512);

      // Generate signal as if it's 44.1kHz (wrong sample rate)
      GenerateSineWave(inputL, 440.0, 44100.0, 0.3); // Wrong!
      GenerateSineWave(inputR, 440.0, 44100.0, 0.3);

      float *inputs[2] = {inputL.data(), inputR.data()};
      float *outputs[2] = {outputL.data(), outputR.data()};

      dsp.Process(inputs, outputs, 512);

      auto stats = AnalyzeBuffer(outputL);
      PrintStats("Output", stats);

      // The output frequency will be wrong but shouldn't cause NaN/Inf
      if (stats.hasNaN || stats.hasInf)
      {
        std::cout << "  WARNING: NaN/Inf detected even though input was valid!\n";
        allPassed = false;
      }
      else
      {
        std::cout << "  Note: Signal will be pitch-shifted but processing is stable\n";
      }
    }

    // Scenario 3: Very high sample rate
    {
      std::cout << "\n--- Scenario: Very high sample rate (192kHz) ---\n";

      GraphHarness dsp(modelPath, 192000.0, 512, 0.0, 0.0, 0.5);
      dsp.SetTone(0.0);

      std::vector<float> inputL(512), inputR(512);
      std::vector<float> outputL(512), outputR(512);

      GenerateSineWave(inputL, 440.0, 192000.0, 0.3);
      GenerateSineWave(inputR, 440.0, 192000.0, 0.3);

      float *inputs[2] = {inputL.data(), inputR.data()};
      float *outputs[2] = {outputL.data(), outputR.data()};

      dsp.Process(inputs, outputs, 512);

      auto stats = AnalyzeBuffer(outputL);
      PrintStats("Output", stats);

      if (stats.hasNaN || stats.hasInf)
      {
        std::cout << "  WARNING: High sample rate caused instability\n";
        allPassed = false;
      }
      else
      {
        std::cout << "  OK: Stable at 192kHz\n";
      }
    }

    return allPassed;
  }

  // ============================================================================
  // Test: Edge Cases
  // ============================================================================

  bool TestEdgeCases(const fs::path &modelPath)
  {
    std::cout << "\n=== Test: Edge Cases ===\n\n";

    bool allPassed = true;

    // Test 1: Very small buffer (might cause issues)
    {
      std::cout << "--- Edge Case: Buffer size = 1 ---\n";

      GraphHarness dsp(modelPath, 48000.0, 1, 0.0, 0.0, 0.5);
      dsp.SetTone(0.0);

      std::vector<float> inputL(1), inputR(1);
      std::vector<float> outputL(1), outputR(1);

      inputL[0] = inputR[0] = 0.3f;

      float *inputs[2] = {inputL.data(), inputR.data()};
      float *outputs[2] = {outputL.data(), outputR.data()};

      bool stable = true;
      for (int i = 0; i < 1000; ++i)
      {
        inputL[0] = inputR[0] = static_cast<float>(0.3 * std::sin(2.0 * kPi * 440.0 * i / 48000.0));
        dsp.Process(inputs, outputs, 1);

        if (std::isnan(outputL[0]) || std::isinf(outputL[0]))
        {
          std::cout << "  NaN/Inf at sample " << i << "\n";
          stable = false;
          break;
        }
      }

      if (stable)
      {
        std::cout << "  PASSED: Buffer size 1 works\n";
      }
      else
      {
        std::cout << "  FAILED: Buffer size 1 causes issues\n";
        allPassed = false;
      }
    }

    // Test 2: Larger than prepared buffer
    /*{
      std::cout << "\n--- Edge Case: Process more samples than Prepare() size ---\n";

      GraphHarness dsp(modelPath, 48000.0, 256, 0.0, 0.0, 0.5);
      dsp.SetTone(0.0);

      // Try to process 512 samples (larger than prepared)
      std::vector<float> inputL(512), inputR(512);
      std::vector<float> outputL(512), outputR(512);

      GenerateSineWave(inputL, 440.0, 48000.0, 0.3);
      GenerateSineWave(inputR, 440.0, 48000.0, 0.3);

      float *inputs[2] = {inputL.data(), inputR.data()};
      float *outputs[2] = {outputL.data(), outputR.data()};

      try
      {
        dsp.Process(inputs, outputs, 512); // More than prepared!

        auto stats = AnalyzeBuffer(outputL);
        PrintStats("Output", stats);

        if (stats.hasNaN || stats.hasInf)
        {
          std::cout << "  WARNING: Oversized buffer caused NaN/Inf\n";
          allPassed = false;
        }
        else
        {
          std::cout << "  Processing handled oversized buffer (buffers resized dynamically)\n";
        }
      }
      catch (const std::exception &ex)
      {
        std::cout << "  Exception with oversized buffer: " << ex.what() << "\n";
      }
    }*/

    // Test 3: Zero-length buffer
    {
      std::cout << "\n--- Edge Case: Zero-length buffer ---\n";

      GraphHarness dsp(modelPath, 48000.0, 512, 0.0, 0.0, 0.5);
      std::vector<float> inputL(0), inputR(0);
      std::vector<float> outputL(0), outputR(0);

      float *inputs[2] = {inputL.data(), inputR.data()};
      float *outputs[2] = {outputL.data(), outputR.data()};

      try
      {
        dsp.Process(inputs, outputs, 0);
        std::cout << "  PASSED: Zero-length buffer handled gracefully\n";
      }
      catch (const std::exception &ex)
      {
        std::cout << "  Exception: " << ex.what() << "\n";
      }
    }

    // Test 4: Extreme input values
    {
      std::cout << "\n--- Edge Case: Extreme input values ---\n";

      GraphHarness dsp(modelPath, 48000.0, 512, 0.0, 0.0, 0.5);
      dsp.SetTone(0.0);

      std::vector<float> inputL(512), inputR(512);
      std::vector<float> outputL(512), outputR(512);

      // Fill with very large values
      std::fill(inputL.begin(), inputL.end(), 100.0f); // Way above normal range
      std::fill(inputR.begin(), inputR.end(), 100.0f);

      float *inputs[2] = {inputL.data(), inputR.data()};
      float *outputs[2] = {outputL.data(), outputR.data()};

      dsp.Process(inputs, outputs, 512);

      auto stats = AnalyzeBuffer(outputL);
      PrintStats("Output (input=100)", stats);

      if (stats.hasNaN || stats.hasInf)
      {
        std::cout << "  WARNING: Extreme input caused NaN/Inf\n";
        allPassed = false;
      }
      else
      {
        std::cout << "  Model handled extreme input\n";
      }

      // Now test with denormalized values
      std::fill(inputL.begin(), inputL.end(), 1e-40); // Denormalized
      std::fill(inputR.begin(), inputR.end(), 1e-40);

      dsp.Process(inputs, outputs, 512);

      stats = AnalyzeBuffer(outputL);
      PrintStats("Output (denorm)", stats);

      if (stats.hasNaN || stats.hasInf)
      {
        std::cout << "  WARNING: Denormalized input caused NaN/Inf\n";
        allPassed = false;
      }
    }

    return allPassed;
  }

} // namespace

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[])
{
#ifndef GUITARFX_TEST_RESOURCES_DIR
#error "GUITARFX_TEST_RESOURCES_DIR must be defined"
#endif

  try
  {
    const fs::path resourcesDir = fs::path(GUITARFX_TEST_RESOURCES_DIR);
    const fs::path dataDir = resourcesDir / "data";

    std::cout << "Sample Rate and Buffer Size Tests\n";
    std::cout << "==================================\n";
    std::cout << "Resources: " << resourcesDir.string() << "\n\n";

    // Load model library to find a test model
    const auto audioModelsJson = nlohmann::json::parse(
        std::ifstream(dataDir / "audiofx-models.json"));

    if (!audioModelsJson.is_array() || audioModelsJson.empty())
    {
      std::cerr << "ERROR: No models found in audiofx-models.json\n";
      return 1;
    }

    const auto &firstModel = audioModelsJson[0];
    const fs::path modelPath = resourcesDir / firstModel.value("filePath", "");

    std::cout << "Test model: " << firstModel.value("title", "Unknown") << "\n\n";

    if (!fs::exists(modelPath))
    {
      std::cerr << "ERROR: Model file not found!\n";
      return 1;
    }

    bool allPassed = true;

    allPassed &= TestSampleRates(modelPath);
    allPassed &= TestBufferSizes(modelPath);
    allPassed &= TestDynamicBufferChanges(modelPath);
    allPassed &= TestSampleRateMismatch(modelPath);
    allPassed &= TestEdgeCases(modelPath);

    std::cout << "\n==================================\n";
    std::cout << "Tests " << (allPassed ? "PASSED" : "HAD FAILURES") << "\n";

    return allPassed ? 0 : 1;
  }
  catch (const std::exception &ex)
  {
    std::cerr << "Fatal error: " << ex.what() << "\n";
    return 1;
  }
}
