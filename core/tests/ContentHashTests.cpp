/**
 * ContentHashTests.cpp — the content hashes that identify a NAM model by its weights and an
 * IR by its samples, whatever its metadata says (models/ContentHash.h).
 *
 * These check that:
 *  - every test vector of the Neural Amp Modeler trainer's recipe gives its hash, or none for
 *    the ones it rejects (testdata/content-hash-test-vectors.json, from its repository);
 *  - literals a standard JSON parser cannot round-trip (1e309, NaN, true) are rejected as the
 *    reference rejects them, and the order of a model's members does not matter;
 *  - testdata captures and IRs give the hashes the Soundshed API's TypeScript implementation
 *    gives them, so the two agree, and renaming or re-tagging a model does not change its hash;
 *  - an IR's hash covers its data chunk only, found past chunks with or without a pad byte;
 *  - the library keeps a contentHash on NAM and IR rows: imports set it, a metadata edit from
 *    the UI does not lose it, and a library from before it is hashed once at startup;
 *  - a preset archive's reference to a remote model finds a library copy with the same content.
 */

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "MessageThreadTestHost.h"
#include "PluginController.h"
#include "controller/internal/NamResourceMetadata.h"
#include "controller/internal/RemoteResourceRefs.h"
#include "models/ContentHash.h"
#include "resources/ResourceLibrary.h"
#include "storage/JsonStore.h"
#include "util/Base64.h"

namespace fs = std::filesystem;
using namespace guitarfx;
using namespace guitarfx::controller_detail;

namespace
{
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

bool Expect(bool condition, const std::string& message)
{
    if (!condition)
    {
        std::cout << "    " << message << "\n";
    }

    return condition;
}

bool ExpectEq(const std::string& actual, const std::string& expected, const std::string& what)
{
    return Expect(actual == expected, what + ": got '" + actual + "', expected '" + expected + "'");
}

std::string HashOfText(const std::string& text)
{
    std::istringstream in(text);
    return NamWeightsHash(in).value_or("(none)");
}

fs::path Assets()
{
    return fs::path(GUITARFX_TEST_RESOURCES_DIR) / "assets";
}

std::vector<std::uint8_t> ReadBytes(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

fs::path FreshSandbox(const std::string& name)
{
    // Per-process, so a parallel ctest run of another build cannot delete it mid-test. The
    // settings root moves into it too: without that the controller opens the real profile.
    const fs::path root =
        fs::temp_directory_path() / "soundshed-content-hash-tests" / (name + "-" + std::to_string(std::random_device{}()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    test::SetSettingsEnvRoot(root);
    return root;
}

bool TestTrainerVectors()
{
    std::ifstream file(fs::path(GUITARFX_TEST_RESOURCES_DIR) / "content-hash-test-vectors.json");
    std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

    // nlohmann refuses 1e309, which Python reads as infinity. As a string it is rejected just the
    // same; TestRawLiterals feeds the literal itself to the hash.
    // Only where it is a value: the notes mention it inside a string.
    for (std::size_t at = text.find("1e309"); at != std::string::npos; at = text.find("1e309", at + 1))
    {
        const auto before = text.find_last_not_of(" \t\r\n", at - 1);

        if (before != std::string::npos && (text[before] == '[' || text[before] == ','))
        {
            text.replace(at, 5, "\"1e309\"");
        }
    }

    const auto vectors = nlohmann::json::parse(text, nullptr, false);

    if (!Expect(vectors.is_object() && vectors["vectors"].is_array() && vectors["vectors"].size() >= 20,
                "the test vectors did not load"))
    {
        return false;
    }

    bool ok = true;

    for (const auto& vector : vectors["vectors"])
    {
        // Re-serialised: doubles print back as the same value.
        const std::string expected = vector["weights_hash"].is_string() ? vector["weights_hash"].get<std::string>() : "(none)";
        ok &= ExpectEq(HashOfText(vector["model"].dump()), expected, vector["name"].get<std::string>());
    }

    return ok;
}

bool TestRawLiterals()
{
    const std::string wavenet =
        R"({"version":"0.7.0","architecture":"WaveNet","config":{"head_scale":0.02},"weights":[0.5,-1.25,3.0,1e-08,0.02],"metadata":{}})";
    const std::string expected = "nam-content-v1:7f5c79c758b2e211325b9fe3f42b31cc0e42782d53bfe09a91b3b83be381538b";

    bool ok = ExpectEq(HashOfText(wavenet), expected, "the plain WaveNet vector as written");

    // Members in any order, the architecture last: the weights are read before it is known.
    ok &= ExpectEq(HashOfText(R"({"weights":[0.5,-1.25,3.0,1e-8,0.02],"metadata":{"name":"x"},"architecture":"WaveNet"})"),
                   expected, "weights before the architecture, another spelling of 1e-08");
    ok &= ExpectEq(HashOfText(R"({"architecture":"WaveNet","weights":[0.5,-1.25,3E0,0.00000001,2e-2]})"), expected,
                   "exponent spellings");

    for (const char* literal : {"1e309", "-1e309", "NaN", "Infinity", "-Infinity", "true", "null", "\"1\""})
    {
        ok &= ExpectEq(HashOfText(std::string{R"({"architecture":"WaveNet","weights":[0.5,)"} + literal + "]}"), "(none)",
                       std::string{"a weight of "} + literal);
    }

    // Too small for a float32 is zero, not an error: [1e-400] hashes as [0].
    ok &= ExpectEq(HashOfText(R"({"architecture":"WaveNet","weights":[1e-400]})"),
                   HashOfText(R"({"architecture":"WaveNet","weights":[0]})"), "an underflowing weight");

    // A container's own weights are not hashed; its children's are.
    ok &= ExpectEq(HashOfText(R"({"architecture":"Sequential","weights":[9],"config":{"models":[)" + wavenet + "]}}"),
                   HashOfText(R"({"architecture":"Sequential","weights":[],"config":{"models":[)" + wavenet + "]}}"),
                   "a Sequential's own weights");

    // A falsy config is an empty one; a truthy non-object is malformed.
    ok &= ExpectEq(HashOfText(R"({"architecture":"WaveNet","config":null,"weights":[0.5,-1.25,3.0,1e-08,0.02]})"),
                   expected, "a null config");
    ok &= ExpectEq(HashOfText(R"({"architecture":"WaveNet","config":[1],"weights":[0.5]})"), "(none)",
                   "a list config");
    ok &= ExpectEq(HashOfText("not json"), "(none)", "not JSON");
    ok &= ExpectEq(HashOfText(R"({"architecture":"WaveNet","weights":[0.5)"), "(none)", "a truncated file");
    return ok;
}

bool TestTestdataFiles()
{
    // Hashes the Soundshed API's TypeScript implementation (api/src/lib/content-hash.ts) gives
    // these files; it matches Tone3000's own content_hash on 29 of its models.
    struct Case
    {
        fs::path path;
        const char* type;
        const char* hash;
    };

    const std::vector<Case> cases = {
        {Assets() / "amps/Guitar/A2/Boosted-6505+-A2-Chugs-FullRig.nam", "nam",
         "nam-content-v1:8bde975150a3ed6bf4208a555090a7237d87dd40643fd051e3c09bac7b633c14"},
        {Assets() / "amps/Guitar/A2/[POW] MESA.MKVII-90W-EQ@ON Pres@5 - SM58.nam", "nam",
         "nam-content-v1:d8fcd1ae106fe0425cb986ac5b4cfde051f07570022a84531e0bfb02e8c5790e"},
        {Assets() / "amps/Guitar/TimR/JCM800 2203 1985/JCM800 Hi P6 B8 M4 T7 G10.nam", "nam",
         "nam-content-v1:d58e0069878ff3b47319295362a9421c1345ccda96f98546528c16a677e8716f"},
        // Its odd-sized bext chunk is not followed by a pad byte.
        {Assets() / "ir/421 1960.wav", "ir", "ir-content-v1:f7f40423cd886ff9813b3101af50783d5cd4ebbf0d0212c047a48701e630f7b5"},
        {Assets() / "ir/906 1960.wav", "ir", "ir-content-v1:569b49bdce544c090a26f8c0485372d16d7102fd28510a9cc44b27ed097097dd"},
    };

    bool ok = true;

    for (const auto& c : cases)
    {
        ok &= ExpectEq(ContentHashOfFile(c.type, c.path).value_or("(none)"), c.hash, c.path.filename().string());
    }

    ok &= ExpectEq(ContentHashOfFile("plugin", cases[0].path).value_or("(none)"), "(none)", "a plugin");
    ok &= ExpectEq(ContentHashOfFile("nam", Assets() / "missing.nam").value_or("(none)"), "(none)", "a missing file");
    ok &= Expect(IsContentHash(cases[0].hash) && IsContentHash(cases[3].hash), "IsContentHash rejected a real hash");
    ok &= Expect(!IsContentHash("nam-content-v1:8BDE") && !IsContentHash("sha256:00"), "IsContentHash accepted junk");

    // A model renamed, re-tagged and recalibrated is the same model.
    auto model = nlohmann::ordered_json::parse(std::ifstream(cases[0].path));
    model["metadata"]["name"] = "Renamed";
    model["metadata"]["gear_type"] = "pedal";
    model["metadata"]["input_level_dbu"] = 3.5;
    ok &= ExpectEq(HashOfText(model.dump(1)), cases[0].hash, "an edited copy of an A2 capture");
    return ok;
}

std::vector<std::uint8_t> Wav(const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& chunks, bool pad)
{
    std::vector<std::uint8_t> out = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};

    for (const auto& [id, body] : chunks)
    {
        out.insert(out.end(), id.begin(), id.end());
        const auto size = static_cast<std::uint32_t>(body.size());

        for (int i = 0; i < 4; ++i)
        {
            out.push_back(static_cast<std::uint8_t>(size >> (8 * i)));
        }

        out.insert(out.end(), body.begin(), body.end());

        if (pad && (size & 1u) != 0)
        {
            out.push_back(0);
        }
    }

    return out;
}

bool TestIrChunks()
{
    const std::vector<std::uint8_t> fmt(16, 1);
    const std::vector<std::uint8_t> data = {1, 2, 3, 4, 5, 6};
    const std::vector<std::uint8_t> odd = {7, 8, 9};

    const auto plain = IrContentHash(Wav({{"fmt ", fmt}, {"data", data}}, true));
    const auto padded = IrContentHash(Wav({{"bext", odd}, {"fmt ", fmt}, {"data", data}}, true));
    const auto unpadded = IrContentHash(Wav({{"bext", odd}, {"fmt ", fmt}, {"data", data}}, false));
    const auto otherSamples = IrContentHash(Wav({{"fmt ", fmt}, {"data", {1, 2, 3, 4, 5, 7}}}, true));

    bool ok = Expect(plain.has_value(), "a plain WAV had no hash");
    ok &= Expect(plain == padded && plain == unpadded, "a chunk before the data changed the hash");
    ok &= Expect(plain != otherSamples, "different samples gave the same hash");
    ok &= Expect(!IrContentHash(Wav({{"fmt ", fmt}}, true)), "a WAV with no data chunk had a hash");
    ok &= Expect(!IrContentHash(std::vector<std::uint8_t>{'O', 'g', 'g', 'S'}), "a non-WAV had a hash");

    // A data chunk claiming more than the file holds is read to its end.
    auto truncated = Wav({{"fmt ", fmt}, {"data", data}}, true);
    truncated[truncated.size() - data.size() - 4] = 0xff;
    ok &= Expect(IrContentHash(truncated) == plain, "an oversized data chunk was not read to the end of the file");
    return ok;
}

std::string MetadataValue(const LibraryResource& resource, const std::string& key)
{
    const auto it = resource.metadata.find(key);
    return it == resource.metadata.end() ? std::string{"(none)"} : it->second;
}

bool TestLibraryKeepsContentHash()
{
    const fs::path sandbox = FreshSandbox("library");
    const fs::path capture = Assets() / "amps/Guitar/A2/Boosted-6505+-A2-Chugs-FullRig.nam";
    const fs::path ir = Assets() / "ir/421 1960.wav";
    const std::string captureHash = "nam-content-v1:8bde975150a3ed6bf4208a555090a7237d87dd40643fd051e3c09bac7b633c14";
    const std::string irHash = "ir-content-v1:f7f40423cd886ff9813b3101af50783d5cd4ebbf0d0212c047a48701e630f7b5";

    fs::path dbPath;
    bool ok = true;
    {
        QuietHost host(sandbox);
        PluginController controller(host);
        controller.Initialize();
        const auto import =[&](const std::string& type, const std::string& id, const fs::path& path,
                                const std::string& category) {
            controller.HandleUIMessage(nlohmann::json{{"type", "importRemoteResource"},
                                                      {"requestId", "import-" + id},
                                                      {"provider", "tone3000"},
                                                      {"resourceType", type},
                                                      {"resourceId", id},
                                                      {"name", id},
                                                      {"category", category},
                                                      {"subfolder", type + "/Test"},
                                                      {"fileName", path.filename().string()},
                                                      {"data", util::EncodeBase64(ReadBytes(path))}}
                                           .dump());
            return controller.GetResourceLibrary().LookupResource(type, id);
        };

        const auto nam = import("nam", "local-nam", capture, "amp");
        const auto wav = import("ir", "local-ir", ir, "cab");

        if (!Expect(nam && wav, "the capture or the IR did not import"))
        {
            return false;
        }

        ok &= ExpectEq(MetadataValue(*nam, kContentHashMetadataKey), captureHash, "an imported capture");
        ok &= ExpectEq(MetadataValue(*wav, kContentHashMetadataKey), irHash, "an imported IR");

        // The UI sends a metadata map that replaces the row's; the hash is the file's and stays.
        controller.HandleUIMessage(nlohmann::json{{"type", "updateLibraryResource"},
                                                  {"resourceType", "nam"},
                                                  {"resourceId", "local-nam"},
                                                  {"name", "Renamed"},
                                                  {"category", "amp"},
                                                  {"metadata", {{"toneId", "123"}}}}
                                       .dump());
        const auto edited = controller.GetResourceLibrary().LookupResource("nam", "local-nam");
        ok &= Expect(edited.has_value(), "the edited row went missing");

        if (edited)
        {
            ok &= ExpectEq(MetadataValue(*edited, "toneId"), "123", "the edit");
            ok &= ExpectEq(MetadataValue(*edited, kContentHashMetadataKey), captureHash, "the hash after an edit");
        }
    }

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

    // A library from before hashes: rows without one and no revision stamp.
    {
        storage::JsonStore store;
        std::string error;

        if (!Expect(store.Open(dbPath, error), "could not open the store: " + error))
        {
            return false;
        }

        LibraryResource namRow;
        namRow.type = "nam";
        namRow.id = "old-nam";
        namRow.name = "Old capture";
        namRow.category = "Amp";
        namRow.filePath = capture;
        namRow.metadata["namFileVersion"] = "0.7.0";
        LibraryResource irRow;
        irRow.type = "ir";
        irRow.id = "old-ir";
        irRow.name = "Old cab";
        irRow.category = "cab";
        irRow.filePath = ir;
        ResourceLibrary::PutInStore(store, namRow, sandbox / "elsewhere");
        ResourceLibrary::PutInStore(store, irRow, sandbox / "elsewhere");
        ok &= Expect(store.SetMeta(kContentHashRevisionKey, "0"), "could not clear the stamp");
    }

    {
        QuietHost host(sandbox);
        PluginController controller(host);
        controller.Initialize();
        const auto namRow = controller.GetResourceLibrary().LookupResource("nam", "old-nam");
        const auto irRow = controller.GetResourceLibrary().LookupResource("ir", "old-ir");

        if (!Expect(namRow && irRow, "the seeded rows did not load"))
        {
            return false;
        }

        ok &= ExpectEq(MetadataValue(*namRow, kContentHashMetadataKey), captureHash, "a capture hashed at startup");
        ok &= ExpectEq(MetadataValue(*irRow, kContentHashMetadataKey), irHash, "an IR hashed at startup");
    }

    storage::JsonStore store;
    std::string error;
    ok &= Expect(store.Open(dbPath, error), "could not reopen the store: " + error);
    ok &= ExpectEq(store.GetMeta(kContentHashRevisionKey).value_or("(none)"), kContentHashRevision, "revision stamp");
    return ok;
}
bool TestArchiveReferenceFindsTheSameModel()
{
    const std::string hash = "nam-content-v1:8bde975150a3ed6bf4208a555090a7237d87dd40643fd051e3c09bac7b633c14";
    ResourceLibrary library;
    LibraryResource local;
    local.type = "nam";
    local.id = "local:6505";
    local.name = "6505 chugs";
    local.metadata[kContentHashMetadataKey] = hash;
    library.AddResource(local);

    // A shared preset names the Tone3000 model it used; this library has it from a folder.
    nlohmann::json reference = {{"id", "tone3000:579453"},
                                {"type", "nam"},
                                {"provider", "tone3000"},
                                {"providerKey", "579453"},
                                {"hash", "0123456789abcdef"},
                                {"providerData", {{"toneId", "57741"}, {"modelId", "579453"}}}};
    bool ok = ExpectEq(FindRemoteResource(library, reference), "", "a reference without a content hash");

    reference["contentHash"] = hash;
    ok &= ExpectEq(FindRemoteResource(library, reference), "local:6505", "a reference by content hash");

    reference["type"] = "ir";
    ok &= ExpectEq(FindRemoteResource(library, reference), "", "a reference of another type");

    reference["type"] = "nam";
    reference["contentHash"] = "nam-content-v1:not-a-hash";
    ok &= ExpectEq(FindRemoteResource(library, reference), "", "a malformed content hash");
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

    run("The trainer's test vectors", TestTrainerVectors());
    run("Literals, member order and malformed models", TestRawLiterals());
    run("Testdata captures and IRs match the API's hashes", TestTestdataFiles());
    run("An IR's hash is its data chunk's", TestIrChunks());
    run("The library keeps a content hash on NAM and IR rows", TestLibraryKeepsContentHash());
    run("An archive's reference finds the same model by content hash", TestArchiveReferenceFindsTheSameModel());

    std::cout << "\nContent hash tests: " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
