#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

#include "IPlugConstants.h"
#include "NAM/dsp.h"

// Forward declare factory registration function
namespace nam { namespace factory { void ForceFactoryRegistration(); } }
#include "dsp/IRTypes.h"
#include "dsp/IRManager.h"
#include "dsp/RealtimeConvolver.h"
#include "dsp/SimpleCabSim.h"
#include "dsp/ParametricEQ.h"

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
    void SetTranspose(int semitones);
    
    // Amp and Cab enable/disable
    void SetAmpEnabled(bool enabled) { mAmpEnabled = enabled; }
    [[nodiscard]] bool IsAmpEnabled() const noexcept { return mAmpEnabled; }
    void SetCabEnabled(bool enabled) { mCabEnabled = enabled; }
    [[nodiscard]] bool IsCabEnabled() const noexcept { return mCabEnabled; }
    
    // IR Quality settings
    void SetIRQuality(IRQuality quality);
    [[nodiscard]] IRQuality GetIRQuality() const noexcept { return mIRManager.GetQuality(); }
    
    // Simple cabinet simulation (alternative to IR convolution)
    void SetSimpleCabEnabled(bool enabled) { mSimpleCabEnabled = enabled; }
    [[nodiscard]] bool IsSimpleCabEnabled() const noexcept { return mSimpleCabEnabled; }
    void SetSimpleCabBass(double bass);      // 0.0-1.0
    void SetSimpleCabPresence(double presence); // 0.0-1.0
    void SetSimpleCabBrightness(double brightness); // 0.0-1.0
    
    // Parametric EQ
    void SetEQEnabled(bool enabled) { mEQEnabled = enabled; }
    [[nodiscard]] bool IsEQEnabled() const noexcept { return mEQEnabled; }
    void SetEQBandGain(int band, double gainDb);
    void SetEQBandFrequency(int band, double freqHz);
    void SetEQBandQ(int band, double q);
    
    // Input mode settings
    void SetMonoMode(bool enabled) { mMonoMode = enabled; }
    [[nodiscard]] bool IsMonoMode() const noexcept { return mMonoMode; }
    void SetInputChannel(int channel) { mInputChannel = std::clamp(channel, 0, 1); }
    [[nodiscard]] int GetInputChannel() const noexcept { return mInputChannel; }

    void Process(iplug::sample **inputs, iplug::sample **outputs, int nFrames);

    // Tuner functionality
    struct TunerResult
    {
      std::string noteName;       // e.g., "E", "A#", "Bb"
      int octave = 0;             // Octave number (e.g., 2 for low E on guitar)
      double frequency = 0.0;     // Detected frequency in Hz
      double centOffset = 0.0;    // Cents deviation from perfect pitch (-50 to +50)
      double confidence = 0.0;    // Detection confidence (0.0 to 1.0)
      bool detected = false;      // Whether a valid pitch was detected
      double debugRms = 0.0;      // Debug: RMS of input signal
      double debugRawFreq = 0.0;  // Debug: Raw detected frequency before note mapping
    };

    using TunerCallback = std::function<void(const TunerResult&)>;

    void SetTunerEnabled(bool enabled);
    [[nodiscard]] bool IsTunerEnabled() const noexcept { return mTunerEnabled; }
    void SetLiveTunerMode(bool enabled) { mLiveTunerMode = enabled; }
    [[nodiscard]] bool IsLiveTunerMode() const noexcept { return mLiveTunerMode; }
    void SetTunerCallback(TunerCallback callback);
    void SetTunerReferenceFrequency(double frequency);
    [[nodiscard]] double GetTunerReferenceFrequency() const noexcept { return mTunerReferenceFrequency; }

    // Test accessors
    [[nodiscard]] bool HasModel() const noexcept;
    [[nodiscard]] bool HasImpulseResponse() const noexcept;

    // Test-only methods for IR convolution testing
    void SetImpulseResponseForTest(const std::vector<float>& impulse);
    void ApplyImpulseResponseForTest(std::vector<double>& channelSamples, int channel);

  private:
    double ApplyDrive(double sample) const;
    double ApplyTone(double sample, int channel);
    double ApplyGate(double sample, int channel);
    void ApplyImpulseResponse(std::vector<double> &channelSamples, int channel) const;
    
    // Pitch detection using autocorrelation
    void ProcessTuner(iplug::sample** inputs, int nFrames);
    double DetectPitch(const std::vector<double>& samples) const;
    TunerResult FrequencyToNote(double frequency) const;

    std::array<std::unique_ptr<nam::DSP>, 2> mModels;
    IRManager mIRManager;

    std::vector<NAM_SAMPLE> mNamInput;
    std::vector<NAM_SAMPLE> mNamOutput;

    std::vector<double> mGateEnvelope;
    std::vector<double> mToneLowState;
    std::vector<double> mToneHighState;

    double mSampleRate = 48000;///44100.0;
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
    
    // Amp and Cab enable state
    bool mAmpEnabled = true;   // NAM model processing enabled by default
    bool mCabEnabled = true;   // IR convolution enabled by default
    bool mSimpleCabEnabled = false; // Simple cab sim disabled by default (use IR instead)
    
    // Simple cabinet simulation (filter-based, no IR)
    std::array<SimpleCabSim, 2> mSimpleCabSim; // One per channel
    
    // Parametric EQ
    bool mEQEnabled = false;
    std::array<ParametricEQ, 2> mParametricEQ; // One per channel
    
    // Input mode settings
    bool mMonoMode = true;  // Default to mono mode
    int mInputChannel = 1;  // Default to input 2 (index 1) for typical guitar setups

    // Doubler effect state
    bool mDoublerEnabled = false;
    double mDoublerDelayMs = 6.0; // Default 6ms separation
    int mDoublerDelaySamples = 0;
    std::vector<double> mDoublerDelayBufferL;
    std::vector<double> mDoublerDelayBufferR;
    std::size_t mDoublerWriteIndex = 0;

    // Transpose (pitch shift) state
    int mTransposeSemitones = 0;
    double mPitchRatio = 1.0;
    std::vector<double> mPitchBufferL;
    std::vector<double> mPitchBufferR;
    std::size_t mPitchReadIndex = 0;
    std::size_t mPitchWriteIndex = 0;
    double mPitchPhase = 0.0;

    // Preallocated processing buffers to avoid per-block heap allocations
    std::vector<double> mProcessedL;
    std::vector<double> mProcessedR;
    std::vector<double> mMonoSignal;
    std::vector<double> mPitchShiftedL;
    std::vector<double> mPitchShiftedR;

    // Denormal protection helper
    static inline double SanitizeDenormal(double x)
    {
      constexpr double kThreshold = 1e-20;
      return (std::abs(x) < kThreshold) ? 0.0 : x;
    }

    // FFT-based convolution for IR processing (real-time optimized using UPOLS algorithm)
    mutable std::array<RealtimeConvolver, 2> mIRConvolution;  // One per channel

    // Tuner state
    bool mTunerEnabled = false;
    bool mLiveTunerMode = true;  // When true, audio passes through DSP while tuning; when false, output is silent
    double mTunerReferenceFrequency = 440.0;  // A4 reference pitch
    TunerCallback mTunerCallback;
    std::vector<double> mTunerBuffer;         // Circular buffer for pitch detection
    std::size_t mTunerBufferWriteIndex = 0;
    std::size_t mTunerSampleCounter = 0;      // For throttling callback rate
    static constexpr std::size_t kTunerBufferSize = 4096;  // ~85ms at 48kHz for good low-frequency detection
    static constexpr std::size_t kTunerUpdateInterval = 2048;  // Update every ~42ms at 48kHz
  };
} // namespace namguitar
