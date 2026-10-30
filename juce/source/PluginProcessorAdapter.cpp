/**
 * PluginProcessorAdapter.cpp — JUCE thin adapter implementation.
 *
 * All business logic (DSP, presets, message handling) is delegated to
 * PluginController from soundshed-guitar core. This file only contains
 * JUCE-specific glue code.
 */

#include "PluginProcessorAdapter.h"
#include "JuceHostedPluginEffect.h"
#include "JucePathConversion.h"
#include "ProductInfo.h"
#include "ProfileFolder.h"
#include "editor/UiResourcePrefetch.h"

#include "PluginEditor.h"
#include "UiBridge.h"

#if SOUNDSHED_NANOQ_UI
    #include "NanoQUiEditor.h"
#endif

#include "ControllerMidiOutput.h"
#include "resources/PluginPathUtils.h"

// Same include order as Main.cpp: the standalone window header expects the audio and GUI
// modules to be visible before it. Used only to read the standalone device's channels.
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include "util/FileSystem.h"
#include "util/SessionLog.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>
#include <thread>

#ifdef _WIN32
    #include <shobjidl.h>
    #include <windows.h>
    #include <wrl/client.h>
#endif

namespace
{
#ifdef _WIN32
struct ScopedComInitializer
{
    HRESULT hr;

    ScopedComInitializer() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))
    {
    }

    ~ScopedComInitializer()
    {
        if (SUCCEEDED(hr))
        {
            CoUninitialize();
        }
    }

    ScopedComInitializer(const ScopedComInitializer&) = delete;
    ScopedComInitializer& operator=(const ScopedComInitializer&) = delete;
};
#endif

#if JUCE_MAC
/**
 * One-time migration of the standalone's user data out of the old App Sandbox
 * container.
 *
 * While core/macos.standalone.entitlements enabled com.apple.security.app-sandbox,
 * macOS redirected the standalone's "~/Library" into
 * ~/Library/Containers/com.soundshed.guitar/Data/Library. Now that the sandbox is
 * off, the same juce::File::userApplicationDataDirectory lookup resolves to the
 * real ~/Library, so an existing install would otherwise start up with no
 * settings, presets, resource library or riff library.
 *
 * The container tree is copied across the first time we see it, and never
 * deleted, so this stays reversible by hand. If the destination already holds
 * data we leave both trees alone rather than guess at a merge: the plugin
 * builds were never sandboxed, so anyone who ran the VST3/AU already has a
 * populated ~/Library/Soundshed Guitar that must not be clobbered.
 */
void migrateDataOutOfSandboxContainerOnce(const juce::File& destination)
{
    static std::once_flag onceFlag;
    std::call_once(onceFlag, [&destination] {
        // Bundle ID is BUNDLE_ID in juce/CMakeLists.txt.
        const auto container = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                                   .getChildFile("Library/Containers/com.soundshed.guitar/Data/Library")
                                   .getChildFile(destination.getFileName());

        if (container == destination || !container.isDirectory())
        {
            return;
        }

        // ignoreHiddenFiles so a stray .DS_Store does not count as "has data"
        // and block the migration.
        if (destination.isDirectory() &&
            destination.getNumberOfChildFiles(juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles) > 0)
        {
            guitarfx::util::AppendSessionLog(
                ("[PluginProcessorAdapter] Sandbox container data at " + container.getFullPathName() +
                 " left in place: " + destination.getFullPathName() + " already holds data.")
                    .toStdString());
            return;
        }

        if (container.copyDirectoryTo(destination))
        {
            guitarfx::util::AppendSessionLog(
                ("[PluginProcessorAdapter] Migrated user data out of the sandbox container: " +
                 container.getFullPathName() + " -> " + destination.getFullPathName())
                    .toStdString());
        }
        else
        {
            guitarfx::util::AppendSessionLog(("[PluginProcessorAdapter] ERROR: failed to migrate user data from " +
                                              container.getFullPathName() + " to " + destination.getFullPathName())
                                                 .toStdString());
        }
    });
}
#endif

#if JUCE_LINUX
class HeadlessLv2ManifestEditor final : public juce::AudioProcessorEditor
{
  public:
    explicit HeadlessLv2ManifestEditor(juce::AudioProcessor& processor) : juce::AudioProcessorEditor(&processor)
    {
        setResizable(true, true);
        setResizeLimits(640, 400, 8192, 8192);
        setSize(1200, 900);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colours::black);
    }

    void resized() override
    {
    }
};

bool shouldUseHeadlessLv2ManifestEditor(const PluginProcessorAdapter& processor)
{
    return processor.wrapperType == juce::AudioProcessor::wrapperType_LV2 && std::getenv("DISPLAY") == nullptr &&
           std::getenv("WAYLAND_DISPLAY") == nullptr;
}
#endif
} // namespace

// ════════════════════════════════════════════════════════════════════════
// AutomationSlotParameter — JUCE parameter backed by an automation slot
// ════════════════════════════════════════════════════════════════════════

class PluginProcessorAdapter::AutomationSlotParameter : public juce::AudioProcessorParameter
{
  public:
    /// `sceneSteps` above 0 makes it the Scene parameter: that many steps, each shown as its scene.
    AutomationSlotParameter(PluginProcessorAdapter& owner, int parameterIndex, juce::String paramID, juce::String label,
                            int sceneSteps = 0)
        : mOwner(owner), mParameterIndex(parameterIndex), mParamID(std::move(paramID)), mLabel(std::move(label)),
          mSceneSteps(sceneSteps)
    {
    }

    [[nodiscard]] int getParameterIndex() const
    {
        return mParameterIndex;
    }

    [[nodiscard]] const juce::String& getSlotId() const
    {
        return mParamID;
    }

    [[nodiscard]] float getValue() const override
    {
        // Any thread, the audio thread included (JUCE's VST3 wrapper compares against it while
        // applying the host's automation), so it reads the parameter's own cell without a lock.
        return mOwner.mController.GetDawParameterValue(mParameterIndex);
    }

    void setValue(float newValue) override
    {
        // While audio runs, the change waits for processBlock, which applies it under the
        // DSP lock ahead of the block it belongs to.
        //
        // Nothing drains that queue before the first prepareToPlay or after
        // releaseResources, so a value set then would never reach the chain, and
        // getValue() would go on reporting the old one. Hosts do set values then: a CLAP
        // host flushes parameter changes into a plugin it has not activated, which is
        // what clap-validator's param-set tests check. With no audio thread to race,
        // apply it now.
        if (!mOwner.mAudioActive.load(std::memory_order_acquire))
        {
            mOwner.mController.ApplyAutomationFromDAW(mParamID.toStdString(), newValue);
            return;
        }

        std::lock_guard<std::mutex> lock(mOwner.mPendingDAWParamMutex);
        mOwner.mPendingDAWParamChanges.emplace_back(mParameterIndex, newValue);
    }

    [[nodiscard]] float getDefaultValue() const override
    {
        return 0.0f;
    }

    [[nodiscard]] juce::String getName(int maximumLength) const override
    {
        return mLabel.substring(0, maximumLength);
    }

    [[nodiscard]] juce::String getLabel() const override
    {
        return {};
    }

    [[nodiscard]] int getNumSteps() const override
    {
        return mSceneSteps > 0 ? mSceneSteps : juce::AudioProcessorParameter::getNumSteps();
    }

    [[nodiscard]] bool isDiscrete() const override
    {
        return mSceneSteps > 0;
    }

    [[nodiscard]] bool isBoolean() const override
    {
        return false;
    }

    [[nodiscard]] bool isOrientationInverted() const override
    {
        return false;
    }

    [[nodiscard]] juce::String getText(float value, int) const override
    {
        if (mSceneSteps > 0)
        {
            return "Scene " + juce::String(juce::roundToInt(value * static_cast<float>(mSceneSteps - 1)) + 1);
        }

        return juce::String(value, 3);
    }

    [[nodiscard]] float getValueForText(const juce::String& text) const override
    {
        if (mSceneSteps > 0)
        {
            // "Scene 3" or "3".
            const int scene = juce::jlimit(1, mSceneSteps, text.retainCharacters("0123456789").getIntValue());
            return static_cast<float>(scene - 1) / static_cast<float>(mSceneSteps - 1);
        }

        return text.getFloatValue();
    }

    [[nodiscard]] bool isAutomatable() const override
    {
        return true;
    }

    [[nodiscard]] bool isMetaParameter() const override
    {
        return false;
    }

    [[nodiscard]] juce::AudioProcessorParameter::Category getCategory() const override
    {
        return genericParameter;
    }

  private:
    PluginProcessorAdapter& mOwner;
    int mParameterIndex = 0;
    juce::String mParamID;
    juce::String mLabel;
    int mSceneSteps = 0;
};

// ════════════════════════════════════════════════════════════════════════
// Construction / Destruction
// ════════════════════════════════════════════════════════════════════════

PluginProcessorAdapter::PluginProcessorAdapter()
    : AudioProcessor(BusesProperties()
#if !JucePlugin_IsMidiEffect
    #if !JucePlugin_IsSynth
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
    #endif
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
                         ),
      mController(*this)
{
    guitarfx::RegisterJuceHostedPluginEffect();
#if JUCE_ANDROID
    // Android has no HOME to derive a settings root from, so hand the core the
    // sandbox path before anything asks it for one.  Must happen before
    // mController.Initialize(), which reads settings.
    guitarfx::FileSystem::SetPlatformRootOverride(
        std::filesystem::path(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                                  .getChildFile("files")
                                  .getFullPathName()
                                  .toStdString()));
#endif
    mAssetRoot = locateAssetsRoot();

    // The standalone's page loads once the engine and audio device are up; read its files meanwhile.
    if (wrapperType == wrapperType_Standalone && !mAssetRoot.empty())
    {
        soundshed::editor::prefetchUiResources(soundshed::toJuceFile(mAssetRoot).getChildFile("ui"));
    }

    mController.Initialize();
    registerAutomationParameters();

    // Metering off until an editor opens and turns it on (SoundshedEditorBase), or a DAW
    // instance whose editor is never opened meters every block all session.
    soundshed::editor::sendUiMessage(*this, "uiVisibility", {{"visible", false}});

    // A footswitch mapped to a setlist preset or scene must work with the editor closed, and a
    // node parameter automation moves must reach the working copy a save is built from. The
    // audio thread cannot hand either over itself (callAsync allocates), so they wait for the
    // message thread and are polled for here; a tick with nothing waiting is two atomic loads.
    // Without a message manager there is no loop to run it on (see IsMessageThread).
    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr)
    {
        mControlSurfaceDrain.startTimerHz(kControlSurfaceDrainHz);
    }
}

PluginProcessorAdapter::~PluginProcessorAdapter()
{
    // Before the members go: a task posted to the message thread that runs after this finds
    // nothing rather than a freed instance.
    masterReference.clear();
}

// ════════════════════════════════════════════════════════════════════════
// juce::AudioProcessor overrides
// ════════════════════════════════════════════════════════════════════════

void PluginProcessorAdapter::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    mController.Prepare(sampleRate, samplesPerBlock);
#if JUCE_ANDROID
    mAudioThreadPlacement.prepare(sampleRate);
#endif
    // A second of latency covers any effect or hosted plugin here; more is clamped.
    mBypassDelay.setSize(2, juce::jmax(1, static_cast<int>(sampleRate)));
    mBypassDelay.clear();
    mBypassDelayWritePos = 0;
    mBypassDelayPrimed = false;
    mAudioActive.store(true, std::memory_order_release);
}

void PluginProcessorAdapter::releaseResources()
{
    // Anything still queued was waiting for a block that is not coming. Apply it now,
    // or it would sit unapplied until the next prepare while getValue() reports stale
    // values. The flag goes first so a setValue racing this applies directly.
    mAudioActive.store(false, std::memory_order_release);
    applyPendingDAWParamChanges(true);
    mController.Reset();
#if JUCE_ANDROID
    mAudioThreadPlacement.release();
#endif
}

void PluginProcessorAdapter::applyPendingDAWParamChanges(bool mayBlock)
{
    // The audio thread only tries this lock: a host thread holds it while appending, which can allocate.
    std::unique_lock<std::mutex> lock(mPendingDAWParamMutex, std::try_to_lock);

    if (!lock.owns_lock() && mayBlock)
    {
        lock.lock();
    }

    if (!lock.owns_lock() || mPendingDAWParamChanges.empty())
    {
        return;
    }

    if (mController.ApplyAutomationFromDAW(mPendingDAWParamChanges, mSlotIdsByParameter, mayBlock))
    {
        mPendingDAWParamChanges.clear(); // in place: the queue keeps its capacity, so nothing is freed
    }
}

bool PluginProcessorAdapter::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto isMonoOrStereo = [](const juce::AudioChannelSet& set) {
        return set == juce::AudioChannelSet::mono() || set == juce::AudioChannelSet::stereo();
    };

    if (!isMonoOrStereo(layouts.getMainOutputChannelSet()))
    {
        return false;
    }

#if !JucePlugin_IsSynth

    // Mono or stereo in, mono or stereo out, in any pairing. A mono track's insert can then be
    // mono in, stereo out, which is how a mono guitar keeps its reverb and delay in stereo; a
    // mono output gets the stereo mix summed (see MultiPresetMixer::Process).
    if (!isMonoOrStereo(layouts.getMainInputChannelSet()))
    {
        return false;
    }

#endif

    return true;
}

void PluginProcessorAdapter::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
#if JUCE_ANDROID
    mAudioThreadPlacement.beginBlock();
#endif

    // The play head may only be asked here, on the audio thread; the controller reads tempo
    // and transport on the message thread (the metronome, a riff's tempo), so each block
    // leaves them behind. Asking from the message thread is undefined in the AU wrapper.
    if (auto* playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
        {
            const auto bpm = position->getBpm();
            mHostTempoBpm.store(bpm.hasValue() ? *bpm : 0.0, std::memory_order_relaxed);
            mHostPlaying.store(position->getIsPlaying(), std::memory_order_relaxed);
        }
    }

    // Not bypassed: the next bypassed block starts its delay line afresh.
    mBypassDelayPrimed = false;

    const auto totalInputCh = getTotalNumInputChannels();
    const auto totalOutputCh = getTotalNumOutputChannels();
    const int numSamples = buffer.getNumSamples();

    // Clear any output channels that don't have corresponding inputs
    for (auto i = totalInputCh; i < totalOutputCh; ++i)
    {
        buffer.clear(i, 0, numSamples);
    }

    // Drain MIDI messages and queue them for the controller
    if (!midiMessages.isEmpty())
    {
        for (const auto metadata : midiMessages)
        {
            const auto msg = metadata.getMessage();

            if (msg.isSysEx())
            {
                continue;
            }

            guitarfx::MidiEvent ev;
            const auto* rawData = msg.getRawData();
            ev.status = rawData[0];
            ev.data1 = rawData[1];
            ev.data2 = rawData[2];
            ev.sampleOffset = metadata.samplePosition;
            mController.EnqueueMidi(ev);
        }

        midiMessages.clear();
    }

    // Drain pending DAW parameter changes (collected by AutomationSlotParameter::setValue)
    // without blocking: a block that finds the DSP lock held retries next time. The MIDI
    // queued above is applied by ProcessAudio itself, under the lock it takes for the block.
    applyPendingDAWParamChanges(false);

    // Set up float** for the core ProcessAudio
    float* inputs[2] = {const_cast<float*>(buffer.getReadPointer(0)),
                        (totalInputCh > 1) ? const_cast<float*>(buffer.getReadPointer(1)) : nullptr};
    float* outputs[2] = {buffer.getWritePointer(0), (totalOutputCh > 1) ? buffer.getWritePointer(1) : nullptr};

    const bool processed = mController.ProcessAudio(inputs, outputs, numSamples);

    if (!processed)
    {
        // Controller couldn't acquire DSP lock — silence
        buffer.clear();
    }

    // The only MIDI this plugin sends: the preset name for the controller's display, and the
    // bank and setlist slot picked in the app. The incoming messages were cleared above.
    AddControllerMidiOutput(mController, midiMessages);
#if JUCE_ANDROID
    mAudioThreadPlacement.endBlock(numSamples);
#endif
}

void PluginProcessorAdapter::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // JUCE's default passes the incoming MIDI through to the output. With a MIDI output
    // declared, that would echo a controller's own footswitches back at it while the host
    // has the plugin bypassed.
    midiMessages.clear();
    juce::AudioProcessor::processBlockBypassed(buffer, midiMessages);

    // The default passes the audio straight through. The host delays every other track by
    // the latency this plugin reports (a pitch shifter, an oversampled amp, a hosted plugin),
    // so a bypass that skipped it put this track early by that much: the bypassed signal is
    // delayed to match, as the processed one is.
    const int capacity = mBypassDelay.getNumSamples();
    const int latency = juce::jmin(getLatencySamples(), capacity);

    if (latency <= 0)
    {
        return;
    }

    if (!mBypassDelayPrimed)
    {
        // Entering bypass: whatever an earlier bypass left in the line is not this one's.
        mBypassDelay.clear();
        mBypassDelayWritePos = 0;
        mBypassDelayPrimed = true;
    }

    const int numSamples = buffer.getNumSamples();
    const int channels = juce::jmin(buffer.getNumChannels(), mBypassDelay.getNumChannels());

    for (int ch = 0; ch < channels; ++ch)
    {
        float* data = buffer.getWritePointer(ch);
        float* line = mBypassDelay.getWritePointer(ch);
        int writePos = mBypassDelayWritePos;
        int readPos = (writePos - latency + capacity) % capacity;

        for (int i = 0; i < numSamples; ++i)
        {
            const float in = data[i];
            data[i] = line[readPos];
            line[writePos] = in;

            if (++writePos == capacity)
            {
                writePos = 0;
            }

            if (++readPos == capacity)
            {
                readPos = 0;
            }
        }
    }

    mBypassDelayWritePos = (mBypassDelayWritePos + numSamples) % capacity;
}

#ifdef HAS_CLAP_JUCE_EXTENSIONS
void PluginProcessorAdapter::addOutboundEventsToQueue(const clap_output_events* outEvents,
                                                      const juce::MidiBuffer& midiBuffer, int sampleOffset)
{
    // Called after each slice's processBlock; the first slice of a process() call is at
    // offset 0, and nothing pushed during the previous call is read any more.
    if (sampleOffset == 0)
    {
        mClapSysExArenaUsed = 0;
    }

    for (const auto metadata : midiBuffer)
    {
        const auto size = static_cast<std::size_t>(metadata.numBytes);
        const auto time = static_cast<uint32_t>(metadata.samplePosition + sampleOffset);

        if (size == 0)
        {
            continue;
        }

        if (metadata.data[0] == 0xF0)
        {
            // Dropped rather than allocated for if the arena is full. The display message is
            // 39 bytes and at most one is sent per block, so this is only a safety valve.
            if (size > mClapSysExArena.size() - mClapSysExArenaUsed)
            {
                continue;
            }

            auto* bytes = mClapSysExArena.data() + mClapSysExArenaUsed;
            std::memcpy(bytes, metadata.data, size);
            mClapSysExArenaUsed += size;

            clap_event_midi_sysex event{};
            event.header.size = sizeof(event);
            event.header.time = time;
            event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            event.header.type = CLAP_EVENT_MIDI_SYSEX;
            event.port_index = 0;
            event.buffer = bytes;
            event.size = static_cast<uint32_t>(size);
            outEvents->try_push(outEvents, &event.header);
        }
        else if (size <= 3)
        {
            clap_event_midi event{};
            event.header.size = sizeof(event);
            event.header.time = time;
            event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            event.header.type = CLAP_EVENT_MIDI;
            event.port_index = 0;
            std::memcpy(event.data, metadata.data, size);
            outEvents->try_push(outEvents, &event.header);
        }
    }
}
#endif

std::vector<juce::String> PluginProcessorAdapter::getAutomationParameterIds() const
{
    std::vector<juce::String> ids;

    for (const auto& slotId : mController.GetAutomationSlotIds())
    {
        ids.push_back(juce::String(slotId));
    }

    return ids;
}

void PluginProcessorAdapter::registerAutomationParameters()
{
    // Register one JUCE parameter per automation slot.
    //
    // Parameter *order* is load-bearing. AutomationSlotParameter derives from
    // juce::AudioProcessorParameter rather than AudioProcessorParameterWithID, so
    // JUCE derives the VST3 ParamID from the parameter index (see
    // LegacyAudioParameter::getParamID). Any change to the ordering silently
    // rebinds automation in DAW projects that were saved with the old layout.
    //
    // The layout is therefore append-only:
    //   [0]                        the original default slots, in their shipped order
    //   [..]                       custom slots, padded with reserved placeholders
    //                              up to kMaxCustomSlots
    //   [after the reserved block] default slots added after the layout shipped
    //
    // Default slots added later go *after* the reserved block rather than at the
    // end of the defaults, because inserting them there would push every custom
    // slot down and break existing projects.
    const auto slotIds = mController.GetAutomationSlotIds();

    std::vector<std::string> defaultSlotIds;
    std::vector<std::string> customSlotIds;

    for (const auto& slotId : slotIds)
    {
        if (slotId.rfind("default.", 0) == 0)
        {
            defaultSlotIds.push_back(slotId);
        }
        else
        {
            customSlotIds.push_back(slotId);
        }
    }

    int paramIndex = 0;
    std::vector<std::string> slotIdsByParameter;
    const auto addSlotParameter = [&](const juce::String& id, const juce::String& label, int sceneSteps = 0) {
        addParameter(new AutomationSlotParameter(*this, paramIndex, id, label, sceneSteps));
        slotIdsByParameter.push_back(id.toStdString());
        ++paramIndex;
    };
    const auto addSlot = [&](const std::string& slotId) {
        const auto* slot = mController.GetAutomationSlots().FindSlot(slotId);
        // A default slot's address never changes, so the Scene parameter stays one for the
        // host's lifetime of the plugin.
        const bool isScene = slot != nullptr && slot->isDefault && slot->address == guitarfx::kSceneSelectAddress;
        addSlotParameter(juce::String(slotId), slot ? juce::String(slot->label) : juce::String(slotId),
                         isScene ? guitarfx::kSceneSelectSteps : 0);
    };

    const int stableDefaultCount =
        std::min(guitarfx::kLayoutStableDefaultSlots, static_cast<int>(defaultSlotIds.size()));

    for (int i = 0; i < stableDefaultCount; ++i)
    {
        addSlot(defaultSlotIds[static_cast<std::size_t>(i)]);
    }

    for (const auto& slotId : customSlotIds)
    {
        addSlot(slotId);
    }

    // Reserve placeholders for unused custom slots so adding a custom slot later
    // doesn't shift anything either.
    for (int i = static_cast<int>(customSlotIds.size()); i < guitarfx::kMaxCustomSlots; ++i)
    {
        addSlotParameter("custom._reserved_" + juce::String(i), "Reserved " + juce::String(i));
    }

    for (std::size_t i = static_cast<std::size_t>(stableDefaultCount); i < defaultSlotIds.size(); ++i)
    {
        addSlot(defaultSlotIds[i]);
    }

    // Before the host has the plugin, so before any thread can call getValue().
    mController.BindDawParameters(slotIdsByParameter);

    // The DAW queue resolves parameter indices through this, and is sized for a burst of
    // changes to every parameter several times over before it would have to grow.
    mSlotIdsByParameter = std::move(slotIdsByParameter);
    mPendingDAWParamChanges.reserve(mSlotIdsByParameter.size() * 8);
}

juce::AudioProcessorEditor* PluginProcessorAdapter::createEditor()
{
    // soundshed:// links open tone sharing.
    ensureStandaloneProtocolHandlerRegistration();

#if JUCE_LINUX

    // JUCE's LV2 manifest helper instantiates the editor in headless CI just to query
    // resize metadata. Avoid constructing the real WebView-based editor in that path.
    if (shouldUseHeadlessLv2ManifestEditor(*this))
    {
        return new HeadlessLv2ManifestEditor(*this);
    }

#endif

#if SOUNDSHED_NANOQ_UI
    // The spike build (-DSOUNDSHED_NANOQ_UI=ON): Nano Q's native UI instead of the WebView.
    return new NanoQUiEditor(*this);
#else
    return new PluginEditor(*this);
#endif
}

bool PluginProcessorAdapter::hasEditor() const
{
    return true;
}

const juce::String PluginProcessorAdapter::getName() const
{
    return JucePlugin_Name;
}

bool PluginProcessorAdapter::acceptsMidi() const
{
    return true;
}

bool PluginProcessorAdapter::producesMidi() const
{
    return true;
} // must match NEEDS_MIDI_OUTPUT in juce/CMakeLists.txt

bool PluginProcessorAdapter::isMidiEffect() const
{
    return false;
}

// Delays and reverbs ring on after the input stops. A host that was told zero here cut a tail
// off at a region's end, or stopped calling the plugin while one was still sounding.
double PluginProcessorAdapter::getTailLengthSeconds() const
{
    return 10.0;
}

// The program calls come on whatever thread the host uses: AU's factory presets, LV2's program
// presets. The getters read the setlist from the document store, which locks itself, and the
// cursor, which is atomic. setCurrentProgram loads a preset, so the controller hands it to the
// message thread (see PluginController::ApplySetlistPresetByIndex).
int PluginProcessorAdapter::getNumPrograms()
{
    return std::max(1, mController.GetSetlistLength());
}

int PluginProcessorAdapter::getCurrentProgram()
{
    const int count = std::max(1, mController.GetSetlistLength());
    return std::clamp(mController.GetSetlistCursorIndex(), 0, count - 1);
}

void PluginProcessorAdapter::setCurrentProgram(int index)
{
    mController.ApplySetlistPresetByIndex(index);
}

const juce::String PluginProcessorAdapter::getProgramName(int index)
{
    const auto presetId = mController.GetSetlistSlotPresetId(index);

    if (!presetId.empty())
    {
        return juce::String(presetId);
    }

    return "Program " + juce::String(index + 1);
}

void PluginProcessorAdapter::changeProgramName(int, const juce::String&)
{
}

void PluginProcessorAdapter::getStateInformation(juce::MemoryBlock& destData)
{
    // Any thread: the controller builds on the message thread and hands requests from
    // elsewhere over to it (see PluginController::SerializeState).
    const auto controllerState = mController.SerializeState();
    juce::MemoryOutputStream stream(destData, false);
    stream.write(controllerState.data(), controllerState.size());
}

void PluginProcessorAdapter::setStateInformation(const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
    {
        return;
    }

    std::string controllerState(reinterpret_cast<const char*>(data), static_cast<size_t>(sizeInBytes));

    if (controllerState.empty())
    {
        return;
    }

    // Any thread: the controller restores on the message thread and hands calls from elsewhere
    // over to it (see PluginController::DeserializeState). If the message thread could not get
    // to it in time, nothing has changed yet, and the host hears about it once it has instead
    // (NotifyDeferredStateRestored).
    if (mController.DeserializeState(controllerState))
    {
        notifyHostOfRestoredState();
    }
}

void PluginProcessorAdapter::NotifyDeferredStateRestored()
{
    // VST3 misses out here. Its host re-read every parameter in setComponentState, which came
    // too early, and the program change below does not reach it (see notifyHostOfRestoredState).
    // Only a host breaking VST3's rule that state is set on the UI thread can land here.
    notifyHostOfRestoredState();
}

void PluginProcessorAdapter::notifyHostOfRestoredState()
{
    // The automation parameters' values are part of that state, so a restore changes what
    // they report, and the host has to be asked to read them again. JUCE turns a program
    // change into CLAP's values rescan; values that change on load without it are a bug to a
    // CLAP host and fail clap-validator's state tests. AU keeps it as well, because JUCE's AU
    // restore sends no change notification of its own.
    //
    // Not for VST3. The host follows setState with setComponentState, which already re-reads
    // every parameter and raises kParamValuesChanged. All a program change adds there is JUCE's
    // Program parameter: once the setlist cursor has moved, it reports that as a
    // begin/perform/end edit from inside setState, and hosts that turn plugin gestures into
    // undo steps record the restore (their own undo, say) as a new edit and drop their redo.
    if (wrapperType != wrapperType_VST3)
    {
        updateHostDisplay(juce::AudioProcessor::ChangeDetails().withProgramChanged(true));
    }
}

void PluginProcessorAdapter::setNonRealtime(bool isNonRealtime) noexcept
{
    juce::AudioProcessor::setNonRealtime(isNonRealtime);

    // Any thread, the audio thread included: JUCE's VST3 wrapper calls this from process() every
    // block. The controller only records the mode, so it takes no lock and cannot throw.
    mController.SetOfflineRendering(isNonRealtime);
}

// ════════════════════════════════════════════════════════════════════════
// IPluginHost implementation
// ════════════════════════════════════════════════════════════════════════

void PluginProcessorAdapter::SendMessageToUI(const std::string& jsonMessage)
{
    // evaluateJavascript must be called on the message thread.
    // When called from the audio thread (e.g. riffCaptureStarted/Progress/Stopped),
    // dispatch asynchronously; when already on the message thread call directly.
    auto msg = juce::String(jsonMessage);

    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        sendMessageToUI(msg);
    }
    else
    {
        // A message still queued when the host removes the instance must find it gone.
        juce::MessageManager::callAsync([weakThis = juce::WeakReference<PluginProcessorAdapter>(this), msg]() {
            if (auto* self = weakThis.get())
            {
                self->sendMessageToUI(msg);
            }
        });
    }
}

void PluginProcessorAdapter::BrowseFileAsync(guitarfx::BrowseFileType type, const std::string& title,
                                             std::function<void(const guitarfx::BrowseFileResult&)> callback)
{
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        juce::MessageManager::callAsync([weakThis = juce::WeakReference<PluginProcessorAdapter>(this), type, title,
                                         callback = std::move(callback)]() mutable {
            if (auto* self = weakThis.get())
            {
                self->BrowseFileAsync(type, title, std::move(callback));
            }
        });
        return;
    }

    // One dialog at a time. Replacing the chooser while its native dialog was open (a second
    // click on Browse) destroyed it under that dialog, and the first request never answered.
    if (mFileChooser != nullptr)
    {
        if (callback)
        {
            callback(guitarfx::BrowseFileResult{});
        }

        return;
    }

    juce::String filters;

    switch (type)
    {
    case guitarfx::BrowseFileType::NAMModel:
        filters = "*.nam";
        break;
    case guitarfx::BrowseFileType::IRFile:
        filters = "*.wav;*.aiff;*.aif;*.flac";
        break;
    case guitarfx::BrowseFileType::PresetFile:
        filters = "*.json";
        break;
    case guitarfx::BrowseFileType::ImageFile:
        filters = "*.png;*.jpg;*.jpeg;*.svg";
        break;
    case guitarfx::BrowseFileType::AudioFile:
        // FLAC/OGG are accepted here but our decoder only supports WAV/AIFF/MP3;
        // picking one reports "unsupported format" the same way DemoPreviewService does.
        filters = "*.wav;*.mp3;*.flac;*.ogg;*.aiff;*.aif";
        break;
    case guitarfx::BrowseFileType::ArchiveFile:
        filters = "*.soundshed.preset;*.soundshed.presets;*.zip";
        break;
    case guitarfx::BrowseFileType::PluginFile:
        // Bundle *payloads* are offered alongside the bundles themselves, because
        // no native dialog reliably lets a folder be selected while a file filter
        // is active. Whichever the user picks, ResolvePluginBundlePath normalizes
        // it to the bundle root, so every route stores the same path.
        //
        // Legacy VST2 is listed deliberately even though it cannot be loaded:
        // hiding it only turns "VST2 is not supported, use the VST3 version" into
        // "my plugin isn't in the list". Selecting one reaches the loader, which
        // explains the problem — see DescribeUnsupportedPluginFile. CLAP and AAX
        // cannot be hosted at all, so PluginBrowseFilters leaves those out.
        filters = guitarfx::pluginpath::PluginBrowseFilters();
        break;
    case guitarfx::BrowseFileType::Any:
        filters = "*.*";
        break;
    default:
        filters = "*.*";
        break;
    }

    mFileChooser = std::make_unique<juce::FileChooser>(juce::String(title), juce::File(), filters);

    const bool folderMode = type == guitarfx::BrowseFileType::Folder;

    int flags = juce::FileBrowserComponent::openMode;

    if (folderMode)
    {
        flags |= juce::FileBrowserComponent::canSelectDirectories;
    }
    else
    {
        flags |= juce::FileBrowserComponent::canSelectFiles;
    }

#if !JUCE_WINDOWS

    // Plugin bundles (.vst3 on Linux, .component on macOS, .lv2 everywhere) are
    // directories; allow selecting them where the platform permits it. On Windows the
    // native dialog switches to a folders-only picker when directories are selectable,
    // so keep files-only there. Either way this is only a convenience — picking a
    // payload file inside the bundle always works too.
    if (type == guitarfx::BrowseFileType::PluginFile)
    {
        flags |= juce::FileBrowserComponent::canSelectDirectories;
    }

#endif

    mFileChooser->launchAsync(flags, [this, type, callback](const juce::FileChooser& chooser) {
        guitarfx::BrowseFileResult result;
        auto file = chooser.getResult();
        mFileChooser.reset();

        // The dialog reports what was picked and nothing more. Normalizing a selection
        // to its bundle root, and judging whether it can load, belong to the layers
        // that own those questions: the resource library and the plugin loader.
        const bool acceptDirectories =
            type == guitarfx::BrowseFileType::PluginFile || type == guitarfx::BrowseFileType::Folder;

        if (file.existsAsFile() || (acceptDirectories && file.isDirectory()))
        {
            result.success = true;
            result.path = soundshed::toStdPath(file);
        }

        if (callback)
        {
            callback(result);
        }
    });
}

void PluginProcessorAdapter::SaveFileAsync(guitarfx::BrowseFileType type, const std::string& title,
                                           const std::string& defaultName,
                                           std::function<void(const guitarfx::BrowseFileResult&)> callback)
{
#ifdef _WIN32
    std::thread([type, title, defaultName, callback = std::move(callback),
                 weakThis = juce::WeakReference<PluginProcessorAdapter>(this)]() mutable {
        ScopedComInitializer com;
        guitarfx::BrowseFileResult result;

        // The dialog runs on this thread; its answer goes back to the message thread, which
        // is where the controller expects it (a render touches the live mixer, an export the
        // store), and is dropped if the host has removed the instance meanwhile.
        const auto deliver = [&weakThis, &callback](const guitarfx::BrowseFileResult& answer) {
            juce::MessageManager::callAsync([weakThis, callback, answer]() {
                if (weakThis.get() != nullptr && callback)
                {
                    callback(answer);
                }
            });
        };

        Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;
        HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dialog));

        if (FAILED(hr))
        {
            deliver(result);
            return;
        }

        auto normalizedDefaultName = defaultName;
        std::transform(normalizedDefaultName.begin(), normalizedDefaultName.end(), normalizedDefaultName.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        const auto hasSuffix = [&normalizedDefaultName](std::string_view suffix) {
            return normalizedDefaultName.size() >= suffix.size() &&
                   normalizedDefaultName.compare(normalizedDefaultName.size() - suffix.size(), suffix.size(), suffix) ==
                       0;
        };

        std::vector<COMDLG_FILTERSPEC> filters;
        std::wstring defaultExtension;

        switch (type)
        {
        case guitarfx::BrowseFileType::PresetFile:
            filters = {{L"JSON Files", L"*.json"}};
            defaultExtension = L"json";
            break;
        case guitarfx::BrowseFileType::ArchiveFile:

            if (hasSuffix(".soundshed.preset"))
            {
                filters = {{L"Preset Archive", L"*.soundshed.preset"}};
                defaultExtension = L"soundshed.preset";
            }
            else if (hasSuffix(".soundshed.presets"))
            {
                filters = {{L"Preset Archives", L"*.soundshed.presets"}};
                defaultExtension = L"soundshed.presets";
            }
            else if (hasSuffix(".zip"))
            {
                filters = {{L"ZIP Archives", L"*.zip"}};
                defaultExtension = L"zip";
            }
            else
            {
                filters = {{L"Preset Archive", L"*.soundshed.preset"}};
                defaultExtension = L"soundshed.preset";
            }

            break;
        case guitarfx::BrowseFileType::AudioFile:
            filters = {{L"WAV Files", L"*.wav"}};
            defaultExtension = L"wav";
            break;
        default:
            filters = {{L"All Files", L"*.*"}};
            break;
        }

        dialog->SetFileTypes(static_cast<UINT>(filters.size()), filters.data());

        if (!defaultExtension.empty())
        {
            dialog->SetDefaultExtension(defaultExtension.c_str());
        }

        std::wstring wtitle(title.begin(), title.end());
        dialog->SetTitle(wtitle.c_str());

        std::wstring wname(defaultName.begin(), defaultName.end());
        dialog->SetFileName(wname.c_str());

        hr = dialog->Show(nullptr);

        if (SUCCEEDED(hr))
        {
            Microsoft::WRL::ComPtr<IShellItem> item;
            hr = dialog->GetResult(&item);

            if (SUCCEEDED(hr))
            {
                PWSTR filePath = nullptr;
                hr = item->GetDisplayName(SIGDN_FILESYSPATH, &filePath);

                if (SUCCEEDED(hr) && filePath)
                {
                    result.path = std::filesystem::path(filePath);
                    result.success = true;
                    CoTaskMemFree(filePath);
                }
            }
        }

        deliver(result);
    }).detach();
    return;
#endif

    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        juce::MessageManager::callAsync([weakThis = juce::WeakReference<PluginProcessorAdapter>(this), type, title,
                                         defaultName, callback = std::move(callback)]() mutable {
            if (auto* self = weakThis.get())
            {
                self->SaveFileAsync(type, title, defaultName, std::move(callback));
            }
        });
        return;
    }

    // One dialog at a time, as BrowseFileAsync.
    if (mFileChooser != nullptr)
    {
        if (callback)
        {
            callback(guitarfx::BrowseFileResult{});
        }

        return;
    }

    auto normalizedDefaultName = defaultName;
    std::transform(normalizedDefaultName.begin(), normalizedDefaultName.end(), normalizedDefaultName.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    const auto hasSuffix = [&normalizedDefaultName](std::string_view suffix) {
        return normalizedDefaultName.size() >= suffix.size() &&
               normalizedDefaultName.compare(normalizedDefaultName.size() - suffix.size(), suffix.size(), suffix) == 0;
    };

    juce::String filters;

    switch (type)
    {
    case guitarfx::BrowseFileType::PresetFile:
        filters = "*.json";
        break;
    case guitarfx::BrowseFileType::ArchiveFile:

        if (hasSuffix(".soundshed.preset"))
        {
            filters = "*.soundshed.preset";
        }
        else if (hasSuffix(".soundshed.presets"))
        {
            filters = "*.soundshed.presets";
        }
        else if (hasSuffix(".zip"))
        {
            filters = "*.zip";
        }
        else
        {
            filters = "*.soundshed.preset";
        }

        break;
    case guitarfx::BrowseFileType::NAMModel:
    case guitarfx::BrowseFileType::IRFile:
    case guitarfx::BrowseFileType::ImageFile:
    case guitarfx::BrowseFileType::AudioFile:
    case guitarfx::BrowseFileType::Any:
    default:
        filters = "*.*";
        break;
    }

    mFileChooser =
        std::make_unique<juce::FileChooser>(juce::String(title), juce::File(juce::String(defaultName)), filters);

    const auto flags = juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles;

    mFileChooser->launchAsync(flags, [this, callback](const juce::FileChooser& chooser) {
        guitarfx::BrowseFileResult result;
        const auto file = chooser.getResult();
        mFileChooser.reset();

        if (file != juce::File())
        {
            result.success = true;
            result.path = soundshed::toStdPath(file);
        }

        if (callback)
        {
            callback(result);
        }
    });
}

void PluginProcessorAdapter::RunOnMainThread(std::function<void()> fn)
{
    juce::MessageManager::callAsync(std::move(fn));
}

bool PluginProcessorAdapter::IsMessageThread() const
{
    // Without a message manager there is no loop to hand work to, so the caller keeps it.
    // The controller relies on this for getStateInformation(), which JUCE's AAX, AU and LV2
    // wrappers pass straight through from whatever thread the host asked on.
    const auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();
    return messageManager == nullptr || messageManager->isThisTheMessageThread();
}

std::filesystem::path PluginProcessorAdapter::GetUserDataPath() const
{
    // The shared profile, whichever product this is (ProfileFolder.h).
    const auto dataDir = soundshed::profileFolder();

#if JUCE_MAC
    // Runs at most once per process; every consumer of the user data path funnels
    // through here, so the migration cannot be missed by call ordering.
    migrateDataOutOfSandboxContainerOnce(dataDir);
#endif

    return soundshed::toStdPath(dataDir);
}

std::filesystem::path PluginProcessorAdapter::GetBundledAssetsPath() const
{
    return mAssetRoot;
}

double PluginProcessorAdapter::GetSampleRate() const
{
    return juce::AudioProcessor::getSampleRate();
}

int PluginProcessorAdapter::GetBlockSize() const
{
    return juce::AudioProcessor::getBlockSize();
}

bool PluginProcessorAdapter::SupportsAudioDeviceSettings() const
{
    return mAudioDeviceRequestHandler != nullptr;
}

void PluginProcessorAdapter::HandleAudioDeviceRequest(const std::string& requestJson)
{
    if (mAudioDeviceRequestHandler)
    {
        mAudioDeviceRequestHandler(requestJson);
        return;
    }

    // A plugin format: the host owns the devices. Say so, so the UI stops asking.
    SendMessageToUI(R"({"type":"audioDeviceState","available":false})");
}

void PluginProcessorAdapter::setAudioDeviceRequestHandler(AudioDeviceRequestHandler handler)
{
    mAudioDeviceRequestHandler = std::move(handler);
}

void PluginProcessorAdapter::setDeviceLatencyQuery(DeviceLatencyQuery query)
{
    mDeviceLatencyQuery = std::move(query);
}

int PluginProcessorAdapter::GetDeviceLatencySamples() const
{
    return mDeviceLatencyQuery ? mDeviceLatencyQuery() : 0;
}

void PluginProcessorAdapter::setWindowChromeHandler(WindowChromeHandler handler)
{
    mWindowChromeHandler = std::move(handler);
}

void PluginProcessorAdapter::NotifyStateChanged()
{
    updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
}

void PluginProcessorAdapter::NotifyLatencyChanged(int newLatencySamples)
{
    setLatencySamples(newLatencySamples);
    updateHostDisplay(juce::AudioProcessor::ChangeDetails().withLatencyChanged(true));
}

double PluginProcessorAdapter::GetHostTempo() const
{
    // What processBlock read from the play head; it is not asked here, off the audio thread.
    const double bpm = mHostTempoBpm.load(std::memory_order_relaxed);
    return bpm > 0.0 ? bpm : 120.0;
}

bool PluginProcessorAdapter::IsHostPlaying() const
{
    return mHostPlaying.load(std::memory_order_relaxed);
}

bool PluginProcessorAdapter::IsStandalone() const
{
    return wrapperType == wrapperType_Standalone;
}

guitarfx::IPluginHost::AudioChannelCounts PluginProcessorAdapter::GetAudioChannelCounts() const
{
    // The bus layout the host chose. The standalone app's bus is always stereo, but a device
    // with one active input (or output) only fills (or plays) one of its channels.
    AudioChannelCounts counts{getMainBusNumInputChannels(), getMainBusNumOutputChannels()};

    if (wrapperType == wrapperType_Standalone)
    {
        if (auto* holder = juce::StandalonePluginHolder::getInstance())
        {
            if (auto* device = holder->deviceManager.getCurrentAudioDevice())
            {
                counts.inputs = std::min(counts.inputs, device->getActiveInputChannels().countNumberOfSetBits());
                counts.outputs = std::min(counts.outputs, device->getActiveOutputChannels().countNumberOfSetBits());
            }
        }
    }

    return counts;
}

void PluginProcessorAdapter::ensureStandaloneProtocolHandlerRegistration()
{
    if (mStandaloneProtocolRegistrationAttempted)
    {
        return;
    }

    mStandaloneProtocolRegistrationAttempted = true;

    if (wrapperType != wrapperType_Standalone)
    {
        return;
    }

#if JUCE_WINDOWS
    static constexpr const wchar_t* protocolRoot = L"Software\\Classes\\soundshed";

    const auto setRegistryString = [](const wchar_t* subKey, const wchar_t* valueName, const juce::String& value) {
        HKEY key = nullptr;
        const auto createResult = RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                                                  KEY_SET_VALUE, nullptr, &key, nullptr);

        if (createResult != ERROR_SUCCESS || key == nullptr)
        {
            return false;
        }

        const auto scopedClose = [&key]() {
            if (key != nullptr)
            {
                RegCloseKey(key);
            }
        };

        const auto* raw = value.toWideCharPointer();
        const auto bytes = static_cast<DWORD>((value.length() + 1) * static_cast<int>(sizeof(wchar_t)));
        const auto setResult = RegSetValueExW(key, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(raw), bytes);
        scopedClose();
        return setResult == ERROR_SUCCESS;
    };

    const auto executablePath = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName();

    if (executablePath.isEmpty())
    {
        return;
    }

    const juce::String command = "\"" + executablePath + "\" \"%1\"";

    const auto keyPaths = std::vector<std::tuple<const wchar_t*, const wchar_t*, juce::String>>{
        {protocolRoot, nullptr, "URL:Soundshed Protocol"},
        {protocolRoot, L"URL Protocol", ""},
        {L"Software\\Classes\\soundshed\\DefaultIcon", nullptr, executablePath + ",0"},
        {L"Software\\Classes\\soundshed\\shell\\open\\command", nullptr, command},
    };

    bool allSucceeded = true;

    for (const auto& [subKey, valueName, value] : keyPaths)
    {
        if (!setRegistryString(subKey, valueName, value))
        {
            allSucceeded = false;
        }
    }

    if (!allSucceeded)
    {
        juce::Logger::writeToLog(
            "[PluginProcessorAdapter] Failed to fully register soundshed:// URL protocol handler.");
    }

#endif
}

void PluginProcessorAdapter::setWebMessageCallback(std::function<void(const juce::String&)> callback)
{
    std::scoped_lock lock(mWebMessageMutex);
    mWebMessageCallback = std::move(callback);
}

void PluginProcessorAdapter::handleWebMessage(const juce::String& message)
{
    // Handle openUrl locally — open in the system default browser.
    const auto parsed = juce::JSON::parse(message);

    if (auto* obj = parsed.getDynamicObject(); obj != nullptr)
    {
        const auto typeId = juce::Identifier{"type"};
        const auto urlId = juce::Identifier{"url"};

        if (obj->getProperty(typeId).toString() == "openUrl")
        {
            const auto url = obj->getProperty(urlId).toString();

            if (url.startsWith("https://") || url.startsWith("http://"))
            {
                juce::URL(url).launchInDefaultBrowser();
            }

            return;
        }

        // The window's business, not the engine's.
        if (obj->getProperty(typeId).toString() == "setWindowChrome")
        {
            if (mWindowChromeHandler)
            {
                mWindowChromeHandler(parsed);
            }

            return;
        }
    }

    mController.HandleUIMessage(message.toStdString());
}

void PluginProcessorAdapter::sendMessageToUI(const juce::String& message)
{
    std::function<void(const juce::String&)> callback;
    {
        std::scoped_lock lock(mWebMessageMutex);
        callback = mWebMessageCallback;
    }

    if (callback)
    {
        callback(message);
    }
}

std::filesystem::path PluginProcessorAdapter::locateAssetsRoot() const
{
    std::vector<std::filesystem::path> candidates;

#if JUCE_ANDROID
    // On Android there is no directory next to the binary: the UI ships as APK
    // assets, and SoundshedApp unpacks them to "<dataDir>/resources" before
    // JUCE starts.  userApplicationDataDirectory is that same dataDir.
    const auto androidDataDir =
        soundshed::toStdPath(juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory));

    if (!androidDataDir.empty())
    {
        candidates.push_back(androidDataDir / "resources");
    }

#endif

#if JUCE_IOS
    // An iOS bundle is flat: the executable sits at its root, beside ui/. Not under
    // "resources/" as on desktop: a directory of that name at the root of an iOS bundle makes
    // codesign take it for a macOS bundle and refuse to sign it. The editor takes this root
    // before its own guesses.
    const auto bundleDir =
        soundshed::toStdPath(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory());

    if (!bundleDir.empty())
    {
        candidates.push_back(bundleDir);
    }

#endif

    const auto cwd = soundshed::toStdPath(juce::File::getCurrentWorkingDirectory());

    if (!cwd.empty())
    {
        candidates.push_back(cwd / "resources");
        candidates.push_back(cwd / "Resources");
    }

    const auto exeDir =
        soundshed::toStdPath(juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory());

    if (!exeDir.empty())
    {
        candidates.push_back(exeDir / "resources");
        candidates.push_back(exeDir / "Resources");
    }

    return guitarfx::ui::ResolveResourceRoot(candidates);
}

// ════════════════════════════════════════════════════════════════════════
// JUCE plugin instance creator
// ════════════════════════════════════════════════════════════════════════

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PluginProcessorAdapter();
}
