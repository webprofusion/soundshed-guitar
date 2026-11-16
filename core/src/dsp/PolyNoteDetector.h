#pragma once

#include "dsp/FiniteCheck.h"
#include "dsp/PolyNoteModel.h"
#include "dsp/PolyPitch.h"
#include "dsp/PolySpectrum.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace guitarfx
{
/**
 * Real-time polyphonic note detection for guitar: which notes are sounding, several at once.
 *
 * Every frame of PolySpectrum (5.3 ms) each note from B1 to E6 is judged by one small network, the
 * same for every note, in three steps:
 *
 *  1. Its own reading. The level at the note's partials, and at the places a lower note that
 *     could be mistaken for it would have partials of its own (a third and a half of its
 *     frequency, and their odd multiples), in each window, now and a few frames ago; with the
 *     frame's level and how fast it is rising.
 *  2. Its neighbours. A summary of the same reading for the notes an octave, a twelfth, a fifth
 *     and a few semitones either side: a note whose partials all belong to a stronger note below
 *     is that note's overtone, not a note.
 *  3. Two answers: is this note sounding, and did it start within the last 50 ms.
 *
 * A note starts when its "started" answer crosses the start threshold, or when it has been
 * "sounding" for a while with no start seen (a hammer-on, a note that swelled in, a start the
 * network was not sure of). It starts again, as a re-pick, when "started" crosses a second time
 * having fallen back in between. It stops once "sounding" has stayed under the stop threshold for a
 * few frames.
 *
 * Levels are read against the loudest partial of the last second or so, so the detector does not
 * depend on the input's gain; under the threshold set with SetThresholdDb() nothing starts, and
 * 12 dB under it everything stops and no work is done. SetLowestNote() rules out notes below the
 * instrument's lowest string.
 *
 * The model is trained, not designed: dsp/PolyNoteModel.h. Prepare() allocates; Process() does
 * not. The per-frame work is in PolyNoteDetector.cpp, which is built optimised in Debug too
 * (cmake/PolyNotes): unoptimised it takes about as long as the audio lasts.
 */
class PolyNoteDetector
{
  public:
    static constexpr int kLowestNote = 35;
    static constexpr int kHighestNote = 88;
    static constexpr int kNotes = kHighestNote - kLowestNote + 1;

    struct Event
    {
        enum class Type : std::uint8_t
        {
            Start,
            Stop,
            /// The note's pitch moved (SetFollowPitch only): a bend, vibrato, or the string's own tuning.
            Bend
        };

        Type type = Type::Start;
        int note = 0;          ///< MIDI note number
        float velocity = 0.0f; ///< 0 to 1 (Start only): the input's peak level, linear
        int sampleOffset = 0;
        /// Bend: where the note really is, as a fractional MIDI note. Stop, with SetFollowPitch:
        /// where it was a little before it stopped, which its release should keep (as a note
        /// ends its reading wanders onto whatever rings beside it). Start, and without
        /// SetFollowPitch: the note itself.
        double pitch = 0.0;
    };

    /// The defaults are where notes found and notes right balance on annotated playing: see
    /// "Polyphonic" under Guitar to MIDI in docs/fx-library.md.
    struct Settings
    {
        float startThreshold = 0.9f;
        float rearmThreshold = 0.25f;
        float soundingThreshold = 0.8f;
        int soundingFrames = 4;
        float stopThreshold = 0.3f;
        int stopFrames = 4;
        double minRestartSeconds = 0.05;
    };

    void Prepare(double sampleRate, const PolyNoteModel& model);
    void Reset();
    void SetSettings(const Settings& settings);

    /// The level, in dBFS, the input's loudest partial has to reach for a note to start.
    void SetThresholdDb(double dB)
    {
        mThresholdBel = static_cast<float>(std::clamp(dB, -96.0, 0.0) / 20.0);
    }

    /// Notes below this MIDI note never start: the instrument's lowest string.
    void SetLowestNote(int note)
    {
        mLowestAllowed = std::clamp(note, kLowestNote, kHighestNote);
    }

    /// Follows each sounding note's exact pitch (dsp/PolyPitch.h), reported with its Start and in
    /// Bend events, and keeps a bent note going as the one note: see "Bends" in the .cpp. Off,
    /// pitches are whole notes and nothing is measured. Takes effect from the next note.
    void SetFollowPitch(bool follow)
    {
        mFollowPitch = follow;
    }

    /// Where `note` is sounding, as a fractional MIDI note; the note itself when it is not
    /// sounding or pitch is not followed.
    [[nodiscard]] double Pitch(int note) const
    {
        if (note < kLowestNote || note > kHighestNote)
        {
            return note;
        }

        const Note& n = mNotes[static_cast<std::size_t>(note - kLowestNote)];
        return n.on ? n.pitch : static_cast<double>(note);
    }

    /// Feeds `numSamples` of mono input, calling `sink(const Event&)` for each note that starts
    /// or stops, with the event's sampleOffset set to its index in `input`. A null input counts
    /// as silence.
    template <typename Sink> void Process(const float* input, int numSamples, Sink&& sink)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float raw = input ? input[i] : 0.0f;
            const float x = IsFinite(raw) ? raw : 0.0f;
            mPeak = std::max(mPeak * mPeakRelease, std::abs(x));

            if (mSpectrum.Push(x))
            {
                OnFrame(i, sink);
            }
        }
    }

    [[nodiscard]] bool IsSounding(int note) const
    {
        return note >= kLowestNote && note <= kHighestNote && mNotes[static_cast<std::size_t>(note - kLowestNote)].on;
    }

    [[nodiscard]] int SoundingCount() const
    {
        return mSounding;
    }

    /// The last frame's two answers for `note`, 0 to 1: for tests and probes.
    [[nodiscard]] float SoundingProbability(int note) const
    {
        return mNotes[static_cast<std::size_t>(note - kLowestNote)].sounding;
    }

    [[nodiscard]] float StartProbability(int note) const
    {
        return mNotes[static_cast<std::size_t>(note - kLowestNote)].started;
    }

    [[nodiscard]] int HopSamples() const
    {
        return mSpectrum.HopSamples();
    }

    /// Frames judged since Prepare() or Reset().
    [[nodiscard]] std::uint64_t FrameCount() const
    {
        return mSpectrum.FrameCount();
    }

  private:
    struct Note
    {
        bool on = false;
        bool armed = true;
        int high = 0;
        int low = 0;
        int age = 0;
        float sounding = 0.0f;
        float started = 0.0f;
        double pitch = 0.0;             ///< where it is, while on
        double sentPitch = 0.0;         ///< the pitch last reported
        double peakEnergy = 0.0;        ///< its partials' loudest, since it started
        std::array<float, 8> history{}; ///< its pitch over the last frames, by frame % 8
    };

    /// Pitch moves smaller than this are not reported.
    static constexpr double kBendReportSemitones = 0.01;

    /// Takes the spectrum's new frame and works out every note's two answers (mAnswers). False
    /// for a frame that is silence, which has none.
    [[nodiscard]] bool JudgeFrame();

    /// Measures every sounding note's pitch in the new frame (SetFollowPitch).
    void FollowPitches();

    /// How many frames back a stopping note's pitch is taken from (Event::pitch).
    static constexpr int kStopPitchFrames = 4;

    template <typename Sink> void OnFrame(int offset, Sink& sink)
    {
        if (!JudgeFrame())
        {
            // Quiet: nothing is sounding.
            for (int p = 0; p < kNotes; ++p)
            {
                if (mNotes[static_cast<std::size_t>(p)].on)
                {
                    Stop(p, offset, sink);
                }
            }

            mNotes = {};
            return;
        }

        const bool following = mFollowPitch && mSounding > 0;

        if (following)
        {
            FollowPitches();
        }

        const bool loudEnough = mLevel >= mThresholdBel;

        for (int p = 0; p < kNotes; ++p)
        {
            Decide(p, mAnswers[static_cast<std::size_t>(2 * p)], mAnswers[static_cast<std::size_t>(2 * p + 1)],
                   loudEnough, offset, sink);
        }

        if (!following)
        {
            return;
        }

        for (int p = 0; p < kNotes; ++p)
        {
            Note& note = mNotes[static_cast<std::size_t>(p)];

            if (note.on && note.age > 0 && std::abs(note.pitch - note.sentPitch) >= kBendReportSemitones)
            {
                note.sentPitch = note.pitch;
                Event event;
                event.type = Event::Type::Bend;
                event.note = kLowestNote + p;
                event.sampleOffset = offset;
                event.pitch = note.pitch;
                sink(static_cast<const Event&>(event));
            }
        }
    }

    template <typename Sink> void Decide(int p, float sounding, float started, bool loudEnough, int offset, Sink& sink)
    {
        Note& note = mNotes[static_cast<std::size_t>(p)];
        note.sounding = sounding;
        note.started = started;

        if (started < mSettings.rearmThreshold)
        {
            note.armed = true;
        }

        const bool fire = note.armed && started >= mSettings.startThreshold;

        if (fire)
        {
            note.armed = false;
        }

        if (!note.on)
        {
            note.high = sounding >= mSettings.soundingThreshold ? note.high + 1 : 0;

            if ((fire || note.high >= mSettings.soundingFrames) && loudEnough && kLowestNote + p >= mLowestAllowed)
            {
                Start(p, offset, sink);
            }

            return;
        }

        ++note.age;

        if (fire && note.age >= mMinRestartFrames)
        {
            Stop(p, offset, sink);
            Start(p, offset, sink);
            return;
        }

        note.low = sounding < mSettings.stopThreshold ? note.low + 1 : 0;

        if (note.low >= mSettings.stopFrames)
        {
            Stop(p, offset, sink);
        }
    }

    template <typename Sink> void Start(int p, int offset, Sink& sink)
    {
        Note& note = mNotes[static_cast<std::size_t>(p)];
        // It starts as the whole note: read in a window it has only just come into, the pitch is
        // no nearer, and the next few frames settle it.
        note.pitch = static_cast<double>(kLowestNote + p);
        note.sentPitch = note.pitch;
        note.peakEnergy = 0.0;
        note.history.fill(static_cast<float>(note.pitch));
        note.on = true;
        note.low = 0;
        note.high = 0;
        note.age = 0;
        ++mSounding;
        Event event;
        event.type = Event::Type::Start;
        event.note = kLowestNote + p;
        event.velocity = mPeak;
        event.sampleOffset = offset;
        event.pitch = note.pitch;
        sink(static_cast<const Event&>(event));
    }

    template <typename Sink> void Stop(int p, int offset, Sink& sink)
    {
        Note& note = mNotes[static_cast<std::size_t>(p)];
        note.on = false;
        note.high = 0;
        --mSounding;
        Event event;
        event.type = Event::Type::Stop;
        event.note = kLowestNote + p;
        event.sampleOffset = offset;
        event.pitch = mFollowPitch
                          ? note.history[static_cast<std::size_t>((mSpectrum.FrameCount() + 8 - kStopPitchFrames) % 8)]
                          : static_cast<double>(event.note);
        sink(static_cast<const Event&>(event));
    }

    PolyNoteModel mModel;
    PolySpectrum mSpectrum;
    Settings mSettings;
    double mFrameSeconds = 64.0 / 12000.0;
    int mMinRestartFrames = 10;
    int mDepth = 1;            ///< frames of history kept
    std::size_t mPerFrame = 0; ///< inputs a note reads from one grid
    int mReading = 0;          ///< size of a note's own reading
    int mNewest = 0;
    float mLevel = PolySpectrum::kFloor;
    float mThresholdBel = -5.0f;
    float mPeak = 0.0f;
    float mPeakRelease = 0.9995f;
    int mSounding = 0;
    int mLowestAllowed = kLowestNote;
    bool mFollowPitch = false;

    std::vector<float> mHistory;      ///< the model's grids of the last mDepth frames
    std::vector<float> mShortLevel;   ///< the short plain window's mean level, per frame of history
    std::vector<float> mNormalised;   ///< the grids the network reads this frame, against the level
    std::vector<int> mIndex;          ///< per note, the bin of each input; -1 for one off the grid
    std::vector<float> mFirstColumns; ///< the first layer's weights, a column per input
    std::vector<float> mA;
    std::vector<float> mB;
    std::vector<float> mReadings;
    std::vector<float> mSummaries;
    std::array<float, 2 * kNotes> mAnswers{};
    std::array<Note, kNotes> mNotes{};
};
} // namespace guitarfx
