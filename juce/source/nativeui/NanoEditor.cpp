#include "nativeui/NanoEditor.h"

#include "ProductInfo.h"
#include "editor/StartupLog.h"
#include "nativeui/NanoShell.h"
#include "nativeui/debug/NanoDebugServer.h"

namespace soundshed::nano
{
namespace
{
soundshed::editor::SoundshedEditorBase::SizeLimits nanoSizeLimits()
{
    soundshed::editor::SoundshedEditorBase::SizeLimits limits;
    limits.minWidth = soundshed::product::minEditorWidth;
    limits.minHeight = soundshed::product::minEditorHeight;
    limits.preferredWidth = soundshed::product::preferredEditorWidth;
    limits.preferredHeight = soundshed::product::preferredEditorHeight;
    return limits;
}
} // namespace

NanoEditor::NanoEditor (PluginProcessorAdapter& processor)
    : SoundshedEditorBase (processor, nanoSizeLimits()),
      context (std::make_unique<NanoContext> (processor))
{
    setLookAndFeel (context->lookAndFeel.get());
    startSeconds = NanoContext::now();

    // Everything the engine sends lands in the client's queue and is applied on the next
    // message-loop turn. The adapter already moves off-thread sends onto the message thread.
    processorRef.setWebMessageCallback ([this] (const juce::String& message) {
        context->client.Enqueue (message.toStdString());
        triggerAsyncUpdate();
    });

    shell = std::make_unique<NanoShell> (*context);
    addAndMakeVisible (*shell);
    shell->sendLookAndFeelChange(); // built before it had this editor's look to inherit
    shell->onScaleChanged = [this] { resized(); };

    if (const auto port = juce::SystemStats::getEnvironmentVariable ("SOUNDSHED_NANO_DEBUG_PORT", {}).getIntValue(); port > 0)
        debugServer = std::make_unique<NanoDebugServer> (*this, port);

    applyInitialEditorSize();

    context->client.Start();
    reportUiVisible (true);
    soundshed::editor::writeStartupLog ("[Nano] editor started");
}

NanoEditor::~NanoEditor()
{
    processorRef.setWebMessageCallback (nullptr);
    cancelPendingUpdate();
    debugServer = nullptr;
    shell = nullptr;
    setLookAndFeel (nullptr);
}

void NanoEditor::paint (juce::Graphics& g)
{
    g.fillAll (context->theme.background());
}

juce::BorderSize<int> NanoEditor::systemInsets() const
{
#if JUCE_ANDROID || JUCE_IOS
    // A phone's app fills the screen: keep clear of the status bar, a camera cut-out and the
    // gesture bar. JUCE reads them from the activity's window, but only when it refreshes its
    // display list, which on Android happens after the editor's first layout (idleTick
    // re-lays out when they change). The user area is a second source where it differs.
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds()))
    {
        const auto& safe = display->safeAreaInsets;
        const auto total = display->totalArea;
        const auto user = display->userArea;
        return { juce::jmax (safe.getTop(), user.getY() - total.getY()),
                 juce::jmax (safe.getLeft(), user.getX() - total.getX()),
                 juce::jmax (safe.getBottom(), total.getBottom() - user.getBottom()),
                 juce::jmax (safe.getRight(), total.getRight() - user.getRight()) };
    }
#endif

    return {};
}

void NanoEditor::resized()
{
    if (shell != nullptr)
    {
        // The editor's own background shows in the margins the system bars need.
        appliedInsets = systemInsets();
        const auto area = appliedInsets.subtractedFrom (getLocalBounds());

        // The shell is laid out at the user's scale: smaller logical size, scaled back up.
        const float scale = shell->uiScale();
        shell->setTransform (juce::AffineTransform::scale (scale).translated ((float) area.getX(), (float) area.getY()));
        shell->setBounds (0, 0, juce::roundToInt ((float) area.getWidth() / scale), juce::roundToInt ((float) area.getHeight() / scale));
    }

    rememberEditorSize();
}

void NanoEditor::handleDeepLinkFromAnotherInstance (const juce::String& deepLinkQuery)
{
    // Deep links open tone sharing, which is Soundshed Guitar's; Nano leaves it out.
    if (deepLinkQuery.isNotEmpty() && context->showToast)
        context->showToast ("Open this link in Soundshed Guitar", "Tone sharing is not part of Nano.", false);

    SoundshedEditorBase::handleDeepLinkFromAnotherInstance (deepLinkQuery);
}

void NanoEditor::handleAsyncUpdate()
{
    context->client.DrainPending();
}

void NanoEditor::idleTick()
{
    const auto now = NanoContext::now();
    context->client.Tick (now);

    // Engine messages normally arrive through the async update; draining here too means a
    // queue never waits on a message loop that is busy with something else.
    context->client.DrainPending();

    reportUiVisible (isShowing());

    // The system bars arrive (and move, on rotation) after the layout that needed them.
    if (systemInsets() != appliedInsets)
        resized();

    if (shell != nullptr)
        shell->idleTick (now);
}
} // namespace soundshed::nano
