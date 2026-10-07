#pragma once

#include "dsp/EffectProcessor.h"
#include "dsp/EffectRegistry.h"
#include "dsp/EffectGuids.h"
#include "dsp/PitchTracker.h"
#include "dsp/effects/PitchPresets.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

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
 * 1. PITCH TRACKING
 *    --------------
 *    The shared real-time tracker, dsp/PitchTracker.h: YIN on a copy of the mono
 *    input low-passed at 2 kHz and decimated to about 12 kHz, refined at the full
 *    rate, every 5 ms, from 45 Hz to 2 kHz. Its accepted pitch already rejects
 *    outliers (a jump of over a semitone must repeat on two detections before it is
 *    taken), and it holds the last pitch through silence and a note's dying tail.
 *
 *    This effect used to run a brute-force full-rate YIN of its own every 3 ms on
 *    the audio thread, with a median filter and octave correction on top. Measured
 *    at 48 kHz in 64-sample blocks, that cost 50 us per block on average and 130 us
 *    at the 99th percentile on the DI demo (220 us on noise, rising with the square
 *    of the sample rate); the tracker costs 1.7 us and 7.7 us in Release (built
 *    without /fp:fast its sums go unvectorised, at five times the cost), gets the
 *    oscillator to a new note in 13-60 ms where the old one took up to 83 ms, reads
 *    steady tones within a cent, and reaches down to 45 Hz where the old one
 *    stopped at 50.
 *
 * 2. ONSET DETECTION
 *    ---------------
 *    Guitar notes have transient attacks. The onset detector monitors the envelope
 *    follower and triggers when the level rises significantly (a new note). Until
 *    the tracker has confirmed a pitch for about 10 ms after it, the glide runs four
 *    times faster, so a new note is reached quickly and a held one still glides.
 *
 * 3. POLYBLEP ANTI-ALIASING
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
 *   Input -> Envelope Follower -> Onset Detection
 *     |                                  |
 *     +-> Pitch Tracker -----------> Glide (faster after an onset)
 *                                        |
 *                                        v
 *   Sawtooth Oscillator -> PolyBLEP Anti-aliasing -> Envelope Shaping -> Dry/Wet Mix -> Output
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
 *   - Pulse width modulation (Saw, Square, Triangle and Sine are in)
 *   - Polyphonic pitch detection for chords
 *   - Pitch quantization to musical scales
 *
 * =============================================================================
 */
class SynthSawEffect : public EffectProcessor
{
  public:
    /// Identical sides in, identical sides out, whatever the settings (EffectProcessor::CanWiden).
    [[nodiscard]] bool CanWiden() const override
    {
        return false;
    }

    void Prepare(double sampleRate, int maxBlockSize) override
    {
        mSampleRate = sampleRate;
        mMaxBlockSize = maxBlockSize;
        mTracker.Prepare(sampleRate);

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
        mEnvelopeLevel = 0.0f;
        mPitchConfidence = 0.0f;
        mPrevEnvelopeLevel = 0.0f;
        mOnsetDetected = false;
        mStableFrameCount = 0;
        mTargetHeld = false;
        mTracker.Reset();
        mDetectionsSeen = 0;
    }

    void Process(float** inputs, float** outputs, int numSamples) override
    {
        if (!inputs || !outputs)
        {
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            // Mix to mono for pitch detection
            const float inL = inputs[0] ? inputs[0][i] : 0.0f;
            const float inR = inputs[1] ? inputs[1][i] : 0.0f;
            const float mono = 0.5f * (inL + inR);
            mTracker.Push(mono);

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
            // When a new note is played, the glide runs faster until the tracker
            // has confirmed a pitch for a few detections, so the new note is
            // reached quickly.
            //
            // We detect onsets by monitoring the envelope's rate of change.
            // If the envelope rises by more than 15% of its current value within
            // one sample period, we consider it a new note attack.
            // ======================================================================
            const float envelopeDelta = mEnvelopeLevel - mPrevEnvelopeLevel;

            if (envelopeDelta > kOnsetThreshold * mEnvelopeLevel && mEnvelopeLevel > kGateThreshold)
            {
                mOnsetDetected = true;
                mStableFrameCount = 0;
            }

            mPrevEnvelopeLevel = mEnvelopeLevel;

            if (mTracker.DetectionCount() != mDetectionsSeen)
            {
                mDetectionsSeen = mTracker.DetectionCount();
                OnPitchDetection();
            }

            // Frequency smoothing with adaptive rate
            double freqSmoothCoef = mGlideCoef;

            if (!mTargetHeld && (mOnsetDetected || mStableFrameCount < kStableFramesForLock))
            {
                // Faster response during onset or unstable periods
                freqSmoothCoef = std::min(1.0, mGlideCoef * 4.0);
            }

            if (mTargetFreq > 0.0 && (mTargetHeld || mPitchConfidence > kConfidenceThreshold))
            {
                const double freqDiff = mTargetFreq - mCurrentFreq;

                // Jump immediately if large pitch change (new note)
                const double semitoneRatio = mTargetFreq / std::max(1.0, mCurrentFreq);

                if (semitoneRatio > 1.5 || semitoneRatio < 0.67)
                {
                    mCurrentFreq = mTargetFreq;
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

            if (mCurrentFreq >= PitchTracker::kMinHz && mEnvelopeLevel > kGateThreshold)
            {
                // Apply octave shift: 2^octaveShift multiplies frequency
                // octaveShift=1 doubles freq, octaveShift=-1 halves it
                double freq = mCurrentFreq * mOctaveRatio;

                // Apply detune in cents (100 cents = 1 semitone, 1200 cents = 1 octave)
                // Formula: freq * 2^(cents/1200)
                freq *= mDetuneRatio;

                // Clamp frequency to reasonable range to avoid aliasing at high freq
                // and subsonic rumble at low freq
                // Note: kMinOutputFrequency (20 Hz) is lower than the tracker's lowest
                // pitch (45 Hz) to allow octave-down shifting from detected pitches
                freq = std::clamp(freq, kMinOutputFrequency, kMaxFrequency);

                // Phase accumulator: increments by (freq/sampleRate) each sample
                // When phase >= 1.0, it wraps back, creating the sawtooth cycle
                const double phaseInc = freq / mSampleRate;
                mOscPhase += phaseInc;

                if (mOscPhase >= 1.0)
                {
                    mOscPhase -= 1.0;
                }

                // Generate voice 1 waveform sample (shape-selected, PolyBLEP anti-aliased)
                float voice1Out = GenerateSample(mOscPhase, phaseInc, mWaveShape, mPulseWidth);

                // Generate 2nd voice with semitone offset
                float voice2Out = 0.0f;

                if (mVoice2Mix > 0.0f)
                {
                    // Apply semitone shift: freq * 2^(semitones/12)
                    const double freq2 = freq * mVoice2Ratio;
                    const double freq2Clamped = std::clamp(freq2, kMinOutputFrequency, kMaxFrequency);
                    const double phaseInc2 = freq2Clamped / mSampleRate;
                    mOscPhase2 += phaseInc2;

                    if (mOscPhase2 >= 1.0)
                    {
                        mOscPhase2 -= 1.0;
                    }

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
            {
                outputs[0][i] = outL;
            }

            if (outputs[1])
            {
                outputs[1][i] = outR;
            }
        }
    }

    void SetParam(const std::string& key, double value) override
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
            mDetuneRatio = std::pow(2.0, mDetune / 1200.0);
        }
        else if (key == "octaveShift")
        {
            mOctaveShift = std::clamp(value, -2.0, 2.0);
            mOctaveRatio = std::pow(2.0, mOctaveShift);
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
            mVoice2Ratio = std::pow(2.0, mVoice2Semitones / 12.0);
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

    void SetConfig(const std::string&, const std::string&) override
    {
    }

    [[nodiscard]] double GetParam(const std::string& key) const override
    {
        if (key == "mix")
        {
            return mMix;
        }

        if (key == "attack")
        {
            return mAttackMs;
        }

        if (key == "release")
        {
            return mReleaseMs;
        }

        if (key == "detune")
        {
            return mDetune;
        }

        if (key == "octaveShift")
        {
            return mOctaveShift;
        }

        if (key == "glide")
        {
            return mGlideMs;
        }

        if (key == "outputGain")
        {
            return 20.0 * std::log10(mOutputGain + 1e-10f);
        }

        if (key == "gate")
        {
            return 20.0 * std::log10(kGateThreshold + 1e-10f);
        }

        if (key == "voice2Semitones")
        {
            return mVoice2Semitones;
        }

        if (key == "voice2Mix")
        {
            return mVoice2Mix;
        }

        if (key == "waveShape")
        {
            return static_cast<double>(mWaveShape);
        }

        if (key == "pulseWidth")
        {
            return mPulseWidth;
        }

        if (key == "voice2WaveShape")
        {
            return static_cast<double>(mWaveShape2);
        }

        if (key == "voice2PulseWidth")
        {
            return mPulseWidth2;
        }

        return 0.0;
    }

    [[nodiscard]] std::string GetType() const override
    {
        return "synth_saw";
    }

    [[nodiscard]] std::string GetCategory() const override
    {
        return "synth";
    }

    // For future MIDI output support
    [[nodiscard]] double GetDetectedFrequency() const
    {
        return mCurrentFreq;
    }

    [[nodiscard]] double GetDetectedMidiNote() const
    {
        if (mCurrentFreq <= 0.0)
        {
            return -1.0;
        }

        // MIDI note = 69 + 12 * log2(freq / 440)
        return 69.0 + 12.0 * std::log2(mCurrentFreq / 440.0);
    }

    [[nodiscard]] float GetPitchConfidence() const
    {
        return mPitchConfidence;
    }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kMinOutputFrequency = 20.0; // ~E0 - minimum for synth output (allows -2 octave shift)
    static constexpr double kMaxFrequency = 2000.0;     // ~B6
    static constexpr float kConfidenceThreshold = 0.7f;
    static constexpr float kOnsetThreshold = 0.15f; // Envelope rise threshold for onset
    /// Confident detections, 5 ms apart, before the glide slows back down after an onset: about the
    /// 10 ms the old detector's three 3 ms frames took.
    static constexpr size_t kStableFramesForLock = 2;

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
            t /= phaseInc; // Normalize t to [0, 1] within the correction region
            return static_cast<float>(t + t - t * t - 1.0);
        }
        // Near phase=1: just before the discontinuity (about to wrap)
        // Apply correction polynomial: t² + 2t + 1 where t = (phase-1)/phaseInc ∈ [-1,0]
        else if (t > 1.0 - phaseInc)
        {
            t = (t - 1.0) / phaseInc; // Normalize t to [-1, 0] within the correction region
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
     * @return           Waveform sample, zero-mean: in approximately [-1, +1], the Square in
     *                   [-2 * pulseWidth, 2 - 2 * pulseWidth]
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
            out += PolyBLEP(phase, phaseInc); // smooth rising edge
            double phaseFall = phase - pulseWidth;

            if (phaseFall < 0.0)
            {
                phaseFall += 1.0;
            }

            out -= PolyBLEP(phaseFall, phaseInc); // smooth falling edge

            // At +1 for pulseWidth of the cycle and -1 for the rest, the pulse's mean is
            // 2 * pulseWidth - 1, which the envelope would turn into a DC step on every note: a
            // thump. Taking it off makes the wave zero-mean at any width, as an AC-coupled analog
            // pulse is, between 2 - 2 * pulseWidth and -2 * pulseWidth. The PolyBLEP corrections
            // stay as they are: each edge is still a step of 2, and a correction integrates to zero.
            out -= static_cast<float>(2.0 * pulseWidth - 1.0);
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
     * A new estimate from the tracker, every 5 ms. Its accepted pitch is the glide's target: a jump of
     * over a semitone has already been confirmed on two detections, so a stray octave never reaches
     * the oscillator. While the tracker finds no pitch (silence, noise, a note dying away) the target
     * is the pitch it holds, the note's own from before it began to stop, and the glide goes on to it
     * at the Glide time. The last estimate it accepted may already have heard the start of the
     * silence, which on a low note reads several cents out, and the tail would otherwise keep it.
     */
    void OnPitchDetection()
    {
        if (!mTracker.HasPitch())
        {
            mPitchConfidence = 0.0f;
            mStableFrameCount = 0;

            if (const double held = mTracker.FrequencyHz(); held > 0.0)
            {
                mTargetFreq = held;
                mTargetHeld = true;
            }

            return;
        }

        mTargetFreq = mTracker.FrequencyHz();
        mPitchConfidence = mTracker.Confidence();
        mTargetHeld = false;

        if (mPitchConfidence > kConfidenceThreshold)
        {
            ++mStableFrameCount;
            mOnsetDetected = false;
        }
    }

    // Parameters
    float mMix = 1.0f;
    double mAttackMs = 5.0;
    double mReleaseMs = 100.0;
    double mDetune = 0.0;      // cents
    double mOctaveShift = 0.0; // octaves (-2 to +2)
    // Frequency ratios, worked out when their parameter changes rather than with a std::pow per sample
    double mDetuneRatio = 1.0; // 2^(detune / 1200)
    double mOctaveRatio = 1.0; // 2^octaveShift
    double mGlideMs = 10.0;    // portamento time (reduced default)
    float mOutputGain = 1.0f;
    float kGateThreshold = 0.001f; // -60 dB default

    // Envelope follower
    float mAttackCoef = 0.1f;
    float mReleaseCoef = 0.01f;
    float mEnvelopeLevel = 0.0f;
    float mPrevEnvelopeLevel = 0.0f;

    // Oscillator state
    double mOscPhase = 0.0;
    double mOscPhase2 = 0.0; // 2nd voice oscillator phase
    double mCurrentFreq = 0.0;
    double mTargetFreq = 0.0; ///< the tracker's accepted pitch, which the glide heads for
    bool mTargetHeld = false; ///< the target is the pitch the tracker holds while it finds none
    double mGlideCoef = 0.1;

    // 2nd voice parameters
    double mVoice2Semitones = 0.0; // semitone offset (-24 to +24)
    double mVoice2Ratio = 1.0;     // 2^(voice2Semitones / 12)
    float mVoice2Mix = 0.0f;       // mix between voice 1 and voice 2 (0 = only voice 1)

    // Waveform selection per voice (0=Saw, 1=Square, 2=Triangle, 3=Sine)
    int mWaveShape = 0;
    int mWaveShape2 = 0;
    double mPulseWidth = 0.5;  // Square duty cycle voice 1 [0.1, 0.9]
    double mPulseWidth2 = 0.5; // Square duty cycle voice 2 [0.1, 0.9]

    // Pitch detection state
    PitchTracker mTracker;
    std::uint64_t mDetectionsSeen = 0;
    float mPitchConfidence = 0.0f;
    size_t mStableFrameCount = 0;
    bool mOnsetDetected = false;
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
        {"voice2Semitones", "Voice 2 Pitch", 0.0, -24.0, 24.0, "st", "", false, 1.0},
        {"voice2Mix", "Voice 2 Mix", 0.0, 0.0, 1.0, "amount", ""},
        {"waveShape", "Wave Shape", 0.0, 0.0, 3.0, "enum", "voice1", false, 1.0, {"Saw", "Square", "Triangle", "Sine"}},
        {"pulseWidth", "Pulse Width", 0.5, 0.1, 0.9, "amount", "voice1"},
        {"voice2WaveShape",
         "V2 Wave Shape",
         0.0,
         0.0,
         3.0,
         "enum",
         "voice2",
         false,
         1.0,
         {"Saw", "Square", "Triangle", "Sine"}},
        {"voice2PulseWidth", "V2 Pulse Width", 0.5, 0.1, 0.9, "amount", "voice2"}};
    info.presets = pitch_presets::SynthVoice(info.parameters);

    EffectRegistry::Instance().Register(info.type, info, []() { return std::make_unique<SynthSawEffect>(); });
}
} // namespace guitarfx
