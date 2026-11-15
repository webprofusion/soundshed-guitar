#pragma once

// ResourceFolderScanner — listing a folder of NAM captures and IRs for the
// resource browser, without ever blocking the message thread.
//
// Enumerating a folder can stall for seconds on a network share or a sleeping
// drive, and parsing each .nam/.wav for metadata is slower still, so none of it
// runs on the message thread. The request handler does only two cheap things —
// snapshot the library's (filePath, id) index and spawn a worker — and returns.
//
// A generation counter supersedes in-flight scans: every request bumps it, and
// a worker that observes a newer generation drops its results and returns. That
// is what keeps a user clicking quickly through folders from racing three
// listings onto the screen in the wrong order.
//
// Workers are detached rather than joined, because joining would reintroduce
// the message-thread stall the design exists to avoid. Teardown therefore has
// to wait for them explicitly: Shutdown() supersedes every outstanding scan and
// blocks until the last one has left, so no worker can outlive the callback it
// publishes through. It is idempotent, and the destructor calls it.
//
// The scan reports in two phases, so the listing appears before the expensive
// part starts: "resourceFolderListing" carries the entries with
// metadataPending set, then "resourceFolderMetadata" streams the parsed
// metadata back in batches. Failures at any point become
// "resourceFolderListingFailed".
//
// "hashResourceFile" works out one listed file's content hash (models/ContentHash.h)
// on a worker of its own, so the details panel can look the file up on Tone3000
// without the listing paying to hash every file. It never supersedes a scan, but
// Shutdown() waits for it like one.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx
{
class MessageHandlerRegistry;
class ResourceLibrary;

class ResourceFolderScanner
{
  public:
    using SendMessageFn = std::function<void(const std::string&)>;

    ResourceFolderScanner(SendMessageFn sendMessage, ResourceLibrary& resourceLibrary);
    ~ResourceFolderScanner();

    ResourceFolderScanner(const ResourceFolderScanner&) = delete;
    ResourceFolderScanner& operator=(const ResourceFolderScanner&) = delete;

    /// Registers "listResourceFolder" and "hashResourceFile" with the dispatcher.
    void RegisterMessageHandlers(MessageHandlerRegistry& registry);

    /**
     * Supersedes every in-flight scan and waits until the last worker has
     * finished. Idempotent, so calling it before the destructor — which owners
     * should do, to pin the teardown point relative to whatever the send
     * callback reaches into — costs nothing the second time.
     */
    void Shutdown();

  private:
    /// Message thread: snapshots the library path index and spawns a worker. Does no
    /// filesystem work of its own, so it cannot stall however slow the drive is.
    void HandleListResourceFolderRequest(const nlohmann::json& payload);

    /// Worker thread: validates the path, enumerates the folder, then parses metadata,
    /// bailing out at each step if a newer request has superseded this generation.
    void ScanWorker(std::string requestPath, std::vector<std::pair<std::string, std::string>> libraryPaths,
                    std::uint64_t generation);

    /// Message thread: spawns a worker that answers "resourceFileContentHash" for one file.
    void HandleHashResourceFileRequest(const nlohmann::json& payload);

    /// Drops one worker from the active count and wakes anyone waiting in Shutdown().
    void ReleaseWorker();

    SendMessageFn mSendMessage;
    ResourceLibrary& mResourceLibrary;

    std::atomic<std::uint64_t> mScanGeneration{0};
    std::atomic<int> mActiveScans{0};
    std::mutex mScanDoneMutex;
    std::condition_variable mScanDoneCv;
};
} // namespace guitarfx
