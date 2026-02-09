#pragma once

/**
 * Optimized NAM DSP processor.
 *
 * This is a high-performance replacement for nam::DSP that uses SIMD-accelerated
 * activations and fused operations. It maintains API compatibility with the
 * original NAM library while providing significant performance improvements.
 *
 * Key optimizations:
 * - SIMD-vectorized activation functions (AVX/SSE)
 * - Fused gated activation kernels for WaveNet
 * - Reduced virtual dispatch overhead
 * - Better cache utilization through memory layout optimization
 */

#include "OptimizedWaveNet.h"
#include "OptimizedLSTM.h"
#include "SimdMath.h"
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <optional>
#include <variant>

// Include nlohmann JSON for parsing NAM files
#include "json.hpp"

namespace guitarfx
{
namespace nam
{

// ============================================================================
// NAM Model Metadata
// ============================================================================

struct ModelMetadata
{
  std::string name;
  std::string author;
  std::optional<double> inputLevel;     // dBu for 0 dBFS
  std::optional<double> outputLevel;    // dBu for 0 dBFS
  std::optional<double> loudness;       // dB
  double expectedSampleRate = -1.0;
};

// ============================================================================
// Architecture Type
// ============================================================================

enum class Architecture
{
  Unknown = 0,
  WaveNet,
  LSTM,
  ConvNet,
  Linear
};

inline Architecture ParseArchitecture(const std::string& arch)
{
  if (arch == "WaveNet" || arch == "CatWaveNet")
    return Architecture::WaveNet;
  if (arch == "LSTM" || arch == "CatLSTM")
    return Architecture::LSTM;
  if (arch == "ConvNet")
    return Architecture::ConvNet;
  if (arch == "Linear")
    return Architecture::Linear;
  return Architecture::Unknown;
}

// ============================================================================
// Optimized DSP Base
// ============================================================================

class OptimizedDSP
{
public:
  using SampleType = float;

  virtual ~OptimizedDSP() = default;

  virtual void Process(SampleType* input, SampleType* output, int numFrames) = 0;
  virtual void Reset(double sampleRate, int maxBufferSize) = 0;
  virtual void Prewarm() = 0;

  double GetExpectedSampleRate() const { return mMetadata.expectedSampleRate; }

  bool HasInputLevel() const { return mMetadata.inputLevel.has_value(); }
  double GetInputLevel() const { return mMetadata.inputLevel.value_or(0.0); }

  bool HasOutputLevel() const { return mMetadata.outputLevel.has_value(); }
  double GetOutputLevel() const { return mMetadata.outputLevel.value_or(0.0); }

  bool HasLoudness() const { return mMetadata.loudness.has_value(); }
  double GetLoudness() const { return mMetadata.loudness.value_or(0.0); }

  const ModelMetadata& GetMetadata() const { return mMetadata; }

protected:
  ModelMetadata mMetadata;
};

// ============================================================================
// Optimized WaveNet DSP Adapter
// ============================================================================

class OptimizedWaveNetDSP : public OptimizedDSP
{
public:
  OptimizedWaveNetDSP(
    const std::vector<LayerArrayParams>& layerArrayParams,
    float headScale,
    std::vector<float>& weights,
    double expectedSampleRate)
    : mWaveNet(layerArrayParams, headScale, weights, expectedSampleRate)
  {
    mMetadata.expectedSampleRate = expectedSampleRate;
  }

  void Process(SampleType* input, SampleType* output, int numFrames) override
  {
    mWaveNet.Process(input, output, numFrames);
  }

  void Reset(double sampleRate, int maxBufferSize) override
  {
    mWaveNet.Reset(sampleRate, maxBufferSize);
  }

  void Prewarm() override
  {
    mWaveNet.Prewarm();
  }

private:
  OptimizedWaveNet mWaveNet;
};

// ============================================================================
// Optimized LSTM DSP Adapter
// ============================================================================

class OptimizedLSTMDSP : public OptimizedDSP
{
public:
  OptimizedLSTMDSP(
    int numLayers,
    int inputSize,
    int hiddenSize,
    std::vector<float>& weights,
    double expectedSampleRate)
    : mLSTM(numLayers, inputSize, hiddenSize, weights, expectedSampleRate)
  {
    mMetadata.expectedSampleRate = expectedSampleRate;
  }

  void Process(SampleType* input, SampleType* output, int numFrames) override
  {
    mLSTM.Process(input, output, numFrames);
  }

  void Reset(double sampleRate, int maxBufferSize) override
  {
    mLSTM.Reset(sampleRate, maxBufferSize);
  }

  void Prewarm() override
  {
    mLSTM.Prewarm();
  }

private:
  OptimizedLSTM mLSTM;
};

// ============================================================================
// NAM Model Loader
// ============================================================================

/**
 * Load an optimized NAM model from a .nam file.
 *
 * This function parses the NAM file format and instantiates the appropriate
 * optimized processor (WaveNet or LSTM).
 *
 * @param modelPath Path to the .nam model file
 * @return Unique pointer to the optimized DSP, or nullptr on failure
 */
inline std::unique_ptr<OptimizedDSP> LoadOptimizedModel(const std::filesystem::path& modelPath)
{
  // Read file
  std::ifstream file(modelPath);
  if (!file.is_open())
    return nullptr;

  try
  {
    nlohmann::json j;
    file >> j;

    // Parse version
    std::string version = j.value("version", "");

    // Parse architecture
    std::string archStr = j.value("architecture", "");
    Architecture arch = ParseArchitecture(archStr);

    // Parse config
    auto config = j["config"];

    // Parse weights
    std::vector<float> weights;
    if (j.contains("weights"))
    {
      for (const auto& w : j["weights"])
        weights.push_back(w.get<float>());
    }

    // Parse expected sample rate
    double expectedSampleRate = -1.0;
    if (j.contains("metadata") && j["metadata"].contains("expected_sample_rate"))
    {
      expectedSampleRate = j["metadata"]["expected_sample_rate"].get<double>();
    }
    else if (config.contains("sample_rate"))
    {
      expectedSampleRate = config["sample_rate"].get<double>();
    }

    // Create optimized processor based on architecture
    std::unique_ptr<OptimizedDSP> dsp;

    switch (arch)
    {
      case Architecture::WaveNet:
      {
        std::vector<LayerArrayParams> layerArrayParams;

        for (const auto& layerConfig : config["layers"])
        {
          LayerArrayParams params;
          params.inputSize = layerConfig["input_size"];
          params.conditionSize = layerConfig["condition_size"];
          params.headSize = layerConfig["head_size"];
          params.channels = layerConfig["channels"];
          params.kernelSize = layerConfig["kernel_size"];
          params.activation = layerConfig.value("activation", "Tanh");
          params.gated = layerConfig.value("gated", false);
          params.headBias = layerConfig.value("head_bias", false);

          for (const auto& d : layerConfig["dilations"])
            params.dilations.push_back(d.get<int>());

          layerArrayParams.push_back(std::move(params));
        }

        float headScale = config.value("head_scale", 1.0f);

        dsp = std::make_unique<OptimizedWaveNetDSP>(
          layerArrayParams, headScale, weights, expectedSampleRate);
        break;
      }

      case Architecture::LSTM:
      {
        int numLayers = config["num_layers"];
        int inputSize = config["input_size"];
        int hiddenSize = config["hidden_size"];

        dsp = std::make_unique<OptimizedLSTMDSP>(
          numLayers, inputSize, hiddenSize, weights, expectedSampleRate);
        break;
      }

      default:
        // Unsupported architecture - fall back to original NAM library
        return nullptr;
    }

    // Parse and set metadata
    if (dsp && j.contains("metadata"))
    {
      auto& meta = j["metadata"];
      ModelMetadata& dspMeta = const_cast<ModelMetadata&>(dsp->GetMetadata());

      if (meta.contains("name"))
        dspMeta.name = meta["name"].get<std::string>();
      if (meta.contains("author"))
        dspMeta.author = meta["author"].get<std::string>();
      if (meta.contains("input_level"))
        dspMeta.inputLevel = meta["input_level"].get<double>();
      if (meta.contains("output_level"))
        dspMeta.outputLevel = meta["output_level"].get<double>();
      if (meta.contains("loudness"))
        dspMeta.loudness = meta["loudness"].get<double>();
    }

    return dsp;
  }
  catch (const std::exception& e)
  {
    // Failed to parse - return nullptr
    return nullptr;
  }
}

// ============================================================================
// Compatibility Wrapper
// ============================================================================

/**
 * Wrapper that provides nam::DSP-compatible interface for OptimizedDSP.
 * This allows drop-in replacement in existing code.
 */
class OptimizedDSPWrapper
{
public:
  using SampleType = float;

  OptimizedDSPWrapper(std::unique_ptr<OptimizedDSP> dsp)
    : mDSP(std::move(dsp))
  {
  }

  void process(SampleType* input, SampleType* output, int numFrames)
  {
    if (mDSP)
      mDSP->Process(input, output, numFrames);
    else
    {
      // Passthrough if no model
      for (int i = 0; i < numFrames; ++i)
        output[i] = input[i];
    }
  }

  void Reset(double sampleRate, int maxBufferSize)
  {
    if (mDSP)
      mDSP->Reset(sampleRate, maxBufferSize);
  }

  void prewarm()
  {
    if (mDSP)
      mDSP->Prewarm();
  }

  double GetExpectedSampleRate() const
  {
    return mDSP ? mDSP->GetExpectedSampleRate() : -1.0;
  }

  bool HasInputLevel() const { return mDSP && mDSP->HasInputLevel(); }
  double GetInputLevel() const { return mDSP ? mDSP->GetInputLevel() : 0.0; }

  bool HasOutputLevel() const { return mDSP && mDSP->HasOutputLevel(); }
  double GetOutputLevel() const { return mDSP ? mDSP->GetOutputLevel() : 0.0; }

  bool HasLoudness() const { return mDSP && mDSP->HasLoudness(); }
  double GetLoudness() const { return mDSP ? mDSP->GetLoudness() : 0.0; }

  bool IsValid() const { return mDSP != nullptr; }

  OptimizedDSP* GetDSP() { return mDSP.get(); }
  const OptimizedDSP* GetDSP() const { return mDSP.get(); }

private:
  std::unique_ptr<OptimizedDSP> mDSP;
};

/**
 * Load an optimized model and return a wrapper.
 * If the model architecture is not supported, returns an empty wrapper.
 */
inline std::unique_ptr<OptimizedDSPWrapper> LoadOptimizedModelWrapper(const std::filesystem::path& modelPath)
{
  auto dsp = LoadOptimizedModel(modelPath);
  if (dsp)
    return std::make_unique<OptimizedDSPWrapper>(std::move(dsp));
  return nullptr;
}

} // namespace nam
} // namespace guitarfx
