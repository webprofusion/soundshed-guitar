#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

// WebView2 only, and only where JUCE's web browser is built in (not Nano, which draws natively).
#define SOUNDSHED_LAUNCH_WEBVIEW_WARMUP (JUCE_WINDOWS && JUCE_WEB_BROWSER)

#if ! JUCE_ANDROID

#if SOUNDSHED_LAUNCH_WEBVIEW_WARMUP
class LaunchWebViewWarmup;
#endif

//==============================================================================
// Hides the startup flicker between MainWindow going on screen and the editor having
// something real to show: on Windows that's JUCE's WebView2 fallback paint (solid white,
// juce_WebBrowserComponent_windows.cpp), a few frames of the browser surface before its
// first frame, then its own transparent background, then the page's first paint before
// the theme class lands on <body> (ThemeSwitcher runs from a module script, after first
// paint). A plain child Component added on top of the WebView cannot cover any of that:
// WebView2 owns a real native child HWND, which always draws above its parent's
// software-painted content regardless of JUCE's z-order ("airspace"). This is its own
// always-on-top native window instead, a sibling of MainWindow rather than a child of it,
// so normal OS window-manager z-order hides MainWindow - and whatever it is doing - until
// the owner takes the curtain down.
//
// Self-triggers onTimedOut after a timeout so a WebView that never finishes loading
// (resource root missing, WebView2 runtime missing) does not leave the window
// permanently covered.
//
// The app puts it up first thing, before the plugin holder initialises the engine and
// opens the audio device, so a launch shows a window at once instead of nothing for the
// second and a half that takes. On Windows it also carries the LaunchWebViewWarmup
// (LaunchCurtain.cpp). It only goes always-on-top once MainWindow exists (coverMainWindow):
// until then there is nothing of ours to hide, and an audio driver that puts up its own
// message while the device opens must not end up behind it.
class LaunchCurtain : public juce::Component,
                      private juce::Timer
{
public:
    LaunchCurtain (juce::Rectangle<int> bounds, bool warmUpWebView);
    ~LaunchCurtain() override;

    void paint (juce::Graphics& g) override;

    /** From MainWindow's constructor, before it shows: stays above it from now on, and gives
        the page its time to load from here. */
    void coverMainWindow();

    /** Takes the curtain off the screen now, and calls `deleteMe` (asynchronously) once
        nothing it carries is still being set up, at which point the owner deletes it. */
    void dismiss (std::function<void()> deleteMe);

    std::function<void()> onTimedOut;

private:
    void timerCallback() override;
    void runWhenIdle();

    std::function<void()> whenIdle;

#if SOUNDSHED_LAUNCH_WEBVIEW_WARMUP
    std::unique_ptr<LaunchWebViewWarmup> warmup;
#endif
};

#endif
