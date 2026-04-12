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

private:
    void timerCallback() override;
    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);
    juce::String getResourceRootUrl() const;

   #if JUCE_LINUX
    void markLinuxWebViewLoaded (const juce::String& url);
    bool handleLinuxWebViewNetworkError (const juce::String& errorInfo);
    void showLinuxWebViewDependencyMessage (const juce::String& reason);
   #endif

    PluginProcessorAdapter& processorRef;

    juce::File resourceRoot;

    SinglePageBrowser webView;

   #if JUCE_LINUX
    juce::Label linuxWebViewStatusLabel;
    bool linuxWebViewLoadCompleted = false;
    bool linuxWebViewFallbackShown = false;
    juce::uint32 linuxWebViewInitTimeMs = 0;
   #endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
