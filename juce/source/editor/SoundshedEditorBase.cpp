#include "SoundshedEditorBase.h"

#include "StartupLog.h"

namespace soundshed::editor
{
void sendUiMessage (PluginProcessorAdapter& processor, const char* type, nlohmann::json payload)
{
    if (! payload.is_object())
        payload = nlohmann::json::object();

    payload["type"] = type;
    processor.handleWebMessage (juce::String (payload.dump()));
}

SoundshedEditorBase::SoundshedEditorBase (PluginProcessorAdapter& processor, SizeLimits limits)
    : AudioProcessorEditor (&processor),
      processorRef (processor),
      sizeLimits (limits)
{
}

SoundshedEditorBase::~SoundshedEditorBase()
{
    stopTimer();

    // The editor is going, so nobody reads the telemetry feeds any more: say so, which also
    // switches the per-node metering on the audio thread off until an editor comes back.
    reportUiVisible (false);

    // One line per editor lifetime, and the pair that matters when a window comes back at
    // the wrong size: what the editor was last laid out at, and what was actually kept.
    const auto remembered = processorRef.getController().GetEditorWindowSize();
    writeStartupLog ("[Editor] closing at " + juce::String (getWidth()) + "x" + juce::String (getHeight())
                     + "; remembered "
                     + (remembered.IsValid() ? juce::String (remembered.width) + "x" + juce::String (remembered.height)
                                             : juce::String ("nothing")));
}

void SoundshedEditorBase::handleDeepLinkFromAnotherInstance (const juce::String&)
{
    if (auto* window = getTopLevelComponent())
        window->toFront (true);
}

void SoundshedEditorBase::applyInitialEditorSize()
{
    setResizable (true, true);
    setResizeLimits (sizeLimits.minWidth, sizeLimits.minHeight, sizeLimits.maxWidth, sizeLimits.maxHeight);

    // Reopen at the size this instance was last left at. The size is restored from the
    // DAW project (host state), so it survives both closing the editor window and
    // reopening the project; a brand-new instance falls back to the default. Clamped to
    // the resize limits, which setSize() itself does not enforce.
    const auto rememberedSize = processorRef.getController().GetEditorWindowSize();
    const auto defaultSize = defaultEditorSize();
    const auto initialWidth = rememberedSize.IsValid()
                                  ? juce::jlimit (sizeLimits.minWidth, sizeLimits.maxWidth, rememberedSize.width)
                                  : defaultSize.x;
    const auto initialHeight = rememberedSize.IsValid()
                                   ? juce::jlimit (sizeLimits.minHeight, sizeLimits.maxHeight, rememberedSize.height)
                                   : defaultSize.y;
    setSize (initialWidth, initialHeight);
    writeStartupLog ("[Editor] opening at " + juce::String (initialWidth) + "x" + juce::String (initialHeight)
                     + (rememberedSize.IsValid() ? " (remembered)" : " (default for this display)"));

    // This drives state broadcasts, DSP performance updates, tuner data, etc.
    startTimerHz (60);
}

juce::Point<int> SoundshedEditorBase::defaultEditorSize() const
{
    // A share of the display rather than a fixed pixel size, because no fixed pair works on
    // both ends of the range: 1200x900 is only half the width of a 4K desktop running at
    // 175%, while 1600x1000 is wider than a 1366x768 laptop screen. The preferred size is the
    // cap - past it the extra pixels stop buying anything.
    constexpr double displayFraction = 0.8;
    auto width = sizeLimits.preferredWidth;
    auto height = sizeLimits.preferredHeight;

    // There is no peer yet while the editor is being constructed, so this is the primary
    // display rather than the one the host's window will end up on. userArea excludes the
    // taskbar and is in the same logical units the editor is sized in.
    if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
    {
        const auto usable = display->userArea;
        width = juce::jmin (width, juce::roundToInt ((double) usable.getWidth() * displayFraction));
        height = juce::jmin (height, juce::roundToInt ((double) usable.getHeight() * displayFraction));
    }

    return { juce::jlimit (sizeLimits.minWidth, sizeLimits.maxWidth, width),
             juce::jlimit (sizeLimits.minHeight, sizeLimits.maxHeight, height) };
}

void SoundshedEditorBase::rememberEditorSize()
{
    // Remember the size the host left us at, so reopening this editor - or this project -
    // comes back the same size. Only the size: the DAW owns where the window sits.
    //
    // Only while we are actually on screen, though. A host resizes the editor on its way
    // to closing the window as well as while the user drags it, and setResizeLimits means
    // a degenerate rect arrives here already clamped up to the minimum size - a plausible
    // looking size nobody chose. Remembering that is what makes the next open come up
    // tiny. The controller drops anything that does not then survive an idle tick.
    if (isShowing())
    {
        processorRef.getController().SetEditorWindowSize (getWidth(), getHeight());
    }
    else if (getWidth() != lastIgnoredResize.x || getHeight() != lastIgnoredResize.y)
    {
        lastIgnoredResize = { getWidth(), getHeight() };
        writeStartupLog ("[Editor] not remembering off-screen resize to "
                         + juce::String (getWidth()) + "x" + juce::String (getHeight()));
    }
}

void SoundshedEditorBase::reportUiVisible (bool visible)
{
    const int state = visible ? 1 : 0;

    if (state == lastReportedVisible)
        return;

    lastReportedVisible = state;
    sendUiMessage (processorRef, "uiVisibility", { { "visible", visible } });
}

void SoundshedEditorBase::timerCallback()
{
    processorRef.getController().OnIdle();
    idleTick();
}

void SoundshedEditorBase::setScaleFactor (float newScale)
{
    hostSuppliedScaleFactor = true;
    juce::AudioProcessorEditor::setScaleFactor (newScale);
    applyHostScaleWorkaround();
}

void SoundshedEditorBase::parentHierarchyChanged()
{
    // The peer is created when the wrapper calls addToDesktop(), which happens after the host
    // has already pushed its scale factor at us, so re-apply the workaround against the new peer.
    applyHostScaleWorkaround();
}

// Works around double DPI scaling under CLAP on Windows.
//
// CLAP's win32 window API is defined in *physical* pixels, and the clap-juce-extensions wrapper
// is written on that basis: it applies the host's DPI scale as a JUCE editor transform via
// setScaleFactor(), then reports the resulting JUCE bounds straight back to the host from
// guiGetSize()/guiRequestResize(). That is only correct while JUCE logical units and physical
// pixels are the same thing.
//
// They are not. The wrapper attaches us with addToDesktop(0, hostHwnd), and a per-monitor
// DPI-aware HWNDComponentPeer inherits its platform scale from the parent window, so the peer is
// already scaling by the monitor DPI. The host scale then lands on top as a transform and the UI
// renders at scale squared - 2.25x on a 150% display - while the host sizes its window for only
// one factor, so the UI is both oversized and clipped.
//
// JUCE's own VST3 wrapper sidesteps this by routing the host scale through
// ComponentPeer::setCustomPlatformScaleFactor(), which replaces the platform scale instead of
// compounding with it. We can't reach that path from CLAP, so do the equivalent from this side:
// pin the peer to 1.0 so JUCE units are physical pixels again, which is the contract the wrapper
// assumes, and let the host-supplied transform be the only scale in play.
//
// Only Windows is affected. On macOS the wrapper never calls setScaleFactor(), and X11 peers
// report a platform scale of 1.0 already, so the host scale is the only one applied there too.
void SoundshedEditorBase::applyHostScaleWorkaround()
{
#if JUCE_WINDOWS && JUCE_WIN_PER_MONITOR_DPI_AWARE
    if (! hostSuppliedScaleFactor)
        return;

    auto* peer = getPeer();
    if (peer == nullptr || peer->getCustomPlatformScaleFactor().has_value())
        return;

    writeStartupLog ("[Editor] pinning peer platform scale to 1.0 (was "
                     + juce::String (peer->getPlatformScaleFactor(), 3)
                     + "); host scale factor is applied as an editor transform");

    peer->setCustomPlatformScaleFactor (1.0);
#endif
}
} // namespace soundshed::editor
