/**
 * IrResourceCategoryTests.cpp — Filing IRs in the library under "cab" or "reverb".
 *
 * An IR's type ("ir") says what the file is and its category says which slot it is for, so
 * the category has to be one of the two slots or the IR Cabinet's and IR Reverb's pickers
 * cannot tell their files apart. These check that:
 *  - the classifier calls every cab IR in testdata a cab, and a synthetic small room, hall
 *    and plate reverbs;
 *  - a slot named on import wins over the measurement ("use the slot"), while Tone3000's
 *    "ir", a folder name or "Local" is settled by measuring the file;
 *  - startup settles the free-form categories older builds stored, once, keeping a pack or
 *    folder name as a tag;
 *  - an edit can move an IR between the slots, and cannot file it anywhere else.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "MessageThreadTestHost.h"
#include "PluginController.h"
#include "controller/internal/IrResourceCategory.h"
#include "resources/ResourceLibrary.h"
#include "storage/JsonStore.h"
#include "util/Base64.h"

namespace fs = std::filesystem;
using namespace guitarfx;
using namespace guitarfx::controller_detail;

namespace
{
constexpr int kSampleRate = 48000;

class QuietHost final : public test::PumpedTestHost
{
  public:
    explicit QuietHost(const fs::path& root) : PumpedTestHost(root, std::this_thread::get_id(), 48000.0, 512)
    {
    }

    void SendMessageToUI(const std::string&) override
    {
    }

    void RunOnMainThread(std::function<void()> fn) override
    {
        fn();
    }
};

fs::path FreshSandbox(const std::string& name)
{
    // Per-process, so a parallel ctest run of another build cannot delete it mid-test.
    const fs::path sandbox = fs::temp_directory_path() / "guitarfx-ir-category-tests" /
                             (name + "-" + std::to_string(std::random_device{}()));
    std::error_code ec;
    fs::remove_all(sandbox, ec);
    fs::create_directories(sandbox, ec);
    test::SetSettingsEnvRoot(sandbox);
    return sandbox;
}

/// A mono 16-bit WAV.
std::vector<std::uint8_t> WavBytes(const std::vector<float>& samples)
{
    std::vector<std::uint8_t> out;
    const auto put32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i)
        {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    const auto put16 = [&](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    const auto tag = [&](const char* t) { out.insert(out.end(), t, t + 4); };
    const auto dataBytes = static_cast<std::uint32_t>(samples.size() * 2);

    tag("RIFF");
    put32(36 + dataBytes);
    tag("WAVE");
    tag("fmt ");
    put32(16);
    put16(1);
    put16(1);
    put32(kSampleRate);
    put32(kSampleRate * 2);
    put16(2);
    put16(16);
    tag("data");
    put32(dataBytes);

    for (const float s : samples)
    {
        put16(
            static_cast<std::uint16_t>(static_cast<std::int16_t>(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f))));
    }

    return out;
}

/// Exponentially decaying noise: a stand-in for a room with this RT60.
std::vector<float> Room(double rt60Seconds, double lengthSeconds)
{
    std::mt19937 rng(7);
    std::normal_distribution<float> noise(0.0f, 0.25f);
    std::vector<float> ir(static_cast<std::size_t>(lengthSeconds * kSampleRate));
    const double perSample = std::log(1000.0) / (rt60Seconds * kSampleRate);

    for (std::size_t n = 0; n < ir.size(); ++n)
    {
        ir[n] = static_cast<float>(noise(rng) * std::exp(-perSample * static_cast<double>(n)));
    }

    ir[0] = 0.9f;
    return ir;
}

fs::path WriteWav(const fs::path& path, const std::vector<float>& samples)
{
    const auto bytes = WavBytes(samples);
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

bool Expect(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::cerr << "  " << what << "\n";
    }

    return ok;
}

bool TestEveryTestdataCabIrIsACab()
{
    bool ok = true;
    int count = 0;

    for (const auto& entry : fs::recursive_directory_iterator(fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "ir"))
    {
        if (entry.path().extension() != ".wav")
        {
            continue;
        }

        ++count;
        const auto category = ClassifyIrFileCategory(entry.path());
        ok &= Expect(category == std::optional<std::string>{"cab"},
                     entry.path().filename().string() + " measured " + category.value_or("(unreadable)"));
    }

    return Expect(count >= 10, "too few testdata IRs found") && ok;
}

bool TestRoomsAreReverbs()
{
    const fs::path sandbox = FreshSandbox("rooms");
    bool ok = true;

    // A small room is the hardest case: in the 2026-10-08 survey a heavily damped one fell
    // 30 dB in 160 ms, against 110 ms for the slowest cab.
    for (const auto& [rt60, name] : {std::pair{0.35, "small-room"}, std::pair{1.2, "hall"}, std::pair{2.5, "plate"}})
    {
        const auto category =
            ClassifyIrFileCategory(WriteWav(sandbox / (std::string(name) + ".wav"), Room(rt60, rt60 * 1.5)));
        ok &= Expect(category == std::optional<std::string>{"reverb"},
                     std::string(name) + " measured " + category.value_or("(unreadable)"));
    }

    // A hall whose reflections arrive after a gap, and a plate mixed with a dry hit 30 dB above
    // it: both fall 30 dB at once, and are caught by how long they keep delivering energy.
    std::vector<float> gapped(static_cast<std::size_t>(1.5 * kSampleRate), 0.0f);
    const auto hall = Room(1.2, 1.4);
    std::copy(hall.begin(), hall.end(), gapped.begin() + static_cast<std::ptrdiff_t>(0.06 * kSampleRate));
    gapped[0] = 1.0f;
    std::vector<float> wetDry = Room(2.0, 2.5);

    for (auto& sample : wetDry)
    {
        sample *= 0.03f;
    }

    wetDry[0] = 1.0f;

    for (const auto& [ir, name] : {std::pair{&gapped, "gapped-hall"}, std::pair{&wetDry, "wet-dry-plate"}})
    {
        const auto category = ClassifyIrFileCategory(WriteWav(sandbox / (std::string(name) + ".wav"), *ir));
        ok &= Expect(category == std::optional<std::string>{"reverb"},
                     std::string(name) + " measured " + category.value_or("(unreadable)"));
    }

    // Trailing silence must not make a cab look long, nor a cab repeated later in the file.
    std::vector<float> paddedCab = Room(0.02, 2.0);
    const auto padded = ClassifyIrFileCategory(WriteWav(sandbox / "padded-cab.wav", paddedCab));
    ok &= Expect(padded == std::optional<std::string>{"cab"}, "a cab padded to 2 s measured " + padded.value_or("?"));
    std::vector<float> twiceCab(static_cast<std::size_t>(kSampleRate), 0.0f);
    const auto cab = Room(0.03, 0.5);
    std::copy(cab.begin(), cab.end(), twiceCab.begin());
    std::copy(cab.begin(), cab.end(), twiceCab.begin() + kSampleRate / 2);
    const auto twice = ClassifyIrFileCategory(WriteWav(sandbox / "twice-cab.wav", twiceCab));
    ok &= Expect(twice == std::optional<std::string>{"cab"}, "a cab held twice measured " + twice.value_or("?"));
    return ok;
}

bool TestCategoryNamesMapToSlots()
{
    bool ok = true;
    ok &= Expect(MapToIrLibraryCategory("Cabinet") == std::optional<std::string>{"cab"}, "Cabinet is not a cab");
    ok &= Expect(MapToIrLibraryCategory(" cab ") == std::optional<std::string>{"cab"}, "' cab ' is not a cab");
    ok &= Expect(MapToIrLibraryCategory("space") == std::optional<std::string>{"reverb"},
                 "Tone3000 space is not a reverb");
    ok &= Expect(MapToIrLibraryCategory("Reverbs") == std::optional<std::string>{"reverb"}, "Reverbs is not a reverb");
    // Tone3000's "ir" covers cabs and rooms alike, so it decides nothing.
    ok &= Expect(!MapToIrLibraryCategory("ir"), "ir was taken to name a slot");
    ok &= Expect(!MapToIrLibraryCategory("pedal"), "pedal was taken to name a slot");
    ok &= Expect(!MapToIrLibraryCategory("Local"), "Local was taken to name a slot");

    ok &= Expect(IrCategoryToKeepAsTag(" Big Gee's Lexicon 480L ") ==
                     std::optional<std::string>{"Big Gee's Lexicon 480L"},
                 "a pack name was not kept as a tag");

    for (const char* generic : {"Local", "ir", "cab", "Reverbs", "space", "Uncategorized", ""})
    {
        ok &= Expect(!IrCategoryToKeepAsTag(generic), std::string("'") + generic + "' would become a tag");
    }

    return ok;
}

bool TestSlotWinsAndTheRestIsMeasured()
{
    const fs::path sandbox = FreshSandbox("resolve");
    LibraryResource room;
    room.type = "ir";
    room.filePath = WriteWav(sandbox / "room.wav", Room(0.8, 1.2));

    bool ok = true;
    // The user picked it for the IR Cabinet: that is their call.
    ok &= Expect(ResolveIrLibraryCategory(room, "cab") == "cab", "the cab slot was overruled");
    ok &= Expect(ResolveIrLibraryCategory(room, "ir") == "reverb", "Tone3000's ir was not measured");
    ok &= Expect(ResolveIrLibraryCategory(room, "Local") == "reverb", "Local was not measured");
    ok &= Expect(ResolveIrLibraryCategory(room, "pedal") == "reverb", "a reverb pedal IR was not measured");

    LibraryResource missing;
    missing.type = "ir";
    missing.name = "Twin Reverb 2x12";
    missing.filePath = sandbox / "gone.wav";
    ok &= Expect(ResolveIrLibraryCategory(missing, "IMreverbs") == "reverb", "a missing file in a reverb pack");
    // An amp called "... Reverb" names the cab, not a room.
    ok &= Expect(ResolveIrLibraryCategory(missing, "Local") == "cab", "a missing file guessed from its name");
    missing.filePath = sandbox / "Reverbs" / "IMreverbs" / "Bottle Hall.wav";
    ok &= Expect(ResolveIrLibraryCategory(missing, "Local") == "reverb", "a missing file in a Reverbs folder");
    return ok;
}

bool TestImportUsesTheSlotOrTheMeasurement()
{
    const fs::path sandbox = FreshSandbox("import");
    QuietHost host(sandbox);
    PluginController controller(host);
    const auto roomData = util::EncodeBase64(WavBytes(Room(0.8, 1.2)));

    const auto import = [&](const std::string& id, const std::string& category) {
        controller.HandleUIMessage(nlohmann::json{
            {"type", "importRemoteResource"},
            {"requestId", "import-" + id},
            {"provider", "tone3000"},
            {"resourceType", "ir"},
            {"resourceId", id},
            {"name", id},
            {"category", category},
            {"subfolder", "ir/Test"},
            {"fileName", id + ".wav"},
            {"data", roomData}}.dump());
        const auto resource = controller.GetResourceLibrary().LookupResource("ir", id);
        return resource ? resource->category : std::string{"(not imported)"};
    };

    bool ok = true;
    ok &= Expect(import("from-reverb-slot", "reverb") == "reverb", "the reverb slot's import");
    ok &= Expect(import("from-tone3000-gear", "ir") == "reverb", "Tone3000's ir was not measured on import");

    controller.HandleUIMessage(nlohmann::json{{"type", "updateLibraryResource"},
                                              {"resourceType", "ir"},
                                              {"resourceId", "from-reverb-slot"},
                                              {"category", "My Rooms"}}
                                   .dump());
    ok &= Expect(controller.GetResourceLibrary().LookupResource("ir", "from-reverb-slot")->category == "reverb",
                 "an edit filed an IR outside the two slots");

    controller.HandleUIMessage(nlohmann::json{{"type", "updateLibraryResource"},
                                              {"resourceType", "ir"},
                                              {"resourceId", "from-reverb-slot"},
                                              {"category", "cab"}}
                                   .dump());
    ok &= Expect(controller.GetResourceLibrary().LookupResource("ir", "from-reverb-slot")->category == "cab",
                 "an edit could not move an IR to the cab slot");
    return ok;
}

bool TestStartupSettlesOldCategoriesOnce()
{
    const fs::path sandbox = FreshSandbox("startup");
    const fs::path cabFile = fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets" / "ir" / "421 1960.wav";
    const fs::path roomFile = WriteWav(sandbox / "hall.wav", Room(1.5, 2.0));

    {
        QuietHost host(sandbox);
        PluginController controller(host);
        controller.Initialize();
    }

    fs::path dbPath;

    for (const auto& entry : fs::recursive_directory_iterator(sandbox))
    {
        if (entry.path().filename() == "soundshed.db")
        {
            dbPath = entry.path();
        }
    }

    if (!Expect(!dbPath.empty(), "no document store was created"))
    {
        return false;
    }

    const auto row = [](const std::string& id, const std::string& category, const fs::path& file) {
        LibraryResource resource;
        resource.type = "ir";
        resource.id = id;
        resource.name = id;
        resource.category = category;
        resource.filePath = file;
        return resource;
    };

    {
        storage::JsonStore store;
        std::string error;

        if (!Expect(store.Open(dbPath, error), "could not open the store: " + error))
        {
            return false;
        }

        for (const auto& resource : {row("tone3000-ir", "ir", cabFile), row("tone3000-space", "space", roomFile),
                                     row("folder-pack", "Big Gee's Lexicon 480L", roomFile),
                                     row("local", "Local", cabFile), row("kept", "reverb", cabFile)})
        {
            ResourceLibrary::PutInStore(store, resource, sandbox / "elsewhere");
        }
    }

    QuietHost host(sandbox);
    PluginController controller(host);
    controller.Initialize();
    const auto& library = controller.GetResourceLibrary();
    const auto category = [&](const std::string& id) {
        const auto resource = library.LookupResource("ir", id);
        return resource ? resource->category : std::string{"(missing)"};
    };

    bool ok = true;
    ok &= Expect(category("tone3000-ir") == "cab", "Tone3000 ir cab became " + category("tone3000-ir"));
    ok &= Expect(category("tone3000-space") == "reverb", "Tone3000 space became " + category("tone3000-space"));
    ok &= Expect(category("folder-pack") == "reverb", "a reverb pack became " + category("folder-pack"));
    ok &= Expect(category("local") == "cab", "a Local cab became " + category("local"));
    // A category already naming a slot is never second-guessed, even against the file.
    ok &= Expect(category("kept") == "reverb", "a settled category was re-measured");

    const auto pack = library.LookupResource("ir", "folder-pack");
    ok &= Expect(pack && std::find(pack->tags.begin(), pack->tags.end(), "Big Gee's Lexicon 480L") != pack->tags.end(),
                 "the pack name was not kept as a tag");
    return ok;
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

    run("Every testdata cab IR measures as a cab", TestEveryTestdataCabIrIsACab());
    run("Rooms, halls and plates measure as reverbs", TestRoomsAreReverbs());
    run("Category names map to slots only when unambiguous", TestCategoryNamesMapToSlots());
    run("The slot wins and the rest is measured", TestSlotWinsAndTheRestIsMeasured());
    run("Import uses the slot or the measurement", TestImportUsesTheSlotOrTheMeasurement());
    run("Startup settles old categories once", TestStartupSettlesOldCategoriesOnce());

    std::cout << "\nIR resource category tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
