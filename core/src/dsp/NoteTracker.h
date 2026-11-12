#pragma once

#include "dsp/BiquadDesign.h"
#include "dsp/FiniteCheck.h"
#include "dsp/PitchTracker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace guitarfx
{
/**
 * Real-time monophonic note detection for guitar: where notes start and stop, how hard they were
 * picked, and where their pitch goes in between.
 *
 * PitchTracker (dsp/PitchTracker.h) says what pitch is sounding, continuously. That is what a
 * pitch-following oscillator, a tuner or a tracking ring modulator wants. A MIDI note stream wants
 * something else: "a note began here, this hard, at this pitch; it moved there; it stopped here".
 * This turns the one into the other, with two level followers beside the tracker:
 *
 * - Attacks. A pick throws broadband energy that the ringing string then loses within tens of
 *   milliseconds, so the power above 4 kHz, followed over 1.5 ms, jumps 6 dB over the same power
 *   followed over 50 ms. That finds a re-pick of a note that is still ringing, which the overall
 *   level barely shows; lower, the harmonics of a high note that is still ringing hide it. An
 *   attack re-arms once the two have come back together.
 * - Starts. After an attack the note is placed at the first detection whose estimate the tracker
 *   has accepted as its pitch. If a note was already sounding and that reads the same pitch, it
 *   is only believed once the tracker's window has mostly moved past the attack: until then it may
 *   be reading the old note. A pitch held above the threshold for three detections with no attack
 *   seen (a volume swell) starts a note too.
 * - Moves. A move of over a semitone from the note's pitch (a hammer-on, pull-off or slide) is
 *   reported as a legato note, anything smaller (a bend, vibrato) as a glide; either only once two
 *   detections in a row agree, since one whose window straddles the move reads between the two.
 *   A move of an octave has to hold for three: a power chord or a strong second harmonic can flip
 *   the tracker between a note and its octave, and a legato octave is rare in playing.
 * - Stops. The note ends when it is muted (the level falls 20 dB under the note's own, which a
 *   ringing string never does that fast; not in the note's first 50 ms), when the level stays
 *   6 dB under the threshold for longer than the longest period tracked, or when nothing pitched
 *   has been found for 100 ms.
 *
 * The threshold cannot usefully go below about -52 dBFS: the pitch tracker holds its last pitch
 * rather than read anything under -55 dBFS RMS.
 *
 * Velocity is the attack's peak, placed between the threshold (0) and -6 dBFS (1).
 *
 * The events come out through a callback, at the sample they were decided on, so a caller can
 * place them within its block. Prepare() allocates; nothing else does, and nothing locks.
 */
class NoteTracker
{
  public:
    struct Event
    {
        enum class Type : std::uint8_t
        {
            Start,  ///< a note began: pitch and velocity are set
            Legato, ///< the note moved to a new pitch without being picked again
            Glide,  ///< the note's pitch moved a little: a bend or vibrato
            Stop    ///< the note ended
        };

        Type type = Type::Start;
        double pitch = 0.0;    ///< MIDI note number, fractional: 69 is A4 at 440 Hz
        float velocity = 0.0f; ///< 0 to 1 (Start only)
        int sampleOffset = 0;
    };

    static constexpr double kDefaultThresholdDb = -50.0;

    void Prepare(double sampleRate)
    {
        mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        mTracker.Prepare(mSampleRate);
        // Notes are played from this, so a new one read a few milliseconds sooner is worth more
        // than its first reading being exact: the tracker's long window takes over within a few
        // detections, and the move comes out as a glide.
        mTracker.SetFastNoteChanges(true);
        mHighPass = biquad::HighPass(kAttackHighPassHz, 0.7071067811865476, mSampleRate);
        mFastCoef = SmoothingCoefficient(kAttackFastSeconds);
        mSlowCoef = SmoothingCoefficient(kAttackSlowSeconds);
        mLevelRelease = static_cast<float>(std::exp(-1.0 / (kLevelReleaseSeconds * mSampleRate)));
        mNoteLevelRelease = static_cast<float>(std::exp(-1.0 / (kNoteLevelReleaseSeconds * mSampleRate)));
        mRefractorySamples = SecondsToSamples(kAttackRefractorySeconds);
        mPendingTimeoutSamples = SecondsToSamples(kPendingTimeoutSeconds);
        mUnpitchedStopSamples = SecondsToSamples(kUnpitchedStopSeconds);
        mHopSamples = SecondsToSamples(kDetectionSeconds);
        UpdateRange();
        Reset();
    }

    void Reset()
    {
        mTracker.Reset();
        mFilterState.Reset();
        mFastPower = 0.0f;
        mSlowPower = 0.0f;
        mLevel = 0.0f;
        mNoteLevel = 0.0f;
        mArmed = true;
        mSinceAttack = mRefractorySamples;
        mDetectionsSeen = mTracker.DetectionCount();
        mSinceDetection = 0;
        mLastRaw = 0.0;
        ClearNote();
    }

    /// The level, in dBFS, a note has to reach to start; it stops 6 dB below.
    void SetThresholdDb(double dB)
    {
        const double clamped = IsFinite(dB) ? std::clamp(dB, -96.0, 0.0) : kDefaultThresholdDb;
        mThresholdDb = clamped;
        mOnLevel = static_cast<float>(std::pow(10.0, clamped / 20.0));
        mOffLevel = mOnLevel * kOffLevelRatio;
    }

    /// The lowest pitch looked for (see PitchTracker::SetLowestFrequency). A change drops any note.
    void SetLowestFrequency(double hz)
    {
        const double before = mTracker.LowestFrequencyHz();
        mTracker.SetLowestFrequency(hz);

        if (mTracker.LowestFrequencyHz() != before)
        {
            UpdateRange();
            Reset();
        }
    }

    [[nodiscard]] double LowestFrequencyHz() const
    {
        return mTracker.LowestFrequencyHz();
    }

    /// Whether a note's start pitch has to say which note it is (the default), or only has to be
    /// near it. A first reading taken as the note begins can be half a semitone out, which rounds
    /// to the note next door; with this on, one that far off a note's centre waits for the next
    /// detection, up to 5 ms. A caller that follows the pitch on from the start, as a pitch bend
    /// does, loses nothing by starting on the early reading and can turn it off.
    void SetExactStarts(bool on)
    {
        mExactStarts = on;
    }

    /// Feeds `numSamples` of mono input, calling `sink(const Event&)` for each decision, with the
    /// event's sampleOffset set to its index in `input`. A null input counts as silence.
    template <typename Sink> void Process(const float* input, int numSamples, Sink&& sink)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float raw = input ? input[i] : 0.0f;
            const float x = IsFinite(raw) ? raw : 0.0f;
            mTracker.Push(x);
            ++mSinceDetection;
            FollowLevels(x);

            if (mSinceAttack < mRefractorySamples)
            {
                ++mSinceAttack;
            }

            if (mPending)
            {
                ++mPendingAge;
                mPendingPeak = std::max(mPendingPeak, std::abs(x));
            }

            if (DetectAttack())
            {
                BeginPending(std::abs(x));
            }

            if (mTracker.DetectionCount() != mDetectionsSeen)
            {
                mDetectionsSeen = mTracker.DetectionCount();
                OnDetection(i, sink);
                mSinceDetection = 0;
            }

            CheckLevelStop(i, sink);
        }
    }

    [[nodiscard]] bool IsSounding() const
    {
        return mSounding;
    }

    /// The sounding note's pitch as a fractional MIDI note number; 0 when none is sounding.
    [[nodiscard]] double Pitch() const
    {
        return mSounding ? mPitch : 0.0;
    }

    [[nodiscard]] float Velocity() const
    {
        return mSounding ? mVelocity : 0.0f;
    }

    /// An attack has been seen and is waiting for its pitch: a note is about to start, or a
    /// chord or scrape whose pitch never comes.
    [[nodiscard]] bool IsPending() const
    {
        return mPending;
    }

    [[nodiscard]] static double MidiFromHz(double hz)
    {
        return 69.0 + 12.0 * std::log2(hz / 440.0);
    }

  private:
    static constexpr double kAttackHighPassHz = 4000.0;
    static constexpr double kAttackFastSeconds = 0.0015;
    static constexpr double kAttackSlowSeconds = 0.05;
    /// The fast high-band power over the slow one that marks an attack (+6 dB)...
    static constexpr float kAttackRatio = 4.0f;
    /// ...and the ratio it has to fall back under before another can be seen.
    static constexpr float kRearmRatio = 1.6f;
    static constexpr double kAttackRefractorySeconds = 0.04;
    /// How long an attack waits for a pitch before it is written off as noise.
    static constexpr double kPendingTimeoutSeconds = 0.12;
    /// A re-pick reading the pitch already sounding is believed once this much of the tracker's
    /// half-window lies after the attack.
    static constexpr double kSamePitchSettle = 0.75;
    static constexpr double kLevelReleaseSeconds = 0.015;
    static constexpr float kOffLevelRatio = 0.5f; ///< -6 dB
    /// The note's own level, followed slowly: a string that falls 20 dB under it within a few tens
    /// of milliseconds has been muted, which a string left ringing never does...
    static constexpr double kNoteLevelReleaseSeconds = 0.2;
    static constexpr float kMuteRatio = 0.1f;
    /// ...and it has to stay there this long, past the dip between two peaks of a low note. A pick
    /// is often 20 dB over the note it leaves, and a palm-muted chug falls that far at once, so a
    /// note has to have sounded a while before a mute can end it: a blip of a few milliseconds is
    /// no use to an instrument.
    static constexpr double kMuteHoldSeconds = 0.005;
    static constexpr double kMuteAfterSeconds = 0.05;
    /// A legato move of an octave or two has to hold for this many detections: a string with a
    /// strong second harmonic, or a power chord, can flip the tracker between a note and its
    /// octave, and a real legato octave is rare.
    static constexpr int kOctaveDetections = 3;
    static constexpr double kUnpitchedStopSeconds = 0.1;
    static constexpr int kSwellDetections = 3;
    /// The tracker detects every 5 ms.
    static constexpr double kDetectionSeconds = 0.005;
    static constexpr double kFullVelocityDb = -6.0;
    /// An estimate within this many semitones of the tracker's accepted pitch is one it took.
    static constexpr double kAcceptedSemitones = 0.5;
    static constexpr double kSamePitchSemitones = 0.5;
    /// Over a ringing note, two detections in a row this close are needed: one whose window
    /// straddles the two notes reads a pitch between them.
    static constexpr double kStableSemitones = 0.25;
    /// The tracker takes a move of up to a semitone straight away; one bigger it had to confirm.
    static constexpr double kJumpSemitones = 1.0;
    /// An early reading (PitchTracker::IsRoughEstimate) further than this from a whole note waits
    /// for one more detection before it starts the note.
    static constexpr double kRoughOffCentreSemitones = 0.2;

    /// Whether a move of `semitones` is within half a semitone of a whole number of octaves.
    [[nodiscard]] static bool IsOctaves(double semitones)
    {
        const double octaves = std::abs(semitones) / 12.0;
        const double nearest = std::round(octaves);
        return nearest >= 1.0 && std::abs(octaves - nearest) * 12.0 < 0.5;
    }

    /// The time `detections` of the tracker's usual 5 ms detections span, in samples, less half a
    /// detection for rounding. The tracker detects more often while it looks for a note, so what
    /// was once counted in detections is timed.
    [[nodiscard]] int HeldFor(int detections) const
    {
        return (detections - 1) * mHopSamples - mHopSamples / 2;
    }

    [[nodiscard]] int SecondsToSamples(double seconds) const
    {
        return std::max(1, static_cast<int>(std::lround(seconds * mSampleRate)));
    }

    [[nodiscard]] float SmoothingCoefficient(double seconds) const
    {
        return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * mSampleRate)));
    }

    /// The timings that depend on the lowest pitch: a stop waits out one longest period (plus a
    /// little) so the gap between two peaks of a low note never reads as the note ending.
    void UpdateRange()
    {
        const double longestPeriod = 1.0 / mTracker.LowestFrequencyHz();
        mOffHoldSamples = SecondsToSamples(std::max(0.02, 1.25 * longestPeriod));
        mSamePitchSettleSamples = SecondsToSamples(kSamePitchSettle * longestPeriod);
        mMuteHoldSamples = SecondsToSamples(kMuteHoldSeconds);
        mMuteAfterSamples = SecondsToSamples(kMuteAfterSeconds);
    }

    void ClearNote()
    {
        mSounding = false;
        mPitch = 0.0;
        mVelocity = 0.0f;
        mPending = false;
        mPendingAge = 0;
        mPendingPeak = 0.0f;
        mPendingOverNote = false;
        mSwellHits = 0;
        mUnpitchedSamples = 0;
        mBelowSamples = 0;
        mMutedSamples = 0;
        mSoundingSamples = 0;
        mOctaveHits = 0;
    }

    void FollowLevels(float x)
    {
        const auto high = static_cast<float>(mFilterState.Process(mHighPass, x));
        const float power = high * high;
        mFastPower += mFastCoef * (power - mFastPower);
        mSlowPower += mSlowCoef * (power - mSlowPower);

        const float magnitude = std::abs(x);
        mLevel = magnitude > mLevel ? magnitude : mLevel * mLevelRelease;
        mNoteLevel = magnitude > mNoteLevel ? magnitude : mNoteLevel * mNoteLevelRelease;
    }

    [[nodiscard]] bool DetectAttack()
    {
        const float floor = mOnLevel * mOnLevel * 1.0e-4f; // well under anything audible above the threshold

        if (!mArmed)
        {
            if (mFastPower < kRearmRatio * mSlowPower + floor)
            {
                mArmed = true;
            }

            return false;
        }

        if (mSinceAttack < mRefractorySamples || mLevel < mOnLevel)
        {
            return false;
        }

        if (mFastPower > kAttackRatio * mSlowPower + floor)
        {
            mArmed = false;
            mSinceAttack = 0;
            return true;
        }

        return false;
    }

    void BeginPending(float magnitude)
    {
        mPending = true;
        mPendingAge = 0;
        mPendingPeak = std::max(magnitude, mLevel);
        mPendingOverNote = mSounding;
        mWaitedForBetter = false;
        mSwellHits = 0;
    }

    [[nodiscard]] float VelocityFromPeak(float peak) const
    {
        if (!(peak > 0.0f))
        {
            return 0.0f;
        }

        const double dB = 20.0 * std::log10(static_cast<double>(peak));
        const double span = kFullVelocityDb - mThresholdDb;
        return static_cast<float>(std::clamp(span > 0.0 ? (dB - mThresholdDb) / span : 1.0, 0.0, 1.0));
    }

    template <typename Sink> void Emit(Sink& sink, Event::Type type, int offset, double pitch, float velocity = 0.0f)
    {
        Event event;
        event.type = type;
        event.pitch = pitch;
        event.velocity = velocity;
        event.sampleOffset = offset;
        sink(static_cast<const Event&>(event));
    }

    template <typename Sink> void Start(Sink& sink, int offset, double pitch, float peak)
    {
        mSounding = true;
        mPitch = pitch;
        mVelocity = VelocityFromPeak(peak);
        mNoteLevel = std::max(mLevel, peak);
        mSoundingSamples = 0;
        mPending = false;
        mSwellHits = 0;
        mUnpitchedSamples = 0;
        mBelowSamples = 0;
        Emit(sink, Event::Type::Start, offset, mPitch, mVelocity);
    }

    template <typename Sink> void Stop(Sink& sink, int offset)
    {
        const bool wasSounding = mSounding;
        const double pitch = mPitch;
        ClearNote();

        if (wasSounding)
        {
            Emit(sink, Event::Type::Stop, offset, pitch);
        }
    }

    template <typename Sink> void OnDetection(int offset, Sink& sink)
    {
        const bool pitched = mTracker.HasPitch() && mTracker.FrequencyHz() > 0.0;
        const double accepted = pitched ? MidiFromHz(mTracker.FrequencyHz()) : 0.0;
        const double raw = pitched ? MidiFromHz(mTracker.RawFrequencyHz()) : 0.0;
        // The tracker holds a pitch through a note change until the new one has been seen twice,
        // so an estimate it has not taken yet says nothing about which note this is.
        const bool settled = pitched && std::abs(raw - accepted) <= kAcceptedSemitones;
        const bool stable = pitched && mLastRaw > 0.0 && std::abs(raw - mLastRaw) <= kStableSemitones;
        mLastRaw = raw;

        if (pitched)
        {
            mUnpitchedSamples = 0;
        }
        else
        {
            mUnpitchedSamples += mSinceDetection;
        }

        if (mPending)
        {
            if (settled && (stable || !mPendingOverNote))
            {
                const bool samePitch = mPendingOverNote && std::abs(accepted - mPitch) <= kSamePitchSemitones;

                if (!samePitch || mPendingAge >= mSamePitchSettleSamples)
                {
                    // An early reading well off a note's centre may belong to the note either
                    // side: 54 cents sharp, 2.5 ms into a high E, is the F above. One more
                    // detection settles which, and costs a note that is in tune nothing.
                    const bool offCentre = std::abs(accepted - std::round(accepted)) > kRoughOffCentreSemitones;

                    if (mExactStarts && mTracker.IsRoughEstimate() && offCentre && !mWaitedForBetter)
                    {
                        mWaitedForBetter = true;
                        return;
                    }

                    Start(sink, offset, accepted, mPendingPeak);
                    return;
                }
            }

            if (mPendingAge >= mPendingTimeoutSamples)
            {
                // No pitch came of it: noise, or a scrape. A note that was sounding carries on.
                mPending = false;
            }

            if (mSounding && mUnpitchedSamples >= mUnpitchedStopSamples)
            {
                Stop(sink, offset);
            }

            return;
        }

        if (mSounding)
        {
            if (!pitched)
            {
                if (mUnpitchedSamples >= mUnpitchedStopSamples)
                {
                    Stop(sink, offset);
                }

                return;
            }

            // A detection whose window straddles a hammer-on reads between the two pitches, and
            // the tracker follows it there if it is under a semitone off. Wait for one that agrees
            // with the detection before it, and judge the move against the note's own pitch.
            if (!stable)
            {
                return;
            }

            if (std::abs(accepted - mPitch) > kJumpSemitones)
            {
                if (IsOctaves(accepted - mPitch))
                {
                    mOctaveSamples = mOctaveHits++ > 0 ? mOctaveSamples + mSinceDetection : 0;

                    if (mOctaveSamples < HeldFor(kOctaveDetections))
                    {
                        return;
                    }
                }

                mOctaveHits = 0;
                mPitch = accepted;
                Emit(sink, Event::Type::Legato, offset, mPitch);
                return;
            }

            mOctaveHits = 0;

            if (accepted != mPitch)
            {
                mPitch = accepted;
                Emit(sink, Event::Type::Glide, offset, mPitch);
            }

            return;
        }

        // Nothing sounding and no attack: a note faded in (a volume swell), or an attack too soft
        // to see, still becomes a note once its pitch has held for a few detections.
        if (settled && mLevel >= mOnLevel)
        {
            mSwellSamples = mSwellHits++ > 0 ? mSwellSamples + mSinceDetection : 0;

            if (mSwellSamples >= HeldFor(kSwellDetections))
            {
                Start(sink, offset, accepted, mLevel);
            }
        }
        else
        {
            mSwellHits = 0;
        }
    }

    template <typename Sink> void CheckLevelStop(int offset, Sink& sink)
    {
        if (!mSounding && !mPending)
        {
            return;
        }

        mSoundingSamples = mSounding ? std::min(mSoundingSamples + 1, mMuteAfterSamples) : 0;
        const bool muted = mSoundingSamples >= mMuteAfterSamples && mLevel < kMuteRatio * mNoteLevel;
        mMutedSamples = muted ? mMutedSamples + 1 : 0;
        mBelowSamples = mLevel < mOffLevel ? mBelowSamples + 1 : 0;

        if (mBelowSamples >= mOffHoldSamples || mMutedSamples >= mMuteHoldSamples)
        {
            if (mSounding)
            {
                Stop(sink, offset);
            }
            else
            {
                ClearNote();
            }
        }
    }

    PitchTracker mTracker;
    double mSampleRate = 48000.0;

    BiquadCoefficients mHighPass;
    biquad::State mFilterState;
    float mFastCoef = 0.0f;
    float mSlowCoef = 0.0f;
    float mFastPower = 0.0f;
    float mSlowPower = 0.0f;
    float mLevel = 0.0f;
    float mLevelRelease = 0.0f;
    float mNoteLevel = 0.0f;
    float mNoteLevelRelease = 0.0f;
    bool mArmed = true;
    int mSinceAttack = 0;
    int mRefractorySamples = 1;

    double mThresholdDb = kDefaultThresholdDb;
    float mOnLevel = 0.0031623f; ///< -50 dBFS
    float mOffLevel = 0.0015811f;

    int mPendingTimeoutSamples = 1;
    int mUnpitchedStopSamples = 1;
    int mOffHoldSamples = 1;
    int mMuteHoldSamples = 1;
    int mMuteAfterSamples = 1;
    int mHopSamples = 1;
    int mSamePitchSettleSamples = 1;

    std::uint64_t mDetectionsSeen = 0;
    double mLastRaw = 0.0; ///< the previous detection's own estimate; 0 when it found none

    bool mSounding = false;
    double mPitch = 0.0;
    float mVelocity = 0.0f;
    bool mPending = false;
    int mPendingAge = 0;
    float mPendingPeak = 0.0f;
    bool mPendingOverNote = false; ///< a note was sounding when the attack came
    int mSwellHits = 0;
    int mUnpitchedSamples = 0;
    int mBelowSamples = 0;
    int mMutedSamples = 0;
    int mSoundingSamples = 0; ///< since the note started, up to mMuteAfterSamples
    int mOctaveHits = 0;
    bool mExactStarts = true;
    bool mWaitedForBetter = false; ///< this attack's note has already waited on an early reading
    int mSinceDetection = 0;       ///< samples since the tracker's last detection
    int mOctaveSamples = 0;        ///< how long an octave move has held
    int mSwellSamples = 0;         ///< how long an unpicked pitch has held
};
} // namespace guitarfx
