#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace guitarfx
{
  /**
   * =============================================================================
   * SYNTH SAW EFFECT - Audio-to-Sawtooth Synthesizer via Pitch Tracking
   * =============================================================================
   *
   * OVERVIEW:
   * This effect converts incoming audio (typically guitar) into a synthesized
   * sawtooth waveform by detecting the fundamental frequency (pitch) in real-time
   * and generating a bandlimited sawtooth oscillator locked to that frequency.
   *
   * KEY ALGORITHMS:
   *
   * 1. YIN PITCH DETECTION (de Cheveigné & Kawahara, 2002)
   *    ------------------------------------------------
   *    YIN is an autocorrelation-based pitch detection algorithm known for its
   *    accuracy and robustness. It works by finding the fundamental period (tau)
   *    where the signal most closely matches a time-shifted version of itself.
   *
   *    The algorithm has 6 steps:
   *      Step 1: Difference Function d(tau) - measures how different the signal
   *              is from a tau-shifted version: d(tau) = sum((x[j] - x[j+tau])^2)
   *
   *      Step 2: Cumulative Mean Normalized Difference (CMND) - normalizes d(tau)
   *              to remove the bias toward tau=0: d'(tau) = d(tau) / ((1/tau) * sum(d[1..tau]))
   *              This makes the algorithm independent of signal amplitude.
   *
   *      Step 3: Absolute Threshold - find the first tau where d'(tau) < threshold.
   *              Lower thresholds (0.10) are more selective but may miss pitches;
   *              higher thresholds (0.20) catch more pitches but may have errors.
   *
   *      Step 4: Parabolic Interpolation - refines the tau estimate to sub-sample
   *              precision by fitting a parabola through 3 points around the minimum.
   *              This improves frequency accuracy from ~1% to <0.1%.
   *
   *    Reference: http://audition.ens.fr/adc/pdf/2002_JASA_YIN.pdf
   *
   * 2. ADAPTIVE WINDOW SIZING
   *    -----------------------
   *    YIN requires a window containing at least 2 full periods of the signal.
   *    For low frequencies (82 Hz = low E), this needs ~2400 samples at 48kHz.
   *    For high frequencies (1000+ Hz), we only need ~100 samples.
   *
   *    Using a fixed large window for all frequencies wastes CPU and adds latency.
   *    This implementation adapts the window size based on the currently tracked
   *    frequency, using 2.5x the expected period length (halfSize >= 1.25 periods).
   *
   *    Benefits:
   *    - Faster response time for higher pitches
   *    - Reduced CPU usage when tracking known frequencies
   *    - Lower latency during stable tracking
   *
   * 3. ONSET DETECTION & MEDIAN FILTERING
   *    -----------------------------------
   *    Guitar notes have transient attacks that can confuse pitch detection.
   *    The onset detector monitors the envelope follower and triggers when
   *    the level rises significantly (new note attack). This:
   *    - Resets the median filter for fast response to new pitches
   *    - Temporarily increases frequency smoothing rate
   *    - Resets the "stable frame count" for pitch locking
   *
   *    A 5-sample median filter removes outliers and octave errors by selecting
   *    the middle value from recent pitch estimates. This smooths jitter without
   *    the lag of a moving average filter.
   *
   * 4. OCTAVE ERROR CORRECTION
   *    ------------------------
   *    YIN can sometimes detect harmonics (2x, 3x) or subharmonics (0.5x) instead
   *    of the fundamental. This is especially common with distorted guitar.
   *
   *    When an octave jump is detected (frequency ratio ~2.0 or ~0.5 vs previous),
   *    we check if the YIN value at the expected frequency is actually better.
   *    If so, we correct to that frequency. This prevents "octave jumping" during
   *    sustained notes while still allowing legitimate octave changes.
   *
   * 5. POLYBLEP ANTI-ALIASING
   *    ----------------------
   *    Naive sawtooth generation creates aliasing (harsh digital artifacts) because
   *    the sharp discontinuity contains infinite harmonics that fold back below Nyquist.
   *
   *    PolyBLEP (Polynomial Bandlimited Step) smooths the discontinuity by subtracting
   *    a polynomial correction near the transition point. This provides excellent
   *    anti-aliasing with minimal CPU cost (unlike wavetable or additive methods).
   *
   *    The correction polynomial is applied in the region [0, 2*phaseInc] around
   *    the discontinuity, blending the sharp edge into a smooth transition.
   *
   * SIGNAL FLOW:
   *   Input -> Envelope Follower -> Onset Detection -> Pitch Detection -> Median Filter
   *                                                           |
   *                                                           v
   *   Sawtooth Oscillator <- Frequency Smoothing <- Octave Correction
   *                |
   *                v
   *   PolyBLEP Anti-aliasing -> Envelope Shaping -> Dry/Wet Mix -> Output
   *
   * PARAMETERS:
   *   - mix: Blend between original (dry) and synthesized (wet) signal
   *   - attack/release: Envelope follower time constants
   *   - detune: Fine pitch adjustment in cents (±100 = ±1 semitone)
   *   - octaveShift: Transpose output by ±2 octaves
   *   - glide: Portamento time for smooth pitch transitions
   *   - outputGain: Synth output level in dB
   *   - gate: Input threshold below which synth is silent (noise gate)
   *
   * FUTURE ENHANCEMENTS:
   *   - MIDI note output for driving external instruments
   *   - Multiple waveform types (square, triangle, pulse width modulation)
   *   - Polyphonic pitch detection for chords
   *   - Pitch quantization to musical scales
   *
   * =============================================================================
   */
  class SynthSawEffect : public EffectProcessor
  {
  public:
    void Prepare(double sampleRate, int maxBlockSize) override
    {
      mSampleRate = sampleRate;
      mMaxBlockSize = maxBlockSize;

      // ========================================================================
      // BUFFER SIZING FOR YIN ALGORITHM
      // ========================================================================
      // YIN's difference function compares x[j] with x[j+tau] for j in [0, halfSize).
      // This means we need: windowSize >= halfSize + maxTau.
      // Since maxTau = sampleRate/minFreq (period of lowest frequency), and we want
      // halfSize >= maxTau for best accuracy, we need windowSize >= 2 * maxTau.
      //
      // At 48000 Hz with minFreq=50 Hz:
      //   - period = 48000/50 = 960 samples
      //   - minimum window = 2 * 960 = 1920 samples
      //   - we use 2.5x for extra margin = 2400 samples
      //
      // This 2.5x factor ensures we have enough "lookahead" samples for the
      // autocorrelation to find a clean minimum even with slight frequency drift.
      // ========================================================================
      mMaxYinBufferSize = static_cast<size_t>(2.5 * mSampleRate / kMinFrequency);
      mYinBuffer.assign(mMaxYinBufferSize, 0.0f);
      mYinDiff.assign(mMaxYinBufferSize / 2, 0.0f);
      mYinCumulative.assign(mMaxYinBufferSize / 2, 0.0f);

      // Larger input buffer for pitch history
      mInputBuffer.assign(mMaxYinBufferSize * 2, 0.0f);
      mInputWritePos = 0;
      mSamplesCollected = 0;

      // Initialize adaptive window to max size
      mCurrentWindowSize = mMaxYinBufferSize;

      // ========================================================================
      // HOP SIZE - How often we run pitch detection
      // ========================================================================
      // Running YIN every sample would be wasteful. Instead, we "hop" forward
      // by a fixed number of samples between each detection.
      //
      // ~3ms hop = 144 samples at 48kHz = ~333 pitch updates per second.
      // This is fast enough for guitar pitch bends and vibrato while keeping
      // CPU usage reasonable. The minimum of 32 samples prevents excessive
      // CPU use at very low sample rates.
      //
      // Trade-off: Smaller hop = faster tracking but higher CPU
      //            Larger hop = lower CPU but may miss fast pitch changes
      // ========================================================================
      mHopSize = static_cast<size_t>(mSampleRate * 0.003);
      mHopSize = std::max(mHopSize, size_t(32));

      UpdateEnvelopeCoefs();
      UpdateGlideCoef();
      Reset();
    }

    void Reset() override
    {
      mOscPhase = 0.0;
      mOscPhase2 = 0.0;
      mCurrentFreq = 0.0;
      mTargetFreq = 0.0;
      mSmoothedFreq = 0.0;
      mEnvelopeLevel = 0.0f;
      mPitchConfidence = 0.0f;
      mInputWritePos = 0;
      mSamplesCollected = 0;
      mCurrentWindowSize = mMaxYinBufferSize;
      mPrevEnvelopeLevel = 0.0f;
      mOnsetDetected = false;
      mStableFrameCount = 0;
      std::fill(mInputBuffer.begin(), mInputBuffer.end(), 0.0f);
      std::fill(mYinBuffer.begin(), mYinBuffer.end(), 0.0f);
      std::fill(mMedianBuffer.begin(), mMedianBuffer.end(), 0.0);
      mMedianIndex = 0;
    }

    void Process(float **inputs, float **outputs, int numSamples) override
    {
      if (!inputs || !outputs)
        return;

      for (int i = 0; i < numSamples; ++i)
      {
        // Mix to mono for pitch detection
        const float inL = inputs[0] ? inputs[0][i] : 0.0f;
        const float inR = inputs[1] ? inputs[1][i] : 0.0f;
        const float mono = 0.5f * (inL + inR);

        // Add to input buffer for pitch detection
        mInputBuffer[mInputWritePos] = mono;
        mInputWritePos = (mInputWritePos + 1) % mInputBuffer.size();
        mSamplesCollected++;

        // Envelope follower on input signal
        const float absInput = std::abs(mono);
        if (absInput > mEnvelopeLevel)
        {
          mEnvelopeLevel += mAttackCoef * (absInput - mEnvelopeLevel);
        }
        else
        {
          mEnvelopeLevel += mReleaseCoef * (absInput - mEnvelopeLevel);
        }

        // ======================================================================
        // ONSET DETECTION - Detecting new note attacks
        // ======================================================================
        // When a new note is played, we want to:
        //   1. Respond quickly to the new pitch (reset median filter)
        //   2. Allow larger frequency jumps without octave correction
        //   3. Use faster frequency smoothing temporarily
        //
        // We detect onsets by monitoring the envelope's rate of change.
        // If the envelope rises by more than 15% of its current value within
        // one sample period, we consider it a new note attack.
        //
        // The median filter is reset because it may contain the old note's
        // frequency, which would slow down tracking to the new pitch.
        // ======================================================================
        const float envelopeDelta = mEnvelopeLevel - mPrevEnvelopeLevel;
        if (envelopeDelta > kOnsetThreshold * mEnvelopeLevel && mEnvelopeLevel > kGateThreshold)
        {
          mOnsetDetected = true;
          mStableFrameCount = 0;
          // Reset median filter to allow fast tracking of new note
          std::fill(mMedianBuffer.begin(), mMedianBuffer.end(), 0.0);
          mMedianIndex = 0;
        }
        mPrevEnvelopeLevel = mEnvelopeLevel;

        // Run pitch detection every hop size samples
        if (mSamplesCollected >= mHopSize)
        {
          mSamplesCollected = 0;
          DetectPitchAdaptive();
        }

        // Frequency smoothing with adaptive rate
        double freqSmoothCoef = mGlideCoef;
        if (mOnsetDetected || mStableFrameCount < kStableFramesForLock)
        {
          // Faster response during onset or unstable periods
          freqSmoothCoef = std::min(1.0, mGlideCoef * 4.0);
        }

        if (mSmoothedFreq > 0.0 && mPitchConfidence > kConfidenceThreshold)
        {
          const double freqDiff = mSmoothedFreq - mCurrentFreq;
          
          // Jump immediately if large pitch change (new note)
          const double semitoneRatio = mSmoothedFreq / std::max(1.0, mCurrentFreq);
          if (semitoneRatio > 1.5 || semitoneRatio < 0.67)
          {
            mCurrentFreq = mSmoothedFreq;
            mOscPhase = 0.0;  // Reset phase on new note
            mOscPhase2 = 0.0; // Reset 2nd voice phase on new note
          }
          else
          {
            mCurrentFreq += freqSmoothCoef * freqDiff;
          }
        }

        // ======================================================================
        // SAWTOOTH WAVEFORM GENERATION WITH POLYBLEP ANTI-ALIASING
        // ======================================================================
        // A naive sawtooth is just: output = 2 * phase - 1 (ramps -1 to +1)
        //
        // However, the sharp discontinuity at phase=1.0 contains frequencies
        // above Nyquist (sampleRate/2), which "fold back" as aliasing artifacts.
        // This sounds like harsh, inharmonic buzzing.
        //
        // PolyBLEP fixes this by "softening" the discontinuity:
        //   - Near phase=0 (just after the jump), we subtract a polynomial
        //   - Near phase=1 (just before the jump), we add a polynomial
        //   - The polynomial is designed to remove the infinite-slope discontinuity
        //     while preserving the overall sawtooth character
        //
        // The correction region is 2*phaseInc wide (about 2 samples), so we're
        // only modifying a tiny portion of each cycle. The result is a sawtooth
        // that sounds clean even at high frequencies.
        // ======================================================================
        float synthOut = 0.0f;
        if (mCurrentFreq > kMinFrequency && mEnvelopeLevel > kGateThreshold)
        {
          // Apply octave shift: 2^octaveShift multiplies frequency
          // octaveShift=1 doubles freq, octaveShift=-1 halves it
          double freq = mCurrentFreq * std::pow(2.0, mOctaveShift);
          
          // Apply detune in cents (100 cents = 1 semitone, 1200 cents = 1 octave)
          // Formula: freq * 2^(cents/1200)
          freq *= std::pow(2.0, mDetune / 1200.0);

          // Clamp frequency to reasonable range to avoid aliasing at high freq
          // and subsonic rumble at low freq
          // Note: kMinOutputFrequency (20 Hz) is lower than kMinFrequency (50 Hz)
          // to allow octave-down shifting from detected pitches
          freq = std::clamp(freq, kMinOutputFrequency, kMaxFrequency);

          // Phase accumulator: increments by (freq/sampleRate) each sample
          // When phase >= 1.0, it wraps back, creating the sawtooth cycle
          const double phaseInc = freq / mSampleRate;
          mOscPhase += phaseInc;
          if (mOscPhase >= 1.0)
            mOscPhase -= 1.0;

          // Generate voice 1 waveform sample (shape-selected, PolyBLEP anti-aliased)
          float voice1Out = GenerateSample(mOscPhase, phaseInc, mWaveShape, mPulseWidth);

          // Generate 2nd voice with semitone offset
          float voice2Out = 0.0f;
          if (mVoice2Mix > 0.0f)
          {
            // Apply semitone shift: freq * 2^(semitones/12)
            const double freq2 = freq * std::pow(2.0, mVoice2Semitones / 12.0);
            const double freq2Clamped = std::clamp(freq2, kMinOutputFrequency, kMaxFrequency);
            const double phaseInc2 = freq2Clamped / mSampleRate;
            mOscPhase2 += phaseInc2;
            if (mOscPhase2 >= 1.0)
              mOscPhase2 -= 1.0;

            voice2Out = GenerateSample(mOscPhase2, phaseInc2, mWaveShape2, mPulseWidth2);
          }

          // Mix voice 1 and voice 2
          synthOut = voice1Out * (1.0f - mVoice2Mix) + voice2Out * mVoice2Mix;

          // Apply envelope
          synthOut *= mEnvelopeLevel;
        }

        // Apply output gain
        synthOut *= mOutputGain;

        // Mix dry/wet
        const float dryMix = 1.0f - mMix;
        const float wetMix = mMix;
        const float outL = inL * dryMix + synthOut * wetMix;
        const float outR = inR * dryMix + synthOut * wetMix;

        if (outputs[0])
          outputs[0][i] = outL;
        if (outputs[1])
          outputs[1][i] = outR;
      }
    }

    void SetParam(const std::string &key, double value) override
    {
      if (key == "mix")
      {
        mMix = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
      else if (key == "attack")
      {
        mAttackMs = std::clamp(value, 0.1, 100.0);
        UpdateEnvelopeCoefs();
      }
      else if (key == "release")
      {
        mReleaseMs = std::clamp(value, 10.0, 1000.0);
        UpdateEnvelopeCoefs();
      }
      else if (key == "detune")
      {
        mDetune = std::clamp(value, -100.0, 100.0);
      }
      else if (key == "octaveShift")
      {
        mOctaveShift = std::clamp(value, -2.0, 2.0);
      }
      else if (key == "glide")
      {
        mGlideMs = std::clamp(value, 0.0, 500.0);
        UpdateGlideCoef();
      }
      else if (key == "outputGain")
      {
        const double dB = std::clamp(value, -24.0, 12.0);
        mOutputGain = static_cast<float>(std::pow(10.0, dB / 20.0));
      }
      else if (key == "gate")
      {
        const double dB = std::clamp(value, -80.0, 0.0);
        kGateThreshold = static_cast<float>(std::pow(10.0, dB / 20.0));
      }
      else if (key == "voice2Semitones")
      {
        mVoice2Semitones = std::clamp(value, -24.0, 24.0);
      }
      else if (key == "voice2Mix")
      {
        mVoice2Mix = static_cast<float>(std::clamp(value, 0.0, 1.0));
      }
      else if (key == "waveShape")
      {
        mWaveShape = static_cast<int>(std::clamp(std::round(value), 0.0, 3.0));
      }
      else if (key == "pulseWidth")
      {
        mPulseWidth = std::clamp(value, 0.1, 0.9);
      }
      else if (key == "voice2WaveShape")
      {
        mWaveShape2 = static_cast<int>(std::clamp(std::round(value), 0.0, 3.0));
      }
      else if (key == "voice2PulseWidth")
      {
        mPulseWidth2 = std::clamp(value, 0.1, 0.9);
      }
    }

    void SetConfig(const std::string &, const std::string &) override {}

    [[nodiscard]] double GetParam(const std::string &key) const override
    {
      if (key == "mix")
        return mMix;
      if (key == "attack")
        return mAttackMs;
      if (key == "release")
        return mReleaseMs;
      if (key == "detune")
        return mDetune;
      if (key == "octaveShift")
        return mOctaveShift;
      if (key == "glide")
        return mGlideMs;
      if (key == "outputGain")
        return 20.0 * std::log10(mOutputGain + 1e-10f);
      if (key == "gate")
        return 20.0 * std::log10(kGateThreshold + 1e-10f);
      if (key == "voice2Semitones")
        return mVoice2Semitones;
      if (key == "voice2Mix")
        return mVoice2Mix;
      if (key == "waveShape")
        return static_cast<double>(mWaveShape);
      if (key == "pulseWidth")
        return mPulseWidth;
      if (key == "voice2WaveShape")
        return static_cast<double>(mWaveShape2);
      if (key == "voice2PulseWidth")
        return mPulseWidth2;
      return 0.0;
    }

    [[nodiscard]] std::string GetType() const override { return "synth_saw"; }
    [[nodiscard]] std::string GetCategory() const override { return "synth"; }

    // For future MIDI output support
    [[nodiscard]] double GetDetectedFrequency() const { return mCurrentFreq; }
    [[nodiscard]] double GetDetectedMidiNote() const
    {
      if (mCurrentFreq <= 0.0)
        return -1.0;
      // MIDI note = 69 + 12 * log2(freq / 440)
      return 69.0 + 12.0 * std::log2(mCurrentFreq / 440.0);
    }
    [[nodiscard]] float GetPitchConfidence() const { return mPitchConfidence; }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kMinFrequency = 50.0;       // ~G1 - minimum for pitch detection
    static constexpr double kMinOutputFrequency = 20.0; // ~E0 - minimum for synth output (allows -2 octave shift)
    static constexpr double kMaxFrequency = 2000.0;     // ~B6
    static constexpr float kConfidenceThreshold = 0.7f;
    static constexpr float kOnsetThreshold = 0.15f;  // Envelope rise threshold for onset
    static constexpr size_t kStableFramesForLock = 3; // Frames needed for stable pitch
    static constexpr size_t kMedianFilterSize = 5;   // Median filter for outlier rejection

    // YIN algorithm thresholds
    static constexpr float kYinThreshold = 0.10f;       // Lower = more selective
    static constexpr float kYinThresholdHigh = 0.20f;   // Relaxed threshold for tracking

    /**
     * PolyBLEP (Polynomial Bandlimited Step) Anti-Aliasing
     * 
     * This function computes a polynomial correction that smooths the
     * sawtooth's discontinuity without significantly affecting the timbre.
     * 
     * How it works:
     *   - The discontinuity occurs at phase=0 (or equivalently phase=1)
     *   - We define a small region around the discontinuity: [0, phaseInc] and [1-phaseInc, 1]
     *   - Within this region, we compute a polynomial that:
     *     1. Equals zero at the boundaries (smooth transition)
     *     2. Has the same integral as the aliased portion (energy preservation)
     *     3. Has continuous first derivative (no new discontinuities)
     * 
     * The polynomial 2t - t² - 1 (normalized) approximates the ideal bandlimited
     * step function that a perfect DAC would produce. This removes most aliasing
     * while being computationally cheap (just a few multiplies).
     * 
     * @param phase Current oscillator phase [0, 1)
     * @param phaseInc Phase increment per sample (freq/sampleRate)
     * @return Correction value to subtract from naive sawtooth
     */
    static float PolyBLEP(double phase, double phaseInc)
    {
      double t = phase;
      
      // Near phase=0: just after the discontinuity (phase wrapped from 1.0)
      // Apply correction polynomial: 2t - t² - 1 where t = phase/phaseInc ∈ [0,1]
      if (t < phaseInc)
      {
        t /= phaseInc;  // Normalize t to [0, 1] within the correction region
        return static_cast<float>(t + t - t * t - 1.0);
      }
      // Near phase=1: just before the discontinuity (about to wrap)
      // Apply correction polynomial: t² + 2t + 1 where t = (phase-1)/phaseInc ∈ [-1,0]
      else if (t > 1.0 - phaseInc)
      {
        t = (t - 1.0) / phaseInc;  // Normalize t to [-1, 0] within the correction region
        return static_cast<float>(t * t + t + t + 1.0);
      }
      // Outside correction regions: no modification needed
      return 0.0f;
    }

    /**
     * Generate one sample for the chosen waveform with PolyBLEP anti-aliasing.
     *
     * @param phase      Current oscillator phase [0, 1)
     * @param phaseInc   Phase increment per sample (freq / sampleRate)
     * @param shape      0=Saw, 1=Square/Pulse, 2=Triangle, 3=Sine
     * @param pulseWidth Duty cycle for Square waveform [0.1, 0.9] — ignored for other shapes
     * @return           Waveform sample in approximately [-1, +1]
     */
    static float GenerateSample(double phase, double phaseInc, int shape, double pulseWidth)
    {
      switch (shape)
      {
        default:
        case 0: // Sawtooth
        {
          float out = static_cast<float>(2.0 * phase - 1.0);
          out -= PolyBLEP(phase, phaseInc);
          return out;
        }
        case 1: // Square / Pulse — two PolyBLEP corrections (rising edge at 0, falling edge at pulseWidth)
        {
          float out = (phase < pulseWidth) ? 1.0f : -1.0f;
          out += PolyBLEP(phase, phaseInc);              // smooth rising edge
          double phaseFall = phase - pulseWidth;
          if (phaseFall < 0.0) phaseFall += 1.0;
          out -= PolyBLEP(phaseFall, phaseInc);          // smooth falling edge
          return out;
        }
        case 2: // Triangle — piecewise linear; continuous waveform, no PolyBLEP required
        {
          return static_cast<float>(2.0 * std::abs(2.0 * phase - 1.0) - 1.0);
        }
        case 3: // Sine — no anti-aliasing needed
        {
          return static_cast<float>(std::sin(2.0 * kPi * phase));
        }
      }
    }

    void UpdateEnvelopeCoefs()
    {
      const double attackSamples = mAttackMs * mSampleRate / 1000.0;
      const double releaseSamples = mReleaseMs * mSampleRate / 1000.0;
      mAttackCoef = static_cast<float>(1.0 - std::exp(-1.0 / std::max(1.0, attackSamples)));
      mReleaseCoef = static_cast<float>(1.0 - std::exp(-1.0 / std::max(1.0, releaseSamples)));
    }

    void UpdateGlideCoef()
    {
      const double glideSamples = mGlideMs * mSampleRate / 1000.0;
      mGlideCoef = glideSamples > 0.0 ? (1.0 - std::exp(-1.0 / glideSamples)) : 1.0;
    }

    /**
     * Compute adaptive window size based on expected frequency.
     * Smaller windows = faster response for higher pitches.
     */
    size_t ComputeAdaptiveWindowSize(double expectedFreq) const
    {
      if (expectedFreq <= 0.0)
        return mMaxYinBufferSize;

      // Need at least 2.5 periods for YIN (halfSize must be >= period)
      size_t minSamples = static_cast<size_t>(2.5 * mSampleRate / expectedFreq);
      
      // Clamp to valid range
      minSamples = std::max(minSamples, size_t(512));
      minSamples = std::min(minSamples, mMaxYinBufferSize);
      
      return minSamples;
    }

    /**
     * Median filter to reject pitch outliers and octave errors.
     */
    double MedianFilter(double newFreq)
    {
      mMedianBuffer[mMedianIndex] = newFreq;
      mMedianIndex = (mMedianIndex + 1) % kMedianFilterSize;

      // Copy and sort
      std::array<double, kMedianFilterSize> sorted = mMedianBuffer;
      std::sort(sorted.begin(), sorted.end());

      return sorted[kMedianFilterSize / 2];
    }

    /**
     * ========================================================================
     * YIN PITCH DETECTION WITH ADAPTIVE WINDOWING
     * ========================================================================
     * 
     * This is the core pitch detection algorithm. It implements the YIN method
     * (de Cheveigné & Kawahara, 2002) with several enhancements:
     * 
     * STANDARD YIN STEPS:
     *   1. Difference Function - autocorrelation-like measure
     *   2. Cumulative Mean Normalized Difference (CMND) - removes amplitude bias
     *   3. Absolute Threshold - find first minimum below threshold
     *   4. Parabolic Interpolation - sub-sample accuracy refinement
     * 
     * OUR ENHANCEMENTS:
     *   - Adaptive window sizing based on tracked frequency
     *   - Two-stage threshold (strict for initial detection, relaxed for tracking)
     *   - Octave error correction by comparing with previous frequency
     *   - Median filtering for outlier rejection
     * 
     * MATHEMATICAL BACKGROUND:
     * 
     * The difference function d(tau) measures dissimilarity at lag tau:
     *   d(tau) = Σ (x[j] - x[j+tau])²  for j ∈ [0, W/2)
     * 
     * For a periodic signal with period T, d(tau) has minima at tau = T, 2T, 3T...
     * The fundamental is at the first minimum (tau = T), but harmonics can create
     * local minima at tau = T/2, T/3... which causes octave errors.
     * 
     * The CMND normalization prevents d(0)=0 from always being selected:
     *   d'(tau) = d(tau) / [(1/tau) * Σ d(k) for k ∈ [1,tau]]
     * 
     * This makes d'(tau) ≈ 1.0 for non-periodic signals and << 1.0 at the
     * true period, regardless of signal amplitude.
     * 
     * ========================================================================
     */
    void DetectPitchAdaptive()
    {
      // ----------------------------------------------------------------------
      // ADAPTIVE WINDOW SIZING
      // ----------------------------------------------------------------------
      // When we're already tracking a pitch, we can use a smaller window
      // (2.5x the expected period) for faster response. When searching for
      // a new pitch, we use the maximum window to catch low frequencies.
      // ----------------------------------------------------------------------
      if (mTargetFreq > kMinFrequency && mStableFrameCount > 0)
      {
        mCurrentWindowSize = ComputeAdaptiveWindowSize(mTargetFreq);
      }
      else
      {
        // Use larger window when no pitch is known
        mCurrentWindowSize = mMaxYinBufferSize;
      }

      const size_t windowSize = mCurrentWindowSize;
      const size_t halfSize = windowSize / 2;

      // Copy recent samples to YIN buffer (unwrap circular buffer)
      for (size_t i = 0; i < windowSize; ++i)
      {
        size_t idx = (mInputWritePos + mInputBuffer.size() - windowSize + i) % mInputBuffer.size();
        mYinBuffer[i] = mInputBuffer[idx];
      }

      // Check if we have enough signal
      float maxAbs = 0.0f;
      for (size_t i = 0; i < windowSize; ++i)
      {
        maxAbs = std::max(maxAbs, std::abs(mYinBuffer[i]));
      }
      if (maxAbs < kGateThreshold)
      {
        // Too quiet, skip detection
        mPitchConfidence = 0.0f;
        return;
      }

      // ----------------------------------------------------------------------
      // STEP 1: DIFFERENCE FUNCTION d(tau)
      // ----------------------------------------------------------------------
      // For each lag tau, compute how different the signal is from itself
      // shifted by tau samples. The formula is:
      //   d(tau) = Σ (x[j] - x[j+tau])²  for j ∈ [0, halfSize)
      //
      // Intuition:
      //   - For a perfectly periodic signal with period T:
      //     d(T) = 0 because x[j] = x[j+T] for all j
      //   - For noise or non-periodic signals:
      //     d(tau) stays high for all tau
      //
      // We compute this for all tau values from 0 to halfSize-1.
      // The frequency limits will be enforced in the search step.
      // ----------------------------------------------------------------------
      for (size_t tau = 0; tau < halfSize; ++tau)
      {
        float sum = 0.0f;
        for (size_t j = 0; j < halfSize; ++j)
        {
          const float diff = mYinBuffer[j] - mYinBuffer[j + tau];
          sum += diff * diff;
        }
        mYinDiff[tau] = sum;
      }

      // ----------------------------------------------------------------------
      // STEP 2: CUMULATIVE MEAN NORMALIZED DIFFERENCE (CMND)
      // ----------------------------------------------------------------------
      // The raw difference function has a problem: d(0) = 0 always, because
      // any signal matches itself perfectly at zero lag. This would make
      // tau=0 always look like the best "period", which is wrong.
      //
      // The CMND normalization fixes this:
      //   d'(tau) = d(tau) / [(1/tau) * Σ d(k) for k ∈ [1,tau]]
      //           = d(tau) * tau / Σ d(k)
      //
      // This divides d(tau) by the average of all previous d values.
      // At the true period, d(tau) is small while the average is moderate,
      // giving d'(tau) << 1. At tau=0 or random tau, d'(tau) ≈ 1.
      //
      // We set d'(0) = 1.0 by convention (undefined by the formula).
      // The epsilon check (1e-10) prevents division by zero for silence.
      // ----------------------------------------------------------------------
      mYinCumulative[0] = 1.0f;
      float runningSum = 0.0f;
      for (size_t tau = 1; tau < halfSize; ++tau)
      {
        runningSum += mYinDiff[tau];
        if (runningSum > 1e-10f)
        {
          mYinCumulative[tau] = mYinDiff[tau] * static_cast<float>(tau) / runningSum;
        }
        else
        {
          mYinCumulative[tau] = 1.0f;  // Treat silence as "no pitch detected"
        }
      }

      // ----------------------------------------------------------------------
      // STEP 3: ABSOLUTE THRESHOLD SEARCH
      // ----------------------------------------------------------------------
      // We search for the first tau where d'(tau) falls below a threshold.
      // This "first" criterion helps avoid octave errors - the fundamental
      // period T appears before its multiples 2T, 3T, etc.
      //
      // THRESHOLD STRATEGY:
      //   - kYinThreshold (0.10): Strict threshold for initial detection.
      //     Only very clear pitches pass, reducing false positives.
      //   - kYinThresholdHigh (0.20): Relaxed threshold when already tracking.
      //     Allows tracking through slight pitch variations and noise.
      //
      // FREQUENCY RANGE ENFORCEMENT:
      //   tau = sampleRate / frequency, so:
      //   - minTau = sampleRate / maxFreq (highest pitch we want to detect)
      //   - maxTau = sampleRate / minFreq (lowest pitch we want to detect)
      //
      // SEARCH ALGORITHM:
      //   1. Find first tau where d'(tau) < threshold
      //   2. Continue to find the local minimum (keep going while decreasing)
      //   3. Stop at the local minimum - this is our period estimate
      //
      // We always do a full search (no "nearby search" optimization) to ensure
      // we detect octave jumps when playing new notes.
      // ----------------------------------------------------------------------
      const float threshold = (mStableFrameCount > 0) ? kYinThresholdHigh : kYinThreshold;
      
      // Convert frequency limits to tau (period) limits
      // tau = sampleRate / freq, so higher freq = smaller tau
      const size_t minTau = std::max(size_t(2), static_cast<size_t>(mSampleRate / kMaxFrequency));
      const size_t maxTau = std::min(halfSize - 1, static_cast<size_t>(mSampleRate / kMinFrequency));
      
      size_t tauEstimate = 0;
      float minCmnd = 1.0f;

      // Search from minTau (high freq) to maxTau (low freq)
      // Stop at the first local minimum below threshold
      for (size_t tau = minTau; tau <= maxTau; ++tau)
      {
        if (mYinCumulative[tau] < threshold)
        {
          // Found a candidate - now find the exact local minimum
          // by continuing while the function is still decreasing
          while (tau + 1 <= maxTau && mYinCumulative[tau + 1] < mYinCumulative[tau])
          {
            ++tau;
          }
          tauEstimate = tau;
          minCmnd = mYinCumulative[tau];
          break;  // Stop at first valid minimum (fundamental, not harmonic)
        }
      }

      // ----------------------------------------------------------------------
      // STEP 4: PARABOLIC INTERPOLATION
      // ----------------------------------------------------------------------
      // The discrete tau estimate is only accurate to ±0.5 samples, which
      // translates to ~1% frequency error at low frequencies. Parabolic
      // interpolation improves this to ~0.1% by fitting a parabola through
      // three points and finding the true minimum.
      //
      // Given three consecutive d'(tau) values: s0, s1, s2 at tau-1, tau, tau+1,
      // the parabola through them has its minimum at:
      //   delta = (s2 - s0) / (2 * (2*s1 - s0 - s2))
      //   refinedTau = tau + delta
      //
      // The denominator (2*s1 - s0 - s2) measures the "curvature" of the parabola.
      // If it's near zero (flat), we skip interpolation and use the integer tau.
      //
      // CONFIDENCE CALCULATION:
      // We use the CMND value as an inverse confidence measure:
      //   confidence = 1 - min(1, d'(tau))
      // A CMND of 0.05 gives confidence 0.95, while 0.5 gives confidence 0.5.
      // ----------------------------------------------------------------------
      double detectedFreq = 0.0;
      float confidence = 0.0f;

      if (tauEstimate > 1 && tauEstimate < halfSize - 1)
      {
        // Get three consecutive CMND values for parabolic fit
        const float s0 = mYinCumulative[tauEstimate - 1];
        const float s1 = mYinCumulative[tauEstimate];      // Should be the minimum
        const float s2 = mYinCumulative[tauEstimate + 1];
        
        // Parabolic interpolation formula: delta = (s2 - s0) / (2 * (2*s1 - s0 - s2))
        const float denom = 2.0f * s1 - s0 - s2;  // Denominator = curvature
        if (std::abs(denom) > 1e-10f)  // Avoid division by zero for flat regions
        {
          const float delta = (s2 - s0) / (2.0f * denom);
          // Clamp delta to [-1, 1] to prevent wild extrapolation
          const float refinedTau = static_cast<float>(tauEstimate) + std::clamp(delta, -1.0f, 1.0f);
          
          if (refinedTau > 0.0f)
          {
            // frequency = sampleRate / period
            detectedFreq = mSampleRate / refinedTau;
            // Confidence: lower CMND = higher confidence (clearer pitch)
            confidence = 1.0f - std::min(1.0f, minCmnd);
          }
        }
        else
        {
          // Flat region - use integer tau without interpolation
          detectedFreq = mSampleRate / static_cast<double>(tauEstimate);
          confidence = 1.0f - std::min(1.0f, minCmnd);
        }
      }

      // Validate frequency range
      if (detectedFreq < kMinFrequency || detectedFreq > kMaxFrequency)
      {
        detectedFreq = 0.0;
        confidence = 0.0f;
      }

      // ----------------------------------------------------------------------
      // OCTAVE ERROR CORRECTION
      // ----------------------------------------------------------------------
      // YIN can sometimes detect harmonics instead of the fundamental:
      //   - Detecting 2x the true frequency (jumped up an octave)
      //   - Detecting 0.5x the true frequency (jumped down an octave)
      //
      // This happens because harmonics also create minima in the CMND.
      // For example, a 110 Hz note has strong energy at 220 Hz (2nd harmonic),
      // which can look like a period of T/2 in the autocorrelation.
      //
      // CORRECTION STRATEGY:
      // When we detect a frequency that's ~2x or ~0.5x the previous frequency,
      // we check if the CMND value at the expected frequency is actually better.
      // If so, we correct to that frequency instead.
      //
      // CONDITIONS FOR CORRECTION:
      //   1. We're already tracking a pitch (stableFrameCount > 2)
      //   2. The new detection isn't extremely confident (minCmnd > 0.05)
      //   3. The frequency ratio is close to 2.0 or 0.5 (within ±5%)
      //   4. The corrected frequency has better (lower) CMND value
      //
      // We DON'T correct when:
      //   - The detection is very confident (likely a real octave jump)
      //   - We're in onset mode (new note being played)
      //   - The ratio isn't close to an octave (legitimate interval change)
      // ----------------------------------------------------------------------
      if (detectedFreq > 0.0 && mTargetFreq > kMinFrequency && 
          mStableFrameCount > 2 && minCmnd > 0.05f)
      {
        const double ratio = detectedFreq / mTargetFreq;
        
        // Check for octave jump up (detected 2x expected)
        if (ratio > 1.9 && ratio < 2.1)
        {
          // Likely jumped up an octave - check if half frequency has better YIN
          const double halfFreq = detectedFreq / 2.0;
          if (halfFreq >= kMinFrequency)
          {
            const size_t halfTau = static_cast<size_t>(mSampleRate / halfFreq);
            if (halfTau < halfSize)
            {
              // Only correct if half-freq YIN is significantly better
              const float halfCmnd = mYinCumulative[halfTau];
              if (halfCmnd < minCmnd * 0.8f && halfCmnd < kYinThresholdHigh)
              {
                detectedFreq = halfFreq;
                confidence = 1.0f - halfCmnd;
              }
            }
          }
        }
        // Check for octave jump down (detected 0.5x expected)
        else if (ratio > 0.45 && ratio < 0.55)
        {
          // Likely jumped down an octave - check if double frequency has better YIN
          const double doubleFreq = detectedFreq * 2.0;
          if (doubleFreq <= kMaxFrequency)
          {
            const size_t doubleTau = static_cast<size_t>(mSampleRate / doubleFreq);
            if (doubleTau >= minTau && doubleTau < halfSize)
            {
              const float doubleCmnd = mYinCumulative[doubleTau];
              if (doubleCmnd < minCmnd * 0.8f && doubleCmnd < kYinThresholdHigh)
              {
                detectedFreq = doubleFreq;
                confidence = 1.0f - doubleCmnd;
              }
            }
          }
        }
      }

      // Apply median filter
      if (detectedFreq > 0.0 && confidence > kConfidenceThreshold)
      {
        mTargetFreq = detectedFreq;
        mSmoothedFreq = MedianFilter(detectedFreq);
        mPitchConfidence = confidence;
        mStableFrameCount++;
        mOnsetDetected = false;
      }
      else if (confidence > 0.5f && detectedFreq > 0.0)
      {
        // Lower confidence - use but don't update stable count
        mTargetFreq = detectedFreq;
        mSmoothedFreq = MedianFilter(detectedFreq);
        mPitchConfidence = confidence;
      }
      else
      {
        mPitchConfidence = confidence;
        mStableFrameCount = 0;
      }
    }

    // Parameters
    float mMix = 1.0f;
    double mAttackMs = 5.0;
    double mReleaseMs = 100.0;
    double mDetune = 0.0;       // cents
    double mOctaveShift = 0.0;  // octaves (-2 to +2)
    double mGlideMs = 10.0;     // portamento time (reduced default)
    float mOutputGain = 1.0f;
    float kGateThreshold = 0.001f; // -60 dB default

    // Envelope follower
    float mAttackCoef = 0.1f;
    float mReleaseCoef = 0.01f;
    float mEnvelopeLevel = 0.0f;
    float mPrevEnvelopeLevel = 0.0f;

    // Oscillator state
    double mOscPhase = 0.0;
    double mOscPhase2 = 0.0;  // 2nd voice oscillator phase
    double mCurrentFreq = 0.0;
    double mTargetFreq = 0.0;
    double mSmoothedFreq = 0.0;
    double mGlideCoef = 0.1;

    // 2nd voice parameters
    double mVoice2Semitones = 0.0;  // semitone offset (-12 to +12)
    float mVoice2Mix = 0.0f;        // mix between voice 1 and voice 2 (0 = only voice 1)

    // Waveform selection per voice (0=Saw, 1=Square, 2=Triangle, 3=Sine)
    int mWaveShape = 0;
    int mWaveShape2 = 0;
    double mPulseWidth = 0.5;   // Square duty cycle voice 1 [0.1, 0.9]
    double mPulseWidth2 = 0.5;  // Square duty cycle voice 2 [0.1, 0.9]

    // Pitch detection state
    float mPitchConfidence = 0.0f;
    size_t mMaxYinBufferSize = 2048;
    size_t mCurrentWindowSize = 2048;
    size_t mHopSize = 128;
    size_t mStableFrameCount = 0;
    bool mOnsetDetected = false;
    
    std::vector<float> mYinBuffer;
    std::vector<float> mYinDiff;
    std::vector<float> mYinCumulative;

    // Median filter for pitch smoothing
    std::array<double, kMedianFilterSize> mMedianBuffer = {0.0};
    size_t mMedianIndex = 0;

    // Input circular buffer
    std::vector<float> mInputBuffer;
    size_t mInputWritePos = 0;
    size_t mSamplesCollected = 0;
  };

  inline void RegisterSynthSawEffect()
  {
    EffectTypeInfo info;
    info.type = EffectGuids::kSynthSaw;
    info.aliases = {"synth_saw"};
    info.displayName = "Synth Voice";
    info.category = "synth";
    info.description = "Converts audio to synthesized voice via pitch tracking (Saw, Square, Triangle, Sine)";
    info.requiresResource = false;
    info.parameters = {
      {"mix", "Mix", 1.0, 0.0, 1.0, "amount"},
      {"attack", "Attack", 5.0, 0.1, 100.0, "ms"},
      {"release", "Release", 100.0, 10.0, 1000.0, "ms"},
      {"detune", "Detune", 0.0, -100.0, 100.0, "cents"},
      {"octaveShift", "Octave", 0.0, -2.0, 2.0, "oct", "", false, 1.0},
      {"glide", "Glide", 10.0, 0.0, 500.0, "ms"},
      {"outputGain", "Output", 0.0, -24.0, 12.0, "dB"},
      {"gate", "Gate", -60.0, -80.0, 0.0, "dB"},
      {"voice2Semitones", "Voice 2 Pitch",  0.0, -24.0, 24.0, "st",     "",       false, 1.0},
      {"voice2Mix",       "Voice 2 Mix",    0.0,   0.0,  1.0, "amount", ""},
      {"waveShape",       "Wave Shape",     0.0,   0.0,  3.0, "enum",   "voice1", false, 1.0,
          {"Saw", "Square", "Triangle", "Sine"}},
      {"pulseWidth",      "Pulse Width",    0.5,   0.1,  0.9, "amount", "voice1"},
      {"voice2WaveShape", "V2 Wave Shape",  0.0,   0.0,  3.0, "enum",   "voice2", false, 1.0,
          {"Saw", "Square", "Triangle", "Sine"}},
      {"voice2PulseWidth","V2 Pulse Width", 0.5,   0.1,  0.9, "amount", "voice2"}
    };

    EffectRegistry::Instance().Register(info.type, info, []()
      { return std::make_unique<SynthSawEffect>(); });
  }

} // namespace guitarfx
