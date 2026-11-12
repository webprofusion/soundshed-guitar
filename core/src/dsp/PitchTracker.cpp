// PitchTracker.cpp - The tracker's detection, run once per hop (every 5 ms) rather than per
// sample: the YIN search on the decimated signal, the short window that follows a new note, the
// steady weak dip, the level change that says a note is stopping, and the full-rate refinement of
// the period. Push(), Process() and the readings stay inline in PitchTracker.h.

#include "dsp/PitchTracker.h"

namespace guitarfx
{
void PitchTracker::Detect(int elapsed)
{
    ++mDetections;
    mDetected = false;
    mRough = false;
    mRawFrequency = 0.0;
    mSinceHold = std::min(mSinceHold + elapsed, mHalf);

    // A non-finite input leaves the low-pass state poisoned; start again rather than hold it.
    if (!IsFinite(mFilter[0].s1) || !IsFinite(mFilter[0].s2) || !IsFinite(mFilter[1].s1) || !IsFinite(mFilter[1].s2))
    {
        Reset();
        return;
    }

    const int half = mHalf;
    const int maxLag = mMaxLag;
    const int minLag = mMinLag;
    const float* window = mDecimated.Newest(mWindowSize);
    const float* recent = window + (mWindowSize - half); // the newest half-window

    const float energy = Energy(recent, half);

    if (!(energy > kGateRms * kGateRms * static_cast<float>(half)))
    {
        mConfidence = 0.0f;
        Hold();
        mSearching = false; // silence: nothing to look for
        return;
    }

    // YIN steps 1 to 3 together: the difference function over the newest half-window, its
    // cumulative-mean normalisation, and the first dip below the threshold followed down to
    // its minimum. The search stops one lag past that, which the interpolation needs.
    float* normalised = mNormalised.data();
    float running = 0.0f;
    int lag = 0;
    normalised[0] = 1.0f;

    for (int tau = 1; tau <= maxLag + 1; ++tau)
    {
        const float difference = SquaredDifference(recent, recent - tau, half);
        running += difference;
        normalised[tau] = running > 0.0f ? difference * static_cast<float>(tau) / running : 1.0f;

        if (lag != 0)
        {
            if (tau <= maxLag && normalised[tau] < normalised[lag])
            {
                lag = tau;
                continue;
            }

            break;
        }

        if (normalised[tau] < kDipThreshold && tau <= maxLag)
        {
            // A first dip above the range is a pitch the tracker cannot follow; the dips after
            // it are that pitch's subharmonics, not the pitch.
            if (tau < minLag)
            {
                mConfidence = 0.0f;
                Hold();
                return;
            }

            lag = tau;
        }
    }

    // A first dip that only just passes may be the second harmonic's, at half the period: a
    // note with a strong octave partial, or one picked while the octave above still rings,
    // nearly repeats there. Where the signal repeats markedly better at twice the lag, that is
    // the note. A clean note's dip is under the margin, so it is never looked at again.
    if (lag != 0 && DipDepth(normalised, lag) > kOctaveMargin && 2 * lag + 1 <= maxLag)
    {
        for (int tau = lag + 2; tau <= 2 * lag + 2; ++tau)
        {
            const float difference = SquaredDifference(recent, recent - tau, half);
            running += difference;
            normalised[tau] = running > 0.0f ? difference * static_cast<float>(tau) / running : 1.0f;
        }

        lag = OctaveBelow(normalised, lag);
    }

    const bool passed = lag != 0;
    const bool weak = !passed && mFastNoteChanges && FindSteadyWeakDip(normalised, lag);

    if (passed)
    {
        mWeakFrequency = 0.0;
    }

    bool found = lag != 0;
    double coarse = 0.0;
    float level = 0.0f;
    float confidence = 0.0f;

    if (found)
    {
        confidence = 1.0f - normalised[lag];
        coarse = lag + ParabolicOffset(normalised[lag - 1], normalised[lag], normalised[lag + 1]);

        // A note that has just stopped leaves the newest part of the window near silent, and
        // YIN run over that reads several cents out, so skip it. Against the window's mean
        // power, as it was once judged, a quarter shorter than a period (below about 90 Hz)
        // read as stopping or not by where in the cycle it fell: a steady low sawtooth or
        // pluck lost up to a quarter of its detections.
        level = LevelChange(recent + half, coarse);
        found = level >= kStoppingPowerRatio;
    }

    // A weak dip is no evidence that the note before is still sounding, so it does not get to
    // overrule the short window as a dip that passed does.
    if (mFastNoteChanges && DetectNewNote(recent + half, (found && !weak) ? coarse : 0.0))
    {
        return;
    }

    if (!found)
    {
        mConfidence = 0.0f;
        Hold();
        return;
    }

    mConfidence = confidence;

    // Step 4: interpolate the decimated lag, then refine it at the full rate.
    const double frequency = mSampleRate / RefinePeriod(coarse * mDecimation, mHalf * mDecimation);

    if (!(frequency >= mLowestHz && frequency <= kMaxHz))
    {
        Hold();
        return;
    }

    mRawFrequency = frequency;
    mDetected = true;
    // Within a half-window of a detection that found nothing, the window is still partly the
    // silence or the scrape before the note.
    mRough = weak || mSinceHold < mHalf;

    if (weak && !(mFrequency > 0.0 && std::abs(Semitones(mFrequency, frequency)) <= kContinuousSemitones))
    {
        // Its lag has already held for kWeakSeconds: that was the second look.
        mCandidate = frequency;
        mCandidateHits = kConfirmations - 1;
        mCandidateSince = mClock - mHop;
    }

    // A weak dip is read through whatever else is in the window: never the pitch to hold.
    Accept(frequency, (weak || mSinceHold < mHalf) ? 0.0f : level);
}

bool PitchTracker::FindSteadyWeakDip(const float* normalised, int& lag)
{
    int best = mMinLag;

    for (int tau = mMinLag + 1; tau <= mMaxLag; ++tau)
    {
        if (normalised[tau] < normalised[best])
        {
            best = tau;
        }
    }

    if (best == mMinLag || best == mMaxLag || !(normalised[best] < kWeakDipThreshold))
    {
        mWeakFrequency = 0.0;
        return false;
    }

    const double frequency =
        mDecimatedRate / (best + ParabolicOffset(normalised[best - 1], normalised[best], normalised[best + 1]));

    if (!(mWeakFrequency > 0.0 && std::abs(Semitones(mWeakFrequency, frequency)) <= kCandidateSemitones))
    {
        mWeakSince = mClock;
    }

    mWeakFrequency = frequency;

    if (mClock - mWeakSince < mWeakSpan)
    {
        return false;
    }

    lag = best;
    return true;
}

bool PitchTracker::DetectNewNote(const float* end, double fullLag)
{
    const int span = mShortHalf;
    const float* recent = end - span;

    if (!(Energy(recent, span) > kGateRms * kGateRms * static_cast<float>(span)))
    {
        return false;
    }

    float* normalised = mShortNormalised.data();
    float running = 0.0f;
    normalised[0] = 1.0f;

    for (int tau = 1; tau <= mMaxLag + 1; ++tau)
    {
        const float difference = SquaredDifference(recent, recent - tau, span);
        running += difference;
        normalised[tau] = running > 0.0f ? difference * static_cast<float>(tau) / running : 1.0f;
    }

    int lag = 0;

    for (int tau = 1; tau <= mMaxLag && lag == 0; ++tau)
    {
        if (normalised[tau] < kShortDipThreshold)
        {
            if (tau < mMinLag)
            {
                return false;
            }

            lag = tau;

            while (lag < mMaxLag && normalised[lag + 1] < normalised[lag])
            {
                ++lag;
            }
        }
    }

    if (lag == 0)
    {
        return false;
    }

    if (2 * lag + 1 <= mMaxLag && DipDepth(normalised, lag) > kOctaveMargin)
    {
        lag = OctaveBelow(normalised, lag);
    }

    const double coarse = lag + ParabolicOffset(normalised[lag - 1], normalised[lag], normalised[lag + 1]);
    const double coarseHz = mDecimatedRate / coarse;

    if (mFrequency > 0.0 && std::abs(Semitones(mFrequency, coarseHz)) <= kContinuousSemitones)
    {
        return false; // the note being tracked: the long window's to read
    }

    if (fullLag > 0.0)
    {
        const int at = std::clamp(static_cast<int>(std::lround(fullLag)), 1, mMaxLag);
        const bool stillThere = std::min({normalised[at - 1], normalised[at], normalised[at + 1]}) < kDipThreshold;

        if (stillThere || std::abs(Semitones(mDecimatedRate / fullLag, coarseHz)) <= kCandidateSemitones)
        {
            return false;
        }
    }

    const double frequency = mSampleRate / RefinePeriod(coarse * mDecimation, span * mDecimation);

    if (!(frequency >= mLowestHz && frequency <= kMaxHz))
    {
        return false;
    }

    mConfidence = 1.0f - normalised[lag];
    mRawFrequency = frequency;
    mDetected = true;
    mRough = true;
    Accept(frequency, 0.0f); // read off a short window: never the pitch to hold
    return true;
}

float PitchTracker::LevelChange(const float* end, double period) const
{
    const int quarter = std::max(1, mHalf / 4);
    const double periods = std::ceil(quarter / period);
    const int shift = std::min(static_cast<int>(std::lround(periods * period)), mWindowSize - quarter);
    const float newest = Energy(end - quarter, quarter);
    const float earlier = Energy(end - quarter - shift, quarter);

    if (!(earlier > 0.0f))
    {
        return newest > 0.0f ? 1.0f : 0.0f;
    }

    return newest / earlier;
}

double PitchTracker::RefinePeriod(double estimate, int span) const
{
    const int longest = (mMaxLag + 1) * mDecimation;
    const int centre = std::clamp(static_cast<int>(std::lround(estimate)), mRefineReach + 1, longest);
    const int first = centre - mRefineReach - 1; // one below the search, for the parabola
    const int count = 2 * mRefineReach + 3;
    const float* history = mFull.Newest(span + first + count);
    const float* recent = history + first + count; // the newest `span` samples
    const double recentEnergy = Dot(recent, recent, span);
    std::array<double, kMaxRefineLags> values{};

    for (int i = 0; i < count; ++i)
    {
        const float* lagged = recent - (first + i);
        const double denominator = std::sqrt(recentEnergy * Dot(lagged, lagged, span));
        values[static_cast<std::size_t>(i)] = denominator > 0.0 ? Dot(recent, lagged, span) / denominator : 0.0;
    }

    int best = 1;

    for (int i = 2; i < count - 1; ++i)
    {
        if (values[static_cast<std::size_t>(i)] > values[static_cast<std::size_t>(best)])
        {
            best = i;
        }
    }

    const double offset =
        ParabolicOffset(-values[static_cast<std::size_t>(best - 1)], -values[static_cast<std::size_t>(best)],
                        -values[static_cast<std::size_t>(best + 1)]);
    return first + best + offset;
}
} // namespace guitarfx
