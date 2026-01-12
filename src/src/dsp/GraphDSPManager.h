#pragma once

#include "IPlugConstants.h"
#include "presets/PresetTypes.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/IRTypes.h"
#include "resources/ResourceLibrary.h"
#include <memory>
#include <filesystem>

namespace guitarfx
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
      // Register all built-in effects on first use
      RegisterAllEffects();
    }

    /**
     * Prepare for audio processing.
     * Must be called before Process().
     */
    void Prepare(double sampleRate, int maxBlockSize)
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      // Resize and zero-initialize float buffers
      mInputBufferL.resize(static_cast<size_t>(maxBlockSize), 0.0f);
      mInputBufferR.resize(static_cast<size_t>(maxBlockSize), 0.0f);
      mOutputBufferL.resize(static_cast<size_t>(maxBlockSize), 0.0f);
      mOutputBufferR.resize(static_cast<size_t>(maxBlockSize), 0.0f);

      // Clear buffers to ensure no garbage data
      std::fill(mInputBufferL.begin(), mInputBufferL.end(), 0.0f);
      std::fill(mInputBufferR.begin(), mInputBufferR.end(), 0.0f);
      std::fill(mOutputBufferL.begin(), mOutputBufferL.end(), 0.0f);
      std::fill(mOutputBufferR.begin(), mOutputBufferR.end(), 0.0f);

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
    bool LoadPreset(const Preset &preset)
    {
      mCurrentPreset = preset;

      // Apply global settings first
      mInputTrim = preset.global.inputTrim;
      mOutputTrim = preset.global.outputTrim;
      mOutputVolume = preset.global.outputVolume;
      mAutoLevelInput = preset.global.autoLevelInput;
      mAutoLevelOutput = preset.global.autoLevelOutput;

      // Create a new executor for the signal graph
      mExecutor = std::make_unique<SignalGraphExecutor>();

      // CRITICAL: Set resource library BEFORE setting graph, so resources can load during node creation
      mExecutor->SetResourceLibrary(mResourceLibrary.get());

      // Apply trim settings to executor
      mExecutor->SetInputTrim(mInputTrim);
      mExecutor->SetOutputTrim(mOutputTrim);

      mExecutor->SetGraph(preset.graph);

      ApplyAutoLevelSettingsToAmpNodes();

      // ResolveResources is not needed - SetGraph already loads resources during node creation
      // ResolveResources(preset);

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
    void Process(iplug::sample **inputs, iplug::sample **outputs, int numSamples)
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

      // Convert double to float (SignalGraphExecutor handles trim internally)
      for (int i = 0; i < numSamples; ++i)
      {
        mInputBufferL[i] = inputs[0] ? static_cast<float>(inputs[0][i]) : 0.0f;
        mInputBufferR[i] = inputs[1] ? static_cast<float>(inputs[1][i]) : 0.0f;
      }

      float *floatInputs[2] = {mInputBufferL.data(), mInputBufferR.data()};
      float *floatOutputs[2] = {mOutputBufferL.data(), mOutputBufferR.data()};

      // Process through the graph
      mExecutor->Process(floatInputs, floatOutputs, numSamples);

      // Apply output volume only (SignalGraphExecutor handles output trim)
      for (int i = 0; i < numSamples; ++i)
      {
        if (outputs[0])
          outputs[0][i] = static_cast<iplug::sample>(static_cast<double>(mOutputBufferL[i]) * mOutputVolume);
        if (outputs[1])
          outputs[1][i] = static_cast<iplug::sample>(static_cast<double>(mOutputBufferR[i]) * mOutputVolume);
      }
    }

    /**
     * Set a parameter on a specific node.
     * @param nodeId The ID of the node to modify
     * @param key The parameter key
     * @param value The new parameter value
     */
    void SetNodeParam(const std::string &nodeId, const std::string &key, double value)
    {
      if (mExecutor)
      {
        mExecutor->SetNodeParam(nodeId, key, value);
      }

      // Update the stored preset
      for (auto &node : mCurrentPreset.graph.nodes)
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
    void SetNodeEnabled(const std::string &nodeId, bool enabled)
    {
      if (mExecutor)
      {
        mExecutor->SetNodeEnabled(nodeId, enabled);
      }

      for (auto &node : mCurrentPreset.graph.nodes)
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
      if (mExecutor)
      {
        mExecutor->SetInputTrim(db);
      }
    }

    /**
     * Set global output trim.
     */
    void SetOutputTrim(double db)
    {
      mOutputTrim = db;
      mCurrentPreset.global.outputTrim = db;
      if (mExecutor)
      {
        mExecutor->SetOutputTrim(db);
      }
    }

    /**
     * Set output volume (linear 0.0-1.0).
     */
    void SetOutputVolume(double linear)
    {
      mOutputVolume = linear;
      mCurrentPreset.global.outputVolume = linear;
    }

    /** Enable or disable model-aware input auto-leveling for amp nodes. */
    void SetAutoLevelInput(bool enabled)
    {
      mAutoLevelInput = enabled;
      mCurrentPreset.global.autoLevelInput = enabled;
      ApplyAutoLevelSettingsToAmpNodes();
    }

    /** Enable or disable model-aware output auto-leveling for amp nodes. */
    void SetAutoLevelOutput(bool enabled)
    {
      mAutoLevelOutput = enabled;
      mCurrentPreset.global.autoLevelOutput = enabled;
      ApplyAutoLevelSettingsToAmpNodes();
    }

    [[nodiscard]] bool GetAutoLevelInput() const { return mAutoLevelInput; }
    [[nodiscard]] bool GetAutoLevelOutput() const { return mAutoLevelOutput; }

    /**
     * Get the current preset state.
     */
    [[nodiscard]] const Preset &GetCurrentPreset() const { return mCurrentPreset; }

    /**
     * Get the resource library for managing pre-defined resources.
     */
    [[nodiscard]] ResourceLibrary &GetResourceLibrary() { return *mResourceLibrary; }

    /**
     * Check if a preset is loaded.
     */
    [[nodiscard]] bool HasPreset() const { return mExecutor != nullptr; }

    // ======================================================================
    // Parameter wrapper methods for backward compatibility
    // These methods map plugin parameters to specific graph nodes
    // ======================================================================

    /** Set drive parameter on the first NAM amp node found */
    void SetDrive(double value)
    {
      auto nodeId = FindFirstNodeOfType("amp_nam");
      if (!nodeId.empty())
      {
        SetNodeParam(nodeId, "drive", value);
      }
    }

    /** Set tone parameter on the first NAM amp node found */
    void SetTone(double value)
    {
      auto nodeId = FindFirstNodeOfType("amp_nam");
      if (!nodeId.empty())
      {
        SetNodeParam(nodeId, "tone", value);
      }
    }

    /** Enable/disable the first noise gate node found */
    void SetGateEnabled(bool enabled)
    {
      auto nodeId = FindFirstNodeOfType("noise_gate");
      if (nodeId.empty())
        nodeId = FindFirstNodeOfType("dynamics_gate");
      if (!nodeId.empty())
      {
        SetNodeEnabled(nodeId, enabled);
      }
    }

    /** Set threshold on the first noise gate node found */
    void SetGateThreshold(double value)
    {
      auto nodeId = FindFirstNodeOfType("noise_gate");
      if (nodeId.empty())
        nodeId = FindFirstNodeOfType("dynamics_gate");
      if (!nodeId.empty())
      {
        SetNodeParam(nodeId, "threshold", value);
      }
    }

    /** Set mix parameter on the first node that supports it */
    void SetMix(double value)
    {
      // Mix is typically on delay/reverb effects
      auto nodeId = FindFirstNodeOfType("delay");
      if (nodeId.empty())
        nodeId = FindFirstNodeOfType("reverb");
      if (!nodeId.empty())
      {
        SetNodeParam(nodeId, "mix", value);
      }
    }

    /** Enable/disable doubler effect */
    void SetDoublerEnabled(bool enabled)
    {
      auto nodeId = FindFirstNodeOfType("doubler");
      if (!nodeId.empty())
      {
        SetNodeEnabled(nodeId, enabled);
      }
    }

    /** Set doubler delay time */
    void SetDoublerDelay(double value)
    {
      auto nodeId = FindFirstNodeOfType("doubler");
      if (!nodeId.empty())
      {
        SetNodeParam(nodeId, "delay", value);
      }
    }

    /** Set transpose parameter */
    void SetTranspose(int semitones)
    {
      auto nodeId = FindFirstNodeOfType("pitch_shifter");
      if (!nodeId.empty())
      {
        SetNodeParam(nodeId, "semitones", static_cast<double>(semitones));
      }
    }

    /** Enable/disable simple cab simulation */
    void SetSimpleCabEnabled(bool enabled)
    {
      auto nodeId = FindFirstNodeOfType("cab_simple");
      if (!nodeId.empty())
      {
        SetNodeEnabled(nodeId, enabled);
      }
    }

    /** Set simple cab bass parameter */
    void SetSimpleCabBass(double value)
    {
      auto nodeId = FindFirstNodeOfType("cab_simple");
      if (!nodeId.empty())
      {
        SetNodeParam(nodeId, "bass", value);
      }
    }

    /** Set simple cab presence parameter */
    void SetSimpleCabPresence(double value)
    {
      auto nodeId = FindFirstNodeOfType("cab_simple");
      if (!nodeId.empty())
      {
        SetNodeParam(nodeId, "presence", value);
      }
    }

    /** Set simple cab brightness parameter */
    void SetSimpleCabBrightness(double value)
    {
      auto nodeId = FindFirstNodeOfType("cab_simple");
      if (!nodeId.empty())
      {
        SetNodeParam(nodeId, "brightness", value);
      }
    }

    /** Set IR quality setting */
    void SetIRQuality(IRQuality quality)
    {
      auto nodeId = FindFirstNodeOfType("cab_ir");
      if (nodeId.empty())
        nodeId = FindFirstNodeOfType("ir_cab");
      if (!nodeId.empty())
      {
        // Map IRQuality enum to a parameter value
        int qualityValue = static_cast<int>(quality);
        SetNodeParam(nodeId, "quality", static_cast<double>(qualityValue));
      }
    }

    /** Enable/disable the first EQ node found */
    void SetEQEnabled(bool enabled)
    {
      auto nodeId = FindFirstNodeOfType("eq_parametric");
      if (nodeId.empty())
        nodeId = FindFirstNodeOfType("eq");
      if (!nodeId.empty())
      {
        SetNodeEnabled(nodeId, enabled);
      }
    }

    /** Set EQ band gain (band 0-3) */
    void SetEQBandGain(int band, double value)
    {
      auto nodeId = FindFirstNodeOfType("eq_parametric");
      if (nodeId.empty())
        nodeId = FindFirstNodeOfType("eq");
      if (!nodeId.empty())
      {
        const char *paramNames[] = {"lowGain", "lowMidGain", "highMidGain", "highGain"};
        if (band >= 0 && band < 4)
        {
          SetNodeParam(nodeId, paramNames[band], value);
        }
      }
    }

    /** Set EQ band frequency (band 0-3) */
    void SetEQBandFrequency(int band, double value)
    {
      auto nodeId = FindFirstNodeOfType("eq_parametric");
      if (nodeId.empty())
        nodeId = FindFirstNodeOfType("eq");
      if (!nodeId.empty())
      {
        const char *paramNames[] = {"lowFreq", "lowMidFreq", "highMidFreq", "highFreq"};
        if (band >= 0 && band < 4)
        {
          SetNodeParam(nodeId, paramNames[band], value);
        }
      }
    }

    /** Set EQ band Q (band 1-2 only, for parametric bands) */
    void SetEQBandQ(int band, double value)
    {
      auto nodeId = FindFirstNodeOfType("eq_parametric");
      if (nodeId.empty())
        nodeId = FindFirstNodeOfType("eq");
      if (!nodeId.empty())
      {
        const char *paramNames[] = {"", "lowMidQ", "highMidQ", ""};
        if (band >= 1 && band <= 2)
        {
          SetNodeParam(nodeId, paramNames[band], value);
        }
      }
    }

    /** Enable/disable the first delay node found */
    void SetDelayEnabled(bool enabled)
    {
      auto nodeId = FindFirstNodeOfType("delay");
      if (!nodeId.empty())
      {
        SetNodeEnabled(nodeId, enabled);
      }
    }

    /** Set delay time */
    void SetDelayTime(double value)
    {
      auto nodeId = FindFirstNodeOfType("delay");
      if (!nodeId.empty())
      {
        SetNodeParam(nodeId, "time", value);
      }
    }

    /** Set delay feedback */
    void SetDelayFeedback(double value)
    {
      auto nodeId = FindFirstNodeOfType("delay");
      if (!nodeId.empty())
      {
        SetNodeParam(nodeId, "feedback", value);
      }
    }

    /** Set delay mix */
    void SetDelayMix(double value)
    {
      auto nodeId = FindFirstNodeOfType("delay");
      if (!nodeId.empty())
      {
        SetNodeParam(nodeId, "mix", value);
      }
    }

    /** Enable/disable the first reverb node found */
    void SetReverbEnabled(bool enabled)
    {
      auto nodeId = FindFirstNodeOfType("reverb");
      if (!nodeId.empty())
      {
        SetNodeEnabled(nodeId, enabled);
      }
    }

    /** Set reverb decay */
    void SetReverbDecay(double value)
    {
      auto nodeId = FindFirstNodeOfType("reverb");
      if (!nodeId.empty())
      {
        SetNodeParam(nodeId, "decay", value);
      }
    }

    /** Set reverb damping */
    void SetReverbDamping(double value)
    {
      auto nodeId = FindFirstNodeOfType("reverb");
      if (!nodeId.empty())
      {
        SetNodeParam(nodeId, "damping", value);
      }
    }

    /** Set reverb mix */
    void SetReverbMix(double value)
    {
      auto nodeId = FindFirstNodeOfType("reverb");
      if (!nodeId.empty())
      {
        SetNodeParam(nodeId, "mix", value);
      }
    }

    /** Enable/disable the first amp node */
    void SetAmpEnabled(bool enabled)
    {
      auto nodeId = FindFirstNodeOfType("amp_nam");
      if (nodeId.empty())
        nodeId = FindFirstNodeOfType("nam_amp");
      if (!nodeId.empty())
      {
        SetNodeEnabled(nodeId, enabled);
      }
    }

    /** Check if first amp node is enabled */
    bool IsAmpEnabled() const
    {
      auto nodeId = FindFirstNodeOfType("amp_nam");
      if (nodeId.empty())
        nodeId = FindFirstNodeOfType("nam_amp");

      if (!nodeId.empty())
      {
        for (const auto &node : mCurrentPreset.graph.nodes)
        {
          if (node.id == nodeId)
          {
            return node.enabled;
          }
        }
      }
      return false;
    }

    /** Enable/disable the first cab node */
    void SetCabEnabled(bool enabled)
    {
      auto nodeId = FindFirstNodeOfType("cab_ir");
      if (nodeId.empty())
        nodeId = FindFirstNodeOfType("ir_cab");
      if (!nodeId.empty())
      {
        SetNodeEnabled(nodeId, enabled);
      }
    }

    /** Check if first cab node is enabled */
    bool IsCabEnabled() const
    {
      auto nodeId = FindFirstNodeOfType("cab_ir");
      if (nodeId.empty())
        nodeId = FindFirstNodeOfType("ir_cab");

      if (!nodeId.empty())
      {
        for (const auto &node : mCurrentPreset.graph.nodes)
        {
          if (node.id == nodeId)
          {
            return node.enabled;
          }
        }
      }
      return false;
    }

    /** Set mono/stereo processing mode */
    void SetMonoMode(bool mono)
    {
      // Store for future use - not implemented in V2 yet
      mMonoMode = mono;
    }

    /** Get mono/stereo mode */
    bool IsMonoMode() const { return mMonoMode; }

    /** Set input channel selection (0=L, 1=R) */
    void SetInputChannel(int channel)
    {
      mInputChannel = channel;
    }

    /** Get input channel selection */
    int GetInputChannel() const { return mInputChannel; }

    /** Enable/disable live tuner mode */
    void SetLiveTunerMode(bool enabled)
    {
      mLiveTunerMode = enabled;
    }

    /** Check if live tuner mode is active */
    bool IsLiveTunerMode() const { return mLiveTunerMode; }

    /** Set tuner enabled state */
    void SetTunerEnabled(bool enabled)
    {
      mTunerEnabled = enabled;
    }

    /** Set tuner reference frequency */
    void SetTunerReferenceFrequency(double freq)
    {
      mTunerReferenceFreq = freq;
    }

    /** Get tuner reference frequency */
    double GetTunerReferenceFrequency() const { return mTunerReferenceFreq; }

    /** Set tuner callback (legacy - not used in V2) */
    template <typename TCallback>
    void SetTunerCallback(TCallback &&callback)
    {
      // Tuner functionality not implemented in V2 graph architecture yet
      // This is a no-op for now
    }

    // Queries
    [[nodiscard]] bool IsValid() const { return mExecutor && mExecutor->IsValid(); }
    [[nodiscard]] SignalGraphExecutor::DSPPerformanceStats GetPerformanceStats() const
    {
      return mExecutor ? mExecutor->GetPerformanceStats() : SignalGraphExecutor::DSPPerformanceStats{};
    }

  private:
    /** Helper to find the first node of a given type in the current preset */
    std::string FindFirstNodeOfType(const std::string &type) const
    {
      for (const auto &node : mCurrentPreset.graph.nodes)
      {
        if (node.type == type)
        {
          return node.id;
        }
      }
      return "";
    }

    void ApplyAutoLevelSettingsToAmpNodes()
    {
      const std::string autoInput = mAutoLevelInput ? "1" : "0";
      const std::string autoOutput = mAutoLevelOutput ? "1" : "0";

      for (auto &node : mCurrentPreset.graph.nodes)
      {
        if (node.type == "amp_nam" || node.type == "nam_amp" || node.type == "nam")
        {
          node.config["autoLevelInput"] = autoInput;
          node.config["autoLevelOutput"] = autoOutput;

          if (mExecutor)
          {
            mExecutor->SetNodeConfig(node.id, "autoLevelInput", autoInput);
            mExecutor->SetNodeConfig(node.id, "autoLevelOutput", autoOutput);
          }
        }
      }
    }

    void ResolveResources(const Preset &preset)
    {
      for (const auto &node : preset.graph.nodes)
      {
        if (!node.resource.has_value())
          continue;

        const auto &ref = *node.resource;
        std::filesystem::path resourcePath;

        if (ref.IsEmbedded())
        {
          // Look up embedded resource
          for (const auto &embedded : preset.embeddedResources)
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
    double mOutputVolume = 1.0;
    bool mAutoLevelInput = false;
    bool mAutoLevelOutput = false;

    // Float buffers for SignalGraphExecutor
    std::vector<float> mInputBufferL;
    std::vector<float> mInputBufferR;
    std::vector<float> mOutputBufferL;
    std::vector<float> mOutputBufferR;

    // Legacy settings for backward compatibility
    bool mMonoMode = false;
    int mInputChannel = 0;
    bool mLiveTunerMode = false;
    bool mTunerEnabled = false;
    double mTunerReferenceFreq = 440.0;
  };

} // namespace guitarfx
