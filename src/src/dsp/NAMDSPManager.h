#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <vector>

#include "IPlugConstants.h"
#include "NAM/dsp.h"
#include "dsp/IRManager.h"

namespace nam
{
  class DSP;
} // namespace nam

namespace iplug
{
  class IParam;
}

namespace namguitar
{
  class NAMDSPManager
  {
  public:
    NAMDSPManager();
    ~NAMDSPManager();

    void Prepare(double sampleRate, int maxBlockSize);
    void Reset();
    bool LoadModel(const std::filesystem::path &modelPath);
    bool LoadImpulseResponse(const std::filesystem::path &impulsePath);

    void SetInputTrim(double decibels);
    void SetOutputTrim(double decibels);
    void SetDrive(double amount);
    void SetTone(double tilt); // -1..1 tilt high/low emphasis
    void SetGateEnabled(bool enabled);
    void SetGateThreshold(double decibels);
    void SetMix(double mix);
    void SetDoublerEnabled(bool enabled);
    void SetDoublerDelay(double milliseconds);

    void Process(iplug::sample **inputs, iplug::sample **outputs, int nFrames);

    // Test accessors
    [[nodiscard]] bool HasModel() const noexcept;
    [[nodiscard]] bool HasImpulseResponse() const noexcept;

  private:
    double ApplyDrive(double sample) const;
    double ApplyTone(double sample, int channel);
    double ApplyGate(double sample, int channel);
    void ApplyImpulseResponse(std::vector<double> &channelSamples, int channel) const;

    std::array<std::unique_ptr<nam::DSP>, 2> mModels;
    IRManager mIRManager;

    std::vector<NAM_SAMPLE> mNamInput;
    std::vector<NAM_SAMPLE> mNamOutput;

    std::vector<double> mGateEnvelope;
    std::vector<double> mToneLowState;
    std::vector<double> mToneHighState;

    double mSampleRate = 44100.0;
    int mMaxBlockSize = 512;

    double mInputTrimDb = 0.0;
    double mOutputTrimDb = 0.0;
    double mInputTrimLinear = 1.0;
    double mOutputTrimLinear = 1.0;
    double mDriveAmount = 1.0;
    double mToneTilt = 0.0;
    bool mGateEnabled = false;
    double mGateThreshold = -60.0;
    double mMix = 1.0; // 0.0 = fully dry, 1.0 = fully wet

    // Doubler effect state
    bool mDoublerEnabled = false;
    double mDoublerDelayMs = 6.0; // Default 6ms separation
    int mDoublerDelaySamples = 0;
    std::vector<double> mDoublerDelayBufferL;
    std::vector<double> mDoublerDelayBufferR;
    std::size_t mDoublerWriteIndex = 0;

    struct IRHistory
    {
      std::vector<double> buffer;
      std::size_t writeIndex = 0;
    };

    mutable std::vector<IRHistory> mIRState;
  };
} // namespace namguitar
