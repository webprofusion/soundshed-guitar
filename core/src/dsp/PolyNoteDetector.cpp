#include "dsp/PolyNoteDetector.h"

namespace guitarfx
{
namespace
{
/// The frame's level, and the short window's rise over each of two spans, come before the note's
/// place in the range among the inputs that are the same for every note.
constexpr int kGlobals = 3;
constexpr float kLevelDecayBelPerSecond = 1.0f;
/// A frame whose loudest partial is this far under the threshold (12 dB) is silence.
constexpr float kQuietBel = 0.6f;
constexpr double kPeakReleaseSeconds = 0.05;
/// The plain middle window sets the level, the plain short one how fast it is rising.
constexpr int kLevelChannel = 1;
constexpr int kRiseChannel = 2;

// Following pitch: measured on GuitarSet player 04 against its per-string pitch contours
// ("Polyphonic + Bend" under Guitar to MIDI in docs/fx-library.md). The 43 ms window is steady in
// chords; the 21 ms one follows a lone note's bends with half the delay. A reading counts only when
// the note's partials agree (a partial of a neighbour pulls one of them away), the note is within
// 10 dB of its own peak, that peak falling 20 dB a second as a string does (a muted note's place
// fills with its neighbours' leakage), and the model still has the note at even odds or better (as
// the next note starts beside it, the reading wanders toward it); otherwise the last pitch holds.
// A tighter level or sounding gate freezes real bends: the model is less sure of a bent note, and
// a 10 dB gate on the peak since the start shuts out a string bent once it has rung a while. Each
// partial is looked for at most half a semitone either side of where it was,
// less when another note's partial is nearer: 0.3 to 0.75 measure alike.
constexpr double kWidestPitchSearch = 0.5;
constexpr int kChordPitchWindow = 1;
constexpr int kAlonePitchWindow = 2;
constexpr int kPitchPartials = 6;
constexpr double kMaxPartialSpread = 0.1;
constexpr double kMaxPitchDrop = 10.0; // 10 dB, as an energy ratio
constexpr double kPitchPeakFallDbPerSecond = 20.0;
constexpr float kFollowWhileSounding = 0.5f;

float Sigmoid(float x)
{
    return 1.0f / (1.0f + std::exp(-x));
}

/// acc += scale * column
void AddScaled(float* acc, const float* column, float scale, std::size_t count)
{
    for (std::size_t o = 0; o < count; ++o)
    {
        acc[o] += scale * column[o];
    }
}

/// out[o] = bias[o] + weights[o] . in
void Apply(const PolyNoteModel::Dense& layer, const float* in, float* out)
{
    for (int o = 0; o < layer.outputs; ++o)
    {
        const float* w = layer.weights + static_cast<std::size_t>(o) * static_cast<std::size_t>(layer.inputs);
        float acc = layer.bias[o];

        for (int i = 0; i < layer.inputs; ++i)
        {
            acc += w[i] * in[i];
        }

        out[o] = acc;
    }
}
} // namespace

void PolyNoteDetector::Prepare(double sampleRate, const PolyNoteModel& model)
{
    mModel = model;
    mSpectrum.Prepare(sampleRate);
    const double rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    mFrameSeconds = mSpectrum.HopSamples() / rate;
    mMinRestartFrames = std::max(1, static_cast<int>(std::ceil(mSettings.minRestartSeconds / mFrameSeconds)));
    mPeakRelease = static_cast<float>(std::exp(-1.0 / (kPeakReleaseSeconds * rate)));
    mDepth = 1;

    for (int back : mModel.context)
    {
        mDepth = std::max(mDepth, back + 1);
    }

    const std::size_t channels = mModel.channels.size();
    mHistory.assign(static_cast<std::size_t>(mDepth) * channels * PolySpectrum::kBins, PolySpectrum::kFloor);
    mShortLevel.assign(static_cast<std::size_t>(mDepth), PolySpectrum::kFloor);

    mPerFrame = mModel.ratios.size() * mModel.offsets.size();
    mIndex.assign(static_cast<std::size_t>(kNotes) * mPerFrame, -1);

    for (int p = 0; p < kNotes; ++p)
    {
        const int base = (kLowestNote + p - PolySpectrum::kLowestMidi) * PolySpectrum::kBinsPerSemitone;
        std::size_t j = 0;

        for (float ratio : mModel.ratios)
        {
            const int harmonic = PolySpectrum::HarmonicOffset(ratio);

            for (int offset : mModel.offsets)
            {
                const int bin = base + harmonic + offset;
                mIndex[static_cast<std::size_t>(p) * mPerFrame + j] =
                    (bin >= 0 && bin < PolySpectrum::kBins) ? bin : -1;
                ++j;
            }
        }
    }

    std::size_t widest = 1;

    for (const auto& layer : mModel.body)
    {
        widest = std::max(widest, static_cast<std::size_t>(std::max(layer.inputs, layer.outputs)));
    }

    widest = std::max(widest, static_cast<std::size_t>(std::max(mModel.mix.inputs, mModel.mix.outputs)));
    mReading = mModel.body.empty() ? 0 : mModel.body.back().outputs;

    // The first layer by input, not by output: nearly half of a frame's inputs are zero (nothing at
    // that partial), and one that is costs nothing this way round.
    mFirstColumns.clear();

    if (!mModel.body.empty())
    {
        const auto& first = mModel.body.front();
        mFirstColumns.assign(static_cast<std::size_t>(first.inputs) * static_cast<std::size_t>(first.outputs), 0.0f);

        for (int o = 0; o < first.outputs; ++o)
        {
            for (int i = 0; i < first.inputs; ++i)
            {
                mFirstColumns[static_cast<std::size_t>(i) * first.outputs + o] =
                    first.weights[static_cast<std::size_t>(o) * first.inputs + i];
            }
        }
    }

    mA.assign(widest, 0.0f);
    mB.assign(widest, 0.0f);
    mNormalised.assign(mModel.context.size() * channels * PolySpectrum::kBins, 0.0f);
    mReadings.assign(static_cast<std::size_t>(kNotes) * static_cast<std::size_t>(std::max(1, mReading)), 0.0f);
    mSummaries.assign(static_cast<std::size_t>(kNotes) * static_cast<std::size_t>(std::max(1, mModel.summary.outputs)),
                      0.0f);
    Reset();
}

void PolyNoteDetector::Reset()
{
    mSpectrum.Reset();
    std::fill(mHistory.begin(), mHistory.end(), PolySpectrum::kFloor);
    std::fill(mShortLevel.begin(), mShortLevel.end(), PolySpectrum::kFloor);
    mAnswers = {};
    mNewest = 0;
    mLevel = PolySpectrum::kFloor;
    mPeak = 0.0f;
    mNotes = {};
    mSounding = 0;
}

// Bends. With SetFollowPitch, each sounding note's pitch is measured every frame from its own
// partials, starting from where it was the frame before, so it follows a bend or vibrato. Notes
// start and stop exactly as without: a bend past the next note's place starts that note as well
// (the bent note carries on until its own place empties). Keeping the bent note in the other's
// place instead, so that only one sounded, lost notes: on GuitarSet player 04 it found a sixth fewer
// solo notes (slides and hammer-ons read as bends) and measured pitch no better.
//
// A note starts at the whole note: in the frame it starts, the windows have only just taken it in,
// and a reading there was no nearer the pitch it settles at. It settles within a few frames. A
// note's last frames are its least sure (it is fading, or the next note is starting beside it), so
// a Stop carries its pitch from kStopPitchFrames before, for its release to keep.
void PolyNoteDetector::FollowPitches()
{
    const double peakFall = std::pow(10.0, -kPitchPeakFallDbPerSecond / 10.0 * mFrameSeconds);
    std::array<double, kNotes> pitches{};
    int count = 0;

    for (const Note& note : mNotes)
    {
        if (note.on)
        {
            pitches[static_cast<std::size_t>(count++)] = note.pitch;
        }
    }

    int index = 0;

    for (Note& note : mNotes)
    {
        if (!note.on)
        {
            continue;
        }

        // The others: every sounding note but this one.
        std::array<double, kNotes> others{};
        int n = 0;

        for (int j = 0; j < count; ++j)
        {
            if (j != index)
            {
                others[static_cast<std::size_t>(n++)] = pitches[static_cast<std::size_t>(j)];
            }
        }

        ++index;
        poly_pitch::Options options;
        options.window = n == 0 ? kAlonePitchWindow : kChordPitchWindow;
        options.partials = kPitchPartials;
        options.avoidCollisions = false;
        options.widestSearchSemitones = kWidestPitchSearch;
        const auto reading = poly_pitch::Measure(
            mSpectrum, note.pitch, std::span<const double>(others.data(), static_cast<std::size_t>(n)), options);

        if (!(reading.energy > 0.0) || !IsFinite(reading.pitch))
        {
            continue;
        }

        note.peakEnergy = std::max(note.peakEnergy * peakFall, reading.energy);
        const auto place = static_cast<std::size_t>(&note - mNotes.data());

        if (reading.spread <= kMaxPartialSpread && reading.energy * kMaxPitchDrop >= note.peakEnergy &&
            mAnswers[2 * place] >= kFollowWhileSounding)
        {
            note.pitch = reading.pitch;
        }
    }

    const auto frame = static_cast<std::size_t>(mSpectrum.FrameCount() % 8);

    for (Note& note : mNotes)
    {
        if (note.on)
        {
            note.history[frame] = static_cast<float>(note.pitch);
        }
    }
}

void PolyNoteDetector::SetSettings(const Settings& settings)
{
    mSettings = settings;
    mMinRestartFrames = std::max(1, static_cast<int>(std::ceil(mSettings.minRestartSeconds / mFrameSeconds)));
}

bool PolyNoteDetector::JudgeFrame()
{
    // The new frame joins the history: only the grids the model reads are kept.
    const std::size_t contexts = mModel.context.size();
    const std::size_t channels = mModel.channels.size();
    const std::size_t stride = channels * PolySpectrum::kBins;
    const float* frame = mSpectrum.Frame();
    mNewest = (mNewest + 1) % mDepth;
    float* slot = mHistory.data() + static_cast<std::size_t>(mNewest) * stride;

    for (std::size_t r = 0; r < channels; ++r)
    {
        std::copy_n(frame + static_cast<std::size_t>(mModel.channels[r]) * PolySpectrum::kBins, PolySpectrum::kBins,
                    slot + r * PolySpectrum::kBins);
    }

    const float* mid = frame + kLevelChannel * PolySpectrum::kBins;
    const float* shortest = frame + kRiseChannel * PolySpectrum::kBins;
    float loudest = PolySpectrum::kFloor;
    float sum = 0.0f;

    for (int k = 0; k < PolySpectrum::kBins; ++k)
    {
        loudest = std::max(loudest, mid[k]);
        sum += shortest[k];
    }

    mShortLevel[static_cast<std::size_t>(mNewest)] = sum / PolySpectrum::kBins;
    mLevel = std::max(loudest, mLevel - kLevelDecayBelPerSecond * static_cast<float>(mFrameSeconds));

    if (loudest < mThresholdBel - kQuietBel)
    {
        return false;
    }

    const auto back = [this](int frames) { return (mNewest + mDepth - frames) % mDepth; };

    // Every context frame against this frame's level: 0 at the model's range under it, 1 at it.
    // The level is held up off the spectrum's floor by that range, so that in a quiet passage an
    // empty bin still reads 0.
    const float perBel = 1.0f / std::max(0.1f, mModel.range);
    const float reference = std::max(mLevel, PolySpectrum::kFloor + mModel.range);

    for (std::size_t c = 0; c < contexts; ++c)
    {
        const float* grids = mHistory.data() + static_cast<std::size_t>(back(mModel.context[c])) * stride;
        float* out = mNormalised.data() + c * stride;

        for (std::size_t k = 0; k < stride; ++k)
        {
            out[k] = std::clamp((grids[k] - reference) * perBel + 1.0f, 0.0f, 1.0f);
        }
    }

    const float now = mShortLevel[static_cast<std::size_t>(mNewest)];
    const float globals[kGlobals] = {
        (mLevel + 4.0f) / 4.0f,
        std::clamp(now - mShortLevel[static_cast<std::size_t>(back(contexts > 1 ? mModel.context[1] : 0))], -1.0f,
                   1.0f),
        std::clamp(now - mShortLevel[static_cast<std::size_t>(back(mModel.context.back()))], -1.0f, 1.0f),
    };

    // 1. Each note's own reading.
    const auto& first = mModel.body.front();
    const auto width = static_cast<std::size_t>(first.outputs);
    const int summarySize = mModel.summary.outputs;

    for (int p = 0; p < kNotes; ++p)
    {
        float* a = mA.data();
        float* b = mB.data();
        const int* index = mIndex.data() + static_cast<std::size_t>(p) * mPerFrame;
        const float* column = mFirstColumns.data();
        std::copy_n(first.bias, width, a);

        for (std::size_t c = 0; c < contexts * channels; ++c)
        {
            const float* grid = mNormalised.data() + c * PolySpectrum::kBins;

            for (std::size_t j = 0; j < mPerFrame; ++j, column += width)
            {
                const float value = index[j] >= 0 ? grid[index[j]] : 0.0f;

                if (value != 0.0f)
                {
                    AddScaled(a, column, value, width);
                }
            }
        }

        const float tail[kGlobals + 1] = {globals[0], globals[1], globals[2],
                                          static_cast<float>(p) / static_cast<float>(kNotes - 1)};

        for (float value : tail)
        {
            AddScaled(a, column, value, width);
            column += width;
        }

        for (std::size_t o = 0; o < width; ++o)
        {
            a[o] = std::max(a[o], 0.0f);
        }

        for (std::size_t l = 1; l < mModel.body.size(); ++l)
        {
            const auto& layer = mModel.body[l];
            Apply(layer, a, b);

            for (int o = 0; o < layer.outputs; ++o)
            {
                b[o] = std::max(b[o], 0.0f);
            }

            std::swap(a, b);
        }

        float* reading = mReadings.data() + static_cast<std::size_t>(p) * mReading;
        std::copy_n(a, mReading, reading);

        if (summarySize > 0)
        {
            float* summary = mSummaries.data() + static_cast<std::size_t>(p) * summarySize;
            Apply(mModel.summary, reading, summary);

            for (int e = 0; e < summarySize; ++e)
            {
                summary[e] = std::tanh(summary[e]);
            }
        }
    }

    // 2 and 3. Neighbours, answers.
    for (int p = 0; p < kNotes; ++p)
    {
        const float* x = mReadings.data() + static_cast<std::size_t>(p) * mReading;

        if (summarySize > 0)
        {
            float* in = mA.data();

            for (int distance : mModel.neighbours)
            {
                const int q = p + distance;

                if (q >= 0 && q < kNotes)
                {
                    in = std::copy_n(mSummaries.data() + static_cast<std::size_t>(q) * summarySize, summarySize, in);
                }
                else
                {
                    in = std::fill_n(in, summarySize, 0.0f);
                }
            }

            std::copy_n(x, mReading, in);
            float* mixed = mB.data();
            Apply(mModel.mix, mA.data(), mixed);

            for (int o = 0; o < mModel.mix.outputs; ++o)
            {
                mixed[o] = std::max(mixed[o], 0.0f);
            }

            x = mixed;
        }

        float logits[2];
        Apply(mModel.head, x, logits);
        mAnswers[static_cast<std::size_t>(2 * p)] = Sigmoid(logits[0]);
        mAnswers[static_cast<std::size_t>(2 * p + 1)] = Sigmoid(logits[1]);
    }

    return true;
}
} // namespace guitarfx
