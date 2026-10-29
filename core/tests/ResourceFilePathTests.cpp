/**
 * ResourceFilePathTests.cpp - Library files whose names carry their content hash twice, and files
 * whose paths pass Windows' MAX_PATH.
 *
 * Before June 2026 the web UI's archive export prefixed "<sha256>-" onto a library file name that
 * an earlier install had already prefixed, so an archive could name a file
 * "<sha256>-<sha256>-<name>". Preset Pack Vol. 1 shipped four such files until July, and under the
 * profile folder two of its models sat at 282 and 292 characters, which Windows reports as missing
 * unless the path carries the \\?\ prefix. The install paths now drop the repeat, startup renames
 * the files older installs left, and model and IR files are opened long-path safe.
 */

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <miniz.h>
#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "dsp/EffectGuids.h"
#include "dsp/EffectRegistry.h"
#include "dsp/IRWavLoader.h"
#include "dsp/NamModelCache.h"
#include "dsp/effects/BuiltinEffects.h"
#include "presets/PresetStorage.h"
#include "resources/ResourceLibrary.h"
#include "storage/JsonStore.h"
#include "util/Base64.h"
#include "util/FileIO.h"
#include "util/LongPath.h"
#include "util/PathEncoding.h"
#include "util/PathSanitizer.h"

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace
{
class TestHost final : public guitarfx::IPluginHost
{
  public:
    explicit TestHost(fs::path root) : mRoot(root), mBundled(std::move(root))
    {
    }

    TestHost(fs::path root, fs::path bundled) : mRoot(std::move(root)), mBundled(std::move(bundled))
    {
    }

    void SendMessageToUI(const std::string& json) override
    {
        sent.push_back(json);
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
        return mBundled;
    }

    [[nodiscard]] double GetSampleRate() const override
    {
        return 48000.0;
    }

    [[nodiscard]] int GetBlockSize() const override
    {
        return 256;
    }

    [[nodiscard]] bool IsStandalone() const override
    {
        return true;
    }

    std::vector<std::string> sent;

  private:
    fs::path mRoot;
    fs::path mBundled;
};

int gFailures = 0;

void Expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << std::endl;
        ++gFailures;
    }
}

void Send(guitarfx::PluginController& controller, const std::string& type, nlohmann::json payload = {})
{
    if (!payload.is_object())
    {
        payload = nlohmann::json::object();
    }

    payload["type"] = type;
    controller.HandleUIMessage(payload.dump());
}

std::vector<nlohmann::json> Messages(const TestHost& host, const std::string& type)
{
    std::vector<nlohmann::json> found;

    for (const auto& text : host.sent)
    {
        auto parsed = nlohmann::json::parse(text, nullptr, false);

        if (parsed.is_object() && parsed.value("type", std::string{}) == type)
        {
            found.push_back(std::move(parsed));
        }
    }

    return found;
}

/// A fresh profile root for one test, unique to this run: another session's tests share %TEMP%.
fs::path RunRoot()
{
    static const auto run = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000000);
    return fs::temp_directory_path() / ("guitarfx-resource-file-path-tests-" + run);
}

/// Removes `root` and everything under it, the files past MAX_PATH included: remove_all builds each
/// child path onto the root it is given, so the root itself must carry the \\?\ prefix.
void RemoveTree(const fs::path& root)
{
    std::error_code ec;
#ifdef _WIN32
    fs::remove_all(fs::path(L"\\\\?\\" + fs::absolute(root, ec).lexically_normal().native()), ec);
#else
    fs::remove_all(root, ec);
#endif
}

fs::path Sandbox(const std::string& name)
{
    const auto root = RunRoot() / name;
    RemoveTree(root);
    std::error_code ec;
    fs::create_directories(root, ec);
#ifdef _WIN32
    _putenv_s("APPDATA", root.string().c_str());
#else
    setenv("HOME", root.string().c_str(), 1);
#endif
    return root;
}

fs::path ResourcesRoot(const fs::path& sandbox)
{
    return sandbox / "Soundshed Guitar" / "data" / "v1" / "resources";
}

fs::path StorePath(const fs::path& sandbox)
{
    return sandbox / "Soundshed Guitar" / "data" / "v1" / "soundshed.db";
}

/// Writes through the extended-length form, so a test can put a file past MAX_PATH.
void WriteBytes(const fs::path& path, const Bytes& bytes)
{
    std::error_code ec;
    fs::create_directories(guitarfx::util::LongPathSafe(path.parent_path()), ec);
    std::ofstream out(guitarfx::util::LongPathSafe(path), std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

Bytes ToBytes(const std::string& text)
{
    return Bytes(text.begin(), text.end());
}

Bytes Fixture(const fs::path& relative)
{
    return guitarfx::util::ReadFileBytes(fs::path(GUITARFX_TEST_RESOURCES_DIR) / relative);
}

Bytes BuildZip(const std::vector<std::pair<std::string, Bytes>>& entries)
{
    mz_zip_archive archive{};

    if (!mz_zip_writer_init_heap(&archive, 0, 0))
    {
        return {};
    }

    for (const auto& [name, data] : entries)
    {
        mz_zip_writer_add_mem(&archive, name.c_str(), data.empty() ? nullptr : data.data(), data.size(),
                              MZ_DEFAULT_COMPRESSION);
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

/// Input, one NAM node playing `resourceId`, output.
nlohmann::json PresetJson(const std::string& id, const std::string& resourceId)
{
    guitarfx::Preset preset;
    preset.id = id;
    preset.name = id;
    preset.category = "Shared";
    preset.version = 2;

    guitarfx::GraphNode input;
    input.id = "__input__";
    input.type = guitarfx::kNodeTypeInput;
    guitarfx::GraphNode amp;
    amp.id = "amp_1";
    amp.type = "amp_nam_optimized";
    amp.resources.push_back({"nam", resourceId});
    guitarfx::GraphNode output;
    output.id = "__output__";
    output.type = guitarfx::kNodeTypeOutput;
    preset.graph.nodes = {input, amp, output};
    preset.graph.edges = {guitarfx::GraphEdge{"__input__", "amp_1", 0, 0, 1.0},
                          guitarfx::GraphEdge{"amp_1", "__output__", 0, 0, 1.0}};
    return nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
}

// Four distinct SHA-256-shaped prefixes. The engine never checks them against the bytes.
const std::string kHashA = "687b9b24f986fb0f02aba1d0d1a6dcab8086bb87030d4e5b12f4259234cadd05";
const std::string kHashB = "7dc2f69a0e9d47b4b2710181a60ebbd7ec1d79434be3c45c779d57e3196ef978";
const std::string kHashC = "50656f74a3f67aed28ff01912d0eb8bb66f3b8b769f7b56f6fbce04dd2096182";
const std::string kHashD = "5bf150fa2e48402140bfb15766aa0b6373a646fc12f33d98991ab6996bd99720";

const Bytes kModelA = ToBytes(
    R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":2,"bias":false},"weights":[1.0,0.0]})");
const Bytes kModelB = ToBytes(
    R"({"version":"0.5.0","architecture":"Linear","config":{"receptive_field":2,"bias":false},"weights":[0.5,0.0]})");

void TestCollapseRepeatedHashPrefix()
{
    using guitarfx::util::CollapseRepeatedHashPrefix;
    std::string upper = kHashA;
    std::transform(upper.begin(), upper.end(), upper.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });

    Expect(CollapseRepeatedHashPrefix(kHashA + "-" + kHashA + "-Dumble.nam") == kHashA + "-Dumble.nam",
           "a doubled prefix becomes one");
    Expect(CollapseRepeatedHashPrefix(kHashA + "-" + kHashA + "-" + kHashA + "-Dumble.nam") == kHashA + "-Dumble.nam",
           "so does a tripled one");
    Expect(CollapseRepeatedHashPrefix(kHashA + "-" + upper + "-Dumble.nam") == kHashA + "-Dumble.nam",
           "the repeat matches whatever its case");
    Expect(CollapseRepeatedHashPrefix(kHashA + "-Dumble.nam") == kHashA + "-Dumble.nam", "a single prefix stays");
    Expect(CollapseRepeatedHashPrefix(kHashA + "-" + kHashB + "-Dumble.nam") == kHashA + "-" + kHashB + "-Dumble.nam",
           "two different hashes are not a repeat");
    Expect(CollapseRepeatedHashPrefix(kHashA + "-" + kHashA + ".nam") == kHashA + "-" + kHashA + ".nam",
           "a name that is only the hash keeps it");
    Expect(CollapseRepeatedHashPrefix("Dumble-Overdrive.nam") == "Dumble-Overdrive.nam", "a plain name stays");
    Expect(CollapseRepeatedHashPrefix("deadbeef-deadbeef-Amp.nam") == "deadbeef-deadbeef-Amp.nam",
           "a short hex prefix is not a content hash");
    Expect(CollapseRepeatedHashPrefix("") == "", "an empty name stays empty");
}

void TestLongPathSafe()
{
    using guitarfx::util::LongPathSafe;
    const fs::path shortPath = fs::temp_directory_path() / "model.nam";
    Expect(LongPathSafe(shortPath) == shortPath, "a short path is handed over as it is");
    Expect(LongPathSafe(fs::path("relative") / std::string(300, 'r') / "model.nam") ==
               fs::path("relative") / std::string(300, 'r') / "model.nam",
           "a relative path is handed over as it is");

#ifdef _WIN32
    const std::wstring deep(300, L'd');
    const auto longPath = fs::path(L"C:/Users/someone/" + deep + L"/../" + deep + L"/model.nam");
    Expect(LongPathSafe(longPath).native() == L"\\\\?\\C:\\Users\\someone\\" + deep + L"\\model.nam",
           "a long path gets \\\\?\\, backslashes and its .. resolved");
    Expect(LongPathSafe(LongPathSafe(longPath)) == LongPathSafe(longPath), "a prefixed path is left alone");
    Expect(LongPathSafe(fs::path(L"\\\\server\\share\\" + deep + L"\\ir.wav")).native() ==
               L"\\\\?\\UNC\\server\\share\\" + deep + L"\\ir.wav",
           "a long share path gets \\\\?\\UNC\\");
#else
    const fs::path deepPath = fs::path("/home/someone") / std::string(300, 'd') / "model.nam";
    Expect(LongPathSafe(deepPath) == deepPath, "other platforms have no MAX_PATH to work around");
#endif
}

/// A pack exported before the fix: the archive's own fileName repeats the hash.
void TestFactoryArchiveDropsTheRepeat()
{
    const auto root = Sandbox("factory");
    const std::string doubled = kHashA + "-" + kHashA + "-Dumble-Overdrive.nam";
    const nlohmann::json index = {
        {"formatVersion", 1},
        {"presets", {PresetJson("gravity-storm", "e2b2c5ca")}},
        {"resources",
         {{{"id", "e2b2c5ca"},
           {"name", "Dumble"},
           {"category", "amp"},
           {"type", "nam"},
           {"fileName", doubled},
           {"hash", kHashA}}}},
        {"blends", nlohmann::json::array()},
    };
    WriteBytes(root / "ui" / "presets" / "factory" / "Old-Pack.soundshed.presets",
               BuildZip({{"presets.json", ToBytes(index.dump())}, {"resources/" + doubled, kModelA}}));

    TestHost host(root);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const auto resource = controller.GetResourceLibrary().LookupResource("nam", "Old-Pack__e2b2c5ca");
    Expect(resource.has_value(), "the factory model is in the library");

    if (resource)
    {
        Expect(guitarfx::util::PathToUtf8(resource->filePath.filename()) == kHashA + "-Dumble-Overdrive.nam",
               "it is extracted under a single hash prefix");
        Expect(fs::exists(resource->filePath), "and the file is there");
    }

    std::error_code ec;
    Expect(!fs::exists(ResourcesRoot(root) / "content" / "factory-archives" / "Old-Pack" / doubled, ec),
           "nothing is written under the doubled name");
}

/// A relaunch re-imports the factory archives; what the library learned about a model since (its
/// gear, architecture and calibration) must not be dropped for the archive's own few keys.
void TestFactoryArchiveKeepsKnownMetadata()
{
    const auto root = Sandbox("factory-metadata");
    const nlohmann::json index = {
        {"formatVersion", 1},
        {"presets", {PresetJson("gravity-storm", "e2b2c5ca")}},
        {"resources",
         {{{"id", "e2b2c5ca"},
           {"name", "Dumble"},
           {"category", "amp"},
           {"type", "nam"},
           {"fileName", kHashA + "-Dumble.nam"},
           {"hash", kHashA}}}},
        {"blends", nlohmann::json::array()},
    };
    WriteBytes(root / "ui" / "presets" / "factory" / "Meta-Pack.soundshed.presets",
               BuildZip({{"presets.json", ToBytes(index.dump())}, {"resources/" + kHashA + "-Dumble.nam", kModelA}}));

    {
        TestHost host(root);
        guitarfx::PluginController controller(host);
        controller.Initialize();
        Send(controller, "updateLibraryResource",
             {{"resourceType", "nam"},
              {"resourceId", "Meta-Pack__e2b2c5ca"},
              {"metadata", {{"gearModel", "Overdrive Special"}, {"inputLevelDbu", "16"}}}});
    }

    TestHost host(root);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    const auto resource = controller.GetResourceLibrary().LookupResource("nam", "Meta-Pack__e2b2c5ca");
    Expect(resource.has_value(), "the factory model is in the library after a relaunch");

    if (resource)
    {
        const auto& meta = resource->metadata;
        Expect(meta.count("gearModel") == 1 && meta.at("gearModel") == "Overdrive Special",
               "it keeps what the library knew of the model");
        Expect(meta.count("inputLevelDbu") == 1 && meta.at("inputLevelDbu") == "16", "and its calibration");
        Expect(meta.count("factoryArchiveKey") == 1 && meta.at("factoryArchiveKey") == "Meta-Pack",
               "beside the archive's own keys");
    }
}

/// A tone shared from such an export, installed through installPresetArchives.
void TestToneSharingInstallDropsTheRepeat()
{
    const auto root = Sandbox("tone-sharing");
    TestHost host(root);
    guitarfx::PluginController controller(host);
    controller.Initialize();

    const std::string doubled = kHashB + "-" + kHashB + "-a.nam";
    const nlohmann::json index = {
        {"formatVersion", 1},
        {"preset", PresetJson("shared-1", "r1")},
        {"resources",
         {{{"id", "r1"},
           {"name", "Shared Model"},
           {"category", "amp"},
           {"type", "nam"},
           {"fileName", doubled},
           {"hash", kHashB}}}},
        {"blends", nlohmann::json::array()},
    };
    const auto zip = BuildZip({{"preset.json", ToBytes(index.dump())}, {"resources/" + doubled, kModelB}});
    Send(controller, "installPresetArchives",
         {{"requestId", "r1"},
          {"entry", {{"id", "tone-sharing-api:item:item-1"}, {"title", "Shared"}, {"source", "toneSharingApi"}}},
          {"archives", {{{"data", guitarfx::util::EncodeBase64(zip)}, {"title", "Shared"}}}}});
    Expect(!Messages(host, "presetArchivesInstalled").empty(), "the tone installs");

    bool found = false;

    for (const auto& resource : controller.GetResourceLibrary().GetAllResources())
    {
        if (resource.type == "nam" && resource.metadata.count("sourceFile") &&
            resource.metadata.at("sourceFile") == doubled)
        {
            found = true;
            Expect(guitarfx::util::PathToUtf8(resource.filePath.filename()) == kHashB + "-a.nam",
                   "the installed model keeps one hash prefix and its own name");
        }
    }

    Expect(found, "the installed model is in the library");
}

guitarfx::LibraryResource Entry(const std::string& id, const fs::path& file)
{
    guitarfx::LibraryResource resource;
    resource.type = "nam";
    resource.id = id;
    resource.name = id;
    resource.category = "amp";
    resource.filePath = file;
    // Read already, so the startup metadata backfill leaves these rows alone.
    resource.metadata["namFileVersion"] = "0.5.0";
    return resource;
}

/// What an older build left: entries pointing at doubled names under the profile's content folder.
void TestStartupRenamesDoubledFiles()
{
    const auto root = Sandbox("migrate");
    const auto pack = ResourcesRoot(root) / "content" / "factory-archives" / "Old-Pack";
    const auto alone = pack / (kHashA + "-" + kHashA + "-Alone.nam");
    const auto twinDoubled = pack / (kHashB + "-" + kHashB + "-Twin.nam");
    const auto twin = pack / (kHashB + "-Twin.nam");
    const auto clashDoubled = pack / (kHashC + "-" + kHashC + "-Clash.nam");
    const auto clash = pack / (kHashC + "-Clash.nam");
    const auto mine = root / "My Models" / (kHashD + "-" + kHashD + "-Mine.nam");
    // Past MAX_PATH while doubled, under it once collapsed, as the Preset Pack Vol. 1 models were.
    const std::size_t packLength = pack.native().size();
    const auto deepDir = pack / std::string(packLength + 80 < 240 ? 240 - packLength - 80 : 1, 'p');
    const auto longDoubled = deepDir / (kHashD + "-" + kHashD + "-Long.nam");
    const auto longCollapsed = deepDir / (kHashD + "-Long.nam");

    {
        TestHost host(root);
        guitarfx::PluginController controller(host);
        controller.Initialize();
    }

    WriteBytes(alone, kModelA);
    WriteBytes(twinDoubled, kModelB);
    WriteBytes(twin, kModelB);
    WriteBytes(clashDoubled, kModelA);
    WriteBytes(clash, kModelB);
    WriteBytes(mine, kModelA);
    WriteBytes(longDoubled, kModelA);

    {
        guitarfx::storage::JsonStore store;
        std::string error;
        Expect(store.Open(StorePath(root), error), "the test can open the store: " + error);

        for (const auto& resource : {Entry("alone", alone), Entry("twin-old", twinDoubled), Entry("twin-new", twin),
                                     Entry("clash", clashDoubled), Entry("mine", mine), Entry("long", longDoubled)})
        {
            Expect(guitarfx::ResourceLibrary::PutInStore(store, resource, ResourcesRoot(root)),
                   "the test can seed " + resource.id);
        }
    }

    TestHost host(root);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    const auto& library = controller.GetResourceLibrary();
    const auto pathOf = [&](const std::string& id) {
        const auto resource = library.LookupResource("nam", id);
        return resource ? resource->filePath.lexically_normal() : fs::path{};
    };

    Expect(pathOf("alone") == (pack / (kHashA + "-Alone.nam")).lexically_normal(), "a doubled file is renamed");
    Expect(guitarfx::util::PathExists(pack / (kHashA + "-Alone.nam")) && !guitarfx::util::PathExists(alone),
           "on disk too");
    Expect(pathOf("twin-old") == twin.lexically_normal(), "an entry moves onto an identical copy already there");
    Expect(!guitarfx::util::PathExists(twinDoubled), "whose doubled duplicate goes");
    Expect(pathOf("twin-new") == twin.lexically_normal() && guitarfx::util::PathExists(twin), "and the copy stays");
    Expect(pathOf("clash") == clashDoubled.lexically_normal() && guitarfx::util::PathExists(clashDoubled) &&
               guitarfx::util::ReadFileBytes(clash) == kModelB,
           "a different file under the collapsed name is never replaced");
    Expect(pathOf("mine") == mine.lexically_normal() && guitarfx::util::PathExists(mine),
           "a file in the user's own folder keeps its name");
    Expect(pathOf("long") == longCollapsed.lexically_normal() && guitarfx::util::PathExists(longCollapsed) &&
               !guitarfx::util::PathExists(longDoubled),
           "a doubled file past MAX_PATH is renamed too (" + std::to_string(longDoubled.native().size()) + " chars)");

    guitarfx::storage::JsonStore store;
    std::string error;

    if (store.Open(StorePath(root), error))
    {
        const auto row =
            store.Get(guitarfx::storage::ItemType::kResource, guitarfx::ResourceLibrary::MakeStoreId("nam", "alone"));
        Expect(row && row->value("filePath", "").find(kHashA + "-Alone.nam") != std::string::npos &&
                   row->value("filePath", "").find(kHashA + "-" + kHashA) == std::string::npos,
               "the stored row is updated");
    }
}

/// A model and an IR at paths past MAX_PATH load: through the library, the effects and the
/// controller's own file route.
void TestLongPathsLoad()
{
    const auto root = Sandbox("long-load");
    const auto deep = root / std::string(120, 'm') / std::string(120, 'n');
    const auto model = deep / "Boosted-6505+-A2-Chugs-FullRig.nam";
    const auto ir = deep / "421 1960.wav";
    WriteBytes(model, Fixture(fs::path("assets") / "amps" / "Guitar" / "A2" / "Boosted-6505+-A2-Chugs-FullRig.nam"));
    WriteBytes(ir, Fixture(fs::path("assets") / "ir" / "421 1960.wav"));
    Expect(model.native().size() > 260 && ir.native().size() > 260, "the fixtures sit past MAX_PATH");
    Expect(guitarfx::util::PathExists(model) && guitarfx::util::PathExists(ir), "PathExists sees them");

    Expect(guitarfx::nammodelcache::GetModel(model) != nullptr, "the model cache parses the long-path model");
    guitarfx::IRWavData wav;
    Expect(guitarfx::irwav::LoadAudioFile(ir, wav) && !wav.samples.empty(), "the WAV loader reads the long-path IR");

    guitarfx::ResourceLibrary library;
    library.AddResource(Entry("deep", model));
    const auto resolved = library.ResolveResource(guitarfx::ResourceRef{"nam", "deep"});
    Expect(resolved && *resolved == model, "the library resolves the entry to its own long path");

    auto amp = guitarfx::EffectRegistry::Instance().Create(guitarfx::EffectGuids::kAmpNamOptimized);
    Expect(amp && amp->LoadResource(model) && amp->HasResource(), "the NAM amp loads the long-path model");
    auto cab = guitarfx::EffectRegistry::Instance().Create(guitarfx::EffectGuids::kCabIr);

    if (cab)
    {
        cab->Prepare(48000.0, 256);
    }

    Expect(cab && cab->LoadResource(ir) && cab->HasResource(), "the IR cab loads the long-path IR");

    TestHost host(root);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    Send(controller, "loadModel", {{"path", guitarfx::util::PathToUtf8(model)}});

    for (const auto& error : Messages(host, "error"))
    {
        Expect(false, "loadModel reported \"" + error.value("message", "") + "\" for the long path");
    }

    bool registered = false;

    for (const auto& resource : controller.GetResourceLibrary().GetAllResources())
    {
        registered = registered || resource.filePath.lexically_normal() == model.lexically_normal();
    }

    Expect(registered, "loadModel adds the long-path model to the library");
}

/// A profile so deep that a factory archive's extraction folder passes the 248 characters
/// CreateDirectory allows and its files pass MAX_PATH, while the archive itself sits at a short
/// path, as an install's does. The files were written, but the existence check after the write
/// failed, so none reached the library and nodes showed the file's hash name.
void TestFactoryArchiveUnderLongProfile()
{
    const auto bundled = Sandbox("factory-long");
    const auto packTail =
        fs::path("Soundshed Guitar") / "data" / "v1" / "resources" / "content" / "factory-archives" / "Long-Pack";
    const std::size_t shortest = (bundled / "p" / packTail).native().size();
    const auto profile = bundled / std::string(shortest < 252 ? 253 - shortest : 1, 'p');
    const auto packDir = profile / packTail;
    _putenv_s("APPDATA", profile.string().c_str());

    const std::string modelName = kHashA + "-Deep-Model.nam";
    const std::string irName = kHashB + "-Deep-Cab.wav";
    const nlohmann::json index = {
        {"formatVersion", 1},
        {"presets", {PresetJson("deep-tone", "m1")}},
        {"resources",
         {{{"id", "m1"},
           {"name", "Deep Model"},
           {"category", "amp"},
           {"type", "nam"},
           {"fileName", modelName},
           {"hash", kHashA}},
          {{"id", "c1"},
           {"name", "Deep Cab"},
           {"category", "ir"},
           {"type", "ir"},
           {"fileName", irName},
           {"hash", kHashB}}}},
        {"blends", nlohmann::json::array()},
    };
    WriteBytes(bundled / "ui" / "presets" / "factory" / "Long-Pack.soundshed.presets",
               BuildZip({{"presets.json", ToBytes(index.dump())},
                         {"resources/" + modelName, kModelA},
                         {"resources/" + irName, Fixture(fs::path("assets") / "ir" / "421 1960.wav")}}));
    Expect(packDir.native().size() > 248 && (packDir / modelName).native().size() > 260,
           "the extraction folder passes 248 characters and its files MAX_PATH (" +
               std::to_string((packDir / modelName).native().size()) + " chars)");

    // The second start finds the files already there and only checks their size.
    for (const char* start : {"first start", "second start"})
    {
        TestHost host(profile, bundled);
        guitarfx::PluginController controller(host);
        controller.Initialize();
        const auto& library = controller.GetResourceLibrary();

        for (const auto& [type, id, name, file] : {std::tuple{"nam", "Long-Pack__m1", "Deep Model", modelName},
                                                   std::tuple{"ir", "Long-Pack__c1", "Deep Cab", irName}})
        {
            const auto resource = library.LookupResource(type, id);
            Expect(resource.has_value(), std::string(start) + ": " + id + " is in the library");

            if (!resource)
            {
                continue;
            }

            Expect(resource->name == name, std::string(start) + ": " + id + " keeps its name");
            Expect(resource->filePath.lexically_normal() == (packDir / file).lexically_normal() &&
                       guitarfx::util::PathExists(resource->filePath),
                   std::string(start) + ": " + id + " points at its long-path file");
        }

        const auto resolved = library.ResolveResource(guitarfx::ResourceRef{"nam", "Long-Pack__m1"});
        Expect(resolved && guitarfx::util::ReadFileBytes(*resolved) == kModelA,
               std::string(start) + ": the preset's model resolves to the extracted bytes");
    }
}
} // namespace

int main()
{
    guitarfx::RegisterAllEffects();

    TestCollapseRepeatedHashPrefix();
    TestLongPathSafe();
    TestFactoryArchiveDropsTheRepeat();
    TestFactoryArchiveKeepsKnownMetadata();
    TestToneSharingInstallDropsTheRepeat();
    TestStartupRenamesDoubledFiles();
#ifdef _WIN32
    TestLongPathsLoad();
    TestFactoryArchiveUnderLongProfile();
#endif

    // Some tools cannot delete what sits past MAX_PATH, so nothing is left behind.
    RemoveTree(RunRoot());

    if (gFailures > 0)
    {
        std::cerr << gFailures << " check(s) failed" << std::endl;
        return 1;
    }

    std::cout << "ResourceFilePathTests passed" << std::endl;
    return 0;
}
