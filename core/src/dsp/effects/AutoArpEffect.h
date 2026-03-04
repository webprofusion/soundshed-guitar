#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "signalsmith-stretch.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace guitarfx
{
  /**
   * Auto-Arpeggiator effect.
   *
   * Rhythmically cycles through a semitone interval pattern by pitch-shifting
   * the full guitar signal, producing an arpeggio effect without the need for
   * a separate synthesizer.
   *
   * Step timing is BPM-synced via the requiresTempo injection mechanism:
   * PluginController::ProcessAudio() calls MultiPresetMixer::SetTempo() each
   * block which ultimately calls SetParam("bpm", bpm) on this effect.
   *
   * Audio architecture:
   *  - Steps with 0 semitones: dry audio passes through (no stretch latency).
   *  - Steps with non-zero semitones: audio is pitch-shifted via Signalsmith
   *    Stretch (same library as PitchShiftEffect).
   *  - Gate envelope is applied per-sample to control note length and attack,
   *    independent of which DSP path is active.
   *  - Wet/dry mix controls blend between gated-wet and always-on-dry.
   */
  class AutoArpEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      if (!ValidatePrepare(sampleRate, maxBlockSize))
        return;

      mSampleRate  = sampleRate;
      mMaxBlockSize = maxBlockSize;

      const auto buf = static_cast<size_t>(maxBlockSize);
      mWetL.assign(buf, 0.0f);
      mWetR.assign(buf, 0.0f);
      mZero.assign(buf, 0.0f);

      mStretch.presetCheaper(2, static_cast<float>(sampleRate), false);
      mConfigured = true;

      mPitchBuf.assign(static_cast<size_t>(kPitchBufSize), 0.0f);
      mPitchFillPos = 0;
      mDetectedHz   = 0.0;
      mArpActive    = true;

      RebuildStepList();
      UpdatePhaseIncrement();
      ApplyStretchSemitones(mCurrentSemitones);

      Reset();
    }

    void Reset() override
    {
      mPhase       = 0.0;
      mCurrentStep = 0;
      if (!mStepSemitones.empty())
        mCurrentSemitones = mStepSemitones[0];
      if (mConfigured)
      {
        mStretch.reset();
        ApplyStretchSemitones(mCurrentSemitones);
      }
      mDetectedHz   = 0.0;
      mSmoothedHz   = 0.0;
      mTriggerVote  = 0;
      mArpActive    = (mPitchMode == 0);
      mPitchFillPos = 0;
      if (!mPitchBuf.empty())
        std::fill(mPitchBuf.begin(), mPitchBuf.end(), 0.0f);
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (!inputs || !outputs || !mConfigured)
        return;

      numSamples = std::min(numSamples, mMaxBlockSize);

      // Pitch-mode gating — fill analysis buffer, evaluate trigger condition.
      if (mPitchMode != 0 && inputs[0])
      {
        const int toCopy = std::min(numSamples, kPitchBufSize - mPitchFillPos);
        std::copy(inputs[0], inputs[0] + toCopy, mPitchBuf.data() + mPitchFillPos);
        mPitchFillPos += toCopy;
        if (mPitchFillPos >= kPitchBufSize)
        {
          mDetectedHz   = EstimatePitch();
          mPitchFillPos = 0;

          // EMA smoothing — update only when a confident pitch is detected;
          // during silence let the smoothed value decay gently.
          if (mDetectedHz > 0.0)
            mSmoothedHz = kPitchEmaAlpha * mDetectedHz + (1.0 - kPitchEmaAlpha) * mSmoothedHz;
          else
            mSmoothedHz *= 0.80; // decay toward zero on silence

          const bool conditionMet = (mPitchMode == 1)
              ? (mSmoothedHz > mPitchThreshold)
              : (mSmoothedHz > 20.0 && mSmoothedHz < mPitchThreshold);

          // Asymmetric debounce: fewer windows needed to activate than to release,
          // so the arp latches on quickly but doesn't drop out on brief dips.
          if (conditionMet)
          {
            mTriggerVote = std::max(0, mTriggerVote) + 1;
            if (mTriggerVote >= kActivateFrames && !mArpActive)
            {
              // Reset to beat-start on fresh activation.
              mPhase       = 0.0;
              mCurrentStep = 0;
              if (!mStepSemitones.empty())
              {
                mCurrentSemitones = mStepSemitones[0];
                ApplyStretchSemitones(mCurrentSemitones);
              }
              mStretch.reset();
              mArpActive = true;
            }
          }
          else
          {
            mTriggerVote = std::min(0, mTriggerVote) - 1;
            if (mTriggerVote <= -kDeactivateFrames)
              mArpActive = false;
          }
        }
      }

      // When pitch-gated off, pass through dry audio unchanged.
      if (!mArpActive)
      {
        for (int i = 0; i < numSamples; ++i)
        {
          if (outputs[0]) outputs[0][i] = inputs[0] ? inputs[0][i] : 0.0f;
          if (outputs[1]) outputs[1][i] = inputs[1] ? inputs[1][i] : 0.0f;
        }
        return;
      }

      // Pitch-shift (or bypass) based on the semitones active at block start.
      const int blockSemitones = mCurrentSemitones;
      if (blockSemitones == 0)
      {
        // No pitch change: copy input to wet buffers directly.
        for (int i = 0; i < numSamples; ++i)
        {
          mWetL[static_cast<size_t>(i)] = inputs[0] ? inputs[0][i] : 0.0f;
          mWetR[static_cast<size_t>(i)] = inputs[1] ? inputs[1][i] : 0.0f;
        }
      }
      else
      {
        if (static_cast<size_t>(numSamples) > mWetL.size())
        {
          mWetL.resize(static_cast<size_t>(numSamples), 0.0f);
          mWetR.resize(static_cast<size_t>(numSamples), 0.0f);
          mZero.resize(static_cast<size_t>(numSamples), 0.0f);
        }
        float *ip[2] = {
          inputs[0] ? inputs[0] : mZero.data(),
          inputs[1] ? inputs[1] : mZero.data()
        };
        float *wp[2] = {mWetL.data(), mWetR.data()};
        mStretch.process(ip, numSamples, wp, numSamples);
      }

      // Per-sample: apply gate envelope, advance phase, detect step transitions.
      const float dryMix = static_cast<float>(1.0 - mMix);
      const float wetMix = static_cast<float>(mMix);
      const float attackFrac  = mAttack;
      const float gateFrac    = mGate;
      // Release window starts at gateFrac; clamped so it never overruns phase 1.0
      const float releaseFrac = std::min(mRelease, std::max(0.0f, 1.0f - gateFrac));
      const float releaseEnd  = gateFrac + releaseFrac;

      for (int i = 0; i < numSamples; ++i)
      {
        // Gate envelope: [0, attack) ramp up | [attack, gate) hold | [gate, gate+release) ramp down | silence
        const float phase = static_cast<float>(mPhase);
        float gateGain;
        if (phase < attackFrac)
          gateGain = (attackFrac > 0.0f) ? (phase / attackFrac) : 1.0f;
        else if (phase < gateFrac)
          gateGain = 1.0f;
        else if (releaseFrac > 0.0f && phase < releaseEnd)
          gateGain = 1.0f - (phase - gateFrac) / releaseFrac;
        else
          gateGain = 0.0f;

        const float dryL = inputs[0] ? inputs[0][i] : 0.0f;
        const float dryR = inputs[1] ? inputs[1][i] : 0.0f;

        if (outputs[0])
          outputs[0][i] = dryL * dryMix + mWetL[static_cast<size_t>(i)] * gateGain * wetMix;
        if (outputs[1])
          outputs[1][i] = dryR * dryMix + mWetR[static_cast<size_t>(i)] * gateGain * wetMix;

        // Advance phase; on wrap, advance to next step.
        mPhase += mPhaseIncrement;
        if (mPhase >= 1.0)
        {
          mPhase -= 1.0;
          const int count = static_cast<int>(mStepSemitones.size());
          if (count > 0)
          {
            mCurrentStep = (mCurrentStep + 1) % count;
            mCurrentSemitones = mStepSemitones[static_cast<size_t>(mCurrentStep)];
            // Update stretch with new pitch for the next block.
            ApplyStretchSemitones(mCurrentSemitones);
          }
        }
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "bpm")
      {
        mBpm = std::clamp(value, 30.0, 300.0);
        UpdatePhaseIncrement();
      }
      else if (key == "stepRate")
      {
        mStepRate = static_cast<int>(std::clamp(std::round(value), 0.0, 4.0));
        UpdatePhaseIncrement();
      }
      else if (key == "numSteps")
      {
        mNumSteps = static_cast<int>(std::clamp(std::round(value), 2.0, 8.0));
        RebuildStepList();
      }
      else if (key == "pattern")
      {
        mPattern = static_cast<int>(std::clamp(std::round(value), 0.0, 4.0));
        RebuildStepList();
      }
      else if (key == "direction")
      {
        mDirection = static_cast<int>(std::clamp(std::round(value), 0.0, 2.0));
        RebuildStepList();
      }
      else if (key == "gate")
      {
        mGate = static_cast<float>(std::clamp(value, 0.05, 1.0));
      }
      else if (key == "attack")
      {
        mAttack = static_cast<float>(std::clamp(value, 0.0, 0.5));
      }
      else if (key == "release")
      {
        mRelease = static_cast<float>(std::clamp(value, 0.0, 0.5));
      }
      else if (key == "mix")
      {
        mMix = std::clamp(value, 0.0, 1.0);
      }
      else if (key == "pitchMode")
      {
        mPitchMode = static_cast<int>(std::clamp(std::round(value), 0.0, 2.0));
        if (mPitchMode == 0) mArpActive = true;
      }
      else if (key == "pitchThreshold")
      {
        mPitchThreshold = std::clamp(value, 50.0, 2000.0);
      }
      else if (key.size() > 4 && key.substr(0, 4) == "step")
      {
        // step0 .. step7
        const int idx = std::stoi(key.substr(4));
        if (idx >= 0 && idx < kMaxCustomSteps)
        {
          mCustomSteps[static_cast<size_t>(idx)] = static_cast<int>(std::clamp(std::round(value), -24.0, 24.0));
          if (mPattern == kPatternCustom)
            RebuildStepList();
        }
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "bpm")            return mBpm;
      if (key == "stepRate")       return static_cast<double>(mStepRate);
      if (key == "numSteps")       return static_cast<double>(mNumSteps);
      if (key == "pattern")        return static_cast<double>(mPattern);
      if (key == "direction")      return static_cast<double>(mDirection);
      if (key == "gate")           return mGate;
      if (key == "attack")         return mAttack;
      if (key == "release")        return mRelease;
      if (key == "mix")            return mMix;
      if (key == "pitchMode")      return static_cast<double>(mPitchMode);
      if (key == "pitchThreshold") return mPitchThreshold;
      if (key.size() > 4 && key.substr(0, 4) == "step")
      {
        const int idx = std::stoi(key.substr(4));
        if (idx >= 0 && idx < kMaxCustomSteps)
          return static_cast<double>(mCustomSteps[static_cast<size_t>(idx)]);
      }
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "arp_auto"; }
    [[nodiscard]] std::string GetCategory() const override { return "modulation"; }

    [[nodiscard]] int GetLatencySamples() const override
    {
      return mConfigured ? static_cast<int>(mStretch.inputLatency()) : 0;
    }

  private:
    // ── Pattern table ─────────────────────────────────────────────────────
    static constexpr int kMaxCustomSteps = 8;
    static constexpr int kPatternCustom  = 4;
    static constexpr int    kPitchBufSize      = 2048; // ~46ms analysis window at 44.1kHz
    static constexpr int    kActivateFrames    = 2;    // consecutive on-windows needed to activate
    static constexpr int    kDeactivateFrames  = 5;    // consecutive off-windows needed to deactivate
    static constexpr double kPitchEmaAlpha     = 0.6;  // EMA weight for new pitch measurement

    // Base patterns: [pattern][step], -1 terminates the list
    static constexpr int kPatternTable[5][9] = {
      {  0,  4,  7, 12, -1, -1, -1, -1, -1 },  // 0: Major Triad
      {  0,  3,  7, 12, -1, -1, -1, -1, -1 },  // 1: Minor Triad
      {  0,  7, 12, -1, -1, -1, -1, -1, -1 },  // 2: Power Chord
      {  0, 12, -1, -1, -1, -1, -1, -1, -1 },  // 3: Octaves
      { -1, -1, -1, -1, -1, -1, -1, -1, -1 },  // 4: Custom (use mCustomSteps / mNumSteps)
    };

    // Beats per step for each stepRate enum value (0-4).
    // Based on a common 4/4 meter reference beat (quarter note = 1 beat).
    static constexpr double kBeatFractions[5] = {
      1.0,          // 0: 1/4  note  = 1 beat
      0.5,          // 1: 1/8  note  = 0.5 beats
      0.25,         // 2: 1/16 note  = 0.25 beats
      1.0 / 3.0,    // 3: 1/8  triplet = 1/3 beat
      1.0 / 6.0,    // 4: 1/16 triplet = 1/6 beat
    };

    // ── Helpers ───────────────────────────────────────────────────────────

    // Compute phase increment (per sample) for the current BPM and step rate.
    void UpdatePhaseIncrement()
    {
      const double beatsPerStep   = kBeatFractions[static_cast<size_t>(mStepRate)];
      const double secondsPerBeat = 60.0 / std::max(1.0, mBpm);
      const double stepSeconds    = secondsPerBeat * beatsPerStep;
      const double stepSamples    = stepSeconds * std::max(1.0, mSampleRate);
      mPhaseIncrement = 1.0 / std::max(1.0, stepSamples);
    }

    // Rebuild the resolved step semitone list from pattern/direction/numSteps.
    void RebuildStepList()
    {
      std::vector<int> base;

      if (mPattern == kPatternCustom)
      {
        for (int i = 0; i < mNumSteps && i < kMaxCustomSteps; ++i)
          base.push_back(mCustomSteps[static_cast<size_t>(i)]);
      }
      else
      {
        const auto &row = kPatternTable[static_cast<size_t>(mPattern)];
        for (int i = 0; i < 9 && row[i] >= 0; ++i)
          base.push_back(row[i]);
      }

      if (base.empty())
      {
        mStepSemitones = {0};
        mCurrentStep = 0;
        mCurrentSemitones = 0;
        return;
      }

      // Apply direction
      switch (mDirection)
      {
        case 0: // Up — use base as-is
          mStepSemitones = base;
          break;
        case 1: // Down — reverse
          mStepSemitones = std::vector<int>(base.rbegin(), base.rend());
          break;
        case 2: // Up-Down — base + reverse without duplicating endpoints
        {
          mStepSemitones = base;
          if (base.size() > 1)
          {
            for (int i = static_cast<int>(base.size()) - 2; i >= 1; --i)
              mStepSemitones.push_back(base[static_cast<size_t>(i)]);
          }
          break;
        }
        default:
          mStepSemitones = base;
          break;
      }

      // Clamp current step index to new list size
      const int count = static_cast<int>(mStepSemitones.size());
      if (count > 0)
      {
        mCurrentStep = mCurrentStep % count;
        mCurrentSemitones = mStepSemitones[static_cast<size_t>(mCurrentStep)];
        if (mConfigured)
          ApplyStretchSemitones(mCurrentSemitones);
      }
    }

    // YIN-inspired autocorrelation pitch detector operating on mPitchBuf.
    // Returns detected fundamental in Hz, or 0.0 if no clear pitch is found.
    // No dynamic allocation; operates entirely on pre-allocated mPitchBuf.
    [[nodiscard]] double EstimatePitch() const
    {
      const int n         = kPitchBufSize;
      const int minPeriod = static_cast<int>(mSampleRate / 1300.0); // ~1300 Hz max
      const int maxPeriod = static_cast<int>(mSampleRate / 50.0);   // ~50 Hz min
      if (minPeriod < 2 || maxPeriod >= n / 2)
        return 0.0;

      // Silence check: skip if RMS² is below noise floor
      double sumSq = 0.0;
      for (int i = 0; i < n; ++i)
      {
        const double s = static_cast<double>(mPitchBuf[static_cast<size_t>(i)]);
        sumSq += s * s;
      }
      if (sumSq / n < 9.0e-6) // RMS < 0.003
        return 0.0;

      // Cumulative mean normalised difference function (YIN steps 2–4)
      double runningSum = 0.0;
      double bestCmndf  = std::numeric_limits<double>::max();
      int    bestTau    = -1;

      for (int tau = minPeriod; tau <= maxPeriod; ++tau)
      {
        double sum = 0.0;
        for (int i = 0; i < n - tau; ++i)
        {
          const double delta =
            static_cast<double>(mPitchBuf[static_cast<size_t>(i)]) -
            static_cast<double>(mPitchBuf[static_cast<size_t>(i + tau)]);
          sum += delta * delta;
        }
        runningSum += sum;
        const double cmndf = (runningSum > 0.0)
                               ? (sum * static_cast<double>(tau) / runningSum)
                               : 1.0;
        if (cmndf < 0.15) // First clear minimum wins (YIN step 5)
        {
          bestTau   = tau;
          bestCmndf = cmndf;
          break;
        }
        if (cmndf < bestCmndf)
        {
          bestCmndf = cmndf;
          bestTau   = tau;
        }
      }
      if (bestTau <= 0 || bestCmndf > 0.5)
        return 0.0;
      return mSampleRate / static_cast<double>(bestTau);
    }

    // Apply semitone transposition to the Signalsmith Stretch instance.
    void ApplyStretchSemitones(int semitones)
    {
      if (!mConfigured)
        return;
      static constexpr double kTonalityLimitHz = 8000.0;
      const float tonalityLimit = static_cast<float>(kTonalityLimitHz / std::max(1.0, mSampleRate));
      mStretch.setTransposeSemitones(static_cast<float>(semitones), tonalityLimit);
    }

    // ── Parameters ────────────────────────────────────────────────────────
    double mBpm           = 120.0;
    int    mStepRate      = 1;     // 0=1/4, 1=1/8, 2=1/16, 3=1/8T, 4=1/16T
    int    mNumSteps      = 4;     // active steps in Custom mode
    int    mPattern       = 0;     // 0=Major, 1=Minor, 2=Power, 3=Octaves, 4=Custom
    int    mDirection     = 0;     // 0=Up, 1=Down, 2=UpDown
    float  mGate          = 0.8f;
    float  mAttack        = 0.05f;
    float  mRelease       = 0.08f; // short fade-out to avoid clicks at gate close
    int    mCustomSteps[kMaxCustomSteps] = {0, 4, 7, 12, 0, 0, 0, 0};
    double mMix           = 0.8;
    int    mPitchMode     = 0;     // 0=Always, 1=Above threshold, 2=Below threshold
    double mPitchThreshold = 330.0; // Hz — default E4 (open high E string)

    // ── Internal state ────────────────────────────────────────────────────
    double mSampleRate     = 48000.0;
    int    mMaxBlockSize   = 512;
    bool   mConfigured     = false;
    double mPhase          = 0.0;
    double mPhaseIncrement = 0.0;
    int    mCurrentStep    = 0;
    int    mCurrentSemitones = 0;
    std::vector<int>   mStepSemitones;
    signalsmith::stretch::SignalsmithStretch<float> mStretch;
    std::vector<float> mWetL;
    std::vector<float> mWetR;
    std::vector<float> mZero;
    // Pitch detection state
    double mDetectedHz   = 0.0;   // most recently detected fundamental (Hz)
    double mSmoothedHz   = 0.0;   // EMA-smoothed fundamental used for threshold comparison
    int    mTriggerVote  = 0;     // debounce counter (>0 = consecutive on-frames, <0 = off-frames)
    bool   mArpActive    = true;  // current pitch-gate state
    int    mPitchFillPos = 0;     // write cursor into mPitchBuf
    std::vector<float> mPitchBuf; // pre-allocated analysis window
  };

  // ── Registration ──────────────────────────────────────────────────────────

  inline void RegisterAutoArpEffect()
  {
    EffectTypeInfo info;
    info.type          = EffectGuids::kAutoArp;
    info.aliases       = {"arp_auto"};
    info.displayName   = "Auto Arpeggiator";
    info.category      = "modulation";
    info.description   = "BPM-synced rhythmic arpeggiator. Cycles through interval patterns by pitch-shifting the signal each step.";
    info.requiresTempo = true;
    info.parameters    = {
      // Step timing
      {"stepRate",   "Step Rate",    1.0, 0.0,  4.0,  "enum", "timing",  false, 1.0,
       {"1/4 Note", "1/8 Note", "1/16 Note", "1/8 Triplet", "1/16 Triplet"}},
      // Pattern
      {"pattern",    "Pattern",      0.0, 0.0,  4.0,  "enum", "pattern", false, 1.0,
       {"Major Triad", "Minor Triad", "Power Chord", "Octaves", "Custom"}},
      {"direction",  "Direction",    0.0, 0.0,  2.0,  "enum", "pattern", false, 1.0,
       {"Up", "Down", "Up-Down"}},
      {"numSteps",   "Steps",        4.0, 2.0,  8.0,  "enum", "pattern", false, 1.0,
       {"2", "3", "4", "5", "6", "7", "8"}},
      // Envelope
      {"gate",       "Gate",         0.8,  0.05, 1.0,  "",  "envelope", false, 0.0, {}},
      {"attack",     "Attack",       0.05, 0.0,  0.5,  "",  "envelope", false, 0.0, {}},
      {"release",    "Release",      0.08, 0.0,  0.5,  "",  "envelope", false, 0.0, {}},
      // Per-step intervals (Custom mode)
      {"step0", "Step 1", 0.0,  -24.0, 24.0, "st", "steps", true, 1.0, {}},
      {"step1", "Step 2", 4.0,  -24.0, 24.0, "st", "steps", true, 1.0, {}},
      {"step2", "Step 3", 7.0,  -24.0, 24.0, "st", "steps", true, 1.0, {}},
      {"step3", "Step 4", 12.0, -24.0, 24.0, "st", "steps", true, 1.0, {}},
      {"step4", "Step 5", 0.0,  -24.0, 24.0, "st", "steps", true, 1.0, {}},
      {"step5", "Step 6", 0.0,  -24.0, 24.0, "st", "steps", true, 1.0, {}},
      {"step6", "Step 7", 0.0,  -24.0, 24.0, "st", "steps", true, 1.0, {}},
      {"step7", "Step 8", 0.0,  -24.0, 24.0, "st", "steps", true, 1.0, {}},
      // Pitch trigger
      {"pitchMode",      "Pitch Trigger", 0.0,   0.0,    2.0,    "enum", "trigger", false, 1.0,
       {"Always", "Above", "Below"}},
      {"pitchThreshold", "Pitch",        330.0, 50.0, 2000.0, "Hz",   "trigger", false, 0.0, {}},
      // Mix
      {"mix",        "Mix",          0.8, 0.0,  1.0,  "",   "",       false, 0.0, {}},
    };

    EffectRegistry::Instance().Register(info.type, info,
      []() { return std::make_unique<AutoArpEffect>(); });
  }

} // namespace guitarfx
