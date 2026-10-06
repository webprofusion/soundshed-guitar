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
 *   2 kHz finds nothing, rather than its subharmonic an octave down.
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
 * SetLowestFrequency() narrows the range from the bottom. The window is two of the longest period
 * the tracker looks for, so an instrument that never goes below a standard guitar's low E is
 * tracked over a window 58% as long and picks a new note up that much sooner. It is opt-in: left
 * alone, the tracker covers 45 Hz up as it always has.
 *
 * Prepare() allocates; Process(), Push() and SetLowestFrequency() do not, and none of them locks.
 */
class PitchTracker
{
  public:
    static constexpr double kMinHz = 45.0;
    static constexpr double kMaxHz = 2000.0;
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
        mRefineReach = std::min(mDecimation / 2 + 2, (kMaxRefineLags - 3) / 2);

        // Sized for the full range whatever the lowest frequency is now, so that narrowing or
        // widening it later never allocates.
        const int longestLag = LongestLag(kMinHz);
        mDecimated.Assign(2 * longestLag + 2);
        mFull.Assign(longestLag * mDecimation + (longestLag + 2) * mDecimation + mRefineReach + 2);
        mNormalised.assign(static_cast<std::size_t>(longestLag + 2), 1.0f);

        mLowPass[0] = biquad::LowPass(kLowPassHz, biquad::ButterworthSectionQ(4, 0), mSampleRate);
        mLowPass[1] = biquad::LowPass(kLowPassHz, biquad::ButterworthSectionQ(4, 1), mSampleRate);
        ApplyLowestFrequency();
        Reset();
    }

    /// Tracks pitches from `hz` up rather than from kMinHz, over a window sized to match. A tone
    /// below it reads as no pitch, or as its octave. A change resets the tracker; setting the
    /// value it already has does nothing.
    void SetLowestFrequency(double hz)
    {
        const double lowest = IsFinite(hz) ? std::clamp(hz, kMinHz, kMaxLowestHz) : kMinHz;

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

        if (++mHopCount >= mHop)
        {
            mHopCount = 0;
            Detect();
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
    static constexpr float kDipThreshold = 0.2f;
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

    void Detect()
    {
        ++mDetections;
        mDetected = false;
        mRawFrequency = 0.0;
        mSinceHold = std::min(mSinceHold + mHop, mHalf);

        // A non-finite input leaves the low-pass state poisoned; start again rather than hold it.
        if (!IsFinite(mFilter[0].s1) || !IsFinite(mFilter[0].s2) || !IsFinite(mFilter[1].s1) ||
            !IsFinite(mFilter[1].s2))
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

        if (lag == 0)
        {
            mConfidence = 0.0f;
            Hold();
            return;
        }

        mConfidence = 1.0f - normalised[lag];
        const double coarse = lag + ParabolicOffset(normalised[lag - 1], normalised[lag], normalised[lag + 1]);

        // A note that has just stopped leaves the newest part of the window near silent, and YIN
        // run over that reads several cents out, so skip it. Against the window's mean power, as
        // it was once judged, a quarter shorter than a period (below about 90 Hz) read as stopping
        // or not by where in the cycle it fell: a steady low sawtooth or pluck lost up to a
        // quarter of its detections.
        const float level = LevelChange(recent + half, coarse);

        if (!(level >= kStoppingPowerRatio))
        {
            mConfidence = 0.0f;
            Hold();
            return;
        }

        // Step 4: interpolate the decimated lag, then refine it at the full rate.
        const double frequency = mSampleRate / RefinePeriod(coarse * mDecimation);

        if (!(frequency >= mLowestHz && frequency <= kMaxHz))
        {
            Hold();
            return;
        }

        mRawFrequency = frequency;
        mDetected = true;
        Accept(frequency, mSinceHold < mHalf ? 0.0f : level);
    }

    /// The power of the newest quarter of the half-window that ends at `end`, over that of the
    /// same span the fewest whole periods of `period` (in decimated samples) earlier that clear it.
    /// For a steady tone the two hold the same part of the waveform, so this is near 1 whatever
    /// its shape; it falls as a note stops.
    [[nodiscard]] float LevelChange(const float* end, double period) const
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
    /// sample of `estimate`, interpolated. The span is the decimated window's, so both judge the
    /// same stretch of audio.
    ///
    /// Normalised correlation ignores a level difference between the two copies being compared.
    /// A decaying note's older copy is louder than its newer one, and a squared difference then
    /// grows with the lag and pulls its minimum early: 10 cents sharp at 82 Hz with a 10 ms
    /// release, which is the estimate that gets held when the note stops. For an exponential
    /// decay the correlation peaks exactly at the period. It is accumulated in double, since
    /// near a long period neighbouring lags differ by parts in 10^5.
    [[nodiscard]] double RefinePeriod(double estimate) const
    {
        const int span = mHalf * mDecimation;
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

    /// The note-change check: small moves are followed, big ones must repeat. `level` is
    /// LevelChange() for the estimate, or 0 if it is not to be held.
    void Accept(double frequency, float level)
    {
        if (mFrequency > 0.0 && std::abs(Semitones(mFrequency, frequency)) <= kContinuousSemitones)
        {
            mFrequency = frequency;
            mCandidateHits = 0;
            Keep(frequency, level);
            return;
        }

        if (mCandidateHits > 0 && std::abs(Semitones(mCandidate, frequency)) <= kCandidateSemitones)
        {
            ++mCandidateHits;
        }
        else
        {
            mCandidateHits = 1;
        }

        mCandidate = frequency;

        if (mCandidateHits >= kConfirmations)
        {
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
    int mMaxLag = 0;
    int mMinLag = 2;
    int mHalf = 0;
    int mWindowSize = 0;
    int mHop = 60;
    int mRefineReach = 3;

    MirroredRing mDecimated;
    MirroredRing mFull;
    std::vector<float> mNormalised; ///< YIN's cumulative-mean normalised difference, by lag

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
};
} // namespace guitarfx
