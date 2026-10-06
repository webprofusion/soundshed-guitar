#pragma once

#include "dsp/FiniteCheck.h"
#include "dsp/MusicalScale.h"
#include "dsp/OnsetPitchEstimator.h"
#include "dsp/PickAttackDetector.h"
#include "dsp/PitchTracker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace guitarfx
{
/**
 * Which note of a key is being played on a single-note line, decided as soon after each pick as
 * it can be: what a harmony in a key, or anything else that needs the scale degree, follows.
 *
 * Three parts, each doing what it is best at:
 *  - PickAttackDetector (dsp/PickAttackDetector.h) finds the picks, as SpliceTransposer's own copy
 *    does, so a harmony voice and this agree on where a note starts.
 *  - After each pick, OnsetPitchEstimator (dsp/OnsetPitchEstimator.h) estimates the new note from
 *    the audio since the pick alone. Two estimates in a row on the same scale note confirm it.
 *    An early estimate (one that has not yet searched far enough to rule out a lower fundamental)
 *    only counts within kEarlyReachSemitones of the note before: right after a low note's pick a
 *    strong fifth harmonic reads two octaves and more too high. On real riffs from E2 to C3 this
 *    confirms a note in 20-32 ms, where PitchTracker's fixed window took 50-75 ms or never.
 *  - Between picks PitchTracker (dsp/PitchTracker.h) follows legato notes, slides and bends, once
 *    its window lies wholly after the last pick. A move needs two readings in a row on the same new
 *    scale note.
 *
 * Every pitch is snapped to the nearest scale note (music::NearestScaleNote), with kMarginSemitones
 * of hysteresis, rather than to the nearest semitone, so a note 40 cents sharp does not flip.
 *
 * A pick whose note is not confirmed within the timeout ends its wait with the note unchanged
 * (TimedOut). Chords and palm-muted chugs often have no pitch to find. The estimate carries on to
 * the end of its window, and a later confirmation is reported as a move.
 *
 * Push() reports what it decides through a callback, on the sample it is decided. Prepare()
 * allocates; nothing else does, and nothing locks.
 */
class ScaleNoteFollower
{
  public:
    struct Event
    {
        enum class Type : std::uint8_t
        {
            Pick,      ///< a pick was heard; its note is not known yet
            Confirmed, ///< the pick's note is known: `note`, `samplesSincePick` after it
            TimedOut,  ///< the pick's note was not found in time; `note` is unchanged
            Moved      ///< the note changed without a pick, or was confirmed late
        };

        Type type = Type::Pick;
        int note = -1; ///< a MIDI note number in the scale, or -1 before the first note
        int samplesSincePick = 0;
    };

    static constexpr double kMarginSemitones = 0.3;
    static constexpr double kEarlyReachSemitones = 19.0;
    static constexpr double kDefaultTimeoutSeconds = 0.035;

    void Prepare(double sampleRate)
    {
        mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        mDetector.Prepare(mSampleRate);
        mTracker.Prepare(mSampleRate);
        mOnset.Prepare(mSampleRate, mLowestHz);
        mTracker.SetLowestFrequency(mLowestHz);
        SetTimeoutSeconds(mTimeoutSeconds);
        UpdateTrackerSettle();
        Reset();
    }

    void Reset()
    {
        mDetector.Reset();
        mTracker.Reset();
        mOnset.Stop();
        mNote = -1;
        mPending = false;
        mSincePick = 1 << 30;
        mCandidate = -1;
        mAgree = 0;
        mMoveCandidate = -1;
        mMoveAgree = 0;
        mDetectionsSeen = mTracker.DetectionCount();
    }

    /// The lowest pitch looked for. A change resets the tracker, and the note with it.
    void SetLowestFrequency(double hz)
    {
        const double lowest =
            IsFinite(hz) ? std::clamp(hz, PitchTracker::kMinHz, PitchTracker::kMaxLowestHz) : PitchTracker::kMinHz;

        if (lowest == mLowestHz)
        {
            return;
        }

        mLowestHz = lowest;
        mTracker.SetLowestFrequency(lowest);
        mOnset.SetLowestFrequency(lowest);
        UpdateTrackerSettle();
        mNote = -1;
        mPending = false;
        mDetectionsSeen = mTracker.DetectionCount();
    }

    /// The key (0-11, C = 0) and scale the notes are snapped to. The note being played is snapped
    /// again at once, so a key change takes effect on it.
    void SetScale(int key, music::Scale scale) noexcept
    {
        if (key == mKey && scale == mScale)
        {
            return;
        }

        mKey = key;
        mScale = scale;

        if (mNote >= 0)
        {
            mNote = music::NearestScaleNote(static_cast<double>(mNote), mKey, mScale);
        }
    }

    void SetTimeoutSeconds(double seconds) noexcept
    {
        mTimeoutSeconds = IsFinite(seconds) ? std::clamp(seconds, 0.005, OnsetPitchEstimator::kActiveSeconds) : 0.035;
        mTimeout = static_cast<int>(std::lround(mTimeoutSeconds * mSampleRate));
    }

    /// One input sample; `sink(const Event&)` is called for each decision made on it.
    template <typename Sink> void Push(float sample, Sink&& sink)
    {
        const float x = IsFinite(sample) ? sample : 0.0f;
        mSincePick = std::min(mSincePick + 1, 1 << 30);

        if (mDetector.Process(x))
        {
            mPending = true;
            mSincePick = 0;
            mCandidate = -1;
            mAgree = 0;
            mMoveCandidate = -1;
            mMoveAgree = 0;
            mOnset.Begin();
            Emit(sink, Event::Type::Pick);
        }

        if (mOnset.Push(x))
        {
            OnOnsetEstimate(sink);
        }

        mTracker.Push(x);

        if (mTracker.DetectionCount() != mDetectionsSeen)
        {
            mDetectionsSeen = mTracker.DetectionCount();
            OnTrackerReading(sink);
        }

        // The first note of all waits as long as the estimate runs, since there is nothing to carry
        // on with; after that the tracker takes over.
        if (mPending && ((mNote >= 0 && mSincePick >= mTimeout) || !mOnset.IsActive()))
        {
            mPending = false;
            Emit(sink, Event::Type::TimedOut);
        }
    }

    /// The scale note being played, or -1 before the first.
    [[nodiscard]] int Note() const noexcept
    {
        return mNote;
    }

    /// A pick has been heard and its note is still being waited for.
    [[nodiscard]] bool IsPending() const noexcept
    {
        return mPending;
    }

    [[nodiscard]] int SamplesSincePick() const noexcept
    {
        return mSincePick;
    }

  private:
    [[nodiscard]] static double MidiFromHz(double hz) noexcept
    {
        return 69.0 + 12.0 * std::log2(hz / 440.0);
    }

    void UpdateTrackerSettle() noexcept
    {
        // The tracker's newest half-window and the lag behind it, plus a detection's hop.
        mTrackerSettle = static_cast<int>(std::lround((2.0 * mTracker.HalfWindowSeconds() + 0.005) * mSampleRate));
    }

    template <typename Sink> void Emit(Sink& sink, typename Event::Type type)
    {
        Event event;
        event.type = type;
        event.note = mNote;
        event.samplesSincePick = mSincePick;
        sink(static_cast<const Event&>(event));
    }

    template <typename Sink> void OnOnsetEstimate(Sink& sink)
    {
        const double hz = mOnset.Hz();

        if (!(hz > 0.0))
        {
            return;
        }

        const double pitch = MidiFromHz(hz);

        // Before it has ruled out a lower fundamental, an estimate is only believed near the note
        // before; the first note of all waits for a complete one.
        if (!mOnset.Complete() && (mNote < 0 || std::abs(pitch - static_cast<double>(mNote)) > kEarlyReachSemitones))
        {
            return;
        }

        const int note = music::NearestScaleNote(pitch, mKey, mScale);
        mAgree = note == mCandidate ? mAgree + 1 : 1;
        mCandidate = note;

        if (mAgree < 2)
        {
            return;
        }

        mOnset.Stop();
        const bool late = !mPending;
        const bool changed = note != mNote;
        mNote = note;
        mPending = false;

        if (!late)
        {
            Emit(sink, Event::Type::Confirmed);
        }
        else if (changed)
        {
            Emit(sink, Event::Type::Moved);
        }
    }

    template <typename Sink> void OnTrackerReading(Sink& sink)
    {
        // Until its window lies wholly after the pick, the tracker is still hearing the note before.
        if (mPending || mSincePick < mTrackerSettle || !mTracker.HasPitch() || !(mTracker.RawFrequencyHz() > 0.0))
        {
            return;
        }

        const double pitch = MidiFromHz(mTracker.RawFrequencyHz());
        const int note = music::FollowScaleNote(mNote, pitch, mKey, mScale, kMarginSemitones);

        if (note == mNote)
        {
            mMoveCandidate = -1;
            mMoveAgree = 0;
            return;
        }

        mMoveAgree = note == mMoveCandidate ? mMoveAgree + 1 : 1;
        mMoveCandidate = note;

        if (mMoveAgree >= 2)
        {
            mNote = note;
            mMoveCandidate = -1;
            mMoveAgree = 0;
            mOnset.Stop();
            Emit(sink, Event::Type::Moved);
        }
    }

    double mSampleRate = 48000.0;
    double mLowestHz = 70.0;
    double mTimeoutSeconds = kDefaultTimeoutSeconds;
    int mTimeout = 1680;
    int mTrackerSettle = 1600;
    int mKey = 0;
    music::Scale mScale = music::Scale::Major;

    PickAttackDetector mDetector;
    PitchTracker mTracker;
    OnsetPitchEstimator mOnset;
    std::uint64_t mDetectionsSeen = 0;

    int mNote = -1;
    bool mPending = false;
    int mSincePick = 1 << 30;
    int mCandidate = -1;
    int mAgree = 0;
    int mMoveCandidate = -1;
    int mMoveAgree = 0;
};
} // namespace guitarfx
