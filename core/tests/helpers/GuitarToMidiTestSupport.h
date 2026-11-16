#pragma once

/**
 * @file GuitarToMidiTestSupport.h
 * @brief What the Guitar to MIDI tests share: playing audio through the effect and a note player,
 *        as a Plugin Host downstream would hear it, and reading the MIDI that comes out.
 */

#include <cstddef>
#include <string>
#include <vector>

#include "GuitarPhraseSynth.h"
#include "dsp/NoteEvents.h"
#include "dsp/effects/GuitarToMidiEffect.h"

namespace guitarfx::test::g2m
{
inline constexpr double kRate = 48000.0;
inline constexpr int kBlock = 128;

struct Midi
{
    int status = 0;
    int data1 = 0;
    int data2 = 0;
    std::size_t sample = 0;

    [[nodiscard]] int Type() const
    {
        return status & 0xF0;
    }

    [[nodiscard]] int Channel() const
    {
        return status & 0x0F;
    }

    [[nodiscard]] int Bend() const
    {
        return (data1 | (data2 << 7)) - 8192;
    }
};

inline std::string Notes(const std::vector<Midi>& midi)
{
    std::string text;

    for (const auto& m : midi)
    {
        if (m.Type() == 0x90)
        {
            text += (text.empty() ? "" : " ") + std::to_string(m.data1);
        }
    }

    return text;
}

inline std::vector<int> NoteOns(const std::vector<Midi>& midi)
{
    std::vector<int> notes;

    for (const auto& m : midi)
    {
        if (m.Type() == 0x90)
        {
            notes.push_back(m.data1);
        }
    }

    return notes;
}

/// Plays `audio` through `fx` and a note player, the way a Plugin Host downstream would hear it.
/// `between` runs before each block, for changing settings mid-phrase.
template <typename Between>
std::vector<Midi> Play(GuitarToMidiEffect& fx, NotePlayer& player, const std::vector<float>& audio, Between&& between,
                       bool stereo = false)
{
    std::vector<Midi> midi;
    std::vector<float> outL(kBlock), outR(kBlock);

    for (std::size_t start = 0; start + kBlock <= audio.size(); start += kBlock)
    {
        between(start);
        std::vector<float> block(audio.begin() + static_cast<std::ptrdiff_t>(start),
                                 audio.begin() + static_cast<std::ptrdiff_t>(start + kBlock));

        if (stereo)
        {
            float* in[2] = {block.data(), block.data()};
            float* out[2] = {outL.data(), outR.data()};
            fx.Process(in, out, kBlock);
        }
        else
        {
            fx.ProcessMono(block.data(), outL.data(), kBlock);
        }

        const NoteBlock* sources[1] = {fx.GetNoteOutput()};

        for (const auto& m : player.Build(sources, kBlock))
        {
            midi.push_back({m.status, m.data1, m.data2, start + static_cast<std::size_t>(m.sampleOffset)});
        }

        player.Commit();
    }

    return midi;
}

inline std::vector<Midi> Play(GuitarToMidiEffect& fx, NotePlayer& player, const std::vector<float>& audio,
                              bool stereo = false)
{
    return Play(fx, player, audio, [](std::size_t) {}, stereo);
}

inline bool AnythingSounding(const NotePlayer& player)
{
    for (int channel = 0; channel < 16; ++channel)
    {
        for (int note = 0; note < 128; ++note)
        {
            if (player.IsSounding(channel, note))
            {
                return true;
            }
        }
    }

    return false;
}

inline PhraseNote Note(double start, double length, int midi, bool muted = true)
{
    PhraseNote n;
    n.start = start;
    n.length = length;
    n.hz = NoteHz(midi);
    n.peak = 0.25;
    n.muted = muted;
    return n;
}
} // namespace guitarfx::test::g2m
