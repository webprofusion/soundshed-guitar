#include "PluginProcessorAdapter.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>
#include <juce_gui_basics/juce_gui_basics.h>

//==============================================================================
class MainWindow : public juce::DocumentWindow
{
public:
    explicit MainWindow (const juce::String& appName,
        std::unique_ptr<juce::StandalonePluginHolder> pluginHolderIn)
        : DocumentWindow (appName,
              juce::Desktop::getInstance().getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
              juce::DocumentWindow::allButtons),
          mPluginHolder (std::move (pluginHolderIn))
    {
        setUsingNativeTitleBar (true);
        setResizable (true, true);
        setResizeLimits (1024, 768, 8192, 8192);

        if (auto* processor = mPluginHolder != nullptr ? mPluginHolder->processor.get() : nullptr)
        {
            auto* editor = processor->hasEditor()
                               ? processor->createEditorIfNeeded()
                               : static_cast<juce::AudioProcessorEditor*> (new juce::GenericAudioProcessorEditor (*processor));

            if (editor != nullptr)
            {
                setContentOwned (editor, true);
                setResizable (editor->isResizable(), true);
            }
        }

        const auto state = loadWindowState();
        centreWithSize (state.width, state.height);

        if (state.maximized)
            setFullScreen (true);

        mPluginHolder->startPlaying();
        setVisible (true);
    }

    ~MainWindow() override
    {
        saveWindowState();

        if (auto* editor = dynamic_cast<juce::AudioProcessorEditor*> (getContentComponent()))
            if (auto* processor = mPluginHolder != nullptr ? mPluginHolder->processor.get() : nullptr)
                processor->editorBeingDeleted (editor);

        clearContentComponent();

        if (mPluginHolder != nullptr)
            mPluginHolder->stopPlaying();
    }

    void resized() override
    {
        juce::DocumentWindow::resized();

        if (!isFullScreen())
            mLastNonMaximizedBounds = getBounds();
    }

    void closeButtonPressed() override
    {
        saveWindowState();

        if (mPluginHolder != nullptr)
            mPluginHolder->savePluginState();

        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }

    juce::StandalonePluginHolder* getPluginHolder() const noexcept
    {
        return mPluginHolder.get();
    }

private:
    struct WindowState
    {
        int width = 1200;
        int height = 900;
        bool maximized = false;
    };

    juce::File getWindowStateFile() const
    {
        auto* adapter = dynamic_cast<PluginProcessorAdapter*> (
            mPluginHolder != nullptr ? mPluginHolder->processor.get() : nullptr);
        if (adapter == nullptr)
            return {};

        const auto path = adapter->GetUserDataPath() / "data" / "v1" / "settings" / "ui" / "window-state.json";
        return juce::File (path.string());
    }

    WindowState loadWindowState() const
    {
        WindowState state;

        const auto file = getWindowStateFile();
        if (file.existsAsFile())
        {
            const auto parsed = juce::JSON::parse (file.loadFileAsString());
            if (auto* obj = parsed.getDynamicObject(); obj != nullptr)
            {
                const auto widthId = juce::Identifier { "width" };
                const auto heightId = juce::Identifier { "height" };
                const auto maximizedId = juce::Identifier { "maximized" };

                if (obj->hasProperty (widthId))
                    state.width = static_cast<int> (obj->getProperty (widthId));
                if (obj->hasProperty (heightId))
                    state.height = static_cast<int> (obj->getProperty (heightId));
                if (obj->hasProperty (maximizedId))
                    state.maximized = static_cast<bool> (obj->getProperty (maximizedId));
            }
        }

        // Clamp to the primary display's usable area.
        if (auto* primary = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            const auto userArea = primary->userArea;
            state.width = juce::jmin (state.width, userArea.getWidth());
            state.height = juce::jmin (state.height, userArea.getHeight());
        }

        state.width = juce::jlimit (1024, 8192, state.width);
        state.height = juce::jlimit (768, 8192, state.height);

        return state;
    }

    void saveWindowState() const
    {
        const auto file = getWindowStateFile();
        if (file == juce::File {})
            return;

        file.getParentDirectory().createDirectory();

        juce::DynamicObject::Ptr state (new juce::DynamicObject());
        state->setProperty ("width", mLastNonMaximizedBounds.getWidth());
        state->setProperty ("height", mLastNonMaximizedBounds.getHeight());
        state->setProperty ("maximized", isFullScreen());
        file.replaceWithText (juce::JSON::toString (juce::var (state)));
    }

    std::unique_ptr<juce::StandalonePluginHolder> mPluginHolder;

    juce::Rectangle<int> mLastNonMaximizedBounds { 0, 0, 1200, 900 };
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
};

//==============================================================================
class SoundshedGuitarApplication : public juce::JUCEApplication
{
public:
    SoundshedGuitarApplication()
    {
        juce::PropertiesFile::Options options;
        options.applicationName = PRODUCT_NAME_WITHOUT_VERSION;
        options.filenameSuffix = ".settings";
        options.osxLibrarySubFolder = "Application Support";
#if JUCE_LINUX || JUCE_BSD
        options.folderName = "~/.config";
#else
        options.folderName = {};
#endif
        mAppProperties.setStorageParameters (options);
    }

    const juce::String getApplicationName() override { return PRODUCT_NAME_WITHOUT_VERSION; }
    const juce::String getApplicationVersion() override { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        if (juce::Desktop::getInstance().getDisplays().displays.isEmpty())
        {
            jassertfalse;
            return;
        }

        mMainWindow = std::make_unique<MainWindow> (getApplicationName(), createPluginHolder());
    }

    void shutdown() override
    {
        mMainWindow = nullptr;
        mAppProperties.saveIfNeeded();
    }

    void systemRequestedQuit() override
    {
        if (mMainWindow != nullptr)
            if (auto* holder = mMainWindow->getPluginHolder())
                holder->savePluginState();

        if (juce::ModalComponentManager::getInstance()->cancelAllModalComponents())
        {
            juce::Timer::callAfterDelay (100, []() {
                if (auto* app = juce::JUCEApplicationBase::getInstance())
                    app->systemRequestedQuit();
            });

            return;
        }

        quit();
    }

    void anotherInstanceStarted (const juce::String&) override
    {
    }

private:
    std::unique_ptr<juce::StandalonePluginHolder> createPluginHolder()
    {
        constexpr auto autoOpenMidiDevices =
#if (JUCE_ANDROID || JUCE_IOS) && !JUCE_DONT_AUTO_OPEN_MIDI_DEVICES_ON_MOBILE
            true;
#else
            false;
#endif

#ifdef JucePlugin_PreferredChannelConfigurations
        constexpr juce::StandalonePluginHolder::PluginInOuts channels[] { JucePlugin_PreferredChannelConfigurations };
        const juce::Array<juce::StandalonePluginHolder::PluginInOuts> channelConfig (channels, juce::numElementsInArray (channels));
#else
        const juce::Array<juce::StandalonePluginHolder::PluginInOuts> channelConfig;
#endif

        return std::make_unique<juce::StandalonePluginHolder> (mAppProperties.getUserSettings(),
            false,
            juce::String {},
            nullptr,
            channelConfig,
            autoOpenMidiDevices);
    }

    juce::ApplicationProperties mAppProperties;
    std::unique_ptr<MainWindow> mMainWindow;
};

namespace juce
{
    void JUCE_CALLTYPE juce_showStandaloneAudioSettingsDialog()
    {
        if (auto* holder = StandalonePluginHolder::getInstance())
            holder->showAudioSettingsDialog();
    }
}

//==============================================================================
START_JUCE_APPLICATION (SoundshedGuitarApplication)
