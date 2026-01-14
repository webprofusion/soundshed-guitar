#pragma once

#include "presets/PresetTypes.h"
#include "dsp/EffectProcessor.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/ParametricEQEffect.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace guitarfx
{
  class ResourceLibrary;

  /**
   * Central DSP manager that runs multiple presets in parallel and mixes their outputs.
   * Supports per-preset mix level, mute/solo, stereo panning, and per-preset global FX.
   * Also handles global input settings like auto-level and mono/stereo mode.
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

    // Tuner result data
    struct TunerResult
    {
      std::string noteName;       // e.g., "E", "A#/Bb"
      int octave = 0;             // Octave number (e.g., 2 for low E on guitar)
      double frequency = 0.0;     // Detected frequency in Hz
      double centOffset = 0.0;    // Cents deviation from perfect pitch (-50 to +50)
      double confidence = 0.0;    // Detection confidence (0.0 to 1.0)
      bool detected = false;      // Whether a valid pitch was detected
      double debugRms = 0.0;      // Debug: RMS of input signal
      double debugRawFreq = 0.0;  // Debug: Raw detected frequency before note mapping
    };

    using TunerCallback = std::function<void(const TunerResult &)>;

    MultiPresetMixer() = default;

    void SetResourceLibrary(ResourceLibrary *library) { mResourceLibrary = library; }
    [[nodiscard]] ResourceLibrary* GetResourceLibrary() const { return mResourceLibrary; }

    // Add/Remove instances
    bool AddActivePreset(const Preset &preset, const std::string &presetId, const std::string &name);
    void RemoveActivePreset(const std::string &presetId);

    // Per-preset mixing controls
    void SetPresetMix(const std::string &presetId, double value);
    void SetPresetPan(const std::string &presetId, double pan);
    void SetPresetMute(const std::string &presetId, bool mute);
    void SetPresetSolo(const std::string &presetId, bool solo);

    // Master/global controls
    void SetMasterGain(double value) { mMasterGain = value; }
    void SetLimiterEnabled(bool enabled) { mLimiterEnabled = enabled; }

    // Global input/output settings
    void SetAutoLevelInput(bool enabled) { mAutoLevelInput = enabled; }
    void SetAutoLevelOutput(bool enabled) { mAutoLevelOutput = enabled; }
    [[nodiscard]] bool GetAutoLevelInput() const { return mAutoLevelInput; }
    [[nodiscard]] bool GetAutoLevelOutput() const { return mAutoLevelOutput; }

    void SetMonoMode(bool mono) { mMonoMode = mono; }
    void SetInputChannel(int channel) { mInputChannel = std::clamp(channel, 0, 1); }
    [[nodiscard]] bool IsMonoMode() const { return mMonoMode; }
    [[nodiscard]] int GetInputChannel() const { return mInputChannel; }

    // Signal chain parameter routing (apply to all presets)
    void SetInputTrim(double dB);
    void SetOutputTrim(double dB);
    void SetAmpDrive(double value);
    void SetAmpTone(double value);
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

    // Global signal chain configuration
    void SetGlobalChainConfig(const GlobalSignalChainConfig& config);
    [[nodiscard]] const GlobalSignalChainConfig& GetGlobalChainConfig() const { return mGlobalChainConfig; }

    // Global pre-chain controls (noise gate, transpose)
    void SetGlobalGateEnabled(bool enabled);
    void SetGlobalGateThreshold(double thresholdDb);
    void SetGlobalGateAttack(double attackMs);
    void SetGlobalGateHold(double holdMs);
    void SetGlobalGateRelease(double releaseMs);
    void SetGlobalTransposeEnabled(bool enabled);
    void SetGlobalTranspose(int semitones);

    // Global post-chain controls (EQ, doubler)
    void SetGlobalEQEnabled(bool enabled);
    void SetGlobalEQBandGain(int band, double dB);
    void SetGlobalEQBandFrequency(int band, double freq);
    void SetGlobalEQBandQ(int band, double q);
    void SetGlobalDoublerEnabled(bool enabled);
    void SetGlobalDoublerDelay(double delayMs);
    void SetGlobalDoublerMix(double mix);
    void SetGlobalDoublerDetune(double cents);

    // Global input/output gain
    void SetGlobalInputGain(double dB);
    void SetGlobalOutputGain(double dB);

    // Legacy global FX methods (route to global chain for backward compatibility)
    void SetGateEnabled(bool enabled);
    void SetGateThreshold(double thresholdDb);
    void SetDoublerEnabled(bool enabled);
    void SetDoublerDelay(double delayMs);
    void SetTranspose(int semitones);
    // Node-level control (for signal chain editing)
    void SetNodeEnabled(const std::string &presetId, const std::string &nodeId, bool enabled);
    void SetNodeParam(const std::string &presetId, const std::string &nodeId, const std::string &key, double value);
    bool LoadNodeResource(const std::string &presetId, const std::string &nodeId, const ResourceRef &ref);

    // Lifecycle
    void Prepare(double sampleRate, int maxBlockSize);
    void Reset();

    // Processing
    void Process(float **inputs, float **outputs, int numSamples);

    // Queries
    [[nodiscard]] std::vector<std::string> GetActivePresetIds() const;
    [[nodiscard]] std::vector<std::string> GetPresetNodeTypes(const std::string &presetId) const;
    [[nodiscard]] size_t GetPresetCount() const { return mInstances.size(); }
    [[nodiscard]] SignalGraphExecutor::DSPPerformanceStats GetPerformanceStats() const;

    // Tuner functionality
    void SetTunerEnabled(bool enabled);
    [[nodiscard]] bool IsTunerEnabled() const noexcept { return mTunerEnabled; }
    void SetTunerCallback(TunerCallback callback);
    void SetTunerReferenceFrequency(double frequency);
    [[nodiscard]] double GetTunerReferenceFrequency() const noexcept { return mTunerReferenceFrequency; }
    void SetLiveTunerMode(bool enabled) { mLiveTunerMode = enabled; }
    [[nodiscard]] bool IsLiveTunerMode() const noexcept { return mLiveTunerMode; }

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
    void AllocateInstanceBuffers(PresetInstance &inst, int maxBlockSize);
    static void ComputePanGains(double pan, float &gL, float &gR);
    void RebuildGlobalChains();
    void EnsureGlobalChainsUpToDate();

    // Tuner processing (YIN-based pitch detection)
    void ProcessTuner(float **inputs, int numSamples);
    [[nodiscard]] double DetectPitch(const std::vector<double> &samples) const;
    [[nodiscard]] TunerResult FrequencyToNote(double frequency) const;

    ResourceLibrary *mResourceLibrary = nullptr;
    std::vector<PresetInstance> mInstances;

    double mSampleRate = 44100.0;
    int mMaxBlockSize = 512;
    bool mPrepared = false;
    double mMasterGain = 1.0;
    bool mLimiterEnabled = false;

    // Global settings
    bool mAutoLevelInput = false;
    bool mAutoLevelOutput = false;
    bool mMonoMode = false;
    int mInputChannel = 0; // 0=left, 1=right (for mono mode)

    // Auto-level gain state
    float mInputAutoLevelGain = 1.0f;
    float mOutputAutoLevelGain = 1.0f;

    // Temporary buffers for input processing
    std::vector<float> mTempInL, mTempInR;
    std::vector<float> mPreChainOutL, mPreChainOutR;
    std::vector<float> mPostChainOutL, mPostChainOutR;

    // Global signal chain configuration and executors
    GlobalSignalChainConfig mGlobalChainConfig;
    SignalGraphExecutor mPreChainExecutor;   // input → gate → transpose
    SignalGraphExecutor mPostChainExecutor;  // eq → doubler → output
    bool mGlobalChainNeedsRebuild = true;

    // Tuner state
    bool mTunerEnabled = false;
    bool mLiveTunerMode = true;  // When true, audio passes through DSP while tuning; when false, output is silent
    double mTunerReferenceFrequency = 440.0;  // A4 reference pitch
    TunerCallback mTunerCallback;
    std::vector<double> mTunerBuffer;         // Circular buffer for pitch detection
    std::size_t mTunerBufferWriteIndex = 0;
    std::size_t mTunerSampleCounter = 0;      // For throttling callback rate
    static constexpr std::size_t kTunerBufferSize = 4096;      // ~85ms at 48kHz for good low-frequency detection
    static constexpr std::size_t kTunerUpdateInterval = 2048;  // Update every ~42ms at 48kHz
  };

} // namespace guitarfx
