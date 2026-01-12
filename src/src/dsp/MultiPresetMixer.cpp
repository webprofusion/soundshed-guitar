#include "dsp/MultiPresetMixer.h"
#include "resources/ResourceLibrary.h"

#include <cmath>
#include <array>

namespace guitarfx
{
  namespace
  {
    // Note names for pitch detection
    constexpr std::array<const char *, 12> kNoteNames = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

    // Alternative note names (flats)
    constexpr std::array<const char *, 12> kNoteNamesFlat = {
        "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
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

    inst.executor.SetResourceLibrary(mResourceLibrary);
    inst.executor.SetGraph(preset.graph);

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

  // Global gate control (routes to dynamics_gate nodes in signal chain)
  void MultiPresetMixer::SetGateEnabled(bool enabled)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("dynamics_gate");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeEnabled(nodeId, enabled);
      }
    }
  }

  void MultiPresetMixer::SetGateThreshold(double thresholdDb)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("dynamics_gate");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "threshold", thresholdDb);
      }
    }
  }

  // Global doubler control (routes to delay_doubler nodes in signal chain)
  void MultiPresetMixer::SetDoublerEnabled(bool enabled)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("delay_doubler");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeEnabled(nodeId, enabled);
      }
    }
  }

  void MultiPresetMixer::SetDoublerDelay(double delayMs)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("delay_doubler");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "time", std::clamp(delayMs, 0.5, 100.0));
      }
    }
  }

  // Global transpose control (routes to pitch_shift nodes in signal chain)
  void MultiPresetMixer::SetTranspose(int semitones)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("pitch_shift");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "semitones", static_cast<double>(std::clamp(semitones, -24, 24)));
      }
    }
  }

  void MultiPresetMixer::SetAmpDrive(double value)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("amp_nam");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "inputGain", value);
      }
    }
  }

  void MultiPresetMixer::SetSimpleCabEnabled(bool enabled)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("cab_simple");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeEnabled(nodeId, enabled);
      }
    }
  }

  void MultiPresetMixer::SetSimpleCabBass(double value)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("cab_simple");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "bass", value);
      }
    }
  }

  void MultiPresetMixer::SetSimpleCabPresence(double value)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("cab_simple");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "presence", value);
      }
    }
  }

  void MultiPresetMixer::SetSimpleCabBrightness(double value)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("cab_simple");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "brightness", value);
      }
    }
  }

  void MultiPresetMixer::SetIRQuality(double value)
  {
    for (auto &inst : mInstances)
    {
      auto nodeId = inst.executor.FindFirstNodeOfType("cab_ir");
      if (nodeId.empty())
      {
        nodeId = inst.executor.FindFirstNodeOfType("ir_cab");
      }
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "quality", value);
      }
    }
  }

  void MultiPresetMixer::SetEQEnabled(bool enabled)
  {
    mGlobalEQEnabled = enabled;
  }

  void MultiPresetMixer::SetEQBandGain(int band, double value)
  {
    static const char *kParamNames[] = {"lowGain", "lowMidGain", "highMidGain", "highGain"};
    if (band < 0 || band > 3)
      return;

    mGlobalEQ.SetParam(kParamNames[band], value);
  }

  void MultiPresetMixer::SetEQBandFrequency(int band, double value)
  {
    static const char *kParamNames[] = {"lowFreq", "lowMidFreq", "highMidFreq", "highFreq"};
    if (band < 0 || band > 3)
      return;

    mGlobalEQ.SetParam(kParamNames[band], value);
  }

  void MultiPresetMixer::SetEQBandQ(int band, double value)
  {
    static const char *kParamNames[] = {"", "lowMidQ", "highMidQ", ""};
    if (band < 1 || band > 2)
      return;

    mGlobalEQ.SetParam(kParamNames[band], value);
  }

  void MultiPresetMixer::SetDelayEnabled(bool enabled)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("delay_digital");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeEnabled(nodeId, enabled);
      }
    }
  }

  void MultiPresetMixer::SetDelayTime(double value)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("delay_digital");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "timeMs", value);
      }
    }
  }

  void MultiPresetMixer::SetDelayFeedback(double value)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("delay_digital");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "feedback", value);
      }
    }
  }

  void MultiPresetMixer::SetDelayMix(double value)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("delay_digital");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "mix", value);
      }
    }
  }

  void MultiPresetMixer::SetReverbEnabled(bool enabled)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("reverb_room");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeEnabled(nodeId, enabled);
      }
    }
  }

  void MultiPresetMixer::SetReverbDecay(double value)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("reverb_room");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "decay", value);
      }
    }
  }

  void MultiPresetMixer::SetReverbDamping(double value)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("reverb_room");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "damping", value);
      }
    }
  }

  void MultiPresetMixer::SetReverbMix(double value)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("reverb_room");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "mix", value);
      }
    }
  }

  void MultiPresetMixer::SetAmpTone(double value)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("amp_nam");
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

  bool MultiPresetMixer::LoadNodeResource(const std::string &presetId, const std::string &nodeId, const ResourceRef &ref)
  {
    if (auto *inst = FindInstance(presetId))
    {
      return inst->executor.LoadNodeResource(nodeId, ref);
    }
    return false;
  }

  void MultiPresetMixer::Prepare(double sampleRate, int maxBlockSize)
  {
    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;
    mPrepared = true;

    // Allocate global temp buffers
    mTempInL.resize(static_cast<size_t>(maxBlockSize), 0.0f);
    mTempInR.resize(static_cast<size_t>(maxBlockSize), 0.0f);

    // Prepare global post-mix EQ
    mGlobalEQ.Prepare(sampleRate, maxBlockSize);

    AllocateBuffers(maxBlockSize);

    for (auto &inst : mInstances)
    {
      inst.executor.Prepare(sampleRate, maxBlockSize);
      AllocateInstanceBuffers(inst, maxBlockSize);
    }
  }

  void MultiPresetMixer::Reset()
  {
    mGlobalEQ.Reset();
    for (auto &inst : mInstances)
    {
      inst.executor.Reset();
    }
  }

  void MultiPresetMixer::Process(float **inputs, float **outputs, int numSamples)
  {
    if (!outputs || numSamples <= 0)
      return;

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
        const float targetGain = 0.7f / peak;
        const float limitedGain = std::min(targetGain, 4.0f); // Max 4x gain
        mInputAutoLevelGain = mInputAutoLevelGain * 0.99f + limitedGain * 0.01f;

        for (int i = 0; i < numSamples; ++i)
        {
          mTempInL[static_cast<std::size_t>(i)] = processInL[i] * mInputAutoLevelGain;
          mTempInR[static_cast<std::size_t>(i)] = processInR[i] * mInputAutoLevelGain;
        }
        processInL = mTempInL.data();
        processInR = mTempInR.data();
      }
    }

    // Process tuner (sits at the start of signal chain, uses raw input for pitch detection)
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

    // Clear output
    if (outputs[0])
    {
      std::fill(outputs[0], outputs[0] + numSamples, 0.0f);
    }
    if (outputs[1])
    {
      std::fill(outputs[1], outputs[1] + numSamples, 0.0f);
    }

    // Process each preset and mix
    float *inputPtrs[2] = {processInL, processInR};
    for (auto &inst : mInstances)
    {
      const bool include = (!inst.cfg.mute) && (!anySolo || inst.cfg.solo);
      if (!include)
        continue;

      // Process through signal chain (gate/pitch/doubler are now nodes in the graph)
      float *presetOutPtrs[2] = {inst.outL.data(), inst.outR.data()};
      inst.executor.Process(inputPtrs, presetOutPtrs, numSamples);

      // Apply per-preset pan (equal-power) and mix gain
      float gL = 1.0f, gR = 1.0f;
      ComputePanGains(inst.cfg.pan, gL, gR);
      const float mixGain = static_cast<float>(inst.cfg.mix);

      for (int i = 0; i < numSamples; ++i)
      {
        if (outputs[0])
        {
          outputs[0][i] += inst.outL[static_cast<size_t>(i)] * mixGain * gL;
        }
        if (outputs[1])
        {
          outputs[1][i] += inst.outR[static_cast<size_t>(i)] * mixGain * gR;
        }
      }
    }

    // Apply global post-mix EQ (after preset mixing, before pitch/doubler)
    if (mGlobalEQEnabled && outputs[0] && outputs[1])
    {
      mGlobalEQ.Process(outputs, outputs, numSamples);
    }

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
      float peak = 0.0f;
      for (int i = 0; i < numSamples; ++i)
      {
        if (outputs[0])
          peak = std::max(peak, std::abs(outputs[0][i]));
        if (outputs[1])
          peak = std::max(peak, std::abs(outputs[1][i]));
      }

      if (peak > 0.95f)
      {
        const float attenuation = 0.95f / peak;
        mOutputAutoLevelGain = std::min(mOutputAutoLevelGain, attenuation);
        mOutputAutoLevelGain = mOutputAutoLevelGain * 0.99f + attenuation * 0.01f;

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
        mOutputAutoLevelGain = std::min(1.0f, mOutputAutoLevelGain * 1.0001f);
      }
    }

    // Optional simple limiter (clip)
    if (mLimiterEnabled)
    {
      if (outputs[0])
      {
        for (int i = 0; i < numSamples; ++i)
          outputs[0][i] = std::clamp(outputs[0][i], -1.0f, 1.0f);
      }
      if (outputs[1])
      {
        for (int i = 0; i < numSamples; ++i)
          outputs[1][i] = std::clamp(outputs[1][i], -1.0f, 1.0f);
      }
    }
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

    if (mInstances.empty())
    {
      return aggregatedStats;
    }

    // Aggregate stats from all active preset instances
    for (const auto& instance : mInstances)
    {
      auto instanceStats = instance.executor.GetPerformanceStats();

      // Sum up total processing time
      aggregatedStats.totalProcessingTimeUs += instanceStats.totalProcessingTimeUs;

      // Use the maximum real time (since they process in parallel)
      if (instanceStats.realTimeUs > aggregatedStats.realTimeUs)
      {
        aggregatedStats.realTimeUs = instanceStats.realTimeUs;
      }

      // Calculate average DSP load across instances
      aggregatedStats.dspLoadPercent += instanceStats.dspLoadPercent;

      // Merge node processing times
      for (const auto& [nodeId, timeUs] : instanceStats.nodeProcessingTimesUs)
      {
        aggregatedStats.nodeProcessingTimesUs[nodeId] += timeUs;
      }
    }

    // Average the DSP load across all instances
    if (!mInstances.empty())
    {
      aggregatedStats.dspLoadPercent /= static_cast<double>(mInstances.size());
    }

    return aggregatedStats;
  }

  // ============================================================================
  // Tuner implementation
  // ============================================================================

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
    if (rms < 0.01)
    {
      return 0.0;
    }

    // Define search range for guitar: 70Hz (D2) to 1500Hz (F#6)
    const int minPeriod = static_cast<int>(mSampleRate / 1500.0); // Highest frequency
    const int maxPeriod = static_cast<int>(mSampleRate / 70.0);   // Lowest frequency

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
