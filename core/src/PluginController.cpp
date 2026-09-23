/**
 * PluginController.cpp — Framework-agnostic plugin orchestration.
 *
 * Shared business logic that was previously duplicated across host-framework
 * plugin entry points. Every host operation goes through the IPluginHost
 * interface, so nothing here knows which framework it is running under.
 *
 * This file holds the parts that belong to no single feature: construction and
 * lifecycle, the audio callback, the idle loop, and the handful of request
 * handlers too small to be an area of their own. The rest of the class
 * is defined alongside the feature it serves, in core/src/controller/
 * PluginController*.cpp — see docs/agent-quickstart.md for the map. Free
 * functions shared between those files live in core/src/controller/internal/,
 * in namespace guitarfx::controller_detail.
 */
#include "PluginController.h"
#include "MessageDispatcher.h"
#include "controller/ControlSurfaceQueue.h"
#include "controller/ControllerDisplayFeed.h"
#include "controller/DemoPreviewService.h"
#include "controller/HostStateRelay.h"
#include "controller/MetronomeService.h"
#include "controller/ResourceFolderScanner.h"
#include "controller/SignalTestService.h"
#include "controller/TelemetryPublisher.h"
#include "controller/TunerService.h"
#include "controller/PracticeToolService.h"
#include "controller/internal/ControllerUtils.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"

#include <iostream>
#include <utility>

#include "GuitarFXConfig.h"

using namespace guitarfx::controller_detail;

namespace guitarfx
{
// ════════════════════════════════════════════════════════════════════
// Construction / Lifecycle
// ════════════════════════════════════════════════════════════════════

PluginController::PluginController(IPluginHost& host) : mHost(host)
{
    RegisterAllEffects();

    // Every service publishes through these two. `this` outlives them all.
    const auto sendToUI = [this](const std::string& json) { SendMessageToUI(json); };
    const auto onError = [this](const std::string& msg, const std::string& detail) { ReportErrorToUI(msg, detail); };

    mControlSurface = std::make_unique<ControlSurfaceQueue>(sendToUI);
    mControllerDisplay = std::make_unique<ControllerDisplayFeed>();
    mMetronome = std::make_unique<MetronomeService>(mHost, mAppSettings, mResourceRoot, sendToUI);
    mTelemetry = std::make_unique<TelemetryPublisher>(mHost, mPresetMixer, sendToUI);
    mSignalTest = std::make_unique<SignalTestService>(sendToUI);
    mResourceFolderScanner = std::make_unique<ResourceFolderScanner>(sendToUI, mResourceLibrary);
    mResourceFolderScanner->RegisterMessageHandlers(mMessageHandlers);
    mTuner = std::make_unique<TunerService>(sendToUI, mPresetMixer, mDSPMutex);
    mTuner->RegisterMessageHandlers(mMessageHandlers);
    mDemoPreview = std::make_unique<DemoPreviewService>(mHost, mPresetMixer, mDSPMutex, mSignalTest->ActiveFlag(),
                                                        onError, sendToUI);
    mPracticeTool = std::make_unique<PracticeToolService>(mHost, mDSPMutex, onError, sendToUI);
    mPracticeTool->RegisterMessageHandlers(mMessageHandlers);
    mHostStateRelay = std::make_unique<HostStateRelay>(mHost);
}

PluginController::~PluginController()
{
    // Before anything else goes: a restore or program change the host stopped waiting for can
    // still be queued for the message thread, or running there.
    mHostStateRelay->Shutdown();

    // Release retired hosted processors while their callbacks' controller services still exist.
    mPresetMixer.CollectRetiredMainThread();

    if (mPresetArchiveSession)
    {
        std::error_code ec;
        std::filesystem::remove_all(mPresetArchiveSession->rootPath, ec);
    }

    // Supersede any in-flight folder scans and wait for their detached workers here,
    // rather than leaving it to the scanner's own destructor: the workers publish through
    // SendMessageToUI, so they must stop while the members it reaches into are still
    // alive, not part-way through member teardown.
    mResourceFolderScanner->Shutdown();

    // Close after the workers are done, since they can still write through it.
    // This checkpoints the WAL so the -wal file does not grow across sessions.
    // Deliberately not Store(): a controller torn down without ever touching
    // storage should not open a database just to close it again.
    mStore.Close();
}

void PluginController::Initialize()
{
    mResourceRoot = mHost.GetUserDataPath();
    mUserPresetsPath = mFileSystem.ResolvePresetDirectory() / "user";

    std::cout << "[Plugin] Initializing. Resource root: " << mResourceRoot.string() << std::endl;

    // Ensure essential directories exist on first launch
    [[maybe_unused]] const auto ensuredResourceRoot = mFileSystem.EnsureDirectory(mResourceRoot);
    [[maybe_unused]] const auto ensuredSettingsRoot =
        mFileSystem.EnsureDirectory(mFileSystem.ResolveSettingsDirectory());
    [[maybe_unused]] const auto ensuredUserPresets = mFileSystem.EnsureDirectory(mUserPresetsPath);
    [[maybe_unused]] const auto ensuredResources =
        mFileSystem.EnsureDirectory(mFileSystem.ResolveSettingsDirectory() / "resources");

    // Eagerly, so the migration log lands at startup rather than on first use.
    // Goes through Store() so the once-flag is consumed here and a later access
    // does not try to open again.
    (void)Store();

    mPresetMixer.SetResourceLibrary(&mResourceLibrary);

    // When hosted in a DAW the host controls the input configuration; disable
    // app-side mono folding/channel selection so the input is used as provided.
    mPresetMixer.SetHostControlledInput(!mHost.IsStandalone());

    LoadAppSettings();

    if (ApplySettingsToRuntime(SettingsApplyMode::kApplyAll))
    {
        SaveAppSettings();
    }

    if (!IsPresetArchiveSessionActive())
    {
        LoadResourceLibraries();
    }

    if (!IsPresetArchiveSessionActive())
    {
        LoadBlendLibrary();
    }

    LoadCustomEffectLibrary();
    LoadFactoryPresetArchives();
    LoadCompositeLibrary();
    LoadLayoutLibrary();
    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
        mRiffLibraryIndex = LoadRiffLibraryIndex();
    }
    LoadLastSessionState();
    ApplyInputModeSettingsFromAppSettings();

    // Initialize automation system
    mAutomationSlots.SetMixer(&mPresetMixer);
    mAutomationSlots.SetEffectRegistry(&EffectRegistry::Instance());
    // A setlist step from automation arrives under mDSPMutex (MIDI on the audio thread, a DAW
    // parameter, the UI), and loading its preset takes that lock, so it is parked for the message
    // thread (see DrainControlSurfaceRequests).
    mAutomationSlots.InitializeRegistry(
        mPresetMixer, [this]() { return static_cast<double>(mSetlistCursorIndex.load(std::memory_order_relaxed)); },
        [this](int idx) { mControlSurface->RequestSetlistPreset(idx); }, [this](int steps) { SetlistBankUp(steps); },
        [this](int steps) { SetlistBankDown(steps); }, [this]() { return GetSetlistLength(); },
        [this]() { return GetSetlistBankBase(); }, [this](int bankNumber) { SelectSetlistBank(bankNumber); },
        [this]() { return GetSetlistBankNumber(); }, [this](int index) { SelectSceneByIndex(index); },
        [this]() { return GetActiveSceneIndex(); });

    // Load automation.json
    const auto automationData = LoadUiStorageJson("automation.json", nlohmann::json::object());

    if (!automationData.empty())
    {
        ReplaceAutomationSlots(automationData, nullptr);
    }

    // Load setlist cursor/bankSize from setlists.json
    const auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    mSetlistBankSize = setlistsData.value("bankSize", 8);
    mSetlistCursorIndex = setlistsData.value("cursorIndex", 0);

    mNextSharedSyncPollAt = std::chrono::steady_clock::now();

    // Something to answer a host that asks from another thread before the message thread
    // has built anything (see SerializeState).
    RememberHostStateFromWorkingCopy();
}

void PluginController::Prepare(double sampleRate, int blockSize)
{
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        mPresetMixer.Prepare(sampleRate, blockSize);

        if (mPracticeTool)
        {
            mPracticeTool->Prepare(sampleRate, blockSize);
        }

        // A stream that has just started may be feeding a MIDI output the user has only now
        // picked (the standalone restarts its callbacks when the output changes), or a
        // controller that was power-cycled meanwhile. Its display may be blank either way.
        mControllerDisplay->RequestRefresh();

        if (mHost.IsStandalone())
        {
            mMetronome->ResetTransport();
            mMetronome->RefreshClickSamples(sampleRate);
        }
    }

    // Report initial latency to the host (e.g. IR cab partition size may be
    // known only after Prepare sets the sample rate).
    UpdateHostLatency();
}

void PluginController::Reset()
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.Reset();
    mMetronome->RequestReset();
}

// ════════════════════════════════════════════════════════════════════
// Audio processing
// ════════════════════════════════════════════════════════════════════

bool PluginController::ProcessAudio(float** inputs, float** outputs, int numSamples)
{
    // Try to acquire the DSP lock without blocking the audio thread.
    std::unique_lock<std::mutex> lock(mDSPMutex, std::try_to_lock);

    if (!lock.owns_lock())
    {
        return false; // Caller should output silence
    }

    ProcessAudioLocked(inputs, outputs, numSamples);
    return true;
}

void PluginController::ProcessAudioLocked(float** inputs, float** outputs, int numSamples)
{
    // Riff capture runs ahead of the chain: it records the dry input. The guard is
    // here rather than inside so a session that is not capturing — every ordinary
    // block — costs one predicted branch and no call.
    if (mRiffCapture && (mRiffCapture->armed || mRiffCapture->active))
    {
        ProcessRiffCaptureBlock(inputs, numSamples);
    }

    // Mix in demo audio preview if active
    if (mDemoPreview)
    {
        mDemoPreview->MixIntoInput(inputs, numSamples);
    }

    // Deactivate guidance for preview only once the preview has been active and then stopped.
    // This avoids a race where guidance is deactivated before DemoPreview has loaded the buffer.
    if (mMetronome->IsGuidanceForPreview() && mDemoPreview)
    {
        if (mDemoPreview->IsPreviewActive())
        {
            mMetronome->SetGuidancePreviewWasActive(true);
        }
        else if (mMetronome->GuidancePreviewWasActive())
        {
            DeactivateRiffGuidance(true);
            mMetronome->SetGuidancePreviewWasActive(false);
        }
    }

    mSignalTest->InjectInput(inputs, numSamples);

    // Push current tempo to any tempo-aware effect nodes
    mPresetMixer.SetTempo(GetEffectiveTempoBpm());

    // Main DSP processing
    mPresetMixer.Process(inputs, outputs, numSamples);

    // Add metronome click on top of processed audio (standalone only)
    mMetronome->Render(outputs, numSamples);

    // Mix in the local backing-track player, post-chain (like the
    // metronome) — it is not the guitar signal and must never be routed
    // through the amp/cab chain. Audio-thread-safe: pops from a lock-free
    // ring only, never blocks.
    if (mPracticeTool)
    {
        mPracticeTool->RenderPostChain(outputs, numSamples);
    }

    mSignalTest->CollectOutput(outputs, numSamples);
}

// ════════════════════════════════════════════════════════════════════
// UI message entry point
// ════════════════════════════════════════════════════════════════════

void PluginController::HandleUIMessage(const std::string& jsonMessage)
{
    try
    {
        const auto msg = nlohmann::json::parse(jsonMessage);

        if (msg.is_object() && msg.value("type", std::string{}) == "uiBootstrapError")
        {
            const auto source = msg.value("source", std::string{"unknown"});
            const auto details = msg.value("details", std::string{"(no details)"});
            AppendSessionLog("UI bootstrap error (" + source + "): " + details);
            return;
        }
    }
    catch (const std::exception&)
    {
        // Defer malformed payload handling to the dispatcher.
    }

    // Delegate to the MessageDispatcher which routes by message type.
    MessageDispatcher::Dispatch(*this, jsonMessage);
}

// ════════════════════════════════════════════════════════════════════
// Idle processing
// ════════════════════════════════════════════════════════════════════

void PluginController::OnIdle()
{
    // A restore or program change from a host thread normally arrives as a task of its own.
    // Applied here as well, ahead of everything below, so the idle refresh cannot replace the
    // pending restore's blob with the working copy it is about to overwrite.
    mHostStateRelay->ApplyQueued();

    mPresetMixer.CollectRetiredMainThread();
    ApplyDeferredNodeRebuilds();
    PollSharedSyncState();

    // Has the editor's reported size settled? Commit it as the remembered size and tell
    // the host its saved state is stale so the project keeps it. Deferred by one idle tick
    // so a drag costs one notification rather than one per frame — and, more importantly,
    // so a size that only ever existed while the window was being torn down is dropped
    // rather than remembered (see SetEditorWindowSize).
    if (mPendingEditorWindowSize.has_value())
    {
        if (mEditorWindowSizeChangedSinceIdle)
        {
            mEditorWindowSizeChangedSinceIdle = false;
        }
        else
        {
            mEditorWindowSize = *mPendingEditorWindowSize;
            mPendingEditorWindowSize.reset();

            if (!mHost.IsStandalone())
            {
                NotifyHostStateChanged();
            }
        }
    }

    SyncAutomationActivePreset();
    SyncControllerDisplay();

    // MIDI learn capture polling
    if (mAutomationSlots.IsMidiLearnArmed())
    {
        // The learn state (mMidiLearnSlotId/mMidiLearnCapture) is written by the
        // audio thread in HandleMidi under mDSPMutex, so read + commit it under the
        // same lock. Disk I/O and UI sends are done afterwards, outside the lock.
        nlohmann::json captureMsg;
        bool committed = false;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            const auto slotId = mAutomationSlots.GetMidiLearnSlot();
            auto captured = mAutomationSlots.PollMidiLearnCapture();

            // A slot removed while it listened (a per-preset mapping cleared mid-learn, say) has
            // nothing to commit to: committing would recreate it as an addressless custom slot.
            if (captured.has_value() && !slotId.empty() && mAutomationSlots.FindSlot(slotId))
            {
                const auto* slot = mAutomationSlots.FindSlot(slotId);
                const bool isDefault = slot && slot->isDefault;
                const auto presetId = slot ? slot->presetId : std::string{};
                const auto label = slot ? std::optional<std::string>(slot->label) : std::nullopt;
                const auto address = slot ? std::optional<std::string>(slot->address) : std::nullopt;
                const auto nodeSelector = slot ? std::optional<std::string>(slot->nodeSelector) : std::nullopt;
                const auto keyMaps = slot ? std::optional<std::vector<KeyboardMap>>(slot->keyMaps) : std::nullopt;

                if (isDefault)
                {
                    mAutomationSlots.SetDefaultSlotOverrides(slotId, label, *captured, keyMaps);
                }
                else if (!presetId.empty())
                {
                    mAutomationSlots.SetPresetSlot(slotId, presetId, label, address, *captured);
                }
                else
                {
                    mAutomationSlots.SetCustomSlot(slotId, label, address, nodeSelector, *captured, keyMaps);
                }

                captureMsg["type"] = "midiLearnCapture";
                captureMsg["slotId"] = slotId;
                captureMsg["eventType"] = static_cast<int>(captured->eventType);
                captureMsg["channel"] = captured->channel;
                captureMsg["controller"] = captured->controller;
                committed = true;
            }
        }

        if (committed)
        {
            SaveUiStorageJson("automation.json", mAutomationSlots.SaveToJson());
            SendMessageToUI(captureMsg.dump());
            HandleGetAutomationRequest();
        }
    }

    // Broadcast pending state. A full request supersedes any preset-only request queued
    // alongside it — the full payload is a superset.
    if (mPendingStateBroadcast)
    {
        mPendingStateBroadcast = false;
        mPendingPresetStateBroadcast = false;
        mHostStateRelay->MarkStale();
        BroadcastState(StateScope::Full);
    }
    else if (mPendingPresetStateBroadcast)
    {
        mPendingPresetStateBroadcast = false;
        mHostStateRelay->MarkStale();
        BroadcastState(StateScope::PresetOnly);
    }

    // A change the UI has just been told about is one a host may ask for from another thread,
    // at a moment the message thread cannot build it (see SerializeState).
    if (mHostStateRelay->TakeRefreshDue(std::chrono::steady_clock::now()))
    {
        RememberHostStateFromWorkingCopy();
    }

    RefreshAutomationBindings();

    // Fold the node changes MIDI, keyboard and DAW automation made into the working copy, which
    // only this thread may change, then tell the UI. The latest for each node parameter and each
    // bypassed type, however long since the last tick (see NodeChangeQueue).
    {
        std::vector<NodeChangeQueue::Change> changes;
        mAutomationSlots.TakeNodeChanges(
            [&changes](NodeChangeQueue::Change&& change) { changes.push_back(std::move(change)); });

        if (const auto dropped = mAutomationSlots.TakeDroppedNodeChangeCount(); dropped > 0)
        {
            AppendSessionLog("[Automation] " + std::to_string(dropped) +
                             " node changes arrived with no room to report them; the editor may show old values");
        }

        if (mActivePreset)
        {
            for (const auto& change : changes)
            {
                if (!change.nodeId)
                {
                    continue; // A bypass, below
                }

                if (auto* node = mActivePreset->graph.FindNode(*change.nodeId))
                {
                    node->params[change.binding->paramId] = change.value;
                }
            }

            bool bypassChanged = false;

            for (const auto& change : changes)
            {
                if (change.nodeId)
                {
                    continue;
                }

                const bool enabled = change.value != 0.0;
                const auto& resolvedType = change.binding->effectType;
                const auto applyBypassToGraph = [&](SignalGraph& graph) {
                    for (auto& node : graph.nodes)
                    {
                        if (EffectRegistry::Instance().Resolve(node.type) == resolvedType)
                        {
                            node.enabled = enabled;
                            bypassChanged = true;
                        }
                    }
                };

                // Keep both the active scene graph and mActivePreset->graph in sync.
                // BroadcastState calls SyncActivePresetSceneGraph(), which copies the
                // active scene graph into mActivePreset->graph.
                if (auto* scene = FindPresetScene(*mActivePreset, GetResolvedActiveSceneId()))
                {
                    applyBypassToGraph(scene->graph);
                }

                applyBypassToGraph(mActivePreset->graph);
            }

            if (bypassChanged)
            {
                mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);

                if (!mActivePresetId.empty())
                {
                    mMixerPresetJsonCache[mActivePresetId] = mActivePresetJson;
                }

                mPendingStateBroadcast = true;
            }
        }

        for (const auto& change : changes)
        {
            if (!change.nodeId)
            {
                continue;
            }

            nlohmann::json msg;
            msg["type"] = "signalPathNodeParamUpdated";
            msg["nodeId"] = *change.nodeId;
            msg["key"] = change.binding->paramId;
            msg["value"] = change.value;
            SendMessageToUI(msg.dump());
        }
    }

    mControlSurface->PublishMidiLog();

    DrainControlSurfaceRequests();

    mSignalTest->OnIdle();

    mTuner->OnIdle();

    mTelemetry->OnIdle();

    mMetronome->OnIdle(mUIReady && mTelemetry->IsUiVisible());

    if (mDemoPreview)
    {
        mDemoPreview->OnIdle();
    }

    if (mPracticeTool)
    {
        mPracticeToolUpdateCounter++;

        if (mPracticeToolUpdateCounter >= 60 / kPracticeToolRateHz)
        {
            mPracticeToolUpdateCounter = 0;
            mPracticeTool->OnIdle();
        }
    }
}

void PluginController::DrainControlSurfaceRequests()
{
    // Polled off the plugin's own timer, so the common case, nothing parked, costs an atomic load.
    if (!mControlSurface->HasPending())
    {
        return;
    }

    // Anything the host queued earlier goes first, a restore especially: it would replace the
    // preset a parked step or scene switch is about to change.
    mHostStateRelay->ApplyQueued();

    // These all load presets or rewrite the setlist, which needs the DSP lock the audio thread
    // was holding when it asked.
    const auto pending = mControlSurface->TakePending();

    if (pending.setlistPresetIndex.has_value())
    {
        ApplySetlistPresetByIndexDirect(*pending.setlistPresetIndex);
    }

    if (pending.setlistBankDelta.has_value())
    {
        SetlistBankChangeDirect(*pending.setlistBankDelta);
    }

    if (pending.setlistBankSelect.has_value())
    {
        SelectSetlistBankDirect(*pending.setlistBankSelect);
    }

    if (pending.sceneIndex.has_value())
    {
        SelectSceneByIndexDirect(*pending.sceneIndex);
    }
}

void PluginController::OnWebContentLoaded()
{
    mUIReady = true;
    mPendingStateBroadcast = true;

    // The UI may not be ready when Initialize() loads/sends the layout library.
    // Resend here so custom layouts are available immediately after startup.
    LoadLayoutLibrary();
}

// ════════════════════════════════════════════════════════════════════
// Messaging helpers
// ════════════════════════════════════════════════════════════════════

void PluginController::SendMessageToUI(const std::string& jsonMessage)
{
    mHost.SendMessageToUI(jsonMessage);
}

void PluginController::ReportErrorToUI(const std::string& message, const std::string& detail)
{
    nlohmann::json msg;
    msg["type"] = "error";
    msg["message"] = message;

    if (!detail.empty())
    {
        msg["detail"] = detail;
    }

    SendMessageToUI(msg.dump());
}

// ════════════════════════════════════════════════════════════════════
// Request handlers
// ════════════════════════════════════════════════════════════════════

void PluginController::HandleStateRequest()
{
    mPendingStateBroadcast = true;
}

void PluginController::HandleAudioDeviceRequest(const nlohmann::json& payload)
{
    // The devices belong to the host framework, so the whole request goes to it.
    mHost.HandleAudioDeviceRequest(payload.dump());
}

void PluginController::HandleGetEffectCatalogRequest()
{
    SendEffectCatalogToUI();
}

void PluginController::HandleGetPresetListRequest()
{
    SendPresetListToUI();
}

void PluginController::HandleGetThemeRequest()
{
    std::string theme = "dark";

    const auto appThemeIt = mAppSettings.find("theme");

    if (appThemeIt != mAppSettings.end() && appThemeIt->is_string())
    {
        theme = appThemeIt->get<std::string>();
    }

    nlohmann::json msg;
    msg["type"] = "theme";
    msg["theme"] = theme;
    SendMessageToUI(msg.dump());
}

void PluginController::HandleSetThemeRequest(const nlohmann::json& payload)
{
    mAppSettings["theme"] = payload.value("theme", "dark");
    SaveAppSettings();
}

void PluginController::HandleGetAppInfoRequest()
{
    nlohmann::json msg;
    msg["type"] = "appInfo";
    msg["version"] = GUITARFX_APP_VERSION;

#if defined(_WIN32)
    msg["os"] = "Windows";
#elif defined(__APPLE__)
    msg["os"] = "macOS";
#elif defined(__linux__)
    msg["os"] = "Linux";
#else
    msg["os"] = "Unknown";
#endif

#if defined(__x86_64__) || defined(_M_X64)
    msg["cpu"] = "x64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    msg["cpu"] = "arm64";
#else
    msg["cpu"] = "Unknown";
#endif

    SendMessageToUI(msg.dump());
}

// ════════════════════════════════════════════════════════════════════
// Internal helpers
// ════════════════════════════════════════════════════════════════════

void PluginController::UpdateHostLatency()
{
    // A host thread is waiting on this change and may hold what the host needs to hear it.
    if (mHost.IsMessageThread() && mHostCallsHeld)
    {
        mHeldLatencyReport = true;
        return;
    }

    int latency = 0;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);
        latency = mPresetMixer.GetTotalLatencySamples();

        if (latency == mLastReportedLatency)
        {
            return;
        }

        mLastReportedLatency = latency;
    }

    mHost.NotifyLatencyChanged(latency);
}
} // namespace guitarfx
