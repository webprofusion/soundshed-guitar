#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessorAdapter.h"
#include "PluginEditor.h"

//==============================================================================
/**
 * Minimal standalone window following JUCE tutorial pattern.
 * Provides a resizable, native-looking window with standard controls.
 */
class MainWindow : public juce::DocumentWindow
{
public:
    explicit MainWindow (const juce::String& appName,
                        std::unique_ptr<PluginProcessorAdapter> processor)
        : DocumentWindow (appName,
                         juce::Desktop::getInstance().getDefaultLookAndFeel()
                                                     .findColour (juce::ResizableWindow::backgroundColourId),
                         juce::DocumentWindow::allButtons),
          mProcessor (std::move (processor))
    {
        setUsingNativeTitleBar (true);
        setResizable (true, true);
        setResizeLimits (1024, 768, 8192, 8192);

        // Create and embed the editor
        if (auto* editor = mProcessor->createEditor())
        {
            setContentOwned (editor, true);
        }

        centreWithSize (1200, 900);
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

    PluginProcessorAdapter& getProcessor() { return *mProcessor; }

private:
    std::unique_ptr<PluginProcessorAdapter> mProcessor;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

//==============================================================================
/**
 * Minimal standalone application.
 * Creates a processor, wraps it in a window, and manages the app lifecycle.
 */
class SoundshedGuitarApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return PRODUCT_NAME_WITHOUT_VERSION; }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        auto processor = std::make_unique<PluginProcessorAdapter>();
        processor->setPlayHead (nullptr);
        mMainWindow = std::make_unique<MainWindow> (getApplicationName(), std::move (processor));
    }

    void shutdown() override
    {
        mMainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted (const juce::String&) override
    {
    }

private:
    std::unique_ptr<MainWindow> mMainWindow;
};

//==============================================================================
START_JUCE_APPLICATION (SoundshedGuitarApplication)
