/**
 * ResourceFolderScanner.cpp — the resource browser's folder listing.
 *
 * Everything here past HandleListResourceFolderRequest runs on a detached
 * worker thread. See ResourceFolderScanner.h for why the work is off the
 * message thread, how a newer request supersedes an older one, and what
 * Shutdown() guarantees about worker lifetimes.
 */

#include "controller/ResourceFolderScanner.h"

#include "MessageHandlerRegistry.h"
#include "controller/internal/NamResourceMetadata.h"
#include "resources/ResourceLibrary.h"
#include "util/PathEncoding.h"
#include "util/Wav.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <system_error>
#include <thread>

using namespace guitarfx::controller_detail;

namespace guitarfx
{
namespace
{
/// Serialises a listing or metadata batch. Its strings should all be UTF-8 already, but
/// they come from what is on disk, and one that is not must cost a U+FFFD, not the scan:
/// plain dump() throws, and the whole folder would be reported as unreadable.
std::string DumpForUi(const nlohmann::json& message)
{
    return message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}
} // namespace

ResourceFolderScanner::ResourceFolderScanner(SendMessageFn sendMessage, ResourceLibrary& resourceLibrary)
    : mSendMessage(std::move(sendMessage)), mResourceLibrary(resourceLibrary)
{
}

ResourceFolderScanner::~ResourceFolderScanner()
{
    Shutdown();
}

void ResourceFolderScanner::RegisterMessageHandlers(MessageHandlerRegistry& registry)
{
    registry.Register("listResourceFolder",
                      [this](const nlohmann::json& payload) { HandleListResourceFolderRequest(payload); });
}

void ResourceFolderScanner::Shutdown()
{
    // Bump first: workers check the generation at every step, so superseding them
    // before waiting is what bounds how long the wait can last.
    mScanGeneration.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock<std::mutex> lock(mScanDoneMutex);
    mScanDoneCv.wait(lock, [this]() { return mActiveScans.load(std::memory_order_relaxed) == 0; });
}

void ResourceFolderScanner::ReleaseWorker()
{
    // Notified under the lock: once the count reaches 0, Shutdown() may return and the
    // scanner be destroyed, so a detached worker must not touch the condition variable after
    // it has let go of the mutex.
    const std::lock_guard<std::mutex> lock(mScanDoneMutex);
    mActiveScans.fetch_sub(1, std::memory_order_relaxed);
    mScanDoneCv.notify_all();
}

void ResourceFolderScanner::HandleListResourceFolderRequest(const nlohmann::json& payload)
{
    const std::string rawPath = payload.value("path", "");

    // Snapshot existing library (filePath -> id) on the message thread. This
    // uses the lightweight path index (two string copies per entry, no metadata
    // maps, no filesystem access, no canonicalization) so even a very large
    // library can never freeze the UI. The worker normalizes/matches off-thread.
    std::vector<std::pair<std::string, std::string>> libraryPaths = mResourceLibrary.GetResourcePathIndex();

    // Supersede any in-flight scan, then spawn a detached worker. We never join
    // on the message thread (which could block on a slow filesystem); detached
    // workers observe the bumped generation and exit quickly, and Shutdown()
    // waits for all of them via mScanDoneCv.
    const std::uint64_t generation = mScanGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    mActiveScans.fetch_add(1, std::memory_order_relaxed);

    try
    {
        std::thread worker([this, rawPath, libraryPaths = std::move(libraryPaths), generation]() mutable {
            const std::string requestedPath = rawPath;
            try
            {
                ScanWorker(std::move(rawPath), std::move(libraryPaths), generation);
            }
            catch (const std::exception& exception)
            {
                if (mScanGeneration.load(std::memory_order_relaxed) == generation)
                {
                    mSendMessage(nlohmann::json{{"type", "resourceFolderListingFailed"},
                                                {"path", requestedPath},
                                                {"message", std::string{"Unable to scan folder: "} + exception.what()}}
                                     .dump());
                }
            }
            catch (...)
            {
                if (mScanGeneration.load(std::memory_order_relaxed) == generation)
                {
                    mSendMessage(nlohmann::json{{"type", "resourceFolderListingFailed"},
                                                {"path", requestedPath},
                                                {"message", "Unable to scan folder"}}
                                     .dump());
                }
            }
            ReleaseWorker();
        });
        worker.detach();
    }
    catch (const std::exception&)
    {
        // Spawning failed: undo the active-scan bump and report the error so the
        // UI doesn't sit on "Loading…" forever. Never let the exception escape
        // into the WebView native-function callback (which would skip its
        // completion handler and can wedge the message pump).
        ReleaseWorker();
        mSendMessage(nlohmann::json{
            {"type", "resourceFolderListingFailed"}, {"path", rawPath}, {"message", "Unable to start folder scan"}}
                         .dump());
    }
}

void ResourceFolderScanner::ScanWorker(std::string requestPath,
                                       std::vector<std::pair<std::string, std::string>> libraryPaths,
                                       std::uint64_t generation)
{
    const auto superseded = [this, generation]() {
        return mScanGeneration.load(std::memory_order_relaxed) != generation;
    };

    // Pure-lexical, no-filesystem normalization: lowercase + forward slashes +
    // collapse of "."/".."/redundant separators. Unlike weakly_canonical this
    // never touches the disk, so it can't stall on a slow/disconnected drive.
    const auto normalizePath = [](const std::filesystem::path& p) -> std::string {
        std::string s = util::PathToUtf8(p.lexically_normal());

        if (!s.empty() && s.back() == '/')
        {
            s.pop_back();
        }

        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    };

    const auto classify = [](const std::filesystem::path& p) -> std::string {
        // Not extension().string(): that converts to the ANSI code page and throws for a
        // name it cannot hold, and "Notes v1.2 <Hebrew>" has the extension ".2 <Hebrew>".
        std::string ext = util::PathToUtf8(p.extension());
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (ext == ".nam")
        {
            return std::string{"nam"};
        }

        if (ext == ".wav" || ext == ".ir" || ext == ".aif" || ext == ".aiff" || ext == ".flac")
        {
            return std::string{"ir"};
        }

        return std::string{};
    };

    // Validate the requested path here (off the message thread) so even a slow
    // exists()/is_directory() probe on a bad drive never freezes the UI.
    if (requestPath.empty())
    {
        if (!superseded())
        {
            mSendMessage(
                nlohmann::json{{"type", "resourceFolderListingFailed"}, {"message", "Missing folder path"}}.dump());
        }

        return;
    }

    const std::filesystem::path dir = util::PathFromUtf8(requestPath);
    std::error_code dec;

    if (!std::filesystem::is_directory(dir, dec) || dec)
    {
        if (!superseded())
        {
            mSendMessage(nlohmann::json{
                {"type", "resourceFolderListingFailed"}, {"path", requestPath}, {"message", "Folder not found"}}
                             .dump());
        }

        return;
    }

    // Build the (normalized path -> library id) lookup off-thread from the cheap
    // snapshot captured on the message thread.
    std::map<std::string, std::string> libraryIdByPath;

    for (auto& entry : libraryPaths)
    {
        if (superseded())
        {
            return;
        }

        libraryIdByPath.emplace(normalizePath(std::filesystem::path(entry.first)), std::move(entry.second));
    }

    std::error_code ec;
    constexpr std::size_t kMaxEntries = 5000;
    std::vector<nlohmann::json> dirs;
    std::vector<nlohmann::json> files;
    bool truncated = false;

    // Parallel list of (filesystem path, resourceType) for the second (metadata)
    // pass. Kept separate so the cheap listing can be sent before any file is
    // opened and parsed.
    struct PendingFile
    {
        std::filesystem::path path;
        std::string resourceType;
    };

    std::vector<PendingFile> pendingMetadata;

    // ── Phase 1: enumerate the immediate level only (no file content reads) ──
    // directory_iterator is intentionally non-recursive: we list just the folder
    // the user navigated into. This is cheap even for large folders, so the UI
    // gets a populated listing almost immediately.
    std::filesystem::directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);

    if (ec)
    {
        const std::string detail = ec.message();

        if (!superseded())
        {
            mSendMessage(nlohmann::json{
                {"type", "resourceFolderListingFailed"},
                {"path", requestPath},
                {"message", detail.empty() ? "Unable to read folder" : "Unable to read folder: " + detail}}
                             .dump());
        }

        return;
    }

    for (; it != std::filesystem::directory_iterator(); it.increment(ec))
    {
        if (ec)
        {
            break; // Stop on iteration error rather than throwing on a detached thread.
        }

        if (superseded())
        {
            return;
        }

        const auto& entry = *it;

        if (dirs.size() + files.size() >= kMaxEntries)
        {
            truncated = true;
            break;
        }

        std::error_code eec;
        const auto& entryPath = entry.path();

        if (entry.is_directory(eec) && !eec)
        {
            dirs.push_back(nlohmann::json{{"name", util::PathToUtf8(entryPath.filename())},
                                          {"path", util::PathToUtf8(entryPath)}});
            continue;
        }

        if (!entry.is_regular_file(eec) || eec)
        {
            continue;
        }

        const std::string resourceType = classify(entryPath);

        if (resourceType.empty())
        {
            continue;
        }

        nlohmann::json file;
        file["name"] = util::PathToUtf8(entryPath.filename());
        file["path"] = util::PathToUtf8(entryPath);
        file["resourceType"] = resourceType;

        std::error_code sec;
        const auto sizeBytes = std::filesystem::file_size(entryPath, sec);
        file["sizeBytes"] = sec ? 0 : static_cast<std::uint64_t>(sizeBytes);

        const auto libIt = libraryIdByPath.find(normalizePath(entryPath));

        if (libIt != libraryIdByPath.end())
        {
            file["alreadyInLibrary"] = true;
            file["libraryId"] = libIt->second;
        }
        else
        {
            file["alreadyInLibrary"] = false;
        }

        // Metadata is filled in later (Phase 2) so the listing isn't blocked.
        file["metadata"] = nlohmann::json::object();
        file["metadataPending"] = true;
        files.push_back(std::move(file));
        pendingMetadata.push_back({entryPath, resourceType});
    }

    if (superseded())
    {
        return;
    }

    const auto byName = [](const nlohmann::json& a, const nlohmann::json& b) {
        std::string an = a.value("name", "");
        std::string bn = b.value("name", "");
        std::transform(an.begin(), an.end(), an.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::transform(bn.begin(), bn.end(), bn.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return an < bn;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);

    const auto parentPath = dir.parent_path();
    std::string parentStr;

    if (!parentPath.empty() && parentPath != dir)
    {
        parentStr = util::PathToUtf8(parentPath);
    }

    const std::string folderPath = util::PathToUtf8(dir);

    nlohmann::json msg;
    msg["type"] = "resourceFolderListing";
    msg["path"] = folderPath;
    msg["parent"] = parentStr;
    const auto leaf = dir.filename();
    msg["name"] = leaf.empty() ? folderPath : util::PathToUtf8(leaf);
    msg["dirs"] = dirs;
    msg["files"] = files;
    msg["truncated"] = truncated;
    msg["metadataPending"] = !pendingMetadata.empty();

    if (superseded())
    {
        return;
    }

    mSendMessage(DumpForUi(msg));

    // ── Phase 2: parse per-file metadata and stream it back in batches ──
    // This is the expensive part (each file is opened/parsed). It runs after the
    // listing is already on screen, so badges/details fill in progressively
    // without ever blocking the UI.
    constexpr std::size_t kMetadataBatchSize = 40;
    nlohmann::json batch = nlohmann::json::array();

    const auto flushBatch = [&]() {
        if (batch.empty())
        {
            return true;
        }

        if (superseded())
        {
            return false;
        }

        mSendMessage(
            DumpForUi(nlohmann::json{{"type", "resourceFolderMetadata"}, {"path", folderPath}, {"items", batch}}));
        batch = nlohmann::json::array();
        return true;
    };

    for (const auto& pending : pendingMetadata)
    {
        if (superseded())
        {
            return;
        }

        nlohmann::json metadata = nlohmann::json::object();

        if (pending.resourceType == "nam")
        {
            LibraryResource temp;
            EnrichNamResourceMetadata(temp, pending.path);

            for (const auto& [key, value] : temp.metadata)
            {
                if (!value.empty())
                {
                    metadata[key] = value;
                }
            }
        }
        else
        {
            const util::WavHeaderInfo wav = util::ProbeWavHeader(pending.path);

            if (wav.valid)
            {
                if (wav.sampleRate > 0)
                {
                    metadata["sampleRate"] = std::to_string(wav.sampleRate);
                }

                if (wav.channels > 0)
                {
                    metadata["channels"] = std::to_string(wav.channels);
                }

                if (wav.bitsPerSample > 0)
                {
                    metadata["bitsPerSample"] = std::to_string(wav.bitsPerSample);
                }

                if (wav.durationSec > 0.0)
                {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "%.3f", wav.durationSec);
                    metadata["durationSec"] = std::string(buf);
                }
            }
        }

        batch.push_back(nlohmann::json{{"path", util::PathToUtf8(pending.path)}, {"metadata", std::move(metadata)}});

        if (batch.size() >= kMetadataBatchSize)
        {
            if (!flushBatch())
            {
                return;
            }
        }
    }

    flushBatch();
}
} // namespace guitarfx
