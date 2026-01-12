#include "dsp/MultiPresetMixer.h"

#include <cmath>

namespace guitarfx
{
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

  void MultiPresetMixer::SetGateThreshold(double value)
  {
    for (auto &inst : mInstances)
    {
      const auto nodeId = inst.executor.FindFirstNodeOfType("dynamics_gate");
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, "thresholdDb", value);
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
    for (auto &inst : mInstances)
    {
      auto nodeId = inst.executor.FindFirstNodeOfType("eq_parametric");
      if (nodeId.empty())
      {
        nodeId = inst.executor.FindFirstNodeOfType("eq");
      }
      if (!nodeId.empty())
      {
        inst.executor.SetNodeEnabled(nodeId, enabled);
      }
    }
  }

  void MultiPresetMixer::SetEQBandGain(int band, double value)
  {
    static const char *kParamNames[] = {"lowGain", "lowMidGain", "highMidGain", "highGain"};
    if (band < 0 || band > 3)
      return;

    const std::string param = kParamNames[band];
    for (auto &inst : mInstances)
    {
      auto nodeId = inst.executor.FindFirstNodeOfType("eq_parametric");
      if (nodeId.empty())
      {
        nodeId = inst.executor.FindFirstNodeOfType("eq");
      }
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, param, value);
      }
    }
  }

  void MultiPresetMixer::SetEQBandFrequency(int band, double value)
  {
    static const char *kParamNames[] = {"lowFreq", "lowMidFreq", "highMidFreq", "highFreq"};
    if (band < 0 || band > 3)
      return;

    const std::string param = kParamNames[band];
    for (auto &inst : mInstances)
    {
      auto nodeId = inst.executor.FindFirstNodeOfType("eq_parametric");
      if (nodeId.empty())
      {
        nodeId = inst.executor.FindFirstNodeOfType("eq");
      }
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, param, value);
      }
    }
  }

  void MultiPresetMixer::SetEQBandQ(int band, double value)
  {
    static const char *kParamNames[] = {"", "lowMidQ", "highMidQ", ""};
    if (band < 1 || band > 2)
      return;

    const std::string param = kParamNames[band];
    for (auto &inst : mInstances)
    {
      auto nodeId = inst.executor.FindFirstNodeOfType("eq_parametric");
      if (nodeId.empty())
      {
        nodeId = inst.executor.FindFirstNodeOfType("eq");
      }
      if (!nodeId.empty())
      {
        inst.executor.SetNodeParam(nodeId, param, value);
      }
    }
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

  void MultiPresetMixer::Prepare(double sampleRate, int maxBlockSize)
  {
    mSampleRate = sampleRate;
    mMaxBlockSize = maxBlockSize;
    mPrepared = true;

    AllocateBuffers(maxBlockSize);

    for (auto &inst : mInstances)
    {
      inst.executor.Prepare(sampleRate, maxBlockSize);
    }
  }

  void MultiPresetMixer::Reset()
  {
    for (auto &inst : mInstances)
    {
      inst.executor.Reset();
    }
  }

  void MultiPresetMixer::Process(float **inputs, float **outputs, int numSamples)
  {
    if (!outputs || numSamples <= 0)
      return;

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
    for (auto &inst : mInstances)
    {
      const bool include = (!inst.cfg.mute) && (!anySolo || inst.cfg.solo);
      if (!include)
        continue;

      // Process into per-instance buffers
      float *presetOutPtrs[2] = {inst.outL.data(), inst.outR.data()};
      inst.executor.Process(inputs, presetOutPtrs, numSamples);

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
    }
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

} // namespace guitarfx
