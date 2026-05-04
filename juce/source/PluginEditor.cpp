#include "PluginEditor.h"

#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "UiBridge.h"

namespace
{
    const juce::String kResourceOrigin = "http://soundshed.local/";

    bool isYouTubeUrl (const juce::String& url)
    {
        return url.containsIgnoreCase ("youtube-nocookie.com")
               || url.containsIgnoreCase ("youtube.com")
               || url.containsIgnoreCase ("youtu.be");
    }

    bool isScalexUrl (const juce::String& url)
    {
         return url.equalsIgnoreCase ("https://scalex.soundshed.com")
             || url.startsWithIgnoreCase ("https://scalex.soundshed.com/")
             || url.startsWithIgnoreCase ("https://scalex.soundshed.com?");
    }

    juce::WebBrowserComponent::Options::Backend getPreferredBrowserBackend()
    {
#if JUCE_WINDOWS
        return juce::WebBrowserComponent::Options::Backend::webview2;
#else
        return juce::WebBrowserComponent::Options::Backend::defaultBackend;
#endif
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
        if (!stream.openedOk())
            return {};

        std::vector<std::byte> result (static_cast<size_t> (stream.getTotalLength()));
        if (result.empty())
            return result;

        stream.read (result.data(), static_cast<int> (result.size()));
        return result;
    }

    bool isSafeResourcePath (const juce::String& path)
    {
        if (path.isEmpty() || path.startsWithChar ('/') || path.startsWithChar ('\\') || path.contains (":"))
            return false;

        juce::StringArray parts;
        parts.addTokens (path.replaceCharacter ('\\', '/'), "/", "");
        for (const auto& part : parts)
        {
            if (part.isEmpty() || part == "." || part == "..")
                return false;
        }

        return true;
    }

    juce::String getResourcePathFromUrl (juce::String url)
    {
        url = url.upToFirstOccurrenceOf ("#", false, false)
                  .upToFirstOccurrenceOf ("?", false, false);

        if (url.startsWith (kResourceOrigin))
            url = url.substring (kResourceOrigin.length());
        else if (url.startsWithChar ('/'))
            url = url.substring (1);

        return url.isEmpty() ? juce::String ("index.html") : url;
    }

    juce::String extractToneSharingDeepLinkQuery (juce::String commandLine)
    {
        auto normalized = juce::URL::removeEscapeChars (commandLine.unquoted().trim());

        auto extractValue = [&normalized] (const juce::String& key) -> juce::String
        {
            const auto marker = key + "=";
            const auto index = normalized.indexOfIgnoreCase (marker);
            if (index < 0)
                return {};

            auto value = normalized.substring (index + marker.length());
            const auto ampPos = value.indexOfChar ('&');
            if (ampPos >= 0)
                value = value.substring (0, ampPos);
            const auto spacePos = value.indexOfChar (' ');
            if (spacePos >= 0)
                value = value.substring (0, spacePos);
            const auto quotePos = value.indexOfChar ('\"');
            if (quotePos >= 0)
                value = value.substring (0, quotePos);
            const auto hashPos = value.indexOfChar ('#');
            if (hashPos >= 0)
                value = value.substring (0, hashPos);

            return value.trim();
        };

        const auto itemId = extractValue ("itemId");
        const auto packId = extractValue ("packId");

        juce::String sanitized;
        if (itemId.isNotEmpty())
            sanitized << "itemId=" << itemId;
        if (packId.isNotEmpty())
        {
            if (sanitized.isNotEmpty())
                sanitized << "&";
            sanitized << "packId=" << packId;
        }

        return sanitized;
    }

    juce::String getStandaloneDeepLinkQuery()
    {
        return extractToneSharingDeepLinkQuery (juce::JUCEApplicationBase::getCommandLineParameters());
    }

}

bool SinglePageBrowser::pageAboutToLoad (const juce::String& newURL)
{
    if (newURL.startsWith (getResourceProviderRoot()))
        return true;

    // Allow local file navigation when resource-provider mode is unavailable.
    if (newURL.startsWith ("file://") || newURL.startsWith ("about:blank"))
        return true;

    // Allow data: URLs (used by the WebView2-missing error page)
    if (newURL.startsWith ("data:"))
        return true;

    // The Jam view embeds trusted tool/player pages that should remain inside the WebView.
    if (isYouTubeUrl (newURL) || isScalexUrl (newURL))
        return true;

    if (newURL.startsWith ("https://") || newURL.startsWith ("http://"))
    {
        juce::URL (newURL).launchInDefaultBrowser();
        return false;
    }

    return false;
}

void SinglePageBrowser::setPageFinishedCallback (PageFinishedCallback callback)
{
    pageFinishedCallback = std::move (callback);
}

void SinglePageBrowser::setNetworkErrorCallback (NetworkErrorCallback callback)
{
    networkErrorCallback = std::move (callback);
}

void SinglePageBrowser::pageFinishedLoading (const juce::String& url)
{
    if (pageFinishedCallback != nullptr)
        pageFinishedCallback (url);
}

bool SinglePageBrowser::pageLoadHadNetworkError (const juce::String& errorInfo)
{
    if (networkErrorCallback != nullptr)
        return networkErrorCallback (errorInfo);

    return false;
}

void SinglePageBrowser::newWindowAttemptingToLoad (const juce::String& newURL)
{
    // YouTube embeds frequently request popup windows for watch pages, sign-in, and
    // player chrome. Ignore those requests so the iframe remains in-place on macOS.
    if (isYouTubeUrl (newURL))
        return;

    if (newURL.startsWith ("https://") || newURL.startsWith ("http://"))
        juce::URL (newURL).launchInDefaultBrowser();
}

namespace
{
    // Write a line to a persistent startup log file sitting next to the executable.
    // This survives after the process exits and works in release builds where
    // OutputDebugString is the only other option.
    void writeStartupLog (const juce::String& message)
    {
        const auto logDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                                .getChildFile ("Soundshed Guitar")
                                .getChildFile ("logs");
        logDir.createDirectory();
        const auto logFile = logDir.getChildFile ("soundshed-startup.log");
        juce::FileOutputStream stream (logFile);
        if (stream.openedOk())
        {
            stream.setPosition (stream.getFile().getSize()); // append
            const auto line = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M:%S") + "  " + message + "\n";
            stream.writeText (line, false, false, nullptr);
        }

#if !JUCE_LINUX
        juce::Logger::writeToLog (message);
#endif
    }
}

PluginEditor::PluginEditor (PluginProcessorAdapter& p)
    : AudioProcessorEditor (&p),
      processorRef (p),
      resourceRoot ([&p] {
          const auto exeFile = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
          const auto exeDir = std::filesystem::path (exeFile.getParentDirectory().getFullPathName().toStdString());
          const auto cwd = std::filesystem::current_path();
          const auto adapterRoot = p.GetBundledAssetsPath();

          std::vector<std::filesystem::path> candidates;
          if (!adapterRoot.empty())
              candidates.push_back (adapterRoot);
          candidates.push_back (exeDir / "resources");
          candidates.push_back (exeDir / "Resources");
          candidates.push_back (cwd / "resources");
          candidates.push_back (cwd / "Resources");

          writeStartupLog ("[PluginEditor] exe       : " + exeFile.getFullPathName());
          writeStartupLog ("[PluginEditor] cwd       : " + juce::String (cwd.string()));
          writeStartupLog ("[PluginEditor] adapter   : " + juce::String (adapterRoot.string()));

          const auto resolved = guitarfx::ui::ResolveResourceRoot (candidates);
          writeStartupLog ("[PluginEditor] resolved  : " + juce::String (resolved.string()));

          if (resolved.empty())
          {
              writeStartupLog ("[PluginEditor] WARNING: resource root NOT found – UI will show fallback");
          }
          else
          {
              const auto indexPath = resolved / "ui" / "index.html";
              const bool indexExists = std::filesystem::exists (indexPath);
              writeStartupLog ("[PluginEditor] index.html exists: " + juce::String (indexExists ? "YES" : "NO")
                               + " (" + juce::String (indexPath.string()) + ")");
          }

          return juce::File (resolved.string());
      }()),
      webView ([this] {
          auto options = juce::WebBrowserComponent::Options {}
                             .withBackend (getPreferredBrowserBackend())
                             .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2 {}
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
                             .withEventListener ("iplugSendMsg", [this] (const juce::var& payload) {
                                 const auto message = payload.isString() ? payload.toString()
                                                                         : juce::JSON::toString (payload);
                                 if (message.isNotEmpty())
                                     processorRef.handleWebMessage (message);
                             })
                             .withNativeFunction ("IPlugSendMsg", [this] (const juce::Array<juce::var>& args, auto complete) {
                                 if (args.size() > 0)
                                 {
                                     const auto& arg = args[0];
                                     const auto payload = arg.isString() ? arg.toString()
                                                                         : juce::JSON::toString (arg);
                                     processorRef.handleWebMessage (payload);
                                 }

                                 complete (juce::var());
                             })
                             .withNativeFunction ("postMessage", [this] (const juce::Array<juce::var>& args, auto complete) {
                                 if (args.size() > 0)
                                 {
                                     const auto& arg = args[0];
                                     const auto payload = arg.isString() ? arg.toString()
                                                                         : juce::JSON::toString (arg);
                                     processorRef.handleWebMessage (payload);
                                 }

                                 complete (juce::var());
                             });

#if JUCE_WEB_BROWSER_RESOURCE_PROVIDER_AVAILABLE
          options = options.withResourceProvider ([this] (const auto& url) {
              return getResource (url);
          },
              std::optional<juce::String> { getResourceRootUrl() });
#else
          writeStartupLog ("[PluginEditor] Resource provider unavailable for this JUCE configuration; using file:// UI loading");
#endif

          return options;
      }())
{
    addAndMakeVisible (webView);

#if JUCE_LINUX
    webView.setPageFinishedCallback ([this] (const juce::String& url) {
        markLinuxWebViewLoaded (url);
    });

    webView.setNetworkErrorCallback ([this] (const juce::String& errorInfo) {
        return handleLinuxWebViewNetworkError (errorInfo);
    });

    linuxWebViewInitTimeMs = juce::Time::getMillisecondCounter();
    linuxWebViewSlowLoadLogged = false;
    linuxWebViewRetryAttempted = false;
    linuxWebViewNetworkErrorCount = 0;
    linuxWebViewStatusLabel.setJustificationType (juce::Justification::centred);
    linuxWebViewStatusLabel.setColour (juce::Label::backgroundColourId, juce::Colour::fromRGBA (18, 19, 24, 242));
    linuxWebViewStatusLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    linuxWebViewStatusLabel.setVisible (false);

    const auto linuxWebViewSupported = juce::WebBrowserComponent::areOptionsSupported (
        juce::WebBrowserComponent::Options {}
            .withBackend (getPreferredBrowserBackend())
            .withNativeIntegrationEnabled());

    if (!linuxWebViewSupported)
    {
        showLinuxWebViewDependencyMessage ("Embedded WebKit support was not detected. webkit2gtk is required.");
        writeStartupLog ("[PluginEditor] Linux WebView unsupported: missing runtime dependencies");
    }
#endif

    processorRef.setWebMessageCallback ([this] (const juce::String& message) {
        const auto script = guitarfx::ui::BuildIPlugReceiveScript (message.toStdString());
#if JUCE_LINUX
        // JUCE's Linux WebKit backend emits evaluation callbacks even when none
        // were requested; providing a no-op callback avoids assertion noise.
        webView.evaluateJavascript (juce::String (script), [] (const auto&) {});
#else
        webView.evaluateJavascript (juce::String (script));
#endif
    });

#if JUCE_WINDOWS
    // NOTE: areOptionsSupported must be called with a writable user data folder.
    // When installed under Program Files the default folder (next to the .exe) is
    // read-only for standard users, causing the check to falsely return false.
    const auto webView2Supported = juce::WebBrowserComponent::areOptionsSupported (
        juce::WebBrowserComponent::Options {}
            .withBackend (getPreferredBrowserBackend())
            .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2 {}
                    .withUserDataFolder (
                        juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("SoundshedGuitarWebView2Check"))));

    writeStartupLog ("[PluginEditor] WebView2 supported: " + juce::String (webView2Supported ? "YES" : "NO"));

    if (!webView2Supported)
    {
        const juce::String missingRuntimeHtml =
            "data:text/html;charset=UTF-8,"
            "<!doctype html><html><head><meta charset='utf-8'/>"
            "<title>Soundshed Guitar</title></head>"
            "<body style='font-family:sans-serif;background:#101014;color:#eee;padding:24px;line-height:1.5;'>"
            "<h2>Microsoft Edge WebView2 Runtime is required</h2>"
            "<p>The Soundshed Guitar UI cannot start because WebView2 is not available.</p>"
            "<p>Install the runtime, then restart the app.</p>"
            "<p>Download: <a href='https://aka.ms/webview2' style='color:#8ab4ff;'>https://aka.ms/webview2</a></p>"
            "</body></html>";
        webView.goToURL (missingRuntimeHtml);
    }
    else
#endif
    {
        auto cacheBust = "?v=" + juce::String (juce::Time::getCurrentTime().toMilliseconds());
        const auto deepLinkQuery = getStandaloneDeepLinkQuery();
        if (deepLinkQuery.isNotEmpty())
            cacheBust << "&" << deepLinkQuery;
        juce::String startUrl;

#if JUCE_WEB_BROWSER_RESOURCE_PROVIDER_AVAILABLE
        startUrl = juce::WebBrowserComponent::getResourceProviderRoot() + cacheBust;
#else
        const auto indexFile = resourceRoot.getChildFile ("ui").getChildFile ("index.html");
        if (indexFile.existsAsFile())
            startUrl = juce::URL (indexFile).toString (false) + cacheBust;
        else
            startUrl = "data:text/html;charset=UTF-8,<html><body><h2>UI not found</h2></body></html>";
#endif

        writeStartupLog ("[PluginEditor] goToURL: " + startUrl);

#if JUCE_LINUX
        if (!linuxWebViewFallbackShown)
        {
            linuxInitialUrl = startUrl;
            linuxWebViewInitTimeMs = juce::Time::getMillisecondCounter();
            linuxWebViewSlowLoadLogged = false;
            linuxWebViewRetryAttempted = false;
            linuxWebViewNetworkErrorCount = 0;
            webView.goToURL (startUrl);
        }
#else
        webView.goToURL (startUrl);
#endif
    }

    setResizable (true, true);
    setResizeLimits (800, 600, 8192, 8192);
    setSize (1200, 900);

    // Start periodic idle timer (~60 fps) to match iPlug2's OnIdle() cadence.
    // This drives state broadcasts, DSP performance updates, tuner data, etc.
    startTimerHz (60);
}

PluginEditor::~PluginEditor()
{
    stopTimer();
    processorRef.setWebMessageCallback (nullptr);
}

void PluginEditor::timerCallback()
{
    processorRef.getController().OnIdle();

#if JUCE_LINUX
    if (!linuxWebViewLoadCompleted && !linuxWebViewFallbackShown)
    {
        constexpr juce::uint32 loadWarningMs = 8000;
        constexpr juce::uint32 loadRetryMs = 12000;
        constexpr juce::uint32 loadTimeoutMs = 25000;
        const auto elapsedMs = juce::Time::getMillisecondCounter() - linuxWebViewInitTimeMs;

        if (!linuxWebViewSlowLoadLogged && elapsedMs > loadWarningMs)
        {
            linuxWebViewSlowLoadLogged = true;
            writeStartupLog ("[PluginEditor] Linux WebView still loading after 8s");
        }

        if (!linuxWebViewRetryAttempted && elapsedMs > loadRetryMs && linuxInitialUrl.isNotEmpty())
        {
            linuxWebViewRetryAttempted = true;
            linuxWebViewInitTimeMs = juce::Time::getMillisecondCounter();
            linuxWebViewNetworkErrorCount = 0;
            writeStartupLog ("[PluginEditor] Linux WebView load retry after slow startup");
            webView.goToURL (linuxInitialUrl);
            return;
        }

        if (elapsedMs > loadTimeoutMs)
            showLinuxWebViewDependencyMessage ("Timed out while waiting for the embedded WebView to finish loading.");
    }
#endif
}

std::optional<juce::WebBrowserComponent::Resource> PluginEditor::getResource (const juce::String& url)
{
    const auto urlToRetrieve = getResourcePathFromUrl (url);

    if (!isSafeResourcePath (urlToRetrieve))
    {
        writeStartupLog ("[getResource] rejected unsafe path: " + urlToRetrieve);
        return std::nullopt;
    }

    if (resourceRoot.exists())
    {
        const auto file = resourceRoot.getChildFile ("ui").getChildFile (urlToRetrieve);
        if (file.existsAsFile())
        {
            auto data = readFileToVector (file);

            return juce::WebBrowserComponent::Resource { std::move (data),
                getMimeForExtension (file.getFileExtension().substring (1)) };
        }

        writeStartupLog ("[getResource] MISS " + resourceRoot.getChildFile ("ui").getChildFile (urlToRetrieve).getFullPathName());
    }
    else
    {
        writeStartupLog ("[getResource] resourceRoot invalid (" + resourceRoot.getFullPathName() + ") for: " + urlToRetrieve);
    }

    if (urlToRetrieve == "index.html")
    {
        writeStartupLog ("[getResource] serving fallback index.html (resources not found)");
        const juce::String fallbackHtml =
            "<!doctype html><html><head><meta charset=\"utf-8\"/>"
            "<title>Soundshed Guitar</title></head>"
            "<body style=\"font-family:sans-serif;background:#101014;color:#eee;padding:24px;\">"
            "<h1>UI not found</h1><p>Expected resources/ui/index.html on disk.</p>"
            "<p>Resource root: "
            + resourceRoot.getFullPathName() + "</p>"
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

#if JUCE_LINUX
void PluginEditor::markLinuxWebViewLoaded (const juce::String& url)
{
    if (!linuxWebViewLoadCompleted)
        writeStartupLog ("[PluginEditor] Linux WebView loaded: " + url);

    linuxWebViewLoadCompleted = true;
    linuxWebViewNetworkErrorCount = 0;

    if (linuxWebViewFallbackShown)
    {
        linuxWebViewFallbackShown = false;
        linuxWebViewStatusLabel.setVisible (false);
    }
}

bool PluginEditor::handleLinuxWebViewNetworkError (const juce::String& errorInfo)
{
    writeStartupLog ("[PluginEditor] Linux WebView network error: " + errorInfo);

    if (linuxWebViewLoadCompleted || linuxWebViewFallbackShown)
        return false;

    ++linuxWebViewNetworkErrorCount;

    const bool missingResourceError = errorInfo.containsIgnoreCase ("ERR_FILE_NOT_FOUND")
                                      || errorInfo.containsIgnoreCase ("FILE_DOES_NOT_EXIST")
                                      || errorInfo.containsIgnoreCase ("404");

    if (missingResourceError || linuxWebViewNetworkErrorCount >= 3)
        showLinuxWebViewDependencyMessage ("Network/WebView error: " + errorInfo);

    return false;
}

void PluginEditor::showLinuxWebViewDependencyMessage (const juce::String& reason)
{
    if (!linuxWebViewFallbackShown)
    {
        writeStartupLog ("[PluginEditor] Linux WebView fallback shown: " + reason);
        linuxWebViewFallbackShown = true;
    }

    const juce::String message =
        "The Soundshed Guitar UI could not be displayed in the embedded browser on Linux.\n\n"
        "Please confirm the required app dependencies are installed.\n"
        "See guitar.soundshed.com for details.\n\n"
        "Details: "
        + reason;

    linuxWebViewStatusLabel.setText (message, juce::dontSendNotification);

    if (linuxWebViewStatusLabel.getParentComponent() == nullptr)
        addAndMakeVisible (linuxWebViewStatusLabel);
    else
        linuxWebViewStatusLabel.setVisible (true);

    linuxWebViewStatusLabel.toFront (false);
    resized();
}
#endif

void PluginEditor::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void PluginEditor::resized()
{
    webView.setBounds (getLocalBounds());

#if JUCE_LINUX
    linuxWebViewStatusLabel.setBounds (getLocalBounds().reduced (24));
#endif
}

void PluginEditor::handleDeepLinkFromAnotherInstance (const juce::String& deepLinkQuery)
{
    // Route incoming deep link to UI via postMessage
    // The UI handler will navigate to the appropriate tone sharing item/pack
    if (deepLinkQuery.isEmpty())
        return;

    nlohmann::json msg;
    msg["type"] = "navigateToToneSharingDeepLink";
    msg["deepLink"] = deepLinkQuery.toStdString();

    const auto jsonStr = msg.dump();
    processorRef.SendMessageToUI (jsonStr);

    // Also bring the window to focus
    if (auto* window = getTopLevelComponent())
        window->toFront (true);
}
