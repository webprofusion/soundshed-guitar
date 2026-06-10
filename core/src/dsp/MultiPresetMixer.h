#pragma once

#include "presets/PresetTypes.h"
#include "dsp/EffectProcessor.h"
#include "dsp/SignalGraphExecutor.h"
#include "dsp/effects/ParametricEQEffect.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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

    struct SignalLevelStats
    {
      double peak = 0.0;
      double rms = 0.0;
      int clipCount = 0;
    };

    struct NodeSignalLevel
    {
      std::string scope; // pre, post, preset
      std::string presetId;
      std::string nodeId;
      std::string nodeType;
      int channelCount = 0;
      SignalLevelStats levels;
    };

    struct SignalDiagnosticsSnapshot
    {
      SignalLevelStats rawInput; // Before any gain/trim/mono processing
      SignalLevelStats input;
      SignalLevelStats output;
      std::vector<NodeSignalLevel> nodes;
    };

    using TunerCallback = std::function<void(const TunerResult &)>;

    MultiPresetMixer() = default;
    ~MultiPresetMixer();
    MultiPresetMixer(const MultiPresetMixer &) = delete;
    MultiPresetMixer &operator=(const MultiPresetMixer &) = delete;
    MultiPresetMixer(MultiPresetMixer &&other) noexcept;
    MultiPresetMixer &operator=(MultiPresetMixer &&other) noexcept;

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
    [[nodiscard]] double GetMasterGain() const { return mMasterGain; }
    [[nodiscard]] bool IsLimiterEnabled() const { return mLimiterEnabled; }
    void SetMultiThreadedProcessingEnabled(bool enabled);
    [[nodiscard]] bool IsMultiThreadedProcessingEnabled() const noexcept
    {
      return mMultiThreadedProcessingEnabled.load(std::memory_order_acquire);
    }

    // Global input/output settings
    void SetAutoLevelInput(bool enabled) { mAutoLevelInput = enabled; }
    void SetAutoLevelOutput(bool enabled) { mAutoLevelOutput = enabled; }
    [[nodiscard]] bool GetAutoLevelInput() const { return mAutoLevelInput; }
    [[nodiscard]] bool GetAutoLevelOutput() const { return mAutoLevelOutput; }

    void SetUserInputCalibrationGainDb(double dB);
    [[nodiscard]] double GetUserInputCalibrationGainDb() const { return mUserInputCalibrationGainDb; }

    void SetMonoMode(bool mono) { mMonoMode = mHostControlledInput ? false : mono; }
    void SetInputChannel(int channel) { mInputChannel = std::clamp(channel, 0, 1); }
    [[nodiscard]] bool IsMonoMode() const { return mMonoMode; }
    [[nodiscard]] int GetInputChannel() const { return mInputChannel; }

    // When hosted in a DAW the host owns the input configuration: mono
    // folding/channel selection is disabled and the input is used as provided.
    void SetHostControlledInput(bool hostControlled)
    {
      mHostControlledInput = hostControlled;
      if (hostControlled)
        mMonoMode = false;
    }
    [[nodiscard]] bool IsHostControlledInput() const { return mHostControlledInput; }

    // Signal chain parameter routing (apply to all presets)
    void SetInputTrim(double dB);
    void SetOutputTrim(double dB);
    void SetAmpDrive(double value);
    void SetAmpTone(double value);
    void SetIRQuality(double value);
    void SetEQEnabled(bool enabled);
    void SetEQBandGain(int band, double value);
    void SetEQBandFrequency(int band, double value);
    void SetEQBandQ(int band, double value);

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
    void SetNodeConfig(const std::string &presetId, const std::string &nodeId, const std::string &key, const std::string &value);
    void SetNodeConfigForType(const std::string &type, const std::string &key, const std::string &value);
    [[nodiscard]] std::string GetNodeConfig(const std::string &presetId, const std::string &nodeId, const std::string &key) const;
    [[nodiscard]] EffectProcessor *GetNodeProcessor(const std::string &presetId, const std::string &nodeId);
    [[nodiscard]] const EffectProcessor *GetNodeProcessor(const std::string &presetId, const std::string &nodeId) const;
    bool LoadNodeResource(const std::string &presetId, const std::string &nodeId, const ResourceRef &ref);

    // Push the current tempo (BPM) to all tempo-aware nodes in every preset and global chain.
    // Call once per audio block before Process().
    void SetTempo(double bpm);

    // Lifecycle
    void Prepare(double sampleRate, int maxBlockSize);
    void Reset();

    // Processing
    void Process(float **inputs, float **outputs, int numSamples);

    // Queries
    [[nodiscard]] std::vector<std::string> GetActivePresetIds() const;
    [[nodiscard]] std::vector<std::string> GetPresetNodeTypes(const std::string &presetId) const;
    [[nodiscard]] std::optional<InstanceConfig> GetPresetConfig(const std::string &presetId) const;
    [[nodiscard]] size_t GetPresetCount() const { return mInstances.size(); }
    [[nodiscard]] SignalGraphExecutor::DSPPerformanceStats GetPerformanceStats() const;
    /// Total algorithmic latency in samples: pre-chain + max(preset instances) + post-chain.
    [[nodiscard]] int GetTotalLatencySamples() const;

    // Signal diagnostics
    void SetSignalDiagnosticsEnabled(bool enabled);
    [[nodiscard]] bool IsSignalDiagnosticsEnabled() const noexcept { return mSignalDiagnosticsEnabled.load(std::memory_order_acquire); }
    [[nodiscard]] SignalDiagnosticsSnapshot GetSignalDiagnosticsSnapshot() const;

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
      int complexityScore = 1;

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
    std::atomic<bool> mMultiThreadedProcessingEnabled{true};

    // Global settings
    bool mAutoLevelInput = false;
    bool mAutoLevelOutput = false;
    double mUserInputCalibrationGainDb = 0.0;
    float mUserInputCalibrationGainLinear = 1.0f;
    bool mMonoMode = false;
    int mInputChannel = 0; // 0=left, 1=right (for mono mode)
    bool mHostControlledInput = false; // true when a DAW host owns the input config

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
    std::atomic<bool> mGlobalChainNeedsRebuild{true};

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

    struct AtomicLevelStats
    {
      std::atomic<double> peak{0.0};
      std::atomic<double> rms{0.0};
      std::atomic<int> clipCount{0};
    };

    std::atomic<bool> mSignalDiagnosticsEnabled{true};
    AtomicLevelStats mRawInputLevels;
    AtomicLevelStats mInputLevels;
    AtomicLevelStats mOutputLevels;

    // ---- Parallel preset processing -----------------------------------------------
    static constexpr int kMaxParallelWorkers = 7;
    static constexpr int kMaxWorkItems = 16;

    struct ParallelWorkItem
    {
      PresetInstance *inst = nullptr;
      float *preChainOutL  = nullptr;
      float *preChainOutR  = nullptr;
      int numSamples       = 0;
    };

    std::array<ParallelWorkItem, kMaxWorkItems> mWorkItems{};
    std::atomic<int>      mParallelTaskHead{0};
    std::atomic<int>      mParallelTaskCount{0};
    std::atomic<int>      mParallelDoneCount{0};
    std::atomic<uint32_t> mParallelGeneration{0};
    std::atomic<bool>     mParallelQuit{false};
    std::mutex              mParallelMutex;
    std::condition_variable mParallelCv;
    std::vector<std::thread> mWorkerThreads;

    void StartWorkers(int count);
    void StopWorkers();
    void WorkerLoop();
  };

} // namespace guitarfx
