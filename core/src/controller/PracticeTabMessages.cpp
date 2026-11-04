#include "controller/PracticeTabMessages.h"

#include "MessageHandlerRegistry.h"
#include "models/ModelHasher.h"
#include "storage/JsonStore.h"
#include "util/Base64.h"
#include "util/FileIO.h"
#include "util/PathEncoding.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <nlohmann/json.hpp>

namespace guitarfx
{
namespace
{
constexpr const char* kErrorTitle = "Unable to open tablature";

/// The file's own name, without folders, and never empty — it labels the tab in the UI.
std::string CleanFileName(const std::string& name)
{
    const auto slash = name.find_last_of("/\\");
    std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
    return base.empty() ? std::string("Tablature") : base;
}

/// Stores the score under its content hash (the same file opened twice is one row) and
/// hands it to the UI. Returns false, having reported why, when it cannot be kept.
bool StoreAndSend(const PracticeTabMessageDeps& deps, const std::string& fileName,
                  const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty())
    {
        deps.reportError(kErrorTitle, "The file is empty.");
        return false;
    }

    if (bytes.size() > kMaxPracticeTabBytes)
    {
        deps.reportError(kErrorTitle, "The file is larger than 16 MB, which is more than any score needs.");
        return false;
    }

    const std::string id = ModelHasher().HashBytes(std::span<const std::uint8_t>(bytes.data(), bytes.size()));
    const std::string name = CleanFileName(fileName);
    const std::string data = util::EncodeBase64(bytes);
    const auto now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();

    const nlohmann::json document = {{"fileName", name}, {"data", data}, {"size", bytes.size()}, {"storedAt", now}};

    if (!deps.store().Put(storage::ItemType::kPracticeTab, id, document))
    {
        deps.reportError(kErrorTitle, "The file could not be saved to the library.");
        return false;
    }

    deps.sendToUI(
        nlohmann::json{{"type", "practiceToolTabLoaded"}, {"id", id}, {"fileName", name}, {"data", data}}.dump());
    return true;
}

void SendFailed(const PracticeTabMessageDeps& deps, const std::string& id, const std::string& error)
{
    deps.sendToUI(nlohmann::json{{"type", "practiceToolTabFailed"}, {"id", id}, {"error", error}}.dump());
}
} // namespace

void RegisterPracticeTabMessages(MessageHandlerRegistry& registry, PracticeTabMessageDeps deps)
{
    auto shared = std::make_shared<PracticeTabMessageDeps>(std::move(deps));

    registry.Register("browsePracticeToolTab", [shared](const nlohmann::json&) {
        shared->browseForTab([shared](const BrowseFileResult& result) {
            if (!result.success)
            {
                return;
            }

            std::error_code error;
            const auto size = std::filesystem::file_size(result.path, error);

            if (!error && size > kMaxPracticeTabBytes)
            {
                shared->reportError(kErrorTitle, "The file is larger than 16 MB, which is more than any score needs.");
                return;
            }

            const auto bytes = util::ReadFileBytes(result.path);

            if (bytes.empty())
            {
                shared->reportError(kErrorTitle, "The file could not be read: " + util::PathToUtf8(result.path));
                return;
            }

            StoreAndSend(*shared, util::PathToUtf8(result.path.filename()), bytes);
        });
    });

    registry.Register("importPracticeToolTabData", [shared](const nlohmann::json& payload) {
        const std::string encoded = payload.value("data", "");

        // The base64 of the largest file allowed, with room for padding.
        if (encoded.size() > (kMaxPracticeTabBytes / 3 + 1) * 4)
        {
            shared->reportError(kErrorTitle, "The file is larger than 16 MB, which is more than any score needs.");
            return;
        }

        StoreAndSend(*shared, payload.value("fileName", ""), util::DecodeBase64(encoded));
    });

    registry.Register("requestPracticeToolTab", [shared](const nlohmann::json& payload) {
        const std::string id = payload.value("id", "");

        if (id.empty())
        {
            return;
        }

        const auto stored = shared->store().Get(storage::ItemType::kPracticeTab, id);

        if (!stored || !stored->is_object() || !stored->contains("data") || !(*stored)["data"].is_string())
        {
            SendFailed(*shared, id, "This tablature is no longer in the library. Open the file again.");
            return;
        }

        shared->sendToUI(nlohmann::json{{"type", "practiceToolTabLoaded"},
                                        {"id", id},
                                        {"fileName", stored->value("fileName", std::string("Tablature"))},
                                        {"data", (*stored)["data"]}}
                             .dump());
    });
}
} // namespace guitarfx
