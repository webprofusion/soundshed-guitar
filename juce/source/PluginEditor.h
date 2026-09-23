#pragma once

#include "PluginProcessorAdapter.h"
#include "editor/SoundshedEditorBase.h"
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
/// The Soundshed Guitar editor: the WebView UI in core/ui. What every editor does regardless of
/// how it draws (idle tick, sizing, DPI, visibility) lives in SoundshedEditorBase.
class PluginEditor : public soundshed::editor::SoundshedEditorBase
{
public:
    explicit PluginEditor (PluginProcessorAdapter&);
    ~PluginEditor() override;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;

    // Handle deep link from another instance of the app
    void handleDeepLinkFromAnotherInstance (const juce::String& deepLinkQuery) override;

private:
    void idleTick() override;
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);
    juce::String getResourceRootUrl() const;

   #if JUCE_LINUX
    void markLinuxWebViewLoaded (const juce::String& url);
    bool handleLinuxWebViewNetworkError (const juce::String& errorInfo);
    void showLinuxWebViewDependencyMessage (const juce::String& reason);
   #endif

    juce::File resourceRoot;

    SinglePageBrowser webView;

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
