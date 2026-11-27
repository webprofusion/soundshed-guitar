// ToneServicePreview.cpp - auditioning a shared preset before installing it, and the favourites and
// ratings of installed ones followed back to the service. The rest of ToneService is in
// ToneService.cpp.
//
// A preview is read and played by the engine ("previewPresetArchive"), which writes nothing: no
// preset, model, IR or blend reaches the library. What only a UI can do stays here: downloading
// the item, and fetching the Tone3000 models it names that the library lacks, as the web UI's
// preview does. Those are found by their tone and model ids in Tone3000's own listing; a URL the
// archive carries is never followed, so a shared archive cannot choose where a request goes.

#include "uiclient/ToneService.h"

#include "controller/internal/PresetArchiveSupport.h"
#include "util/Base64.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <array>
#include <span>
#include <string_view>

namespace guitarfx::uiclient
{
namespace
{
std::string PreviewPresetId(const std::string& itemId)
{
    return "preview-" + itemId;
}

std::string Base64Of(const std::string& bytes)
{
    return util::EncodeBase64(std::vector<std::uint8_t>(bytes.begin(), bytes.end()));
}

/// A provider's download: the file itself, or a zip holding it (its first model or IR file).
std::optional<std::string> ModelFile(const std::string& bytes, const std::string& resourceType)
{
    if (bytes.size() <= 4 || bytes[0] != 'P' || bytes[1] != 'K')
    {
        return bytes;
    }

    static constexpr std::array<std::string_view, 2> kNam{".nam", ".json"};
    static constexpr std::array<std::string_view, 2> kIr{".wav", ".ir"};
    const std::vector<std::uint8_t> zip(bytes.begin(), bytes.end());
    auto entry = controller_detail::ExtractFirstZipEntryWithExtension(
        zip, resourceType == "ir" ? std::span<const std::string_view>(kIr) : std::span<const std::string_view>(kNam));

    if (!entry)
    {
        return std::nullopt;
    }

    return std::string(entry->bytes.begin(), entry->bytes.end());
}

/// What the preview gathers while its models download.
struct PreviewFetch
{
    std::string archive;
    nlohmann::json fetched = nlohmann::json::array(); // {resourceType, resourceId, name, data}
    std::vector<std::string> unavailable;
    std::size_t remaining = 0;
};
} // namespace

void ToneService::Preview(const tones::Tone& item)
{
    if (item.id.empty() || item.IsPack())
    {
        return;
    }

    // Only the first preview remembers what was playing: a second one goes back to the same.
    if (mPreviewItemId.empty())
    {
        mPriorPresetId = mClient.State().activePresetId;
    }

    mPreviewItemId = item.id;
    mPreviewTitle = item.title;
    mPreviewLoading = true;
    const int generation = ++mPreviewGeneration;
    Changed();

    Request(
        "GET", tones::ResolveUrl(tones::ItemDownloadPath(item.id)), {}, false,
        [this, generation, item](const HttpResult& result) {
            if (generation != mPreviewGeneration)
            {
                return;
            }

            if (result.status != 200 || result.body.empty())
            {
                PreviewFailed(generation, result.status == 0 ? result.error
                                                             : tones::ParseResponse(result.status, result.body).error);
                return;
            }

            // Read here first only to find what to fetch; the engine reads it again to play it.
            std::string error;
            const std::vector<std::uint8_t> bytes(result.body.begin(), result.body.end());

            if (!controller_detail::ParseFactoryPresetArchive(util::PathFromUtf8("preview"), bytes, error))
            {
                PreviewFailed(generation, error.empty() ? std::string("The download is not a preset archive.") : error);
                return;
            }

            std::vector<tone3000::RemoteRef> missing;
            auto pending = std::make_shared<PreviewFetch>();
            pending->archive = result.body;

            for (auto& ref : tone3000::MissingRemoteRefs(mClient.State(), result.body))
            {
                if (ref.provider == "tone3000" && !ref.toneId.empty() && !ref.modelId.empty())
                {
                    missing.push_back(std::move(ref));
                }
                else
                {
                    pending->unavailable.push_back(ref.name.empty() ? ref.id : ref.name);
                }
            }

            if (missing.empty())
            {
                SendPreview(generation, item, pending->archive, pending->fetched);
                return;
            }

            pending->remaining = missing.size();
            const auto finishOne = [this, generation, item, pending] {
                if (--pending->remaining > 0 || generation != mPreviewGeneration)
                {
                    return;
                }

                if (!pending->unavailable.empty() && mHost.toast)
                {
                    std::string names;

                    for (const auto& name : pending->unavailable)
                    {
                        names += (names.empty() ? "" : ", ") + name;
                    }

                    mHost.toast("The preview plays without some models",
                                names + " could not be fetched. Installing the tone tries again.", true);
                }

                SendPreview(generation, item, pending->archive, pending->fetched);
            };

            for (const auto& ref : missing)
            {
                Tone3000Models(ref.toneId, [this, ref, pending, finishOne](const std::vector<tone3000::Model>& models,
                                                                           const std::string&) {
                    const auto name = ref.name.empty() ? ref.id : ref.name;
                    const auto model = std::find_if(models.begin(), models.end(), [&ref](const tone3000::Model& m) {
                        return m.id == ref.modelId && !m.modelUrl.empty();
                    });

                    if (model == models.end())
                    {
                        pending->unavailable.push_back(name);
                        finishOne();
                        return;
                    }

                    Tone3000Get(model->modelUrl, [ref, name, pending, finishOne](const HttpResult& download) {
                        const auto file = download.status == 200 && !download.body.empty()
                                              ? ModelFile(download.body, ref.type)
                                              : std::nullopt;

                        if (file)
                        {
                            pending->fetched.push_back({{"resourceType", ref.type},
                                                        {"resourceId", ref.id},
                                                        {"name", name},
                                                        {"data", Base64Of(*file)}});
                        }
                        else
                        {
                            pending->unavailable.push_back(name);
                        }

                        finishOne();
                    });
                });
            }
        });
}

void ToneService::SendPreview(int generation, const tones::Tone& item, const std::string& archive,
                              const nlohmann::json& fetched)
{
    if (generation != mPreviewGeneration)
    {
        return;
    }

    // Answered by the preset loading under this id (OnActivePresetChanged), or by the engine's
    // error, which the UI shows as it shows every other.
    mClient.Send("previewPresetArchive", {{"requestId", "tone-preview-" + std::to_string(generation)},
                                          {"itemId", item.id},
                                          {"presetId", PreviewPresetId(item.id)},
                                          {"title", item.title},
                                          {"data", Base64Of(archive)},
                                          {"fetchedResources", fetched}});
}

void ToneService::PreviewFailed(int generation, const std::string& reason)
{
    if (generation != mPreviewGeneration)
    {
        return;
    }

    const auto title = mPreviewTitle;
    // Back to what was there before: a preview already playing goes on playing.
    const bool playing = mClient.State().activePresetId == PreviewPresetId(mPreviewItemId);

    if (!playing)
    {
        mPreviewItemId.clear();
        mPreviewTitle.clear();
        mPriorPresetId.clear();
    }

    mPreviewLoading = false;
    Changed();

    if (mHost.toast)
    {
        mHost.toast("Could not preview \"" + title + "\"", reason, true);
    }
}

void ToneService::StopPreview()
{
    if (mPreviewItemId.empty())
    {
        return;
    }

    ++mPreviewGeneration;
    const auto prior = mPriorPresetId;
    const bool playing = mClient.State().activePresetId == PreviewPresetId(mPreviewItemId);
    mPreviewItemId.clear();
    mPreviewTitle.clear();
    mPriorPresetId.clear();
    mPreviewLoading = false;
    Changed();

    if (playing && !prior.empty())
    {
        mCommands.LoadPreset(prior);
    }
}

void ToneService::OnActivePresetChanged()
{
    if (mPreviewItemId.empty())
    {
        return;
    }

    const auto& active = mClient.State().activePresetId;

    if (active == PreviewPresetId(mPreviewItemId))
    {
        if (mPreviewLoading)
        {
            mPreviewLoading = false;
            Changed();
        }

        return;
    }

    // Another preset was loaded over the preview (or before it arrived, when not still loading):
    // the preview is over, and there is nothing to put back.
    if (!mPreviewLoading)
    {
        ++mPreviewGeneration;
        mPreviewItemId.clear();
        mPreviewTitle.clear();
        mPriorPresetId.clear();
        Changed();
    }
}

// ── Favourites and ratings ──────────────────────────────────────────────────────

void ToneService::SyncFavorite(const std::string& presetId, bool favorite)
{
    const auto itemId = tones::ItemIdForPreset(mClient.State().appSettings, presetId);

    if (itemId.empty() || !HasSession())
    {
        return;
    }

    Request(favorite ? "PUT" : "DELETE", tones::ResolveUrl(tones::ItemFavoritePath(itemId)), {}, true,
            [](const HttpResult&) {});
}

void ToneService::SyncRating(const std::string& presetId, int rating)
{
    const auto itemId = tones::ItemIdForPreset(mClient.State().appSettings, presetId);

    if (itemId.empty() || !HasSession())
    {
        return;
    }

    if (rating <= 0)
    {
        Request("DELETE", tones::ResolveUrl(tones::ItemRatingPath(itemId)), {}, true, [](const HttpResult&) {});
    }
    else
    {
        Request("PUT", tones::ResolveUrl(tones::ItemRatingPath(itemId)),
                nlohmann::json{{"rating", std::min(rating, 5)}}.dump(), true, [](const HttpResult&) {});
    }
}
} // namespace guitarfx::uiclient
