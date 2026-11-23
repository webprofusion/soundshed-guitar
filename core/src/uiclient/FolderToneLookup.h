#pragma once

/**
 * FolderToneLookup.h - The resource browser's Folder tab as the native UIs see its tones: which
 * published tone a file on disk comes from, found by its content hash (models/ContentHash.h), and
 * the files of one tone drawn as one group, as the Library groups a pack.
 *
 * - FolderToneLookup is "Look up details": the first kBatch listed files that still need it,
 *   one after another. Each is hashed by the engine (hashResourceFile) unless its hash is known,
 *   looked up through Soundshed's relay (models/lookup), and its tone read (tones/<id>), a tone
 *   read once a batch. What is found is kept for the session by path; a file in the library has
 *   it written onto its entry too (updateLibraryResource). A file looked up, found or not, is not
 *   asked about again this session.
 * - GroupFolderFiles gathers files sharing a tone at the first one's place; a tone with one file
 *   here stays a plain row, and a file a later lookup matches joins the group already shown.
 *
 * The web UI's equivalent is core/ui/ts/resourceBrowser/folderTone3000.ts and folderGroups.ts.
 * UI thread only.
 */

#include "uiclient/ClientState.h"
#include "uiclient/Tone3000.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace guitarfx::uiclient
{
class ToneService;
class UiClient;
class Subscription;

using FolderFile = ResourceFolderListing::File;

/// The key a file's path is kept under: forward slashes, lower case.
[[nodiscard]] std::string FolderPathKey(const std::string& path);

/// Files in one folder that came from the same tone.
struct FolderToneGroup
{
    std::string key;         // "tone:<id>"
    std::string title;       // the tone's title when a member's details say it, else empty
    nlohmann::json metadata; // the tone's details, each field from whichever member has it
    std::vector<const FolderFile*> files;
};

struct FolderToneGroups
{
    static constexpr std::size_t kNoGroup = static_cast<std::size_t>(-1);

    struct Item
    {
        const FolderFile* file = nullptr; // a plain row
        std::size_t group = kNoGroup;     // else a group's header (and its files)
    };

    std::vector<FolderToneGroup> groups;
    std::vector<Item> items; // in list order
};

/// `files` in order, those whose details name the same tone (`toneId`) gathered at the first one's
/// place. `detailsFor` gives a file's details, or null.
[[nodiscard]] FolderToneGroups GroupFolderFiles(
    const std::vector<const FolderFile*>& files,
    const std::function<const nlohmann::json*(const FolderFile&)>& detailsFor);

/// A tone's title from its models' library names when none was saved: an import names a model
/// "<tone> - <model>", so it is what the names share up to their last " - ". Empty when fewer than
/// two names are given or they share no such part.
[[nodiscard]] std::string SharedTonePrefix(const std::vector<std::string>& names);

/// Who made the tone, as its details name them (never the provider a bare entry falls back to).
[[nodiscard]] std::string FolderGroupCreator(const nlohmann::json& metadata);

class FolderToneLookup
{
  public:
    /// The most files one "Look up details" looks up; the rest wait for the next.
    static constexpr std::size_t kBatch = 10;

    /// How a batch ended, for the UI's notification.
    struct Summary
    {
        std::size_t found = 0, looked = 0, remaining = 0;
        std::string error; // set when it stopped on a failed request
    };

    FolderToneLookup(UiClient& client, ToneService& tones);
    ~FolderToneLookup();

    FolderToneLookup(const FolderToneLookup&) = delete;
    FolderToneLookup& operator=(const FolderToneLookup&) = delete;

    /// A NAM capture or a WAV IR: the files Tone3000 hashes.
    [[nodiscard]] static bool CanLookUp(const FolderFile& file);

    /// The tone details for a file: what a lookup found this session, else its library entry's when
    /// that names a tone. Null when neither.
    [[nodiscard]] const nlohmann::json* DetailsFor(const FolderFile& file) const;

    /// Whether a batch would look the file up: one that can be, with no tone details yet, not
    /// looked up already this session.
    [[nodiscard]] bool NeedsLookup(const FolderFile& file) const;

    [[nodiscard]] bool Running() const noexcept
    {
        return mRunning;
    }

    /// The batch's progress: files finished, and how many it holds.
    [[nodiscard]] std::size_t Done() const noexcept
    {
        return mDone;
    }

    [[nodiscard]] std::size_t Total() const noexcept
    {
        return mBatch.size();
    }

    /// Looks up the first kBatch of `files` that need it. `onChange` runs after each file (a match
    /// regroups the list); `onFinished` once, at the end. Does nothing while a batch runs.
    void Start(const std::vector<FolderFile>& files, std::function<void()> onChange,
               std::function<void(const Summary&)> onFinished);

  private:
    void Next();
    void WithHash(const std::string& hash);
    void TryModels(std::shared_ptr<std::vector<tone3000::Model>> models, std::size_t index);
    void Matched(const tone3000::Tone& tone, const tone3000::Model& model, std::size_t others);
    void FileDone(bool found);
    void Fail(const std::string& error);
    void Finish();
    void OnEngineNews();
    [[nodiscard]] const LibraryResource* LibraryEntry(const FolderFile& file) const;

    UiClient& mClient;
    ToneService& mTones;
    std::shared_ptr<bool> mAlive = std::make_shared<bool>(true);
    std::unique_ptr<Subscription> mSubscription;

    std::map<std::string, nlohmann::json> mMatches; // path key -> what a lookup found
    std::set<std::string> mLookedUp;                // path keys

    bool mRunning = false;
    std::vector<FolderFile> mBatch;
    std::size_t mDone = 0, mFound = 0, mRemaining = 0;
    std::string mWaitingForHash; // path key of the file whose hash the engine is working out
    std::string mCurrentHash;
    std::map<std::string, std::optional<tone3000::Tone>> mToneCache; // a batch's tones read (null: hidden)
    std::function<void()> mOnChange;
    std::function<void(const Summary&)> mOnFinished;
};
} // namespace guitarfx::uiclient
