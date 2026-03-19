#include "PluginEditor.h"

#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <vector>

#include "UiBridge.h"

namespace
{
    const juce::String kResourceOrigin = "http://soundshed.local/";

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
        if (! stream.openedOk())
            return {};

        std::vector<std::byte> result (static_cast<size_t> (stream.getTotalLength()));
        if (result.empty())
            return result;

        stream.read (result.data(), static_cast<int> (result.size()));
        return result;
    }

    juce::String extractToneSharingDeepLinkQuery (juce::String commandLine)
    {
        const auto marker = juce::String ("soundshed://tone-sharing?");
        const auto markerIndex = commandLine.indexOfIgnoreCase (marker);
        if (markerIndex < 0)
            return {};

        auto query = commandLine.substring (markerIndex + marker.length());

        // Strip wrapping quotes and trailing args.
        query = query.unquoted().trim();
        const auto firstSpace = query.indexOfChar (' ');
        if (firstSpace >= 0)
            query = query.substring (0, firstSpace);

        const auto hashPos = query.indexOfChar ('#');
        if (hashPos >= 0)
            query = query.substring (0, hashPos);

        if (query.isEmpty())
            return {};

        // Keep only supported target keys.
        juce::StringArray parts;
        parts.addTokens (query, "&", "");

        juce::String sanitized;
        for (const auto& part : parts)
        {
            const auto trimmed = part.trim();
            if (trimmed.startsWithIgnoreCase ("itemId=") || trimmed.startsWithIgnoreCase ("packId="))
            {
                if (sanitized.isNotEmpty())
                    sanitized << "&";
                sanitized << trimmed;
            }
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

    // Allow data: URLs (used by the WebView2-missing error page)
    if (newURL.startsWith ("data:"))
        return true;

    if (newURL.startsWith ("https://") || newURL.startsWith ("http://"))
    {
        juce::URL (newURL).launchInDefaultBrowser();
        return false;
    }

    return false;
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
        juce::Logger::writeToLog (message);
    }
}

PluginEditor::PluginEditor (PluginProcessorAdapter& p)
    : AudioProcessorEditor (&p),
      processorRef (p),
      resourceRoot ([]{
        // --- Resource root resolution logging ---
        const auto exeFile = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
        const auto exeDir  = std::filesystem::path (exeFile.getParentDirectory().getFullPathName().toStdString());
        const auto cwd     = std::filesystem::current_path();

        writeStartupLog ("[PluginEditor] exe       : " + exeFile.getFullPathName());
        writeStartupLog ("[PluginEditor] cwd       : " + juce::String (cwd.string()));
        writeStartupLog ("[PluginEditor] candidate : " + juce::String ((exeDir / "resources").string()));

        const auto resolved = guitarfx::ui::ResolveResourceRoot ({ exeDir / "resources" });
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
      webView ([this]
      {
          auto options = juce::WebBrowserComponent::Options{}
                             .withBackend (getPreferredBrowserBackend())
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
                             });

         #if JUCE_WEB_BROWSER_RESOURCE_PROVIDER_AVAILABLE
          options = options.withResourceProvider ([this] (const auto& url)
                                                  {
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

    processorRef.setWebMessageCallback ([this] (const juce::String& message)
    {
        const auto script = guitarfx::ui::BuildIPlugReceiveScript (message.toStdString());
        webView.evaluateJavascript (juce::String (script));
    });

   #if JUCE_WINDOWS
    // NOTE: areOptionsSupported must be called with a writable user data folder.
    // When installed under Program Files the default folder (next to the .exe) is
    // read-only for standard users, causing the check to falsely return false.
    const auto webView2Supported = juce::WebBrowserComponent::areOptionsSupported (
        juce::WebBrowserComponent::Options{}
            .withBackend (getPreferredBrowserBackend())
            .withWinWebView2Options (juce::WebBrowserComponent::Options::WinWebView2{}
                .withUserDataFolder (
                    juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("SoundshedGuitarWebView2Check"))));

    writeStartupLog ("[PluginEditor] WebView2 supported: " + juce::String (webView2Supported ? "YES" : "NO"));

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
            startUrl = indexFile.getFullPathName() + cacheBust;
        else
            startUrl = "data:text/html;charset=UTF-8,<html><body><h2>UI not found</h2></body></html>";
       #endif

        writeStartupLog ("[PluginEditor] goToURL: " + startUrl);
        webView.goToURL (startUrl);
    }

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
            // Only log first-time requests (index + JS modules) to avoid flooding
            if (urlToRetrieve == "index.html" || file.getFileExtension() == ".js")
                writeStartupLog ("[getResource] HIT  " + file.getFullPathName());
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
            "<p>Resource root: " + resourceRoot.getFullPathName() + "</p>"
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
}
