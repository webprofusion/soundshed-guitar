/**
 * Intonation.cpp - The tuner's intonation check (see Intonation.h; tunerIntonation.ts is the web
 * UI's copy).
 */

#include "uiclient/Intonation.h"

#include "dsp/FiniteCheck.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace guitarfx::uiclient::intonation
{
namespace
{
constexpr double kNewNoteSemitones = 0.5;

const char* const kSaddleSharp = "Move this string's saddle further from the fretboard (lengthens the string).";
const char* const kSaddleFlat = "Move this string's saddle closer to the fretboard (shortens the string).";
const char* const kNutHigh = "The nut slot may be cut too high: fretting near the nut stretches the string sharp. "
                             "Fret lightly to rule out finger pressure.";

bool Fine(double cents)
{
    const auto q = QualityOf(cents);
    return q == Quality::Excellent || q == Quality::Good;
}

double Median(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    const auto mid = values.size() / 2;
    return values.size() % 2 ? values[mid] : (values[mid - 1] + values[mid]) / 2.0;
}
} // namespace

Quality QualityOf(double cents)
{
    const double off = std::abs(cents);

    if (off <= 2.0)
    {
        return Quality::Excellent;
    }

    if (off <= 5.0)
    {
        return Quality::Good;
    }

    return off <= 10.0 ? Quality::Fair : Quality::Poor;
}

const char* QualityLabel(Quality quality)
{
    switch (quality)
    {
    case Quality::Excellent:
        return "Excellent";
    case Quality::Good:
        return "Good";
    case Quality::Fair:
        return "Fair";
    case Quality::Poor:
        return "Poor";
    }

    return "";
}

std::optional<FretReading> ReadFret(double openFrequency, double frequency)
{
    if (!(openFrequency > 0.0) || !(frequency > 0.0) || !IsFinite(openFrequency) || !IsFinite(frequency))
    {
        return std::nullopt;
    }

    const double semitones = 12.0 * std::log2(frequency / openFrequency);
    const int fret = static_cast<int>(std::lround(semitones));

    if (fret < 0 || fret > kMaxFret)
    {
        return std::nullopt;
    }

    return FretReading{fret, (semitones - fret) * 100.0, openFrequency * std::pow(2.0, fret / 12.0)};
}

NearestNote Nearest(double frequency, double referenceFrequency)
{
    static constexpr std::array<const char*, 12> kNames = {"C",  "C#", "D",  "D#", "E",  "F",
                                                           "F#", "G",  "G#", "A",  "A#", "B"};

    if (!(frequency > 0.0) || !(referenceFrequency > 0.0))
    {
        return {"-", 0.0};
    }

    const double midiFloat = 69.0 + 12.0 * std::log2(frequency / referenceFrequency);
    const long midi = std::lround(midiFloat);
    const long octave = static_cast<long>(std::floor(static_cast<double>(midi) / 12.0)) - 1;
    return {std::string(kNames[static_cast<std::size_t>(((midi % 12) + 12) % 12)]) + std::to_string(octave),
            (midiFloat - static_cast<double>(midi)) * 100.0};
}

std::string Advice(int fret, double cents)
{
    if (Fine(cents))
    {
        return {};
    }

    const bool sharp = cents > 0.0;

    if (fret == 0)
    {
        return "The open string has drifted: retune it and set it again.";
    }

    if (fret <= kNutFrets)
    {
        return sharp ? kNutHigh : "Flat this close to the nut: check the open string is still in tune.";
    }

    if (fret < kSaddleFrets)
    {
        return sharp ? "Sharp: fret lightly, then check the 12th fret to see whether the saddle needs moving."
                     : "Flat: check the 12th fret to see whether the saddle needs moving.";
    }

    return sharp ? kSaddleSharp : kSaddleFlat;
}

std::vector<std::string> StringAdvice(const std::vector<Fret>& frets)
{
    std::vector<std::string> out;
    const Fret* saddle = nullptr;

    for (const auto& f : frets)
    {
        if (f.fret >= kSaddleFrets && (!saddle || std::abs(f.fret - 12) < std::abs(saddle->fret - 12)))
        {
            saddle = &f;
        }
    }

    if (saddle && !Fine(saddle->cents))
    {
        out.emplace_back(saddle->cents > 0.0 ? kSaddleSharp : kSaddleFlat);
    }
    else if (saddle)
    {
        out.push_back("Saddle is right: fret " + std::to_string(saddle->fret) + " is within " +
                      std::to_string(static_cast<int>(std::ceil(std::abs(saddle->cents)))) + "\xC2\xA2.");
    }

    double lowSum = 0.0;
    int lowCount = 0;

    for (const auto& f : frets)
    {
        if (f.fret >= 1 && f.fret <= kNutFrets)
        {
            lowSum += f.cents;
            ++lowCount;
        }
    }

    if (lowCount > 0)
    {
        const double lowCents = lowSum / lowCount;
        const double highCents = saddle ? saddle->cents : 0.0;

        if (lowCents > 5.0 && lowCents - highCents > 4.0)
        {
            out.emplace_back(kNutHigh);
        }
    }

    return out;
}

std::string FormatCents(double cents)
{
    const double rounded = std::round(cents * 10.0) / 10.0;
    // "+", U+2212 minus, U+00B1 plus-minus; then U+00A2 cent sign.
    const char* sign = rounded > 0.0 ? "+" : rounded < 0.0 ? "\xE2\x88\x92" : "\xC2\xB1";
    char text[32];
    std::snprintf(text, sizeof(text), "%s%.1f\xC2\xA2", sign, std::abs(rounded));
    return text;
}

void NoteSettler::Reset()
{
    mSeen = 0;
    mLastSemitones.reset();
    mLastLevelDb.reset();
    mWindow.clear();
}

std::optional<SteadyNote> NoteSettler::Push(const Reading& reading)
{
    if (!reading.detected || !(reading.frequency > 0.0) || !IsFinite(reading.frequency))
    {
        Reset();
        return std::nullopt;
    }

    const double semitones = 12.0 * std::log2(reading.frequency / 440.0);
    const bool jumped = mLastSemitones && std::abs(semitones - *mLastSemitones) > kNewNoteSemitones;
    const bool plucked = reading.levelDb && mLastLevelDb && *reading.levelDb - *mLastLevelDb >= kNewPluckDb;

    if (jumped || plucked)
    {
        Reset();
    }

    mLastSemitones = semitones;
    mLastLevelDb = reading.levelDb;

    if (++mSeen <= kAttackReadings)
    {
        return std::nullopt;
    }

    mWindow.push_back(semitones);

    while (mWindow.size() > kSettleWindow)
    {
        mWindow.pop_front();
    }

    if (mWindow.size() < kSettleReadings)
    {
        return std::nullopt;
    }

    const double centre = Median({mWindow.begin(), mWindow.end()});
    double sum = 0.0;
    std::size_t steady = 0;

    for (const double s : mWindow)
    {
        if (std::abs(s - centre) * 100.0 < kSettleSpreadCents)
        {
            sum += s;
            ++steady;
        }
    }

    if (static_cast<double>(steady) < std::ceil(static_cast<double>(mWindow.size()) * kSettleSteadyShare))
    {
        return std::nullopt;
    }

    return SteadyNote{440.0 * std::pow(2.0, (sum / static_cast<double>(steady)) / 12.0), steady};
}

void IntonationTest::SetReferenceFrequency(double hz)
{
    if (hz > 0.0)
    {
        mReferenceFrequency = hz;
    }
}

void IntonationTest::Push(const Reading& reading)
{
    const auto steady = mSettler.Push(reading);

    if (!steady)
    {
        // Between notes the last measurement stays up, so it can be read after the note dies.
        if (reading.detected)
        {
            mOffString = false;
        }

        return;
    }

    mLastSteady = steady;
    mMeasurement.reset();
    mOffString = false;

    if (!mCurrent)
    {
        return;
    }

    auto string = std::find_if(mChecked.begin(), mChecked.end(), [&](const String& s) { return s.name == *mCurrent; });

    if (string == mChecked.end())
    {
        return;
    }

    const auto fret = ReadFret(string->openFrequency, steady->frequency);

    if (!fret)
    {
        mOffString = true;
        return;
    }

    Measurement m;
    m.fret = fret->fret;
    m.cents = fret->cents;
    m.quality = QualityOf(fret->cents);
    m.frequency = steady->frequency;
    m.targetFrequency = fret->targetFrequency;
    mMeasurement = m;

    if (m.fret > 0)
    {
        const Fret entry{m.fret, m.cents, m.quality};
        auto existing =
            std::find_if(string->frets.begin(), string->frets.end(), [&](const Fret& f) { return f.fret == m.fret; });

        if (existing != string->frets.end())
        {
            *existing = entry;
        }
        else
        {
            string->frets.push_back(entry);
            std::sort(string->frets.begin(), string->frets.end(),
                      [](const Fret& a, const Fret& b) { return a.fret < b.fret; });
        }
    }
}

bool IntonationTest::SetOpenString()
{
    if (!mLastSteady)
    {
        return false;
    }

    const double frequency = mLastSteady->frequency;
    const auto name = Nearest(frequency, mReferenceFrequency).name;
    // Setting a string again (retuned, or a saddle moved) starts its frets over: the old ones were
    // measured against the old pitch.
    std::erase_if(mChecked, [&](const String& s) { return s.name == name; });
    mChecked.push_back({name, frequency, {}});
    std::sort(mChecked.begin(), mChecked.end(),
              [](const String& a, const String& b) { return a.openFrequency < b.openFrequency; });
    mCurrent = name;
    mMeasurement.reset();
    mOffString = false;
    return true;
}

void IntonationTest::NewString()
{
    mCurrent.reset();
    mMeasurement.reset();
    mOffString = false;
}

void IntonationTest::Clear()
{
    mChecked.clear();
    mCurrent.reset();
    mMeasurement.reset();
    mOffString = false;
    mLastSteady.reset();
    mSettler.Reset();
}

void IntonationTest::ResetReadings()
{
    mSettler.Reset();
    mMeasurement.reset();
    mOffString = false;
}

View IntonationTest::GetView() const
{
    View view;
    view.strings = mChecked;
    view.measurement = mMeasurement;
    view.offString = mOffString;

    if (mLastSteady)
    {
        view.candidate = Nearest(mLastSteady->frequency, mReferenceFrequency);
    }

    if (mCurrent)
    {
        for (const auto& s : mChecked)
        {
            if (s.name == *mCurrent)
            {
                view.hasString = true;
                view.string = s;
            }
        }
    }

    return view;
}

std::optional<FretReading> IntonationTest::Readout(double frequency) const
{
    if (!mCurrent)
    {
        return std::nullopt;
    }

    for (const auto& s : mChecked)
    {
        if (s.name == *mCurrent)
        {
            return ReadFret(s.openFrequency, frequency);
        }
    }

    return std::nullopt;
}
} // namespace guitarfx::uiclient::intonation
