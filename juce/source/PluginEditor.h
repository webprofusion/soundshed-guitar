#pragma once

#include "PluginProcessorAdapter.h"
#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <optional>

class SinglePageBrowser : public juce::WebBrowserComponent
{
public:
    using PageFinishedCallback = std::function<void (const juce::String&)>;
    using NetworkErrorCallback = std::function<bool (const juce::String&)>;

    using juce::WebBrowserComponent::WebBrowserComponent;

    void setPageFinishedCallback (PageFinishedCallback callback);
    void setNetworkErrorCallback (NetworkErrorCallback callback);

    bool pageAboutToLoad (const juce::String& newURL) override;
    void pageFinishedLoading (const juce::String& url) override;
    bool pageLoadHadNetworkError (const juce::String& errorInfo) override;
    void newWindowAttemptingToLoad (const juce::String& newURL) override;

private:
    PageFinishedCallback pageFinishedCallback;
    NetworkErrorCallback networkErrorCallback;
};

//==============================================================================
class PluginEditor : public juce::AudioProcessorEditor,
                     private juce::Timer
{
public:
    explicit PluginEditor (PluginProcessorAdapter&);
    ~PluginEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;
    void setScaleFactor (float newScale) override;
    void parentHierarchyChanged() override;

    // Handle deep link from another instance of the app
    void handleDeepLinkFromAnotherInstance (const juce::String& deepLinkQuery);

private:
    void timerCallback() override;
    void applyHostScaleWorkaround();
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);
    juce::String getResourceRootUrl() const;

   #if JUCE_LINUX
    void markLinuxWebViewLoaded (const juce::String& url);
    bool handleLinuxWebViewNetworkError (const juce::String& errorInfo);
    void showLinuxWebViewDependencyMessage (const juce::String& reason);
   #endif

    PluginProcessorAdapter& processorRef;

    juce::File resourceRoot;

    // Set once a host has pushed its DPI scale at us via setScaleFactor(). Of the formats we
    // build, only the CLAP wrapper does that - see applyHostScaleWorkaround() for why it matters.
    bool hostSuppliedScaleFactor = false;

    SinglePageBrowser webView;

    // Last off-screen size we logged as not-remembered, so a host that keeps re-reporting
    // one does not fill the log with it. See resized().
    juce::Point<int> lastIgnoredResize { -1, -1 };

   #if JUCE_LINUX
    juce::Label linuxWebViewStatusLabel;
    bool linuxWebViewLoadCompleted = false;
    bool linuxWebViewFallbackShown = false;
    bool linuxWebViewSlowLoadLogged = false;
    bool linuxWebViewRetryAttempted = false;
    int linuxWebViewNetworkErrorCount = 0;
    juce::uint32 linuxWebViewInitTimeMs = 0;
    juce::String linuxInitialUrl;
   #endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
