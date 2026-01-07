#pragma once

#include "presets/PresetTypesV2.h"
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
    bool LoadPreset(const PresetV2& preset)
    {
      mCurrentPreset = preset;

      // Create a new executor for the signal graph
      mExecutor = std::make_unique<SignalGraphExecutor>(preset.graph);

      // Apply global settings
      mInputTrim = preset.globals.inputTrim;
      mOutputTrim = preset.globals.outputTrim;
      mMasterVolume = preset.globals.masterVolume;

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
     * Bypass or enable a node.
     */
    void SetNodeBypassed(const std::string& nodeId, bool bypassed)
    {
      if (mExecutor)
      {
        mExecutor->SetNodeBypassed(nodeId, bypassed);
      }

      for (auto& node : mCurrentPreset.graph.nodes)
      {
        if (node.id == nodeId)
        {
          node.bypassed = bypassed;
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
      mCurrentPreset.globals.inputTrim = db;
    }

    /**
     * Set global output trim.
     */
    void SetOutputTrim(double db)
    {
      mOutputTrim = db;
      mCurrentPreset.globals.outputTrim = db;
    }

    /**
     * Set master volume.
     */
    void SetMasterVolume(double linear)
    {
      mMasterVolume = linear;
      mCurrentPreset.globals.masterVolume = linear;
    }

    /**
     * Get the current preset state.
     */
    [[nodiscard]] const PresetV2& GetCurrentPreset() const { return mCurrentPreset; }

    /**
     * Get the resource library for managing pre-defined resources.
     */
    [[nodiscard]] ResourceLibrary& GetResourceLibrary() { return *mResourceLibrary; }

    /**
     * Check if a preset is loaded.
     */
    [[nodiscard]] bool HasPreset() const { return mExecutor != nullptr; }

  private:
    void ResolveResources(const PresetV2& preset)
    {
      for (const auto& node : preset.graph.nodes)
      {
        if (!node.resource.isSet())
          continue;

        std::filesystem::path resourcePath;

        if (!node.resource.embeddedId.empty())
        {
          // Look up embedded resource
          for (const auto& embedded : preset.embeddedResources)
          {
            if (embedded.id == node.resource.embeddedId)
            {
              // TODO: Materialize embedded resource to temp file
              // For now, skip embedded resources
              break;
            }
          }
        }
        else if (!node.resource.filePath.empty())
        {
          // Direct file path
          resourcePath = node.resource.filePath;
        }
        else if (!node.resource.id.empty())
        {
          // Library reference
          auto resolved = mResourceLibrary->ResolveResource(node.resource.type, node.resource.id);
          if (resolved)
          {
            resourcePath = *resolved;
          }
        }

        if (!resourcePath.empty() && std::filesystem::exists(resourcePath))
        {
          mExecutor->LoadNodeResource(node.id, resourcePath);
        }
      }
    }

    std::unique_ptr<SignalGraphExecutor> mExecutor;
    std::unique_ptr<ResourceLibrary> mResourceLibrary;
    PresetV2 mCurrentPreset;

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
