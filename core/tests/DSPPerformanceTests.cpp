/**
 * @file DSPPerformanceTests.cpp
 * @brief Verifies DSP performance statistics are correctly populated and updated
 */

#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>

#include "dsp/SignalGraphExecutor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/MultiPresetMixer.h"
#include "presets/PresetTypes.h"

namespace
{
constexpr double kSR = 48000.0;
constexpr int kBlock = 512;

struct TestResult
{
  bool passed = false;
  std::string message;
};

TestResult TestDSPPerformanceStatsPopulation()
{
  TestResult result;
  result.passed = false;

  try
  {
    // Register effects
    guitarfx::RegisterAllEffects();

    // Create a simple signal graph
    guitarfx::SignalGraph graph;
    graph.nodes.push_back({"in", guitarfx::kNodeTypeInput, "", "Input", true});
    graph.nodes.push_back({"gain", "gain", "utility", "Gain", true});
    graph.nodes.back().params["gainDb"] = 0.0; // Unity gain
    graph.nodes.push_back({"out", guitarfx::kNodeTypeOutput, "", "Output", true});
    graph.edges.push_back({"in", "gain", 0, 0, 1.0});
    graph.edges.push_back({"gain", "out", 0, 0, 1.0});

    // Create executor
    guitarfx::SignalGraphExecutor executor;
    executor.SetGraph(graph);
    executor.Prepare(kSR, kBlock);

    // Get initial stats
    auto initialStats = executor.GetPerformanceStats();

    // Check that initial stats are reasonable (realTimeUs starts at 0)
    if (initialStats.totalProcessingTimeUs < 0.0)
    {
      result.message = "Initial totalProcessingTimeUs is negative: " + std::to_string(initialStats.totalProcessingTimeUs);
      return result;
    }

    if (initialStats.realTimeUs < 0.0)
    {
      result.message = "Initial realTimeUs is negative: " + std::to_string(initialStats.realTimeUs);
      return result;
    }

    if (initialStats.dspLoadPercent < 0.0 || initialStats.dspLoadPercent > 100.0)
    {
      result.message = "Initial dspLoadPercent out of range [0,100]: " + std::to_string(initialStats.dspLoadPercent);
      return result;
    }

    // Process some audio to generate performance data
    std::vector<float> inputL(kBlock, 0.5f);
    std::vector<float> inputR(kBlock, 0.5f);
    std::vector<float> outputL(kBlock, 0.0f);
    std::vector<float> outputR(kBlock, 0.0f);

    // Process multiple blocks to accumulate timing data
    float* inputs[2] = { inputL.data(), inputR.data() };
    float* outputs[2] = { outputL.data(), outputR.data() };
    for (int i = 0; i < 10; ++i)
    {
      executor.Process(inputs, outputs, kBlock);
    }

    // Get updated stats
    auto updatedStats = executor.GetPerformanceStats();

    // Verify stats are updated and non-zero
    if (updatedStats.totalProcessingTimeUs <= initialStats.totalProcessingTimeUs)
    {
      result.message = "totalProcessingTimeUs did not increase after processing: initial=" +
                      std::to_string(initialStats.totalProcessingTimeUs) + ", updated=" +
                      std::to_string(updatedStats.totalProcessingTimeUs);
      return result;
    }

    // Explicitly check for non-zero performance data
    if (updatedStats.totalProcessingTimeUs <= 0.0)
    {
      result.message = "totalProcessingTimeUs is not positive after processing: " + std::to_string(updatedStats.totalProcessingTimeUs);
      return result;
    }

    if (updatedStats.dspLoadPercent < 0.0 || updatedStats.dspLoadPercent > 100.0)
    {
      result.message = "Updated dspLoadPercent out of range [0,100]: " + std::to_string(updatedStats.dspLoadPercent);
      return result;
    }

    // Note: dspLoadPercent might be 0 for very fast processing, so we don't require it to be > 0

    // Check that node processing times are populated
    if (updatedStats.nodeProcessingTimesUs.empty())
    {
      result.message = "nodeProcessingTimesUs is empty after processing";
      return result;
    }

    // Verify we have timing data for our nodes
    bool hasGainTiming = false;
    double gainProcessingTime = 0.0;
    for (const auto& [nodeId, timeUs] : updatedStats.nodeProcessingTimesUs)
    {
      if (nodeId == "gain" && timeUs >= 0.0)
      {
        hasGainTiming = true;
        gainProcessingTime = timeUs;
        break;
      }
    }

    if (!hasGainTiming)
    {
      result.message = "No timing data found for 'gain' node";
      return result;
    }

    // Explicitly check that node processing time is positive (non-zero performance)
    if (gainProcessingTime <= 0.0)
    {
      result.message = "Gain node processing time is not positive: " + std::to_string(gainProcessingTime);
      return result;
    }

    // Log the performance data for verification
    std::cout << "DSP Performance Data:" << std::endl;
    std::cout << "  Total Processing Time: " << updatedStats.totalProcessingTimeUs << " μs" << std::endl;
    std::cout << "  Real Time: " << updatedStats.realTimeUs << " μs" << std::endl;
    std::cout << "  DSP Load: " << updatedStats.dspLoadPercent << " %" << std::endl;
    std::cout << "  Gain Node Processing Time: " << gainProcessingTime << " μs" << std::endl;

    // Verify output is correct (unity gain)
    bool outputCorrect = true;
    for (size_t i = 0; i < outputL.size(); ++i)
    {
      if (std::abs(outputL[i] - 0.5f) > 0.01f || std::abs(outputR[i] - 0.5f) > 0.01f)
      {
        outputCorrect = false;
        break;
      }
    }

    if (!outputCorrect)
    {
      result.message = "Audio processing output is incorrect";
      return result;
    }

    result.passed = true;
    result.message = "DSP performance stats populated correctly with non-zero performance data";

  }
  catch (const std::exception& e)
  {
    result.message = "Exception thrown: " + std::string(e.what());
  }
  catch (...)
  {
    result.message = "Unknown exception thrown";
  }

  return result;
}

TestResult TestDSPPerformanceStatsReset()
{
  TestResult result;
  result.passed = false;

  try
  {
    // Register effects
    guitarfx::RegisterAllEffects();

    // Create a signal graph
    guitarfx::SignalGraph graph;
    graph.nodes.push_back({"in", guitarfx::kNodeTypeInput, "", "Input", true});
    graph.nodes.push_back({"delay", "delay_digital", "delay", "Delay", true});
    graph.nodes.back().params["timeMs"] = 100.0;
    graph.nodes.back().params["feedback"] = 0.3;
    graph.nodes.back().params["mix"] = 0.5;
    graph.nodes.push_back({"out", guitarfx::kNodeTypeOutput, "", "Output", true});
    graph.edges.push_back({"in", "delay", 0, 0, 1.0});
    graph.edges.push_back({"delay", "out", 0, 0, 1.0});

    // Create executor
    guitarfx::SignalGraphExecutor executor;
    executor.SetGraph(graph);
    executor.Prepare(kSR, kBlock);

    // Process some audio
    std::vector<float> inputL(kBlock, 0.1f);
    std::vector<float> inputR(kBlock, 0.1f);
    std::vector<float> outputL(kBlock, 0.0f);
    std::vector<float> outputR(kBlock, 0.0f);

    // Process some audio
    float* inputs[2] = { inputL.data(), inputR.data() };
    float* outputs[2] = { outputL.data(), outputR.data() };
    for (int i = 0; i < 5; ++i)
    {
      executor.Process(inputs, outputs, kBlock);
    }

    auto statsAfterProcessing = executor.GetPerformanceStats();

    // Reset the executor
    executor.Reset();

    auto statsAfterReset = executor.GetPerformanceStats();

    // After reset, performance stats should be preserved (they represent accumulated performance)
    if (statsAfterReset.totalProcessingTimeUs != statsAfterProcessing.totalProcessingTimeUs)
    {
      result.message = "totalProcessingTimeUs changed after reset: before=" +
                      std::to_string(statsAfterProcessing.totalProcessingTimeUs) + ", after=" +
                      std::to_string(statsAfterReset.totalProcessingTimeUs);
      return result;
    }

    if (statsAfterReset.realTimeUs != statsAfterProcessing.realTimeUs)
    {
      result.message = "realTimeUs changed after reset: before=" +
                      std::to_string(statsAfterProcessing.realTimeUs) + ", after=" +
                      std::to_string(statsAfterReset.realTimeUs);
      return result;
    }

    result.passed = true;
    result.message = "DSP performance stats preserved after reset";

  }
  catch (const std::exception& e)
  {
    result.message = "Exception thrown: " + std::string(e.what());
  }
  catch (...)
  {
    result.message = "Unknown exception thrown";
  }

  return result;
}

TestResult TestMultiPresetMixerPerformanceStats()
{
  TestResult result;
  result.passed = false;

  try
  {
    // Register effects
    guitarfx::RegisterAllEffects();

    // Create a simple signal graph
    guitarfx::SignalGraph graph;
    graph.nodes.push_back({"in", guitarfx::kNodeTypeInput, "", "Input", true});
    graph.nodes.push_back({"gain", "gain", "utility", "Gain", true});
    graph.nodes.back().params["gainDb"] = 0.0; // Unity gain
    graph.nodes.push_back({"out", guitarfx::kNodeTypeOutput, "", "Output", true});
    graph.edges.push_back({"in", "gain", 0, 0, 1.0});
    graph.edges.push_back({"gain", "out", 0, 0, 1.0});

    // Create preset
    guitarfx::Preset preset;
    preset.id = "test_preset";
    preset.name = "Test Preset";
    preset.graph = graph;

    // Create MultiPresetMixer
    guitarfx::MultiPresetMixer mixer;
    mixer.Prepare(kSR, kBlock);

    // Add preset
    if (!mixer.AddActivePreset(preset, "preset1", "Test Preset"))
    {
      result.message = "Failed to add preset to mixer";
      return result;
    }

    // Get initial stats (should be zero)
    auto initialStats = mixer.GetPerformanceStats();
    if (initialStats.totalProcessingTimeUs != 0.0)
    {
      result.message = "Initial totalProcessingTimeUs should be 0, got: " + std::to_string(initialStats.totalProcessingTimeUs);
      return result;
    }

    // Process some audio
    std::vector<float> inputL(kBlock, 0.5f);
    std::vector<float> inputR(kBlock, 0.5f);
    std::vector<float> outputL(kBlock, 0.0f);
    std::vector<float> outputR(kBlock, 0.0f);

    float* inputs[2] = { inputL.data(), inputR.data() };
    float* outputs[2] = { outputL.data(), outputR.data() };
    for (int i = 0; i < 10; ++i)
    {
      mixer.Process(inputs, outputs, kBlock);
    }

    // Get updated stats
    auto updatedStats = mixer.GetPerformanceStats();

    // Verify stats are populated
    if (updatedStats.totalProcessingTimeUs <= 0.0)
    {
      result.message = "totalProcessingTimeUs should be positive after processing, got: " + std::to_string(updatedStats.totalProcessingTimeUs);
      return result;
    }

    if (updatedStats.nodeProcessingTimesUs.empty())
    {
      result.message = "nodeProcessingTimesUs should not be empty after processing";
      return result;
    }

    // Check that we have timing data for the gain node
    bool hasGainTiming = updatedStats.nodeProcessingTimesUs.find("gain") != updatedStats.nodeProcessingTimesUs.end();
    if (!hasGainTiming)
    {
      result.message = "No timing data found for 'gain' node in MultiPresetMixer stats";
      return result;
    }

    result.passed = true;
    result.message = "MultiPresetMixer performance stats aggregation works correctly";

  }
  catch (const std::exception& e)
  {
    result.message = "Exception thrown: " + std::string(e.what());
  }
  catch (...)
  {
    result.message = "Unknown exception thrown";
  }

  return result;
}

} // namespace

int main()
{
  std::cout << "========================================\n";
  std::cout << "DSP Performance Tests\n";
  std::cout << "========================================\n\n";

  int passed = 0;
  int failed = 0;

  // Test 1: DSP performance stats population
  {
    std::cout << "Testing DSP performance stats population... ";
    auto result = TestDSPPerformanceStatsPopulation();
    std::cout << (result.passed ? "PASS" : "FAIL") << "\n";
    if (!result.passed)
    {
      std::cout << "  " << result.message << "\n";
    }
    if (result.passed) ++passed; else ++failed;
  }

  // Test 2: DSP performance stats reset
  {
    std::cout << "Testing DSP performance stats reset... ";
    auto result = TestDSPPerformanceStatsReset();
    std::cout << (result.passed ? "PASS" : "FAIL") << "\n";
    if (!result.passed)
    {
      std::cout << "  " << result.message << "\n";
    }
    if (result.passed) ++passed; else ++failed;
  }

  // Test 3: MultiPresetMixer performance stats
  {
    std::cout << "Testing MultiPresetMixer performance stats... ";
    auto result = TestMultiPresetMixerPerformanceStats();
    std::cout << (result.passed ? "PASS" : "FAIL") << "\n";
    if (!result.passed)
    {
      std::cout << "  " << result.message << "\n";
    }
    if (result.passed) ++passed; else ++failed;
  }

  std::cout << "\n========================================\n";
  std::cout << "Results: " << passed << " passed, " << failed << " failed\n";

  return failed == 0 ? 0 : 1;
}