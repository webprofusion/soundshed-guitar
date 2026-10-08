#include "UiResourcePrefetch.h"

#include <juce_events/juce_events.h>

#include <map>
#include <mutex>

namespace soundshed::editor
{
namespace
{
    // What a page load asks for, in about the order it asks: the page, its stylesheets and
    // font, the module graph, then the data files and icons the first render pulls in.
    // Anything else (equipment photos, demo audio, presets) comes later or not at all.
    struct PrefetchRule
    {
        const char* folder;
        const char* wildcard;
        bool recursive;
    };

    constexpr PrefetchRule kRules[] {
        { "", "index.html;build-flags.js", false },
        { "css", "*.css;*.ttf;*.woff2", true },
        { "dist", "*.js", true },
        { "data", "*.json", false },
        { "images/icons", "*.svg", false },
        { "images", "icon.png", false },
    };

    // Nothing for a file that could not be read whole: the handler then reads it itself.
    std::optional<std::vector<std::byte>> readWholeFile (const juce::File& file)
    {
        juce::FileInputStream stream (file);

        if (! stream.openedOk())
            return std::nullopt;

        std::vector<std::byte> bytes (static_cast<size_t> (stream.getTotalLength()));

        if (! bytes.empty() && stream.read (bytes.data(), static_cast<int> (bytes.size())) != static_cast<int> (bytes.size()))
            return std::nullopt;

        return bytes;
    }

    // A thread JUCE deletes at shutdown rather than a detached one: in a plugin, JUCE's
    // singletons go before the host unloads the DLL, and this stops reading before then.
    class Prefetcher final : public juce::DeletedAtShutdown, private juce::Thread
    {
    public:
        Prefetcher() : juce::Thread ("UI resource prefetch") {}

        ~Prefetcher() override
        {
            stopThread (-1);
            clearSingletonInstance();
        }

        JUCE_DECLARE_SINGLETON_INLINE (Prefetcher, true)

        // start() and release() run on the message thread only; the lock is for the files.
        void start (const juce::File& folder)
        {
            if (! folder.isDirectory())
                return;

            if (folder == uiFolder && (isThreadRunning() || hasFiles()))
                return;

            stopThread (-1);

            {
                const std::scoped_lock lock (mutex);
                files.clear();
            }

            uiFolder = folder;
            startThread();
        }

        std::optional<std::vector<std::byte>> take (const juce::File& file)
        {
            const std::scoped_lock lock (mutex);
            const auto found = files.find (file.getFullPathName());

            if (found == files.end())
                return std::nullopt;

            auto bytes = std::move (found->second);
            files.erase (found);
            return bytes;
        }

        void release()
        {
            // Not waited for: the reader checks this before it keeps another file.
            signalThreadShouldExit();

            const std::scoped_lock lock (mutex);
            files.clear();
        }

    private:
        bool hasFiles()
        {
            const std::scoped_lock lock (mutex);
            return ! files.empty();
        }

        void run() override
        {
            for (const auto& rule : kRules)
            {
                const auto folder = juce::String (rule.folder).isEmpty() ? uiFolder : uiFolder.getChildFile (rule.folder);

                for (const auto& file : folder.findChildFiles (juce::File::findFiles, rule.recursive, rule.wildcard))
                {
                    if (threadShouldExit())
                        return;

                    auto bytes = readWholeFile (file);

                    if (! bytes.has_value())
                        continue;

                    const std::scoped_lock lock (mutex);

                    if (threadShouldExit())
                        return;

                    files.emplace (file.getFullPathName(), std::move (*bytes));
                }
            }
        }

        juce::File uiFolder;
        std::mutex mutex;
        std::map<juce::String, std::vector<std::byte>> files;
    };
} // namespace

void prefetchUiResources (const juce::File& uiFolder)
{
    Prefetcher::getInstance()->start (uiFolder);
}

std::optional<std::vector<std::byte>> takePrefetchedUiResource (const juce::File& file)
{
    if (auto* prefetcher = Prefetcher::getInstanceWithoutCreating())
        return prefetcher->take (file);

    return std::nullopt;
}

void releasePrefetchedUiResources()
{
    if (auto* prefetcher = Prefetcher::getInstanceWithoutCreating())
        prefetcher->release();
}
} // namespace soundshed::editor
