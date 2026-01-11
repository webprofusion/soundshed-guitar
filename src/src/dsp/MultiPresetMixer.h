#pragma once

#include "presets/PresetTypes.h"
#include "dsp/EffectProcessor.h"
#include "dsp/SignalGraphExecutor.h"
#include "resources/ResourceLibrary.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx
{
  /**
   * Runs multiple presets in parallel and mixes their outputs.
   * Supports per-preset mix level, mute/solo, and stereo panning.
   */
  class MultiPresetMixer
  {
  public:
    struct InstanceConfig
    {
      std::string id;   // Stable preset instance ID (e.g., "p1")
      std::string name; // Display name
      double mix = 1.0; // Linear gain [0.0, 1.0]
      bool mute = false;
      bool solo = false;
      double pan = 0.0; // [-1.0, 1.0] equal-power pan
    };

    MultiPresetMixer() = default;

    void SetResourceLibrary(ResourceLibrary *library) { mResourceLibrary = library; }

    // Add/Remove instances
    bool AddActivePreset(const Preset &preset, const std::string &presetId, const std::string &name);
    void RemoveActivePreset(const std::string &presetId);

    // Controls
    void SetPresetMix(const std::string &presetId, double value);
    void SetPresetPan(const std::string &presetId, double pan);
    void SetPresetMute(const std::string &presetId, bool mute);
    void SetPresetSolo(const std::string &presetId, bool solo);
    void SetMasterGain(double value) { mMasterGain = value; }
    void SetLimiterEnabled(bool enabled) { mLimiterEnabled = enabled; }

    // Global/parameter routing helpers
    void SetInputTrim(double dB);
    void SetOutputTrim(double dB);
    void SetGateEnabled(bool enabled);
    void SetGateThreshold(double value);
    void SetAmpDrive(double value);
    void SetSimpleCabEnabled(bool enabled);
    void SetSimpleCabBass(double value);
    void SetSimpleCabPresence(double value);
    void SetSimpleCabBrightness(double value);
    void SetIRQuality(double value);
    void SetEQEnabled(bool enabled);
    void SetEQBandGain(int band, double value);
    void SetEQBandFrequency(int band, double value);
    void SetEQBandQ(int band, double value);
    void SetDelayEnabled(bool enabled);
    void SetDelayTime(double value);
    void SetDelayFeedback(double value);
    void SetDelayMix(double value);
    void SetReverbEnabled(bool enabled);
    void SetReverbDecay(double value);
    void SetReverbDamping(double value);
    void SetReverbMix(double value);

    // Lifecycle
    void Prepare(double sampleRate, int maxBlockSize);
    void Reset();

    // Processing
    void Process(float **inputs, float **outputs, int numSamples);

    // Queries
    [[nodiscard]] std::vector<std::string> GetActivePresetIds() const;
    [[nodiscard]] std::vector<std::string> GetPresetNodeTypes(const std::string &presetId) const;
    [[nodiscard]] size_t GetPresetCount() const { return mInstances.size(); }

  private:
    struct PresetInstance
    {
      InstanceConfig cfg;
      SignalGraphExecutor executor;
      std::vector<float> outL;
      std::vector<float> outR;

      PresetInstance() = default;
      PresetInstance(PresetInstance &&) noexcept = default;
      PresetInstance &operator=(PresetInstance &&) noexcept = default;
      PresetInstance(const PresetInstance &) = delete;
      PresetInstance &operator=(const PresetInstance &) = delete;
    };

    [[nodiscard]] PresetInstance *FindInstance(const std::string &id);
    [[nodiscard]] const PresetInstance *FindInstance(const std::string &id) const;
    void AllocateBuffers(int maxBlockSize);
    static void ComputePanGains(double pan, float &gL, float &gR);

    ResourceLibrary *mResourceLibrary = nullptr;
    std::vector<PresetInstance> mInstances;

    double mSampleRate = 44100.0;
    int mMaxBlockSize = 512;
    bool mPrepared = false;
    double mMasterGain = 1.0; // applied post-sum
    bool mLimiterEnabled = false;
  };

} // namespace guitarfx
