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
#include "controller/DemoPreviewService.h"
#include "controller/MetronomeService.h"
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
    mMetronome = std::make_unique<MetronomeService>(mHost, mAppSettings, mResourceRoot, sendToUI);
    mTelemetry = std::make_unique<TelemetryPublisher>(mHost, mPresetMixer, sendToUI);
    mSignalTest = std::make_unique<SignalTestService>(sendToUI);
    mTuner = std::make_unique<TunerService>(sendToUI);
    mDemoPreview = std::make_unique<DemoPreviewService>(mHost, mPresetMixer, mDSPMutex, mSignalTest->ActiveFlag(),
                                                        onError, sendToUI);
    mPracticeTool = std::make_unique<PracticeToolService>(mHost, mDSPMutex, onError, sendToUI);
}

PluginController::~PluginController()
{
    if (mPresetArchiveSession)
    {
        std::error_code ec;
        std::filesystem::remove_all(mPresetArchiveSession->rootPath, ec);
    }

    // Supersede any in-flight folder scans so detached workers bail out
    // promptly, then wait until every outstanding worker has finished before
    // our members are destroyed (workers call SendMessageToUI through mHost and
    // read mFolderScanGeneration, so they must not outlive us).
    mFolderScanGeneration.fetch_add(1, std::memory_order_relaxed);
    {
        std::unique_lock<std::mutex> lock(mFolderScanDoneMutex);
        mFolderScanDoneCv.wait(lock, [this]() { return mActiveFolderScans.load(std::memory_order_relaxed) == 0; });
    }

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
    mAutomationSlots.InitializeRegistry(
        mPresetMixer, [this]() { return static_cast<double>(mSetlistCursorIndex); },
        [this](int idx) { ApplySetlistPresetByIndex(idx); }, [this](int steps) { SetlistBankUp(steps); },
        [this](int steps) { SetlistBankDown(steps); }, [this]() { return GetSetlistLength(); },
        [this]() { return GetSetlistBankBase(); }, [this](int bankNumber) { SelectSetlistBank(bankNumber); },
        [this]() { return GetSetlistBankNumber(); }, [this](int index) { SelectSceneByIndex(index); },
        [this]() { return GetActiveSceneIndex(); });

    // Wire node-param-applied callback so the UI can reflect automation-driven changes.
    mAutomationSlots.SetOnNodeParamApplied(
        [this](const std::string& effectType, const std::string& paramId, double value) {
            // Resolve the concrete nodeId from the mixer's runtime graph.
            const auto found = mPresetMixer.FindFirstEnabledNodeOfType(effectType);

            if (!found)
            {
                return;
            }

            const auto& nodeId = found->second;

            // Patch mActivePreset so a subsequent state broadcast is consistent.
            if (mActivePreset)
            {
                auto* node = mActivePreset->graph.FindNode(nodeId);

                if (node)
                {
                    node->params[paramId] = value;
                }
            }

            // Queue a lightweight UI notification (safe from audio or UI thread).
            {
                std::lock_guard<std::mutex> lock(mPendingNodeParamMutex);
                mPendingNodeParamNotifies.push_back({nodeId, paramId, value});
            }
        });

    mAutomationSlots.SetOnNodeBypassApplied([this](const std::string& effectType, bool enabled) {
        if (!mActivePreset)
        {
            return;
        }

        const auto resolvedType = EffectRegistry::Instance().Resolve(effectType);
        bool updated = false;
        const auto applyBypassToGraph = [&](SignalGraph& graph) {
            for (auto& node : graph.nodes)
            {
                if (EffectRegistry::Instance().Resolve(node.type) != resolvedType)
                {
                    continue;
                }

                node.enabled = enabled;
                updated = true;
            }
        };

        // Keep both the active scene graph and mActivePreset->graph in sync.
        // BroadcastState calls SyncActivePresetSceneGraph(), which copies the
        // active scene graph into mActivePreset->graph.
        const std::string activeSceneId = GetResolvedActiveSceneId();

        if (auto* scene = FindPresetScene(*mActivePreset, activeSceneId))
        {
            applyBypassToGraph(scene->graph);
        }

        applyBypassToGraph(mActivePreset->graph);

        if (!updated)
        {
            return;
        }

        mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);

        if (!mActivePresetId.empty())
        {
            mMixerPresetJsonCache[mActivePresetId] = mActivePresetJson;
        }

        mPendingStateBroadcast = true;
    });

    // Load automation.json
    const auto automationData = LoadUiStorageJson("automation.json", nlohmann::json::object());

    if (!automationData.empty())
    {
        mAutomationSlots.LoadFromJson(automationData);
    }

    // Load setlist cursor/bankSize from setlists.json
    const auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    mSetlistBankSize = setlistsData.value("bankSize", 8);
    mSetlistCursorIndex = setlistsData.value("cursorIndex", 0);

    mNextSharedSyncPollAt = std::chrono::steady_clock::now();
}

void PluginController::Prepare(double sampleRate, int blockSize)
{
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.Prepare(sampleRate, blockSize);

    if (mPracticeTool)
    {
        mPracticeTool->Prepare(sampleRate, blockSize);
    }

    // Report initial latency to the host (e.g. IR cab partition size may be
    // known only after Prepare sets the sample rate).
    UpdateHostLatency();

    if (mHost.IsStandalone())
    {
        mMetronome->ResetTransport();
        mMetronome->RefreshClickSamples(sampleRate);
    }
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
    if (mRiffCapture.armed || mRiffCapture.active)
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
                mHost.NotifyStateChanged();
            }
        }
    }

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

            if (captured.has_value() && !slotId.empty())
            {
                const auto* slot = mAutomationSlots.FindSlot(slotId);
                const bool isDefault = slot && slot->isDefault;
                const auto label = slot ? std::optional<std::string>(slot->label) : std::nullopt;
                const auto address = slot ? std::optional<std::string>(slot->address) : std::nullopt;
                const auto nodeSelector = slot ? std::optional<std::string>(slot->nodeSelector) : std::nullopt;
                const auto keyMaps = slot ? std::optional<std::vector<KeyboardMap>>(slot->keyMaps) : std::nullopt;

                if (isDefault)
                {
                    mAutomationSlots.SetDefaultSlotOverrides(slotId, label, *captured, keyMaps);
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
        BroadcastState(StateScope::Full);
    }
    else if (mPendingPresetStateBroadcast)
    {
        mPendingPresetStateBroadcast = false;
        BroadcastState(StateScope::PresetOnly);
    }

    // Drain deferred node-param notifications (from MIDI/keyboard automation)
    {
        std::vector<PendingNodeParamNotify> notifies;
        {
            std::lock_guard<std::mutex> lock(mPendingNodeParamMutex);
            notifies = std::move(mPendingNodeParamNotifies);
            mPendingNodeParamNotifies.clear();
        }

        for (const auto& n : notifies)
        {
            nlohmann::json msg;
            msg["type"] = "signalPathNodeParamUpdated";
            msg["nodeId"] = n.nodeId;
            msg["key"] = n.paramKey;
            msg["value"] = n.value;
            SendMessageToUI(msg.dump());
        }
    }

    mControlSurface->PublishMidiLog();

    // Apply whatever the control surface parked for us. These all load presets,
    // which needs the DSP lock the audio thread was holding when it asked.
    {
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

void PluginController::HandleOpenAudioPreferencesRequest()
{
    mHost.OpenAudioPreferences();
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
    const int latency = mPresetMixer.GetTotalLatencySamples();

    if (latency == mLastReportedLatency)
    {
        return;
    }

    mLastReportedLatency = latency;
    mHost.NotifyLatencyChanged(latency);
}
} // namespace guitarfx
