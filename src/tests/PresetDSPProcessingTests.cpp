/**
 * @file PresetDSPProcessingTests.cpp
 * @brief Integration tests for preset loading and DSP processing through the signal graph
 *
 * These tests verify the complete signal path:
 * 1. Preset JSON parsing → Preset struct
 * 2. GraphDSPManager loads preset and configures SignalGraphExecutor
 * 3. Resources (NAM models, IRs) are resolved and loaded into effect processors
 * 4. Audio flows through the signal graph in correct topological order
 * 5. Output audio is valid (no NaN/Inf, not silent, reasonable levels)
 */

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "dsp/GraphDSPManager.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"
#include "IPlugConstants.h"

namespace fs = std::filesystem;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kTestSampleRate = 48000.0;
constexpr int kTestBlockSize = 512;
constexpr int kStabilityBlocks = 10;

// ============================================================================
// JSON Loading
// ============================================================================

nlohmann::json LoadJson(const fs::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input)
  {
    throw std::runtime_error("Failed to open JSON file: " + path.string());
  }

  nlohmann::json document;
  input >> document;
  return document;
}

// ============================================================================
// Signal Generation
// ============================================================================

void GenerateSineWave(std::vector<double>& buffer, double frequency, double sampleRate, double amplitude = 0.5)
{
  for (std::size_t i = 0; i < buffer.size(); ++i)
  {
    buffer[i] = amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / sampleRate);
  }
}

void GenerateImpulse(std::vector<double>& buffer, double amplitude = 0.8)
{
  std::fill(buffer.begin(), buffer.end(), 0.0);
  if (!buffer.empty())
  {
    buffer[0] = amplitude;
  }
}

void GenerateNoise(std::vector<double>& buffer, double amplitude = 0.3)
{
  static unsigned int seed = 12345;
  for (auto& sample : buffer)
  {
    seed = seed * 1103515245 + 12345;
    double rand01 = static_cast<double>((seed >> 16) & 0x7FFF) / 32767.0;
    sample = amplitude * (2.0 * rand01 - 1.0);
  }
}

// ============================================================================
// Signal Analysis
// ============================================================================

struct SignalAnalysis
{
  bool hasNaN = false;
  bool hasInf = false;
  bool isAllZeros = true;
  bool isAllSameValue = true;
  double peakValue = 0.0;
  double rmsValue = 0.0;
  double dcOffset = 0.0;
};

SignalAnalysis AnalyzeSignal(const std::vector<double>& buffer)
{
  SignalAnalysis result;
  
  if (buffer.empty())
  {
    return result;
  }

  double sumSquares = 0.0;
  double sum = 0.0;
  const double firstValue = buffer[0];

  for (const auto& sample : buffer)
  {
    if (std::isnan(sample))
    {
      result.hasNaN = true;
    }
    if (std::isinf(sample))
    {
      result.hasInf = true;
    }
    
    const double absSample = std::abs(sample);
    if (absSample > result.peakValue)
    {
      result.peakValue = absSample;
    }
    
    if (sample != 0.0)
    {
      result.isAllZeros = false;
    }
    
    if (sample != firstValue)
    {
      result.isAllSameValue = false;
    }
    
    sumSquares += sample * sample;
    sum += sample;
  }

  result.rmsValue = std::sqrt(sumSquares / static_cast<double>(buffer.size()));
  result.dcOffset = sum / static_cast<double>(buffer.size());

  return result;
}

// ============================================================================
// Test Result Structures
// ============================================================================

struct ProcessingTestResult
{
  bool success = false;
  std::string errorMessage;
  SignalAnalysis inputAnalysis;
  SignalAnalysis outputAnalysis;
};

struct GraphValidationResult
{
  bool valid = false;
  std::string errorMessage;
  std::vector<std::string> executionOrder;
  bool hasInputNode = false;
  bool hasOutputNode = false;
  bool hasAmpNode = false;
  bool hasCabNode = false;
};

// ============================================================================
// Graph Validation
// ============================================================================

GraphValidationResult ValidatePresetGraph(const namguitar::Preset& preset)
{
  GraphValidationResult result;
  
  // Check for required nodes
  for (const auto& node : preset.graph.nodes)
  {
    if (node.type == "input" || node.id == "__input__")
    {
      result.hasInputNode = true;
    }
    else if (node.type == "output" || node.id == "__output__")
    {
      result.hasOutputNode = true;
    }
    else if (node.type == "amp_nam")
    {
      result.hasAmpNode = true;
    }
    else if (node.type == "cab_ir")
    {
      result.hasCabNode = true;
    }
  }
  
  // Check edges reference valid nodes
  for (const auto& edge : preset.graph.edges)
  {
    bool fromValid = (edge.from == "__input__");
    bool toValid = (edge.to == "__output__");
    
    for (const auto& node : preset.graph.nodes)
    {
      if (node.id == edge.from)
        fromValid = true;
      if (node.id == edge.to)
        toValid = true;
    }
    
    if (!fromValid)
    {
      result.errorMessage = "Edge references unknown source node: " + edge.from;
      return result;
    }
    if (!toValid)
    {
      result.errorMessage = "Edge references unknown target node: " + edge.to;
      return result;
    }
  }
  
  if (!result.hasAmpNode)
  {
    result.errorMessage = "Preset has no amp_nam node";
    return result;
  }
  
  if (!result.hasCabNode)
  {
    result.errorMessage = "Preset has no cab_ir node";
    return result;
  }
  
  result.valid = true;
  return result;
}

// ============================================================================
// DSP Processing Tests
// ============================================================================

ProcessingTestResult TestGraphDSPProcessing(namguitar::GraphDSPManager& dsp, int blockSize, double sampleRate)
{
  ProcessingTestResult result;

  // Create stereo input/output buffers
  std::vector<double> inputL(static_cast<std::size_t>(blockSize));
  std::vector<double> inputR(static_cast<std::size_t>(blockSize));
  std::vector<double> outputL(static_cast<std::size_t>(blockSize));
  std::vector<double> outputR(static_cast<std::size_t>(blockSize));

  // Generate test signal - 440 Hz sine wave (A4 note)
  GenerateSineWave(inputL, 440.0, sampleRate, 0.5);
  GenerateSineWave(inputR, 440.0, sampleRate, 0.5);

  // Analyze input
  result.inputAnalysis = AnalyzeSignal(inputL);

  // Set up buffer pointers
  double* inputs[2] = {inputL.data(), inputR.data()};
  double* outputs[2] = {outputL.data(), outputR.data()};

  // Process audio through DSP graph
  try
  {
    dsp.Process(inputs, outputs, blockSize);
  }
  catch (const std::exception& ex)
  {
    result.errorMessage = std::string("DSP processing threw exception: ") + ex.what();
    return result;
  }
  catch (...)
  {
    result.errorMessage = "DSP processing threw unknown exception";
    return result;
  }

  // Analyze output
  result.outputAnalysis = AnalyzeSignal(outputL);

  // Validate output
  if (result.outputAnalysis.hasNaN)
  {
    result.errorMessage = "Output contains NaN values";
    return result;
  }

  if (result.outputAnalysis.hasInf)
  {
    result.errorMessage = "Output contains infinite values";
    return result;
  }

  if (result.outputAnalysis.isAllZeros)
  {
    result.errorMessage = "Output is all zeros (no signal produced)";
    return result;
  }

  if (result.outputAnalysis.isAllSameValue)
  {
    result.errorMessage = "Output is all the same value (DC signal)";
    return result;
  }

  // Check for reasonable output levels (not clipping excessively)
  if (result.outputAnalysis.peakValue > 10.0)
  {
    result.errorMessage = "Output peak level is excessively high (" + 
                          std::to_string(result.outputAnalysis.peakValue) + ")";
    return result;
  }

  result.success = true;
  return result;
}

// Process multiple blocks to test stability over time
ProcessingTestResult TestGraphDSPStability(namguitar::GraphDSPManager& dsp, int blockSize, double sampleRate, int numBlocks)
{
  ProcessingTestResult result;

  std::vector<double> inputL(static_cast<std::size_t>(blockSize));
  std::vector<double> inputR(static_cast<std::size_t>(blockSize));
  std::vector<double> outputL(static_cast<std::size_t>(blockSize));
  std::vector<double> outputR(static_cast<std::size_t>(blockSize));

  double* inputs[2] = {inputL.data(), inputR.data()};
  double* outputs[2] = {outputL.data(), outputR.data()};

  for (int block = 0; block < numBlocks; ++block)
  {
    // Vary the test signal slightly between blocks
    const double frequency = 220.0 + static_cast<double>(block) * 20.0; // 220Hz to 400Hz
    GenerateSineWave(inputL, frequency, sampleRate, 0.4);
    GenerateSineWave(inputR, frequency, sampleRate, 0.4);

    try
    {
      dsp.Process(inputs, outputs, blockSize);
    }
    catch (const std::exception& ex)
    {
      result.errorMessage = "Block " + std::to_string(block) + " threw exception: " + ex.what();
      return result;
    }

    // Check output on each block
    const auto analysis = AnalyzeSignal(outputL);
    
    if (analysis.hasNaN)
    {
      result.errorMessage = "Block " + std::to_string(block) + " produced NaN";
      return result;
    }
    
    if (analysis.hasInf)
    {
      result.errorMessage = "Block " + std::to_string(block) + " produced infinity";
      return result;
    }

    if (analysis.peakValue > 10.0)
    {
      result.errorMessage = "Block " + std::to_string(block) + " has excessive peak: " + 
                            std::to_string(analysis.peakValue);
      return result;
    }
  }

  result.success = true;
  result.outputAnalysis = AnalyzeSignal(outputL); // Final block analysis
  return result;
}

// Test that switching presets properly resets DSP state
ProcessingTestResult TestPresetSwitching(namguitar::GraphDSPManager& dsp, 
                                          const namguitar::Preset& preset1,
                                          const namguitar::Preset& preset2,
                                          int blockSize, double sampleRate)
{
  ProcessingTestResult result;

  std::vector<double> inputL(static_cast<std::size_t>(blockSize));
  std::vector<double> inputR(static_cast<std::size_t>(blockSize));
  std::vector<double> outputL(static_cast<std::size_t>(blockSize));
  std::vector<double> outputR(static_cast<std::size_t>(blockSize));

  GenerateSineWave(inputL, 440.0, sampleRate, 0.5);
  GenerateSineWave(inputR, 440.0, sampleRate, 0.5);

  double* inputs[2] = {inputL.data(), inputR.data()};
  double* outputs[2] = {outputL.data(), outputR.data()};

  // Load first preset
  if (!dsp.LoadPreset(preset1))
  {
    result.errorMessage = "Failed to load first preset";
    return result;
  }

  // Process with first preset
  try
  {
    dsp.Process(inputs, outputs, blockSize);
  }
  catch (const std::exception& ex)
  {
    result.errorMessage = "Processing preset 1 failed: " + std::string(ex.what());
    return result;
  }

  auto analysis1 = AnalyzeSignal(outputL);
  
  // Reset and load second preset
  dsp.Reset();
  if (!dsp.LoadPreset(preset2))
  {
    result.errorMessage = "Failed to load second preset";
    return result;
  }

  // Process with second preset
  try
  {
    dsp.Process(inputs, outputs, blockSize);
  }
  catch (const std::exception& ex)
  {
    result.errorMessage = "Processing preset 2 failed: " + std::string(ex.what());
    return result;
  }

  auto analysis2 = AnalyzeSignal(outputL);

  // Verify both produced valid output
  if (analysis1.isAllZeros || analysis2.isAllZeros)
  {
    result.errorMessage = "One or both presets produced silent output";
    return result;
  }

  if (analysis1.hasNaN || analysis2.hasNaN)
  {
    result.errorMessage = "One or both presets produced NaN";
    return result;
  }

  result.success = true;
  result.outputAnalysis = analysis2;
  return result;
}

// ============================================================================
// Resource Validation
// ============================================================================

struct ResourceValidation
{
  bool valid = false;
  std::string errorMessage;
  std::string namResourceId;
  std::string irResourceId;
  fs::path namFilePath;
  fs::path irFilePath;
};

ResourceValidation ValidatePresetResources(const namguitar::Preset& preset, 
                                            const nlohmann::json& modelsLibrary,
                                            const nlohmann::json& irLibrary,
                                            const fs::path& resourcesDir)
{
  ResourceValidation result;
  
  // Find NAM and IR resources from graph nodes
  for (const auto& node : preset.graph.nodes)
  {
    if (node.type == "amp_nam" && node.resource)
    {
      if (node.resource->resourceType == "nam" && !node.resource->resourceId.empty())
      {
        result.namResourceId = node.resource->resourceId;
      }
    }
    else if (node.type == "cab_ir" && node.resource)
    {
      if (node.resource->resourceType == "ir" && !node.resource->resourceId.empty())
      {
        result.irResourceId = node.resource->resourceId;
      }
    }
  }
  
  if (result.namResourceId.empty())
  {
    result.errorMessage = "No NAM resource found in amp_nam nodes";
    return result;
  }
  
  if (result.irResourceId.empty())
  {
    result.errorMessage = "No IR resource found in cab_ir nodes";
    return result;
  }
  
  // Look up NAM file path in library
  for (const auto& entry : modelsLibrary)
  {
    if (entry.value("id", "") == result.namResourceId)
    {
      result.namFilePath = resourcesDir / entry.value("filePath", "");
      break;
    }
  }
  
  // Look up IR file path in library
  for (const auto& entry : irLibrary)
  {
    if (entry.value("id", "") == result.irResourceId)
    {
      result.irFilePath = resourcesDir / entry.value("filePath", "");
      break;
    }
  }
  
  if (result.namFilePath.empty())
  {
    result.errorMessage = "NAM resource not found in library: " + result.namResourceId;
    return result;
  }
  
  if (result.irFilePath.empty())
  {
    result.errorMessage = "IR resource not found in library: " + result.irResourceId;
    return result;
  }
  
  if (!fs::exists(result.namFilePath))
  {
    result.errorMessage = "NAM file does not exist: " + result.namFilePath.string();
    return result;
  }
  
  if (!fs::exists(result.irFilePath))
  {
    result.errorMessage = "IR file does not exist: " + result.irFilePath.string();
    return result;
  }
  
  result.valid = true;
  return result;
}

} // namespace

// ============================================================================
// Main Test Runner
// ============================================================================

int main()
{
#ifndef NAMGUITAR_TEST_RESOURCES_DIR
#error "NAMGUITAR_TEST_RESOURCES_DIR must be defined"
#endif
  try
  {
    const fs::path resourcesDir = fs::path(NAMGUITAR_TEST_RESOURCES_DIR);
    const fs::path dataDir = resourcesDir / "ui" / "data";

    std::vector<std::string> errors;
    const auto recordError = [&errors](std::string message) {
      errors.push_back(std::move(message));
    };

    std::cout << "========================================================================\n";
    std::cout << "Preset DSP Processing Tests - Signal Graph Integration\n";
    std::cout << "========================================================================\n";
    std::cout << "Resources: " << resourcesDir.string() << "\n\n";

    // Load libraries
    const auto audioModelsJson = LoadJson(dataDir / "audiofx-models.json");
    const auto irLibraryJson = LoadJson(dataDir / "ir-library.json");
    const auto presetsJson = LoadJson(dataDir / "default-presets.json");

    if (!presetsJson.is_array())
    {
      throw std::runtime_error("default-presets.json is not an array");
    }

    // Parse all presets upfront
    std::vector<namguitar::Preset> presets;
    for (const auto& presetJson : presetsJson)
    {
      std::string jsonStr = presetJson.dump();
      auto presetOpt = namguitar::PresetStorage::DeserializeFromJson(jsonStr);
      if (presetOpt)
      {
        presets.push_back(*presetOpt);
      }
      else
      {
        std::string presetId = presetJson.value("id", "<unknown>");
        recordError("Failed to parse preset: " + presetId);
      }
    }

    std::cout << "Loaded " << presets.size() << " presets from JSON\n\n";

    // Create a single GraphDSPManager to reuse (tests preset switching)
    namguitar::GraphDSPManager dsp;
    
    // Populate the resource library from JSON files
    auto& library = dsp.GetResourceLibrary();
    
    // Load NAM models into library
    for (const auto& entry : audioModelsJson)
    {
      namguitar::LibraryResource resource;
      resource.type = "nam";
      resource.id = entry.value("id", "");
      resource.name = entry.value("title", entry.value("name", resource.id));
      resource.category = entry.value("category", "");
      resource.description = entry.value("description", "");
      resource.filePath = resourcesDir / entry.value("filePath", "");
      
      if (!resource.id.empty())
      {
        library.AddResource(resource);
      }
    }
    
    // Load IRs into library
    for (const auto& entry : irLibraryJson)
    {
      namguitar::LibraryResource resource;
      resource.type = "ir";
      resource.id = entry.value("id", "");
      resource.name = entry.value("title", entry.value("name", resource.id));
      resource.category = entry.value("category", "");
      resource.description = entry.value("description", "");
      resource.filePath = resourcesDir / entry.value("filePath", "");
      
      if (!resource.id.empty())
      {
        library.AddResource(resource);
      }
    }
    
    std::cout << "Resource library populated with " 
              << library.GetResourcesByType("nam").size() << " NAM models and "
              << library.GetResourcesByType("ir").size() << " IRs\n\n";
    
    dsp.Prepare(kTestSampleRate, kTestBlockSize);

    int presetsProcessed = 0;
    int presetsTested = 0;

    std::cout << "Testing DSP processing through signal graph...\n";
    std::cout << "------------------------------------------------------------------------\n";

    for (const auto& preset : presets)
    {
      ++presetsTested;
      std::cout << std::setw(3) << presetsTested << ". " << std::left << std::setw(25) << preset.name;

      // Validate graph structure
      auto graphValidation = ValidatePresetGraph(preset);
      if (!graphValidation.valid)
      {
        std::cout << " SKIP (graph: " << graphValidation.errorMessage << ")\n";
        recordError("Preset '" + preset.name + "': " + graphValidation.errorMessage);
        continue;
      }

      // Validate resources
      auto resourceValidation = ValidatePresetResources(preset, audioModelsJson, irLibraryJson, resourcesDir);
      if (!resourceValidation.valid)
      {
        std::cout << " SKIP (resource: " << resourceValidation.errorMessage << ")\n";
        recordError("Preset '" + preset.name + "': " + resourceValidation.errorMessage);
        continue;
      }

      // Reset DSP state before loading new preset
      dsp.Reset();

      // Load preset into GraphDSPManager
      if (!dsp.LoadPreset(preset))
      {
        std::cout << " FAIL (load failed)\n";
        recordError("Preset '" + preset.name + "': GraphDSPManager::LoadPreset failed");
        continue;
      }

      // Test 1: Basic processing test
      auto basicResult = TestGraphDSPProcessing(dsp, kTestBlockSize, kTestSampleRate);
      if (!basicResult.success)
      {
        std::cout << " FAIL (" << basicResult.errorMessage << ")\n";
        recordError("Preset '" + preset.name + "' processing: " + basicResult.errorMessage);
        continue;
      }

      // Test 2: Stability test (process multiple blocks)
      auto stabilityResult = TestGraphDSPStability(dsp, kTestBlockSize, kTestSampleRate, kStabilityBlocks);
      if (!stabilityResult.success)
      {
        std::cout << " FAIL (" << stabilityResult.errorMessage << ")\n";
        recordError("Preset '" + preset.name + "' stability: " + stabilityResult.errorMessage);
        continue;
      }

      ++presetsProcessed;
      std::cout << " OK (peak=" << std::fixed << std::setprecision(3) 
                << basicResult.outputAnalysis.peakValue 
                << ", rms=" << basicResult.outputAnalysis.rmsValue << ")\n";
    }

    std::cout << "------------------------------------------------------------------------\n";
    std::cout << "DSP Processing Results: " << presetsProcessed << "/" << presetsTested 
              << " presets processed successfully.\n\n";

    // Test preset switching if we have at least 2 presets
    if (presets.size() >= 2)
    {
      std::cout << "Testing preset switching...\n";
      
      auto switchResult = TestPresetSwitching(dsp, presets[0], presets[1], 
                                               kTestBlockSize, kTestSampleRate);
      if (switchResult.success)
      {
        std::cout << "  Preset switching: OK\n";
      }
      else
      {
        std::cout << "  Preset switching: FAIL (" << switchResult.errorMessage << ")\n";
        recordError("Preset switching: " + switchResult.errorMessage);
      }
    }

    std::cout << "\n========================================================================\n";

    if (!errors.empty())
    {
      std::cerr << "\nTest FAILED with " << errors.size() << " issue(s):\n";
      for (const auto& error : errors)
      {
        std::cerr << " - " << error << '\n';
      }
      return 1;
    }

    std::cout << "\nAll preset DSP processing tests PASSED.\n";
    return 0;
  }
  catch (const std::exception& ex)
  {
    std::cerr << "Fatal error: " << ex.what() << std::endl;
    return 1;
  }
}
