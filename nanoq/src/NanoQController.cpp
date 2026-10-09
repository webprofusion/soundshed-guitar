#include "NanoQController.h"

#include "automation/AutomationTypes.h"

#include <clap/clap.h>

#include <nlohmann/json.hpp>

#include <algorithm>

namespace
{
// The host QPlug is creating a plugin for, noted by the CLAP entry (clap_entry.cpp) on the thread that
// builds it.
thread_local const clap_host_t* gCreatingHost = nullptr;
} // namespace

extern "C" void nanoq_set_creating_host(const clap_host_t* host)
{
    gCreatingHost = host;
}

namespace nanoq
{
NanoQController::NanoQController() : mEngine(std::make_unique<Engine>()), mHost(gCreatingHost)
{
    BuildParameters();
}

NanoQController::~NanoQController() = default;

void NanoQController::BuildParameters()
{
    // One parameter per automation slot, in the JUCE adapter's order (PluginProcessorAdapter::
    // registerAutomationParameters): the layout is append-only, because a DAW project binds
    // automation by position.
    //   - the default slots that shipped first, in their shipped order
    //   - custom slots, padded with reserved placeholders up to kMaxCustomSlots
    //   - default slots added since, after the reserved block
    auto& controller = mEngine->Controller();
    const auto slotIds = controller.GetAutomationSlotIds();

    std::vector<std::string> defaults;
    std::vector<std::string> customs;
    for (const auto& id : slotIds)
        (id.rfind("default.", 0) == 0 ? defaults : customs).push_back(id);

    std::vector<std::pair<std::string, std::string>> layout; // slot id, name
    const auto addSlot = [&](const std::string& id) {
        const auto* slot = controller.GetAutomationSlots().FindSlot(id);
        layout.emplace_back(id, slot != nullptr && !slot->label.empty() ? slot->label : id);
    };

    const int stable = std::min(guitarfx::kLayoutStableDefaultSlots, static_cast<int>(defaults.size()));
    for (int i = 0; i < stable; ++i)
        addSlot(defaults[static_cast<std::size_t>(i)]);
    for (const auto& id : customs)
        addSlot(id);
    for (int i = static_cast<int>(customs.size()); i < guitarfx::kMaxCustomSlots; ++i)
        layout.emplace_back("custom._reserved_" + std::to_string(i), "Reserved " + std::to_string(i));
    for (std::size_t i = static_cast<std::size_t>(stable); i < defaults.size(); ++i)
        addSlot(defaults[i]);

    mParams.reserve(layout.size());
    for (std::size_t i = 0; i < layout.size(); ++i)
    {
        mSlotIds.push_back(layout[i].first);
        mNames.push_back(layout[i].second);
        // Ids are positions (1-based: CLAP ids are free, and 0 reads as "none" in some hosts).
        mParams.push_back(parameter{static_cast<parameter::id_type>(i + 1), mNames.back().c_str(), 0.0}.range(0.0, 1.0).dont_save());
    }

    // Before the host has the plugin, so before any thread can read a value.
    controller.BindDawParameters(mSlotIds);
    mPending.reserve(mSlotIds.size() * 8);
}

NanoQController::parameter_list NanoQController::parameters() const
{
    return {mParams.data(), mParams.data() + mParams.size()};
}

double NanoQController::get_parameter(int index) const
{
    if (index < 0 || static_cast<std::size_t>(index) >= mSlotIds.size())
        return 0.0;
    return mEngine->Controller().GetDawParameterValue(index);
}

void NanoQController::set_parameter(int index, double value)
{
    if (index < 0 || static_cast<std::size_t>(index) >= mSlotIds.size())
        return;

    // Nothing drains the queue before the first activation or after deactivation, so a value
    // set then would never reach the chain and the engine would go on reporting the old one;
    // hosts do set values then (clap-validator's param-set tests). With no audio thread to
    // race, apply it now.
    if (!mAudioActive.load(std::memory_order_acquire))
    {
        mEngine->Controller().ApplyAutomationFromDAW(mSlotIds[static_cast<std::size_t>(index)], static_cast<float>(value));
        return;
    }

    std::lock_guard<std::mutex> lock(mPendingMutex);
    mPending.emplace_back(index, static_cast<float>(value));
}

void NanoQController::SetAudioActive(bool active)
{
    mAudioActive.store(active, std::memory_order_release);
}

void NanoQController::ApplyPendingParameters(bool mayBlock)
{
    // The queue stays locked while its changes apply (a few slot updates) and is cleared in
    // place, so the audio thread frees nothing. A busy DSP lock leaves the changes queued for
    // the next block rather than the audio thread waiting on another thread for it.
    std::lock_guard<std::mutex> lock(mPendingMutex);
    if (mPending.empty())
        return;
    if (mEngine->Controller().ApplyAutomationFromDAW(mPending, mSlotIds, mayBlock))
        mPending.clear();
}

void NanoQController::RescanHostValues()
{
    if (mHost == nullptr || mHost->get_extension == nullptr)
        return;

    const auto* params = static_cast<const clap_host_params_t*>(mHost->get_extension(mHost, CLAP_EXT_PARAMS));
    if (params != nullptr && params->rescan != nullptr)
        params->rescan(mHost, CLAP_PARAM_RESCAN_VALUES);
}

void NanoQController::save_extra(json& j) const
{
    // Any thread: the controller builds on the engine thread and hands requests from elsewhere
    // over to it (PluginController::SerializeState).
    j["engine"] = mEngine->Controller().SerializeState();
}

void NanoQController::load_extra(json const& j, std::uint32_t)
{
    const auto it = j.find("engine");
    if (it == j.end() || !it->is_string())
        return;

    // Restores on the engine thread, and hands calls from elsewhere over to it. If it could not
    // get to the restore in time, nothing has changed yet and it finishes later
    // (IPluginHost::NotifyDeferredStateRestored).
    if (mEngine->Controller().DeserializeState(it->get<std::string>()))
        RescanHostValues();
}
} // namespace nanoq
