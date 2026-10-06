#pragma once

#include "dsp/FiniteCheck.h"
#include "dsp/PickAttackDetector.h"
#include "dsp/WaveformHistory.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace guitarfx
{
/**
 * Live transpose for guitar and bass: the whole instrument tuned down (or up), with every note's
 * attack on time.
 *
 * It works on the waveform. The input goes into a delay line and a read tap moves through it at
 * the pitch ratio `r = 2^(st/12)`, so the tap's delay drifts: it grows while shifting down and
 * shrinks while shifting up, across a window between kFloorSeconds and the window length. At the
 * end of the window the tap has to jump, and the jump is where the quality is won or lost:
 *
 *  - Where to land. Every legal landing point is scored by how badly the waveform there matches
 *    what the tap is playing (normalised cross-correlation over kCorrelationSeconds, longer than
 *    a low E1's period), divided by how long the tap can then run before it must jump again. So
 *    a long run with a good match beats a short perfect one, and a sustained note jumps rarely;
 *    on a single note every candidate worth taking is a whole number of periods away, so the
 *    joint is clean. The search runs on a copy decimated to kAnalysisRateHz, spread over the
 *    kSearchLeadSeconds before the jump is due, and is refined at the full rate to a fraction of
 *    a sample: a period is rarely a whole number of samples.
 *  - How to cross. The two taps crossfade, and the poorer the match the longer the fade
 *    (kMatchedFadeSeconds at kGoodMatch, up to kUnmatchedFadeSeconds at kPoorMatch): a chord has
 *    no common period, and a long fade lets its mismatched partials drift across instead of
 *    stepping. The gains are normalised by the measured correlation, so the level holds whether
 *    the taps add as amplitudes (matched) or as powers (unrelated).
 *  - Attacks. A pick attack (PickAttackDetector) moves the tap to the newest audio at once, with
 *    a kAttackFadeSeconds fade, so attacks arrive a few milliseconds late wherever the tap had
 *    drifted. An attack may cut into a crossfade that is still running: the fade in progress is
 *    frozen and faded out as one signal. Shifting up, the tap eats input faster than it arrives
 *    and must jump back soon after an attack, so it lands an attack deep enough that the next
 *    jump can stay clear of the pick's first kAttackProtectSeconds, up to
 *    kAttackLatencyCapSeconds; the pick is then heard a little later rather than twice.
 *
 * The felt latency is set by the attacks. NominalLatencySamples(), the tap's mean delay, is the
 * figure to report to a host and to align a dry signal to.
 *
 * Usage, per sample: Write() the input, then Process() for the output. Write() every sample, even
 * while the output is not wanted: Engage() starts the tap on that history. Stereo shares one tap
 * and one control path (the channels' mean), so the image never smears. Nothing allocates after
 * Prepare(), and the output does not depend on the block size.
 *
 * docs/transpose-engine.md records the measurements behind the constants.
 */
class SpliceTransposer
{
  public:
    static constexpr double kFloorSeconds = 0.002;
    static constexpr double kDefaultWindowSeconds = 0.030;
    static constexpr double kMinWindowSeconds = 0.015;
    static constexpr double kMaxWindowSeconds = 0.060;
    static constexpr double kCorrelationSeconds = 0.025;
    static constexpr double kAnalysisRateHz = 24000.0;
    static constexpr double kSearchLeadSeconds = 0.003;
    static constexpr double kMatchedFadeSeconds = 0.030;
    static constexpr double kUnmatchedFadeSeconds = 0.120;
    static constexpr double kShortestFadeSeconds = 0.006;
    static constexpr double kGoodMatch = 0.95;
    static constexpr double kPoorMatch = 0.6;
    static constexpr double kAttackFadeSeconds = 0.002;
    static constexpr double kAttackSpanSeconds = 0.002;
    static constexpr double kAttackProtectSeconds = 0.006;
    static constexpr double kAttackLatencyCapSeconds = 0.016;
    /// How long after an attack an upshift's jumps keep clear of it.
    static constexpr double kAttackWatchSeconds = 0.060;
    /// The longest jump on offer, as a share of the window: 0.82 of 30 ms holds a low E1 period.
    static constexpr double kLongestJumpShare = 0.82;
    static constexpr double kGlideSeconds = 0.004;

    void Prepare(double sampleRate)
    {
        mSampleRate = sampleRate;
        mFloor = std::max(4.0, Seconds(kFloorSeconds));
        mLead = std::max(1.0, Seconds(kSearchLeadSeconds));
        mMatchedFade = Seconds(kMatchedFadeSeconds);
        mUnmatchedFade = Seconds(kUnmatchedFadeSeconds);
        mShortestFade = std::max(8.0, Seconds(kShortestFadeSeconds));
        mAttackFade = std::max(8.0, Seconds(kAttackFadeSeconds));
        mAttackSpan = std::max(2.0, Seconds(kAttackSpanSeconds));
        mAttackProtect = Seconds(kAttackProtectSeconds);
        mAttackLatencyCap = Seconds(kAttackLatencyCapSeconds);
        mAttackWatch = Seconds(kAttackWatchSeconds);
        mGlideCoefficient = 1.0 - std::exp(-1.0 / (kGlideSeconds * sampleRate));

        // The deepest read: a downshift tap still fading out past the end of the longest window,
        // plus the correlation window behind a candidate.
        const double reach = Seconds(kMaxWindowSeconds + kUnmatchedFadeSeconds + kCorrelationSeconds + 0.02) + 64.0;
        mCapacity = NextPowerOfTwo(static_cast<std::size_t>(reach));
        mMask = mCapacity - 1;
        mMaxDelay = static_cast<double>(mCapacity) - Seconds(kCorrelationSeconds) - 16.0;
        mLeft.assign(mCapacity, 0.0f);
        mRight.assign(mCapacity, 0.0f);
        mHistory.Prepare(sampleRate, mCapacity, kAnalysisRateHz, kCorrelationSeconds, kCorrelationSeconds);
        mDetector.Prepare(sampleRate);

        SetWindowSeconds(mWindowSeconds);
        Reset();
    }

    /// The window the tap drifts across: a longer one jumps less often and holds lower notes, at
    /// the cost of latency. A low E1's period (24 ms) needs about 30 ms.
    void SetWindowSeconds(double seconds) noexcept
    {
        mWindowSeconds = std::clamp(seconds, kMinWindowSeconds, kMaxWindowSeconds);
        mWindow = Seconds(mWindowSeconds);
        mSearch = {};
        UpdateGeometry();
    }

    /// Forgets the input history and puts the tap back to start.
    void Reset()
    {
        std::fill(mLeft.begin(), mLeft.end(), 0.0f);
        std::fill(mRight.begin(), mRight.end(), 0.0f);
        mHistory.Reset();
        mDetector.Reset();
        mWritten = 0;
        mSemitones = mTargetSemitones;
        mRate = std::exp2(mSemitones / 12.0);
        UpdateGeometry();
        mDelay = mFloor;
        mFading = false;
        mOutgoingPair = false;
        mSearch = {};
        mAttackPending = false;
        mSinceAttack = mAttackWatch + 1.0;
    }

    /// The shift to move to. With `glide` it is followed smoothly (a pedal); without, it lands at
    /// once, which cannot click here: the tap only changes speed.
    void SetSemitones(double semitones, bool glide) noexcept
    {
        mTargetSemitones = semitones;
        mGlide = glide;
    }

    [[nodiscard]] int NominalLatencySamples() const noexcept
    {
        return static_cast<int>(std::lround(0.5 * (mFloor + mWindow)));
    }

    /// The delay of the tap being played, in samples, for tests and measurement.
    [[nodiscard]] double CurrentDelaySamples() const noexcept
    {
        return mDelay;
    }

    [[nodiscard]] long long SpliceCount() const noexcept
    {
        return mSplices;
    }

    [[nodiscard]] long long AttackSpliceCount() const noexcept
    {
        return mAttackSplices;
    }

    /// Records one input sample. Call it for every sample, before Process().
    void Write(float left, float right) noexcept
    {
        // A NaN kept in the history would poison every correlation it falls in.
        left = IsFinite(left) ? left : 0.0f;
        right = IsFinite(right) ? right : 0.0f;

        const std::size_t index = static_cast<std::size_t>(mWritten) & mMask;
        mLeft[index] = left;
        mRight[index] = right;
        ++mWritten;

        const float mono = 0.5f * (left + right);
        mHistory.Write(mono);

        if (mDetector.Process(mono))
        {
            mAttackPending = true;
            mSinceAttack = 0.0;
        }
        else
        {
            mSinceAttack += 1.0;
        }
    }

    /// Starts the tap on the history just written, taking over from the dry signal: at the floor
    /// to shift down, or where it best matches the input playing now to shift up.
    void Engage() noexcept
    {
        mSemitones = mTargetSemitones;
        mRate = std::exp2(mSemitones / 12.0);
        UpdateGeometry();
        mFading = false;
        mOutgoingPair = false;
        mSearch = {};
        mAttackPending = false;
        mDelay = mFloor;

        if (mRate > 1.0)
        {
            // Matched against the newest input the decimated copy covers: the dry signal the
            // crossfade into the shifted one starts from.
            const std::int64_t from = mHistory.NewestCoarse();
            const double fromDelay = static_cast<double>(mHistory.Newest() - from);
            double match = 0.0;
            const double jump = BestMatchJump(from, fromDelay - mLandHigh, fromDelay - mLandLow, match);
            mDelay = std::clamp(fromDelay - jump, 2.0, mMaxDelay);
        }
    }

    /// Starts the tap `delaySamples` behind the newest input instead, held inside the range a
    /// splice could land in: for a voice that has waited to learn its shift, so a note that began
    /// that long ago is still heard from its pick. The tap catches up as it splices, and the next
    /// attack moves it to the newest audio as usual.
    void EngageAt(double delaySamples) noexcept
    {
        mSemitones = mTargetSemitones;
        mRate = std::exp2(mSemitones / 12.0);
        UpdateGeometry();
        mFading = false;
        mOutgoingPair = false;
        mSearch = {};
        mAttackPending = false;

        const double lowest = mRate > 1.0 ? mLandLow : mFloor;
        const double delay = IsFinite(delaySamples) ? delaySamples : lowest;
        mDelay = std::clamp(delay, lowest, std::max(lowest, mLandHigh));
    }

    /// One sample of shifted output, from the input written so far.
    void Process(float& outLeft, float& outRight) noexcept
    {
        AdvanceShift();

        if (mAttackPending)
        {
            mAttackPending = false;
            StartAttackSplice();
        }

        if (!mFading)
        {
            PlanDriftSplice();
        }

        if (mSearch.active)
        {
            StepSearch(mSearch.perSample);
        }

        if (!mFading && mSearch.ready && DriftSpliceDue())
        {
            StartDriftSplice();
        }

        float left = 0.0f;
        float right = 0.0f;
        ReadStereo(mDelay, left, right);

        if (mFading)
        {
            float oldLeft = 0.0f;
            float oldRight = 0.0f;
            ReadStereo(mOutgoing, oldLeft, oldRight);

            if (mOutgoingPair)
            {
                float pairLeft = 0.0f;
                float pairRight = 0.0f;
                ReadStereo(mOutgoingSecond, pairLeft, pairRight);
                oldLeft = oldLeft * mOutgoingGain + pairLeft * mOutgoingSecondGain;
                oldRight = oldRight * mOutgoingGain + pairRight * mOutgoingSecondGain;
            }

            float gainIn = 0.0f;
            float gainOut = 0.0f;
            FadeGains(gainIn, gainOut);
            left = left * gainIn + oldLeft * gainOut;
            right = right * gainIn + oldRight * gainOut;

            const double drift = 1.0 - mRate;
            mOutgoing = std::clamp(mOutgoing + drift, 2.0, mMaxDelay);
            mOutgoingSecond = std::clamp(mOutgoingSecond + drift, 2.0, mMaxDelay);

            if (++mFadePosition >= mFadeLength)
            {
                mFading = false;
                mOutgoingPair = false;
            }
        }

        mDelay = std::clamp(mDelay + 1.0 - mRate, 2.0, mMaxDelay);
        outLeft = left;
        outRight = right;
    }

    /// The input from `delaySamples` ago: a dry path aligned with the nominal latency.
    void ReadDelayed(int delaySamples, float& left, float& right) const noexcept
    {
        const auto delay = static_cast<std::int64_t>(std::clamp(delaySamples, 0, static_cast<int>(mMask)));
        const std::size_t index = static_cast<std::size_t>(static_cast<std::int64_t>(mWritten) - 1 - delay) & mMask;
        left = mLeft[index];
        right = mRight[index];
    }

  private:
    static constexpr double kPi = 3.14159265358979323846;
    static constexpr double kMargin = 8.0;

    /// A search for the next jump, run a few candidates per sample. Jumps are in whole samples
    /// the read position moves forward (the delay drops by as much), relative to `from`.
    struct Search
    {
        bool active = false;
        bool ready = false;
        double atSplice = 0.0; ///< the delay the splice is planned at; 0 scores the plain match
        std::int64_t from = 0;
        double fromEnergy = 0.0;
        std::int64_t lowJump = 0;
        std::int64_t highJump = 0;
        std::int64_t next = 0;
        int perSample = 1;
        double plannedRate = 1.0;
        double bestScore = 0.0;
        double bestMatch = 0.0;
        std::int64_t bestJump = 0;
        double jump = 0.0; ///< the refined jump, to a fraction of a sample
    };

    [[nodiscard]] double Seconds(double seconds) const noexcept
    {
        return std::round(seconds * mSampleRate);
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

    /// Where splices may land and when they are due, for the current shift and window.
    ///
    /// A splice is clean only when its jump is a whole number of the note's periods, so what
    /// matters is the range of jumps on offer: its longest has to hold the lowest period wanted
    /// (kLongestJumpShare of the window), and it has to span an octave, so every shorter period
    /// has a multiple inside it. A downshift tap drifts deeper and jumps forward from the end of
    /// the window to anywhere from the floor up, which gives that for free. An upshift tap gains
    /// on the input, jumps back from a guard near the front, and keeps losing ground while it
    /// fades: the fade, and the search lead before it, spend window at the drift rate. So the
    /// guard is fixed by the longest jump, the longest fade is what the guard leaves the tap
    /// being faded out, and the shortest fade and the lead shrink as the shift grows until the
    /// range still spans its octave.
    void UpdateGeometry() noexcept
    {
        const double drift = std::abs(1.0 - mRate);
        mFadeCeiling = mUnmatchedFade;
        mShortestFadeNow = mShortestFade;
        mLeadNow = mLead;

        if (mRate > 1.0)
        {
            mGuard = std::max(mWindow * (1.0 - kLongestJumpShare), mFloor + kMargin);
            mFadeCeiling = std::clamp((mGuard - 4.0) / drift, 8.0, mUnmatchedFade);
            const double cost = drift * (mShortestFade + mLead);
            const double allowed = 0.5 * (mWindow - mGuard) - kMargin;

            if (cost > allowed)
            {
                const double scale = std::max(0.0, allowed) / cost;
                mShortestFadeNow = std::max(8.0, mShortestFade * scale);
                mLeadNow = std::max(1.0, mLead * scale);
            }

            mShortestFadeNow = std::min(mShortestFadeNow, mFadeCeiling);
            mLandLow = std::min(mGuard + drift * (mShortestFadeNow + mLeadNow) + kMargin, mWindow - kMargin);
            mLandHigh = mWindow;
            // Landing an attack at D, the tap reaches the guard (D - guard) / drift later and then
            // jumps at least mLandLow deep; for that to come after the pick's first
            // mAttackProtect, D >= guard + drift * (mAttackProtect + mLandLow), with some margin.
            const double clear = mGuard + drift * (mAttackProtect + mLandLow + 4.0 * kMargin) + kMargin;
            const double nearest = mGuard + drift * (mAttackFade + mLeadNow) + kMargin;
            mAttackLandLow = std::min(std::max(nearest, std::min(clear, mAttackLatencyCap)), mWindow - mAttackSpan);
        }
        else
        {
            mGuard = mFloor;
            mLandLow = mFloor;
            mLandHigh = std::max(mFloor + kMargin, mWindow - drift * (mShortestFade + mLeadNow) - kMargin);
            mAttackLandLow = mFloor;
        }
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

        const double previous = mRate;
        mRate = std::exp2(mSemitones / 12.0);
        UpdateGeometry();

        // A plan made for the old drift lands a few samples off after a small change, inside the
        // margins; a change of direction or a large step would land it outside the window.
        const auto direction = [](double rate) { return rate > 1.0 ? 1 : (rate < 1.0 ? -1 : 0); };

        if ((mSearch.active || mSearch.ready) &&
            (direction(mRate) != direction(previous) || std::abs(mRate - mSearch.plannedRate) > 0.1))
        {
            mSearch = {};
        }
    }

    [[nodiscard]] bool DriftSpliceDue() const noexcept
    {
        return mRate < 1.0 ? mDelay >= mWindow : (mRate > 1.0 && mDelay <= mGuard);
    }

    /// Starts the search for the next drift splice a lead before it is due, with the candidates
    /// placed where they will be when it lands.
    void PlanDriftSplice() noexcept
    {
        if (mSearch.active || mSearch.ready || mRate == 1.0)
        {
            return;
        }

        const double leadDrift = std::abs(1.0 - mRate) * mLeadNow;
        const bool due = mRate < 1.0 ? mDelay >= mWindow - leadDrift : mDelay <= mGuard + leadDrift;

        if (!due)
        {
            return;
        }

        const double atSplice =
            mRate < 1.0 ? std::max(mDelay + leadDrift, mWindow) : std::min(mDelay - leadDrift, mGuard);
        double landHigh = mLandHigh;

        if (mRate > 1.0 && mSinceAttack < mAttackWatch)
        {
            // Not back into the pick just played: when the splice lands, content older than the
            // attack's first mAttackProtect lies deeper than this.
            const double untilSplice = (mDelay - atSplice) / (mRate - 1.0);
            const double clear = mSinceAttack + untilSplice - mAttackProtect;

            if (clear >= mLandLow)
            {
                landHigh = std::min(landHigh, clear);
            }
        }

        BeginSearch(TapPosition(), atSplice - landHigh, atSplice - mLandLow, static_cast<int>(mLeadNow), atSplice);
    }

    void StartDriftSplice() noexcept
    {
        const double destination = mDelay - mSearch.jump;
        mSearch.ready = false;

        if (destination < 2.0 || destination > mMaxDelay)
        {
            return;
        }

        // The destination tap's fade has to finish before its own next search is due.
        const double drift = std::abs(1.0 - mRate);
        const double room = mRate < 1.0 ? (mWindow - drift * mLeadNow - kMargin - destination) / drift
                                        : (destination - mGuard - drift * mLeadNow - kMargin) / drift;
        const double t = std::clamp((kGoodMatch - mSearch.bestMatch) / (kGoodMatch - kPoorMatch), 0.0, 1.0);
        const double wanted = mMatchedFade + t * (mUnmatchedFade - mMatchedFade);
        const double length = std::max(8.0, std::min({wanted, std::max(room, mShortestFadeNow), mFadeCeiling}));
        BeginFade(destination, length, mSearch.bestMatch, false);
        ++mSplices;
    }

    /// A pick attack: the tap goes to the front of the window at once, if it has drifted far
    /// enough from it to make the jump worth it. Shifting up, the landing is where the jump that
    /// follows can stay clear of the pick, and a tap closer to the input than that is moved back
    /// to it, so the attack comes a little later rather than twice.
    void StartAttackSplice() noexcept
    {
        const double landLow = mAttackLandLow;
        const double landHigh = landLow + mAttackSpan;
        const bool nearEnough = mRate > 1.0 ? (mDelay >= landLow - mAttackSpan && mDelay <= landHigh + mAttackSpan)
                                            : mDelay <= landHigh + mAttackSpan;

        if (nearEnough || (mFading && mOutgoingPair))
        {
            return;
        }

        mSearch = {};
        double match = 0.0;
        const double jump = BestMatchJump(TapPosition(), mDelay - landHigh, mDelay - landLow, match);
        BeginFade(mDelay - jump, mAttackFade, match, true);
        ++mAttackSplices;
    }

    /// Crossfades from what is playing now to a tap at `destination`. With `interrupt`, a fade
    /// already running is frozen at its current gains and faded out as one signal.
    void BeginFade(double destination, double length, double match, bool interrupt) noexcept
    {
        if (mFading && interrupt)
        {
            float gainIn = 0.0f;
            float gainOut = 0.0f;
            FadeGains(gainIn, gainOut);
            mOutgoingSecond = mOutgoing;
            mOutgoingSecondGain = gainOut;
            mOutgoing = mDelay;
            mOutgoingGain = gainIn;
            mOutgoingPair = true;
        }
        else
        {
            mOutgoing = mDelay;
            mOutgoingGain = 1.0f;
            mOutgoingPair = false;
        }

        mDelay = std::clamp(destination, 2.0, mMaxDelay);
        mFading = true;
        mFadePosition = 0;
        mFadeLength = std::max(1, static_cast<int>(length));
        mFadeCorrelation = std::clamp(match, 0.0, 1.0);
    }

    /// A raised-cosine crossfade whose gains are normalised for the two signals' correlation:
    /// matched signals add as amplitudes, unrelated ones as powers.
    void FadeGains(float& gainIn, float& gainOut) const noexcept
    {
        const double t = (static_cast<double>(mFadePosition) + 0.5) / static_cast<double>(mFadeLength);
        const double in = 0.5 - 0.5 * std::cos(kPi * std::min(1.0, t));
        const double out = 1.0 - in;
        const double power = in * in + out * out + 2.0 * in * out * mFadeCorrelation;
        const double scale = 1.0 / std::sqrt(std::max(power, 1.0e-9));
        gainIn = static_cast<float>(in * scale);
        gainOut = static_cast<float>(out * scale);
    }

    /// The tap's read position, in whole samples of input.
    [[nodiscard]] std::int64_t TapPosition() const noexcept
    {
        return static_cast<std::int64_t>(std::floor(static_cast<double>(mHistory.Newest()) - mDelay));
    }

    /// Starts scoring the jumps [lowJump, highJump] against the waveform ending at `from`, spread
    /// over `lead` samples (0: all at once). With `atSplice` above zero the score is mismatch per
    /// sample of run, for a splice taken at that delay; otherwise it is the plain match.
    void BeginSearch(std::int64_t from, double lowJump, double highJump, int lead, double atSplice) noexcept
    {
        Search s;
        s.from = from;
        s.fromEnergy = WaveformHistory::Energy(mHistory.Coarse(from), mHistory.CoarseLength());
        // A candidate's window has to be in the decimated history already.
        const std::int64_t ceilingJump = mHistory.NewestCoarse() - from;
        s.lowJump = static_cast<std::int64_t>(std::ceil(lowJump));
        s.highJump = std::min(static_cast<std::int64_t>(std::floor(highJump)), ceilingJump);

        if (s.highJump < s.lowJump)
        {
            s.highJump = s.lowJump = std::min(s.lowJump, ceilingJump);
        }

        s.next = s.lowJump;
        s.atSplice = atSplice;
        s.plannedRate = mRate;
        s.bestScore = 1.0e30;
        s.bestJump = s.lowJump;
        s.active = true;
        const std::int64_t candidates = (s.highJump - s.lowJump) / mHistory.Decimation() + 1;
        s.perSample = lead > 0 ? static_cast<int>((candidates + lead - 1) / lead) : static_cast<int>(candidates);
        mSearch = s;
    }

    void StepSearch(int count) noexcept
    {
        Search& s = mSearch;
        const float* reference = mHistory.Coarse(s.from);
        const int length = mHistory.CoarseLength();

        while (count-- > 0 && s.next <= s.highJump)
        {
            const std::int64_t jump = s.next;
            const double match =
                WaveformHistory::Correlate(reference, s.fromEnergy, mHistory.Coarse(s.from + jump), length);
            double score = -match;

            if (s.atSplice > 0.0)
            {
                // Mismatch per sample of run the jump buys: from where the tap lands to where it
                // next has to jump.
                const double landing = s.atSplice - static_cast<double>(jump);
                const double run = mRate < 1.0 ? mWindow - landing : landing - mGuard;
                score = (1.0 - match) / std::max(1.0, run);
            }

            if (score < s.bestScore)
            {
                s.bestScore = score;
                s.bestJump = jump;
                s.bestMatch = match;
            }

            s.next += mHistory.Decimation();
        }

        if (s.next > s.highJump)
        {
            s.jump = Refine(s.from, s.bestJump, s.lowJump, s.highJump);
            s.active = false;
            s.ready = true;
        }
    }

    /// The best jump within two decimation steps of `coarse`, at the full rate: the best whole
    /// sample, then a parabola through it and its neighbours. A period is rarely a whole number
    /// of samples, and two octaves down the splices come often enough for the rounding to be
    /// heard as a few cents.
    [[nodiscard]] double Refine(std::int64_t from, std::int64_t coarse, std::int64_t low,
                                std::int64_t high) const noexcept
    {
        const std::int64_t newest = mHistory.Newest();
        const int length = mHistory.FineLength();
        const float* reference = mHistory.Fine(from);
        const double energy = WaveformHistory::Energy(reference, length);
        const std::int64_t reach = 2 * mHistory.Decimation();
        const auto matchAt = [&](std::int64_t jump) {
            return WaveformHistory::Correlate(reference, energy, mHistory.Fine(from + jump), length);
        };
        const std::int64_t first = std::max(low, coarse - reach);
        const std::int64_t last = std::min({high, coarse + reach, newest - from});
        std::int64_t best = std::min(coarse, last);
        double bestMatch = -2.0;

        for (std::int64_t jump = first; jump <= last; ++jump)
        {
            const double match = matchAt(jump);

            if (match > bestMatch)
            {
                bestMatch = match;
                best = jump;
            }
        }

        if (best <= first || best >= last)
        {
            return static_cast<double>(best);
        }

        const double before = matchAt(best - 1);
        const double after = matchAt(best + 1);
        const double curvature = before - 2.0 * bestMatch + after;
        const double offset = curvature < -1.0e-12 ? 0.5 * (before - after) / curvature : 0.0;
        return static_cast<double>(best) + std::clamp(offset, -0.5, 0.5);
    }

    /// The jump in [lowJump, highJump] whose waveform best matches the one ending at `from`,
    /// searched at once. Leaves no search pending.
    [[nodiscard]] double BestMatchJump(std::int64_t from, double lowJump, double highJump, double& match) noexcept
    {
        BeginSearch(from, lowJump, highJump, 0, 0.0);
        StepSearch(mSearch.perSample);
        match = mSearch.bestMatch;
        const double jump = mSearch.jump;
        mSearch = {};
        return jump;
    }

    void ReadStereo(double delay, float& left, float& right) const noexcept
    {
        const auto whole = static_cast<std::int64_t>(delay);
        const auto t = static_cast<float>(delay - static_cast<double>(whole));
        // Newest to oldest around the read point: delays whole-1, whole, whole+1, whole+2.
        const std::int64_t newest = static_cast<std::int64_t>(mWritten) - 1 - whole;
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

    double mSampleRate = 48000.0;
    double mFloor = 96.0;
    double mWindowSeconds = kDefaultWindowSeconds;
    double mWindow = 1440.0;
    double mLead = 144.0;
    double mLeadNow = 144.0;
    double mMatchedFade = 1440.0;
    double mUnmatchedFade = 5760.0;
    double mShortestFade = 288.0;
    double mShortestFadeNow = 288.0;
    double mFadeCeiling = 5760.0;
    double mAttackFade = 96.0;
    double mAttackSpan = 96.0;
    double mAttackProtect = 288.0;
    double mAttackLatencyCap = 768.0;
    double mAttackWatch = 2880.0;
    double mGuard = 96.0;
    double mLandLow = 96.0;
    double mLandHigh = 1440.0;
    double mAttackLandLow = 96.0;
    double mGlideCoefficient = 0.005;
    double mMaxDelay = 8192.0;

    std::vector<float> mLeft;
    std::vector<float> mRight;
    std::size_t mCapacity = 0;
    std::size_t mMask = 0;
    std::uint64_t mWritten = 0;
    WaveformHistory mHistory;
    PickAttackDetector mDetector;

    double mTargetSemitones = 0.0;
    double mSemitones = 0.0;
    double mRate = 1.0;
    bool mGlide = false;

    double mDelay = 96.0;
    bool mFading = false;
    int mFadePosition = 0;
    int mFadeLength = 1;
    double mFadeCorrelation = 1.0;
    double mOutgoing = 96.0;
    float mOutgoingGain = 1.0f;
    bool mOutgoingPair = false;
    double mOutgoingSecond = 96.0;
    float mOutgoingSecondGain = 0.0f;

    Search mSearch;
    bool mAttackPending = false;
    double mSinceAttack = 0.0;
    long long mSplices = 0;
    long long mAttackSplices = 0;
};
} // namespace guitarfx
