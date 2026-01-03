#include "NAMDSPManager.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <array>
#include <iostream>
#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "RealtimeConvolver.h"

namespace namguitar
{
  namespace
  {
    double DbToLinear(double decibels)
    {
      return std::pow(10.0, decibels / 20.0);
    }

    constexpr int kNumChannels = 2;

    // Note names for pitch detection
    constexpr std::array<const char*, 12> kNoteNames = {
      "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    
    // Alternative note names (flats)
    constexpr std::array<const char*, 12> kNoteNamesFlat = {
      "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"
    };

  } // namespace

  NAMDSPManager::NAMDSPManager()
  {
    // Ensure NAM factory registrations are not optimized out by the linker
    nam::factory::ForceFactoryRegistration();
  }

  NAMDSPManager::~NAMDSPManager() = default;

  void NAMDSPManager::Prepare(double sampleRate, int maxBlockSize)
  {
    const double previousSampleRate = mSampleRate;
    mSampleRate = sampleRate;
    mMaxBlockSize = std::max(1, maxBlockSize);

    mNamInput.resize(static_cast<std::size_t>(mMaxBlockSize));
    mNamOutput.resize(static_cast<std::size_t>(mMaxBlockSize));
    mGateEnvelope.assign(kNumChannels, 0.0);
    mToneLowState.assign(kNumChannels, 0.0);
    mToneHighState.assign(kNumChannels, 0.0);

    // Initialize tuner buffer
    mTunerBuffer.resize(kTunerBufferSize, 0.0);
    mTunerBufferWriteIndex = 0;
    mTunerSampleCounter = 0;

    // Reset models with new sample rate and block size
    // NAM models handle internal resampling via Reset, so always call it
    for (auto &model : mModels)
    {
      if (model)
      {
        model->Reset(mSampleRate, mMaxBlockSize);
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

    // Initialize simple cabinet simulation
    for (auto& cabSim : mSimpleCabSim)
    {
      cabSim.Prepare(mSampleRate);
    }

    // Initialize parametric EQ
    for (auto& eq : mParametricEQ)
    {
      eq.Prepare(mSampleRate);
    }
  }

  void NAMDSPManager::Reset()
  {
    for (auto &model : mModels)
    {
      if (model)
      {
        model->Reset(mSampleRate, mMaxBlockSize);
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

    // Reset FFT convolution state
    for (auto &convolution : mIRConvolution)
    {
      convolution.Reset();
    }

    // Reset simple cabinet simulation
    for (auto& cabSim : mSimpleCabSim)
    {
      cabSim.Reset();
    }

    // Reset parametric EQ
    for (auto& eq : mParametricEQ)
    {
      eq.Reset();
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

        model->Reset(mSampleRate, mMaxBlockSize);
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

    // Initialize FFT convolution for both channels with the processed (possibly truncated) IR
    const auto processedImpulse = mIRManager.GetProcessedImpulse(mSampleRate);
    for (auto &convolution : mIRConvolution)
    {
      convolution.SetImpulse(processedImpulse, mMaxBlockSize);
    }
    
    return true;
  }

  void NAMDSPManager::SetIRQuality(IRQuality quality)
  {
    if (mIRManager.GetQuality() == quality)
    {
      return;
    }
    
    mIRManager.SetQuality(quality);
    
    // Re-initialize convolution with the new truncation settings if we have an IR loaded
    if (mIRManager.HasImpulse())
    {
      const auto processedImpulse = mIRManager.GetProcessedImpulse(mSampleRate);
      for (auto &convolution : mIRConvolution)
      {
        convolution.SetImpulse(processedImpulse, mMaxBlockSize);
      }
    }
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

  void NAMDSPManager::SetSimpleCabBass(double bass)
  {
    for (auto& cabSim : mSimpleCabSim)
    {
      cabSim.SetBass(bass);
    }
  }

  void NAMDSPManager::SetSimpleCabPresence(double presence)
  {
    for (auto& cabSim : mSimpleCabSim)
    {
      cabSim.SetPresence(presence);
    }
  }

  void NAMDSPManager::SetSimpleCabBrightness(double brightness)
  {
    for (auto& cabSim : mSimpleCabSim)
    {
      cabSim.SetBrightness(brightness);
    }
  }

  void NAMDSPManager::SetEQBandGain(int band, double gainDb)
  {
    for (auto& eq : mParametricEQ)
    {
      eq.SetBandGain(band, gainDb);
    }
  }

  void NAMDSPManager::SetEQBandFrequency(int band, double freqHz)
  {
    for (auto& eq : mParametricEQ)
    {
      eq.SetBandFrequency(band, freqHz);
    }
  }

  void NAMDSPManager::SetEQBandQ(int band, double q)
  {
    for (auto& eq : mParametricEQ)
    {
      eq.SetBandQ(band, q);
    }
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

  void NAMDSPManager::SetTranspose(int semitones)
  {
    mTransposeSemitones = std::clamp(semitones, -12, 12);
    
    // Calculate pitch ratio: 2^(semitones/12)
    mPitchRatio = std::pow(2.0, static_cast<double>(mTransposeSemitones) / 12.0);
    
    // Resize pitch buffers for time-stretching
    const std::size_t bufferSize = static_cast<std::size_t>(mMaxBlockSize * 4);
    if (mPitchBufferL.size() < bufferSize)
    {
      mPitchBufferL.resize(bufferSize, 0.0);
      mPitchBufferR.resize(bufferSize, 0.0);
      mPitchReadIndex = 0;
      mPitchWriteIndex = 0;
      mPitchPhase = 0.0;
    }
  }

  void NAMDSPManager::Process(iplug::sample **inputs, iplug::sample **outputs, int nFrames)
  {

    /*
    Processing Pipeline
    1. Buffer Validation & Initialization

    Validates input/output pointers
    Resizes working buffers if needed (processed samples, pitch-shifted, gate envelope, etc.)
    2. Per-Channel Processing (stereo)
    For each audio channel:

    Input Trim: Scales input by mInputTrimLinear
    Gate: Noise gate (mutes below threshold if enabled)
    Drive: Soft clipping/saturation using tanh to emulate preamp
    Tone: Splits spectrum with one-pole filter pair, allowing tilt toward lows/highs
    NAM Model: Passes processed samples through the neural amp model via model->process()
    IR Convolution: Optional cabinet simulation (currently disabled)
    3. Doubler Effect (if enabled)

    Creates stereo widening by delaying the right channel relative to left
    Uses circular delay buffer with configurable delay (0.5-50ms)
    4. Pitch Shifting (if transpose != 0)

    Simple time-domain resampling with linear interpolation
    Writes to circular pitch buffer at write rate, reads at adjusted rate
    Advances phase by mPitchRatio (2^(semitones/12))
    5. Output Mix & Trim

    Blends wet (processed) and dry (original) signals: wet * mMix + dry * (1.0 - mMix)
    Applies output trim scaling
    Writes to output buffers
    */
    const auto frames = std::min(nFrames, mMaxBlockSize);

    if (!inputs || !outputs)
    {
      return;
    }

    // Process tuner if enabled (before any audio processing)
    ProcessTuner(inputs, frames);

    // If tuner is enabled but live mode is disabled, output silence
    if (mTunerEnabled && !mLiveTunerMode)
    {
      for (int channel = 0; channel < kNumChannels; ++channel)
      {
        iplug::sample* outputChannel = outputs[channel];
        if (outputChannel)
        {
          std::fill_n(outputChannel, frames, static_cast<iplug::sample>(0.0));
        }
      }
      return;
    }

    const auto impulseSize = static_cast<int>(mIRManager.Impulse().size());

    // Ensure buffers are allocated even if Prepare() hasn't been called yet
    const std::size_t need = static_cast<std::size_t>(frames);
    if (mNamInput.size() < need)  mNamInput.resize(need, 0.0f);
    if (mNamOutput.size() < need) mNamOutput.resize(need, 0.0f);
    if (mProcessedL.size() < need) mProcessedL.resize(need, 0.0);
    if (mProcessedR.size() < need) mProcessedR.resize(need, 0.0);
    if (mMonoSignal.size() < need) mMonoSignal.resize(need, 0.0);
    if (mPitchShiftedL.size() < need) mPitchShiftedL.resize(need, 0.0);
    if (mPitchShiftedR.size() < need) mPitchShiftedR.resize(need, 0.0);
    if (mGateEnvelope.size() < kNumChannels) mGateEnvelope.assign(kNumChannels, 0.0);
    if (mToneLowState.size() < kNumChannels) mToneLowState.assign(kNumChannels, 0.0);
    if (mToneHighState.size() < kNumChannels) mToneHighState.assign(kNumChannels, 0.0);

    // Ensure pitch buffers are large enough
    const std::size_t pitchNeed = std::max<std::size_t>(need * 4, mPitchBufferL.size());
    if (mPitchBufferL.size() < pitchNeed)
    {
      mPitchBufferL.resize(pitchNeed, 0.0);
      mPitchBufferR.resize(pitchNeed, 0.0);
      mPitchReadIndex = 0;
      mPitchWriteIndex = 0;
      mPitchPhase = 0.0;
    }

    // Process both channels through the amp model (use preallocated buffers)
    std::vector<double>* channelBuffers[kNumChannels] = {&mProcessedL, &mProcessedR};
    std::fill(mProcessedL.begin(), mProcessedL.begin() + frames, 0.0);
    std::fill(mProcessedR.begin(), mProcessedR.begin() + frames, 0.0);

    // Determine which input channels to process
    int startChannel =  0;
    int endChannel = kNumChannels - 1;
    
    if (mMonoMode)
    {
      startChannel = mInputChannel;
      endChannel = mInputChannel;
    }
    
    // OPTIMIZATION: Process channels in parallel for better CPU utilization
    // Use std::thread for stereo channel processing (L and R can be processed independently)
    auto ProcessChannel = [&](int channel) {
      // In mono mode, always use the selected input but process to both output channels
      const int inputIdx = mMonoMode ? mInputChannel : channel;
      iplug::sample *inputChannel = inputs[inputIdx];
      if (!inputChannel)
      {
        return;
      }

      // In mono mode, process to the left channel buffer (will be copied to right later)
      const int outputIdx = mMonoMode ? 0 : channel;
      std::vector<double>& channelBuffer = *channelBuffers[outputIdx];
      for (int frame = 0; frame < frames; ++frame)
      {
        double sample = static_cast<double>(inputChannel[frame]) * mInputTrimLinear;
        sample = ApplyGate(sample, outputIdx);
      //  sample = ApplyDrive(sample);
       // sample = ApplyTone(sample, outputIdx);
        mNamInput[static_cast<std::size_t>(frame)] = static_cast<NAM_SAMPLE>(sample);
      }

      // In mono mode, use model index 0 only
      const int modelIdx = mMonoMode ? 0 : outputIdx;
      auto &model = mModels[static_cast<std::size_t>(modelIdx)];
      if (mAmpEnabled && model)
      {
        model->process(mNamInput.data(), mNamOutput.data(), frames);
        std::transform(mNamOutput.begin(), mNamOutput.begin() + frames,
                       channelBuffer.begin(),
                       [](NAM_SAMPLE sample) { return static_cast<double>(sample); });
      }
      else
      {
        std::transform(mNamInput.begin(), mNamInput.begin() + frames,
                       channelBuffer.begin(),
                       [](NAM_SAMPLE sample) { return static_cast<double>(sample); });
      }

      if (mCabEnabled && mIRConvolution[static_cast<std::size_t>(modelIdx)].IsInitialized())
      {
        ApplyImpulseResponse(channelBuffer, modelIdx);
      }
      else if (mSimpleCabEnabled)
      {
        // Apply simple filter-based cabinet simulation (faster alternative to IR)
        for (int frame = 0; frame < frames; ++frame)
        {
          channelBuffer[frame] = mSimpleCabSim[static_cast<std::size_t>(modelIdx)].Process(channelBuffer[frame]);
        }
      }

      // Apply parametric EQ if enabled (post-cab for tone shaping)
      if (mEQEnabled)
      {
        for (int frame = 0; frame < frames; ++frame)
        {
          channelBuffer[frame] = mParametricEQ[static_cast<std::size_t>(modelIdx)].Process(channelBuffer[frame]);
        }
      }
    };

    // Process channels - use threading only if we have multiple channels and modern hardware
    // Single-threaded processing is faster for mono or very simple processing
    if (endChannel > startChannel && frames > 128)
    {
      // Multi-threaded: process L and R channels in parallel
      std::thread threadL(ProcessChannel, startChannel);
      std::thread threadR(ProcessChannel, endChannel);
      threadL.join();
      threadR.join();
    }
    else
    {
      // Single-threaded: sequential processing (faster for small block sizes)
      for (int channel = startChannel; channel <= endChannel; ++channel)
      {
        ProcessChannel(channel);
      }
    }

    // In mono mode, copy left channel to right channel
    if (mMonoMode)
    {
      std::copy(mProcessedL.begin(), mProcessedL.begin() + frames, mProcessedR.begin());
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
      for (int frame = 0; frame < frames; ++frame)
      {
        mMonoSignal[static_cast<std::size_t>(frame)] = (mProcessedL[frame] + mProcessedR[frame]) * 0.5;
      }

      // Process doubler: left gets direct signal, right gets delayed signal
      for (int frame = 0; frame < frames; ++frame)
      {
        // Write current sample to delay buffer
        mDoublerDelayBufferL[mDoublerWriteIndex] = mMonoSignal[static_cast<std::size_t>(frame)];
        
        // Read delayed sample for right channel
        std::size_t readIndex = mDoublerWriteIndex >= static_cast<std::size_t>(mDoublerDelaySamples)
          ? mDoublerWriteIndex - static_cast<std::size_t>(mDoublerDelaySamples)
          : mDoublerDelayBufferL.size() - (static_cast<std::size_t>(mDoublerDelaySamples) - mDoublerWriteIndex);
        
        // Left channel: direct mono signal
        mProcessedL[frame] = mMonoSignal[static_cast<std::size_t>(frame)];
        // Right channel: delayed mono signal
        mProcessedR[frame] = mDoublerDelayBufferL[readIndex];
        
        // Advance write index
        mDoublerWriteIndex = (mDoublerWriteIndex + 1) % mDoublerDelayBufferL.size();
      }
    }

    // Apply pitch shifting (transpose) if enabled
    if (mTransposeSemitones != 0 && mPitchRatio != 1.0)
    {
      // Simple pitch shifting using time-domain resampling with linear interpolation
      // This is a basic implementation; production code would use PSOLA or phase vocoder

      for (int frame = 0; frame < frames; ++frame)
      {
        // Write input samples to pitch buffer
        mPitchBufferL[mPitchWriteIndex] = mProcessedL[frame];
        mPitchBufferR[mPitchWriteIndex] = mProcessedR[frame];
        mPitchWriteIndex = (mPitchWriteIndex + 1) % mPitchBufferL.size();

        // Read from buffer at adjusted rate for pitch shifting
        const double readPos = mPitchPhase;
        const std::size_t readIndex0 = static_cast<std::size_t>(readPos) % mPitchBufferL.size();
        const std::size_t readIndex1 = (readIndex0 + 1) % mPitchBufferL.size();
        const double frac = readPos - std::floor(readPos);

        // Linear interpolation for smoother pitch shifting
        mPitchShiftedL[static_cast<std::size_t>(frame)] = mPitchBufferL[readIndex0] * (1.0 - frac) + mPitchBufferL[readIndex1] * frac;
        mPitchShiftedR[static_cast<std::size_t>(frame)] = mPitchBufferR[readIndex0] * (1.0 - frac) + mPitchBufferR[readIndex1] * frac;

        // Advance phase by pitch ratio
        mPitchPhase += mPitchRatio;
        if (mPitchPhase >= static_cast<double>(mPitchBufferL.size()))
        {
          mPitchPhase -= static_cast<double>(mPitchBufferL.size());
        }
      }

      // Replace processed audio with pitch-shifted version
      std::copy(mPitchShiftedL.begin(), mPitchShiftedL.begin() + frames, mProcessedL.begin());
      std::copy(mPitchShiftedR.begin(), mPitchShiftedR.begin() + frames, mProcessedR.begin());
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
        const double drySample = static_cast<double>(inputChannel[frame]) * mInputTrimLinear * mOutputTrimLinear;
        outputChannel[frame] = static_cast<iplug::sample>(wetSample * mMix + drySample * (1.0 - mMix));
      }
    }
  }

  double NAMDSPManager::ApplyDrive(double sample) const
  {
    // Soft clip the signal to emulate preamp saturation before it hits the NAM block.
    const double drive = 1.0 + mDriveAmount * 9.0;
    const double driven = std::tanh(sample * drive);
    return SanitizeDenormal(driven);
  }

  double NAMDSPManager::ApplyTone(double sample, int channel)
  {
    // Split the spectrum with a one-pole filter pair so we can tilt towards lows or highs.
    const double cutoff = 400.0;
    const double omega = 2.0 * std::numbers::pi * cutoff / mSampleRate;
    const double alpha = omega / (omega + 1.0);

    const double low = alpha * sample + (1.0 - alpha) * mToneLowState[static_cast<std::size_t>(channel)];
    const double high = sample - low;
    mToneLowState[static_cast<std::size_t>(channel)] = SanitizeDenormal(low);
    mToneHighState[static_cast<std::size_t>(channel)] = SanitizeDenormal(high);

    const double mix = (mToneTilt + 1.0) * 0.5; // 0..1
    return SanitizeDenormal(low * (1.0 - mix) + high * mix);
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
    mGateEnvelope[static_cast<std::size_t>(channel)] = SanitizeDenormal(coeff * absSample + (1.0 - coeff) * mGateEnvelope[static_cast<std::size_t>(channel)]);

    const double thresholdLinear = DbToLinear(mGateThreshold);
    if (mGateEnvelope[static_cast<std::size_t>(channel)] < thresholdLinear)
    {
      return 0.0;
    }

    return sample;
  }

  // Applies FFT-based convolution with a cabinet impulse response (IR)
  // Uses Uniformly Partitioned Overlap-Save (UPOLS) for real-time performance
  void NAMDSPManager::ApplyImpulseResponse(std::vector<double> &channelSamples, int channel) const
  {
    if (channel < 0 || channel >= kNumChannels)
    {
      return;
    }

    auto &convolution = mIRConvolution[static_cast<std::size_t>(channel)];
    if (!convolution.IsInitialized())
    {
      return;
    }

    // Process in-place using the real-time convolver
    std::vector<double> output(channelSamples.size());
    convolution.Process(channelSamples.data(), output.data(), static_cast<int>(channelSamples.size()));
    channelSamples = std::move(output);
  }

  bool NAMDSPManager::HasModel() const noexcept
  {
    return mModels[0] != nullptr;
  }

  bool NAMDSPManager::HasImpulseResponse() const noexcept
  {
    return mIRManager.HasImpulse();
  }

  void NAMDSPManager::SetImpulseResponseForTest(const std::vector<float>& impulse)
  {
    mIRManager.SetImpulse(impulse);
    // Initialize FFT convolution with test impulse
    for (auto& convolution : mIRConvolution)
    {
      convolution.SetImpulse(impulse, mMaxBlockSize);
    }
  }

  void NAMDSPManager::ApplyImpulseResponseForTest(std::vector<double>& channelSamples, int channel)
  {
    ApplyImpulseResponse(channelSamples, channel);
  }

  void NAMDSPManager::SetTunerEnabled(bool enabled)
  {
    mTunerEnabled = enabled;
    if (enabled)
    {
      // Clear the buffer when enabling tuner
      std::fill(mTunerBuffer.begin(), mTunerBuffer.end(), 0.0);
      mTunerBufferWriteIndex = 0;
      mTunerSampleCounter = 0;
    }
  }

  void NAMDSPManager::SetTunerCallback(TunerCallback callback)
  {
    mTunerCallback = std::move(callback);
  }

  void NAMDSPManager::SetTunerReferenceFrequency(double frequency)
  {
    mTunerReferenceFrequency = std::clamp(frequency, 400.0, 480.0);
  }

  void NAMDSPManager::ProcessTuner(iplug::sample** inputs, int nFrames)
  {
    // Use the main input channel setting (same as DSP processing)
    const int ch = mInputChannel;
    if (!mTunerEnabled || !mTunerCallback || !inputs || !inputs[ch])
    {
      return;
    }

    // Fill the tuner buffer with input samples (mono - use selected channel)
    for (int i = 0; i < nFrames; ++i)
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
      for (const auto& s : orderedBuffer) sumSq += s * s;
      double rms = std::sqrt(sumSq / static_cast<double>(orderedBuffer.size()));
      
      double frequency = DetectPitch(orderedBuffer);
      TunerResult result = FrequencyToNote(frequency);
      
      // Store debug info in result for UI logging
      result.debugRms = rms;
      result.debugRawFreq = frequency;
      
      mTunerCallback(result);
    }
  }

  double NAMDSPManager::DetectPitch(const std::vector<double>& samples) const
  {
    // Autocorrelation-based pitch detection (YIN-inspired algorithm)
    const std::size_t n = samples.size();
    if (n < 2)
    {
      return 0.0;
    }

    // Calculate RMS to check if there's enough signal
    double sumSquares = 0.0;
    for (const auto& sample : samples)
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
    const int minPeriod = static_cast<int>(mSampleRate / 1500.0);  // Highest frequency
    const int maxPeriod = static_cast<int>(mSampleRate / 70.0);   // Lowest frequency
    
    if (maxPeriod >= static_cast<int>(n / 2) || minPeriod < 2)
    {
      return 0.0;
    }

    // Calculate difference function (YIN step 2)
    std::vector<double> diff(maxPeriod + 1, 0.0);
    
    for (int tau = minPeriod; tau <= maxPeriod; ++tau)
    {
      double sum = 0.0;
      for (std::size_t i = 0; i < n - static_cast<std::size_t>(tau); ++i)
      {
        const double delta = samples[i] - samples[i + tau];
        sum += delta * delta;
      }
      diff[tau] = sum;
    }

    // Cumulative mean normalized difference function (YIN step 4)
    std::vector<double> cmndf(maxPeriod + 1, 1.0);
    double runningSum = 0.0;
    
    for (int tau = minPeriod; tau <= maxPeriod; ++tau)
    {
      runningSum += diff[tau];
      if (runningSum > 0.0)
      {
        cmndf[tau] = diff[tau] * static_cast<double>(tau) / runningSum;
      }
    }

    // Find the first minimum below threshold (YIN step 5)
    constexpr double threshold = 0.15;
    int bestPeriod = -1;
    
    for (int tau = minPeriod; tau < maxPeriod; ++tau)
    {
      if (cmndf[tau] < threshold)
      {
        // Find the local minimum
        while (tau + 1 <= maxPeriod && cmndf[tau + 1] < cmndf[tau])
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
      double minVal = cmndf[minPeriod];
      bestPeriod = minPeriod;
      for (int tau = minPeriod + 1; tau <= maxPeriod; ++tau)
      {
        if (cmndf[tau] < minVal)
        {
          minVal = cmndf[tau];
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
      const double s0 = cmndf[bestPeriod - 1];
      const double s1 = cmndf[bestPeriod];
      const double s2 = cmndf[bestPeriod + 1];
      const double denom = 2.0 * (2.0 * s1 - s0 - s2);
      if (std::abs(denom) > 1e-10)
      {
        period += (s2 - s0) / denom;
      }
    }

    return mSampleRate / period;
  }

  NAMDSPManager::TunerResult NAMDSPManager::FrequencyToNote(double frequency) const
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
    const char* sharpName = kNoteNames[noteIndex];
    const char* flatName = kNoteNamesFlat[noteIndex];
    
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

} // namespace namguitar
