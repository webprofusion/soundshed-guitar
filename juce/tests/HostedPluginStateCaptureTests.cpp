// Capture-side guards in the hosted plugin effect, exercised against a stub plugin so they
// run everywhere rather than only on a machine with a particular VST3 installed.
//
// Covers the two ways auto-capture used to misbehave:
//   * an empty capture being treated as authoritative, which propagated an empty value that
//     the controller turns into an erase of the node's stored state;
//   * a capture per parameter tick during a knob drag, each one a full getStateInformation()
//     on the message thread.
// And one on the restore side: every load restoring the same state twice, before and after
// the prepare that follows it. Then the hand-over a chain rebuild makes: the rebuilt node takes
// its predecessor's instance rather than making its own (EffectProcessor::ExpectHandOver).

#include "JuceHostedPluginEffect.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
constexpr auto kFailCode = 1;
constexpr const char* kStateConfigKey = "pluginStateBase64";

int Fail(const std::string& message)
{
    std::cerr << "[HostedPluginStateCaptureTests] " << message << std::endl;
    return kFailCode;
}

/**
 * Minimal AudioPluginInstance whose serialised state the test controls directly.
 *
 * getStateInformation() hands back whatever the test set, including nothing at all — which
 * is the case the empty-capture guard exists for. It counts calls so the gesture test can
 * assert how many captures a drag actually cost.
 */
class StubPluginInstance final : public juce::AudioPluginInstance
{
  public:
    /**
     * @param withParameter  Whether to expose an automatable parameter. Without one, and
     *                       with an empty chunk, the state envelope encodes to nothing at
     *                       all — which is the case the empty-capture guard exists for.
     *                       With one, parameter and gesture listeners have something to
     *                       attach to.
     */
    explicit StubPluginInstance(juce::String stateText, bool withParameter = true) : mStateText(std::move(stateText))
    {
        if (!withParameter)
        {
            return;
        }

        auto parameter = std::make_unique<juce::AudioParameterFloat>(juce::ParameterID{"p", 1}, "P", 0.0f, 1.0f, 0.5f);
        mParameter = parameter.get();
        addHostedParameter(std::move(parameter));
    }

    void SetStateText(juce::String stateText)
    {
        mStateText = std::move(stateText);
    }

    /// Stands in for a plugin reporting that something other than a parameter changed —
    /// an internal preset load, a sample swap. The only capture trigger available to a
    /// plugin with no automatable parameters.
    void NotifyNonParameterStateChanged()
    {
        updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
    }

    [[nodiscard]] int StateReadCount() const
    {
        return mStateReadCount;
    }

    void ResetStateReadCount()
    {
        mStateReadCount = 0;
    }

    [[nodiscard]] int StateWriteCount() const
    {
        return mStateWriteCount;
    }

    [[nodiscard]] const juce::String& StateText() const
    {
        return mStateText;
    }

    [[nodiscard]] juce::AudioParameterFloat* Parameter() const
    {
        return mParameter;
    }

    // ── AudioPluginInstance ────────────────────────────────────────
    void fillInPluginDescription(juce::PluginDescription& description) const override
    {
        description.name = "Stub";
        description.pluginFormatName = "Stub";
        description.manufacturerName = "Soundshed Tests";
        description.uniqueId = 1;
    }

    // ── AudioProcessor ─────────────────────────────────────────────
    const juce::String getName() const override
    {
        return "Stub";
    }

    void prepareToPlay(double, int) override
    {
    }

    void releaseResources() override
    {
    }

    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override
    {
    }

    juce::AudioProcessorEditor* createEditor() override
    {
        return nullptr;
    }

    bool hasEditor() const override
    {
        return false;
    }

    bool acceptsMidi() const override
    {
        return false;
    }

    bool producesMidi() const override
    {
        return false;
    }

    double getTailLengthSeconds() const override
    {
        return 0.0;
    }

    int getNumPrograms() override
    {
        return 1;
    }

    int getCurrentProgram() override
    {
        return 0;
    }

    void setCurrentProgram(int) override
    {
    }

    const juce::String getProgramName(int) override
    {
        return {};
    }

    void changeProgramName(int, const juce::String&) override
    {
    }

    void getStateInformation(juce::MemoryBlock& destData) override
    {
        ++mStateReadCount;
        destData.reset();

        if (mStateText.isNotEmpty())
        {
            destData.append(mStateText.toRawUTF8(), static_cast<size_t>(mStateText.getNumBytesAsUTF8()));
        }
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        ++mStateWriteCount;
        mStateText = juce::String::fromUTF8(static_cast<const char*>(data), sizeInBytes);
    }

  private:
    juce::String mStateText;
    int mStateReadCount = 0;
    int mStateWriteCount = 0;
    juce::AudioParameterFloat* mParameter = nullptr;
};

struct CapturedRuntimeConfig
{
    std::vector<std::pair<std::string, std::string>> changes;

    [[nodiscard]] int StateChangeCount() const
    {
        int count = 0;

        for (const auto& change : changes)
        {
            if (change.first == kStateConfigKey)
            {
                ++count;
            }
        }

        return count;
    }

    [[nodiscard]] std::string LastStateValue() const
    {
        for (auto it = changes.rbegin(); it != changes.rend(); ++it)
        {
            if (it->first == kStateConfigKey)
            {
                return it->second;
            }
        }

        return "<none>";
    }
};

/// An empty capture must never overwrite a stored chunk. A plugin that momentarily reports
/// nothing — a failed capture, an empty chunk with no automatable parameters — would
/// otherwise erase perfectly valid state on its way through the controller.
bool TestEmptyCaptureDoesNotOverwriteStoredState()
{
    guitarfx::JuceHostedPluginEffect effect;

    CapturedRuntimeConfig captured;
    effect.SetRuntimeConfigChangedCallback(
        [&captured](const std::string& key, const std::string& value) { captured.changes.emplace_back(key, value); });

    // No automatable parameters: with an empty chunk this plugin encodes to nothing at all.
    auto plugin = std::make_unique<StubPluginInstance>("plugin-state-worth-keeping", /*withParameter*/ false);
    auto* raw = plugin.get();
    effect.InstallHostedPluginForTesting(std::move(plugin));
    effect.Prepare(48000.0, 512);

    const auto storedState = effect.GetConfig(kStateConfigKey);

    if (storedState.empty())
    {
        Fail("stub plugin produced no state to begin with");
        return false;
    }

    effect.SetConfig(kStateConfigKey, storedState);

    // The plugin now reports nothing at all, and something triggers a capture.
    raw->SetStateText({});
    captured.changes.clear();
    raw->NotifyNonParameterStateChanged();

    if (!effect.GetConfig(kStateConfigKey).empty() && raw->StateReadCount() == 0)
    {
        Fail("the capture path never ran, so the guard was not exercised");
        return false;
    }

    if (captured.StateChangeCount() != 0)
    {
        Fail("an empty capture was published as a state change: '" + captured.LastStateValue() + "'");
        return false;
    }

    if (effect.GetPendingPluginStateForTesting() != storedState)
    {
        Fail("an empty capture overwrote the stored plugin state");
        return false;
    }

    return true;
}

/// A non-empty change still has to reach the host — the guard above must not suppress real
/// captures.
bool TestRealCaptureIsStillPublished()
{
    guitarfx::JuceHostedPluginEffect effect;

    CapturedRuntimeConfig captured;
    effect.SetRuntimeConfigChangedCallback(
        [&captured](const std::string& key, const std::string& value) { captured.changes.emplace_back(key, value); });

    auto plugin = std::make_unique<StubPluginInstance>("state-one");
    auto* raw = plugin.get();
    effect.InstallHostedPluginForTesting(std::move(plugin));
    effect.Prepare(48000.0, 512);
    effect.SetConfig(kStateConfigKey, effect.GetConfig(kStateConfigKey));

    captured.changes.clear();
    raw->SetStateText("state-two");
    raw->Parameter()->setValueNotifyingHost(0.75f);

    if (captured.StateChangeCount() == 0)
    {
        Fail("a genuine state change was not published");
        return false;
    }

    const auto published = captured.LastStateValue();

    if (published.empty() || published == "<none>")
    {
        Fail("published state was empty for a genuine change");
        return false;
    }

    return true;
}

/// A knob drag is one gesture with many value notifications. Capturing on each one costs a
/// full getStateInformation() per tick; the gesture end is the single point that matters.
bool TestGestureCoalescesCaptures()
{
    guitarfx::JuceHostedPluginEffect effect;

    CapturedRuntimeConfig captured;
    effect.SetRuntimeConfigChangedCallback(
        [&captured](const std::string& key, const std::string& value) { captured.changes.emplace_back(key, value); });

    auto plugin = std::make_unique<StubPluginInstance>("state-initial");
    auto* raw = plugin.get();
    effect.InstallHostedPluginForTesting(std::move(plugin));
    effect.Prepare(48000.0, 512);
    effect.SetConfig(kStateConfigKey, effect.GetConfig(kStateConfigKey));

    auto* parameter = raw->Parameter();
    raw->ResetStateReadCount();

    parameter->beginChangeGesture();

    for (int step = 1; step <= 20; ++step)
    {
        raw->SetStateText("state-drag-" + juce::String(step));
        parameter->setValueNotifyingHost(static_cast<float>(step) / 20.0f);
    }

    const int readsDuringDrag = raw->StateReadCount();

    parameter->endChangeGesture();
    const int readsAfterGestureEnd = raw->StateReadCount();

    if (readsDuringDrag != 0)
    {
        Fail("the drag captured state " + std::to_string(readsDuringDrag) +
             " time(s) before the gesture ended; it should capture once, at the end");
        return false;
    }

    if (readsAfterGestureEnd == 0)
    {
        Fail("the gesture ended without capturing the final state");
        return false;
    }

    // The value that lands must be the end of the drag, not some mid-drag sample.
    if (effect.GetPendingPluginStateForTesting().empty())
    {
        Fail("no state was stored after the gesture ended");
        return false;
    }

    // A change outside any gesture still captures. Straight after the gesture end's capture it
    // waits out the throttle interval rather than capturing again at once.
    raw->ResetStateReadCount();
    raw->SetStateText("state-after-drag");
    parameter->setValueNotifyingHost(0.1f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(800);

    if (raw->StateReadCount() == 0)
    {
        Fail("a change outside a gesture did not capture");
        return false;
    }

    return true;
}

/// A parameter something modulates outside any gesture (an LFO, host automation of the plugin)
/// notifies every block while audio plays; each capture is a full getStateInformation(). They
/// coalesce to one per interval.
bool TestUngesturedChangesAreThrottled()
{
    guitarfx::JuceHostedPluginEffect effect;

    auto plugin = std::make_unique<StubPluginInstance>("state-initial");
    auto* raw = plugin.get();
    effect.InstallHostedPluginForTesting(std::move(plugin));
    effect.Prepare(48000.0, 512);
    effect.SetConfig(kStateConfigKey, effect.GetConfig(kStateConfigKey));

    auto* parameter = raw->Parameter();
    raw->ResetStateReadCount();
    auto* messageManager = juce::MessageManager::getInstance();

    // About 300 ms of a modulated parameter, one notification every 3 ms.
    for (int step = 0; step < 100; ++step)
    {
        raw->SetStateText("state-lfo-" + juce::String(step));
        parameter->setValueNotifyingHost(static_cast<float>(step % 10) / 10.0f);
        messageManager->runDispatchLoopUntil(3);
    }

    messageManager->runDispatchLoopUntil(800);
    const int reads = raw->StateReadCount();

    if (reads == 0 || reads > 3)
    {
        Fail("100 ungestured changes over ~300 ms captured " + std::to_string(reads) +
             " time(s); expected one or two, plus at most one trailing");
        return false;
    }

    // The last value is the one that lands: a capture forced now (a gesture end is never
    // throttled) finds nothing the trailing one had not already stored.
    const auto afterTrailing = effect.GetPendingPluginStateForTesting();
    parameter->beginChangeGesture();
    parameter->endChangeGesture();

    if (afterTrailing.empty() || effect.GetPendingPluginStateForTesting() != afterTrailing)
    {
        Fail("the trailing capture did not store the final state");
        return false;
    }

    return true;
}

/// A plugin that ends a gesture it never began must not wedge capture off permanently.
bool TestUnbalancedGestureEndDoesNotWedgeCapture()
{
    guitarfx::JuceHostedPluginEffect effect;

    auto plugin = std::make_unique<StubPluginInstance>("state-initial");
    auto* raw = plugin.get();
    effect.InstallHostedPluginForTesting(std::move(plugin));
    effect.Prepare(48000.0, 512);
    effect.SetConfig(kStateConfigKey, effect.GetConfig(kStateConfigKey));

    auto* parameter = raw->Parameter();

    // Two ends, one begin: a badly behaved plugin, or one whose editor was torn down
    // mid-drag and re-created.
    parameter->beginChangeGesture();
    parameter->endChangeGesture();
    parameter->endChangeGesture();

    raw->ResetStateReadCount();
    raw->SetStateText("state-after-unbalanced-gestures");
    parameter->setValueNotifyingHost(0.33f);
    // The gesture end just captured, so this one waits out the throttle interval.
    juce::MessageManager::getInstance()->runDispatchLoopUntil(800);

    if (raw->StateReadCount() == 0)
    {
        Fail("capture stayed suppressed after an unbalanced gesture end");
        return false;
    }

    return true;
}

/// The node's stored state for a plugin holding `stateText`, as a preset would carry it.
std::string EncodedStateOf(const juce::String& stateText)
{
    guitarfx::JuceHostedPluginEffect source;
    source.InstallHostedPluginForTesting(std::make_unique<StubPluginInstance>(stateText));
    return source.GetConfig(kStateConfigKey);
}

/// A prepare restores only state the plugin has not been given. A load restores the state and
/// the node is prepared straight after, which used to restore it a second time on every load:
/// a second setStateInformation, ~200 ms for some plugins.
bool TestPrepareDoesNotRestoreStateTwice()
{
    const auto savedState = EncodedStateOf("state-saved");
    const auto laterState = EncodedStateOf("state-later");

    guitarfx::JuceHostedPluginEffect effect;
    auto plugin = std::make_unique<StubPluginInstance>("state-default");
    auto* raw = plugin.get();
    effect.InstallHostedPluginForTesting(std::move(plugin));

    // State that arrives before the node is prepared is restored by the first prepare.
    effect.SetConfig(kStateConfigKey, savedState);
    effect.Prepare(48000.0, 512);

    if (raw->StateWriteCount() != 1 || raw->StateText() != "state-saved")
    {
        Fail("the first prepare did not restore the state once: " + std::to_string(raw->StateWriteCount()) +
             " write(s), plugin holds '" + raw->StateText().toStdString() + "'");
        return false;
    }

    // Preparing again, as a device change does, leaves the state the plugin already has.
    effect.Prepare(48000.0, 256);

    if (raw->StateWriteCount() != 1)
    {
        Fail("a prepare restored state the plugin already had: " + std::to_string(raw->StateWriteCount()) + " writes");
        return false;
    }

    // Restoring state explicitly still lands, even the same state.
    effect.SetConfig(kStateConfigKey, savedState);

    if (raw->StateWriteCount() != 2)
    {
        Fail("an explicit restore of the same state was skipped");
        return false;
    }

    effect.SetConfig(kStateConfigKey, laterState);

    if (raw->StateWriteCount() != 3 || raw->StateText() != "state-later")
    {
        Fail("a new state was not restored");
        return false;
    }

    // A new instance, as a plugin reload makes, has been given nothing yet.
    auto replacement = std::make_unique<StubPluginInstance>("state-default");
    auto* rawReplacement = replacement.get();
    effect.InstallHostedPluginForTesting(std::move(replacement));
    effect.Prepare(48000.0, 512);

    if (rawReplacement->StateWriteCount() != 1 || rawReplacement->StateText() != "state-later")
    {
        Fail("a new instance was not given the node's state on prepare");
        return false;
    }

    return true;
}

/// Where the stub is taken to have been loaded from. It need not exist: a hand-over never
/// opens the file, and a load that does open it fails cleanly.
std::filesystem::path StubPluginPath()
{
    return std::filesystem::temp_directory_path() / "soundshed-hand-over-stub.vst3";
}

/// A node rebuilt with its chain takes over the instance its predecessor holds: nothing is
/// created, the state it was configured with (captured from that instance just before the
/// rebuild) is not restored again, the predecessor passes its input through from then on,
/// and the instance's notifications reach the new node, not the old one.
bool TestRebuiltNodeTakesOverItsPredecessorsInstance()
{
    auto previous = std::make_unique<guitarfx::JuceHostedPluginEffect>();
    auto plugin = std::make_unique<StubPluginInstance>("state-live");
    auto* raw = plugin.get();
    previous->InstallHostedPluginForTesting(std::move(plugin), StubPluginPath());
    previous->Prepare(48000.0, 512);
    const int writesBefore = raw->StateWriteCount();

    // What the controller folds into the preset before every rebuild.
    const auto captured = previous->GetConfig(kStateConfigKey);

    guitarfx::JuceHostedPluginEffect rebuilt;
    CapturedRuntimeConfig published;
    rebuilt.SetRuntimeConfigChangedCallback(
        [&published](const std::string& key, const std::string& value) { published.changes.emplace_back(key, value); });

    if (!rebuilt.ExpectHandOver(*previous))
    {
        Fail("a node rebuilt over a loaded predecessor did not expect its instance");
        return false;
    }

    rebuilt.SetConfig(kStateConfigKey, captured);

    if (!rebuilt.LoadResource(StubPluginPath()) || !rebuilt.IsAwaitingHandOverForTesting() || !rebuilt.HasResource() ||
        rebuilt.GetHostedPluginForTesting() != nullptr)
    {
        Fail("loading the predecessor's plugin did not wait for the hand-over");
        return false;
    }

    rebuilt.Prepare(48000.0, 512);
    rebuilt.CompleteHandOver(previous.get());

    if (rebuilt.GetHostedPluginForTesting() != raw || previous->GetHostedPluginForTesting() != nullptr)
    {
        Fail("the instance did not move to the rebuilt node");
        return false;
    }

    if (raw->StateWriteCount() != writesBefore || raw->StateText() != "state-live")
    {
        Fail("the hand-over restored state the instance already had: " +
             std::to_string(raw->StateWriteCount() - writesBefore) + " write(s)");
        return false;
    }

    // The predecessor, still fading out in the old chain, passes its input through.
    std::vector<float> inL(64, 0.25f), inR(64, 0.25f), outL(64, 0.0f), outR(64, 0.0f);
    float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};
    previous->Process(inputs, outputs, 64);

    if (outL[10] != 0.25f || outR[10] != 0.25f)
    {
        Fail("the predecessor did not pass its input through after the hand-over");
        return false;
    }

    // Retired chains are destroyed later. Its listeners must already be off the instance, or
    // the next notification would reach a node that is gone.
    previous.reset();
    raw->SetStateText("state-changed");
    raw->Parameter()->setValueNotifyingHost(0.9f);

    if (published.LastStateValue() == "<none>")
    {
        Fail("a change on the instance did not reach the node that took it over");
        return false;
    }

    return true;
}

/// A rebuilt node configured with a state the instance does not hold (an effect preset, a
/// scene of its own) restores it onto the instance it takes over.
bool TestHandOverRestoresStateTheInstanceDoesNotHold()
{
    const auto otherState = EncodedStateOf("state-other");

    guitarfx::JuceHostedPluginEffect previous;
    auto plugin = std::make_unique<StubPluginInstance>("state-live");
    auto* raw = plugin.get();
    previous.InstallHostedPluginForTesting(std::move(plugin), StubPluginPath());
    previous.Prepare(48000.0, 512);
    (void)previous.GetConfig(kStateConfigKey);

    guitarfx::JuceHostedPluginEffect rebuilt;
    rebuilt.ExpectHandOver(previous);
    rebuilt.SetConfig(kStateConfigKey, otherState);
    rebuilt.LoadResource(StubPluginPath());
    rebuilt.Prepare(48000.0, 512);
    rebuilt.CompleteHandOver(&previous);

    if (rebuilt.GetHostedPluginForTesting() != raw || raw->StateText() != "state-other")
    {
        Fail("a state the instance did not hold was not restored on hand-over: plugin holds '" +
             raw->StateText().toStdString() + "'");
        return false;
    }

    return true;
}

/// A node that now names another plugin loads that one, and the predecessor keeps its own.
bool TestNodeNamingAnotherPluginDoesNotTakeTheInstance()
{
    guitarfx::JuceHostedPluginEffect previous;
    auto plugin = std::make_unique<StubPluginInstance>("state-live");
    auto* raw = plugin.get();
    previous.InstallHostedPluginForTesting(std::move(plugin), StubPluginPath());
    previous.Prepare(48000.0, 512);

    guitarfx::JuceHostedPluginEffect rebuilt;
    rebuilt.ExpectHandOver(previous);
    // Does not exist, so this load fails, but it is a load of its own.
    rebuilt.LoadResource(std::filesystem::temp_directory_path() / "soundshed-hand-over-other.vst3");

    if (rebuilt.IsAwaitingHandOverForTesting())
    {
        Fail("a node naming another plugin waited for its predecessor's instance");
        return false;
    }

    rebuilt.CompleteHandOver(&previous);

    if (previous.GetHostedPluginForTesting() != raw || rebuilt.GetHostedPluginForTesting() != nullptr)
    {
        Fail("a node naming another plugin took its predecessor's instance");
        return false;
    }

    return true;
}

/// A predecessor gone by the time the chain is installed: the node loads the plugin itself,
/// as a node with no predecessor would, rather than being left without one.
bool TestHandOverWithoutPredecessorLoadsItself()
{
    guitarfx::JuceHostedPluginEffect rebuilt;
    {
        guitarfx::JuceHostedPluginEffect previous;
        previous.InstallHostedPluginForTesting(std::make_unique<StubPluginInstance>("state-live"), StubPluginPath());
        rebuilt.ExpectHandOver(previous);
        rebuilt.LoadResource(StubPluginPath());
    }

    rebuilt.CompleteHandOver(nullptr);

    // The stub's file does not exist, so the load it falls back to fails, and says so.
    if (rebuilt.IsAwaitingHandOverForTesting() || rebuilt.HasResource() ||
        rebuilt.GetConfig("lastErrorCode") != "file-not-found")
    {
        Fail("a hand-over with no predecessor did not fall back to a load of its own");
        return false;
    }

    return true;
}
} // namespace

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    int passed = 0;
    int failed = 0;

    const auto run = [&](const std::string& name, bool ok) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";

        if (ok)
        {
            ++passed;
        }
        else
        {
            ++failed;
        }
    };

    run("Empty capture does not overwrite stored state", TestEmptyCaptureDoesNotOverwriteStoredState());
    run("Real capture is still published", TestRealCaptureIsStillPublished());
    run("Gesture coalesces captures", TestGestureCoalescesCaptures());
    run("Ungestured changes are throttled", TestUngesturedChangesAreThrottled());
    run("Unbalanced gesture end does not wedge capture", TestUnbalancedGestureEndDoesNotWedgeCapture());
    run("Prepare does not restore state twice", TestPrepareDoesNotRestoreStateTwice());
    run("Rebuilt node takes over its predecessor's instance", TestRebuiltNodeTakesOverItsPredecessorsInstance());
    run("Hand-over restores state the instance does not hold", TestHandOverRestoresStateTheInstanceDoesNotHold());
    run("Node naming another plugin does not take the instance", TestNodeNamingAnotherPluginDoesNotTakeTheInstance());
    run("Hand-over without a predecessor loads the plugin itself", TestHandOverWithoutPredecessorLoadsItself());

    std::cout << "\nHosted plugin state capture tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
