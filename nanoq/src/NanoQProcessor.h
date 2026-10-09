#pragma once

#include "NanoQController.h"

#include <q_plug/processor.hpp>

namespace nanoq
{
/// The audio side: hands each block to the engine. Stereo in, stereo out, plus MIDI in.
class NanoQProcessor final : public q_plug::processor
{
public:
    explicit NanoQProcessor(NanoQController& ctl);

    channel_config channels() const override
    {
        return {2, 2};
    }

    void activate() override;
    void deactivate() override;
    void reset() override;
    bool has_midi_input() const override
    {
        return true;
    }

    void midi(cycfi::q::midi_1_0::raw_message msg, std::size_t time) override;
    void midi(cycfi::q::midi_2_0::packet const& p, std::size_t time) override;

    void process(in_channels const& in, out_channels const& out) override;

private:
    NanoQController& mCtl;
};
} // namespace nanoq
