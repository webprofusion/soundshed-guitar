#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include <filesystem>
#include <memory>
#include <vector>
#include <cmath>

namespace namguitar
{
  /**
   * Neural Amp Modeler effect - wrapper around NAM DSP library.
   * Requires a .nam model file to be loaded as a resource.
   */
  class NAMAmpEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      mInputBuffer.resize(static_cast<size_t>(maxBlockSize));
      mOutputBuffer.resize(static_cast<size_t>(maxBlockSize));

      if (mModel)
      {
        mModel->Reset(sampleRate, maxBlockSize);
      }
    }

    void Reset() override
    {
      if (mModel)
      {
        mModel->Reset(mSampleRate, mMaxBlockSize);
      }

      std::fill(mInputBuffer.begin(), mInputBuffer.end(), 0.0f);
      std::fill(mOutputBuffer.begin(), mOutputBuffer.end(), 0.0f);
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
      if (!inputs[0] && !inputs[1])
      {
        if (outputs[0]) std::fill_n(outputs[0], numSamples, 0.0f);
        if (outputs[1]) std::fill_n(outputs[1], numSamples, 0.0f);
        return;
      }

      // Sum to mono for NAM processing (NAM models are mono)
      for (int i = 0; i < numSamples; ++i)
      {
        float inL = inputs[0] ? inputs[0][i] : 0.0f;
        float inR = inputs[1] ? inputs[1][i] : inL;
        mInputBuffer[i] = (inL + inR) * 0.5f * static_cast<float>(mInputGain);
      }

      if (mModel && mEnabled)
      {
        mModel->process(mInputBuffer.data(), mOutputBuffer.data(), numSamples);

        // Output to both channels
        for (int i = 0; i < numSamples; ++i)
        {
          float out = mOutputBuffer[i] * static_cast<float>(mOutputGain);
          if (outputs[0]) outputs[0][i] = out;
          if (outputs[1]) outputs[1][i] = out;
        }
      }
      else
      {
        // Bypass - pass through mono signal
        for (int i = 0; i < numSamples; ++i)
        {
          float out = mInputBuffer[i];
          if (outputs[0]) outputs[0][i] = out;
          if (outputs[1]) outputs[1][i] = out;
        }
      }
    }

    void SetParam(const std::string& key, double value) override
    {
      if (key == "inputGain")
        mInputGain = std::pow(10.0, std::clamp(value, -24.0, 24.0) / 20.0);
      else if (key == "outputGain")
        mOutputGain = std::pow(10.0, std::clamp(value, -24.0, 24.0) / 20.0);
      else if (key == "enabled")
        mEnabled = value > 0.5;
    }

    void SetConfig(const std::string&, const std::string&) override {}

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
      if (key == "inputGain")
        return 20.0 * std::log10(mInputGain);
      if (key == "outputGain")
        return 20.0 * std::log10(mOutputGain);
      if (key == "enabled")
        return mEnabled ? 1.0 : 0.0;
      return 0.0;
    }

    bool LoadResource(const std::filesystem::path& resourcePath) override
    {
      try
      {
        auto model = nam::get_dsp(resourcePath);
        if (!model)
          return false;

        model->Reset(mSampleRate, mMaxBlockSize);
        mModel = std::move(model);
        mModelPath = resourcePath;
        return true;
      }
      catch (...)
      {
        return false;
      }
    }

    [[nodiscard]] bool HasResource() const override { return mModel != nullptr; }
    [[nodiscard]] std::filesystem::path GetResourcePath() const override { return mModelPath; }

    [[nodiscard]] std::string GetType() const override { return "amp_nam"; }
    [[nodiscard]] std::string GetCategory() const override { return "amp"; }

  private:
    std::unique_ptr<nam::DSP> mModel;
    std::filesystem::path mModelPath;

    std::vector<NAM_SAMPLE> mInputBuffer;
    std::vector<NAM_SAMPLE> mOutputBuffer;

    double mInputGain = 1.0;
    double mOutputGain = 1.0;
    bool mEnabled = true;
  };

  inline void RegisterNAMAmpEffect()
  {
    EffectTypeInfo info;
    info.type = "amp_nam";
    info.displayName = "NAM Amp Model";
    info.category = "amp";
    info.description = "Neural Amp Modeler - AI-trained amp simulation";
    info.requiresResource = true;
    info.resourceType = "nam";  // .nam model files
    info.parameters = {
      {"inputGain", "Input Gain", 0.0, -24.0, 24.0, "dB"},
      {"outputGain", "Output Gain", 0.0, -24.0, 24.0, "dB"}
    };

    EffectRegistry::Instance().Register("amp_nam", info, []() {
      return std::make_unique<NAMAmpEffect>();
    });
  }

} // namespace namguitar
