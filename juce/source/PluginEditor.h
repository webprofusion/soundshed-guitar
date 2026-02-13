#pragma once

#include "PluginProcessorAdapter.h"
#include <juce_gui_extra/juce_gui_extra.h>

#include <optional>

class SinglePageBrowser : public juce::WebBrowserComponent
{
public:
    using juce::WebBrowserComponent::WebBrowserComponent;
    bool pageAboutToLoad (const juce::String& newURL) override;
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
    [[nodiscard]] juce::File getStandaloneWindowStateFile() const;
    [[nodiscard]] juce::Rectangle<int> loadStandaloneWindowSize() const;
    void saveStandaloneWindowSize() const;

    PluginProcessorAdapter& processorRef;

    juce::File resourceRoot;
    juce::Rectangle<int> mLastWindowBounds;

    std::optional<juce::WebBrowserComponent::Resource> getResource (const juce::String& url);
    juce::String getResourceRootUrl() const;

    SinglePageBrowser webView;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
