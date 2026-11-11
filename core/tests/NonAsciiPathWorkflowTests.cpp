// Folders and profiles named outside plain ASCII, end to end through the controller.
//
// On Windows the narrow side of std::filesystem::path is the ANSI code page, not UTF-8.
// Anything that crossed from a path to a std::string with path::string(), or back with
// path(std::string), garbled a name like "Müzik" and threw for one the code page cannot
// spell. A user's "Müzik" captures folder stayed on "No folder selected", and a Windows
// user name like "Şule" put the profile in a folder that does not exist.
//
// The JUCE side of the same crossing (the file dialog's result, the profile folder) is
// covered by juce/tests/JucePathConversionTests.cpp.

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "IPluginHost.h"
#include "PluginController.h"
#include "dsp/EffectGuids.h"
#include "presets/PresetStorage.h"
#include "storage/JsonStore.h"
#include "util/FileSystem.h"
#include "util/PathEncoding.h"
#include "util/Wav.h"

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
        mMessages.push_back(jsonMessage);
        mMessageCv.notify_all();
    }

    void BrowseFileAsync(guitarfx::BrowseFileType, const std::string&,
                         std::function<void(const guitarfx::BrowseFileResult&)> callback) override
    {
        guitarfx::BrowseFileResult result;

        if (browseResult)
        {
            result.success = true;
            result.path = *browseResult;
        }

        callback(result);
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

    /// What the file dialog hands back; unset means the user cancelled it.
    std::optional<fs::path> browseResult;

    /// The latest message of this type, waiting up to `timeout` for one to arrive.
    std::optional<nlohmann::json> WaitForMessage(const std::string& type,
                                                 std::chrono::milliseconds timeout = std::chrono::seconds(5))
    {
        std::unique_lock<std::mutex> lock(mMessageMutex);
        std::optional<nlohmann::json> found;

        mMessageCv.wait_for(lock, timeout, [&]() {
            for (auto it = mMessages.rbegin(); it != mMessages.rend(); ++it)
            {
                // Every message must parse: one that does not is a path that left in the ANSI code page.
                auto parsed = nlohmann::json::parse(*it);

                if (parsed.value("type", "") == type)
                {
                    found = std::move(parsed);
                    return true;
                }
            }

            return false;
        });

        return found;
    }

  private:
    fs::path mRoot;
    std::vector<std::string> mMessages;
    std::condition_variable mMessageCv;
    std::mutex mMessageMutex;
};

/// Points the profile root (%APPDATA%, or $HOME elsewhere) at `root` for as long as it lives.
/// On Windows through the wide environment: the only one that can hold every name used here.
class ScopedProfileRoot
{
  public:
    explicit ScopedProfileRoot(const fs::path& root)
    {
#ifdef _WIN32
        wchar_t* value = nullptr;
        std::size_t length = 0;

        if (_wdupenv_s(&value, &length, L"APPDATA") == 0 && value != nullptr)
        {
            mPrevious = value;
            std::free(value);
        }

        _wputenv_s(L"APPDATA", root.c_str());
#else

        if (const char* value = std::getenv("HOME"))
        {
            mPrevious = value;
        }

        setenv("HOME", root.c_str(), 1);
#endif
    }

    ~ScopedProfileRoot()
    {
#ifdef _WIN32
        _wputenv_s(L"APPDATA", mPrevious.c_str());
#else
        setenv("HOME", mPrevious.c_str(), 1);
#endif
    }

    ScopedProfileRoot(const ScopedProfileRoot&) = delete;
    ScopedProfileRoot& operator=(const ScopedProfileRoot&) = delete;

  private:
    fs::path::string_type mPrevious;
};

fs::path FreshSandbox(const fs::path& leaf)
{
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-non-ascii-path-tests" / leaf;
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    return sandbox;
}

/// A folder picked under a name outside plain ASCII is added, listed, and its models already in
/// the library are marked as such. The scan matched library entries through path(std::string),
/// so even once listed, no model in such a folder showed as already in the library.
bool TestFolderPickUnderNonAsciiName()
{
    const fs::path sandbox = FreshSandbox("folder-pick");
    const ScopedProfileRoot profileRoot(sandbox);

    // "Müzik" is the reported folder; "şarkılar" has letters the Western code page cannot spell.
    const std::string leafName = "\xC5\x9F"
                                 "ark\xC4\xB1lar";
    const fs::path folder =
        sandbox / guitarfx::util::PathFromUtf8("M\xC3\xBCzik") / guitarfx::util::PathFromUtf8(leafName);
    const fs::path model = folder / "A2.nam";
    const fs::path source = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "amps" / "Guitar" / "A2" /
                            guitarfx::util::PathFromUtf8("A2 -wth space an \xE2\x80\x94 _emdash.nam");
    std::error_code ec;
    fs::create_directories(folder, ec);

    if (!fs::copy_file(source, model, fs::copy_options::overwrite_existing, ec))
    {
        std::cerr << "Could not stage the test model: " << ec.message() << "\n";
        return false;
    }

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);

    controller.HandleUIMessage(nlohmann::json{
        {"type", "saveLocalLibraryResource"}, {"resourceType", "nam"}, {"filePath", guitarfx::util::PathToUtf8(model)}}
                                   .dump());

    std::string libraryId;

    for (const auto& resource : controller.GetResourceLibrary().GetResourcesByType("nam"))
    {
        if (resource.filePath == model)
        {
            libraryId = resource.id;
        }
    }

    host.browseResult = folder;
    controller.HandleUIMessage(R"({"type":"browseResourceFolder"})");
    const auto picked = host.WaitForMessage("resourceFolderPicked");

    if (libraryId.empty() || !picked || !picked->value("success", false) ||
        picked->value("path", "") != guitarfx::util::PathToUtf8(folder) || picked->value("name", "") != leafName)
    {
        std::cerr << "Unexpected pick: " << (picked ? picked->dump() : std::string("none")) << "\n";
        return false;
    }

    controller.HandleUIMessage(
        nlohmann::json{{"type", "listResourceFolder"}, {"path", picked->value("path", "")}}.dump());
    const auto listing = host.WaitForMessage("resourceFolderListing");

    if (!listing)
    {
        std::cerr << "The picked folder was not listed\n";
        return false;
    }

    for (const auto& file : listing->value("files", nlohmann::json::array()))
    {
        if (file.value("name", "") == "A2.nam")
        {
            return file.value("libraryId", "") == libraryId;
        }
    }

    std::cerr << "The model was missing from the listing: " << listing->dump() << "\n";
    return false;
}

/// A model added to the library from a folder named outside plain ASCII is still found after a
/// restart. The library read each stored path back through path(std::string), the ANSI code page,
/// so after a reload such a model showed as missing and a preset using it could not load it. Both
/// ways a path is stored are covered: absolute, for a folder elsewhere, and relative to the
/// resources folder, for a file inside it.
bool TestLibraryPathsSurviveARestart()
{
    const fs::path sandbox = FreshSandbox("library-reload");
    const ScopedProfileRoot profileRoot(sandbox);
    const fs::path source = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "amps" / "Guitar" / "A2" /
                            guitarfx::util::PathFromUtf8("A2 -wth space an \xE2\x80\x94 _emdash.nam");
    const fs::path outside = sandbox / guitarfx::util::PathFromUtf8("M\xC3\xBCzik") /
                             guitarfx::util::PathFromUtf8("\xC5\x9F"
                                                          "ark\xC4\xB1 \xE2\x80\x94 outside.nam");
    const fs::path inside = guitarfx::FileSystem{}.ResolveSettingsDirectory() / "resources" / "content" / "local" /
                            guitarfx::util::PathFromUtf8("\xC3\x87"
                                                         "a \xE2\x80\x94 inside.nam");
    std::error_code ec;

    for (const auto& target : {outside, inside})
    {
        fs::create_directories(target.parent_path(), ec);

        if (!fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec))
        {
            std::cerr << "Could not stage the test model: " << ec.message() << "\n";
            return false;
        }
    }

    std::vector<std::pair<fs::path, std::string>> added;
    {
        TestHost host(sandbox);
        guitarfx::PluginController controller(host);
        controller.Initialize();

        for (const auto& target : {outside, inside})
        {
            controller.HandleUIMessage(nlohmann::json{{"type", "saveLocalLibraryResource"},
                                                      {"resourceType", "nam"},
                                                      {"filePath", guitarfx::util::PathToUtf8(target)}}
                                           .dump());

            for (const auto& resource : controller.GetResourceLibrary().GetResourcesByType("nam"))
            {
                if (resource.filePath == target)
                {
                    added.emplace_back(target, resource.id);
                }
            }
        }
    }

    if (added.size() != 2)
    {
        std::cerr << "Only " << added.size() << " of the 2 models were added to the library\n";
        return false;
    }

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    bool ok = true;

    for (const auto& [target, id] : added)
    {
        guitarfx::ResourceRef ref;
        ref.resourceType = "nam";
        ref.resourceId = id;
        const auto resource = controller.GetResourceLibrary().LookupResource("nam", id);
        const auto resolved = controller.GetResourceLibrary().ResolveResource(ref);

        if (!resource || resource->filePath != target || !resolved || *resolved != target)
        {
            std::cerr << "After a restart " << guitarfx::util::PathToUtf8(target) << " came back as "
                      << (resource ? guitarfx::util::PathToUtf8(resource->filePath) : std::string("(no row)")) << "\n";
            ok = false;
        }
    }

    return ok;
}

/// A riff take kept in a riff library folder named outside plain ASCII opens for editing. The take's
/// path is stored as UTF-8, and the edit, preview and demo-render handlers read it back as a narrow
/// string, the ANSI code page, so every take in such a library reported its WAV file missing.
bool TestRiffTakeInNonAsciiLibrary()
{
    const fs::path sandbox = FreshSandbox("riff-library");
    const ScopedProfileRoot profileRoot(sandbox);
    const fs::path libraryRoot = sandbox / guitarfx::util::PathFromUtf8("M\xC3\xBCzik \xC5\x9E"
                                                                        "ark\xC4\xB1lar");
    const std::vector<float> tone(4800, 0.25f);

    if (!guitarfx::util::WriteStereo16BitWav(libraryRoot / "takes" / "riff-1" / "take.wav", tone, tone, 48000))
    {
        std::cerr << "Could not write the test take\n";
        return false;
    }

    {
        TestHost host(sandbox);
        guitarfx::PluginController controller(host);
        controller.Initialize();
        controller.HandleUIMessage(
            nlohmann::json{{"type", "setRiffLibraryPath"}, {"path", guitarfx::util::PathToUtf8(libraryRoot)}}.dump());
    }

    {
        guitarfx::storage::JsonStore store;
        std::string error;

        if (!store.Open(guitarfx::FileSystem{}.ResolveSettingsDirectory() / "soundshed.db", error))
        {
            std::cerr << "Could not open the store: " << error << "\n";
            return false;
        }

        const nlohmann::json take{{"id", "take-1"}, {"filePath", "takes/riff-1/take.wav"}, {"durationSec", 0.1}};
        auto index =
            store.Get(guitarfx::storage::ItemType::kDocument, "riff-library").value_or(nlohmann::json::object());
        index["riffs"] = nlohmann::json::array(
            {{{"id", "riff-1"}, {"title", "Riff One"}, {"takes", nlohmann::json::array({take})}}});

        if (!store.Put(guitarfx::storage::ItemType::kDocument, "riff-library", index))
        {
            std::cerr << "Could not seed the riff library\n";
            return false;
        }
    }

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    controller.HandleUIMessage(R"({"type":"loadRiffTakeForEdit","takeId":"take-1"})");

    if (const auto error = host.WaitForMessage("error", std::chrono::milliseconds(200)))
    {
        std::cerr << "Opening the take failed: " << error->dump() << "\n";
        return false;
    }

    if (!host.WaitForMessage("riffCaptureStopped"))
    {
        std::cerr << "The take was not opened for editing\n";
        return false;
    }

    return true;
}

/// A model chosen for a node straight from a file, not the library, in a folder named outside plain
/// ASCII. updateNodeResource took the payload's UTF-8 path as a narrow string, the ANSI code page,
/// so the node pointed at a file that does not exist.
bool TestNodeResourceFromNonAsciiFile()
{
    const fs::path sandbox = FreshSandbox("node-resource");
    const ScopedProfileRoot profileRoot(sandbox);
    const fs::path source = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "amps" / "Guitar" / "A2" /
                            "Boosted-6505+-A2-Chugs-FullRig.nam";
    const fs::path model = sandbox / guitarfx::util::PathFromUtf8("M\xC3\xBCzik") /
                           guitarfx::util::PathFromUtf8("\xC5\x9F"
                                                        "ark\xC4\xB1.nam");
    std::error_code ec;
    fs::create_directories(model.parent_path(), ec);

    if (!fs::copy_file(source, model, fs::copy_options::overwrite_existing, ec))
    {
        std::cerr << "Could not stage the test model: " << ec.message() << "\n";
        return false;
    }

    guitarfx::Preset preset;
    preset.id = "non-ascii-node";
    preset.name = preset.id;
    preset.version = 2;
    preset.graph.nodes.resize(3);
    preset.graph.nodes[0].id = "__input__";
    preset.graph.nodes[0].type = guitarfx::kNodeTypeInput;
    preset.graph.nodes[1].id = "amp";
    preset.graph.nodes[1].type = guitarfx::EffectGuids::kAmpNamOptimized;
    preset.graph.nodes[2].id = "__output__";
    preset.graph.nodes[2].type = guitarfx::kNodeTypeOutput;
    preset.graph.edges = {{"__input__", "amp", 0, 0, 1.0}, {"amp", "__output__", 0, 0, 1.0}};

    TestHost host(sandbox);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    const auto presetJson = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(preset));
    controller.HandleUIMessage(
        nlohmann::json{{"type", "loadPreset"}, {"presetId", preset.id}, {"preset", presetJson}}.dump());
    controller.HandleUIMessage(nlohmann::json{{"type", "updateNodeResource"},
                                              {"nodeId", "amp"},
                                              {"resourceIndex", 0},
                                              {"resourceType", "nam"},
                                              {"filePath", guitarfx::util::PathToUtf8(model)}}
                                   .dump());

    const auto& active = controller.GetActivePreset();
    const auto* amp = active ? active->graph.FindNode("amp") : nullptr;

    if (!amp || amp->resources.empty() || amp->resources.front().filePath != model)
    {
        std::cerr << "The node's model came out as "
                  << (amp && !amp->resources.empty() ? guitarfx::util::PathToUtf8(amp->resources.front().filePath)
                                                     : std::string("(none)"))
                  << "\n";
        return false;
    }

    return true;
}

/// A profile under a Windows user name outside plain ASCII. The core read %APPDATA% through the
/// narrow environment, where the Western code page spells "Şule" as "Sule", and paths it reported
/// went through path::string(): the ANSI code page, which JSON rejects, or a throw.
bool TestProfileUnderNonAsciiUserName()
{
#ifdef _WIN32
    const fs::path sandbox = FreshSandbox(guitarfx::util::PathFromUtf8("\xC5\x9Eule G\xC3\xB6khan"));
    const fs::path profile = sandbox / "Sepia";
    const ScopedProfileRoot profileRoot(sandbox);

    if (const auto resolved = guitarfx::FileSystem{}.ResolvePlatformRootDirectory(); resolved != profile)
    {
        std::cerr << "The profile resolved to " << guitarfx::util::PathToUtf8(resolved) << "\n";
        return false;
    }

    TestHost host(profile);
    guitarfx::PluginController controller(host);
    controller.Initialize();
    controller.HandleUIMessage(R"({"type":"debugReportUiState","source":"test"})");

    const auto written = host.WaitForMessage("debugSnapshotWritten");
    const std::string reportedPath = written ? written->value("path", "") : "";
    const std::string profilePrefix = guitarfx::util::PathToUtf8(profile) + "/";

    if (reportedPath.rfind(profilePrefix, 0) != 0 || !fs::exists(guitarfx::util::PathFromUtf8(reportedPath)) ||
        !fs::exists(profile / "data" / "v1" / "soundshed.db"))
    {
        std::cerr << "Snapshot path '" << reportedPath << "' is not a file under '" << profilePrefix
                  << "', or the document store is missing\n";
        return false;
    }

#endif

    return true;
}

/// The startup writability probe works in a folder outside plain ASCII, leaves nothing behind,
/// and reports a folder that is not there.
bool TestWritableProbeUnderNonAsciiName()
{
    const fs::path sandbox = FreshSandbox(guitarfx::util::PathFromUtf8("M\xC3\xBCzik \xC5\x9E"));
    const guitarfx::FileSystem fileSystem;
    bool ok = true;

    if (const auto problem = fileSystem.CheckWritable(sandbox))
    {
        std::cerr << "A writable folder was reported as not writable: " << *problem << "\n";
        ok = false;
    }

    if (fs::directory_iterator(sandbox) != fs::directory_iterator())
    {
        std::cerr << "The probe left a file behind\n";
        ok = false;
    }

    if (!fileSystem.CheckWritable(sandbox / "missing"))
    {
        std::cerr << "A missing folder was reported as writable\n";
        ok = false;
    }

    return ok;
}
} // namespace

int main()
{
    int failed = 0;

    const auto run = [&](const std::string& name, bool (*test)()) {
        bool ok = false;

        try
        {
            ok = test();
        }
        catch (const std::exception& e)
        {
            std::cerr << name << " threw: " << e.what() << "\n";
        }

        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << "\n";
        failed += ok ? 0 : 1;
    };

    run("Folder pick under a non-ASCII name", TestFolderPickUnderNonAsciiName);
    run("Library paths under non-ASCII names survive a restart", TestLibraryPathsSurviveARestart);
    run("Riff take in a non-ASCII riff library", TestRiffTakeInNonAsciiLibrary);
    run("Node model from a non-ASCII file", TestNodeResourceFromNonAsciiFile);
    run("Profile under a non-ASCII user name", TestProfileUnderNonAsciiUserName);
    run("Writability probe under a non-ASCII name", TestWritableProbeUnderNonAsciiName);

    std::cout << "\nNon-ASCII path workflow tests: " << (failed == 0 ? "all passed" : "FAILED") << "\n";
    return failed == 0 ? 0 : 1;
}
