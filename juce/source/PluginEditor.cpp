#include "PluginEditor.h"

#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <vector>

#include "UiBridge.h"

namespace
{
    const juce::String kResourceOrigin = "http://soundshed.local/";

    bool isMacStandaloneAppExecutable()
    {
       #if JUCE_MAC
        auto current = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        while (current != juce::File())
        {
            const auto ext = current.getFileExtension().toLowerCase();
            if (ext == ".app") return true;
            if (ext == ".vst3" || ext == ".component" || ext == ".au") return false;
            current = current.getParentDirectory();
        }
       #endif

        return false;
    }

    const char* getMimeForExtension (const juce::String& extension)
    {
        static const std::unordered_map<juce::String, const char*> mimeMap = {
            { "htm", "text/html" },
            { "html", "text/html" },
            { "txt", "text/plain" },
            { "jpg", "image/jpeg" },
            { "jpeg", "image/jpeg" },
            { "svg", "image/svg+xml" },
            { "ico", "image/vnd.microsoft.icon" },
            { "json", "application/json" },
            { "png", "image/png" },
            { "css", "text/css" },
            { "map", "application/json" },
            { "js", "text/javascript" },
            { "woff2", "font/woff2" }
        };

        const auto key = extension.toLowerCase();
        if (const auto it = mimeMap.find (key); it != mimeMap.end())
            return it->second;

        return "application/octet-stream";
    }

    std::vector<std::byte> readFileToVector (const juce::File& file)
    {
        juce::FileInputStream stream (file);
        if (! stream.openedOk())
            return {};

        std::vector<std::byte> result (static_cast<size_t> (stream.getTotalLength()));
        if (result.empty())
            return result;

        stream.read (result.data(), result.size());
        return result;
    }

}

bool SinglePageBrowser::pageAboutToLoad (const juce::String& newURL)
{
    return newURL.startsWith (getResourceProviderRoot());
}

PluginEditor::PluginEditor (PluginProcessorAdapter& p)
    : AudioProcessorEditor (&p),
      processorRef (p),
    resourceRoot ([]{
        // Pass the exe-relative resources path as an extra candidate so the
        // UI bundle is discovered regardless of the current working directory.
        const auto exeDir = std::filesystem::path (
            juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                .getParentDirectory().getFullPathName().toStdString());
        return juce::File (guitarfx::ui::ResolveResourceRoot ({ exeDir / "resources" }).string());
    }()),
      webView (juce::WebBrowserComponent::Options{}
                   .withBackend (juce::WebBrowserComponent::Options::Backend::webview2)
                   .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
                                                .withUserDataFolder (
                                                    juce::File::getSpecialLocation (juce::File::tempDirectory)
                                                        .getChildFile ("SoundshedGuitarWebView2")
                                                        .getChildFile (juce::String (juce::Time::getCurrentTime().toMilliseconds()))))
                   .withUserScript (
                       "window.IPlugSendMsg = function(payload) {"
                       "  try {"
                       "    const data = (typeof payload === 'string') ? payload : JSON.stringify(payload);"
                       "    window.__JUCE__.backend.emitEvent('iplugSendMsg', data);"
                       "  } catch (e) {"
                       "    window.__JUCE__.backend.emitEvent('iplugSendMsg', '');"
                       "  }"
                       "};")
                   .withNativeIntegrationEnabled()
                   .withEventListener ("iplugSendMsg", [this] (const juce::var& payload)
                   {
                       const auto message = payload.isString() ? payload.toString()
                                                               : juce::JSON::toString (payload);
                       if (message.isNotEmpty())
                           processorRef.handleWebMessage (message);
                   })
                   .withNativeFunction ("IPlugSendMsg", [this] (const juce::Array<juce::var>& args, auto complete)
                   {
                       if (args.size() > 0)
                       {
                           const auto& arg = args[0];
                           const auto payload = arg.isString() ? arg.toString()
                                                               : juce::JSON::toString (arg);
                           processorRef.handleWebMessage (payload);
                       }

                       complete (juce::var());
                   })
                   .withNativeFunction ("postMessage", [this] (const juce::Array<juce::var>& args, auto complete)
                   {
                       if (args.size() > 0)
                       {
                           const auto& arg = args[0];
                           const auto payload = arg.isString() ? arg.toString()
                                                               : juce::JSON::toString (arg);
                           processorRef.handleWebMessage (payload);
                       }

                       complete (juce::var());
                   })
                   .withResourceProvider ([this] (const auto& url)
                                         {
                                             return getResource (url);
                                         },
                                         std::optional<juce::String> { getResourceRootUrl() }))
{
    addAndMakeVisible (webView);

    processorRef.setWebMessageCallback ([this] (const juce::String& message)
    {
        const auto script = guitarfx::ui::BuildIPlugReceiveScript (message.toStdString());
        webView.evaluateJavascript (juce::String (script));
    });

   #if JUCE_WINDOWS
    const auto webView2Supported = juce::WebBrowserComponent::areOptionsSupported (
        juce::WebBrowserComponent::Options{}
            .withBackend (juce::WebBrowserComponent::Options::Backend::webview2));

    if (! webView2Supported)
    {
        const juce::String missingRuntimeHtml =
            "data:text/html;charset=UTF-8," \
            "<!doctype html><html><head><meta charset='utf-8'/>" \
            "<title>Soundshed Guitar</title></head>" \
            "<body style='font-family:sans-serif;background:#101014;color:#eee;padding:24px;line-height:1.5;'>" \
            "<h2>Microsoft Edge WebView2 Runtime is required</h2>" \
            "<p>The Soundshed Guitar UI cannot start because WebView2 is not available.</p>" \
            "<p>Install the runtime, then restart the app.</p>" \
            "<p>Download: <a href='https://aka.ms/webview2' style='color:#8ab4ff;'>https://aka.ms/webview2</a></p>" \
            "</body></html>";
        webView.goToURL (missingRuntimeHtml);
    }
    else
   #endif
    {
        const auto cacheBust = "?v=" + juce::String (juce::Time::getCurrentTime().toMilliseconds());
        webView.goToURL (juce::WebBrowserComponent::getResourceProviderRoot() + cacheBust);
    }

    setResizable (true, true);
    setResizeLimits (1024, 768, 4096, 3072);
    const auto initialSize = loadStandaloneWindowSize();
    setSize (initialSize.getWidth(), initialSize.getHeight());
    mLastWindowBounds = getBounds();
    saveStandaloneWindowSize();

    // Start periodic idle timer (~60 fps) to match iPlug2's OnIdle() cadence.
    // This drives state broadcasts, DSP performance updates, tuner data, etc.
    startTimerHz (60);
}

PluginEditor::~PluginEditor()
{
    saveStandaloneWindowSize();
    stopTimer();
    processorRef.setWebMessageCallback (nullptr);
}

juce::File PluginEditor::getStandaloneWindowStateFile() const
{
    if (! isMacStandaloneAppExecutable())
        return {};

    const auto userDataPath = processorRef.GetUserDataPath() / "data" / "v1" / "settings" / "ui" / "window-state.json";
    return juce::File (userDataPath.string());
}

juce::Rectangle<int> PluginEditor::loadStandaloneWindowSize() const
{
    constexpr int kDefaultWidth = 1600;
    constexpr int kDefaultHeight = 1200;
    constexpr int kMinWidth = 1024;
    constexpr int kMinHeight = 768;
    constexpr int kMaxWidth = 4096;
    constexpr int kMaxHeight = 3072;

    auto width = kDefaultWidth;
    auto height = kDefaultHeight;

    const auto stateFile = getStandaloneWindowStateFile();
    if (stateFile.existsAsFile())
    {
        const auto parsed = juce::JSON::parse (stateFile.loadFileAsString());
        if (auto* obj = parsed.getDynamicObject(); obj != nullptr)
        {
            const auto widthId = juce::Identifier { "width" };
            const auto heightId = juce::Identifier { "height" };

            if (obj->hasProperty (widthId))
                width = static_cast<int> (obj->getProperty (widthId));

            if (obj->hasProperty (heightId))
                height = static_cast<int> (obj->getProperty (heightId));
        }
    }

    width = juce::jlimit (kMinWidth, kMaxWidth, width);
    height = juce::jlimit (kMinHeight, kMaxHeight, height);
    return { 0, 0, width, height };
}

void PluginEditor::saveStandaloneWindowSize() const
{
    const auto stateFile = getStandaloneWindowStateFile();
    if (stateFile == juce::File{})
        return;

    stateFile.getParentDirectory().createDirectory();

    juce::DynamicObject::Ptr state (new juce::DynamicObject());
    state->setProperty ("width", mLastWindowBounds.getWidth());
    state->setProperty ("height", mLastWindowBounds.getHeight());
    const auto payload = juce::JSON::toString (juce::var (state));
    stateFile.replaceWithText (payload);
}

void PluginEditor::timerCallback()
{
    processorRef.getController().OnIdle();
}

std::optional<juce::WebBrowserComponent::Resource> PluginEditor::getResource (const juce::String& url)
{
    const auto trimmedUrl = url.upToFirstOccurrenceOf ("#", false, false)
                                .upToFirstOccurrenceOf ("?", false, false);
    const auto urlToRetrieve = trimmedUrl == "/" ? juce::String ("index.html")
                                                  : trimmedUrl.fromFirstOccurrenceOf ("/", false, false);

    if (resourceRoot.exists())
    {
        const auto file = resourceRoot.getChildFile ("ui").getChildFile (urlToRetrieve);
        if (file.existsAsFile())
        {
            auto data = readFileToVector (file);
            return juce::WebBrowserComponent::Resource { std::move (data),
                                                         getMimeForExtension (file.getFileExtension().substring (1)) };
        }
    }

    if (urlToRetrieve == "index.html")
    {
        const juce::String fallbackHtml =
            "<!doctype html><html><head><meta charset=\"utf-8\"/>"
            "<title>Soundshed Guitar</title></head>"
            "<body style=\"font-family:sans-serif;background:#101014;color:#eee;padding:24px;\">"
            "<h1>UI not found</h1><p>Expected resources/ui/index.html on disk.</p>"
            "</body></html>";

        std::vector<std::byte> bytes (fallbackHtml.getNumBytesAsUTF8());
        std::memcpy (bytes.data(), fallbackHtml.toRawUTF8(), bytes.size());
        return juce::WebBrowserComponent::Resource { std::move (bytes), "text/html" };
    }

    return std::nullopt;
}

juce::String PluginEditor::getResourceRootUrl() const
{
    return kResourceOrigin;
}

void PluginEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void PluginEditor::resized()
{
    webView.setBounds (getLocalBounds());
    const auto newBounds = getBounds();
    if (newBounds != mLastWindowBounds)
    {
        mLastWindowBounds = newBounds;
        saveStandaloneWindowSize();
    }
}
