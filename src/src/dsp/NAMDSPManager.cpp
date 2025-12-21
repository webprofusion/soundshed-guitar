#include "NAMDSPManager.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

namespace namguitar
{
  namespace
  {
    double DbToLinear(double decibels)
    {
      return std::pow(10.0, decibels / 20.0);
    }

    constexpr int kNumChannels = 2;

  } // namespace

  NAMDSPManager::NAMDSPManager() = default;
  NAMDSPManager::~NAMDSPManager() = default;

  void NAMDSPManager::Prepare(double sampleRate, int maxBlockSize)
  {
    const double previousSampleRate = mSampleRate;
    mSampleRate = sampleRate;
    mMaxBlockSize = std::max(1, maxBlockSize);

    mNamInput.resize(static_cast<std::size_t>(mMaxBlockSize));
    mNamOutput.resize(static_cast<std::size_t>(mMaxBlockSize));
    mIRState.assign(kNumChannels, IRHistory{});
    mGateEnvelope.assign(kNumChannels, 0.0);
    mToneLowState.assign(kNumChannels, 0.0);
    mToneHighState.assign(kNumChannels, 0.0);

    for (auto &model : mModels)
    {
      if (model)
      {
        model->ResetAndPrewarm(mSampleRate, mMaxBlockSize);
      }
    }

    // If sample rate changed and we have an IR loaded, reload it at the new sample rate
    if (std::abs(sampleRate - previousSampleRate) > 1.0)
    {
      const auto currentIR = mIRManager.CurrentImpulseResponse();
      if (currentIR)
      {
        LoadImpulseResponse(*currentIR);
      }
    }
  }

  void NAMDSPManager::Reset()
  {
    for (auto &model : mModels)
    {
      if (model)
      {
        model->ResetAndPrewarm(mSampleRate, mMaxBlockSize);
      }
    }

    // Reset all DSP state to avoid artifacts when switching presets
    std::fill(mGateEnvelope.begin(), mGateEnvelope.end(), 0.0);
    std::fill(mToneLowState.begin(), mToneLowState.end(), 0.0);
    std::fill(mToneHighState.begin(), mToneHighState.end(), 0.0);

    // Reset doubler delay buffers
    std::fill(mDoublerDelayBufferL.begin(), mDoublerDelayBufferL.end(), 0.0);
    std::fill(mDoublerDelayBufferR.begin(), mDoublerDelayBufferR.end(), 0.0);
    mDoublerWriteIndex = 0;

    // Reset IR convolution history
    for (auto &history : mIRState)
    {
      std::fill(history.buffer.begin(), history.buffer.end(), 0.0);
      history.writeIndex = 0;
    }
  }

  bool NAMDSPManager::LoadModel(const std::filesystem::path &modelPath)
  {
    try
    {
      for (int channel = 0; channel < kNumChannels; ++channel)
      {
        auto model = nam::get_dsp(modelPath);
        if (!model)
        {
          return false;
        }

        model->ResetAndPrewarm(mSampleRate, mMaxBlockSize);
        mModels[static_cast<std::size_t>(channel)] = std::move(model);
      }

      return true;
    }
    catch (...)
    {
      return false;
    }
  }

  bool NAMDSPManager::LoadImpulseResponse(const std::filesystem::path &impulsePath)
  {
    if (!mIRManager.LoadImpulseResponse(impulsePath, mSampleRate))
    {
      return false;
    }

    mIRState.assign(kNumChannels, IRHistory{});
    for (auto &history : mIRState)
    {
      history.buffer.assign(mIRManager.Impulse().size(), 0.0);
      history.writeIndex = 0;
    }
    return true;
  }

  void NAMDSPManager::SetInputTrim(double decibels)
  {
    mInputTrimDb = decibels;
    mInputTrimLinear = DbToLinear(decibels);
  }

  void NAMDSPManager::SetOutputTrim(double decibels)
  {
    mOutputTrimDb = decibels;
    mOutputTrimLinear = DbToLinear(decibels);
  }

  void NAMDSPManager::SetDrive(double amount)
  {
    mDriveAmount = std::clamp(amount, 0.0, 1.0);
  }

  void NAMDSPManager::SetTone(double tilt)
  {
    mToneTilt = std::clamp(tilt, -1.0, 1.0);
  }

  void NAMDSPManager::SetGateEnabled(bool enabled)
  {
    mGateEnabled = enabled;
  }

  void NAMDSPManager::SetGateThreshold(double decibels)
  {
    mGateThreshold = decibels;
  }

  void NAMDSPManager::SetMix(double mix)
  {
    mMix = std::clamp(mix, 0.0, 1.0);
  }

  void NAMDSPManager::SetDoublerEnabled(bool enabled)
  {
    mDoublerEnabled = enabled;
  }

  void NAMDSPManager::SetDoublerDelay(double milliseconds)
  {
    mDoublerDelayMs = std::clamp(milliseconds, 0.5, 50.0);
    mDoublerDelaySamples = static_cast<int>(mDoublerDelayMs * mSampleRate / 1000.0);
    
    // Resize delay buffers if needed
    const std::size_t requiredSize = static_cast<std::size_t>(mDoublerDelaySamples + mMaxBlockSize);
    if (mDoublerDelayBufferL.size() < requiredSize)
    {
      mDoublerDelayBufferL.resize(requiredSize, 0.0);
      mDoublerDelayBufferR.resize(requiredSize, 0.0);
    }
  }

  void NAMDSPManager::Process(iplug::sample **inputs, iplug::sample **outputs, int nFrames)
  {
    const auto frames = std::min(nFrames, mMaxBlockSize);

    if (!inputs || !outputs)
    {
      return;
    }

    const auto impulseSize = static_cast<int>(mIRManager.Impulse().size());

    // Process both channels through the amp model
    std::vector<double> processedL(frames, 0.0);
    std::vector<double> processedR(frames, 0.0);
    std::vector<double>* channelBuffers[kNumChannels] = {&processedL, &processedR};

    for (int channel = 0; channel < kNumChannels; ++channel)
    {
      iplug::sample *inputChannel = inputs[channel];
      if (!inputChannel)
      {
        continue;
      }

      std::vector<double>& channelBuffer = *channelBuffers[channel];
      for (int frame = 0; frame < frames; ++frame)
      {
        double sample = static_cast<double>(inputChannel[frame]) * mInputTrimLinear;
        sample = ApplyGate(sample, channel);
        sample = ApplyDrive(sample);
        sample = ApplyTone(sample, channel);
        mNamInput[static_cast<std::size_t>(frame)] = static_cast<NAM_SAMPLE>(sample);
      }

      auto &model = mModels[static_cast<std::size_t>(channel)];
      if (model)
      {
        model->process(mNamInput.data(), mNamOutput.data(), frames);
        for (int frame = 0; frame < frames; ++frame)
        {
          channelBuffer[frame] = static_cast<double>(mNamOutput[static_cast<std::size_t>(frame)]);
        }
      }
      else
      {
        for (int frame = 0; frame < frames; ++frame)
        {
          channelBuffer[frame] = static_cast<double>(mNamInput[static_cast<std::size_t>(frame)]);
        }
      }

      if (impulseSize > 0 && static_cast<std::size_t>(channel) < mIRState.size())
      {
       // ApplyImpulseResponse(channelBuffer, channel);
      }
    }

    // Apply doubler effect if enabled
    // The doubler creates a stereo widening effect by:
    // - Left channel: original signal (or slightly delayed)
    // - Right channel: delayed copy of the signal
    // This creates a "doubled" guitar sound common in recordings
    if (mDoublerEnabled && mDoublerDelaySamples > 0)
    {
      // Ensure delay buffers are properly sized
      const std::size_t bufferSize = static_cast<std::size_t>(mDoublerDelaySamples + mMaxBlockSize + 1);
      if (mDoublerDelayBufferL.size() < bufferSize)
      {
        mDoublerDelayBufferL.resize(bufferSize, 0.0);
        mDoublerDelayBufferR.resize(bufferSize, 0.0);
        mDoublerWriteIndex = 0;
      }

      // Create mono sum for doubler processing
      std::vector<double> monoSignal(frames);
      for (int frame = 0; frame < frames; ++frame)
      {
        monoSignal[frame] = (processedL[frame] + processedR[frame]) * 0.5;
      }

      // Process doubler: left gets direct signal, right gets delayed signal
      for (int frame = 0; frame < frames; ++frame)
      {
        // Write current sample to delay buffer
        mDoublerDelayBufferL[mDoublerWriteIndex] = monoSignal[frame];
        
        // Read delayed sample for right channel
        std::size_t readIndex = mDoublerWriteIndex >= static_cast<std::size_t>(mDoublerDelaySamples)
          ? mDoublerWriteIndex - static_cast<std::size_t>(mDoublerDelaySamples)
          : mDoublerDelayBufferL.size() - (static_cast<std::size_t>(mDoublerDelaySamples) - mDoublerWriteIndex);
        
        // Left channel: direct mono signal
        processedL[frame] = monoSignal[frame];
        // Right channel: delayed mono signal
        processedR[frame] = mDoublerDelayBufferL[readIndex];
        
        // Advance write index
        mDoublerWriteIndex = (mDoublerWriteIndex + 1) % mDoublerDelayBufferL.size();
      }
    }

    // Output final signal with mix and output trim
    for (int channel = 0; channel < kNumChannels; ++channel)
    {
      iplug::sample *inputChannel = inputs[channel];
      iplug::sample *outputChannel = outputs[channel];
      if (!inputChannel || !outputChannel)
      {
        continue;
      }

      std::vector<double>& channelBuffer = *channelBuffers[channel];
      
      // Blend wet (processed) and dry (original) signals based on mix parameter
      for (int frame = 0; frame < frames; ++frame)
      {
        const double wetSample = channelBuffer[frame] * mOutputTrimLinear;
        const double drySample = static_cast<double>(inputChannel[frame]);
        outputChannel[frame] = static_cast<iplug::sample>(wetSample * mMix + drySample * (1.0 - mMix));
      }
    }
  }

  double NAMDSPManager::ApplyDrive(double sample) const
  {
    // Soft clip the signal to emulate preamp saturation before it hits the NAM block.
    const double drive = 1.0 + mDriveAmount * 9.0;
    const double driven = std::tanh(sample * drive);
    return driven;
  }

  double NAMDSPManager::ApplyTone(double sample, int channel)
  {
    // Split the spectrum with a one-pole filter pair so we can tilt towards lows or highs.
    const double cutoff = 400.0;
    const double omega = 2.0 * std::numbers::pi * cutoff / mSampleRate;
    const double alpha = omega / (omega + 1.0);

    const double low = alpha * sample + (1.0 - alpha) * mToneLowState[static_cast<std::size_t>(channel)];
    const double high = sample - low;
    mToneLowState[static_cast<std::size_t>(channel)] = low;
    mToneHighState[static_cast<std::size_t>(channel)] = high;

    const double mix = (mToneTilt + 1.0) * 0.5; // 0..1
    return low * (1.0 - mix) + high * mix;
  }

  double NAMDSPManager::ApplyGate(double sample, int channel)
  {
    if (!mGateEnabled)
    {
      return sample;
    }

    // Track the envelope with simple attack/release smoothing; anything below threshold is muted.
    const double absSample = std::abs(sample);
    const double attack = 0.01;
    const double release = 0.2;
    const double coeff = absSample > mGateEnvelope[static_cast<std::size_t>(channel)] ? attack : release;
    mGateEnvelope[static_cast<std::size_t>(channel)] = coeff * absSample + (1.0 - coeff) * mGateEnvelope[static_cast<std::size_t>(channel)];

    const double thresholdLinear = DbToLinear(mGateThreshold);
    if (mGateEnvelope[static_cast<std::size_t>(channel)] < thresholdLinear)
    {
      return 0.0;
    }

    return sample;
  }

  // Applies convolution with a cabinet impulse response (IR) to simulate speaker characteristics.
  //
  // Convolution "imprints" the acoustic characteristics of a recorded speaker cabinet onto the
  // input signal. The IR captures how a speaker responds to an impulse, encoding its frequency
  // response and resonances.
  //
  // This implements direct-form FIR (Finite Impulse Response) convolution:
  //   output[n] = sum(input[n-k] * impulse[k]) for k = 0 to N-1
  //
  // We use a circular buffer to store the last N input samples efficiently, avoiding the need
  // to shift samples on each iteration.
  void NAMDSPManager::ApplyImpulseResponse(std::vector<double> &channelSamples, int channel) const
  {
    // Take a snapshot copy of the impulse response for thread safety.
    // This ensures we work with consistent data even if LoadImpulseResponse is called
    // on another thread during processing.
    const std::vector<float> impulse = mIRManager.Impulse();
    const std::size_t irLength = impulse.size();
    
    if (irLength == 0)
    {
      return;
    }

    // Get the history buffer for this channel
    auto &history = mIRState[static_cast<std::size_t>(channel)];
    
    // Ensure history buffer is correctly sized for this IR
    if (history.buffer.size() != irLength)
    {
      history.buffer.resize(irLength, 0.0);
      history.writeIndex = 0;
    }
    
    // Validate write index is in bounds
    if (history.writeIndex >= irLength)
    {
      history.writeIndex = 0;
    }

    // Process each input sample
    for (std::size_t i = 0; i < channelSamples.size(); ++i)
    {
      // Store the new input sample in the circular buffer
      history.buffer[history.writeIndex] = channelSamples[i];

      // Compute convolution sum: multiply each past sample by corresponding IR coefficient
      double output = 0.0;
      
      for (std::size_t k = 0; k < irLength; ++k)
      {
        // Calculate index into circular buffer for sample[n-k]
        // We want: current sample (k=0) at writeIndex, older samples going backwards
        std::size_t sampleIndex = (history.writeIndex >= k) 
            ? history.writeIndex - k 
            : irLength - (k - history.writeIndex);
            
        output += history.buffer[sampleIndex] * static_cast<double>(impulse[k]);
      }

      // Write convolved output
      channelSamples[i] = output;

      // Advance write position (circular)
      history.writeIndex = (history.writeIndex + 1) % irLength;
    }
  }

  bool NAMDSPManager::HasModel() const noexcept
  {
    return mModels[0] != nullptr;
  }

  bool NAMDSPManager::HasImpulseResponse() const noexcept
  {
    return mIRManager.HasImpulse();
  }

} // namespace namguitar
