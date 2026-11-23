/**
 * FolderToneLookup.cpp - "Look up details" and the tone groups of the resource browser's Folder
 * tab, for the native UIs (FolderToneLookup.h).
 */

#include "uiclient/FolderToneLookup.h"

#include "models/ContentHash.h"
#include "uiclient/ToneService.h"
#include "uiclient/UiClient.h"

#include <algorithm>
#include <cctype>

namespace guitarfx::uiclient
{
namespace
{
std::string Str(const nlohmann::json& object, const char* key)
{
    if (!object.is_object())
    {
        return {};
    }

    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

std::string Trim(std::string text)
{
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

bool EndsWithWav(const std::string& name)
{
    if (name.size() < 4)
    {
        return false;
    }

    std::string tail = name.substr(name.size() - 4);
    std::transform(tail.begin(), tail.end(), tail.begin(), [](unsigned char c) { return std::tolower(c); });
    return tail == ".wav";
}
} // namespace

std::string FolderPathKey(const std::string& path)
{
    std::string key = path;
    std::replace(key.begin(), key.end(), '\\', '/');
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
    return key;
}

// ── Groups ──────────────────────────────────────────────────────────────────────

FolderToneGroups GroupFolderFiles(const std::vector<const FolderFile*>& files,
                                  const std::function<const nlohmann::json*(const FolderFile&)>& detailsFor)
{
    FolderToneGroups result;
    std::map<std::string, std::size_t> byTone;

    for (const auto* file : files)
    {
        const auto* details = detailsFor ? detailsFor(*file) : nullptr;
        const auto toneId = details != nullptr ? Trim(Str(*details, "toneId")) : std::string();

        if (toneId.empty())
        {
            result.items.push_back({file, FolderToneGroups::kNoGroup});
            continue;
        }

        const auto key = "tone:" + toneId;

        if (const auto it = byTone.find(key); it != byTone.end())
        {
            auto& group = result.groups[it->second];
            group.files.push_back(file);

            // A member that knows more of the tone (its title, its creator) fills in what the first did not.
            for (const auto& [field, value] : details->items())
            {
                if (value.is_string() && !value.get<std::string>().empty() &&
                    Str(group.metadata, field.c_str()).empty())
                {
                    group.metadata[field] = value;
                }
            }

            if (group.title.empty())
            {
                group.title = Trim(Str(group.metadata, "toneTitle"));
                group.title = group.title.empty() ? Trim(Str(group.metadata, "groupName")) : group.title;
            }

            continue;
        }

        FolderToneGroup group;
        group.key = key;
        group.metadata = details->is_object() ? *details : nlohmann::json::object();
        group.title = Trim(Str(group.metadata, "toneTitle"));
        group.title = group.title.empty() ? Trim(Str(group.metadata, "groupName")) : group.title;
        group.files.push_back(file);
        byTone.emplace(key, result.groups.size());
        result.items.push_back({nullptr, result.groups.size()});
        result.groups.push_back(std::move(group));
    }

    // A tone with one file here is just that file.
    for (auto& item : result.items)
    {
        if (item.group != FolderToneGroups::kNoGroup && result.groups[item.group].files.size() < 2)
        {
            item.file = result.groups[item.group].files.front();
            item.group = FolderToneGroups::kNoGroup;
        }
    }

    return result;
}

std::string SharedTonePrefix(const std::vector<std::string>& names)
{
    std::vector<std::string> known;

    for (const auto& name : names)
    {
        if (auto trimmed = Trim(name); !trimmed.empty())
        {
            known.push_back(std::move(trimmed));
        }
    }

    if (known.size() < 2)
    {
        return {};
    }

    std::string prefix = known.front();

    for (std::size_t i = 1; i < known.size(); ++i)
    {
        std::size_t length = 0;

        while (length < prefix.size() && length < known[i].size() &&
               std::tolower(static_cast<unsigned char>(prefix[length])) ==
                   std::tolower(static_cast<unsigned char>(known[i][length])))
        {
            ++length;
        }

        prefix.resize(length);
    }

    // Every name identical: the tone is the part before its own last separator.
    const auto cut = prefix.rfind(" - ");
    return cut != std::string::npos && cut > 0 ? Trim(prefix.substr(0, cut)) : std::string();
}

std::string FolderGroupCreator(const nlohmann::json& metadata)
{
    for (const char* key : {"creatorName", "authorUsername", "modeledBy", "creator"})
    {
        if (auto value = Trim(Str(metadata, key)); !value.empty())
        {
            return value;
        }
    }

    return {};
}

// ── FolderToneLookup ────────────────────────────────────────────────────────────

FolderToneLookup::FolderToneLookup(UiClient& client, ToneService& tones)
    : mClient(client), mTones(tones), mSubscription(std::make_unique<Subscription>(
                                          client.Subscribe(Topic::ResourceFolders, [this] { OnEngineNews(); })))
{
}

FolderToneLookup::~FolderToneLookup()
{
    *mAlive = false;
}

bool FolderToneLookup::CanLookUp(const FolderFile& file)
{
    return file.resourceType == "nam" || EndsWithWav(file.name);
}

const LibraryResource* FolderToneLookup::LibraryEntry(const FolderFile& file) const
{
    const auto& state = mClient.State();

    if (!file.libraryId.empty())
    {
        if (const auto* entry = state.FindLibraryResource(file.resourceType, file.libraryId))
        {
            return entry;
        }
    }

    const auto it = state.resources.find(file.resourceType);

    if (it == state.resources.end())
    {
        return nullptr;
    }

    const auto key = FolderPathKey(file.path);

    for (const auto& resource : it->second)
    {
        if (!resource.filePath.empty() && FolderPathKey(resource.filePath) == key)
        {
            return &resource;
        }
    }

    return nullptr;
}

const nlohmann::json* FolderToneLookup::DetailsFor(const FolderFile& file) const
{
    if (const auto it = mMatches.find(FolderPathKey(file.path)); it != mMatches.end())
    {
        return &it->second;
    }

    const auto* entry = LibraryEntry(file);
    return entry != nullptr && !Trim(Str(entry->metadata, "toneId")).empty() ? &entry->metadata : nullptr;
}

bool FolderToneLookup::NeedsLookup(const FolderFile& file) const
{
    const auto key = FolderPathKey(file.path);
    return CanLookUp(file) && mLookedUp.count(key) == 0 && mMatches.count(key) == 0 && DetailsFor(file) == nullptr;
}

void FolderToneLookup::Start(const std::vector<FolderFile>& files, std::function<void()> onChange,
                             std::function<void(const Summary&)> onFinished)
{
    if (mRunning)
    {
        return;
    }

    mBatch.clear();
    std::size_t wanted = 0;

    for (const auto& file : files)
    {
        if (NeedsLookup(file))
        {
            ++wanted;

            if (mBatch.size() < kBatch)
            {
                mBatch.push_back(file);
            }
        }
    }

    mRemaining = wanted - mBatch.size();
    mOnChange = std::move(onChange);
    mOnFinished = std::move(onFinished);
    mDone = 0;
    mFound = 0;
    mToneCache.clear();

    if (mBatch.empty())
    {
        Finish();
        return;
    }

    mRunning = true;

    if (mOnChange)
    {
        mOnChange();
    }

    Next();
}

void FolderToneLookup::Next()
{
    if (mDone >= mBatch.size())
    {
        Finish();
        return;
    }

    const auto& file = mBatch[mDone];
    const auto key = FolderPathKey(file.path);
    // The hash: its library entry's, the listing's, one the engine already worked out, else ask.
    std::string hash;

    if (const auto* entry = LibraryEntry(file))
    {
        hash = Str(entry->metadata, kContentHashMetadataKey);
    }

    if (!IsContentHash(hash))
    {
        hash = Str(file.metadata, kContentHashMetadataKey);
    }

    if (!IsContentHash(hash))
    {
        const auto& known = mClient.State().resourceFileHashes;

        if (const auto it = known.find(key); it != known.end())
        {
            hash = it->second;
        }
        else
        {
            mWaitingForHash = key;
            mClient.Send("hashResourceFile", {{"path", file.path}, {"resourceType", file.resourceType}});
            return;
        }
    }

    WithHash(hash);
}

void FolderToneLookup::OnEngineNews()
{
    if (mWaitingForHash.empty())
    {
        return;
    }

    const auto& known = mClient.State().resourceFileHashes;
    const auto it = known.find(mWaitingForHash);

    if (it == known.end())
    {
        return;
    }

    mWaitingForHash.clear();
    WithHash(it->second);
}

void FolderToneLookup::WithHash(const std::string& hash)
{
    if (!IsContentHash(hash))
    {
        // Unreadable: not offered again either, it would fail the same way.
        FileDone(false);
        return;
    }

    mCurrentHash = hash;
    mTones.Tone3000ModelsByContentHash(
        hash, [this, alive = mAlive](const std::vector<tone3000::Model>& models, const std::string& error) {
            if (!*alive || !mRunning)
            {
                return;
            }

            if (!error.empty())
            {
                Fail(error);
                return;
            }

            // Preference: a model in the tone its library entry names, then one in a tone this
            // batch already read (files uploaded together end up in one group), then Tone3000's order.
            auto ordered = std::make_shared<std::vector<tone3000::Model>>();
            const auto* entry = LibraryEntry(mBatch[mDone]);
            const auto preferredTone = entry != nullptr ? Trim(Str(entry->metadata, "toneId")) : std::string();
            const auto take = [&](const auto& pick) {
                for (const auto& model : models)
                {
                    const bool already = std::any_of(ordered->begin(), ordered->end(),
                                                     [&](const tone3000::Model& m) { return m.id == model.id; });

                    if (!already && pick(model))
                    {
                        ordered->push_back(model);
                    }
                }
            };
            take([&](const tone3000::Model& m) { return !preferredTone.empty() && m.toneId == preferredTone; });
            take([&](const tone3000::Model& m) {
                const auto it = mToneCache.find(m.toneId);
                return it != mToneCache.end() && it->second.has_value();
            });
            take([](const tone3000::Model&) { return true; });
            TryModels(ordered, 0);
        });
}

void FolderToneLookup::TryModels(std::shared_ptr<std::vector<tone3000::Model>> models, std::size_t index)
{
    // Each model in turn until one's tone can be read (a repost's tone may be private).
    while (index < models->size() && (*models)[index].toneId.empty())
    {
        ++index;
    }

    if (index >= models->size())
    {
        FileDone(false);
        return;
    }

    const auto model = (*models)[index];

    if (const auto it = mToneCache.find(model.toneId); it != mToneCache.end())
    {
        if (it->second)
        {
            Matched(*it->second, model, models->size() - 1);
        }
        else
        {
            TryModels(models, index + 1);
        }

        return;
    }

    mTones.Tone3000Tone(model.toneId, [this, alive = mAlive, models, index,
                                       model](const std::optional<tone3000::Tone>& tone, const std::string& error) {
        if (!*alive || !mRunning)
        {
            return;
        }

        if (!error.empty())
        {
            Fail(error);
            return;
        }

        mToneCache[model.toneId] = tone;

        if (tone)
        {
            Matched(*tone, model, models->size() - 1);
        }
        else
        {
            TryModels(models, index + 1);
        }
    });
}

void FolderToneLookup::Matched(const tone3000::Tone& tone, const tone3000::Model& model, std::size_t /*others*/)
{
    const auto& file = mBatch[mDone];
    // Tone3000's attribution, licence and ids, as an import from that tone records them; what the
    // engine read from the file itself stays: its content hash and architecture version.
    auto found =
        tone3000::ImportPayload(tone, model, file.resourceType, {}, {}).value("metadata", nlohmann::json::object());
    found.erase("architectureVersion");
    found[kContentHashMetadataKey] = mCurrentHash;
    found["tone3000MatchedBy"] = "contentHash";

    for (auto it = found.begin(); it != found.end();)
    {
        it = it->is_string() && it->get<std::string>().empty() ? found.erase(it) : std::next(it);
    }

    if (const auto* entry = LibraryEntry(file))
    {
        auto metadata = entry->metadata.is_object() ? entry->metadata : nlohmann::json::object();

        for (const auto& [field, value] : found.items())
        {
            metadata[field] = value;
        }

        mMatches[FolderPathKey(file.path)] = metadata;
        mClient.Send("updateLibraryResource",
                     {{"resourceType", file.resourceType}, {"resourceId", entry->id}, {"metadata", metadata}});
    }
    else
    {
        mMatches[FolderPathKey(file.path)] = std::move(found);
    }

    FileDone(true);
}

void FolderToneLookup::FileDone(bool found)
{
    mLookedUp.insert(FolderPathKey(mBatch[mDone].path));
    mFound += found ? 1 : 0;
    ++mDone;

    if (mOnChange)
    {
        mOnChange();
    }

    Next();
}

void FolderToneLookup::Fail(const std::string& error)
{
    // No network, or the service refused: stop rather than fail the same way ten times. The files
    // not finished are offered again.
    mRunning = false;
    mWaitingForHash.clear();
    Summary summary;
    summary.found = mFound;
    summary.looked = mDone;
    summary.remaining = mRemaining + (mBatch.size() - mDone);
    summary.error = error;
    mBatch.clear();
    mDone = 0;

    if (auto finished = std::move(mOnFinished))
    {
        finished(summary);
    }
}

void FolderToneLookup::Finish()
{
    mRunning = false;
    Summary summary;
    summary.found = mFound;
    summary.looked = mDone;
    summary.remaining = mRemaining;
    mBatch.clear();
    mDone = 0;

    if (auto finished = std::move(mOnFinished))
    {
        finished(summary);
    }
}
} // namespace guitarfx::uiclient
