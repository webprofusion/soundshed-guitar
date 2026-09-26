#pragma once

#include "dsp/FiniteCheck.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace guitarfx
{
/**
 * Low-latency pitch shifter for live control: an expression pedal, a footswitch, automation.
 *
 * It works on the waveform, not the spectrum. The input goes into a delay line, and a read tap
 * moves through it at the pitch ratio `r = 2^(st/12)`: faster than the input arrives to shift up,
 * slower to shift down. A new shift therefore changes the pitch on the very next sample, where an
 * STFT engine such as Signalsmith Stretch only takes it at its next analysis frame and fades it
 * in over its synthesis window (40 ms at the window Pitch Shift's High Quality engine uses).
 *
 * A tap reading faster than the input catches up with it, and one reading slower falls behind, so
 * every so often the tap jumps: back by a stretch of input when shifting up, forward when
 * shifting down. The jump is crossfaded, and its length is chosen where the waveform lines up
 * with what the tap was playing: the offset whose recent input correlates best with the tap's,
 * found on a copy decimated to about 12 kHz and refined at the full rate. On a single note that
 * lands a whole number of periods away, so the crossfade joins two copies of the same cycle and
 * the splice is close to inaudible. A chord has no common period, so its splices show as a
 * faint flutter; that is the character of a time-domain "whammy". A poorly matched splice uses
 * an equal-power crossfade rather than an amplitude-complementary one, so it does not dip.
 *
 * Latency is the tap's delay, which moves: down to a few milliseconds just before a jump back,
 * up to about 25 ms just before a jump forward. Shifting up it averages about 10 ms (less for a
 * small shift), shifting down about 12 ms. NominalLatencySamples() is the figure to report to a
 * host and to align a dry signal to.
 *
 * Usage, per sample: Write() the input, then Process() for the output. Write() every sample,
 * even while the output is not wanted: Engage() starts a tap on that history, so it has to be
 * current. Nothing allocates after Prepare().
 */
class TimeDomainPitchShifter
{
  public:
    /// Crossfade length of a splice.
    static constexpr double kSpliceFadeSeconds = 0.006;
    /// Shortest jump. A shift near 0 st jumps this far and then plays for a long time before the
    /// next one, so it sets how often a small shift splices.
    static constexpr double kMinJumpSeconds = 0.004;
    /// How far past the shortest jump the search looks: one period at 62.5 Hz, below the low B
    /// of a seven-string, so a single note always has a whole period to land on.
    static constexpr double kSearchSpanSeconds = 0.016;
    /// Length of the stretch of recent input compared at each candidate offset.
    static constexpr double kCorrelationSeconds = 0.010;
    static constexpr double kRefineSeconds = 0.005;
    static constexpr double kMarginSeconds = 0.0005;
    static constexpr double kNominalLatencySeconds = 0.010;
    /// Time constant a gliding shift follows its target with. It smooths the steps of a 7-bit
    /// controller (0.19 st apart across a 24 st pedal range) without being heard as lag.
    static constexpr double kGlideSeconds = 0.004;
    static constexpr double kAnalysisRateHz = 12000.0;

    void Prepare(double sampleRate)
    {
        mSampleRate = sampleRate;
        mDecimation = std::max(1, static_cast<int>(std::lround(sampleRate / kAnalysisRateHz)));

        mFadeLength = std::max(8, Samples(kSpliceFadeSeconds));
        mMinJump = std::max(8.0, static_cast<double>(Samples(kMinJumpSeconds)));
        mSearchSpan = std::max(8.0, static_cast<double>(Samples(kSearchSpanSeconds)));
        mMargin = std::max(2.0, static_cast<double>(Samples(kMarginSeconds)));
        mCorrelationLength = std::max(8, Samples(kCorrelationSeconds) / mDecimation);
        mRefineLength = std::max(8, Samples(kRefineSeconds));
        mNominalLatency = Samples(kNominalLatencySeconds);
        mGlideCoefficient = 1.0 - std::exp(-1.0 / (kGlideSeconds * sampleRate));

        // The longest delay read: a jump back from just before the next one would be due, a whole
        // search span long, plus the correlation window behind it. 100 ms leaves room for all of
        // that at any shift.
        const auto history = static_cast<std::size_t>(Samples(0.1) + 64);
        mCapacity = NextPowerOfTwo(history);
        mMask = mCapacity - 1;
        mLeft.assign(mCapacity, 0.0f);
        mRight.assign(mCapacity, 0.0f);
        mMaxDelay = static_cast<double>(mCapacity) - 8.0;

        mMonoLength = mCapacity;
        mMono.assign(2 * mMonoLength, 0.0f);
        mDecimatedLength = mCapacity / static_cast<std::size_t>(mDecimation) + 1;
        mDecimated.assign(2 * mDecimatedLength, 0.0f);

        Reset();
    }

    /// Forgets the input history and puts the tap back to start.
    void Reset()
    {
        std::fill(mLeft.begin(), mLeft.end(), 0.0f);
        std::fill(mRight.begin(), mRight.end(), 0.0f);
        std::fill(mMono.begin(), mMono.end(), 0.0f);
        std::fill(mDecimated.begin(), mDecimated.end(), 0.0f);
        mWritten = 0;
        mMonoPos = 0;
        mDecimatedPos = 0;
        mDecimatedWritten = 0;
        mAccumulator = 0.0f;
        mAccumulated = 0;
        mSemitones = mTargetSemitones;
        mRate = std::exp2(mSemitones / 12.0);
        mMain = kMinDelay;
        mFading = false;
    }

    /// The shift to move to. With `glide` it is followed smoothly (a pedal sweeping a free
    /// range); without, it lands at once (whole-semitone steps), which cannot click here: the tap
    /// only changes speed, it does not jump.
    void SetSemitones(double semitones, bool glide) noexcept
    {
        mTargetSemitones = semitones;
        mGlide = glide;
    }

    [[nodiscard]] int NominalLatencySamples() const noexcept
    {
        return mNominalLatency;
    }

    /// The current delay of the tap being played, in samples, for tests and measurement.
    [[nodiscard]] double CurrentDelaySamples() const noexcept
    {
        return mMain;
    }

    /// Records one input sample. Call it for every sample, before Process().
    void Write(float left, float right) noexcept
    {
        // A NaN kept in the history would be read for as long as it is in reach, and would turn
        // every correlation it falls in, and so a splice's crossfade gains, into NaN.
        left = IsFinite(left) ? left : 0.0f;
        right = IsFinite(right) ? right : 0.0f;

        const std::size_t index = static_cast<std::size_t>(mWritten) & mMask;
        mLeft[index] = left;
        mRight[index] = right;
        ++mWritten;

        const float mono = 0.5f * (left + right);
        mMono[mMonoPos] = mono;
        mMono[mMonoPos + mMonoLength] = mono;
        mMonoPos = (mMonoPos + 1 == mMonoLength) ? 0 : mMonoPos + 1;

        // A box average ahead of the decimation: crude, but the correlation only needs the lows
        // and middle, where a guitar's periodicity is.
        mAccumulator += mono;

        if (++mAccumulated == mDecimation)
        {
            const float value = mAccumulator / static_cast<float>(mDecimation);
            mDecimated[mDecimatedPos] = value;
            mDecimated[mDecimatedPos + mDecimatedLength] = value;
            mDecimatedPos = (mDecimatedPos + 1 == mDecimatedLength) ? 0 : mDecimatedPos + 1;
            ++mDecimatedWritten;
            mAccumulator = 0.0f;
            mAccumulated = 0;
        }
    }

    /// Starts the tap on the history just written, to take over from the dry signal. It starts
    /// where it lines up with the input playing now, so a crossfade from the dry signal to it
    /// joins matching waveforms: just behind the input to shift down (it falls further behind
    /// from there), one matched jump back to shift up.
    void Engage() noexcept
    {
        mSemitones = mTargetSemitones;
        mRate = std::exp2(mSemitones / 12.0);
        mFading = false;

        if (mRate > 1.0)
        {
            const double lowest = UpTrigger(mRate) + MinJump(mRate);
            mMain = FindSplice(0.0, lowest, lowest + mSearchSpan, mFadeCorrelation);
        }
        else
        {
            mMain = kMinDelay;
        }
    }

    /// One sample of shifted output, from the input written so far.
    void Process(float& outLeft, float& outRight) noexcept
    {
        AdvanceShift();

        if (!mFading)
        {
            StartSpliceIfDue();
        }

        float left = 0.0f;
        float right = 0.0f;
        ReadStereo(mMain, left, right);

        if (mFading)
        {
            float oldLeft = 0.0f;
            float oldRight = 0.0f;
            ReadStereo(mOld, oldLeft, oldRight);

            const double t = (static_cast<double>(mFadePosition) + 0.5) / static_cast<double>(mFadeLength);
            const double fadeIn = 0.5 - 0.5 * std::cos(kPi * t);
            // Matched waveforms add, so their gains should sum to one; unmatched ones add in
            // power, so theirs should. Blend between the two by how well the splice matched.
            const double powerIn = std::sqrt(fadeIn);
            const double powerOut = std::sqrt(1.0 - fadeIn);
            const double matched = std::clamp((mFadeCorrelation - 0.2) / 0.6, 0.0, 1.0);
            const auto gainIn = static_cast<float>(matched * fadeIn + (1.0 - matched) * powerIn);
            const auto gainOut = static_cast<float>(matched * (1.0 - fadeIn) + (1.0 - matched) * powerOut);

            left = left * gainIn + oldLeft * gainOut;
            right = right * gainIn + oldRight * gainOut;

            mOld = std::clamp(mOld + 1.0 - mRate, kMinDelay, mMaxDelay);

            if (++mFadePosition >= mFadeLength)
            {
                mFading = false;
            }
        }

        mMain = std::clamp(mMain + 1.0 - mRate, kMinDelay, mMaxDelay);
        outLeft = left;
        outRight = right;
    }

    /// The input from `delaySamples` ago: a dry path aligned with the nominal latency.
    void ReadDelayed(int delaySamples, float& left, float& right) const noexcept
    {
        const auto delay = static_cast<std::int64_t>(std::clamp(delaySamples, 0, static_cast<int>(mMask)));
        const std::size_t index = static_cast<std::size_t>(mWritten - 1 - delay) & mMask;
        left = mLeft[index];
        right = mRight[index];
    }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    /// Four-point Hermite reads one sample either side of the two it lands between.
    static constexpr double kMinDelay = 2.0;

    [[nodiscard]] int Samples(double seconds) const noexcept
    {
        return static_cast<int>(std::lround(seconds * mSampleRate));
    }

    [[nodiscard]] static std::size_t NextPowerOfTwo(std::size_t value) noexcept
    {
        std::size_t power = 1;

        while (power < value)
        {
            power <<= 1;
        }

        return power;
    }

    // Shifting up, the delay shrinks by (r - 1) a sample, and the tap being faded out keeps
    // shrinking through the crossfade, so a jump back starts early enough for it to last.
    [[nodiscard]] double UpTrigger(double rate) const noexcept
    {
        return kMinDelay + (rate - 1.0) * static_cast<double>(mFadeLength) + mMargin;
    }

    // A jump at least as long as the drift through one crossfade, so the next is not due at once.
    [[nodiscard]] double MinJump(double rate) const noexcept
    {
        return std::max(mMinJump, std::abs(rate - 1.0) * static_cast<double>(mFadeLength) + mMargin);
    }

    // Shifting down, the delay grows; the jump forward waits until the whole search span fits.
    [[nodiscard]] double DownTrigger(double rate) const noexcept
    {
        return kMinDelay + MinJump(rate) + mSearchSpan + mMargin;
    }

    void AdvanceShift() noexcept
    {
        if (mSemitones == mTargetSemitones)
        {
            return;
        }

        if (!mGlide || std::abs(mTargetSemitones - mSemitones) < 1.0e-4)
        {
            mSemitones = mTargetSemitones;
        }
        else
        {
            mSemitones += (mTargetSemitones - mSemitones) * mGlideCoefficient;
        }

        mRate = std::exp2(mSemitones / 12.0);
    }

    void StartSpliceIfDue() noexcept
    {
        double next = mMain;

        if (mRate > 1.0 && mMain < UpTrigger(mRate))
        {
            const double lowest = mMain + MinJump(mRate);
            next = FindSplice(mMain, lowest, lowest + mSearchSpan, mFadeCorrelation);
        }
        else if (mRate < 1.0 && mMain > DownTrigger(mRate))
        {
            const double highest = mMain - MinJump(mRate);
            next = FindSplice(mMain, std::max(kMinDelay, highest - mSearchSpan), highest, mFadeCorrelation);
        }
        else
        {
            return;
        }

        mOld = mMain;
        mMain = next;
        mFading = true;
        mFadePosition = 0;
    }

    /// The delay in [lowest, highest] whose recent input best matches the recent input at
    /// `from`, preferring shorter delays among near-equal matches (less latency, and on a steady
    /// note several whole periods match equally). Keeps the fractional part of `from`, so the
    /// jump is a whole number of samples. `correlation` receives how well it matched, 0 to 1.
    [[nodiscard]] double FindSplice(double from, double lowest, double highest, double& correlation) const noexcept
    {
        const double whole = std::floor(from);
        const double fraction = from - whole;
        const auto fromSample = static_cast<std::int64_t>(whole);
        const auto lowJump = static_cast<std::int64_t>(std::ceil(lowest - from));
        const auto highJump = std::max(lowJump, static_cast<std::int64_t>(std::floor(highest - from)));

        // Coarse search on the decimated copy. A decimated sample's delay is counted from the
        // newest complete one, which is up to one decimation step older than the newest input.
        const std::int64_t pending = mWritten - mDecimatedWritten * mDecimation;
        const auto toDecimated = [&](std::int64_t delay) {
            return std::max<std::int64_t>(0, (delay - pending) / mDecimation);
        };

        const std::int64_t reference = toDecimated(fromSample);
        const std::int64_t available =
            std::min<std::int64_t>(mDecimatedWritten, static_cast<std::int64_t>(mDecimatedLength)) - mCorrelationLength;
        std::int64_t bestJump = lowJump;
        double bestScore = -2.0;
        double bestCorrelation = 0.0;
        const double spanDecimated = std::max(1.0, static_cast<double>(highJump - lowJump) / mDecimation);

        if (reference <= available)
        {
            for (std::int64_t jump = lowJump; jump <= highJump; jump += mDecimation)
            {
                const std::int64_t candidate = toDecimated(fromSample + jump);

                if (candidate > available)
                {
                    break;
                }

                const double match =
                    Correlate(DecimatedWindow(reference), DecimatedWindow(candidate), mCorrelationLength);
                const double score =
                    match - kShorterDelayBias * static_cast<double>(jump - lowJump) / mDecimation / spanDecimated;

                if (score > bestScore)
                {
                    bestScore = score;
                    bestJump = jump;
                    bestCorrelation = match;
                }
            }
        }

        // Refine at the full rate, one decimation step either side.
        const auto monoAvailable = static_cast<std::int64_t>(
            std::min<std::int64_t>(mWritten, static_cast<std::int64_t>(mMonoLength)) - mRefineLength);

        if (fromSample <= monoAvailable)
        {
            std::int64_t refined = bestJump;
            double refinedMatch = -2.0;

            for (std::int64_t jump = std::max(lowJump, bestJump - mDecimation);
                 jump <= std::min(highJump, bestJump + mDecimation); ++jump)
            {
                if (fromSample + jump > monoAvailable)
                {
                    break;
                }

                const double match = Correlate(MonoWindow(fromSample), MonoWindow(fromSample + jump), mRefineLength);

                if (match > refinedMatch)
                {
                    refinedMatch = match;
                    refined = jump;
                }
            }

            bestJump = refined;
        }

        correlation = std::clamp(bestCorrelation, 0.0, 1.0);
        return std::clamp(static_cast<double>(fromSample + bestJump) + fraction, kMinDelay, mMaxDelay);
    }

    /// Start of the contiguous window of `mCorrelationLength` decimated samples whose newest is
    /// `delay` steps old. The rings are mirrored, so a window never wraps.
    [[nodiscard]] const float* DecimatedWindow(std::int64_t delay) const noexcept
    {
        const std::size_t newest = (mDecimatedPos + mDecimatedLength - 1) % mDecimatedLength;
        const std::size_t end = newest + mDecimatedLength - static_cast<std::size_t>(delay);
        return mDecimated.data() + (end + 1 - static_cast<std::size_t>(mCorrelationLength));
    }

    [[nodiscard]] const float* MonoWindow(std::int64_t delay) const noexcept
    {
        const std::size_t newest = (mMonoPos + mMonoLength - 1) % mMonoLength;
        const std::size_t end = newest + mMonoLength - static_cast<std::size_t>(delay);
        return mMono.data() + (end + 1 - static_cast<std::size_t>(mRefineLength));
    }

    /// Normalised cross-correlation; 0 for silence.
    [[nodiscard]] static double Correlate(const float* a, const float* b, int length) noexcept
    {
        float ab = 0.0f;
        float aa = 0.0f;
        float bb = 0.0f;

        for (int i = 0; i < length; ++i)
        {
            ab += a[i] * b[i];
            aa += a[i] * a[i];
            bb += b[i] * b[i];
        }

        const double energy = static_cast<double>(aa) * static_cast<double>(bb);
        return energy > 1.0e-18 ? static_cast<double>(ab) / std::sqrt(energy) : 0.0;
    }

    void ReadStereo(double delay, float& left, float& right) const noexcept
    {
        const auto whole = static_cast<std::int64_t>(delay);
        const auto t = static_cast<float>(delay - static_cast<double>(whole));
        // Newest to oldest around the read point: delays whole-1, whole, whole+1, whole+2.
        const std::int64_t newest = mWritten - 1 - whole;
        const std::size_t i0 = static_cast<std::size_t>(newest + 1) & mMask;
        const std::size_t i1 = static_cast<std::size_t>(newest) & mMask;
        const std::size_t i2 = static_cast<std::size_t>(newest - 1) & mMask;
        const std::size_t i3 = static_cast<std::size_t>(newest - 2) & mMask;
        left = Hermite(mLeft[i0], mLeft[i1], mLeft[i2], mLeft[i3], t);
        right = Hermite(mRight[i0], mRight[i1], mRight[i2], mRight[i3], t);
    }

    /// Catmull-Rom between y0 (t = 0) and y1 (t = 1).
    [[nodiscard]] static float Hermite(float ym1, float y0, float y1, float y2, float t) noexcept
    {
        const float c1 = 0.5f * (y1 - ym1);
        const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
        const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);
        return ((c3 * t + c2) * t + c1) * t + y0;
    }

    /// How much correlation a candidate a whole search span further away has to give up to lose
    /// to the nearest one.
    static constexpr double kShorterDelayBias = 0.1;

    double mSampleRate = 48000.0;
    int mDecimation = 4;
    int mFadeLength = 288;
    double mMinJump = 192.0;
    double mSearchSpan = 768.0;
    double mMargin = 24.0;
    int mCorrelationLength = 120;
    int mRefineLength = 240;
    int mNominalLatency = 480;
    double mGlideCoefficient = 0.005;
    double mMaxDelay = 4096.0;

    std::vector<float> mLeft;
    std::vector<float> mRight;
    std::size_t mCapacity = 0;
    std::size_t mMask = 0;
    std::int64_t mWritten = 0;

    std::vector<float> mMono;
    std::size_t mMonoLength = 0;
    std::size_t mMonoPos = 0;
    std::vector<float> mDecimated;
    std::size_t mDecimatedLength = 0;
    std::size_t mDecimatedPos = 0;
    std::int64_t mDecimatedWritten = 0;
    float mAccumulator = 0.0f;
    int mAccumulated = 0;

    double mTargetSemitones = 0.0;
    double mSemitones = 0.0;
    double mRate = 1.0;
    bool mGlide = false;

    double mMain = kMinDelay;
    double mOld = kMinDelay;
    bool mFading = false;
    int mFadePosition = 0;
    double mFadeCorrelation = 1.0;
};
} // namespace guitarfx
