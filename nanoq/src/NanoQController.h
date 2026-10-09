#pragma once

// The QPlug controller: the host-visible parameters, the plugin's state, and the owner of the
// engine.
//
// The parameters are the engine's automation slots (the same ones MIDI learn and the JUCE
// products expose to a DAW): one normalised 0..1 parameter per slot, in the layout the JUCE
// adapter uses, which keeps the order append-only so a saved project does not rebind. A host
// change goes to the engine the way the JUCE adapter sends it: queued by the audio thread and
// applied at the top of the next block, under the DSP lock, or at once while audio is stopped.
//
// What the host keeps in a session is the engine's own state blob (PluginController::
// SerializeState) in the controller's extra state; the parameter values themselves are not
// saved, since the engine's state holds them.

#include "Engine.h"

#include <q_plug/controller.hpp>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct clap_host;

namespace nanoq
{
namespace q_plug = cycfi::q_plug;

class NanoQController final : public q_plug::controller
{
public:
    NanoQController();
    ~NanoQController() override;

    parameter_list parameters() const override;

    /// The engine's value for a slot, where the host reads it: any thread.
    double get_parameter(int index) const override;
    using q_plug::controller::get_parameter;

    /// The host moved a parameter: audio thread, or the main thread while audio is stopped.
    void set_parameter(int index, double value) override;
    using q_plug::controller::set_parameter;

    [[nodiscard]] Engine& engine()
    {
        return *mEngine;
    }

    /// Between activate and deactivate the audio thread owns the queue; outside it a change
    /// applies at once (a host flushes parameters into a plugin it has not activated).
    void SetAudioActive(bool active);

    /// Applies what the host queued, ahead of a block. `mayBlock` false on the audio thread:
    /// a busy DSP lock leaves the changes queued for the next block.
    void ApplyPendingParameters(bool mayBlock);

    /// Asks the host to read every parameter again: a state load changes the values the engine
    /// reports, and a CLAP host treats values that change on load without that request as a bug.
    /// Main thread.
    void RescanHostValues();

protected:
    void save_extra(json& j) const override;
    void load_extra(json const& j, std::uint32_t version) override;

private:
    void BuildParameters();

    std::unique_ptr<Engine> mEngine;
    const clap_host* mHost = nullptr; // the host this instance was created for, if CLAP

    std::deque<std::string> mNames; // the parameters point at these
    std::vector<parameter> mParams;
    std::vector<std::string> mSlotIds;

    std::mutex mPendingMutex;
    std::vector<std::pair<int, float>> mPending;
    std::atomic<bool> mAudioActive{false};
};
} // namespace nanoq
