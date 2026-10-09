#include "NanoQProcessor.h"

#include "NoDenormals.h"
#include "automation/AutomationTypes.h"

#include <algorithm>
#include <cstring>

namespace nanoq
{
NanoQProcessor::NanoQProcessor(NanoQController& ctl) : mCtl(ctl) {}

void NanoQProcessor::activate()
{
    auto& engine = mCtl.engine();
    engine.SetStreamInfo(static_cast<double>(sps()), static_cast<int>(max_frames()), 2, 2);

    // Main thread. Prepare builds the audio path, so it runs where the controller lives.
    const double rate = static_cast<double>(sps());
    const int block = static_cast<int>(max_frames());
    engine.Call([&] { engine.Controller().Prepare(rate, block); });
    mCtl.SetAudioActive(true);
}

void NanoQProcessor::deactivate()
{
    auto& engine = mCtl.engine();
    // The flag goes first so a host change racing this applies directly; anything still queued
    // was waiting for a block that is not coming.
    mCtl.SetAudioActive(false);
    mCtl.ApplyPendingParameters(true);
    engine.Call([&] { engine.Controller().Reset(); });
}

void NanoQProcessor::reset() {}

void NanoQProcessor::midi(cycfi::q::midi_1_0::raw_message msg, std::size_t time)
{
    guitarfx::MidiEvent ev;
    ev.status = static_cast<std::uint8_t>(msg.data & 0xFF);
    ev.data1 = static_cast<std::uint8_t>((msg.data >> 8) & 0xFF);
    ev.data2 = static_cast<std::uint8_t>((msg.data >> 16) & 0xFF);
    ev.sampleOffset = static_cast<int>(time);
    mCtl.engine().Controller().EnqueueMidi(ev);
}

void NanoQProcessor::midi(cycfi::q::midi_2_0::packet const& p, std::size_t time)
{
    // Channel voice messages only, narrowed to MIDI 1.0 for the engine: type 2 already is,
    // type 4 carries 16-bit velocities and 32-bit controller values.
    guitarfx::MidiEvent ev;
    ev.sampleOffset = static_cast<int>(time);

    if (p.message_type() == 0x2)
    {
        const auto w = p.word(0);
        ev.status = static_cast<std::uint8_t>((w >> 16) & 0xFF);
        ev.data1 = static_cast<std::uint8_t>((w >> 8) & 0x7F);
        ev.data2 = static_cast<std::uint8_t>(w & 0x7F);
    }
    else if (p.message_type() == 0x4)
    {
        const auto w0 = p.word(0);
        const auto w1 = p.word(1);
        const auto status = static_cast<std::uint8_t>((w0 >> 16) & 0xFF);
        ev.status = status;
        ev.data1 = static_cast<std::uint8_t>((w0 >> 8) & 0x7F);

        switch (status & 0xF0)
        {
        case 0x80: // note off, note on: velocity in the top 16 bits
        case 0x90:
            ev.data2 = static_cast<std::uint8_t>(std::min<std::uint32_t>(127, (w1 >> 16) >> 9));
            if ((status & 0xF0) == 0x90 && ev.data2 == 0)
                ev.data2 = 1; // a 16-bit velocity that rounds to 0 must stay a note on
            break;
        case 0xB0: // control change: 32-bit value
            ev.data2 = static_cast<std::uint8_t>(w1 >> 25);
            break;
        case 0xC0: // program change: the program is in the top byte of word 2
            ev.data1 = static_cast<std::uint8_t>((w1 >> 24) & 0x7F);
            ev.data2 = 0;
            break;
        default:
            return;
        }
    }
    else
    {
        return;
    }

    mCtl.engine().Controller().EnqueueMidi(ev);
}

void NanoQProcessor::process(in_channels const& in, out_channels const& out)
{
    // Subnormals decay out of recursive DSP; flush them to zero for this block.
    const ScopedNoDenormals noDenormals;

    // What the host changed since the last block applies first, under the DSP lock.
    mCtl.ApplyPendingParameters(false);

    const auto frames = static_cast<int>(in.frames.size());
    const auto outputs = out.size();
    const auto inputs = in.size();

    float* inPtrs[2] = {nullptr, nullptr};
    float* outPtrs[2] = {nullptr, nullptr};
    for (std::size_t ch = 0; ch < 2 && ch < inputs; ++ch)
        inPtrs[ch] = const_cast<float*>(in[ch].begin());
    for (std::size_t ch = 0; ch < 2 && ch < outputs; ++ch)
        outPtrs[ch] = out[ch].begin();

    if (outPtrs[0] == nullptr || inPtrs[0] == nullptr)
        return;

    if (!mCtl.engine().Controller().ProcessAudio(inPtrs, outPtrs, frames))
    {
        // The controller could not take the DSP lock for this block: silence, as in the JUCE adapter.
        for (std::size_t ch = 0; ch < 2 && ch < outputs; ++ch)
            std::memset(outPtrs[ch], 0, sizeof(float) * static_cast<std::size_t>(frames));
    }
}
} // namespace nanoq
