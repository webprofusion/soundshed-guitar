#pragma once

#include "editor/SoundshedEditorBase.h"
#include "nativeui/NanoContext.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace soundshed::nano
{
class NanoShell;
class NanoDebugServer;

/// The Soundshed Guitar Nano editor: a native JUCE UI for small displays, with no WebView.
///
/// It talks to the engine exactly as the web UI does - the same JSON messages, in process -
/// through guitarfx::uiclient::UiClient, which keeps the state the views draw
/// (docs/plans/native-ui.md). Messages from the engine are queued as they arrive and applied
/// on the next message-loop turn (handleAsyncUpdate), never inside the call that sent a
/// request.
class NanoEditor final : public soundshed::editor::SoundshedEditorBase,
                         private juce::AsyncUpdater
{
public:
    explicit NanoEditor (PluginProcessorAdapter& processor);
    ~NanoEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void handleDeepLinkFromAnotherInstance (const juce::String& deepLinkQuery) override;

    [[nodiscard]] NanoContext& getContext() noexcept { return *context; }

private:
    void idleTick() override;
    void handleAsyncUpdate() override;

    /// The status bar, cut-out and gesture bar on a phone; none on the desktop.
    [[nodiscard]] juce::BorderSize<int> systemInsets() const;

    std::unique_ptr<NanoContext> context;
    std::unique_ptr<NanoShell> shell;
    std::unique_ptr<NanoDebugServer> debugServer;
    juce::BorderSize<int> appliedInsets;
    double startSeconds = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NanoEditor)
};
} // namespace soundshed::nano
