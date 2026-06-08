#include "dsp/MultiPresetMixer.h"
#include "dsp/LevelTargets.h"
#include "dsp/EffectGuids.h"
#include "resources/ResourceLibrary.h"

#include <array>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string_view>

namespace guitarfx
{
  namespace
  {
    constexpr float kInputAutoLevelTargetPeak = 0.7f;
    constexpr float kInputAutoLevelMaxGain = 4.0f;
    constexpr float kAutoLevelAttackMix = 0.01f;
    constexpr float kAutoLevelReleaseMultiplier = 1.0001f;

    bool GraphHasNodeType(const SignalGraph& graph, const std::string& type)
    {
      for (const auto& node : graph.nodes)
      {
        if (node.type == type)
        {
          return true;
        }
      }
      return false;
    }

    GraphNode* FindNodeByIdOrType(SignalGraph& graph, const std::string& id, const std::string& type)
    {
      if (auto* node = graph.FindNode(id))
      {
        return node;
      }

      for (auto& node : graph.nodes)
      {
        if (node.type == type)
        {
          return &node;
        }
      }

      return nullptr;
    }

    std::string FindFirstNamNodeId(SignalGraphExecutor& executor)
    {
      for (const auto* effectType : {
             EffectGuids::kAmpNamOptimized,
             EffectGuids::kAmpNamBlend,
             EffectGuids::kFxNam,
             EffectGuids::kAmpNam})
      {
        const auto nodeId = executor.FindFirstNodeOfType(effectType);
        if (!nodeId.empty())
          return nodeId;
      }
      return {};
    }

    // Note names for pitch detection
    constexpr std::array<const char *, 12> kNoteNames = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

    // Alternative note names (flats)
    constexpr std::array<const char *, 12> kNoteNamesFlat = {
        "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};

    MultiPresetMixer::SignalLevelStats ComputeLevelStats(const float *left, const float *right, int numSamples)
    {
      MultiPresetMixer::SignalLevelStats stats;
      if (numSamples <= 0)
      {
        return stats;
      }

      double sumSquares = 0.0;
      std::size_t sampleCount = 0;

      if (left)
      {
        for (int i = 0; i < numSamples; ++i)
        {
          const float value = left[i];
          const float absValue = std::abs(value);
          stats.peak = std::max(stats.peak, static_cast<double>(absValue));
          sumSquares += static_cast<double>(value) * static_cast<double>(value);
          if (absValue > 1.0f)
          {
            stats.clipCount++;
          }
        }
        sampleCount += static_cast<std::size_t>(numSamples);
      }

      if (right)
      {
        for (int i = 0; i < numSamples; ++i)
        {
          const float value = right[i];
          const float absValue = std::abs(value);
          stats.peak = std::max(stats.peak, static_cast<double>(absValue));
          sumSquares += static_cast<double>(value) * static_cast<double>(value);
          if (absValue > 1.0f)
          {
            stats.clipCount++;
          }
        }
        sampleCount += static_cast<std::size_t>(numSamples);
      }

      if (sampleCount > 0)
      {
        stats.rms = std::sqrt(sumSquares / static_cast<double>(sampleCount));
      }

      return stats;
    }

    static inline void CpuRelax() noexcept
    {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
      _mm_pause();
#elif defined(__GNUC__) || defined(__clang__)
  #if defined(__x86_64__) || defined(__i386__)
      __asm volatile("pause" ::: "memory");
  #elif defined(__aarch64__) || defined(__arm__)
      __asm volatile("yield" ::: "memory");
  #endif
#endif
    }

    int ScoreNodeTypeForParallelWork(std::string_view type)
    {
      // Heuristic weights for per-node CPU cost in realtime processing.
      if (type == EffectGuids::kAmpNam || type == EffectGuids::kAmpNamOptimized || type == EffectGuids::kAmpNamBlend || type == EffectGuids::kFxNam)
        return 14;
      if (type == EffectGuids::kCabIr || type == EffectGuids::kReverbIr)
        return 12;
      if (type == EffectGuids::kReverbAdvanced || type == EffectGuids::kReverbAmbient || type == EffectGuids::kReverbRoom || type == EffectGuids::kReverbSpring)
        return 6;
      if (type == EffectGuids::kDelayDigital || type == EffectGuids::kDelayDoubler || type == EffectGuids::kEqParametric)
        return 3;
      if (type == EffectGuids::kGain)
        return 1;
      return 2;
    }

    int EstimateGraphComplexityScore(const std::vector<std::string>& nodeTypes)
    {
      int score = 0;
      for (const auto& type : nodeTypes)
        score += ScoreNodeTypeForParallelWork(type);
      return std::max(1, score);
    }

    bool ShouldUseParallelPresetDispatch(bool multiThreadingEnabled,
                                         int activeCount,
                                         int totalWorkUnits,
                                         bool workersAvailable)
    {
      if (!multiThreadingEnabled || !workersAvailable)
        return false;
      if (activeCount < 2)
        return false;

      // Avoid parallel fan-out for tiny blocks/light chains where scheduling cost dominates.
      constexpr int kMinParallelWorkUnits = 9000;
      return activeCount >= 3 || totalWorkUnits >= kMinParallelWorkUnits;
    }
  } // namespace
  bool MultiPresetMixer::AddActivePreset(const Preset &preset, const std::string &presetId, const std::string &name)
  {
    // Avoid duplicate IDs
    for (const auto &inst : mInstances)
    {
      if (inst.cfg.id == presetId)
        return false;
    }

    PresetInstance inst;
    inst.cfg.id = presetId;
    inst.cfg.name = name;

    Preset normalizedPreset = preset;
    EnsurePresetBoundaryGainNodes(normalizedPreset);

    inst.executor.SetResourceLibrary(mResourceLibrary);
    inst.executor.SetGraph(normalizedPreset.graph);
    inst.executor.SetSignalDiagnosticsEnabled(mSignalDiagnosticsEnabled.load(std::memory_order_acquire));
    inst.complexityScore = EstimateGraphComplexityScore(inst.executor.GetNodeTypes());

    if (mPrepared)
    {
      inst.executor.Prepare(mSampleRate, mMaxBlockSize);
      AllocateInstanceBuffers(inst, mMaxBlockSize);
    }

    inst.outL.resize(static_cast<size_t>(mMaxBlockSize), 0.0f);
    inst.outR.resize(static_cast<size_t>(mMaxBlockSize), 0.0f);

    mInstances.push_back(std::move(inst));
    return true;
  }

  MultiPresetMixer::MultiPresetMixer(MultiPresetMixer &&other) noexcept
  {
    *this = std::move(other);
  }

  MultiPresetMixer &MultiPresetMixer::operator=(MultiPresetMixer &&other) noexcept
  {
    if (this == &other)
    {
      return *this;
    }

    mResourceLibrary = other.mResourceLibrary;
    mInstances = std::move(other.mInstances);
    mSampleRate = other.mSampleRate;
    mMaxBlockSize = other.mMaxBlockSize;
    mPrepared = other.mPrepared;
    mMasterGain = other.mMasterGain;
    mLimiterEnabled = other.mLimiterEnabled;
    mAutoLevelInput = other.mAutoLevelInput;
    mAutoLevelOutput = other.mAutoLevelOutput;
    mUserInputCalibrationGainDb = other.mUserInputCalibrationGainDb;
    mUserInputCalibrationGainLinear = other.mUserInputCalibrationGainLinear;
    mMonoMode = other.mMonoMode;
    mInputChannel = other.mInputChannel;
    mInputAutoLevelGain = other.mInputAutoLevelGain;
    mOutputAutoLevelGain = other.mOutputAutoLevelGain;
    mTempInL = std::move(other.mTempInL);
    mTempInR = std::move(other.mTempInR);
    mPreChainOutL = std::move(other.mPreChainOutL);
    mPreChainOutR = std::move(other.mPreChainOutR);
    mPostChainOutL = std::move(other.mPostChainOutL);
    mPostChainOutR = std::move(other.mPostChainOutR);
    mGlobalChainConfig = std::move(other.mGlobalChainConfig);
    mPreChainExecutor = std::move(other.mPreChainExecutor);
    mPostChainExecutor = std::move(other.mPostChainExecutor);
    mGlobalChainNeedsRebuild.store(other.mGlobalChainNeedsRebuild.load(std::memory_order_acquire), std::memory_order_release);
    mTunerEnabled = other.mTunerEnabled;
    mLiveTunerMode = other.mLiveTunerMode;
    mTunerReferenceFrequency = other.mTunerReferenceFrequency;
    mTunerCallback = std::move(other.mTunerCallback);
    mTunerBuffer = std::move(other.mTunerBuffer);
    mTunerBufferWriteIndex = other.mTunerBufferWriteIndex;
    mTunerSampleCounter = other.mTunerSampleCounter;

    mSignalDiagnosticsEnabled.store(other.mSignalDiagnosticsEnabled.load(std::memory_order_acquire), std::memory_order_release);
    mRawInputLevels.peak.store(other.mRawInputLevels.peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mRawInputLevels.rms.store(other.mRawInputLevels.rms.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mRawInputLevels.clipCount.store(other.mRawInputLevels.clipCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mInputLevels.peak.store(other.mInputLevels.peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mInputLevels.rms.store(other.mInputLevels.rms.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mInputLevels.clipCount.store(other.mInputLevels.clipCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mOutputLevels.peak.store(other.mOutputLevels.peak.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mOutputLevels.rms.store(other.mOutputLevels.rms.load(std::memory_order_relaxed), std::memory_order_relaxed);
    mOutputLevels.clipCount.store(other.mOutputLevels.clipCount.load(std::memory_order_relaxed), std::memory_order_relaxed);

    return *this;
  }

  void MultiPresetMixer::SetUserInputCalibrationGainDb(double dB)
  {
    const double clamped = std::isfinite(dB) ? std::clamp(dB, -24.0, 24.0) : 0.0;
    mUserInputCalibrationGainDb = clamped;
    mUserInputCalibrationGainLinear = static_cast<float>(std::pow(10.0, clamped / 20.0));
  }

  void MultiPresetMixer::RemoveActivePreset(const std::string &presetId)
  {
    for (auto it = mInstances.begin(); it != mInstances.end(); ++it)
    {
      if (it->cfg.id == presetId)
      {
        mInstances.erase(it);
        break;
      }
    }
  }

  void MultiPresetMixer::SetPresetMix(const std::string &presetId, double value)
  {
    if (auto *inst = FindInstance(presetId))
    {
      inst->cfg.mix = std::clamp(value, 0.0, 1.0);
    }
  }

  void MultiPresetMixer::SetPresetPan(const std::string &presetId, double pan)
  {
    if (auto *inst = FindInstance(presetId))
    {
      inst->cfg.pan = std::clamp(pan, -1.0, 1.0);
    }
  }

  void MultiPresetMixer::SetPresetMute(const std::string &presetId, bool mute)
  {
    if (auto *inst = FindInstance(presetId))
    {
      inst->cfg.mute = mute;
    }
  }

  void MultiPresetMixer::SetPresetSolo(const std::string &presetId, bool solo)
  {
    if (auto *inst = FindInstance(presetId))
    {
      inst->cfg.solo = solo;
    }
  }

  void MultiPresetMixer::SetMultiThreadedProcessingEnabled(bool enabled)
  {
    const bool previous = mMultiThreadedProcessingEnabled.exchange(enabled, std::memory_order_acq_rel);
    if (previous == enabled)
      return;

    if (!enabled)
    {
      StopWorkers();
      return;
    }

    if (!mPrepared)
      return;

    const unsigned int hw = std::thread::hardware_concurrency();
    const int workerCount = static_cast<int>(hw > 1 ? hw - 1 : 0);
    if (workerCount > 0)
      StartWorkers(workerCount);
  }

  void MultiPresetMixer::SetInputTrim(double dB)
  {
    for (auto &inst : mInstances)
    {
      inst.executor.SetInputTrim(dB);
    }
  }

  void MultiPresetMixer::SetOutputTrim(double dB)
  {
    for (auto &inst : mInstances)
    {
      inst.executor.SetOutputTrim(dB);
    }
  }

  void MultiPresetMixer::RebuildGlobalChains()
  {
    if (!mPrepared)
      return;

    mPreChainExecutor.Reset();
    mPostChainExecutor.Reset();

    mPreChainExecutor.SetResourceLibrary(mResourceLibrary);
    auto preGraph = mGlobalChainConfig.BuildPreChainGraph();
    if (preGraph.nodes.empty() && preGraph.edges.empty())
    {
      preGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
      mGlobalChainConfig.preChainGraph = preGraph;
    }
    mPreChainExecutor.SetGraph(preGraph);
    mPreChainExecutor.SetInputTrim(mGlobalChainConfig.inputGain);
    mPreChainExecutor.SetSignalDiagnosticsEnabled(mSignalDiagnosticsEnabled.load(std::memory_order_acquire));
    mPreChainExecutor.Prepare(mSampleRate, mMaxBlockSize);

    mPostChainExecutor.SetResourceLibrary(mResourceLibrary);
    auto postGraph = mGlobalChainConfig.BuildPostChainGraph();
    if (postGraph.nodes.empty() && postGraph.edges.empty())
    {
      postGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
      mGlobalChainConfig.postChainGraph = postGraph;
    }
    mPostChainExecutor.SetGraph(postGraph);
    mPostChainExecutor.SetSignalDiagnosticsEnabled(mSignalDiagnosticsEnabled.load(std::memory_order_acquire));
    mPostChainExecutor.Prepare(mSampleRate, mMaxBlockSize);

    mMasterGain = std::pow(10.0, mGlobalChainConfig.outputGain / 20.0);

    mGlobalChainNeedsRebuild.store(false, std::memory_order_release);
  }

  void MultiPresetMixer::EnsureGlobalChainsUpToDate()
  {
    if (mPrepared && mGlobalChainNeedsRebuild.load(std::memory_order_acquire))
    {
      RebuildGlobalChains();
    }
  }

  // ==========================================================================
  // Global Signal Chain Configuration
  // ==========================================================================

  void MultiPresetMixer::SetGlobalChainConfig(const GlobalSignalChainConfig& config)
  {
    mGlobalChainConfig = config;
    if ((mGlobalChainConfig.preChainGraph.nodes.empty() && mGlobalChainConfig.preChainGraph.edges.empty())
        || !GraphHasNodeType(mGlobalChainConfig.preChainGraph, EffectGuids::kDynamicsGate)
        || !GraphHasNodeType(mGlobalChainConfig.preChainGraph, EffectGuids::kTranspose))
    {
      mGlobalChainConfig.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }
    if ((mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
        || !GraphHasNodeType(mGlobalChainConfig.postChainGraph, EffectGuids::kEqParametric)
        || !GraphHasNodeType(mGlobalChainConfig.postChainGraph, EffectGuids::kDelayDoubler))
    {
      mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }
    mGlobalChainNeedsRebuild.store(true, std::memory_order_release);

    // Apply input/output settings
    mAutoLevelInput = config.autoLevelInput;
    mAutoLevelOutput = config.autoLevelOutput;
    mMonoMode = config.monoMode;
    mInputChannel = config.inputChannel;
    mLimiterEnabled = config.limiterEnabled;

    EnsureGlobalChainsUpToDate();
  }

  void MultiPresetMixer::SetGlobalGateEnabled(bool enabled)
  {
    if (mGlobalChainConfig.preChainGraph.nodes.empty() && mGlobalChainConfig.preChainGraph.edges.empty())
    {
      mGlobalChainConfig.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }
    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.preChainGraph, "global_gate", EffectGuids::kDynamicsGate))
    {
      node->enabled = enabled;
      mPreChainExecutor.SetNodeEnabled(node->id, enabled);
    }
  }

  void MultiPresetMixer::SetGlobalGateThreshold(double thresholdDb)
  {
    if (mGlobalChainConfig.preChainGraph.nodes.empty() && mGlobalChainConfig.preChainGraph.edges.empty())
    {
      mGlobalChainConfig.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }
    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.preChainGraph, "global_gate", EffectGuids::kDynamicsGate))
    {
      node->params["threshold"] = thresholdDb;
      mPreChainExecutor.SetNodeParam(node->id, "threshold", thresholdDb);
    }
  }

  void MultiPresetMixer::SetGlobalGateAttack(double attackMs)
  {
    if (mGlobalChainConfig.preChainGraph.nodes.empty() && mGlobalChainConfig.preChainGraph.edges.empty())
    {
      mGlobalChainConfig.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }
    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.preChainGraph, "global_gate", EffectGuids::kDynamicsGate))
    {
      node->params["attack"] = attackMs;
      mPreChainExecutor.SetNodeParam(node->id, "attack", attackMs);
    }
  }

  void MultiPresetMixer::SetGlobalGateHold(double holdMs)
  {
    if (mGlobalChainConfig.preChainGraph.nodes.empty() && mGlobalChainConfig.preChainGraph.edges.empty())
    {
      mGlobalChainConfig.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }
    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.preChainGraph, "global_gate", EffectGuids::kDynamicsGate))
    {
      node->params["hold"] = holdMs;
      mPreChainExecutor.SetNodeParam(node->id, "hold", holdMs);
    }
  }

  void MultiPresetMixer::SetGlobalGateRelease(double releaseMs)
  {
    if (mGlobalChainConfig.preChainGraph.nodes.empty() && mGlobalChainConfig.preChainGraph.edges.empty())
    {
      mGlobalChainConfig.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }
    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.preChainGraph, "global_gate", EffectGuids::kDynamicsGate))
    {
      node->params["release"] = releaseMs;
      mPreChainExecutor.SetNodeParam(node->id, "release", releaseMs);
    }
  }

  void MultiPresetMixer::SetGlobalTransposeEnabled(bool enabled)
  {
    if (mGlobalChainConfig.preChainGraph.nodes.empty() && mGlobalChainConfig.preChainGraph.edges.empty())
    {
      mGlobalChainConfig.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }
    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.preChainGraph, "global_transpose", EffectGuids::kTranspose))
    {
      node->enabled = enabled;
      mPreChainExecutor.SetNodeEnabled(node->id, enabled);
    }
  }

  void MultiPresetMixer::SetGlobalTranspose(int semitones)
  {
    const double value = static_cast<double>(std::clamp(semitones, -12, 12));
    const bool enabled = (value != 0.0);
    if (mGlobalChainConfig.preChainGraph.nodes.empty() && mGlobalChainConfig.preChainGraph.edges.empty())
    {
      mGlobalChainConfig.preChainGraph = GlobalSignalChainConfig::BuildDefaultPreChainGraph();
    }
    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.preChainGraph, "global_transpose", EffectGuids::kTranspose))
    {
      node->enabled = enabled;
      node->params["semitones"] = value;
      mPreChainExecutor.SetNodeEnabled(node->id, enabled);
      mPreChainExecutor.SetNodeParam(node->id, "semitones", value);
    }
  }

  void MultiPresetMixer::SetGlobalEQEnabled(bool enabled)
  {
    if (mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
    {
      mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }
    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.postChainGraph, "global_eq", EffectGuids::kEqParametric))
    {
      node->enabled = enabled;
      mPostChainExecutor.SetNodeEnabled(node->id, enabled);
    }
  }

  void MultiPresetMixer::SetGlobalEQBandGain(int band, double dB)
  {
    static const char* kParamNames[] = {"lowGain", "lowMidGain", "highMidGain", "highGain"};
    if (band < 0 || band > 3) return;
    if (mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
    {
      mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }
    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.postChainGraph, "global_eq", EffectGuids::kEqParametric))
    {
      node->params[kParamNames[band]] = dB;
      mPostChainExecutor.SetNodeParam(node->id, kParamNames[band], dB);
    }
  }

  void MultiPresetMixer::SetGlobalEQBandFrequency(int band, double freq)
  {
    static const char* kParamNames[] = {"lowFreq", "lowMidFreq", "highMidFreq", "highFreq"};
    if (band < 0 || band > 3) return;
    if (mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
    {
      mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }
    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.postChainGraph, "global_eq", EffectGuids::kEqParametric))
    {
      node->params[kParamNames[band]] = freq;
      mPostChainExecutor.SetNodeParam(node->id, kParamNames[band], freq);
    }
  }

  void MultiPresetMixer::SetGlobalEQBandQ(int band, double q)
  {
    static const char* kParamNames[] = {"", "lowMidQ", "highMidQ", ""};
    if (band < 1 || band > 2) return;
    if (mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
    {
      mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }
    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.postChainGraph, "global_eq", EffectGuids::kEqParametric))
    {
      node->params[kParamNames[band]] = q;
      mPostChainExecutor.SetNodeParam(node->id, kParamNames[band], q);
    }
  }

  void MultiPresetMixer::SetGlobalDoublerEnabled(bool enabled)
  {
    if (mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
    {
      mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }
    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.postChainGraph, "global_doubler", EffectGuids::kDelayDoubler))
    {
      node->enabled = enabled;
      mPostChainExecutor.SetNodeEnabled(node->id, enabled);
    }
  }

  void MultiPresetMixer::SetGlobalDoublerDelay(double delayMs)
  {
    const double clamped = std::clamp(delayMs, 0.5, 100.0);
    if (mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
    {
      mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }
    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.postChainGraph, "global_doubler", EffectGuids::kDelayDoubler))
    {
      node->params["time"] = clamped;
      mPostChainExecutor.SetNodeParam(node->id, "time", clamped);
    }
  }

  void MultiPresetMixer::SetGlobalDoublerMix(double mix)
  {
    const double clamped = std::clamp(mix, 0.0, 1.0);
    if (mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
    {
      mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }
    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.postChainGraph, "global_doubler", EffectGuids::kDelayDoubler))
    {
      node->params["mix"] = clamped;
      mPostChainExecutor.SetNodeParam(node->id, "mix", clamped);
    }
  }

  void MultiPresetMixer::SetGlobalDoublerDetune(double cents)
  {
    if (mGlobalChainConfig.postChainGraph.nodes.empty() && mGlobalChainConfig.postChainGraph.edges.empty())
    {
      mGlobalChainConfig.postChainGraph = GlobalSignalChainConfig::BuildDefaultPostChainGraph();
    }
    if (auto* node = FindNodeByIdOrType(mGlobalChainConfig.postChainGraph, "global_doubler", EffectGuids::kDelayDoubler))
    {
      node->params["detune"] = cents;
      mPostChainExecutor.SetNodeParam(node->id, "detune", cents);
    }
  }

  void MultiPresetMixer::SetGlobalInputGain(double dB)
  {
    mGlobalChainConfig.inputGain = dB;
    // Input gain applied via pre-chain input trim
    mPreChainExecutor.SetInputTrim(dB);
  }

  void MultiPresetMixer::SetGlobalOutputGain(double dB)
  {
    mGlobalChainConfig.outputGain = dB;
    // Convert dB to linear for master gain
    mMasterGain = std::pow(10.0, dB / 20.0);
  }

  // ==========================================================================
  // Legacy global FX routing (deprecated - routes to per-preset nodes)
  // These are kept for backward compatibility but should migrate to global chain
  // ==========================================================================

  // Global gate control (legacy - routes to dynamics_gate nodes in signal chain)
  void MultiPresetMixer::SetGateEnabled(bool enabled)
  {
    // Route to global chain instead
    SetGlobalGateEnabled(enabled);
  }

  void MultiPresetMixer::SetGateThreshold(double thresholdDb)
  {
    // Route to global chain instead
    SetGlobalGateThreshold(thresholdDb);
  }

  // Global doubler control (legacy - routes to delay_doubler nodes in signal chain)
  void MultiPresetMixer::SetDoublerEnabled(bool enabled)
  {
    // Route to global chain instead
    SetGlobalDoublerEnabled(enabled);
  }

  void MultiPresetMixer::SetDoublerDelay(double delayMs)
  {
    // Route to global chain instead
    SetGlobalDoublerDelay(delayMs);
  }

  // Global transpose control (legacy - routes to pitch_shift nodes in signal chain)
  void MultiPresetMixer::SetTranspose(int semitones)
  {
    // Route to global chain instead
    SetGlobalTransposeEnabled(semitones != 0);
    SetGlobalTranspose(semitones);
  }

  void MultiPresetMixer::SetAmpDrive(double value)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = FindFirstNamNodeId(inst.executor);
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "inputGain", value);
      }
    }
  }

  void MultiPresetMixer::SetIRQuality(double value)
  {
    for (auto &inst : mInstances)
    {
      auto nodeId = inst.executor.FindFirstNodeOfType(EffectGuids::kCabIr);
      if (nodeId.empty())
      {
        nodeId = inst.executor.FindFirstNodeOfType(EffectGuids::kCabIr);
      }
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "quality", value);
      }
    }
  }

  // EQ methods now route to global post-chain (legacy compatibility)
  void MultiPresetMixer::SetEQEnabled(bool enabled)
  {
    SetGlobalEQEnabled(enabled);
  }

  void MultiPresetMixer::SetEQBandGain(int band, double value)
  {
    SetGlobalEQBandGain(band, value);
  }

  void MultiPresetMixer::SetEQBandFrequency(int band, double value)
  {
    SetGlobalEQBandFrequency(band, value);
  }

  void MultiPresetMixer::SetEQBandQ(int band, double value)
  {
    SetGlobalEQBandQ(band, value);
  }

  void MultiPresetMixer::SetAmpTone(double value)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = FindFirstNamNodeId(inst.executor);
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "tone", value);
      }
    }
  }

  // Node-level control methods
  void MultiPresetMixer::SetNodeEnabled(const std::string &presetId, const std::string &nodeId, bool enabled)
  {
    if (auto *inst = FindInstance(presetId))
    {
      inst->executor.SetNodeEnabled(nodeId, enabled);
    }
  }

  void MultiPresetMixer::SetNodeParam(const std::string &presetId, const std::string &nodeId, const std::string &key, double value)
  {
    if (auto *inst = FindInstance(presetId))
    {
      inst->executor.SetNodeParam(nodeId, key, value);
    }
  }

  void MultiPresetMixer::SetNodeConfig(const std::string &presetId, const std::string &nodeId, const std::string &key, const std::string &value)
  {
    if (auto *inst = FindInstance(presetId))
    {
      inst->executor.SetNodeConfig(nodeId, key, value);
    }
  }

  void MultiPresetMixer::SetNodeConfigForType(const std::string &type, const std::string &key, const std::string &value)
  {
    for (auto &inst : mInstances)
      inst.executor.SetNodeConfigForType(type, key, value);

    mPreChainExecutor.SetNodeConfigForType(type, key, value);
    mPostChainExecutor.SetNodeConfigForType(type, key, value);
  }

  std::string MultiPresetMixer::GetNodeConfig(const std::string &presetId, const std::string &nodeId, const std::string &key) const
  {
    if (const auto *inst = FindInstance(presetId))
    {
      return inst->executor.GetNodeConfig(nodeId, key);
    }
    return {};
  }

  EffectProcessor *MultiPresetMixer::GetNodeProcessor(const std::string &presetId, const std::string &nodeId)
  {
    if (auto *inst = FindInstance(presetId))
    {
      return inst->executor.GetNodeProcessor(nodeId);
    }

    return nullptr;
  }

  const EffectProcessor *MultiPresetMixer::GetNodeProcessor(const std::string &presetId, const std::string &nodeId) const
  {
    if (const auto *inst = FindInstance(presetId))
    {
      return inst->executor.GetNodeProcessor(nodeId);
    }

    return nullptr;
  }

  void MultiPresetMixer::SetTempo(double bpm)
  {
    for (auto &inst : mInstances)
      inst.executor.SetTempo(bpm);
    mPreChainExecutor.SetTempo(bpm);
    mPostChainExecutor.SetTempo(bpm);
  }

  bool MultiPresetMixer::LoadNodeResource(const std::string &presetId, const std::string &nodeId, const ResourceRef &ref)
  {
    if (auto *inst = FindInstance(presetId))
    {
      return inst->executor.LoadNodeResource(nodeId, ref);
    }
    return false;
  }

  // ---------------------------------------------------------------------------
  // Destructor / parallel worker lifecycle
  // ---------------------------------------------------------------------------

  MultiPresetMixer::~MultiPresetMixer()
  {
    StopWorkers();
  }

  void MultiPresetMixer::StartWorkers(int count)
  {
    StopWorkers();

    {
      std::lock_guard<std::mutex> lock(mParallelMutex);
      mParallelQuit.store(false, std::memory_order_relaxed);
      mParallelGeneration.store(0, std::memory_order_relaxed);
    }

    const int numWorkers = std::min(count, kMaxParallelWorkers);
    mWorkerThreads.reserve(static_cast<size_t>(numWorkers));
    for (int i = 0; i < numWorkers; ++i)
      mWorkerThreads.emplace_back([this] { WorkerLoop(); });
  }

  void MultiPresetMixer::StopWorkers()
  {
    if (mWorkerThreads.empty())
      return;

    {
      std::lock_guard<std::mutex> lock(mParallelMutex);
      mParallelQuit.store(true, std::memory_order_relaxed);
    }
    mParallelCv.notify_all();
    for (auto &t : mWorkerThreads)
    {
      if (t.joinable())
        t.join();
    }
    mWorkerThreads.clear();
  }

  void MultiPresetMixer::WorkerLoop()
  {
    uint32_t lastGen = 0;
    while (true)
    {
      {
        std::unique_lock<std::mutex> lock(mParallelMutex);
        mParallelCv.wait(lock, [&]
        {
          return mParallelQuit.load(std::memory_order_relaxed)
              || mParallelGeneration.load(std::memory_order_relaxed) != lastGen;
        });
      }

      if (mParallelQuit.load(std::memory_order_acquire))
        break;

      lastGen = mParallelGeneration.load(std::memory_order_acquire);

      const int total = mParallelTaskCount.load(std::memory_order_acquire);
      while (true)
      {
        const int idx = mParallelTaskHead.fetch_add(1, std::memory_order_acq_rel);
        if (idx >= total)
          break;
        const auto &wi = mWorkItems[static_cast<size_t>(idx)];
        float *ins[2]  = {wi.preChainOutL, wi.preChainOutR};
        float *outs[2] = {wi.inst->outL.data(), wi.inst->outR.data()};
        wi.inst->executor.Process(ins, outs, wi.numSamples);
        mParallelDoneCount.fetch_add(1, std::memory_order_release);
      }
    }
  }

  void MultiPresetMixer::Prepare(double sampleRate, int maxBlockSize)
  {
    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;
    mPrepared = true;

    // Allocate global temp buffers
    mTempInL.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mTempInR.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mPreChainOutL.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mPreChainOutR.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mPostChainOutL.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mPostChainOutR.resize(static_cast<size_t>(maxBlockSize), 0.0f);

    // Build and prepare global signal chains based on current config
    mGlobalChainNeedsRebuild.store(true, std::memory_order_release);
    EnsureGlobalChainsUpToDate();

    AllocateBuffers(maxBlockSize);

    for (auto &inst : mInstances)
    {
      inst.executor.Prepare(sampleRate, maxBlockSize);
      AllocateInstanceBuffers(inst, maxBlockSize);
    }

    // Start worker threads for parallel preset processing.
    // Reserve hw_concurrency-1 threads so the audio thread's core is not contested.
    if (mMultiThreadedProcessingEnabled.load(std::memory_order_acquire))
    {
      const unsigned int hw = std::thread::hardware_concurrency();
      const int workerCount = static_cast<int>(hw > 1 ? hw - 1 : 0);
      if (workerCount > 0)
        StartWorkers(workerCount);
    }
    else
    {
      StopWorkers();
    }
  }

  void MultiPresetMixer::Reset()
  {
    mPreChainExecutor.Reset();
    mPostChainExecutor.Reset();
    for (auto &inst : mInstances)
    {
      inst.executor.Reset();
    }
  }

  void MultiPresetMixer::Process(float **inputs, float **outputs, int numSamples)
  {
    if (!outputs || numSamples <= 0)
      return;

    // Clamp to allocated buffer size to prevent out-of-bounds writes
    if (mPrepared && mMaxBlockSize > 0)
      numSamples = std::min(numSamples, mMaxBlockSize);

    const bool diagnosticsEnabled = mSignalDiagnosticsEnabled.load(std::memory_order_acquire);

    // NOTE: Do NOT call EnsureGlobalChainsUpToDate() here.
    // Rebuilding global chains allocates memory which is unsafe on the audio thread.
    // The chains are rebuilt from Prepare() and SetGlobalChainConfig() on the UI/main thread.

    // Safety check: ensure we're prepared before processing
    if (!mPrepared || mInstances.empty())
    {
      // Output silence if not ready
      if (outputs[0])
        std::fill(outputs[0], outputs[0] + numSamples, 0.0f);
      if (outputs[1])
        std::fill(outputs[1], outputs[1] + numSamples, 0.0f);
      return;
    }

    // Prepare input based on global mono/stereo settings
    float *processInL = inputs ? inputs[0] : nullptr;
    float *processInR = inputs ? inputs[1] : nullptr;

    if (diagnosticsEnabled)
    {
      const auto rawStats = ComputeLevelStats(processInL, processInR, numSamples);
      mRawInputLevels.peak.store(rawStats.peak, std::memory_order_relaxed);
      mRawInputLevels.rms.store(rawStats.rms, std::memory_order_relaxed);
      mRawInputLevels.clipCount.store(rawStats.clipCount, std::memory_order_relaxed);
    }

    if (mMonoMode && processInL && processInR)
    {
      // Apply mono mode: select input channel or sum to mono
      for (int i = 0; i < numSamples; ++i)
      {
        float monoSample;
        if (mInputChannel == 0)
        {
          monoSample = processInL[i]; // Left only
        }
        else if (mInputChannel == 1)
        {
          monoSample = processInR[i]; // Right only
        }
        else
        {
          monoSample = (processInL[i] + processInR[i]) * 0.5f; // Sum
        }
        mTempInL[static_cast<std::size_t>(i)] = monoSample;
        mTempInR[static_cast<std::size_t>(i)] = monoSample;
      }
      processInL = mTempInL.data();
      processInR = mTempInR.data();
    }

    if (std::abs(mUserInputCalibrationGainLinear - 1.0f) > 1.0e-4f)
    {
      if (processInL)
      {
        for (int i = 0; i < numSamples; ++i)
          mTempInL[static_cast<std::size_t>(i)] = processInL[i] * mUserInputCalibrationGainLinear;
        processInL = mTempInL.data();
      }

      if (processInR)
      {
        for (int i = 0; i < numSamples; ++i)
          mTempInR[static_cast<std::size_t>(i)] = processInR[i] * mUserInputCalibrationGainLinear;
        processInR = mTempInR.data();
      }
    }


    // Apply auto-level input gain (simple peak detection and normalization)
    if (mAutoLevelInput && processInL && processInR)
    {
      // Find peak
      float peak = 0.0f;
      for (int i = 0; i < numSamples; ++i)
      {
        peak = std::max(peak, std::abs(processInL[i]));
        peak = std::max(peak, std::abs(processInR[i]));
      }

      // Apply auto-level with smoothing
      if (peak > 0.001f)
      {
        const float targetGain = kInputAutoLevelTargetPeak / peak;
        const float limitedGain = std::min(targetGain, kInputAutoLevelMaxGain);
        mInputAutoLevelGain = mInputAutoLevelGain * (1.0f - kAutoLevelAttackMix) + limitedGain * kAutoLevelAttackMix;

        for (int i = 0; i < numSamples; ++i)
        {
          mTempInL[static_cast<std::size_t>(i)] = processInL[i] * mInputAutoLevelGain;
          mTempInR[static_cast<std::size_t>(i)] = processInR[i] * mInputAutoLevelGain;
        }
        processInL = mTempInL.data();
        processInR = mTempInR.data();
      }
    }

    if (diagnosticsEnabled)
    {
      const auto stats = ComputeLevelStats(processInL, processInR, numSamples);
      mInputLevels.peak.store(stats.peak, std::memory_order_relaxed);
      mInputLevels.rms.store(stats.rms, std::memory_order_relaxed);
      mInputLevels.clipCount.store(stats.clipCount, std::memory_order_relaxed);
    }

    // Process tuner FIRST (before any processing, uses raw input for accurate pitch detection)
    if (mTunerEnabled)
    {
      float *tunerInputs[2] = {processInL, processInR};
      ProcessTuner(tunerInputs, numSamples);

      // If not in live tuner mode, mute the output
      if (!mLiveTunerMode)
      {
        if (outputs[0])
          std::fill(outputs[0], outputs[0] + numSamples, 0.0f);
        if (outputs[1])
          std::fill(outputs[1], outputs[1] + numSamples, 0.0f);
        return;
      }
    }

    // ==========================================================================
    // GLOBAL PRE-CHAIN: Input → Noise Gate → Transpose
    // ==========================================================================
    float *preChainInputs[2] = {processInL, processInR};
    float *preChainOutputs[2] = {mPreChainOutL.data(), mPreChainOutR.data()};
    mPreChainExecutor.Process(preChainInputs, preChainOutputs, numSamples);

    // ==========================================================================
    // PRESET PROCESSING: Process each active preset and mix
    // ==========================================================================

    // Detect solo mode
    bool anySolo = false;
    for (const auto &inst : mInstances)
    {
      if (inst.cfg.solo)
      {
        anySolo = true;
        break;
      }
    }

    // Clear preset mix accumulator (use outputs as accumulator)
    if (outputs[0])
      std::fill(outputs[0], outputs[0] + numSamples, 0.0f);
    if (outputs[1])
      std::fill(outputs[1], outputs[1] + numSamples, 0.0f);

    // Count active instances to decide whether to use parallel dispatch.
    int activeCount = 0;
    for (const auto &inst : mInstances)
      if (!inst.cfg.mute && (!anySolo || inst.cfg.solo))
        ++activeCount;

    int totalWorkUnits = 0;
    if (activeCount >= 2)
    {
      for (const auto &inst : mInstances)
      {
        if (inst.cfg.mute || (anySolo && !inst.cfg.solo))
          continue;
        totalWorkUnits += inst.complexityScore * numSamples;
      }
    }

    const bool useParallel = ShouldUseParallelPresetDispatch(
      mMultiThreadedProcessingEnabled.load(std::memory_order_acquire),
      activeCount,
      totalWorkUnits,
      !mWorkerThreads.empty());

    // Avoid nested parallelism: if mixer-level fan-out is active, run each preset graph serially.
    for (auto &inst : mInstances)
      inst.executor.SetParallelLevelsEnabled(!useParallel);

    if (useParallel)
    {
      // Pack work items (up to kMaxWorkItems); any extras fall through to serial below.
      int wi = 0;
      for (auto &inst : mInstances)
      {
        if (inst.cfg.mute || (anySolo && !inst.cfg.solo))
          continue;

        if (wi < kMaxWorkItems)
        {
          mWorkItems[static_cast<size_t>(wi)] = {&inst, mPreChainOutL.data(), mPreChainOutR.data(), numSamples};
          ++wi;
        }
        else
        {
          // Overflow beyond kMaxWorkItems: process serially and mix immediately.
          float *presetOutPtrs[2] = {inst.outL.data(), inst.outR.data()};
          inst.executor.Process(preChainOutputs, presetOutPtrs, numSamples);
          float gL = 1.0f, gR = 1.0f;
          ComputePanGains(inst.cfg.pan, gL, gR);
          const float mixGain = static_cast<float>(inst.cfg.mix);
          for (int i = 0; i < numSamples; ++i)
          {
            if (outputs[0]) outputs[0][i] += inst.outL[static_cast<size_t>(i)] * mixGain * gL;
            if (outputs[1]) outputs[1][i] += inst.outR[static_cast<size_t>(i)] * mixGain * gR;
          }
        }
      }

      // Publish tasks and wake only the workers we actually need.
      {
        std::lock_guard<std::mutex> lock(mParallelMutex);
        mParallelTaskHead.store(0, std::memory_order_relaxed);
        mParallelDoneCount.store(0, std::memory_order_relaxed);
        mParallelTaskCount.store(wi, std::memory_order_relaxed);
        mParallelGeneration.fetch_add(1, std::memory_order_relaxed);
      }
      const int workersNeeded = std::min<int>(std::max(0, wi - 1), static_cast<int>(mWorkerThreads.size()));
      for (int n = 0; n < workersNeeded; ++n)
        mParallelCv.notify_one();

      // Audio thread steals tasks alongside workers.
      while (true)
      {
        const int idx = mParallelTaskHead.fetch_add(1, std::memory_order_acq_rel);
        if (idx >= wi)
          break;
        const auto &item = mWorkItems[static_cast<size_t>(idx)];
        float *ins[2]  = {item.preChainOutL, item.preChainOutR};
        float *outs[2] = {item.inst->outL.data(), item.inst->outR.data()};
        item.inst->executor.Process(ins, outs, item.numSamples);
        mParallelDoneCount.fetch_add(1, std::memory_order_release);
      }

      // Spin-wait for all tasks to complete.
      while (mParallelDoneCount.load(std::memory_order_acquire) < wi)
        CpuRelax();

      // Mix all parallel outputs into the accumulator.
      for (int i = 0; i < wi; ++i)
      {
        const auto &item = mWorkItems[static_cast<size_t>(i)];
        float gL = 1.0f, gR = 1.0f;
        ComputePanGains(item.inst->cfg.pan, gL, gR);
        const float mixGain = static_cast<float>(item.inst->cfg.mix);
        for (int s = 0; s < numSamples; ++s)
        {
          if (outputs[0]) outputs[0][s] += item.inst->outL[static_cast<size_t>(s)] * mixGain * gL;
          if (outputs[1]) outputs[1][s] += item.inst->outR[static_cast<size_t>(s)] * mixGain * gR;
        }
      }
    }
    else
    {
      // Serial path: single active preset or no worker threads available.
      for (auto &inst : mInstances)
      {
        const bool include = (!inst.cfg.mute) && (!anySolo || inst.cfg.solo);
        if (!include)
          continue;

        float *presetOutPtrs[2] = {inst.outL.data(), inst.outR.data()};
        inst.executor.Process(preChainOutputs, presetOutPtrs, numSamples);

        float gL = 1.0f, gR = 1.0f;
        ComputePanGains(inst.cfg.pan, gL, gR);
        const float mixGain = static_cast<float>(inst.cfg.mix);

        for (int i = 0; i < numSamples; ++i)
        {
          if (outputs[0])
            outputs[0][i] += inst.outL[static_cast<size_t>(i)] * mixGain * gL;
          if (outputs[1])
            outputs[1][i] += inst.outR[static_cast<size_t>(i)] * mixGain * gR;
        }
      }
    }

    // ==========================================================================
    // GLOBAL POST-CHAIN: EQ → Doubler
    // ==========================================================================
    float *postChainOutputs[2] = {mPostChainOutL.data(), mPostChainOutR.data()};
    mPostChainExecutor.Process(outputs, postChainOutputs, numSamples);

    // Copy post-chain output back to main outputs
    if (outputs[0])
      std::copy(mPostChainOutL.begin(), mPostChainOutL.begin() + numSamples, outputs[0]);
    if (outputs[1])
      std::copy(mPostChainOutR.begin(), mPostChainOutR.begin() + numSamples, outputs[1]);

    // ==========================================================================
    // FINAL OUTPUT STAGE: Master gain, auto-level, limiter
    // ==========================================================================
    
    // Apply master gain
    const float master = static_cast<float>(mMasterGain);
    if (master != 1.0f)
    {
      if (outputs[0])
      {
        for (int i = 0; i < numSamples; ++i)
          outputs[0][i] *= master;
      }
      if (outputs[1])
      {
        for (int i = 0; i < numSamples; ++i)
          outputs[1][i] *= master;
      }
    }

    // Apply auto-level output (simple peak limiting)
    if (mAutoLevelOutput)
    {
      const float outputProtectionCeilingLinear = static_cast<float>(GetOutputProtectionCeilingLinear());
      float peak = 0.0f;
      for (int i = 0; i < numSamples; ++i)
      {
        if (outputs[0])
          peak = std::max(peak, std::abs(outputs[0][i]));
        if (outputs[1])
          peak = std::max(peak, std::abs(outputs[1][i]));
      }

      if (peak > outputProtectionCeilingLinear)
      {
        const float attenuation = outputProtectionCeilingLinear / peak;
        mOutputAutoLevelGain = mOutputAutoLevelGain * (1.0f - kAutoLevelAttackMix) + attenuation * kAutoLevelAttackMix;

        if (outputs[0])
        {
          for (int i = 0; i < numSamples; ++i)
            outputs[0][i] *= mOutputAutoLevelGain;
        }
        if (outputs[1])
        {
          for (int i = 0; i < numSamples; ++i)
            outputs[1][i] *= mOutputAutoLevelGain;
        }
      }
      else
      {
        // Slowly release gain reduction
        mOutputAutoLevelGain = std::min(1.0f, mOutputAutoLevelGain * kAutoLevelReleaseMultiplier);
      }
    }

    if (diagnosticsEnabled)
    {
      const auto stats = ComputeLevelStats(outputs ? outputs[0] : nullptr, outputs ? outputs[1] : nullptr, numSamples);
      mOutputLevels.peak.store(stats.peak, std::memory_order_relaxed);
      mOutputLevels.rms.store(stats.rms, std::memory_order_relaxed);
      mOutputLevels.clipCount.store(stats.clipCount, std::memory_order_relaxed);
    }

    // Optional simple limiter (clip)
    if (mLimiterEnabled)
    {
      const float outputProtectionCeilingLinear = static_cast<float>(GetOutputProtectionCeilingLinear());
      if (outputs[0])
      {
        for (int i = 0; i < numSamples; ++i)
          outputs[0][i] = std::clamp(outputs[0][i], -outputProtectionCeilingLinear, outputProtectionCeilingLinear);
      }
      if (outputs[1])
      {
        for (int i = 0; i < numSamples; ++i)
          outputs[1][i] = std::clamp(outputs[1][i], -outputProtectionCeilingLinear, outputProtectionCeilingLinear);
      }
    }
  }

  void MultiPresetMixer::SetSignalDiagnosticsEnabled(bool enabled)
  {
    mSignalDiagnosticsEnabled.store(enabled, std::memory_order_release);
    mPreChainExecutor.SetSignalDiagnosticsEnabled(enabled);
    mPostChainExecutor.SetSignalDiagnosticsEnabled(enabled);
    for (auto &inst : mInstances)
    {
      inst.executor.SetSignalDiagnosticsEnabled(enabled);
    }
  }

  MultiPresetMixer::SignalDiagnosticsSnapshot MultiPresetMixer::GetSignalDiagnosticsSnapshot() const
  {
    SignalDiagnosticsSnapshot snapshot;
    snapshot.rawInput.peak = mRawInputLevels.peak.load(std::memory_order_relaxed);
    snapshot.rawInput.rms = mRawInputLevels.rms.load(std::memory_order_relaxed);
    snapshot.rawInput.clipCount = mRawInputLevels.clipCount.load(std::memory_order_relaxed);

    snapshot.input.peak = mInputLevels.peak.load(std::memory_order_relaxed);
    snapshot.input.rms = mInputLevels.rms.load(std::memory_order_relaxed);
    snapshot.input.clipCount = mInputLevels.clipCount.load(std::memory_order_relaxed);

    snapshot.output.peak = mOutputLevels.peak.load(std::memory_order_relaxed);
    snapshot.output.rms = mOutputLevels.rms.load(std::memory_order_relaxed);
    snapshot.output.clipCount = mOutputLevels.clipCount.load(std::memory_order_relaxed);

    const auto preLevels = mPreChainExecutor.GetNodeSignalLevels();
    const auto postLevels = mPostChainExecutor.GetNodeSignalLevels();

    snapshot.nodes.reserve(preLevels.size() + postLevels.size() + mInstances.size() * 8);

    for (const auto &entry : preLevels)
    {
      NodeSignalLevel node;
      node.scope = "pre";
      node.nodeId = entry.nodeId;
      node.nodeType = entry.nodeType;
      node.channelCount = entry.channelCount;
      node.levels.peak = entry.peak;
      node.levels.rms = entry.rms;
      node.levels.clipCount = entry.clipCount;
      snapshot.nodes.push_back(std::move(node));
    }

    for (const auto &inst : mInstances)
    {
      const auto levels = inst.executor.GetNodeSignalLevels();
      for (const auto &entry : levels)
      {
        NodeSignalLevel node;
        node.scope = "preset";
        node.presetId = inst.cfg.id;
        node.nodeId = entry.nodeId;
        node.nodeType = entry.nodeType;
        node.channelCount = entry.channelCount;
        node.levels.peak = entry.peak;
        node.levels.rms = entry.rms;
        node.levels.clipCount = entry.clipCount;
        snapshot.nodes.push_back(std::move(node));
      }
    }

    for (const auto &entry : postLevels)
    {
      NodeSignalLevel node;
      node.scope = "post";
      node.nodeId = entry.nodeId;
      node.nodeType = entry.nodeType;
      node.channelCount = entry.channelCount;
      node.levels.peak = entry.peak;
      node.levels.rms = entry.rms;
      node.levels.clipCount = entry.clipCount;
      snapshot.nodes.push_back(std::move(node));
    }

    return snapshot;
  }

  std::vector<std::string> MultiPresetMixer::GetActivePresetIds() const
  {
    std::vector<std::string> ids;
    ids.reserve(mInstances.size());
    for (const auto &inst : mInstances)
      ids.push_back(inst.cfg.id);
    return ids;
  }

  std::vector<std::string> MultiPresetMixer::GetPresetNodeTypes(const std::string &presetId) const
  {
    const auto *inst = FindInstance(presetId);
    if (!inst)
      return {};
    return inst->executor.GetNodeTypes();
  }

  std::optional<MultiPresetMixer::InstanceConfig> MultiPresetMixer::GetPresetConfig(const std::string &presetId) const
  {
    if (const auto *inst = FindInstance(presetId))
      return inst->cfg;
    return std::nullopt;
  }

  MultiPresetMixer::PresetInstance *MultiPresetMixer::FindInstance(const std::string &id)
  {
    for (auto &inst : mInstances)
    {
      if (inst.cfg.id == id)
        return &inst;
    }
    return nullptr;
  }

  const MultiPresetMixer::PresetInstance *MultiPresetMixer::FindInstance(const std::string &id) const
  {
    for (const auto &inst : mInstances)
    {
      if (inst.cfg.id == id)
        return &inst;
    }
    return nullptr;
  }

  void MultiPresetMixer::AllocateBuffers(int maxBlockSize)
  {
    for (auto &inst : mInstances)
    {
      inst.outL.resize(static_cast<size_t>(maxBlockSize), 0.0f);
      inst.outR.resize(static_cast<size_t>(maxBlockSize), 0.0f);
      AllocateInstanceBuffers(inst, maxBlockSize);
    }
  }

  void MultiPresetMixer::AllocateInstanceBuffers(PresetInstance &inst, int maxBlockSize)
  {
    // Output buffers only - gate/pitch/doubler are now signal chain nodes
    (void)inst; // Nothing to allocate per-instance anymore
    (void)maxBlockSize;
  }

  void MultiPresetMixer::ComputePanGains(double pan, float &gL, float &gR)
  {
    // Equal-power pan law
    // pan in [-1, 1] maps to theta in [0, pi/2]
    constexpr double kPi = 3.14159265358979323846;
    const double theta = (pan + 1.0) * (kPi * 0.25); // (-1)->0, 0->pi/4, 1->pi/2
    gL = static_cast<float>(std::cos(theta));
    gR = static_cast<float>(std::sin(theta));
  }

  SignalGraphExecutor::DSPPerformanceStats MultiPresetMixer::GetPerformanceStats() const
  {
    SignalGraphExecutor::DSPPerformanceStats aggregatedStats;

    const auto mergeStats = [&aggregatedStats](const SignalGraphExecutor::DSPPerformanceStats& stats,
                                               const std::string& scopedPrefix)
    {
      aggregatedStats.totalProcessingTimeUs += stats.totalProcessingTimeUs;
      aggregatedStats.realTimeUs = std::max(aggregatedStats.realTimeUs, stats.realTimeUs);

      for (const auto& [nodeId, timeUs] : stats.nodeProcessingTimesUs)
      {
        aggregatedStats.nodeProcessingTimesUs[nodeId] += timeUs;
      }

      if (stats.scopedNodeProcessingTimesUs.empty())
      {
        for (const auto& [nodeId, timeUs] : stats.nodeProcessingTimesUs)
        {
          aggregatedStats.scopedNodeProcessingTimesUs[scopedPrefix + nodeId] += timeUs;
        }
      }
      else
      {
        for (const auto& [nodeId, timeUs] : stats.scopedNodeProcessingTimesUs)
        {
          aggregatedStats.scopedNodeProcessingTimesUs[scopedPrefix + nodeId] += timeUs;
        }
      }

      for (const auto& [nodeId, latencySamples] : stats.nodeLatencySamples)
      {
        aggregatedStats.nodeLatencySamples[nodeId] = std::max(aggregatedStats.nodeLatencySamples[nodeId], latencySamples);
      }

      if (stats.scopedNodeLatencySamples.empty())
      {
        for (const auto& [nodeId, latencySamples] : stats.nodeLatencySamples)
        {
          aggregatedStats.scopedNodeLatencySamples[scopedPrefix + nodeId] = std::max(
            aggregatedStats.scopedNodeLatencySamples[scopedPrefix + nodeId],
            latencySamples);
        }
      }
      else
      {
        for (const auto& [nodeId, latencySamples] : stats.scopedNodeLatencySamples)
        {
          aggregatedStats.scopedNodeLatencySamples[scopedPrefix + nodeId] = std::max(
            aggregatedStats.scopedNodeLatencySamples[scopedPrefix + nodeId],
            latencySamples);
        }
      }
    };

    mergeStats(mPreChainExecutor.GetPerformanceStats(), "pre::");

    for (const auto& instance : mInstances)
    {
      mergeStats(instance.executor.GetPerformanceStats(), instance.cfg.id + "::");
    }

    mergeStats(mPostChainExecutor.GetPerformanceStats(), "post::");

    if (aggregatedStats.realTimeUs > 0.0)
    {
      aggregatedStats.dspLoadPercent = (aggregatedStats.totalProcessingTimeUs / aggregatedStats.realTimeUs) * 100.0;
    }

    return aggregatedStats;
  }

  int MultiPresetMixer::GetTotalLatencySamples() const
  {
    const int preChain  = mPreChainExecutor.GetTotalLatencySamples();
    const int postChain = mPostChainExecutor.GetTotalLatencySamples();

    int instanceMax = 0;
    for (const auto& inst : mInstances)
      instanceMax = std::max(instanceMax, inst.executor.GetTotalLatencySamples());

    return preChain + instanceMax + postChain;
  }

  void MultiPresetMixer::SetTunerEnabled(bool enabled)
  {
    mTunerEnabled = enabled;
    if (enabled)
    {
      // Reset tuner state when enabled
      mTunerBuffer.resize(kTunerBufferSize, 0.0);
      std::fill(mTunerBuffer.begin(), mTunerBuffer.end(), 0.0);
      mTunerBufferWriteIndex = 0;
      mTunerSampleCounter = 0;
    }
  }

  void MultiPresetMixer::SetTunerCallback(TunerCallback callback)
  {
    mTunerCallback = std::move(callback);
  }

  void MultiPresetMixer::SetTunerReferenceFrequency(double frequency)
  {
    mTunerReferenceFrequency = std::clamp(frequency, 400.0, 480.0);
  }

  void MultiPresetMixer::ProcessTuner(float **inputs, int numSamples)
  {
    // Use the main input channel setting (same as DSP processing)
    const int ch = mInputChannel;
    
    if (!mTunerEnabled || !mTunerCallback || !inputs || !inputs[ch])
    {
      return;
    }

    // Ensure buffer is allocated
    if (mTunerBuffer.size() != kTunerBufferSize)
    {
      mTunerBuffer.resize(kTunerBufferSize, 0.0);
    }

    // Fill the tuner buffer with input samples (mono - use selected channel)
    for (int i = 0; i < numSamples; ++i)
    {
      mTunerBuffer[mTunerBufferWriteIndex] = static_cast<double>(inputs[ch][i]);
      mTunerBufferWriteIndex = (mTunerBufferWriteIndex + 1) % kTunerBufferSize;
      ++mTunerSampleCounter;
    }

    // Update tuner at regular intervals
    if (mTunerSampleCounter >= kTunerUpdateInterval)
    {
      mTunerSampleCounter = 0;

      // Reorder buffer to be contiguous for pitch detection
      std::vector<double> orderedBuffer(kTunerBufferSize);
      for (std::size_t i = 0; i < kTunerBufferSize; ++i)
      {
        orderedBuffer[i] = mTunerBuffer[(mTunerBufferWriteIndex + i) % kTunerBufferSize];
      }

      // Calculate RMS for debug
      double sumSq = 0.0;
      for (const auto &s : orderedBuffer)
        sumSq += s * s;
      double rms = std::sqrt(sumSq / static_cast<double>(orderedBuffer.size()));

      double frequency = DetectPitch(orderedBuffer);
      TunerResult result = FrequencyToNote(frequency);

      // Store debug info in result for UI logging
      result.debugRms = rms;
      result.debugRawFreq = frequency;

      mTunerCallback(result);
    }
  }

  double MultiPresetMixer::DetectPitch(const std::vector<double> &samples) const
  {
    // Autocorrelation-based pitch detection (YIN-inspired algorithm)
    const std::size_t n = samples.size();
    if (n < 2)
    {
      return 0.0;
    }

    // Calculate RMS to check if there's enough signal
    double sumSquares = 0.0;
    for (const auto &sample : samples)
    {
      sumSquares += sample * sample;
    }
    const double rms = std::sqrt(sumSquares / static_cast<double>(n));

    // If signal is too quiet, don't try to detect pitch
    if (rms < 0.003)
    {
      return 0.0;
    }

    // Define search range for guitar: 50Hz (low tunings) to 1500Hz (F#6)
    const int minPeriod = static_cast<int>(mSampleRate / 1500.0); // Highest frequency
    const int maxPeriod = static_cast<int>(mSampleRate / 50.0);   // Lowest frequency

    if (maxPeriod >= static_cast<int>(n / 2) || minPeriod < 2)
    {
      return 0.0;
    }

    // Calculate difference function (YIN step 2)
    std::vector<double> diff(static_cast<std::size_t>(maxPeriod) + 1, 0.0);

    for (int tau = minPeriod; tau <= maxPeriod; ++tau)
    {
      double sum = 0.0;
      for (std::size_t i = 0; i < n - static_cast<std::size_t>(tau); ++i)
      {
        const double delta = samples[i] - samples[i + tau];
        sum += delta * delta;
      }
      diff[static_cast<std::size_t>(tau)] = sum;
    }

    // Cumulative mean normalized difference function (YIN step 4)
    std::vector<double> cmndf(static_cast<std::size_t>(maxPeriod) + 1, 1.0);
    double runningSum = 0.0;

    for (int tau = minPeriod; tau <= maxPeriod; ++tau)
    {
      runningSum += diff[static_cast<std::size_t>(tau)];
      if (runningSum > 0.0)
      {
        cmndf[static_cast<std::size_t>(tau)] = diff[static_cast<std::size_t>(tau)] * static_cast<double>(tau) / runningSum;
      }
    }

    // Find the first minimum below threshold (YIN step 5)
    constexpr double threshold = 0.15;
    int bestPeriod = -1;

    for (int tau = minPeriod; tau < maxPeriod; ++tau)
    {
      if (cmndf[static_cast<std::size_t>(tau)] < threshold)
      {
        // Find the local minimum
        while (tau + 1 <= maxPeriod && cmndf[static_cast<std::size_t>(tau + 1)] < cmndf[static_cast<std::size_t>(tau)])
        {
          ++tau;
        }
        bestPeriod = tau;
        break;
      }
    }

    // If no period found below threshold, find the global minimum
    if (bestPeriod < 0)
    {
      double minVal = cmndf[static_cast<std::size_t>(minPeriod)];
      bestPeriod = minPeriod;
      for (int tau = minPeriod + 1; tau <= maxPeriod; ++tau)
      {
        if (cmndf[static_cast<std::size_t>(tau)] < minVal)
        {
          minVal = cmndf[static_cast<std::size_t>(tau)];
          bestPeriod = tau;
        }
      }
      // If the minimum is too high, no pitch detected
      if (minVal > 0.5)
      {
        return 0.0;
      }
    }

    // Parabolic interpolation for sub-sample accuracy (YIN step 6)
    double period = static_cast<double>(bestPeriod);
    if (bestPeriod > minPeriod && bestPeriod < maxPeriod)
    {
      const double s0 = cmndf[static_cast<std::size_t>(bestPeriod - 1)];
      const double s1 = cmndf[static_cast<std::size_t>(bestPeriod)];
      const double s2 = cmndf[static_cast<std::size_t>(bestPeriod + 1)];
      const double denom = 2.0 * (2.0 * s1 - s0 - s2);
      if (std::abs(denom) > 1e-10)
      {
        period += (s2 - s0) / denom;
      }
    }

    return mSampleRate / period;
  }

  MultiPresetMixer::TunerResult MultiPresetMixer::FrequencyToNote(double frequency) const
  {
    TunerResult result;

    if (frequency < 20.0 || frequency > 20000.0)
    {
      result.detected = false;
      return result;
    }

    result.frequency = frequency;
    result.detected = true;

    // Calculate the number of semitones from A4 (reference frequency, typically 440 Hz)
    const double semitonesFromA4 = 12.0 * std::log2(frequency / mTunerReferenceFrequency);

    // Round to nearest semitone
    const int nearestSemitone = static_cast<int>(std::round(semitonesFromA4));

    // Calculate the exact frequency of the nearest note
    const double nearestFrequency = mTunerReferenceFrequency * std::pow(2.0, nearestSemitone / 12.0);

    // Calculate cents offset from the nearest note
    result.centOffset = 1200.0 * std::log2(frequency / nearestFrequency);

    // Clamp to reasonable range
    result.centOffset = std::clamp(result.centOffset, -50.0, 50.0);

    // Calculate note index (A4 is note 9 in octave 4, i.e., index 57 from C0)
    // A4 = 440Hz, note index = 9 (0=C, 1=C#, ..., 9=A)
    // Total semitone from C0 = nearestSemitone + 57 (A4 is 57 semitones above C0)
    const int totalSemitones = nearestSemitone + 57;

    // Handle negative semitones
    const int noteIndex = ((totalSemitones % 12) + 12) % 12;
    result.octave = (totalSemitones / 12);
    if (totalSemitones < 0 && totalSemitones % 12 != 0)
    {
      result.octave -= 1;
    }

    // Get note name with both sharp and flat
    const char *sharpName = kNoteNames[static_cast<std::size_t>(noteIndex)];
    const char *flatName = kNoteNamesFlat[static_cast<std::size_t>(noteIndex)];

    // Use combined notation for accidentals (e.g., "D#/Eb")
    if (std::string(sharpName) != std::string(flatName))
    {
      result.noteName = std::string(sharpName) + "/" + std::string(flatName);
    }
    else
    {
      result.noteName = sharpName;
    }

    // Calculate confidence based on how close we are to the note
    result.confidence = 1.0 - std::abs(result.centOffset) / 50.0;
    result.confidence = std::clamp(result.confidence, 0.0, 1.0);

    return result;
  }

} // namespace guitarfx
