#pragma once

#include "dsp/BiquadDesign.h"
#include "dsp/FiniteCheck.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

// Keeps the YIN difference loop a function of its own. Inlined into Detect()'s lag loop, MSVC
// stopped vectorising it and a detection cost five times as much (measured: 11 us against 2.2 us
// per 64-sample block of noise at 48 kHz); called, it is vectorised on every compiler.
#if defined(_MSC_VER)
    #define GUITARFX_PITCH_TRACKER_NOINLINE __declspec(noinline)
#else
    #define GUITARFX_PITCH_TRACKER_NOINLINE __attribute__((noinline))
#endif

namespace guitarfx
{
namespace pitch_tracker
{
/// The Lowest Note choices an effect offers for SetLowestFrequency(), as an enum parameter's labels.
inline constexpr const char* kLowestNoteLabels[] = {"E2 (standard)", "D2 (drop D)", "B1 (7-string)", "F#1 (8-string)"};
/// A little under each string, so a flat guitar or a bend down still tracks.
inline constexpr double kLowestNoteHz[] = {78.0, 70.0, 58.0, 45.0};
static_assert(std::size(kLowestNoteHz) == std::size(kLowestNoteLabels));

/// The frequency for a Lowest Note choice, held to the list.
[[nodiscard]] inline double LowestNoteHz(int choice) noexcept
{
    return kLowestNoteHz[static_cast<std::size_t>(
        std::clamp(choice, 0, static_cast<int>(std::size(kLowestNoteHz)) - 1))];
}
} // namespace pitch_tracker

/**
 * Real-time monophonic pitch tracker, for guitar: YIN on a decimated copy of the input, refined
 * at the full sample rate.
 *
 * A guitar's fundamental lies between about 45 Hz (a low-tuned seven- or eight-string) and
 * 1.5 kHz (the 24th fret of the high E), and its natural harmonics reach 2 kHz (the high E's
 * third- and fourth-fret harmonics), so the tracker covers 45 Hz to 2 kHz. It low-passes the
 * input at 2 kHz and keeps every Dth sample, for a rate near 12 kHz. YIN (de Cheveigne and
 * Kawahara, 2002) then works on a lag range and window a quarter as long as at 48 kHz, a
 * sixteenth of the multiply-adds, and the low-pass has already removed the upper harmonics that
 * pull YIN an octave high. The decimated lag is refined by evaluating the full-rate difference
 * function at the few lags around it and interpolating, which puts a steady tone within a cent
 * across the range.
 *
 * - A detection runs every 5 ms, over the newest 2 x the longest period (about 45 ms). The
 *   difference function is anchored at the newest sample, so a high note is judged on the most
 *   recent audio. The lag search stops just past the first dip, so it costs least on high notes.
 * - A lag is accepted at the first dip of the normalised difference function below 0.2, the
 *   usual YIN rule, which favours the fundamental over its subharmonics. A first dip above
 *   2 kHz finds nothing, rather than its subharmonic an octave down. A first dip that is shallow
 *   gives way to a markedly deeper one at twice its lag: the note an octave below, whose second
 *   harmonic outweighs its fundamental.
 * - An estimate within a semitone of the current pitch is taken straight away, so bends and
 *   vibrato are followed. A bigger jump, including a first note, must be seen on two successive
 *   detections before it is accepted, which throws out a single stray octave.
 * - Below -55 dBFS, or when nothing periodic is found, a pitch is held. So it is while a note is
 *   stopping: when the newest quarter of the window has under half the power of the same span a
 *   whole number of periods earlier. For a steady note the two spans hold the same part of the
 *   waveform, so they match whatever its shape, even where the quarter is shorter than a period
 *   (below about 90 Hz).
 * - The pitch held is the newest estimate from before the note began to stop. Even half a
 *   millisecond of silence at the end of the window puts YIN several cents out on a low note, far
 *   too little to see in the level, so when a detection finds no pitch the newest estimate is
 *   dropped, and so is any made on a falling level or within a half-window of the last detection
 *   that found none: at most three back, and never past a note change.
 *
 * Both histories are mirrored rings: every sample is written twice, a ring's length apart, so
 * the newest samples are always one contiguous run and the difference loops vectorise.
 *
 * SetLowestFrequency() moves the bottom of the range. The window is two of the longest period
 * the tracker looks for, so an instrument that never goes below a standard guitar's low E is
 * tracked over a window 58% as long and picks a new note up that much sooner. It is opt-in: left
 * alone, the tracker covers 45 Hz up as it always has.
 *
 * Below kMinHz, down to kBassLowestHz for a bass's low strings (the tuner), the window grows to
 * as much as 125 ms, which follows fast playing worse and costs several times as much. So that
 * nothing else pays for it, Prepare() sizes the buffers for kMinHz, or for a lower frequency only
 * if one was set before it, and SetLowestFrequency() after Prepare() goes no lower than that.
 *
 * Prepare() allocates; Process(), Push() and SetLowestFrequency() do not, and none of them locks.
 */
class PitchTracker
{
  public:
    /// The bottom of the range unless SetLowestFrequency() moves it.
    static constexpr double kMinHz = 45.0;
    static constexpr double kMaxHz = 2000.0;
    /// The lowest SetLowestFrequency() goes, set before Prepare(): a little under C0 (16.4 Hz), so
    /// an extended-range bass's low C# (17.3 Hz) still reads a semitone flat. A four-string's low
    /// E is 41.2 Hz, a five- or six-string's low B 30.9 Hz and a seven-string's low F# 23.1 Hz.
    static constexpr double kBassLowestHz = 16.0;
    /// The highest SetLowestFrequency() goes: an octave and a half above a guitar's top string, and
    /// still two octaves below kMaxHz, so the lag range never closes up.
    static constexpr double kMaxLowestHz = 500.0;

    void Prepare(double sampleRate)
    {
        mSampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
        mDecimation = std::max(1, static_cast<int>(std::lround(mSampleRate / kDecimatedRate)));
        mDecimatedRate = mSampleRate / mDecimation;
        mMinLag = std::max(2, static_cast<int>(std::floor(mDecimatedRate / kMaxHz)));
        mHop = std::max(1, static_cast<int>(std::lround(kHopSeconds * mDecimatedRate)));
        mSearchHop = std::max(1, static_cast<int>(std::lround(kSearchHopSeconds * mDecimatedRate)));
        mWeakSpan = std::max(1, static_cast<int>(std::lround(kWeakSeconds * mDecimatedRate)));
        mRefineReach = std::min(mDecimation / 2 + 2, (kMaxRefineLags - 3) / 2);

        // Sized for the full guitar range whatever the lowest frequency is now, so that narrowing
        // or widening it later never allocates; lower only when the lowest frequency already is.
        mPreparedLowestHz = std::min(kMinHz, mLowestHz);
        const int longestLag = LongestLag(mPreparedLowestHz);
        mDecimated.Assign(2 * longestLag + 2);
        mFull.Assign(longestLag * mDecimation + (longestLag + 2) * mDecimation + mRefineReach + 2);
        mNormalised.assign(static_cast<std::size_t>(longestLag + 2), 1.0f);
        mShortNormalised.assign(static_cast<std::size_t>(longestLag + 2), 1.0f);

        mLowPass[0] = biquad::LowPass(kLowPassHz, biquad::ButterworthSectionQ(4, 0), mSampleRate);
        mLowPass[1] = biquad::LowPass(kLowPassHz, biquad::ButterworthSectionQ(4, 1), mSampleRate);
        ApplyLowestFrequency();
        Reset();
    }

    /// Tracks pitches from `hz` up rather than from kMinHz, over a window sized to match. A tone
    /// below it reads as no pitch, or as its octave. A change resets the tracker; setting the
    /// value it already has does nothing. Below kMinHz it must be set before Prepare(): after,
    /// it goes no lower than what Prepare() sized the buffers for.
    void SetLowestFrequency(double hz)
    {
        const double bottom = mNormalised.empty() ? kBassLowestHz : mPreparedLowestHz;
        const double lowest = IsFinite(hz) ? std::clamp(hz, bottom, kMaxLowestHz) : kMinHz;

        if (lowest == mLowestHz)
        {
            return;
        }

        mLowestHz = lowest;

        if (!mNormalised.empty())
        {
            ApplyLowestFrequency();
            Reset();
        }
    }

    [[nodiscard]] double LowestFrequencyHz() const
    {
        return mLowestHz;
    }

    /// Reads a new note sooner, for a caller that plays notes rather than measures pitch. Three
    /// things, none of which touches how a note already being followed is read:
    ///
    /// - A detection's window is twice the longest period tracked, and until a new note fills the
    ///   newer half of it and one of its own periods more, the window reads the note before, or
    ///   nothing. Each detection also looks at the newest quarter of that half alone, and a clean
    ///   pitch found there that is not the one being tracked is taken as the estimate
    ///   (DetectNewNote).
    /// - While no pitch is being followed (the last detection found none, or found one that is
    ///   still to be confirmed) a detection runs every kSearchHopSeconds, not every 5 ms.
    /// - A hard-picked, palm-muted note whose dip never passes the threshold is taken once its
    ///   deepest dip has held its lag (FindSteadyWeakDip).
    ///
    /// A new pitch still has to be seen again 5 ms after it was first seen. On the GuitarSet solos
    /// (12,000 single notes of real playing) Guitar to MIDI's note starts 13.4 ms after the string
    /// sounds at the median where it took 17.4, and 41 ms into the hard low demo riffs where it
    /// took 53; Synth Voice is on a new note's pitch within 13.6 ms nine times in ten where it
    /// took 19.9. The early readings are good to a third of a semitone, not to a cent
    /// (IsRoughEstimate), so one is never the pitch held, and the long window takes over as soon
    /// as it reads the note. Off by default: a tuner wants the long window's reading only. Costs
    /// about half as much again on notes, and up to four times as much on noise or chords, where
    /// it is always searching. Does not allocate or reset.
    void SetFastNoteChanges(bool on)
    {
        mFastNoteChanges = on;
    }

    /// How much of the newest audio a detection judges, in seconds: the longest period tracked.
    /// A note is only read reliably once it fills most of this.
    [[nodiscard]] double HalfWindowSeconds() const
    {
        return mDecimatedRate > 0.0 ? mHalf / mDecimatedRate : 0.0;
    }

    void Reset()
    {
        mDecimated.Clear();
        mFull.Clear();
        mFilter[0].Reset();
        mFilter[1].Reset();
        mDecimationCount = 0;
        mHopCount = 0;
        mFrequency = 0.0;
        mRawFrequency = 0.0;
        mConfidence = 0.0f;
        mDetected = false;
        mCandidate = 0.0;
        mCandidateHits = 0;
        mDetections = 0;
        mKeptCount = 0;
        mSinceHold = mHalf;
        mSearching = false;
        mRough = false;
        mClock = 0;
        mCandidateSince = 0;
        mWeakFrequency = 0.0;
        mWeakSince = 0;
    }

    void Process(const float* input, int numSamples)
    {
        if (!input)
        {
            return;
        }

        for (int i = 0; i < numSamples; ++i)
        {
            Push(input[i]);
        }
    }

    void Push(float sample)
    {
        if (mNormalised.empty())
        {
            return;
        }

        const double low = mFilter[1].Process(mLowPass[1], mFilter[0].Process(mLowPass[0], sample));
        const auto y = static_cast<float>(low);
        mFull.Push(y);

        if (++mDecimationCount < mDecimation)
        {
            return;
        }

        mDecimationCount = 0;
        mDecimated.Push(y);
        ++mClock;

        if (++mHopCount >= ((mFastNoteChanges && mSearching) ? mSearchHop : mHop))
        {
            const int elapsed = mHopCount;
            mHopCount = 0;
            Detect(elapsed);
        }
    }

    /// The pitch being tracked, in Hz: the latest estimate accepted, or while detections find none,
    /// the newest one made before the note began to stop. 0 until the first note has been accepted.
    [[nodiscard]] double FrequencyHz() const
    {
        return mFrequency;
    }

    /// Whether the latest detection found the pitch (rather than holding the last one).
    [[nodiscard]] bool HasPitch() const
    {
        return mDetected;
    }

    /// Whether the latest detection's estimate is an early one: made within a half-window of a
    /// detection that found no pitch, so over a window the note does not yet fill, or one of
    /// SetFastNoteChanges()' readings off the short window or a weak dip that held its lag. It is
    /// the right note, but read through the pick, the silence before it or whatever else is
    /// sounding it can be half a semitone out, where a settled one is within a few cents.
    [[nodiscard]] bool IsRoughEstimate() const
    {
        return mDetected && mRough;
    }

    /// The latest detection's own estimate, before the note-change check; 0 when it found none.
    [[nodiscard]] double RawFrequencyHz() const
    {
        return mRawFrequency;
    }

    /// 1 minus the normalised difference at the chosen lag: near 1 for a clean tone.
    [[nodiscard]] float Confidence() const
    {
        return mConfidence;
    }

    /// Detections run so far, so a caller can tell when a new estimate has arrived.
    [[nodiscard]] std::uint64_t DetectionCount() const
    {
        return mDetections;
    }

  private:
    static constexpr double kDecimatedRate = 12000.0;
    static constexpr double kLowPassHz = 2000.0;
    static constexpr double kHopSeconds = 0.005;
    /// SetFastNoteChanges(): how often a detection runs while no pitch is being followed (a note
    /// just picked, or one not yet confirmed). A new note's first reading then waits 0.6 ms for a
    /// detection on average, not 2.5.
    static constexpr double kSearchHopSeconds = 0.00125;
    /// SetFastNoteChanges(): a deepest dip under this that has held its lag for kWeakSeconds is a
    /// note, though it never passed kDipThreshold (FindSteadyWeakDip).
    static constexpr float kWeakDipThreshold = 0.45f;
    static constexpr double kWeakSeconds = 0.01;
    static constexpr float kDipThreshold = 0.2f;
    /// A first dip shallower than this is passed over for one at twice its lag that is deeper by
    /// as much: the note an octave below, whose second harmonic made the first dip.
    static constexpr float kOctaveMargin = 0.05f;
    /// SetFastNoteChanges(): the short window, as a share of the half-window, and the dip it needs.
    /// A quarter of a cycle of a low note can pass for periodic by chance, and a palm-muted chug
    /// does at 0.1, so the dip has to be far cleaner than the long window's.
    static constexpr double kShortWindowFraction = 0.25;
    static constexpr float kShortDipThreshold = 0.07f;
    static constexpr float kGateRms = 0.0017783f; ///< -55 dBFS
    /// The newest quarter of the window below this fraction of the power of the same span a whole
    /// number of periods earlier means a note stopping.
    static constexpr float kStoppingPowerRatio = 0.5f;
    /// An estimate made with that ratio below this was made on a falling level, and is not held.
    static constexpr float kSteadyPowerRatio = 0.9f;
    /// The accepted estimates kept for the held pitch to go back through.
    static constexpr int kKeptEstimates = 4;
    static constexpr double kContinuousSemitones = 1.0;
    static constexpr double kCandidateSemitones = 0.5;
    static constexpr int kConfirmations = 2;
    /// Enough for the refinement's full reach up to 192 kHz (a decimation of 16); beyond that the
    /// reach is capped, still several full-rate lags either side of the interpolated estimate.
    static constexpr int kMaxRefineLags = 24;
    static_assert(2 * (16 / 2 + 2) + 3 <= kMaxRefineLags);

    /// A ring whose newest `length` samples, up to its size, are always contiguous.
    struct MirroredRing
    {
        std::vector<float> data;
        int size = 0;
        int position = 0;

        void Assign(int length)
        {
            size = std::max(1, length);
            data.assign(static_cast<std::size_t>(2 * size), 0.0f);
            position = 0;
        }

        void Clear()
        {
            std::fill(data.begin(), data.end(), 0.0f);
            position = 0;
        }

        void Push(float value)
        {
            position = (position + 1 == size) ? 0 : position + 1;
            data[static_cast<std::size_t>(position)] = value;
            data[static_cast<std::size_t>(position + size)] = value;
        }

        /// The oldest of the newest `length` samples; the newest is at [length - 1].
        [[nodiscard]] const float* Newest(int length) const
        {
            return data.data() + position + size + 1 - length;
        }
    };

    [[nodiscard]] static double Semitones(double from, double to)
    {
        return 12.0 * std::log2(to / from);
    }

    /// The longest lag, in decimated samples, the lag search needs to reach a period of `hz`.
    [[nodiscard]] int LongestLag(double hz) const
    {
        return static_cast<int>(std::ceil(mDecimatedRate / hz)) + 1;
    }

    /// The lag range and window for mLowestHz, within what Prepare() allocated.
    void ApplyLowestFrequency()
    {
        mMaxLag = LongestLag(mLowestHz);
        mHalf = mMaxLag;
        mWindowSize = mHalf + mMaxLag + 2;
        mShortHalf = std::max(8, static_cast<int>(std::lround(kShortWindowFraction * mHalf)));
    }

    /// The sum of squared differences between `count` samples at `a` and at `b`.
    [[nodiscard]] GUITARFX_PITCH_TRACKER_NOINLINE static float SquaredDifference(const float* a, const float* b,
                                                                                 int count)
    {
        float sum = 0.0f;

        for (int j = 0; j < count; ++j)
        {
            const float delta = a[j] - b[j];
            sum += delta * delta;
        }

        return sum;
    }

    [[nodiscard]] static float Energy(const float* x, int count)
    {
        float sum = 0.0f;

        for (int j = 0; j < count; ++j)
        {
            sum += x[j] * x[j];
        }

        return sum;
    }

    [[nodiscard]] static double Dot(const float* a, const float* b, int count)
    {
        double sum = 0.0;

        for (int j = 0; j < count; ++j)
        {
            sum += static_cast<double>(a[j]) * static_cast<double>(b[j]);
        }

        return sum;
    }

    /// Where the minimum of a parabola through three equally spaced points sits, relative to the
    /// middle one, in [-1, 1].
    [[nodiscard]] static double ParabolicOffset(double left, double middle, double right)
    {
        const double curvature = left - 2.0 * middle + right;

        if (!(std::abs(curvature) > 1.0e-12))
        {
            return 0.0;
        }

        return std::clamp(0.5 * (left - right) / curvature, -1.0, 1.0);
    }

    /// One detection, `elapsed` decimated samples after the one before.
    void Detect(int elapsed);

    /// SetFastNoteChanges(): no dip passed the threshold, but the deepest one is under
    /// kWeakDipThreshold and has sat within half a semitone of the same lag for kWeakSeconds.
    /// That is a hard-picked, palm-muted low note: a pulse at the right period from the first
    /// cycle, each one duller and quieter than the last, so that for some 45 ms only half of it
    /// repeats and no lag gets under 0.2, though the deepest dip is the note's all along. A scrape
    /// or a chord's deepest dip wanders. Sets `lag` and returns true once it has held.
    [[nodiscard]] bool FindSteadyWeakDip(const float* normalised, int& lag);

    /// How deep the dip at `lag` goes between the lags: the minimum of the parabola through it and
    /// its neighbours. A high note's period is only six to eight decimated samples, and its dip
    /// read at the nearest whole lag is well short of the bottom.
    [[nodiscard]] static float DipDepth(const float* normalised, int lag)
    {
        const double left = normalised[lag - 1];
        const double middle = normalised[lag];
        const double right = normalised[lag + 1];
        return static_cast<float>(middle - 0.25 * (left - right) * ParabolicOffset(left, middle, right));
    }

    /// The lag an octave below `lag` (twice it, give or take one) when the signal repeats better
    /// there by more than kOctaveMargin; `lag` otherwise. `normalised` must reach 2 * lag + 2.
    [[nodiscard]] static int OctaveBelow(const float* normalised, int lag)
    {
        int twice = 2 * lag - 1;

        for (int tau = 2 * lag; tau <= 2 * lag + 1; ++tau)
        {
            if (normalised[tau] < normalised[twice])
            {
                twice = tau;
            }
        }

        return DipDepth(normalised, lag) - DipDepth(normalised, twice) > kOctaveMargin ? twice : lag;
    }

    /// SetFastNoteChanges(): YIN again over only the newest quarter of the half-window, across the
    /// whole lag range, looking for a note the long window cannot read yet. Takes the detection
    /// (true) when it finds a pitch over a semitone from the one being tracked that the long
    /// window either did not find or disagrees with; `fullLag` is the long window's lag, 0 for
    /// none. Where the newest audio still repeats at the long window's period the old note is
    /// still sounding, and the long window, which reads it better, keeps the detection.
    [[nodiscard]] bool DetectNewNote(const float* end, double fullLag);

    /// The power of the newest quarter of the half-window that ends at `end`, over that of the
    /// same span the fewest whole periods of `period` (in decimated samples) earlier that clear it.
    /// For a steady tone the two hold the same part of the waveform, so this is near 1 whatever
    /// its shape; it falls as a note stops.
    [[nodiscard]] float LevelChange(const float* end, double period) const;

    /// A detection found no pitch, so the note may have stopped: go back to the newest estimate
    /// that can be trusted. The newest accepted one may already have had the start of the silence
    /// at the end of its window, so it is dropped, and so is any made on a falling level, back to
    /// the oldest kept. So is any made within a half-window of a detection that found none: its
    /// window may still hold that silence, and once the newest quarter and the one it is compared
    /// with are both past the note, the level no longer shows it. A note change clears what is
    /// kept, so this never goes back to a previous note.
    void Hold()
    {
        mSinceHold = 0;
        mSearching = true;

        // Detecting every kSearchHopSeconds, two estimates either side of one that found nothing
        // are no longer two in a row: the candidate starts again.
        if (mFastNoteChanges)
        {
            mCandidateHits = 0;
        }

        if (mKeptCount < 2)
        {
            return;
        }

        --mKeptCount;

        while (mKeptCount > 1 && mKept[static_cast<std::size_t>(mKeptCount - 1)].level < kSteadyPowerRatio)
        {
            --mKeptCount;
        }

        mKept[0] = mKept[static_cast<std::size_t>(mKeptCount - 1)];
        mKeptCount = 1;
        mFrequency = mKept[0].frequency;
    }

    /// Keeps an accepted estimate for Hold(), dropping the oldest when full.
    void Keep(double frequency, float level)
    {
        if (mKeptCount == kKeptEstimates)
        {
            std::copy(mKept.begin() + 1, mKept.end(), mKept.begin());
            --mKeptCount;
        }

        mKept[static_cast<std::size_t>(mKeptCount)] = {frequency, level};
        ++mKeptCount;
    }

    /// The full-rate lag with the highest normalised cross-correlation within about a decimated
    /// sample of `estimate`, interpolated, over the newest `span` full-rate samples: the stretch of
    /// audio the decimated window that made the estimate judged.
    ///
    /// Normalised correlation ignores a level difference between the two copies being compared.
    /// A decaying note's older copy is louder than its newer one, and a squared difference then
    /// grows with the lag and pulls its minimum early: 10 cents sharp at 82 Hz with a 10 ms
    /// release, which is the estimate that gets held when the note stops. For an exponential
    /// decay the correlation peaks exactly at the period. It is accumulated in double, since
    /// near a long period neighbouring lags differ by parts in 10^5.
    [[nodiscard]] double RefinePeriod(double estimate, int span) const;

    /// The note-change check: small moves are followed, big ones must repeat. `level` is
    /// LevelChange() for the estimate, or 0 if it is not to be held.
    void Accept(double frequency, float level)
    {
        if (mFrequency > 0.0 && std::abs(Semitones(mFrequency, frequency)) <= kContinuousSemitones)
        {
            mFrequency = frequency;
            mCandidateHits = 0;
            mSearching = false;
            Keep(frequency, level);
            return;
        }

        mSearching = true;

        if (mCandidateHits > 0 && std::abs(Semitones(mCandidate, frequency)) <= kCandidateSemitones)
        {
            ++mCandidateHits;
        }
        else
        {
            mCandidateHits = 1;
            mCandidateSince = mClock;
        }

        mCandidate = frequency;

        // Seen again, and a whole hop after it was first seen: at the usual hop that is the next
        // detection, as it always was. While searching, detections come four times as often and
        // their windows mostly overlap, so two in a row say little more than one; the wait is
        // what makes the second look a look at new audio.
        if (mCandidateHits >= kConfirmations && mClock - mCandidateSince >= mHop)
        {
            mSearching = false;
            mFrequency = frequency;
            mCandidateHits = 0;
            mKeptCount = 0;
            Keep(frequency, level);
        }
    }

    double mSampleRate = 48000.0;
    int mDecimation = 4;
    double mDecimatedRate = 12000.0;
    double mLowestHz = kMinHz;
    double mPreparedLowestHz = kMinHz; ///< the lowest frequency the buffers are sized for
    int mMaxLag = 0;
    int mMinLag = 2;
    int mHalf = 0;
    int mWindowSize = 0;
    int mHop = 60;
    int mRefineReach = 3;

    MirroredRing mDecimated;
    MirroredRing mFull;
    std::vector<float> mNormalised;      ///< YIN's cumulative-mean normalised difference, by lag
    std::vector<float> mShortNormalised; ///< the same over the short window (SetFastNoteChanges)
    int mShortHalf = 0;
    bool mFastNoteChanges = false;

    BiquadCoefficients mLowPass[2];
    biquad::State mFilter[2];
    int mDecimationCount = 0;
    int mHopCount = 0;

    double mFrequency = 0.0;
    double mRawFrequency = 0.0;
    float mConfidence = 0.0f;
    bool mDetected = false;
    double mCandidate = 0.0;
    int mCandidateHits = 0;
    std::uint64_t mDetections = 0;

    struct KeptEstimate
    {
        double frequency = 0.0;
        float level = 0.0f; ///< LevelChange() when it was made, or 0 if it is not to be held
    };

    std::array<KeptEstimate, kKeptEstimates> mKept{}; ///< the current note's latest, oldest first
    int mKeptCount = 0;
    int mSinceHold = 0; ///< decimated samples since a detection found no pitch, up to a half-window

    int mSearchHop = 15;
    int mWeakSpan = 120;
    bool mSearching = false;          ///< the last detection did not follow the accepted pitch
    bool mRough = false;              ///< the latest estimate is an early one (IsRoughEstimate)
    std::int64_t mClock = 0;          ///< decimated samples since Reset()
    std::int64_t mCandidateSince = 0; ///< when the candidate was first seen
    double mWeakFrequency = 0.0;      ///< the deepest dip's pitch while none passes; 0 for none
    std::int64_t mWeakSince = 0;
};
} // namespace guitarfx
