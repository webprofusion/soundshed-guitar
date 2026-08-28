/**
 * PluginController.cpp — Framework-agnostic plugin orchestration.
 *
 * Shared business logic that was previously duplicated across host-framework
 * plugin entry points. Every host operation goes through the IPluginHost
 * interface, so nothing here knows which framework it is running under.
 *
 * This file holds the parts that belong to no single feature: construction and
 * lifecycle, the audio callback, state (de)serialisation, the idle loop, and
 * the small request handlers with nowhere better to live. The rest of the class
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
#include "controller/internal/HostedPluginSupport.h"
#include "controller/internal/NamResourceMetadata.h"
#include "controller/internal/OfflineRenderSupport.h"
#include "controller/internal/SettingsKeys.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/effects/BuiltinEffects.h"
#include "dsp/effects/NAMSlimmableSettings.h"
#if defined(GUITARFX_ENABLE_WASM_EFFECTS)
    #include "dsp/effects/WasmEffect.h"
#endif
#include "presets/CompositePresetTypes.h"
#include "util/PathEncoding.h"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <cctype>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <unordered_set>

#include "GuitarFXConfig.h"
#include "NAM/get_dsp.h"

// The controller's file-local helpers now live in controller/internal/. They keep the
// same unqualified names here so call sites read the same as before the split.
using namespace guitarfx::controller_detail;

namespace guitarfx
{
// ════════════════════════════════════════════════════════════════════
// Construction / Lifecycle
// ════════════════════════════════════════════════════════════════════

PluginController::PluginController(IPluginHost& host) : mHost(host)
{
    RegisterAllEffects();
    mControlSurface =
        std::make_unique<ControlSurfaceQueue>([this](const std::string& jsonMessage) { SendMessageToUI(jsonMessage); });
    mMetronome = std::make_unique<MetronomeService>(mHost, mAppSettings, mResourceRoot);
    mTelemetry = std::make_unique<TelemetryPublisher>(
        mHost, mPresetMixer, [this](const std::string& jsonMessage) { SendMessageToUI(jsonMessage); });
    mSignalTest =
        std::make_unique<SignalTestService>([this](const std::string& jsonMessage) { SendMessageToUI(jsonMessage); });
    mTuner = std::make_unique<TunerService>([this](const std::string& jsonMessage) { SendMessageToUI(jsonMessage); });
    mDemoPreview = std::make_unique<DemoPreviewService>(
        mHost, mPresetMixer, mDSPMutex, mSignalTest->ActiveFlag(),
        [this](const std::string& message, const std::string& detail) { ReportErrorToUI(message, detail); },
        [this](const std::string& jsonMessage) { SendMessageToUI(jsonMessage); });
    mPracticeTool = std::make_unique<PracticeToolService>(
        mHost, mDSPMutex,
        [this](const std::string& message, const std::string& detail) { ReportErrorToUI(message, detail); },
        [this](const std::string& jsonMessage) { SendMessageToUI(jsonMessage); });
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
    // ARM mode: click is playing, waiting for input signal to trigger recording
    if (mRiffCapture.armed && !mRiffCapture.active && !mRiffCapture.complete)
    {
        const bool hasInput = (inputs && inputs[0]);
        const float* inputR = (inputs && inputs[1]) ? inputs[1] : (inputs ? inputs[0] : nullptr);

        if (!mRiffCapture.armCountInComplete)
        {
            // Track count-in progress
            mRiffCapture.armCountInIndex += static_cast<std::size_t>(numSamples);

            if (mRiffCapture.armCountInIndex >= mRiffCapture.countInSamples)
            {
                mRiffCapture.armCountInComplete = true;
            }
        }
        else if (hasInput)
        {
            // Count-in done; watch for input signal above threshold
            for (int i = 0; i < numSamples; ++i)
            {
                const float level = std::max(std::abs(inputs[0][i]), std::abs(inputR[i]));

                if (level >= mRiffCapture.armThreshold)
                {
                    // Compute bar phase at trigger for snapping the trim start to the bar boundary
                    const double beatScaleTrig = 4.0 / static_cast<double>(std::max(1, mRiffCapture.config.timeSigDen));
                    const double samplesPerBeatTrig =
                        mRiffCapture.sampleRate * (60.0 / std::max(1.0, mRiffCapture.config.tempoBpm)) * beatScaleTrig;
                    const double samplesPerBarTrig =
                        samplesPerBeatTrig * static_cast<double>(std::max(1, mRiffCapture.config.timeSigNum));
                    const std::size_t barSamples = static_cast<std::size_t>(std::max(1.0, samplesPerBarTrig));
                    const std::size_t triggerOffset = mRiffCapture.armPostCountInSamples + static_cast<std::size_t>(i);
                    const std::size_t barAlignOffset = triggerOffset % barSamples;

                    // Trigger: start recording — audio from trigger point is captured
                    mRiffCapture.armed = false;
                    mRiffCapture.active = true;
                    mRiffCapture.writeIndex = mRiffCapture.countInSamples; // already past count-in
                    mRiffCapture.startedAt = std::chrono::steady_clock::now();
                    nlohmann::json startMsg;
                    startMsg["type"] = "riffCaptureStarted";
                    startMsg["takeId"] = mRiffCapture.takeId;
                    startMsg["bars"] = mRiffCapture.config.bars;
                    startMsg["tempoBpm"] = mRiffCapture.config.tempoBpm;
                    startMsg["timeSigNum"] = mRiffCapture.config.timeSigNum;
                    startMsg["timeSigDen"] = mRiffCapture.config.timeSigDen;
                    startMsg["countInBars"] = 0;
                    startMsg["barAlignOffsetSamples"] = barAlignOffset;
                    SendMessageToUI(startMsg.dump());
                    break;
                }
            }

            // Only track detection-phase samples when still waiting (no trigger this block)
            if (mRiffCapture.armed)
            {
                mRiffCapture.armPostCountInSamples += static_cast<std::size_t>(numSamples);
            }
        }
    }

    if (mRiffCapture.active && !mRiffCapture.complete)
    {
        const bool hasInputCh0 = (inputs && inputs[0]);
        const float* capInputR = (inputs && inputs[1]) ? inputs[1] : (hasInputCh0 ? inputs[0] : nullptr);

        if (hasInputCh0 && mRiffCapture.writeIndex < mRiffCapture.targetSamples)
        {
            const std::size_t countInSamples = mRiffCapture.countInSamples;
            const std::size_t bucketSize = std::max<std::size_t>(1, mRiffCapture.livePeakBucketSize);

            for (int i = 0; i < numSamples && mRiffCapture.writeIndex < mRiffCapture.targetSamples; ++i)
            {
                if (mRiffCapture.writeIndex >= countInSamples)
                {
                    const std::size_t captureIndex = mRiffCapture.writeIndex - countInSamples;

                    if (captureIndex < mRiffCapture.left.size() && captureIndex < mRiffCapture.right.size())
                    {
                        mRiffCapture.left[captureIndex] = inputs[0][i];
                        mRiffCapture.right[captureIndex] = capInputR[i];
                        // Update live waveform peak bucket
                        const float peakVal = std::max(std::abs(inputs[0][i]), std::abs(capInputR[i]));
                        const std::size_t bucket = captureIndex / bucketSize;

                        if (bucket < mRiffCapture.livePeaks.size())
                        {
                            mRiffCapture.livePeaks[bucket] = std::max(mRiffCapture.livePeaks[bucket], peakVal);
                        }
                    }
                }

                ++mRiffCapture.writeIndex;
            }

            // Send live progress every ~250 ms
            const std::size_t capturedSoFar =
                mRiffCapture.writeIndex > countInSamples ? mRiffCapture.writeIndex - countInSamples : 0;
            const std::size_t progressInterval =
                std::max<std::size_t>(1, static_cast<std::size_t>(mRiffCapture.sampleRate * 0.25));

            if (capturedSoFar > 0 && capturedSoFar >= mRiffCapture.lastProgressSample + progressInterval)
            {
                mRiffCapture.lastProgressSample = capturedSoFar;
                nlohmann::json progressMsg;
                progressMsg["type"] = "riffCaptureProgress";
                progressMsg["capturedSamples"] = capturedSoFar;
                progressMsg["waveformPeaks"] = mRiffCapture.livePeaks;
                SendMessageToUI(progressMsg.dump());
            }

            if (mRiffCapture.writeIndex >= mRiffCapture.targetSamples)
            {
                const std::size_t capturedFinal = mRiffCapture.left.size();
                mRiffCapture.complete = true;
                mRiffCapture.active = false;
                mRiffCapture.endedAt = std::chrono::steady_clock::now();
                DeactivateRiffGuidance(false);
                const double samplesPerBeat = mRiffCapture.sampleRate *
                                              (60.0 / std::max(1.0, mRiffCapture.config.tempoBpm)) *
                                              (4.0 / static_cast<double>(std::max(1, mRiffCapture.config.timeSigDen)));
                const double samplesPerBar =
                    samplesPerBeat * static_cast<double>(std::max(1, mRiffCapture.config.timeSigNum));
                const int computedBars = std::max(
                    1, static_cast<int>(std::round(static_cast<double>(capturedFinal) / std::max(1.0, samplesPerBar))));
                nlohmann::json msg;
                msg["type"] = "riffCaptureStopped";
                msg["takeId"] = mRiffCapture.takeId;
                msg["bars"] = computedBars;
                msg["capturedSamples"] = capturedFinal;
                msg["sampleRate"] = mRiffCapture.sampleRate;
                msg["hasAudio"] = capturedFinal > 0;
                msg["waveformPeaks"] = BuildWaveformPeaks(mRiffCapture.left, mRiffCapture.right, 256);
                SendMessageToUI(msg.dump());
            }
        }
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

namespace
{
// Sizes an editor window can plausibly have been left at. Anything outside this is a
// layout artefact rather than a size a user dragged to.
bool IsPlausibleEditorWindowSize(int width, int height)
{
    constexpr int kMinRememberedEditorDimension = 200;
    constexpr int kMaxRememberedEditorDimension = 16384;

    return width >= kMinRememberedEditorDimension && height >= kMinRememberedEditorDimension &&
           width <= kMaxRememberedEditorDimension && height <= kMaxRememberedEditorDimension;
}
} // namespace

void PluginController::SetEditorWindowSize(int width, int height)
{
    // Staged, not committed. A range check alone is not enough to tell a real resize from
    // a layout artefact: hosts resize the editor on the way to closing its window, and a
    // degenerate rect comes back through the editor's constrainer as its *minimum* size,
    // which looks entirely plausible but is not a size anyone chose. Committing that is
    // how a remembered size turns into a tiny window on the next open.
    //
    // So a reported size only becomes the remembered one once it is still in effect an
    // idle tick later (see OnIdle). The editor drives the idle callback and stops driving
    // it when it is destroyed, so a size that exists only while the window is being torn
    // down is never committed.
    if (!IsPlausibleEditorWindowSize(width, height))
    {
        return;
    }

    if (mEditorWindowSize.width == width && mEditorWindowSize.height == height)
    {
        mPendingEditorWindowSize.reset();
        return;
    }

    // Repeated reports of the same pending size must not keep restarting the settle, or a
    // host that re-reports its size every frame would never let anything commit.
    if (mPendingEditorWindowSize.has_value() && mPendingEditorWindowSize->width == width &&
        mPendingEditorWindowSize->height == height)
    {
        return;
    }

    mPendingEditorWindowSize = EditorWindowSize{width, height};
    mEditorWindowSizeChangedSinceIdle = true;
}

std::string PluginController::SerializeState() const
{
    nlohmann::json state = nlohmann::json::object();
    state["version"] = 1;

    if (mActivePreset)
    {
        Preset presetWithRuntimeState = *mActivePreset;
        CaptureRuntimePluginStates(presetWithRuntimeState, mActivePresetId);
        state["preset"] = nlohmann::json::parse(PresetStorage::SerializeToJson(presetWithRuntimeState));
    }

    state["presetId"] = mActivePresetId;
    state["activeSceneId"] = GetResolvedActiveSceneId();
    state["appSettings"] = mAppSettings;
    state["uiSettings"] = mUiSettings;
    state["uiViewState"] = mUiViewState;
    state["globalSignalChain"] = mPresetMixer.GetGlobalChainConfig();

    // The editor window size the user last left this instance at. Only emitted once the
    // editor has actually reported a size, so a project saved with the editor never
    // opened does not pin a default over whatever the wrapper would pick.
    if (mEditorWindowSize.IsValid())
    {
        state["editorWindow"] = {{"width", mEditorWindowSize.width}, {"height", mEditorWindowSize.height}};
    }

    // NAM quality is per instance, so it rides in host state rather than app.json.
    // Emitted as its own block (not just inside appSettings) so it stays legible and
    // is restored explicitly, after the appSettings merge, on the way back in.
    state["namQuality"] = {{"slimmableSize", mNamQuality.slimmableSize},
                           {"oversampling", mNamQuality.oversamplingIndex},
                           {"antiAliasPhase", mNamQuality.antiAliasPhaseIndex}};

    // No "parameters" block: global FX live in globalSignalChain, which is the single
    // source of truth. States written before that consolidation still carry the key and
    // are simply ignored on the way back in.

    nlohmann::json mixer = nlohmann::json::object();
    mixer["masterGain"] = mPresetMixer.GetMasterGain();
    mixer["limiterEnabled"] = mPresetMixer.IsLimiterEnabled();

    nlohmann::json activePresetIds = nlohmann::json::array();
    nlohmann::json presetConfigs = nlohmann::json::object();
    // Full preset data for the non-focused slots. Restoring them by id alone (the original
    // behaviour, still the fallback) reloads them from the machine's preset library, which
    // throws away every project-local edit — hosted plugin state included — and fails
    // outright when the project is opened on a machine that does not have the preset.
    nlohmann::json presetData = nlohmann::json::object();

    for (const auto& id : mPresetMixer.GetActivePresetIds())
    {
        activePresetIds.push_back(id);

        if (const auto cfg = mPresetMixer.GetPresetConfig(id))
        {
            presetConfigs[id] = {
                {"name", cfg->name}, {"mix", cfg->mix}, {"pan", cfg->pan}, {"mute", cfg->mute}, {"solo", cfg->solo}};
        }

        // The focused slot already rides in state["preset"] with its runtime state folded in.
        if (mActivePreset && id == mActivePresetId)
        {
            continue;
        }

        const auto cachedIt = mMixerPresetJsonCache.find(id);

        if (cachedIt == mMixerPresetJsonCache.end())
        {
            continue;
        }

        if (auto slotPreset = PresetStorage::DeserializeFromJson(cachedIt->second))
        {
            CaptureMixerSlotHostedPluginState(*slotPreset, id);
            try
            {
                presetData[id] = nlohmann::json::parse(PresetStorage::SerializeToJson(*slotPreset));
            }
            catch (const std::exception&)
            {
                // A slot that will not round-trip is skipped rather than poisoning the whole
                // state blob; it falls back to restore-by-id on the way back in.
            }
        }
    }

    mixer["activePresetIds"] = std::move(activePresetIds);
    mixer["presets"] = std::move(presetConfigs);
    mixer["presetData"] = std::move(presetData);
    state["mixer"] = std::move(mixer);

    state["automation"] = mAutomationSlots.SaveToJson();

    return state.dump();
}

void PluginController::DeserializeState(const std::string& json)
{
    if (mHost.IsStandalone())
    {
        // Standalone startup restores from app settings + preset files
        // (LoadLastSessionState), not from host-serialized transient state — a stale
        // snapshot must never republish machine-wide settings or revive an old graph.
        //
        // Hosted plugin state is the one exception, and it is handled separately below.
        RestoreStandaloneHostedPluginState(json);
        return;
    }

    // Everything restored below belongs to the DAW project, not to the machine-wide
    // store. Without this, reopening an old project republishes its whole settings
    // snapshot over settings the user has changed since — the merge lands in
    // mAppSettings while mAppSettingsBaseline still describes the store, so the next
    // save of anything at all diffs the project's values as this instance's edits.
    //
    // The scope blocks writes for the duration and rebases the baseline on the way
    // out, including on the exception path, so a partial restore cannot leave project
    // values queued for publication either.
    struct HostStateRestoreScope
    {
        PluginController& controller;

        explicit HostStateRestoreScope(PluginController& c) : controller(c)
        {
            controller.mRestoringHostState = true;
        }

        ~HostStateRestoreScope()
        {
            controller.mRestoringHostState = false;
            controller.AdoptAppSettingsAsBaseline();
        }
    };

    const HostStateRestoreScope restoreScope{*this};

    try
    {
        auto state = nlohmann::json::parse(json);
        const nlohmann::json* incomingSettings = nullptr;

        if (state.contains("appSettings") && state["appSettings"].is_object())
        {
            incomingSettings = &state["appSettings"];
        }
        else if (state.contains("settings") && state["settings"].is_object())
        {
            incomingSettings = &state["settings"];
        }

        if (incomingSettings != nullptr)
        {
            if (!mAppSettings.is_object())
            {
                mAppSettings = nlohmann::json::object();
            }

            for (auto it = incomingSettings->begin(); it != incomingSettings->end(); ++it)
            {
                mAppSettings[it.key()] = it.value();
            }

            // Merging is not applying. These values have to reach the DSP, or the
            // instance runs on whatever Initialize() read from the shared store while
            // the UI reports the project's values back — the two silently disagree.
            // The return value is discarded on purpose: nothing restored from host state
            // is this instance's to publish (see HostStateRestoreScope above).
            (void)ApplySettingsToRuntime(SettingsApplyMode::kApplyAll);
        }

        // Applied after the appSettings merge so the instance's own saved tier wins over
        // whatever app.json seeded at Initialize(). Older states without this block fall
        // back to the appSettings values handled above.
        if (state.contains("namQuality") && state["namQuality"].is_object())
        {
            const auto& quality = state["namQuality"];
            const auto readNumber = [&quality](const char* field, double fallback) {
                const auto it = quality.find(field);
                return (it != quality.end() && it->is_number()) ? it->get<double>() : fallback;
            };

            mAppSettings[kNamSlimmableSizeSettingKey] =
                SanitizeNamSlimmableSize(readNumber("slimmableSize", kNamSlimmableSizeDefault));
            mAppSettings[kNamOversamplingSettingKey] =
                SanitizeNamOversamplingIndex(readNumber("oversampling", kNamOversamplingIndexDefault));
            mAppSettings[kNamAntiAliasPhaseSettingKey] =
                SanitizeNamAntiAliasPhaseIndex(readNumber("antiAliasPhase", kNamAntiAliasPhaseIndexDefault));
            ApplyNamQualitySettings();
        }

        if (state.contains("uiSettings") && state["uiSettings"].is_object())
        {
            mUiSettings = state["uiSettings"];
        }
        else
        {
            ApplyUiSettingsFromAppSettings();
        }

        if (state.contains("uiViewState") && state["uiViewState"].is_object())
        {
            mUiViewState = state["uiViewState"];
        }

        if (state.contains("editorWindow") && state["editorWindow"].is_object())
        {
            const auto& editorWindow = state["editorWindow"];
            const auto readDimension = [&editorWindow](const char* field) {
                const auto it = editorWindow.find(field);
                return (it != editorWindow.end() && it->is_number()) ? it->get<int>() : 0;
            };

            // Committed straight away rather than staged: this is the project's own value,
            // not something an editor is currently reporting, and the editor has to be
            // able to read it back the moment it is created. Same sanity check as a live
            // resize, so a hand-edited or corrupt project cannot pin an absurd size.
            const auto width = readDimension("width");
            const auto height = readDimension("height");

            if (IsPlausibleEditorWindowSize(width, height))
            {
                mEditorWindowSize = EditorWindowSize{width, height};
                mPendingEditorWindowSize.reset();
            }
        }

        if (state.contains("globalSignalChain") && state["globalSignalChain"].is_object())
        {
            // Build off the lock, install under it — see PrepareGlobalChainSwap().
            mPresetMixer.PrepareGlobalChainSwap(state["globalSignalChain"].get<GlobalSignalChainConfig>());
            std::lock_guard<std::mutex> dspLock(mDSPMutex);
            mPresetMixer.CommitGlobalChainSwap();
        }

        if (state.contains("preset"))
        {
            auto presetOpt = PresetStorage::DeserializeFromJson(state["preset"].dump());

            if (presetOpt)
            {
                mActivePresetId = state.value("presetId", presetOpt->id);
                mActiveSceneId = state.contains("activeSceneId") && state["activeSceneId"].is_string()
                                     ? state["activeSceneId"].get<std::string>()
                                     : std::string{};
                mActivePreset = *presetOpt;
                mActivePresetJson = PresetStorage::SerializeToJson(*presetOpt);
                ApplyPreset(*presetOpt);
            }
        }

        // A "parameters" array from an older state is deliberately not replayed. It was a
        // flat mirror of the global FX values, and only ever tracked a few of them — the
        // rest read back as 0, which switched those effects off over the chain restored
        // just above. globalSignalChain carries all of it.

        if (state.contains("mixer") && state["mixer"].is_object())
        {
            const auto& mixer = state["mixer"];

            if (mixer.contains("masterGain") && mixer["masterGain"].is_number())
            {
                mPresetMixer.SetMasterGain(mixer["masterGain"].get<double>());
            }

            if (mixer.contains("limiterEnabled") && mixer["limiterEnabled"].is_boolean())
            {
                mPresetMixer.SetLimiterEnabled(mixer["limiterEnabled"].get<bool>());
            }

            // Reset active presets before restoring mixer state
            for (const auto& id : mPresetMixer.GetActivePresetIds())
            {
                mPresetMixer.RemoveActivePreset(id);
            }

            std::vector<std::string> activeIds;

            if (mixer.contains("activePresetIds") && mixer["activePresetIds"].is_array())
            {
                for (const auto& entry : mixer["activePresetIds"])
                {
                    if (entry.is_string())
                    {
                        activeIds.push_back(entry.get<std::string>());
                    }
                }
            }

            const auto presets = mixer.contains("presets") ? mixer["presets"] : nlohmann::json::object();
            // Written since full slot data was added to host state; absent in older projects,
            // which fall through to the restore-by-id path below exactly as before.
            const auto presetData = mixer.contains("presetData") && mixer["presetData"].is_object()
                                        ? mixer["presetData"]
                                        : nlohmann::json::object();

            if (activeIds.empty() && presets.is_object())
            {
                for (const auto& [id, _] : presets.items())
                {
                    activeIds.push_back(id);
                }
            }

            for (const auto& id : activeIds)
            {
                const auto presetEntry =
                    presets.is_object() && presets.contains(id) ? presets[id] : nlohmann::json::object();
                const std::string name = presetEntry.value("name", id);

                bool added = false;

                if (mActivePreset && (id == "p1" || id == mActivePresetId))
                {
                    added = mPresetMixer.AddActivePreset(*mActivePreset, id, name);

                    if (added)
                    {
                        AttachRuntimeConfigCallbacks(id, *mActivePreset);
                        mMixerPresetJsonCache[id] = PresetStorage::SerializeToJson(*mActivePreset);
                    }
                }

                // The project's own copy of this slot wins over the machine's preset library:
                // it is the one carrying the project's edits and its hosted plugin state.
                if (!added && presetData.contains(id))
                {
                    if (auto slotPreset = PresetStorage::DeserializeFromJson(presetData[id].dump()))
                    {
                        added = mPresetMixer.AddActivePreset(*slotPreset, id, name);

                        if (added)
                        {
                            AttachRuntimeConfigCallbacks(id, *slotPreset);
                            mMixerPresetJsonCache[id] = PresetStorage::SerializeToJson(*slotPreset);
                            AppendSessionLog("Mixer slot restored from host state id=" + id +
                                             ", state=" + SummarizeHostedPluginState(*slotPreset));
                        }
                    }
                }

                if (!added)
                {
                    added = AddActivePresetById(id);
                }

                if (!added && mActivePreset)
                {
                    added = mPresetMixer.AddActivePreset(*mActivePreset, id, name);

                    if (added)
                    {
                        AttachRuntimeConfigCallbacks(id, *mActivePreset);
                        mMixerPresetJsonCache[id] = PresetStorage::SerializeToJson(*mActivePreset);
                    }
                }

                if (presetEntry.is_object())
                {
                    if (presetEntry.contains("mix") && presetEntry["mix"].is_number())
                    {
                        mPresetMixer.SetPresetMix(id, presetEntry["mix"].get<double>());
                    }

                    if (presetEntry.contains("pan") && presetEntry["pan"].is_number())
                    {
                        mPresetMixer.SetPresetPan(id, presetEntry["pan"].get<double>());
                    }

                    if (presetEntry.contains("mute") && presetEntry["mute"].is_boolean())
                    {
                        mPresetMixer.SetPresetMute(id, presetEntry["mute"].get<bool>());
                    }

                    if (presetEntry.contains("solo") && presetEntry["solo"].is_boolean())
                    {
                        mPresetMixer.SetPresetSolo(id, presetEntry["solo"].get<bool>());
                    }
                }
            }
        }

        if (state.contains("automation") && state["automation"].is_object())
        {
            mAutomationSlots.LoadFromJson(state["automation"]);
        }
    }
    catch (const std::exception&)
    {
        // Ignore malformed state
    }

    mPendingStateBroadcast = true;
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

void PluginController::ReloadSharedSyncSourcesFromDisk()
{
    // LoadAppSettings() replaces mAppSettings wholesale from the shared store, which would
    // otherwise drag this instance's NAM quality and editor layout back to whatever another
    // instance (or the standalone app) last wrote. kPreserveInstanceOwned re-asserts them
    // instead; the live values are members, so nothing needs snapshotting first.
    LoadAppSettings();

    if (ApplySettingsToRuntime(SettingsApplyMode::kPreserveInstanceOwned))
    {
        SaveAppSettings();
    }

    LoadResourceLibraries();
    LoadBlendLibrary();
    LoadCustomEffectLibrary();

    std::vector<std::string> definitionIds;
    definitionIds.reserve(mCompositeLibrary.GetAllDefinitions().size());

    for (const auto& def : mCompositeLibrary.GetAllDefinitions())
    {
        definitionIds.push_back(def.id);
    }

    for (const auto& id : definitionIds)
    {
        mCompositeLibrary.RemoveDefinition(id);
    }

    LoadCompositeLibrary();

    {
        std::lock_guard<std::mutex> riffLock(mRiffLibraryMutex);
        mRiffLibraryIndex = LoadRiffLibraryIndex();
    }

    const auto automationData = LoadUiStorageJson("automation.json", nlohmann::json::object());

    if (!automationData.empty())
    {
        mAutomationSlots.LoadFromJson(automationData);
    }

    const auto setlistsData = LoadUiStorageJson("setlists.json", nlohmann::json::object());
    mSetlistBankSize = setlistsData.value("bankSize", 8);
    mSetlistCursorIndex = setlistsData.value("cursorIndex", 0);

    InvalidateResourceUsageIndex();
}

void PluginController::PollSharedSyncState()
{
    const auto now = std::chrono::steady_clock::now();

    if (now < mNextSharedSyncPollAt)
    {
        return;
    }

    mNextSharedSyncPollAt = now + kSharedSyncPollInterval;

    const auto payload =
        Store().Get(storage::ItemType::kDocument, kSharedSyncStateDocumentId).value_or(nlohmann::json::object());

    if (!payload.is_object())
    {
        return;
    }

    const auto versionIt = payload.find("version");

    if (versionIt == payload.end() || !versionIt->is_number_unsigned())
    {
        return;
    }

    const auto version = versionIt->get<std::uint64_t>();

    if (!mSharedSyncVersionSeenInitialized)
    {
        mSharedSyncVersionSeen = version;
        mSharedSyncVersionSeenInitialized = true;
        return;
    }

    if (version <= mSharedSyncVersionSeen)
    {
        return;
    }

    mSharedSyncVersionSeen = version;

    if (!mUIReady)
    {
        return;
    }

    nlohmann::json msg;
    msg["type"] = "sharedSyncUpdated";
    msg["version"] = version;

    if (payload.contains("domains") && payload["domains"].is_array())
    {
        msg["domains"] = payload["domains"];
    }

    if (payload.contains("updatedAt"))
    {
        msg["updatedAt"] = payload["updatedAt"];
    }

    SendMessageToUI(msg.dump());
}

// ════════════════════════════════════════════════════════════════════
// Signal path test tone
// ════════════════════════════════════════════════════════════════════

bool PluginController::StartSignalPathTest(double frequencyHz, double durationSeconds)
{
    return mSignalTest->Start(frequencyHz, durationSeconds, mHost.GetSampleRate());
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

void PluginController::AppendSessionLog(const std::string& message) const
{
    if (message.empty())
    {
        return;
    }

    const auto settingsDir = GetEffectiveSettingsDirectory();
    const auto logPath = settingsDir / kSessionLogFileName;
    [[maybe_unused]] const auto ensuredLogDir = mFileSystem.EnsureDirectory(logPath.parent_path());

    std::ofstream output(logPath, std::ios::app);

    if (!output)
    {
        return;
    }

    output << FormatTimestamp() << " " << message << "\n";
}

// ════════════════════════════════════════════════════════════════════
// Request handlers
// ════════════════════════════════════════════════════════════════════

void PluginController::HandleStateRequest()
{
    mPendingStateBroadcast = true;
}

void PluginController::HandleGetSharedSyncStateRequest()
{
    // Only act if the shared sync state has a new version since we last responded.
    const auto filePayload =
        Store().Get(storage::ItemType::kDocument, kSharedSyncStateDocumentId).value_or(nlohmann::json::object());
    std::uint64_t currentVersion = 0;

    if (filePayload.is_object())
    {
        const auto it = filePayload.find("version");

        if (it != filePayload.end() && it->is_number_unsigned())
        {
            currentVersion = it->get<std::uint64_t>();
        }
    }

    if (currentVersion > 0 && currentVersion == mSharedSyncVersionHandled)
    {
        return;
    }

    mSharedSyncVersionHandled = currentVersion;

    if (!mSharedSyncVersionSeenInitialized || currentVersion > mSharedSyncVersionSeen)
    {
        mSharedSyncVersionSeen = currentVersion;
        mSharedSyncVersionSeenInitialized = true;
    }

    ReloadSharedSyncSourcesFromDisk();

    nlohmann::json state;
    state["type"] = "sharedSyncState";
    state["appSettings"] = mAppSettings;
    state["uiSettings"] = mUiSettings;
    state["blendLibrary"] = mBlendLibrary;
    state["presetArchiveSession"] = {{"active", IsPresetArchiveSessionActive()}};

    if (mPresetArchiveSession)
    {
        state["presetArchiveSession"]["archiveName"] = mPresetArchiveSession->archiveName;
        state["presetArchiveSession"]["archiveKey"] = mPresetArchiveSession->archiveKey;
        state["presetArchiveSession"]["presetCount"] = mPresetArchiveSession->presetCount;
    }

    // Resource library summary + per-type entries for UI rendering
    nlohmann::json libraryInfo = nlohmann::json::object();
    auto allResources = mResourceLibrary.GetAllResources();
    libraryInfo["totalCount"] = allResources.size();

    for (const auto& resource : allResources)
    {
        const std::string type = resource.type;

        if (!libraryInfo.contains(type) || !libraryInfo[type].is_array())
        {
            libraryInfo[type] = nlohmann::json::array();
        }

        nlohmann::json entry;
        entry["id"] = resource.id;
        entry["name"] = resource.name;
        entry["category"] = resource.category;
        entry["description"] = resource.description;
        entry["tags"] = resource.tags;
        entry["filePath"] = resource.filePath.empty() ? "" : util::PathToUtf8(resource.filePath);
        entry["hash"] = resource.hash;

        if (!resource.metadata.empty())
        {
            entry["metadata"] = resource.metadata;
        }

        const bool hasPath = !resource.filePath.empty();
        const bool exists = hasPath && std::filesystem::exists(resource.filePath);
        entry["fileMissing"] = !(hasPath && exists);

        libraryInfo[type].push_back(entry);
    }

    state["resourceLibrary"] = std::move(libraryInfo);

    {
        nlohmann::json customEffects = nlohmann::json::array();

        for (const auto& entry : mCustomEffectLibrary.GetAllEntries())
        {
            customEffects.push_back(SerializeCustomEffectLibraryEntry(entry));
        }

        state["customEffectLibrary"] = std::move(customEffects);
    }

    SendMessageToUI(state.dump());

    // Send auxiliary shared datasets over their existing message contracts.
    HandleGetThemeRequest();
    HandleGetPresetListRequest();
    HandleGetPresetFoldersRequest();
    HandleGetPresetFavoritesRequest();
    HandleGetPresetRatingsRequest();
    HandleGetSetlistsRequest();
    HandleGetAutomationRequest();
    SendCompositeLibraryToUI();
    SendCompositePresetListToUI();
    SendRiffLibraryStateToUI();
}

void PluginController::HandleCaptureDebugSnapshotRequest(const nlohmann::json& payload)
{
    const std::string source = payload.value("source", "manual");

    if (!mUIReady)
    {
        HandleDebugReportUiStateRequest(nlohmann::json{
            {"source", source + ":backend-only"},
        });
        return;
    }

    SendMessageToUI(nlohmann::json{
        {"type", "captureDebugSnapshot"},
        {"source", source},
    }
                        .dump());
}

void PluginController::HandleDebugReportUiStateRequest(const nlohmann::json& payload)
{
    try
    {
        const std::string source = payload.value("source", "ui-auto");
        nlohmann::json snapshot = nlohmann::json::object();
        snapshot["type"] = "debugSnapshot";
        snapshot["capturedAt"] = FormatTimestamp();
        snapshot["source"] = source;
        snapshot["paths"] = {
            {"sessionLog", (mFileSystem.ResolveSettingsDirectory() / kSessionLogFileName).generic_string()},
            {"snapshot", ResolveDebugSnapshotPath(mFileSystem).generic_string()},
        };
        snapshot["session"] = {
            {"activePresetId", mActivePresetId},
            {"activeSceneId", GetResolvedActiveSceneId()},
            {"uiReady", mUIReady},
            {"pendingStateBroadcast", mPendingStateBroadcast},
            {"activePresetIds", mPresetMixer.GetActivePresetIds()},
        };

        if (payload.contains("snapshot"))
        {
            snapshot["ui"] = payload["snapshot"];
            ScrubSensitiveJson(snapshot["ui"]);
        }

        nlohmann::json backendState = nlohmann::json::parse(SerializeState());
        ScrubSensitiveJson(backendState);
        snapshot["backend"] = std::move(backendState);

        if (mActivePreset)
        {
            snapshot["activePresetSummary"] = SummarizeHostedPluginState(*mActivePreset);
        }

        const auto snapshotPath = ResolveDebugSnapshotPath(mFileSystem);
        SaveJsonFile(mFileSystem, snapshotPath, snapshot);

        SendMessageToUI(nlohmann::json{
            {"type", "debugSnapshotWritten"},
            {"path", snapshotPath.generic_string()},
            {"source", source},
        }
                            .dump());
    }
    catch (const std::exception& e)
    {
        AppendSessionLog("Debug snapshot write failed: " + std::string{e.what()});
    }
}

void PluginController::HandleSetParameterRequest(const nlohmann::json& payload)
{
    // Named alias for the global-chain paths, kept because "setParameter" is the
    // documented UI/scripting entry point. It carries a flat name and a number, so
    // translate to a path and hand off — HandleSetGlobalChainParamRequest owns the
    // routing and the persistence, and there is no second copy of the values to
    // drift out of sync.
    //
    // Both spellings are accepted: the protocol documents camelCase, older UI code
    // used snake_case, and a name that silently matched neither is what made the
    // legacy path dead in the first place.
    struct GlobalParamAlias
    {
        const char* path;
        bool isBoolean;
        bool isInteger;
    };

    static const std::unordered_map<std::string, GlobalParamAlias> kAliases = {
        {"inputTrim", {"input.gain", false, false}},         {"input_trim", {"input.gain", false, false}},
        {"outputTrim", {"output.gain", false, false}},       {"output_trim", {"output.gain", false, false}},
        {"gateEnabled", {"gate.enabled", true, false}},      {"gate_enabled", {"gate.enabled", true, false}},
        {"gateThreshold", {"gate.threshold", false, false}}, {"gate_threshold", {"gate.threshold", false, false}},
        {"transpose", {"transpose.semitones", false, true}}, {"doublerEnabled", {"doubler.enabled", true, false}},
        {"doublerDelay", {"doubler.delay", false, false}},   {"eqEnabled", {"eq.enabled", true, false}},
        {"eqLowGain", {"eq.lowGain", false, false}},         {"eqLowFreq", {"eq.lowFreq", false, false}},
        {"eqLowMidGain", {"eq.lowMidGain", false, false}},   {"eqLowMidFreq", {"eq.lowMidFreq", false, false}},
        {"eqLowMidQ", {"eq.lowMidQ", false, false}},         {"eqHighMidGain", {"eq.highMidGain", false, false}},
        {"eqHighMidFreq", {"eq.highMidFreq", false, false}}, {"eqHighMidQ", {"eq.highMidQ", false, false}},
        {"eqHighGain", {"eq.highGain", false, false}},       {"eqHighFreq", {"eq.highFreq", false, false}},
    };

    const auto name = payload.value("name", std::string{});
    const auto it = kAliases.find(name);

    if (it == kAliases.end())
    {
        AppendSessionLog("Ignoring setParameter for unknown parameter: " + name);
        return;
    }

    if (!payload.contains("value") || !payload["value"].is_number())
    {
        AppendSessionLog("Ignoring setParameter without a numeric value: " + name);
        return;
    }

    const double raw = payload["value"].get<double>();

    nlohmann::json forwarded;
    forwarded["path"] = it->second.path;

    if (it->second.isBoolean)
    {
        forwarded["value"] = raw > 0.5;
    }
    else if (it->second.isInteger)
    {
        forwarded["value"] = static_cast<int>(std::llround(raw));
    }
    else
    {
        forwarded["value"] = raw;
    }

    HandleSetGlobalChainParamRequest(forwarded);
}

void PluginController::HandleSetGlobalChainParamRequest(const nlohmann::json& payload)
{
    std::string path = payload.value("path", "");
    auto value = payload.value("value", nlohmann::json());
    bool persistGlobalFx = false;
    {
        std::lock_guard<std::mutex> lock(mDSPMutex);

        // Route paramPath strings to the corresponding mixer methods
        if (path == "gate.enabled")
        {
            mPresetMixer.SetGlobalGateEnabled(value.get<bool>());
            persistGlobalFx = true;
        }
        else if (path == "gate.threshold")
        {
            mPresetMixer.SetGlobalGateThreshold(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "gate.attack")
        {
            mPresetMixer.SetGlobalGateAttack(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "gate.hold")
        {
            mPresetMixer.SetGlobalGateHold(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "gate.release")
        {
            mPresetMixer.SetGlobalGateRelease(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "transpose.enabled")
        {
            mPresetMixer.SetGlobalTransposeEnabled(value.get<bool>());
            UpdateHostLatency();
            persistGlobalFx = true;
        }
        else if (path == "transpose.semitones")
        {
            mPresetMixer.SetGlobalTranspose(std::clamp(value.get<int>(), -12, 12));
            UpdateHostLatency();
            persistGlobalFx = true;
        }
        else if (path == "eq.enabled")
        {
            mPresetMixer.SetGlobalEQEnabled(value.get<bool>());
            persistGlobalFx = true;
        }
        else if (path == "doubler.enabled")
        {
            mPresetMixer.SetGlobalDoublerEnabled(value.get<bool>());
            persistGlobalFx = true;
        }
        else if (path == "doubler.delay")
        {
            mPresetMixer.SetGlobalDoublerDelay(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "doubler.mix")
        {
            mPresetMixer.SetGlobalDoublerMix(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "doubler.detune")
        {
            mPresetMixer.SetGlobalDoublerDetune(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "input.gain")
        {
            mPresetMixer.SetGlobalInputGain(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "output.gain")
        {
            mPresetMixer.SetGlobalOutputGain(value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "limiter.enabled")
        {
            mPresetMixer.SetLimiterEnabled(value.get<bool>());
        }
        else if (path == "eq.lowGain")
        {
            mPresetMixer.SetGlobalEQBandGain(0, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.lowFreq")
        {
            mPresetMixer.SetGlobalEQBandFrequency(0, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.lowMidGain")
        {
            mPresetMixer.SetGlobalEQBandGain(1, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.lowMidFreq")
        {
            mPresetMixer.SetGlobalEQBandFrequency(1, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.lowMidQ")
        {
            mPresetMixer.SetGlobalEQBandQ(1, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.highMidGain")
        {
            mPresetMixer.SetGlobalEQBandGain(2, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.highMidFreq")
        {
            mPresetMixer.SetGlobalEQBandFrequency(2, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.highMidQ")
        {
            mPresetMixer.SetGlobalEQBandQ(2, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.highGain")
        {
            mPresetMixer.SetGlobalEQBandGain(3, value.get<double>());
            persistGlobalFx = true;
        }
        else if (path == "eq.highFreq")
        {
            mPresetMixer.SetGlobalEQBandFrequency(3, value.get<double>());
            persistGlobalFx = true;
        }
    }

    if (persistGlobalFx)
    {
        PersistGlobalFxSettingsToAppSettings();
    }

    // No echo: the UI already owns the values it sent.
    // Full state is pushed via HandleGetGlobalChainRequest / HandleSetGlobalChainRequest.
}

void PluginController::HandleSignalTestRequest(const nlohmann::json& payload)
{
    double freq = payload.value("frequency", 440.0);
    double dur = payload.value("duration", 1.0);
    StartSignalPathTest(freq, dur);
}

void PluginController::HandleBrowseModelRequest()
{
    mHost.BrowseFileAsync(BrowseFileType::NAMModel, "Select NAM Model", [this](const BrowseFileResult& result) {
        if (result.success)
        {
            nlohmann::json payload;
            payload["path"] = util::PathToUtf8(result.path);
            HandleLoadModelRequest(payload);
        }
    });
}

void PluginController::HandleBrowseIRRequest()
{
    mHost.BrowseFileAsync(BrowseFileType::IRFile, "Select IR File", [this](const BrowseFileResult& result) {
        if (result.success)
        {
            nlohmann::json payload;
            payload["path"] = util::PathToUtf8(result.path);
            HandleLoadIRRequest(payload);
        }
    });
}

void PluginController::HandleOpenAudioPreferencesRequest()
{
    mHost.OpenAudioPreferences();
}

void PluginController::HandleTunerRequest(const nlohmann::json& payload)
{
    const std::string action = payload.value("action", "");

    if (action == "start")
    {
        mTuner->SetActive(true);
        double referenceFrequency = 440.0;
        bool liveMode = true;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);

            if (payload.contains("liveMode"))
            {
                mPresetMixer.SetLiveTunerMode(payload.value("liveMode", true));
            }

            if (payload.contains("referenceFrequency"))
            {
                mPresetMixer.SetTunerReferenceFrequency(payload["referenceFrequency"].get<double>());
            }

            mPresetMixer.SetTunerEnabled(true);
            referenceFrequency = mPresetMixer.GetTunerReferenceFrequency();
            liveMode = mPresetMixer.IsLiveTunerMode();
        }

        nlohmann::json message;
        message["type"] = "tunerStarted";
        message["referenceFrequency"] = referenceFrequency;
        message["liveMode"] = liveMode;
        SendMessageToUI(message.dump());
        return;
    }

    if (action == "stop")
    {
        mTuner->SetActive(false);
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mPresetMixer.SetTunerEnabled(false);
        }

        nlohmann::json message;
        message["type"] = "tunerStopped";
        SendMessageToUI(message.dump());
        return;
    }

    if (action == "setLiveMode")
    {
        bool liveMode = payload.value("liveMode", true);
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mPresetMixer.SetLiveTunerMode(liveMode);
        }

        nlohmann::json message;
        message["type"] = "tunerLiveModeChanged";
        message["liveMode"] = liveMode;
        SendMessageToUI(message.dump());
        return;
    }

    if (action == "setReference")
    {
        double freq = payload.value("referenceFrequency", 440.0);
        double effectiveFrequency = 440.0;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mPresetMixer.SetTunerReferenceFrequency(freq);
            effectiveFrequency = mPresetMixer.GetTunerReferenceFrequency();
        }

        nlohmann::json message;
        message["type"] = "tunerReferenceChanged";
        message["referenceFrequency"] = effectiveFrequency;
        SendMessageToUI(message.dump());
        return;
    }

    if (payload.contains("enabled"))
    {
        bool enabled = payload.value("enabled", false);
        mTuner->SetActive(enabled);
        double referenceFrequency = 440.0;
        bool liveMode = true;
        {
            std::lock_guard<std::mutex> lock(mDSPMutex);
            mPresetMixer.SetTunerEnabled(enabled);
            referenceFrequency = mPresetMixer.GetTunerReferenceFrequency();
            liveMode = mPresetMixer.IsLiveTunerMode();
        }

        nlohmann::json reply;
        reply["type"] = enabled ? "tunerStarted" : "tunerStopped";
        reply["referenceFrequency"] = referenceFrequency;
        reply["liveMode"] = liveMode;
        SendMessageToUI(reply.dump());
    }
}

void PluginController::HandleSetInputModeRequest(const nlohmann::json& payload)
{
    // In hosted-plugin mode the DAW owns the input configuration; ignore
    // UI overrides and report the effective (host-controlled) state.
    if (mHost.IsStandalone())
    {
        if (payload.contains("monoMode"))
        {
            mPresetMixer.SetMonoMode(payload["monoMode"].get<bool>());
        }
        else if (payload.contains("mono"))
        {
            mPresetMixer.SetMonoMode(payload["mono"].get<bool>());
        }

        if (payload.contains("inputChannel"))
        {
            mPresetMixer.SetInputChannel(payload["inputChannel"].get<int>());
        }
        else if (payload.contains("channel"))
        {
            mPresetMixer.SetInputChannel(payload["channel"].get<int>());
        }
    }
    else
    {
        AppendSessionLog("Ignoring setInputMode request: input is host-controlled in plugin mode");
    }

    nlohmann::json message;
    message["type"] = "inputModeChanged";
    message["monoMode"] = mPresetMixer.IsMonoMode();
    message["inputChannel"] = mPresetMixer.GetInputChannel();
    SendMessageToUI(message.dump());
}

void PluginController::HandleSetAmpCabStateRequest(const nlohmann::json& payload)
{
    bool ampEnabled = true;
    bool cabEnabled = true;

    if (payload.contains("ampEnabled"))
    {
        ampEnabled = payload.value("ampEnabled", true);
    }

    if (payload.contains("cabEnabled"))
    {
        cabEnabled = payload.value("cabEnabled", true);
    }

    nlohmann::json message;
    message["type"] = "ampCabStateChanged";
    message["ampEnabled"] = ampEnabled;
    message["cabEnabled"] = cabEnabled;
    SendMessageToUI(message.dump());
}

void PluginController::HandleSetAutoLevelRequest(const nlohmann::json& payload)
{
    (void)payload;

    // Mixer-wide peak auto-leveling is retired in favor of model metadata plus
    // explicit input/output controls. Keep the message for compatibility but
    // force the legacy path off.
    mPresetMixer.SetAutoLevelInput(false);
    mPresetMixer.SetAutoLevelOutput(false);

    nlohmann::json message;
    message["type"] = "autoLevelChanged";
    message["autoInput"] = false;
    message["autoOutput"] = false;
    SendMessageToUI(message.dump());
}

void PluginController::HandleLoadModelRequest(const nlohmann::json& payload)
{
    std::string path = payload.value("path", "");

    if (path.empty())
    {
        path = payload.value("filePath", "");
    }

    if (path.empty())
    {
        return;
    }

    std::filesystem::path filePath = util::PathFromUtf8(path);

    if (!std::filesystem::exists(filePath))
    {
        ReportErrorToUI("Model file not found", path);
        return;
    }

    std::string resourceError;
    const auto savedResource = SaveLocalLibraryResource(
        nlohmann::json{{"resourceType", "nam"},
                       {"filePath", util::PathToUtf8(filePath)},
                       {"name", util::PathToUtf8(filePath.stem())},
                       {"category", "Local"},
                       {"metadata", nlohmann::json::object({{"provider", kLocalResourceProvider}})}},
        resourceError, true);

    if (!savedResource)
    {
        ReportErrorToUI("Model load failed",
                        resourceError.empty() ? "Could not register model in the resource library" : resourceError);
        return;
    }

    const bool updatedNamResource =
        UpdateResourceForNodeType(EffectGuids::kAmpNamOptimized, savedResource->type, filePath) ||
        UpdateResourceForNodeType(EffectGuids::kAmpNamBlend, savedResource->type, filePath) ||
        UpdateResourceForNodeType(EffectGuids::kFxNam, savedResource->type, filePath) ||
        UpdateResourceForNodeType(EffectGuids::kAmpNam, savedResource->type, filePath);

    if (updatedNamResource)
    {
        mAppSettings["lastModelPath"] = util::PathToUtf8(filePath.parent_path());
        SaveAppSettings();

        nlohmann::json message;
        message["type"] = "modelLoaded";
        message["path"] = util::PathToUtf8(filePath);
        SendMessageToUI(message.dump());
    }
}

void PluginController::HandleLoadIRRequest(const nlohmann::json& payload)
{
    std::string path = payload.value("path", "");

    if (path.empty())
    {
        path = payload.value("filePath", "");
    }

    if (path.empty())
    {
        return;
    }

    std::filesystem::path filePath = util::PathFromUtf8(path);

    if (!std::filesystem::exists(filePath))
    {
        ReportErrorToUI("IR file not found", path);
        return;
    }

    if (UpdateResourceForNodeType(EffectGuids::kCabIr, "ir", filePath))
    {
        mAppSettings["lastIRPath"] = util::PathToUtf8(filePath.parent_path());
        SaveAppSettings();

        nlohmann::json message;
        message["type"] = "irLoaded";
        message["path"] = util::PathToUtf8(filePath);
        SendMessageToUI(message.dump());
    }
}

void PluginController::HandleGetSignalDiagnosticsRequest()
{
    // The UI only asks for this on startup or after a reload, when it has no roster to
    // resolve frames against, so always re-send the roster alongside the next frame.
    mTelemetry->MarkRosterDirty();
    mTelemetry->RequestSignalDiagnostics();
}

void PluginController::HandleGetPerformanceStatsRequest()
{
    mTelemetry->RequestPerformanceStats();
}

void PluginController::HandleSetSignalDiagnosticsEnabledRequest(const nlohmann::json& payload)
{
    // No preference to store: diagnostics follow UI visibility (see "uiVisibility"). The
    // message survives as the UI's request for a fresh node roster, which implies it is up.
    (void)payload;
    mPresetMixer.SetSignalDiagnosticsEnabled(mTelemetry->IsUiVisible());
    mTelemetry->MarkRosterDirty();
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

void PluginController::HandleGetGlobalChainRequest()
{
    SendGlobalChainStateToUI();
}

void PluginController::HandleSetGlobalChainRequest(const nlohmann::json& payload)
{
    // Full global chain config replacement
    if (payload.contains("config"))
    {
        auto config = payload["config"].get<GlobalSignalChainConfig>();

        // Build off the lock, install under it — see PrepareGlobalChainSwap().
        mPresetMixer.PrepareGlobalChainSwap(config);

        {
            std::lock_guard<std::mutex> dspLock(mDSPMutex);
            mPresetMixer.CommitGlobalChainSwap();
        }
        PersistGlobalFxSettingsToAppSettings();
    }

    SendGlobalChainStateToUI();
}

void PluginController::HandleSetNodeEnabledRequest(const nlohmann::json& payload)
{
    const std::string fallbackId = mActivePresetId.empty() ? "p1" : mActivePresetId;
    std::string presetId = payload.value("presetId", fallbackId);
    std::string nodeId = payload.value("nodeId", "");
    bool enabled = payload.value("enabled", true);
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetNodeEnabled(presetId, nodeId, enabled);
    UpdateHostLatency();
}

void PluginController::HandleSetNodeParamRequest(const nlohmann::json& payload)
{
    std::string presetId = payload.value("presetId", "p1");
    std::string nodeId = payload.value("nodeId", "");
    std::string key = payload.value("key", "");
    double value = payload.value("value", 0.0);
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetNodeParam(presetId, nodeId, key, value);
    UpdateHostLatency();
}

void PluginController::HandleLoadNodeResourceRequest(const nlohmann::json& payload)
{
    std::string presetId = payload.value("presetId", "p1");
    std::string nodeId = payload.value("nodeId", "");
    ResourceRef ref;

    if (payload.contains("resourceType"))
    {
        ref.resourceType = payload["resourceType"].get<std::string>();
    }

    if (payload.contains("resourceId"))
    {
        ref.resourceId = payload["resourceId"].get<std::string>();
    }

    if (payload.contains("filePath"))
    {
        ref.filePath = payload["filePath"].get<std::string>();
    }

    const bool loaded = mPresetMixer.LoadNodeResource(presetId, nodeId, ref);

    if (!loaded && ReportHostedPluginResourceLoadFailure(nodeId, ref))
    {
        DiscardFailedHostedPluginResourceSelection(nodeId, ref);
    }
    else if (loaded)
    {
        NotifyHostedPluginResourceLoadCompleted(nodeId, ref);
    }

    UpdateHostLatency();
}

void PluginController::HandleSetTunerEnabledRequest(const nlohmann::json& payload)
{
    bool enabled = payload.value("enabled", false);
    mTuner->SetActive(enabled);
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetTunerEnabled(enabled);
}

void PluginController::HandleSetTunerReferenceRequest(const nlohmann::json& payload)
{
    double freq = payload.value("frequency", 440.0);
    std::lock_guard<std::mutex> lock(mDSPMutex);
    mPresetMixer.SetTunerReferenceFrequency(freq);
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

void PluginController::ResetNamNodeLevelState(const std::string& nodeId)
{
    if (nodeId.empty() || !mActivePreset)
    {
        return;
    }

    auto* node = mActivePreset->graph.FindNode(nodeId);

    if (!node || !IsNamEffectType(node->type))
    {
        return;
    }

    ClearNamCalibrationParams(*node);

    if (!node->params.contains("useCalibration"))
    {
        node->params["useCalibration"] = 1.0;
    }

    mActivePresetJson = PresetStorage::SerializeToJson(*mActivePreset);
    mPendingStateBroadcast = true;

    if (!mActivePresetId.empty())
    {
        const double clearValue = std::numeric_limits<double>::quiet_NaN();
        const auto useCalibrationIt = node->params.find("useCalibration");
        const double useCalibrationValue =
            (useCalibrationIt != node->params.end() && useCalibrationIt->second <= 0.5) ? 0.0 : 1.0;
        mPresetMixer.SetNodeParam(mActivePresetId, nodeId, "useCalibration", useCalibrationValue);

        // Re-inject current interface calibration level for this NAM node.
        // Keep this value resident even if no model is currently resolved so
        // calibration takes effect immediately when a model loads.
        const double calLevelToInject =
            std::isfinite(mNamInterfaceCalibrationLevelDbu) ? mNamInterfaceCalibrationLevelDbu : clearValue;
        mPresetMixer.SetNodeParam(mActivePresetId, nodeId, "calibrationInputLevel", calLevelToInject);
    }
}

void PluginController::ClearNamCalibrationParams(GraphNode& node) const
{
    node.params.erase("calibrationInputLevel");
    node.params.erase("calibrationOutputLevel");
}

// ── Settings persistence ───────────────────────────────────────────

void PluginController::TouchSharedSyncState(const std::vector<std::string>& domains) const
{
    if (domains.empty())
    {
        return;
    }

    nlohmann::json payload = nlohmann::json::object();
    payload["updatedAt"] = BuildUtcIsoTimestamp();
    payload["domains"] = nlohmann::json::array();

    for (const auto& domain : domains)
    {
        if (domain.empty())
        {
            continue;
        }

        payload["domains"].push_back(domain);
    }

    const auto instanceIdIt = mAppSettings.find("app.instanceId");

    if (instanceIdIt != mAppSettings.end() && instanceIdIt->is_string())
    {
        payload["writerInstanceId"] = instanceIdIt->get<std::string>();
    }

    // The counter has to be read and written under the same lock. Reading it
    // outside the transaction lets two instances see the same version and both
    // write version+1, so one instance's notification is lost and every other
    // instance keeps showing stale data until the next unrelated change.
    std::uint64_t nextVersion = 1;
    const bool wrote = Store().Transact([&]() {
        nextVersion = 1;

        if (const auto previous = Store().Get(storage::ItemType::kDocument, kSharedSyncStateDocumentId))
        {
            const auto versionIt = previous->find("version");

            if (versionIt != previous->end() && versionIt->is_number_unsigned())
            {
                nextVersion = versionIt->get<std::uint64_t>() + 1;
            }
        }

        payload["version"] = nextVersion;
        return Store().Put(storage::ItemType::kDocument, kSharedSyncStateDocumentId, payload);
    });

    if (!wrote)
    {
        std::cerr << "[Plugin] TouchSharedSyncState failed" << std::endl;
        return;
    }

    mSharedSyncVersionSeen = nextVersion;
    mSharedSyncVersionSeenInitialized = true;
}

std::filesystem::path PluginController::ResolveDocumentStorePath() const
{
    // Deliberately the real profile, not GetEffectiveSettingsDirectory(): while
    // a preset-archive session is active that points at a throwaway directory
    // which is deleted when the session ends. The session shadows presets and
    // folders on purpose, but the database itself must never move.
    return mFileSystem.ResolveSettingsDirectory() / "soundshed.db";
}

storage::JsonStore& PluginController::Store() const
{
    // call_once, not a bool: concurrent first-touches from the message thread
    // and a folder-scan worker would otherwise race, and two Open() calls on one
    // handle would leave the loser thinking the store is unavailable.
    std::call_once(mStoreOpenOnce, [this]() { OpenDocumentStore(); });

    return mStore;
}

void PluginController::OpenDocumentStore() const
{
    // Resolve the host's user-data path first, and discard it.
    //
    // On macOS this is what triggers migrateDataOutOfSandboxContainerOnce(),
    // which copies a pre-sandbox-removal profile out of
    // ~/Library/Containers/... into ~/Library/Soundshed Guitar. That copy is
    // skipped if the destination already contains anything, and opening the
    // store creates <destination>/data/v1/ — so opening first would strand the
    // user's entire library in the container, silently and permanently.
    //
    // Initialize() happens to call this before anything else today; this makes
    // it a guarantee rather than an ordering coincidence, which matters now that
    // the store opens lazily on first touch.
    (void)mHost.GetUserDataPath();

    const auto dbPath = ResolveDocumentStorePath();

    std::string error;
    auto status = mStore.OpenChecked(dbPath, error);

    if (status == storage::JsonStore::OpenStatus::Damaged)
    {
        // A damaged file is the one failure worth recovering from here, and
        // leaving it in place is not a recovery: the migration stamp lives
        // *inside* the database, so a store that refuses to open means an empty
        // library on every subsequent launch, forever, with nothing but a log
        // line to explain it.
        //
        // Moving it aside makes the next Open() create a fresh database with no
        // stamp, which re-runs the legacy import and puts the user back to their
        // pre-upgrade library. The damaged file is kept, not deleted, so anything
        // salvageable can still be recovered from it by hand.
        const auto quarantinePath =
            dbPath.parent_path() / (dbPath.filename().string() + ".damaged-" +
                                    std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                                       std::chrono::system_clock::now().time_since_epoch())
                                                       .count()));

        std::error_code renameEc;
        std::filesystem::rename(dbPath, quarantinePath, renameEc);

        if (renameEc)
        {
            AppendSessionLog("The document store at " + dbPath.string() +
                             " is damaged and could not be moved aside: " + renameEc.message());
            std::cerr << "[Plugin] Damaged document store could not be quarantined: " << renameEc.message()
                      << std::endl;
            return;
        }

        // The -wal and -shm sidecars belong to the quarantined file; leaving
        // them would let sqlite try to replay them into the new database.
        for (const char* suffix : {"-wal", "-shm"})
        {
            std::error_code sidecarEc;
            std::filesystem::remove(dbPath.parent_path() / (dbPath.filename().string() + suffix), sidecarEc);
        }

        AppendSessionLog("The document store at " + dbPath.string() + " was damaged (" + error +
                         "). It has been moved to " + quarantinePath.string() +
                         " and the library will be rebuilt from the legacy files.");
        std::cerr << "[Plugin] Damaged document store quarantined as " << quarantinePath.string() << std::endl;

        error.clear();
        status = mStore.OpenChecked(dbPath, error);
    }

    if (status != storage::JsonStore::OpenStatus::Ok)
    {
        // Everything downstream degrades to empty-and-read-only rather than
        // crashing, and the legacy tree is still on disk untouched, so the user
        // loses this session's changes but never their library.
        AppendSessionLog("Could not open the document store at " + dbPath.string() + ": " + error);
        std::cerr << "[Plugin] Document store unavailable: " << error << std::endl;
        return;
    }

    // Both paths are recomputed rather than read from members: the store can be
    // opened lazily before Initialize() has filled those in, and importing with
    // an empty preset directory would stamp the schema version having silently
    // skipped every preset.
    const auto report = storage::MigrateLegacyJsonTree(mStore, mFileSystem.ResolveSettingsDirectory(),
                                                       mFileSystem.ResolvePresetDirectory() / "user");

    if (report.ran)
    {
        std::string summary = "Imported the legacy JSON tree into " + dbPath.string() + ": " +
                              std::to_string(report.itemsImported) + " items";

        for (const auto& note : report.notes)
        {
            summary += "\n  " + note;
        }

        for (const auto& failure : report.failures)
        {
            summary += "\n  ! " + failure;
        }

        AppendSessionLog(summary);
        std::cout << "[Plugin] " << summary << std::endl;
    }
}

std::string PluginController::UiStorageDocumentId(const std::string& filename)
{
    // The legacy filename is the document id, minus the extension. Keeping the
    // mapping mechanical means callers keep passing the names they always did
    // and the migration lands documents under exactly these ids.
    const auto dot = filename.rfind(".json");
    return dot == std::string::npos ? filename : filename.substr(0, dot);
}

nlohmann::json PluginController::LoadUiStorageJson(const std::string& filename, const nlohmann::json& fallback) const
{
    // These are envelope documents — a collection plus the state that surrounds
    // it (activeFolderId, bankSize, cursorIndex). They are small and always
    // rewritten whole, so they live as one row each rather than being split into
    // per-item rows the way presets and resources are.
    //
    // A stored JSON `null` is treated as absent: Get() returns it as a value
    // rather than nullopt, and handing a null to callers that expect the shape
    // of `fallback` pushes the failure somewhere harder to read.
    if (auto stored = Store().Get(storage::ItemType::kDocument, UiStorageDocumentId(filename));
        stored && !stored->is_null())
    {
        return *stored;
    }

    return fallback;
}

void PluginController::SaveUiStorageJson(const std::string& filename, const nlohmann::json& payload) const
{
    const bool wrote = Store().Put(storage::ItemType::kDocument, UiStorageDocumentId(filename), payload);

    if (!wrote)
    {
        AppendSessionLog("Failed to save UI storage document: " + UiStorageDocumentId(filename));
        return;
    }

    std::vector<std::string> domains;

    if (filename == "automation.json")
    {
        domains.push_back("automation");
    }
    else if (filename == "setlists.json")
    {
        domains.push_back("setlists");
    }
    else if (filename == "preset-folders.json" || filename == "preset-favorites.json" ||
             filename == "preset-ratings.json")
    {
        domains.push_back("presetMetadata");
    }
    else
    {
        domains.push_back("uiStorage");
    }

    TouchSharedSyncState(domains);
}

bool PluginController::WriteFile(const std::filesystem::path& target, const std::vector<std::uint8_t>& data) const
{
    try
    {
        std::ofstream ofs(target, std::ios::binary);

        if (!ofs.is_open())
        {
            return false;
        }

        ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}
} // namespace guitarfx
