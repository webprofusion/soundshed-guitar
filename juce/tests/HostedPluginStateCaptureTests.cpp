// Capture-side guards in the hosted plugin effect, exercised against a stub plugin so they
// run everywhere rather than only on a machine with a particular VST3 installed.
//
// Covers the two ways auto-capture used to misbehave:
//   * an empty capture being treated as authoritative, which propagated an empty value that
//     the controller turns into an erase of the node's stored state;
//   * a capture per parameter tick during a knob drag, each one a full getStateInformation()
//     on the message thread.

#include "JuceHostedPluginEffect.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

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
    explicit StubPluginInstance(juce::String stateText, bool withParameter = true)
        : mStateText(std::move(stateText))
    {
        if (!withParameter)
            return;

        auto parameter = std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"p", 1}, "P", 0.0f, 1.0f, 0.5f);
        mParameter = parameter.get();
        addHostedParameter(std::move(parameter));
    }

    void SetStateText(juce::String stateText) { mStateText = std::move(stateText); }

    /// Stands in for a plugin reporting that something other than a parameter changed —
    /// an internal preset load, a sample swap. The only capture trigger available to a
    /// plugin with no automatable parameters.
    void NotifyNonParameterStateChanged()
    {
        updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
    }
    [[nodiscard]] int StateReadCount() const { return mStateReadCount; }
    void ResetStateReadCount() { mStateReadCount = 0; }
    [[nodiscard]] juce::AudioParameterFloat* Parameter() const { return mParameter; }

    // ── AudioPluginInstance ────────────────────────────────────────
    void fillInPluginDescription(juce::PluginDescription& description) const override
    {
        description.name = "Stub";
        description.pluginFormatName = "Stub";
        description.manufacturerName = "Soundshed Tests";
        description.uniqueId = 1;
    }

    // ── AudioProcessor ─────────────────────────────────────────────
    const juce::String getName() const override { return "Stub"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override
    {
        ++mStateReadCount;
        destData.reset();
        if (mStateText.isNotEmpty())
            destData.append(mStateText.toRawUTF8(), static_cast<size_t>(mStateText.getNumBytesAsUTF8()));
    }

    void setStateInformation(const void* data, int sizeInBytes) override
    {
        mStateText = juce::String::fromUTF8(static_cast<const char*>(data), sizeInBytes);
    }

private:
    juce::String mStateText;
    int mStateReadCount = 0;
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
                ++count;
        }
        return count;
    }

    [[nodiscard]] std::string LastStateValue() const
    {
        for (auto it = changes.rbegin(); it != changes.rend(); ++it)
        {
            if (it->first == kStateConfigKey)
                return it->second;
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
    effect.SetRuntimeConfigChangedCallback([&captured](const std::string& key, const std::string& value) {
        captured.changes.emplace_back(key, value);
    });

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
    effect.SetRuntimeConfigChangedCallback([&captured](const std::string& key, const std::string& value) {
        captured.changes.emplace_back(key, value);
    });

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
    effect.SetRuntimeConfigChangedCallback([&captured](const std::string& key, const std::string& value) {
        captured.changes.emplace_back(key, value);
    });

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
        Fail("the drag captured state " + std::to_string(readsDuringDrag)
             + " time(s) before the gesture ended; it should capture once, at the end");
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

    // A change outside any gesture still captures immediately.
    raw->ResetStateReadCount();
    raw->SetStateText("state-after-drag");
    parameter->setValueNotifyingHost(0.1f);
    if (raw->StateReadCount() == 0)
    {
        Fail("a change outside a gesture did not capture");
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

    if (raw->StateReadCount() == 0)
    {
        Fail("capture stayed suppressed after an unbalanced gesture end");
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
        if (ok) ++passed; else ++failed;
    };

    run("Empty capture does not overwrite stored state", TestEmptyCaptureDoesNotOverwriteStoredState());
    run("Real capture is still published", TestRealCaptureIsStillPublished());
    run("Gesture coalesces captures", TestGestureCoalescesCaptures());
    run("Unbalanced gesture end does not wedge capture", TestUnbalancedGestureEndDoesNotWedgeCapture());

    std::cout << "\nHosted plugin state capture tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
