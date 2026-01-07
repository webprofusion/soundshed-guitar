#pragma once

#include "presets/PresetTypes.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/EffectRegistry.h"
#include "resources/ResourceLibrary.h"
#include <memory>
#include <filesystem>

namespace namguitar
{
  /**
   * DSP Manager for v2 presets using the new signal graph architecture.
   * 
   * This class manages the signal graph executor and handles:
   * - Loading presets (with automatic migration from v1 if needed)
   * - Resource resolution via ResourceLibrary
   * - Parameter changes from the UI
   * - Audio processing via SignalGraphExecutor
   */
  class GraphDSPManager
  {
  public:
    GraphDSPManager()
      : mResourceLibrary(std::make_unique<ResourceLibrary>())
    {
    }

    /**
     * Prepare for audio processing.
     * Must be called before Process().
     */
    void Prepare(double sampleRate, int maxBlockSize)
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      // Resize float buffers
      mInputBufferL.resize(static_cast<size_t>(maxBlockSize));
      mInputBufferR.resize(static_cast<size_t>(maxBlockSize));
      mOutputBufferL.resize(static_cast<size_t>(maxBlockSize));
      mOutputBufferR.resize(static_cast<size_t>(maxBlockSize));

      if (mExecutor)
      {
        mExecutor->Prepare(sampleRate, maxBlockSize);
      }
    }

    /**
     * Reset DSP state (e.g., when transport stops).
     */
    void Reset()
    {
      if (mExecutor)
      {
        mExecutor->Reset();
      }
    }

    /**
     * Load a v2 preset.
     * @param preset The preset to load
     * @return true if the preset was loaded successfully
     */
    bool LoadPreset(const Preset& preset)
    {
      mCurrentPreset = preset;

      // Create a new executor for the signal graph
      mExecutor = std::make_unique<SignalGraphExecutor>();
      mExecutor->SetGraph(preset.graph);
      mExecutor->SetResourceLibrary(mResourceLibrary.get());

      // Apply global settings
      mInputTrim = preset.global.inputTrim;
      mOutputTrim = preset.global.outputTrim;
      mMasterVolume = 1.0; // Master volume not in GlobalSettings

      // Resolve resources for all nodes
      ResolveResources(preset);

      // Prepare the executor
      if (mSampleRate > 0 && mMaxBlockSize > 0)
      {
        mExecutor->Prepare(mSampleRate, mMaxBlockSize);
      }

      return true;
    }

    /**
     * Process audio through the signal graph.
     * @param inputs Input buffer array [L, R]
     * @param outputs Output buffer array [L, R]
     * @param numSamples Number of samples to process
     */
    void Process(double** inputs, double** outputs, int numSamples)
    {
      if (!mExecutor)
      {
        // No graph loaded - pass through
        if (outputs[0] && inputs[0])
          std::copy_n(inputs[0], numSamples, outputs[0]);
        if (outputs[1] && inputs[1])
          std::copy_n(inputs[1], numSamples, outputs[1]);
        return;
      }

      // Apply input trim
      double inputGain = std::pow(10.0, mInputTrim / 20.0);
      for (int i = 0; i < numSamples; ++i)
      {
        mInputBufferL[i] = inputs[0] ? static_cast<float>(inputs[0][i] * inputGain) : 0.0f;
        mInputBufferR[i] = inputs[1] ? static_cast<float>(inputs[1][i] * inputGain) : 0.0f;
      }

      float* floatInputs[2] = { mInputBufferL.data(), mInputBufferR.data() };
      float* floatOutputs[2] = { mOutputBufferL.data(), mOutputBufferR.data() };

      // Process through the graph
      mExecutor->Process(floatInputs, floatOutputs, numSamples);

      // Apply output trim and master volume
      double outputGain = std::pow(10.0, mOutputTrim / 20.0) * mMasterVolume;
      for (int i = 0; i < numSamples; ++i)
      {
        if (outputs[0])
          outputs[0][i] = static_cast<double>(mOutputBufferL[i]) * outputGain;
        if (outputs[1])
          outputs[1][i] = static_cast<double>(mOutputBufferR[i]) * outputGain;
      }
    }

    /**
     * Set a parameter on a specific node.
     * @param nodeId The ID of the node to modify
     * @param key The parameter key
     * @param value The new parameter value
     */
    void SetNodeParam(const std::string& nodeId, const std::string& key, double value)
    {
      if (mExecutor)
      {
        mExecutor->SetNodeParam(nodeId, key, value);
      }

      // Update the stored preset
      for (auto& node : mCurrentPreset.graph.nodes)
      {
        if (node.id == nodeId)
        {
          node.params[key] = value;
          break;
        }
      }
    }

    /**
     * Enable or disable a node.
     */
    void SetNodeEnabled(const std::string& nodeId, bool enabled)
    {
      if (mExecutor)
      {
        mExecutor->SetNodeEnabled(nodeId, enabled);
      }

      for (auto& node : mCurrentPreset.graph.nodes)
      {
        if (node.id == nodeId)
        {
          node.enabled = enabled;
          break;
        }
      }
    }

    /**
     * Set global input trim.
     */
    void SetInputTrim(double db)
    {
      mInputTrim = db;
      mCurrentPreset.global.inputTrim = db;
    }

    /**
     * Set global output trim.
     */
    void SetOutputTrim(double db)
    {
      mOutputTrim = db;
      mCurrentPreset.global.outputTrim = db;
    }

    /**
     * Set master volume.
     */
    void SetMasterVolume(double linear)
    {
      mMasterVolume = linear;
      // Note: masterVolume is not in GlobalSettings, tracked separately
    }

    /**
     * Get the current preset state.
     */
    [[nodiscard]] const Preset& GetCurrentPreset() const { return mCurrentPreset; }

    /**
     * Get the resource library for managing pre-defined resources.
     */
    [[nodiscard]] ResourceLibrary& GetResourceLibrary() { return *mResourceLibrary; }

    /**
     * Check if a preset is loaded.
     */
    [[nodiscard]] bool HasPreset() const { return mExecutor != nullptr; }

  private:
    void ResolveResources(const Preset& preset)
    {
      for (const auto& node : preset.graph.nodes)
      {
        if (!node.resource.has_value())
          continue;

        const auto& ref = *node.resource;
        std::filesystem::path resourcePath;

        if (ref.IsEmbedded())
        {
          // Look up embedded resource
          for (const auto& embedded : preset.embeddedResources)
          {
            if (embedded.id == ref.embeddedId)
            {
              // TODO: Materialize embedded resource to temp file
              // For now, skip embedded resources
              break;
            }
          }
        }
        else if (ref.IsFilePath())
        {
          // Direct file path
          resourcePath = ref.filePath;
        }
        else if (ref.IsLibraryRef())
        {
          // Library reference
          auto resolved = mResourceLibrary->ResolveResource(ref);
          if (resolved)
          {
            resourcePath = *resolved;
          }
        }

        if (!resourcePath.empty() && std::filesystem::exists(resourcePath))
        {
          mExecutor->LoadNodeResource(node.id, ref);
        }
      }
    }

    std::unique_ptr<SignalGraphExecutor> mExecutor;
    std::unique_ptr<ResourceLibrary> mResourceLibrary;
    Preset mCurrentPreset;

    double mSampleRate = 0.0;
    int mMaxBlockSize = 0;

    double mInputTrim = 0.0;
    double mOutputTrim = 0.0;
    double mMasterVolume = 1.0;

    // Float buffers for SignalGraphExecutor
    std::vector<float> mInputBufferL;
    std::vector<float> mInputBufferR;
    std::vector<float> mOutputBufferL;
    std::vector<float> mOutputBufferR;
  };

} // namespace namguitar
