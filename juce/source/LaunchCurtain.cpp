#include "LaunchCurtain.h"

#include <utility>

#if SOUNDSHED_LAUNCH_WEBVIEW_WARMUP
 #include "WebView2UserData.h"
 #include <juce_gui_extra/juce_gui_extra.h>
#endif

#if ! JUCE_ANDROID

#if SOUNDSHED_LAUNCH_WEBVIEW_WARMUP
//==============================================================================
// Starts WebView2's browser process while the engine starts up. The editor's WebView can
// only be made once MainWindow exists, after the plugin holder has initialised the
// controller and opened the audio device (1.6 s with an ASIO interface, measured), and the
// browser process then took another 0.4 s to come up before the page could even start
// loading. A hidden view in the same profile, made first, boots that process alongside the
// engine; the editor's view joins it. Its options must match the editor's where they reach
// the WebView2 environment (the profile folder), or the runtime refuses to share.
//
// JUCE creates WebView2 controllers one at a time (webView2ConstructionHelper), so the
// editor's waits for this one to finish being created. One destroyed part-way through
// leaves the next waiting for good, which is why LaunchCurtain keeps this until done.
class LaunchWebViewWarmup final : public juce::WebBrowserComponent
{
public:
    LaunchWebViewWarmup() : juce::WebBrowserComponent (options())
    {
        setVisible (false);
    }

    static juce::WebBrowserComponent::Options options()
    {
        return juce::WebBrowserComponent::Options {}
            .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
            .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2 {}
                    .withUserDataFolder (guitarfx::webview2::userDataFolderForThisProcess()))
            .withKeepPageLoadedWhenBrowserIsHidden();
    }

    /** Once it has a parent on the desktop: the controller needs a window to belong to. */
    void start()
    {
        goToURL ("about:blank");
    }

    bool isDone() const noexcept
    {
        return done;
    }

    std::function<void()> onDone;

private:
    void pageFinishedLoading (const juce::String&) override
    {
        finish();
    }

    bool pageLoadHadNetworkError (const juce::String&) override
    {
        finish();
        return false;
    }

    void finish()
    {
        if (std::exchange (done, true))
            return;

        if (onDone != nullptr)
            onDone();
    }

    bool done = false;
};
#endif

//==============================================================================
LaunchCurtain::LaunchCurtain (juce::Rectangle<int> bounds, bool warmUpWebView)
{
    setOpaque (true);
    setVisible (true);
    setBounds (bounds);
    addToDesktop (juce::ComponentPeer::windowIsTemporary | juce::ComponentPeer::windowIgnoresKeyPresses);
    toFront (false);

    // Painted now: the message thread is about to spend a while in the engine's
    // startup, and an unpainted window shows whatever the desktop had there.
    repaint();

    if (auto* peer = getPeer())
        peer->performAnyPendingRepaintsNow();

#if SOUNDSHED_LAUNCH_WEBVIEW_WARMUP
    if (warmUpWebView && juce::WebBrowserComponent::areOptionsSupported (LaunchWebViewWarmup::options()))
    {
        warmup = std::make_unique<LaunchWebViewWarmup>();
        warmup->onDone = [this] { runWhenIdle(); };
        addChildComponent (*warmup);
        warmup->start();
    }
#else
    juce::ignoreUnused (warmUpWebView);
#endif
}

LaunchCurtain::~LaunchCurtain() = default;

void LaunchCurtain::paint (juce::Graphics& g)
{
    // Mirrors core/ui/css/themes/dark.css --bg-primary: the dark theme is the default,
    // so this is what should already be behind the WebView by the time it is safe to
    // drop the curtain.
    g.fillAll (juce::Colour (0xff111116));
}

void LaunchCurtain::coverMainWindow()
{
    setAlwaysOnTop (true);
    startTimer (8000);
}

void LaunchCurtain::dismiss (std::function<void()> deleteMe)
{
    stopTimer();
    setVisible (false);
    whenIdle = std::move (deleteMe);

#if SOUNDSHED_LAUNCH_WEBVIEW_WARMUP
    if (warmup != nullptr && ! warmup->isDone())
        return;
#endif

    runWhenIdle();
}

void LaunchCurtain::timerCallback()
{
    stopTimer();

    if (onTimedOut != nullptr)
        onTimedOut();
}

void LaunchCurtain::runWhenIdle()
{
    // Async: this can be reached from the warm-up's own page callback, and the owner
    // deletes the curtain, warm-up included.
    if (auto callback = std::exchange (whenIdle, nullptr))
        juce::MessageManager::callAsync (std::move (callback));
}

#endif
