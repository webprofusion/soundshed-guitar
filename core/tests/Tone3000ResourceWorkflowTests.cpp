/**
 * Tone3000ResourceWorkflowTests.cpp - Importing and previewing downloaded resources.
 *
 * The Tone3000 tab hands the engine a model's bytes in two ways: `importRemoteResource` puts
 * them in the library, and `previewRemoteResource` plays them from a temp file until the
 * browser closes. These cover what those two must get right: the library's content hash and
 * cross-instance notification on import, never overwriting another resource's file of the same
 * name, zipped models, and the node's true original surviving one preview replacing another.
 */

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "MessageThreadTestHost.h"
#include "PluginController.h"
#include "models/ModelHasher.h"
#include "presets/PresetStorage.h"
#include "presets/PresetTypes.h"
#include "resources/ResourceLibrary.h"
#include "storage/JsonStore.h"
#include "util/Base64.h"
#include "util/PathEncoding.h"

namespace fs = std::filesystem;

namespace
{
using Bytes = std::vector<std::uint8_t>;

/// Records what the controller tells the UI, and runs main-thread work inline.
class RecordingHost final : public guitarfx::test::PumpedTestHost
{
  public:
    explicit RecordingHost(const fs::path& root) : PumpedTestHost(root, std::this_thread::get_id(), 48000.0, 512)
    {
    }

    void SendMessageToUI(const std::string& message) override
    {
        const std::lock_guard<std::mutex> lock(mMutex);
        mMessages.push_back(message);
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        fn();
    }

    [[nodiscard]] std::optional<nlohmann::json> LastMessageOfType(const std::string& type)
    {
        const std::lock_guard<std::mutex> lock(mMutex);

        for (auto it = mMessages.rbegin(); it != mMessages.rend(); ++it)
        {
            const auto parsed = nlohmann::json::parse(*it, nullptr, false);

            if (parsed.is_object() && parsed.value("type", "") == type)
            {
                return parsed;
            }
        }

        return std::nullopt;
    }

  private:
    std::mutex mMutex;
    std::vector<std::string> mMessages;
};

fs::path FreshSandbox(const std::string& name)
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-tone3000-workflow-tests" / name;
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    guitarfx::test::SetSettingsEnvRoot(sandbox);
    return sandbox;
}

Bytes BytesOf(const std::string& text)
{
    return Bytes(text.begin(), text.end());
}

Bytes ReadBytes(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

struct ZipEntry
{
    std::string name;
    Bytes data;
};

/// A zip as Tone3000 serves one: deflated entries, and a folder entry for each name ending "/".
Bytes BuildZip(const std::vector<ZipEntry>& entries)
{
    mz_zip_archive archive{};

    if (!mz_zip_writer_init_heap(&archive, 0, 0))
    {
        return {};
    }

    for (const auto& entry : entries)
    {
        mz_zip_writer_add_mem(&archive, entry.name.c_str(), entry.data.empty() ? nullptr : entry.data.data(),
                              entry.data.size(), MZ_DEFAULT_COMPRESSION);
    }

    void* buffer = nullptr;
    std::size_t size = 0;
    Bytes zip;

    if (mz_zip_writer_finalize_heap_archive(&archive, &buffer, &size))
    {
        zip.assign(static_cast<std::uint8_t*>(buffer), static_cast<std::uint8_t*>(buffer) + size);
    }

    mz_zip_writer_end(&archive);
    return zip;
}

void ImportRemote(guitarfx::PluginController& controller, const std::string& resourceType, const std::string& id,
                  const std::string& fileName, const Bytes& bytes, const std::string& claimedHash = {})
{
    nlohmann::json message{{"type", "importRemoteResource"},
                           {"requestId", "import-" + id},
                           {"provider", "tone3000"},
                           {"resourceType", resourceType},
                           {"resourceId", id},
                           {"name", id},
                           {"subfolder", "amp/Test Tone"},
                           {"fileName", fileName},
                           {"data", guitarfx::util::EncodeBase64(bytes)}};

    if (!claimedHash.empty())
    {
        message["hash"] = claimedHash;
    }

    controller.HandleUIMessage(message.dump());
}

/// The library entry, checked to exist on disk with a hash that is its file's.
std::optional<guitarfx::LibraryResource> ImportedResource(guitarfx::PluginController& controller,
                                                          const std::string& resourceType, const std::string& id)
{
    auto resource = controller.GetResourceLibrary().LookupResource(resourceType, id);

    if (!resource)
    {
        std::cerr << "  " << id << " is not in the library\n";
        return std::nullopt;
    }

    if (!fs::exists(resource->filePath))
    {
        std::cerr << "  " << id << " has no file\n";
        return std::nullopt;
    }

    if (resource->hash != guitarfx::ModelHasher{}.HashFile(resource->filePath))
    {
        std::cerr << "  " << id << " hash '" << resource->hash << "' is not its file's\n";
        return std::nullopt;
    }

    return resource;
}

/// The shared-sync document other instances poll, read through a connection of our own.
nlohmann::json ReadSharedSyncState(const fs::path& sandbox)
{
    for (const auto& entry : fs::recursive_directory_iterator(sandbox))
    {
        if (entry.path().filename() != "soundshed.db")
        {
            continue;
        }

        guitarfx::storage::JsonStore store;
        std::string error;

        if (!store.Open(entry.path(), error))
        {
            break;
        }

        return store.Get(guitarfx::storage::ItemType::kDocument, "shared-sync-state")
            .value_or(nlohmann::json::object());
    }

    return nlohmann::json::object();
}

bool TestRemoteImportHashesContentAndNotifiesOtherInstances()
{
    const fs::path sandbox = FreshSandbox("import-hash-sync");
    RecordingHost host(sandbox);
    guitarfx::PluginController controller(host);

    const auto versionBefore = ReadSharedSyncState(sandbox).value("version", std::uint64_t{0});
    const Bytes model = BytesOf("{\"version\":\"0.5.4\",\"architecture\":\"WaveNet\"}");
    // What libraryActions.ts sends: the browser's SHA-256, a different algorithm from the library's.
    ImportRemote(controller, "nam", "tone3000:101", "Clean.nam", model,
                 "b3a1f0e5c0d4e1a2b3c4d5e6f708192a3b4c5d6e7f8091a2b3c4d5e6f708192a");

    const auto resource = ImportedResource(controller, "nam", "tone3000:101");

    // ImportedResource() has already held the hash to the file's, so this makes it the content's.
    if (!resource || ReadBytes(resource->filePath) != model)
    {
        std::cerr << "  The import does not carry the library's own content hash\n";
        return false;
    }

    const auto imported = host.LastMessageOfType("resourceImported");

    if (!imported || imported->value("requestId", "") != "import-tone3000:101")
    {
        std::cerr << "  No resourceImported answer\n";
        return false;
    }

    const auto sync = ReadSharedSyncState(sandbox);
    const auto domains = sync.value("domains", nlohmann::json::array());

    if (sync.value("version", std::uint64_t{0}) <= versionBefore ||
        std::find(domains.begin(), domains.end(), "resourceLibrary") == domains.end())
    {
        std::cerr << "  The import did not bump shared sync for resourceLibrary: " << sync.dump() << "\n";
        return false;
    }

    return true;
}

bool TestRemoteImportNeverOverwritesAnotherResourcesFile()
{
    const fs::path sandbox = FreshSandbox("import-same-name");
    RecordingHost host(sandbox);
    guitarfx::PluginController controller(host);

    const Bytes first = BytesOf("first capture");
    const Bytes second = BytesOf("second capture");
    ImportRemote(controller, "ir", "tone3000:1", "Model.wav", first);
    ImportRemote(controller, "ir", "tone3000:2", "Model.wav", second);

    const auto a = ImportedResource(controller, "ir", "tone3000:1");
    const auto b = ImportedResource(controller, "ir", "tone3000:2");

    if (!a || !b)
    {
        return false;
    }

    if (a->filePath == b->filePath || ReadBytes(a->filePath) != first || ReadBytes(b->filePath) != second)
    {
        std::cerr << "  A second model of the same name overwrote the first one's file\n";
        return false;
    }

    const std::string bName = guitarfx::util::PathToUtf8(b->filePath.filename());

    if (bName.rfind("Model-", 0) != 0 || b->filePath.extension() != ".wav")
    {
        std::cerr << "  Unexpected name beside the first: " << bName << "\n";
        return false;
    }

    // Importing either again lands where it already is, not beside itself.
    ImportRemote(controller, "ir", "tone3000:2", "Model.wav", second);
    const auto bAgain = ImportedResource(controller, "ir", "tone3000:2");

    if (!bAgain || bAgain->filePath != b->filePath)
    {
        std::cerr << "  Re-importing the same content moved it\n";
        return false;
    }

    // A re-import with new content replaces the resource's own file in place.
    const Bytes updated = BytesOf("first capture, re-trained");
    ImportRemote(controller, "ir", "tone3000:1", "Model.wav", updated);
    const auto aUpdated = ImportedResource(controller, "ir", "tone3000:1");

    if (!aUpdated || aUpdated->filePath != a->filePath || ReadBytes(a->filePath) != updated)
    {
        std::cerr << "  A re-import did not update the resource's own file\n";
        return false;
    }

    // Identical bytes under another id still get a file of their own, since deleting either
    // library entry deletes its file.
    ImportRemote(controller, "ir", "tone3000:3", "Model.wav", updated);
    const auto c = ImportedResource(controller, "ir", "tone3000:3");

    if (!c || c->filePath == a->filePath || c->filePath == b->filePath || c->hash != aUpdated->hash)
    {
        std::cerr << "  Identical content under another id shares or clobbers a file\n";
        return false;
    }

    return true;
}

guitarfx::Preset BuildResourcePreset(const std::string& nodeId, const std::string& resourceType,
                                     const std::vector<std::string>& resourceIds)
{
    using namespace guitarfx;

    Preset preset;
    preset.id = "tone3000-preview";
    preset.name = "Tone3000 Preview";

    GraphNode input;
    input.id = "in";
    input.type = kNodeTypeInput;

    GraphNode effect;
    effect.id = nodeId;
    effect.type = "utility";
    effect.enabled = true;

    for (const auto& id : resourceIds)
    {
        ResourceRef ref;
        ref.resourceType = resourceType;
        ref.resourceId = id;
        effect.resources.push_back(ref);
    }

    GraphNode output;
    output.id = "out";
    output.type = kNodeTypeOutput;

    preset.graph.nodes = {input, effect, output};
    preset.graph.edges = {{"in", nodeId, 0, 0, 1.0}, {nodeId, "out", 0, 0, 1.0}};
    return preset;
}

bool LoadPreset(guitarfx::PluginController& controller, const guitarfx::Preset& preset)
{
    controller.HandleUIMessage(nlohmann::json{
        {"type", "loadPreset"}, {"preset", nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset))}}
                                   .dump());
    return controller.GetActivePreset().has_value();
}

std::optional<guitarfx::ResourceRef> SlotRef(guitarfx::PluginController& controller, const std::string& nodeId,
                                             std::size_t index)
{
    const auto& preset = controller.GetActivePreset();
    const auto* node = preset ? preset->graph.FindNode(nodeId) : nullptr;

    if (!node || index >= node->resources.size())
    {
        return std::nullopt;
    }

    return node->resources[index];
}

void Preview(guitarfx::PluginController& controller, const std::string& resourceType, const std::string& modelId,
             const Bytes& bytes, bool isZip, int resourceIndex = 0)
{
    controller.HandleUIMessage(nlohmann::json{{"type", "previewRemoteResource"},
                                              {"resourceType", resourceType},
                                              {"tempResourceId", "preview:tone3000:7:" + modelId},
                                              {"nodeId", "node"},
                                              {"resourceIndex", resourceIndex},
                                              {"isZip", isZip},
                                              {"data", guitarfx::util::EncodeBase64(bytes)}}
                                   .dump());
}

void CancelPreview(guitarfx::PluginController& controller, bool restoreOriginal, int resourceIndex = 0)
{
    const nlohmann::json cancel{{"type", "cancelPreviewResource"},
                                {"nodeId", "node"},
                                {"resourceIndex", resourceIndex},
                                {"restoreOriginal", restoreOriginal}};
    controller.HandleUIMessage(cancel.dump());
}

/// The slot plays a temp file holding `expected`.
std::optional<fs::path> SlotPlays(guitarfx::PluginController& controller, const Bytes& expected, std::size_t index = 0)
{
    const auto ref = SlotRef(controller, "node", index);

    if (!ref || !ref->resourceId.empty() || ref->filePath.empty() || !fs::exists(ref->filePath) ||
        ReadBytes(ref->filePath) != expected)
    {
        return std::nullopt;
    }

    return ref->filePath;
}

bool SlotHoldsLibraryResource(guitarfx::PluginController& controller, const std::string& resourceId,
                              std::size_t index = 0)
{
    const auto ref = SlotRef(controller, "node", index);
    return ref && ref->resourceId == resourceId && ref->filePath.empty();
}

bool TestZipPreviewPlaysTheFirstMatchingEntry(const std::string& resourceType)
{
    const fs::path sandbox = FreshSandbox("zip-preview-" + resourceType);
    RecordingHost host(sandbox);
    guitarfx::PluginController controller(host);

    if (!LoadPreset(controller, BuildResourcePreset("node", resourceType, {"original"})))
    {
        return false;
    }

    // Folders, other file types and the other resource type come first; the match is by
    // extension in any case, and the first match in the zip's order wins, as in the UI import.
    const Bytes expected = BytesOf(std::string(4096, 'x') + "first match");
    const std::vector<ZipEntry> entries = resourceType == "ir"
                                              ? std::vector<ZipEntry>{{"Cab/", {}},
                                                                      {"Cab/readme.txt", BytesOf("notes")},
                                                                      {"Cab/amp.nam", BytesOf("a model")},
                                                                      {"Cab/SM57 Close.WAV", expected},
                                                                      {"Cab/Room.wav", BytesOf("second match")}}
                                              : std::vector<ZipEntry>{{"Tone/", {}},
                                                                      {"Tone/readme.txt", BytesOf("notes")},
                                                                      {"Tone/cab.wav", BytesOf("an IR")},
                                                                      {"Tone/Gain 5.NAM", expected},
                                                                      {"Tone/Gain 9.nam", BytesOf("second match")}};
    Preview(controller, resourceType, "zipped", BuildZip(entries), true);

    const auto tempFile = SlotPlays(controller, expected);

    if (!tempFile)
    {
        std::cerr << "  The zip's first " << resourceType << " is not what the node plays\n";
        return false;
    }

    CancelPreview(controller, true);

    if (!SlotHoldsLibraryResource(controller, "original") || fs::exists(*tempFile))
    {
        std::cerr << "  Closing a zip preview did not restore the original and clean up\n";
        return false;
    }

    return true;
}

bool TestZipPreviewWithNothingPlayableLeavesTheNodeAlone()
{
    const fs::path sandbox = FreshSandbox("zip-preview-empty");
    RecordingHost host(sandbox);
    guitarfx::PluginController controller(host);

    if (!LoadPreset(controller, BuildResourcePreset("node", "nam", {"original"})))
    {
        return false;
    }

    Preview(controller, "nam", "no-model", BuildZip({{"readme.txt", BytesOf("notes")}, {"cab.wav", BytesOf("IR")}}),
            true);
    Preview(controller, "nam", "not-a-zip", BytesOf("definitely not a zip"), true);

    if (!SlotHoldsLibraryResource(controller, "original"))
    {
        std::cerr << "  A zip with no model changed the node\n";
        return false;
    }

    const auto error = host.LastMessageOfType("error");

    if (!error || error->value("message", "") != "Preview failed")
    {
        std::cerr << "  A zip with no model was not reported\n";
        return false;
    }

    return true;
}

bool TestConsecutivePreviewsRestoreTheTrueOriginal()
{
    const fs::path sandbox = FreshSandbox("consecutive-previews");
    RecordingHost host(sandbox);
    guitarfx::PluginController controller(host);

    if (!LoadPreset(controller, BuildResourcePreset("node", "nam", {"original"})))
    {
        return false;
    }

    const Bytes modelA = BytesOf("model A");
    const Bytes modelB = BytesOf("model B");
    Preview(controller, "nam", "a", modelA, false);
    const auto tempA = SlotPlays(controller, modelA);
    Preview(controller, "nam", "b", modelB, false);
    const auto tempB = SlotPlays(controller, modelB);

    if (!tempA || !tempB || *tempA == *tempB)
    {
        std::cerr << "  The second preview is not what the node plays\n";
        return false;
    }

    if (fs::exists(*tempA))
    {
        std::cerr << "  The replaced preview's temp file was left behind\n";
        return false;
    }

    // The same model again reuses its temp file, which must survive being replaced by itself.
    Preview(controller, "nam", "b", modelB, false);

    if (SlotPlays(controller, modelB) != tempB)
    {
        std::cerr << "  Previewing the same model again lost its file\n";
        return false;
    }

    // A download that fails to yield a model keeps the one playing.
    Preview(controller, "nam", "broken", BytesOf("not a zip"), true);

    if (SlotPlays(controller, modelB) != tempB)
    {
        std::cerr << "  A failed preview replaced the one playing\n";
        return false;
    }

    CancelPreview(controller, true);

    if (!SlotHoldsLibraryResource(controller, "original"))
    {
        const auto ref = SlotRef(controller, "node", 0);
        std::cerr << "  Closing restored '" << (ref ? ref->resourceId : "") << "' at '"
                  << (ref ? guitarfx::util::PathToUtf8(ref->filePath) : "") << "' instead of the original\n";
        return false;
    }

    if (fs::exists(*tempB))
    {
        std::cerr << "  Closing left the preview's temp file behind\n";
        return false;
    }

    return true;
}

/// Provider titles run to 170 characters; the import writes a short name and subfolder so the
/// path stays under Windows' 260-character limit, keeping the extension.
bool TestRemoteImportShortensLongNames()
{
    const fs::path sandbox = FreshSandbox("import-long-names");
    RecordingHost host(sandbox);
    guitarfx::PluginController controller(host);

    const std::string longTitle(170, 'x');
    nlohmann::json message{{"type", "importRemoteResource"},
                           {"requestId", "import-long"},
                           {"provider", "tone3000"},
                           {"resourceType", "nam"},
                           {"resourceId", "tone3000:202"},
                           {"name", "Long"},
                           {"subfolder", "amp/" + longTitle},
                           {"fileName", "Fender Vibroverb 1964 " + longTitle + ".nam"},
                           {"data", guitarfx::util::EncodeBase64(BytesOf("model"))}};
    controller.HandleUIMessage(message.dump());

    const auto resource = ImportedResource(controller, "nam", "tone3000:202");

    if (!resource)
    {
        std::cerr << "  A long name was not imported\n";
        return false;
    }

    const auto fileName = resource->filePath.filename().string();
    const auto folder = resource->filePath.parent_path().filename().string();

    if (fileName.size() > 72 || fileName.rfind(".nam") != fileName.size() - 4 ||
        fileName.rfind("Fender_Vibroverb_1964", 0) != 0 || folder.size() > 40)
    {
        std::cerr << "  The import wrote " << folder << "/" << fileName << "\n";
        return false;
    }

    return true;
}

bool TestPreviewOnAnotherSlotRestoresTheFirst()
{
    const fs::path sandbox = FreshSandbox("preview-other-slot");
    RecordingHost host(sandbox);
    guitarfx::PluginController controller(host);

    if (!LoadPreset(controller, BuildResourcePreset("node", "nam", {"slot-0", "slot-1"})))
    {
        return false;
    }

    const Bytes modelA = BytesOf("model A");
    const Bytes modelB = BytesOf("model B");
    Preview(controller, "nam", "a", modelA, false, 0);
    const auto tempA = SlotPlays(controller, modelA, 0);
    Preview(controller, "nam", "b", modelB, false, 1);

    if (!tempA || !SlotHoldsLibraryResource(controller, "slot-0", 0) || fs::exists(*tempA))
    {
        std::cerr << "  A preview on another slot left the first slot on its preview\n";
        return false;
    }

    if (!SlotPlays(controller, modelB, 1))
    {
        std::cerr << "  The second slot is not previewing\n";
        return false;
    }

    CancelPreview(controller, true, 1);
    return SlotHoldsLibraryResource(controller, "slot-0", 0) && SlotHoldsLibraryResource(controller, "slot-1", 1);
}

/// A Tone3000 import as the UI's Select sends one: provider fields in the metadata.
void ImportTone3000Model(guitarfx::PluginController& controller, const std::string& id, const std::string& name,
                         const std::string& toneId, const std::string& modelId, const Bytes& bytes)
{
    controller.HandleUIMessage(nlohmann::json{{"type", "importRemoteResource"},
                                              {"requestId", "import-" + id},
                                              {"provider", "tone3000"},
                                              {"resourceType", "nam"},
                                              {"resourceId", id},
                                              {"name", name},
                                              {"subfolder", "amp/Test Tone"},
                                              {"fileName", modelId + ".nam"},
                                              {"metadata",
                                               {{"provider", "tone3000"},
                                                {"providerKey", modelId},
                                                {"toneId", toneId},
                                                {"modelId", modelId},
                                                {"creatorName", "Someone"}}},
                                              {"data", guitarfx::util::EncodeBase64(bytes)}}
                                   .dump());
}

/// The first resource of node "node" in a host state blob's focused preset.
std::optional<nlohmann::json> SavedSlot(const std::string& state)
{
    const auto parsed = nlohmann::json::parse(state, nullptr, false);

    if (!parsed.is_object() || !parsed.contains("preset"))
    {
        return std::nullopt;
    }

    // A preset with scenes is saved as its scenes' graphs alone; every one must agree.
    const auto& preset = parsed["preset"];
    std::vector<nlohmann::json> graphs;

    if (preset.contains("graph"))
    {
        graphs.push_back(preset["graph"]);
    }

    for (const auto& scene : preset.value("scenes", nlohmann::json::array()))
    {
        graphs.push_back(scene.value("graph", nlohmann::json::object()));
    }

    std::optional<nlohmann::json> found;

    for (const auto& graph : graphs)
    {
        for (const auto& node : graph.value("nodes", nlohmann::json::array()))
        {
            if (node.value("id", "") != "node" || !node.contains("resources") || node["resources"].empty())
            {
                continue;
            }

            if (found && *found != node["resources"][0])
            {
                std::cerr << "  Scenes disagree on the slot: " << found->dump() << " vs "
                          << node["resources"][0].dump() << "\n";
                return std::nullopt;
            }

            found = node["resources"][0];
        }
    }

    return found;
}

/// Host state the way a project two months old holds it: the slot names a library id this
/// library does not have, with the source that was saved beside it.
std::string StateWithSlot(const nlohmann::json& slot)
{
    guitarfx::Preset preset = BuildResourcePreset("node", "nam", {"placeholder"});
    nlohmann::json presetJson = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
    nlohmann::json state{{"version", 1}, {"presetId", preset.id}};

    const auto replaceSlot = [&](nlohmann::json& nodes) {
        for (auto& node : nodes)
        {
            if (node.value("id", "") == "node")
            {
                node["resources"] = nlohmann::json::array({slot});
            }
        }
    };

    if (presetJson.contains("graph") && presetJson["graph"].contains("nodes"))
    {
        replaceSlot(presetJson["graph"]["nodes"]);
    }

    if (presetJson.contains("scenes"))
    {
        for (auto& scene : presetJson["scenes"])
        {
            if (scene.contains("graph") && scene["graph"].contains("nodes"))
            {
                replaceSlot(scene["graph"]["nodes"]);
            }
        }
    }

    state["preset"] = presetJson;
    return state.dump();
}

bool TestHostStateNeverSavesAPreviewTempFile()
{
    const fs::path sandbox = FreshSandbox("state-skips-preview");
    RecordingHost host(sandbox);
    guitarfx::PluginController controller(host);

    ImportTone3000Model(controller, "tone3000:1", "Tone - Chosen", "7", "1", BytesOf("chosen model"));

    if (!LoadPreset(controller, BuildResourcePreset("node", "nam", {"tone3000:1"})))
    {
        return false;
    }

    Preview(controller, "nam", "2", BytesOf("previewed model"), false);

    if (!SlotPlays(controller, BytesOf("previewed model")))
    {
        std::cerr << "  The preview is not playing\n";
        return false;
    }

    const auto slot = SavedSlot(controller.SerializeState());

    if (!slot || slot->value("resourceId", "") != "tone3000:1" || slot->contains("filePath"))
    {
        std::cerr << "  Saved slot is not the original: " << (slot ? slot->dump() : "none") << "\n";
        return false;
    }

    // Saved with what the library knows of it, to name and fetch it again.
    const auto source = slot->value("source", nlohmann::json::object());

    if (source.value("name", "") != "Tone - Chosen" || source.value("provider", "") != "tone3000" ||
        source.value("toneId", "") != "7" || source.value("modelId", "") != "1")
    {
        std::cerr << "  Saved source is wrong: " << source.dump() << "\n";
        return false;
    }

    return true;
}

bool TestClosingTheEditorKeepsThePreview()
{
    const fs::path sandbox = FreshSandbox("editor-close-keeps-preview");
    RecordingHost host(sandbox);
    guitarfx::PluginController controller(host);

    if (!LoadPreset(controller, BuildResourcePreset("node", "nam", {"original"})))
    {
        return false;
    }

    const Bytes model = BytesOf(std::string(2048, 'k') + "kept model");
    controller.HandleUIMessage(nlohmann::json{{"type", "previewRemoteResource"},
                                              {"resourceType", "nam"},
                                              {"tempResourceId", "preview:tone3000:7:42"},
                                              {"nodeId", "node"},
                                              {"resourceIndex", 0},
                                              {"isZip", false},
                                              {"data", guitarfx::util::EncodeBase64(model)},
                                              {"keepAs",
                                               {{"provider", "tone3000"},
                                                {"resourceType", "nam"},
                                                {"resourceId", "tone3000:42"},
                                                {"name", "Test Tone - Kept"},
                                                {"category", "amp"},
                                                {"subfolder", "amp/Test Tone"},
                                                {"fileName", "Kept.nam"},
                                                {"metadata",
                                                 {{"provider", "tone3000"},
                                                  {"providerKey", "42"},
                                                  {"toneId", "7"},
                                                  {"modelId", "42"}}}}}}
                                   .dump());

    const auto tempFile = SlotPlays(controller, model);

    if (!tempFile)
    {
        std::cerr << "  The preview is not playing\n";
        return false;
    }

    controller.OnEditorClosed();

    if (!SlotHoldsLibraryResource(controller, "tone3000:42"))
    {
        std::cerr << "  The slot is not on the kept model\n";
        return false;
    }

    const auto kept = ImportedResource(controller, "nam", "tone3000:42");

    if (!kept || ReadBytes(kept->filePath) != model || kept->name != "Test Tone - Kept")
    {
        std::cerr << "  The kept model is not in the library as previewed\n";
        return false;
    }

    if (fs::exists(*tempFile))
    {
        std::cerr << "  The preview's temp file is still there\n";
        return false;
    }

    const auto slot = SavedSlot(controller.SerializeState());

    if (!slot || slot->value("resourceId", "") != "tone3000:42" ||
        slot->value("source", nlohmann::json::object()).value("modelId", "") != "42")
    {
        std::cerr << "  Saved slot is not the kept model: " << (slot ? slot->dump() : "none") << "\n";
        return false;
    }

    // Closed again with nothing previewing: nothing changes.
    controller.OnEditorClosed();
    return SlotHoldsLibraryResource(controller, "tone3000:42");
}

bool TestClosingTheEditorWithoutKeepAsLeavesThePreview()
{
    const fs::path sandbox = FreshSandbox("editor-close-no-keep");
    RecordingHost host(sandbox);
    guitarfx::PluginController controller(host);

    if (!LoadPreset(controller, BuildResourcePreset("node", "nam", {"original"})))
    {
        return false;
    }

    Preview(controller, "nam", "9", BytesOf("an older UI's preview"), false);
    controller.OnEditorClosed();

    // Nothing to import it as: the library is untouched and the project still saves the original.
    if (controller.GetResourceLibrary().HasResource("nam", "tone3000:9"))
    {
        std::cerr << "  A preview without keepAs went into the library\n";
        return false;
    }

    const auto slot = SavedSlot(controller.SerializeState());
    return slot && slot->value("resourceId", "") == "original" && !slot->contains("filePath");
}

bool TestRestoreFindsAMissingModelBySource()
{
    const fs::path sandbox = FreshSandbox("restore-by-source");
    RecordingHost host(sandbox);
    guitarfx::PluginController controller(host);

    // Downloaded again since, under a newer id.
    ImportTone3000Model(controller, "tone3000:42:Model.nam", "Test Tone - Model", "7", "42", BytesOf("model"));

    const nlohmann::json slot{{"resourceType", "nam"},
                              {"resourceId", "tone3000:old-id"},
                              {"source",
                               {{"name", "Test Tone - Model"},
                                {"provider", "tone3000"},
                                {"providerKey", "42"},
                                {"toneId", "7"},
                                {"modelId", "42"}}}};

    if (!controller.DeserializeState(StateWithSlot(slot)))
    {
        std::cerr << "  Restore failed\n";
        return false;
    }

    if (!SlotHoldsLibraryResource(controller, "tone3000:42:Model.nam"))
    {
        const auto ref = SlotRef(controller, "node", 0);
        std::cerr << "  Slot was not pointed at the copy: " << (ref ? ref->resourceId : "none") << "\n";
        return false;
    }

    return true;
}

bool TestSourceSurvivesWhileTheModelIsMissing()
{
    const fs::path sandbox = FreshSandbox("source-survives");
    RecordingHost host(sandbox);
    guitarfx::PluginController controller(host);

    const nlohmann::json source{{"name", "Gone Tone - Model"},
                                {"provider", "tone3000"},
                                {"providerKey", "77"},
                                {"toneId", "8"},
                                {"modelId", "77"}};
    const nlohmann::json slot{{"resourceType", "nam"}, {"resourceId", "tone3000:77"}, {"source", source}};

    if (!controller.DeserializeState(StateWithSlot(slot)))
    {
        std::cerr << "  Restore failed\n";
        return false;
    }

    // Saved again before anything is fetched: the project must not forget what it knew.
    const auto saved = SavedSlot(controller.SerializeState());

    if (!saved || saved->value("resourceId", "") != "tone3000:77" ||
        saved->value("source", nlohmann::json::object()) != source)
    {
        std::cerr << "  Saved slot lost its source: " << (saved ? saved->dump() : "none") << "\n";
        return false;
    }

    // Once the model is back under its id, the next save records the library's own source.
    ImportTone3000Model(controller, "tone3000:77", "Gone Tone - Model (renamed)", "8", "77", BytesOf("back again"));
    const auto resaved = SavedSlot(controller.SerializeState());

    return resaved && resaved->value("source", nlohmann::json::object()).value("name", "") ==
                          "Gone Tone - Model (renamed)";
}

bool TestLibraryResolvesALostIdBySource()
{
    const fs::path sandbox = FreshSandbox("library-resolve-by-source");
    const fs::path file = sandbox / "copy.nam";
    {
        std::ofstream out(file, std::ios::binary);
        out << "model";
    }

    guitarfx::ResourceLibrary library;
    guitarfx::LibraryResource zipEntryA;
    zipEntryA.type = "nam";
    zipEntryA.id = "tone3000:5:a.nam";
    zipEntryA.filePath = sandbox / "missing-a.nam";
    zipEntryA.metadata = {{"provider", "tone3000"}, {"providerKey", "5:a.nam"}, {"toneId", "1"}, {"modelId", "5"}};
    library.AddResource(zipEntryA);

    guitarfx::LibraryResource zipEntryB = zipEntryA;
    zipEntryB.id = "tone3000:5:b.nam";
    zipEntryB.filePath = file;
    zipEntryB.metadata["providerKey"] = "5:b.nam";
    library.AddResource(zipEntryB);

    guitarfx::ResourceRef ref;
    ref.resourceType = "nam";
    ref.resourceId = "tone3000:gone";
    ref.source = {{"provider", "tone3000"}, {"providerKey", "5:b.nam"}, {"toneId", "1"}, {"modelId", "5"}};

    const auto resolved = library.ResolveResource(ref);

    if (!resolved || *resolved != file)
    {
        std::cerr << "  A lost id did not resolve by provider key\n";
        return false;
    }

    // Entry a's key: its own file is gone, and entry b shares its tone and model ids, but is
    // another model of the zip. Nothing is found rather than the wrong model.
    ref.source["providerKey"] = "5:a.nam";

    if (library.FindResourceBySource("nam", ref.source))
    {
        std::cerr << "  A zip entry was matched to its sibling\n";
        return false;
    }

    return true;
}
} // namespace

int main()
{
    int passed = 0;
    int failed = 0;

    const auto run = [&](const char* name, bool ok) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";
        (ok ? passed : failed)++;
    };

    run("Remote import hashes content and notifies other instances",
        TestRemoteImportHashesContentAndNotifiesOtherInstances());
    run("Remote import never overwrites another resource's file",
        TestRemoteImportNeverOverwritesAnotherResourcesFile());
    run("Zip preview plays the first NAM entry", TestZipPreviewPlaysTheFirstMatchingEntry("nam"));
    run("Zip preview plays the first IR entry", TestZipPreviewPlaysTheFirstMatchingEntry("ir"));
    run("Zip preview with nothing playable leaves the node alone",
        TestZipPreviewWithNothingPlayableLeavesTheNodeAlone());
    run("Consecutive previews restore the true original", TestConsecutivePreviewsRestoreTheTrueOriginal());
    run("Preview on another slot restores the first", TestPreviewOnAnotherSlotRestoresTheFirst());
    run("Remote import shortens long names", TestRemoteImportShortensLongNames());
    run("Host state never saves a preview's temp file", TestHostStateNeverSavesAPreviewTempFile());
    run("Closing the editor keeps the preview", TestClosingTheEditorKeepsThePreview());
    run("Closing the editor without keepAs leaves the preview", TestClosingTheEditorWithoutKeepAsLeavesThePreview());
    run("Restore finds a missing model by its source", TestRestoreFindsAMissingModelBySource());
    run("Source survives while the model is missing", TestSourceSurvivesWhileTheModelIsMissing());
    run("Library resolves a lost id by source", TestLibraryResolvesALostIdBySource());

    std::cout << "\nTone3000 resource workflow tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
