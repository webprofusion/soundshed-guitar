#pragma once

#include "PluginProcessorAdapter.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <nlohmann/json.hpp>

namespace soundshed::editor
{
/// Sends one UI→engine message from the host side of an editor, the way the page or the
/// native client would. The type is a literal at every call site, which is what lets
/// tools/check-protocol.mjs see these sends.
void sendUiMessage (PluginProcessorAdapter& processor, const char* type, nlohmann::json payload = nlohmann::json::object());

/// Everything an editor does that has nothing to do with how it draws: shared by the
/// WebView editor (Soundshed Guitar) and the native one (Soundshed Guitar Nano).
///
///  - Drives PluginController::OnIdle() at 60 Hz. Nothing else ticks it while an editor is
///    open, and the telemetry rate dividers assume that rate.
///  - Tells the controller whether a UI is on screen ("uiVisibility"), including on close,
///    so per-node metering stops costing audio-thread time once nobody reads it.
///  - Opens at the size this instance was left at, and remembers the size it is left at.
///  - Works around double DPI scaling under CLAP on Windows (applyHostScaleWorkaround).
class SoundshedEditorBase : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    struct SizeLimits
    {
        int minWidth = 640;
        int minHeight = 400;
        int maxWidth = 8192;
        int maxHeight = 8192;
        int preferredWidth = 1600;
        int preferredHeight = 1100;
    };

    SoundshedEditorBase (PluginProcessorAdapter& processor, SizeLimits limits);
    ~SoundshedEditorBase() override;

    void setScaleFactor (float newScale) override;
    void parentHierarchyChanged() override;

    /// A deep link handed over by a second launch of the standalone app.
    virtual void handleDeepLinkFromAnotherInstance (const juce::String& deepLinkQuery);

protected:
    /// Call at the end of the derived constructor, once the content exists: applies the
    /// resize limits and opens at the remembered or default size.
    void applyInitialEditorSize();

    /// Call from the derived resized(): remembers the size the host left us at.
    void rememberEditorSize();

    /// Per-tick work of the derived editor, after the controller's own idle pass.
    virtual void idleTick() {}

    /// Reports whether a UI is on screen; a repeat of the last report is not re-sent. The
    /// base reports "hidden" on close. While open, the WebView page reports its own
    /// visibility, and the native editor reports isShowing() from its idle tick.
    void reportUiVisible (bool visible);

    PluginProcessorAdapter& processorRef;

private:
    void timerCallback() override;
    void applyHostScaleWorkaround();
    [[nodiscard]] juce::Point<int> defaultEditorSize() const;

    SizeLimits sizeLimits;

    // Set once a host has pushed its DPI scale at us via setScaleFactor(). Of the formats we
    // build, only the CLAP wrapper does that - see applyHostScaleWorkaround() for why it matters.
    bool hostSuppliedScaleFactor = false;

    // Last off-screen size we logged as not-remembered, so a host that keeps re-reporting
    // one does not fill the log with it. See rememberEditorSize().
    juce::Point<int> lastIgnoredResize { -1, -1 };

    // What was last reported through reportUiVisible, so a repeat is not re-sent.
    int lastReportedVisible = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SoundshedEditorBase)
};
} // namespace soundshed::editor
