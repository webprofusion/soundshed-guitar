#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"
#include "util/PathEncoding.h"

namespace fs = std::filesystem;

namespace
{
class TestHost final : public guitarfx::IPluginHost
{
  public:
    explicit TestHost(fs::path root) : mRoot(std::move(root))
    {
    }

    void SendMessageToUI(const std::string& jsonMessage) override
    {
        std::lock_guard<std::mutex> lock(mMessageMutex);
        messages.push_back(jsonMessage);
        mMessageCv.notify_all();
    }

    void BrowseFileAsync(guitarfx::BrowseFileType, const std::string&,
                         std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        callback(guitarfx::BrowseFileResult{});
    }

    void SaveFileAsync(guitarfx::BrowseFileType, const std::string&, const std::string&,
                       std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        callback(guitarfx::BrowseFileResult{});
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        fn();
    }

    [[nodiscard]] fs::path GetUserDataPath() const override
    {
        return mRoot;
    }

    [[nodiscard]] fs::path GetBundledAssetsPath() const override
    {
        return mRoot;
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return 48000.0;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return 512;
    }

    std::vector<std::string> messages;

    bool WaitForMessageType(const std::string& type, std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(mMessageMutex);
        return mMessageCv.wait_for(lock, timeout, [&]() {
            for (const auto& message : messages)
            {
                try
                {
                    if (nlohmann::json::parse(message).value("type", "") == type)
                    {
                        return true;
                    }
                }
                catch (const std::exception&)
                {
                }
            }

            return false;
        });
    }

    std::optional<nlohmann::json> LastMessageOfType(const std::string& type)
    {
        std::lock_guard<std::mutex> lock(mMessageMutex);

        for (auto it = messages.rbegin(); it != messages.rend(); ++it)
        {
            try
            {
                const auto parsed = nlohmann::json::parse(*it);

                if (parsed.value("type", "") == type)
                {
                    return parsed;
                }
            }
            catch (const std::exception&)
            {
            }
        }

        return std::nullopt;
    }

  private:
    fs::path mRoot;
    std::condition_variable mMessageCv;
    std::mutex mMessageMutex;
};

void SetSettingsEnvRoot(const fs::path& root)
{
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
}

guitarfx::Preset BuildSingleNodeResourcePreset(const std::string& nodeId, const std::string& resourceType,
                                               const std::string& resourceId, int resourceSlots = 1)
{
    using namespace guitarfx;

    Preset preset;
    preset.id = "preview-workflow";
    preset.name = "Preview Workflow";

    GraphNode input;
    input.id = "in";
    input.type = kNodeTypeInput;

    GraphNode effect;
    effect.id = nodeId;
    effect.type = "utility";
    effect.enabled = true;
    effect.resources.resize(static_cast<size_t>(resourceSlots));
    effect.resources[0].resourceType = resourceType;
    effect.resources[0].resourceId = resourceId;

    GraphNode output;
    output.id = "out";
    output.type = kNodeTypeOutput;

    preset.graph.nodes = {input, effect, output};
    preset.graph.edges = {
        {"in", nodeId, 0, 0, 1.0},
        {nodeId, "out", 0, 0, 1.0},
    };

    return preset;
}

bool LoadPreset(guitarfx::PluginController& controller, const guitarfx::Preset& preset)
{
    const auto presetJson = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
    nlohmann::json message;
    message["type"] = "loadPreset";
    message["preset"] = presetJson;
    controller.HandleUIMessage(message.dump());

    return controller.GetActivePreset().has_value();
}

bool TestPreviewApplyAndCancel(const std::string& resourceType)
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preview-workflow-tests" / resourceType;
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    const std::string nodeId = resourceType == "ir" ? "cab-node" : "amp-node";
    const std::string originalResourceId = resourceType == "ir" ? "orig-ir" : "orig-nam";

    auto preset = BuildSingleNodeResourcePreset(nodeId, resourceType, originalResourceId, 1);

    if (!LoadPreset(controller, preset))
    {
        std::cerr << "Failed to load initial preset for " << resourceType << " test\n";
        return false;
    }

    nlohmann::json preview;
    preview["type"] = "previewRemoteResource";
    preview["resourceType"] = resourceType;
    preview["tempResourceId"] = "preview-temp-id";
    preview["nodeId"] = nodeId;
    preview["resourceIndex"] = 0;
    preview["isZip"] = false;
    preview["data"] = "AQID";
    controller.HandleUIMessage(preview.dump());

    const auto& activeAfterPreview = controller.GetActivePreset();

    if (!activeAfterPreview)
    {
        std::cerr << "Active preset missing after preview for " << resourceType << "\n";
        return false;
    }

    const auto* nodeAfterPreview = activeAfterPreview->graph.FindNode(nodeId);

    if (!nodeAfterPreview || nodeAfterPreview->resources.empty())
    {
        std::cerr << "Target node missing after preview for " << resourceType << "\n";
        return false;
    }

    const auto& previewRef = nodeAfterPreview->resources[0];

    if (!previewRef.filePath.has_filename() || !fs::exists(previewRef.filePath))
    {
        std::cerr << "Preview file path not set/existing for " << resourceType << "\n";
        return false;
    }

    if (!previewRef.resourceId.empty())
    {
        std::cerr << "Preview should clear resourceId but got: " << previewRef.resourceId << "\n";
        return false;
    }

    nlohmann::json cancel;
    cancel["type"] = "cancelPreviewResource";
    cancel["nodeId"] = nodeId;
    cancel["resourceIndex"] = 0;
    controller.HandleUIMessage(cancel.dump());

    const auto& activeAfterCancel = controller.GetActivePreset();

    if (!activeAfterCancel)
    {
        std::cerr << "Active preset missing after cancel for " << resourceType << "\n";
        return false;
    }

    const auto* nodeAfterCancel = activeAfterCancel->graph.FindNode(nodeId);

    if (!nodeAfterCancel || nodeAfterCancel->resources.empty())
    {
        std::cerr << "Target node missing after cancel for " << resourceType << "\n";
        return false;
    }

    const auto& restoredRef = nodeAfterCancel->resources[0];

    if (restoredRef.resourceId != originalResourceId)
    {
        std::cerr << "Cancel did not restore original resourceId for " << resourceType << "\n";
        return false;
    }

    if (!restoredRef.filePath.empty())
    {
        std::cerr << "Cancel should restore library ref without file path for " << resourceType << "\n";
        return false;
    }

    return true;
}

bool TestPreviewHonorsResourceIndex()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preview-workflow-tests" / "index";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    auto preset = BuildSingleNodeResourcePreset("multi-slot", "nam", "slot-0", 2);
    preset.graph.FindNode("multi-slot")->resources[1].resourceType = "nam";
    preset.graph.FindNode("multi-slot")->resources[1].resourceId = "slot-1";

    if (!LoadPreset(controller, preset))
    {
        std::cerr << "Failed to load multi-slot preset\n";
        return false;
    }

    nlohmann::json preview;
    preview["type"] = "previewRemoteResource";
    preview["resourceType"] = "nam";
    preview["tempResourceId"] = "preview-index";
    preview["nodeId"] = "multi-slot";
    preview["resourceIndex"] = 1;
    preview["isZip"] = false;
    preview["data"] = "AQID";
    controller.HandleUIMessage(preview.dump());

    const auto& activeAfterPreview = controller.GetActivePreset();

    if (!activeAfterPreview)
    {
        return false;
    }

    const auto* node = activeAfterPreview->graph.FindNode("multi-slot");

    if (!node || node->resources.size() < 2)
    {
        return false;
    }

    const auto& slot0 = node->resources[0];
    const auto& slot1 = node->resources[1];

    if (slot0.resourceId != "slot-0" || !slot0.filePath.empty())
    {
        std::cerr << "Preview unexpectedly modified slot 0\n";
        return false;
    }

    if (!slot1.resourceId.empty() || slot1.filePath.empty())
    {
        std::cerr << "Preview did not apply to selected slot index\n";
        return false;
    }

    nlohmann::json cancel;
    cancel["type"] = "cancelPreviewResource";
    cancel["nodeId"] = "multi-slot";
    cancel["resourceIndex"] = 1;
    controller.HandleUIMessage(cancel.dump());

    const auto& activeAfterCancel = controller.GetActivePreset();

    if (!activeAfterCancel)
    {
        return false;
    }

    const auto* nodeAfterCancel = activeAfterCancel->graph.FindNode("multi-slot");

    if (!nodeAfterCancel || nodeAfterCancel->resources.size() < 2)
    {
        return false;
    }

    if (nodeAfterCancel->resources[1].resourceId != "slot-1" || !nodeAfterCancel->resources[1].filePath.empty())
    {
        std::cerr << "Cancel did not restore selected slot\n";
        return false;
    }

    return true;
}

bool TestLibraryPreviewCloseRevertsOriginal()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preview-workflow-tests" / "library-revert";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    auto preset = BuildSingleNodeResourcePreset("amp-node", "nam", "original-lib-id", 1);

    if (!LoadPreset(controller, preset))
    {
        std::cerr << "Failed to load preset for library preview revert test\n";
        return false;
    }

    nlohmann::json previewSelect;
    previewSelect["type"] = "updateNodeResource";
    previewSelect["nodeId"] = "amp-node";
    previewSelect["resourceType"] = "nam";
    previewSelect["resourceId"] = "preview-lib-id";
    previewSelect["filePath"] = "";
    previewSelect["resourceIndex"] = 0;
    controller.HandleUIMessage(previewSelect.dump());

    const auto& activeAfterPreview = controller.GetActivePreset();

    if (!activeAfterPreview)
    {
        return false;
    }

    const auto* previewNode = activeAfterPreview->graph.FindNode("amp-node");

    if (!previewNode || previewNode->resources.empty())
    {
        return false;
    }

    if (previewNode->resources[0].resourceId != "preview-lib-id")
    {
        std::cerr << "Preview selection did not update to temporary library resource\n";
        return false;
    }

    // Simulate modal close without confirm: UI reverts with updateNodeResource to original ID.
    nlohmann::json revert;
    revert["type"] = "updateNodeResource";
    revert["nodeId"] = "amp-node";
    revert["resourceType"] = "nam";
    revert["resourceId"] = "original-lib-id";
    revert["filePath"] = "";
    revert["resourceIndex"] = 0;
    controller.HandleUIMessage(revert.dump());

    const auto& activeAfterRevert = controller.GetActivePreset();

    if (!activeAfterRevert)
    {
        return false;
    }

    const auto* revertedNode = activeAfterRevert->graph.FindNode("amp-node");

    if (!revertedNode || revertedNode->resources.empty())
    {
        return false;
    }

    if (revertedNode->resources[0].resourceId != "original-lib-id")
    {
        std::cerr << "Library preview close did not restore original resource\n";
        return false;
    }

    if (!revertedNode->resources[0].filePath.empty())
    {
        std::cerr << "Reverted library resource should not keep filePath override\n";
        return false;
    }

    return true;
}

bool TestPreviewMissingDataNoMutation()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preview-workflow-tests" / "missing-data";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    auto preset = BuildSingleNodeResourcePreset("amp-node", "nam", "original-lib-id", 1);

    if (!LoadPreset(controller, preset))
    {
        return false;
    }

    nlohmann::json preview;
    preview["type"] = "previewRemoteResource";
    preview["resourceType"] = "nam";
    preview["tempResourceId"] = "preview-missing-data";
    preview["nodeId"] = "amp-node";
    preview["resourceIndex"] = 0;
    preview["isZip"] = false;
    preview["data"] = "";
    controller.HandleUIMessage(preview.dump());

    const auto& active = controller.GetActivePreset();

    if (!active)
    {
        return false;
    }

    const auto* node = active->graph.FindNode("amp-node");

    if (!node || node->resources.empty())
    {
        return false;
    }

    return node->resources[0].resourceId == "original-lib-id" && node->resources[0].filePath.empty();
}

bool TestPreviewMissingNodeIdNoMutation()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preview-workflow-tests" / "missing-node-id";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    auto preset = BuildSingleNodeResourcePreset("cab-node", "ir", "original-ir-id", 1);

    if (!LoadPreset(controller, preset))
    {
        return false;
    }

    nlohmann::json preview;
    preview["type"] = "previewRemoteResource";
    preview["resourceType"] = "ir";
    preview["tempResourceId"] = "preview-missing-node";
    preview["resourceIndex"] = 0;
    preview["isZip"] = false;
    preview["data"] = "AQID";
    controller.HandleUIMessage(preview.dump());

    const auto& active = controller.GetActivePreset();

    if (!active)
    {
        return false;
    }

    const auto* node = active->graph.FindNode("cab-node");

    if (!node || node->resources.empty())
    {
        return false;
    }

    return node->resources[0].resourceId == "original-ir-id" && node->resources[0].filePath.empty();
}

bool TestCancelWithoutActivePreviewNoMutation()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preview-workflow-tests" / "cancel-no-preview";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    auto preset = BuildSingleNodeResourcePreset("amp-node", "nam", "steady-lib-id", 1);

    if (!LoadPreset(controller, preset))
    {
        return false;
    }

    nlohmann::json cancel;
    cancel["type"] = "cancelPreviewResource";
    cancel["nodeId"] = "amp-node";
    cancel["resourceIndex"] = 0;
    controller.HandleUIMessage(cancel.dump());

    const auto& active = controller.GetActivePreset();

    if (!active)
    {
        return false;
    }

    const auto* node = active->graph.FindNode("amp-node");

    if (!node || node->resources.empty())
    {
        return false;
    }

    return node->resources[0].resourceId == "steady-lib-id" && node->resources[0].filePath.empty();
}

bool TestFolderEnumerationPreservesUtf8Filename()
{
    const fs::path folder = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "amps" / "Guitar" / "A2";
    const std::string expectedName = "A2 -wth space an \xE2\x80\x94 _emdash.nam";

    TestHost host(fs::temp_directory_path() / "guitarfx-folder-enumeration-tests");
    guitarfx::PluginController controller(host);

    nlohmann::json request;
    request["type"] = "listResourceFolder";
    request["path"] = guitarfx::util::PathToUtf8(folder);
    controller.HandleUIMessage(request.dump());

    if (!host.WaitForMessageType("resourceFolderListing", std::chrono::seconds(5)))
    {
        std::cerr << "Folder enumeration did not return a listing\n";
        return false;
    }

    const auto listing = host.LastMessageOfType("resourceFolderListing");

    if (!listing || !listing->contains("files") || !(*listing)["files"].is_array())
    {
        return false;
    }

    for (const auto& file : (*listing)["files"])
    {
        if (file.value("name", "") == expectedName)
        {
            return true;
        }
    }

    std::cerr << "UTF-8 NAM filename was not returned by folder enumeration\n";
    return false;
}

void WriteBytes(const fs::path& path, const std::string& bytes)
{
    std::ofstream out(path, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/// A .nam header is read raw off disk, and one written in a legacy code page used to put
/// bytes into the metadata batch that the JSON serialiser rejects. The scan then failed as a
/// whole and the browser dropped a listing it was already showing, so one file hid the
/// folder. Also covers a JSON escape in a name, and a file with no extension whose last dot is
/// followed by text no ANSI code page holds, which failed the listing when classified.
bool TestFolderListingSurvivesUnreadableNamMetadata()
{
    const fs::path folder = fs::temp_directory_path() / "guitarfx-folder-metadata-tests";
    std::error_code ec;
    fs::remove_all(folder, ec);
    fs::create_directories(folder, ec);

    // 0xE9 is Latin-1's é: not UTF-8 on its own.
    WriteBytes(folder / "Latin1 metadata.nam",
               "{\"version\": \"0.5.4\", \"metadata\": {\"name\": \"Caf\xE9 Fuzz\", \"gear_type\": \"pedal\"}, "
               "\"architecture\": \"WaveNet\", \"config\": {}, \"weights\": []}");
    WriteBytes(folder / "Escaped metadata.nam",
               "{\"version\": \"0.5.4\", \"metadata\": {\"name\": \"The \\\"Best\\\" Caf\\u00e9\", "
               "\"modeled_by\": \"A \\\\ B\"}, \"architecture\": \"WaveNet\", \"config\": {}, \"weights\": []}");
    // "Notes v1.2 פדל": its "extension" is ".2 פדל".
    WriteBytes(folder / guitarfx::util::PathFromUtf8("Notes v1.2 \xD7\xA4\xD7\x93\xD7\x9C"), "not a capture");

    TestHost host(fs::temp_directory_path() / "guitarfx-folder-metadata-host");
    guitarfx::PluginController controller(host);

    nlohmann::json request;
    request["type"] = "listResourceFolder";
    request["path"] = guitarfx::util::PathToUtf8(folder);
    controller.HandleUIMessage(request.dump());

    const bool gotMetadata = host.WaitForMessageType("resourceFolderMetadata", std::chrono::seconds(5));

    if (const auto failed = host.LastMessageOfType("resourceFolderListingFailed"))
    {
        std::cerr << "Folder scan failed: " << failed->value("message", "") << "\n";
        return false;
    }

    const auto listing = host.LastMessageOfType("resourceFolderListing");
    const auto metadata = host.LastMessageOfType("resourceFolderMetadata");

    if (!gotMetadata || !listing || !metadata)
    {
        std::cerr << "Folder scan did not deliver a listing and its metadata\n";
        return false;
    }

    if ((*listing)["files"].size() != 2)
    {
        std::cerr << "Expected the two captures in the listing, got " << (*listing)["files"].dump() << "\n";
        return false;
    }

    std::map<std::string, nlohmann::json> metadataByName;

    for (const auto& item : (*metadata)["items"])
    {
        metadataByName[guitarfx::util::PathToUtf8(guitarfx::util::PathFromUtf8(item.value("path", "")).filename())] =
            item.value("metadata", nlohmann::json::object());
    }

    // The undecodable byte becomes U+FFFD; the rest of the name survives.
    const std::string latin1Name = metadataByName["Latin1 metadata.nam"].value("namName", "");
    // Escapes are decoded rather than shown as written or cut short at the escaped quote.
    const std::string escapedName = metadataByName["Escaped metadata.nam"].value("namName", "");
    const std::string escapedAuthor = metadataByName["Escaped metadata.nam"].value("modeledBy", "");

    const bool ok =
        latin1Name == "Caf\xEF\xBF\xBD Fuzz" && escapedName == "The \"Best\" Caf\xC3\xA9" && escapedAuthor == "A \\ B";

    if (!ok)
    {
        std::cerr << "Unexpected metadata: " << metadata->dump() << "\n";
    }

    return ok;
}

/// A model named outside plain ASCII keeps its name, imported and downloaded back. Both went through
/// path::string(), the ANSI code page on Windows: "é" came out as bytes JSON rejects, and "√" threw.
bool TestLocalImportPreservesUtf8Filename()
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-preview-workflow-tests" / "utf8-import";
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    SetSettingsEnvRoot(sandbox);

    const std::string stem = "\xC3\x9C"
                             "berdrive \xE2\x88\x9A \xE2\x80\x94 Caf\xC3\xA9";
    const fs::path source = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "amps" / "Guitar" / "A2" /
                            guitarfx::util::PathFromUtf8("A2 -wth space an \xE2\x80\x94 _emdash.nam");
    const fs::path model = sandbox / guitarfx::util::PathFromUtf8(stem + ".nam");

    if (!fs::copy_file(source, model, fs::copy_options::overwrite_existing, ec))
    {
        return false;
    }

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    controller.HandleUIMessage(nlohmann::json{
        {"type", "saveLocalLibraryResource"}, {"resourceType", "nam"}, {"filePath", guitarfx::util::PathToUtf8(model)}}
                                   .dump());

    for (const auto& resource : controller.GetResourceLibrary().GetResourcesByType("nam"))
    {
        if (resource.filePath != model)
        {
            continue;
        }

        const nlohmann::json download{{"type", "requestResourceData"},
                                      {"requestId", "utf8"},
                                      {"resourceType", "nam"},
                                      {"resourceId", resource.id}};
        controller.HandleUIMessage(download.dump());
        const auto data = host.LastMessageOfType("resourceData");
        const auto sourceFileName = resource.metadata.find("sourceFileName");
        return resource.name == stem && sourceFileName != resource.metadata.end() &&
               sourceFileName->second == stem + ".nam" && data && data->value("fileName", "") == stem + ".nam";
    }

    std::cerr << "The model was not imported\n";
    return false;
}
} // namespace

int main()
{
    int passed = 0;
    int failed = 0;

    const auto run = [&](const std::string& name, bool ok) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";

        if (ok)
        {
            ++passed;
        }
        else
        {
            ++failed;
        }
    };

    run("NAM preview apply/cancel", TestPreviewApplyAndCancel("nam"));
    run("IR preview apply/cancel", TestPreviewApplyAndCancel("ir"));
    run("Preview resourceIndex targeting", TestPreviewHonorsResourceIndex());
    run("Library preview close reverts", TestLibraryPreviewCloseRevertsOriginal());
    run("Preview missing data is no-op", TestPreviewMissingDataNoMutation());
    run("Preview missing nodeId is no-op", TestPreviewMissingNodeIdNoMutation());
    run("Cancel without preview is no-op", TestCancelWithoutActivePreviewNoMutation());
    run("Folder enumeration preserves UTF-8 filename", TestFolderEnumerationPreservesUtf8Filename());
    run("Folder listing survives unreadable NAM metadata", TestFolderListingSurvivesUnreadableNamMetadata());
    run("Local import preserves a UTF-8 filename", TestLocalImportPreservesUtf8Filename());

    std::cout << "\nResource preview workflow tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
