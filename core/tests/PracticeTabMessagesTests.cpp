/**
 * PracticeTabMessagesTests.cpp — the Practice Tool's tablature files
 * (controller/PracticeTabMessages.h): a browsed or dropped score is stored under its
 * content hash and handed back, the same file twice is one row, a recalled id comes
 * back with its bytes, and a missing id, an oversized file or a cancelled dialog
 * stores nothing.
 */

#include "MessageHandlerRegistry.h"
#include "controller/PracticeTabMessages.h"
#include "storage/JsonStore.h"
#include "util/Base64.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using guitarfx::MessageHandlerRegistry;

namespace
{
int gFailures = 0;

void Check(bool condition, const char* what)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << what << "\n";
        ++gFailures;
    }
}

struct Harness
{
    fs::path folder;
    guitarfx::storage::JsonStore store;
    MessageHandlerRegistry registry;
    std::vector<nlohmann::json> sent;
    std::vector<std::string> errors;
    guitarfx::BrowseFileResult nextBrowse;

    Harness()
    {
        folder = fs::temp_directory_path() /
                 ("ssg-practice-tab-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(folder);
        std::string error;
        Check(store.Open(folder / "store.db", error), "the store opens");

        guitarfx::RegisterPracticeTabMessages(
            registry, {[this](std::function<void(const guitarfx::BrowseFileResult&)> done) { done(nextBrowse); },
                       [this]() -> guitarfx::storage::JsonStore& { return store; },
                       [this](const std::string& json) { sent.push_back(nlohmann::json::parse(json)); },
                       [this](const std::string& title, const std::string&) { errors.push_back(title); }});
    }

    ~Harness()
    {
        store.Close();
        std::error_code ignored;
        fs::remove_all(folder, ignored);
    }

    void Send(const nlohmann::json& message)
    {
        registry.Dispatch(message.value("type", ""), message);
    }
};

std::vector<std::uint8_t> SampleScore()
{
    const std::string text = "\\title \"Test\" . 3.3 5.3 7.3 | 3.2 5.2 7.2";
    return {text.begin(), text.end()};
}

void TestRegistersEveryMessage()
{
    Harness harness;
    Check(harness.registry.Handles("browsePracticeToolTab"), "browsePracticeToolTab is answered");
    Check(harness.registry.Handles("importPracticeToolTabData"), "importPracticeToolTabData is answered");
    Check(harness.registry.Handles("requestPracticeToolTab"), "requestPracticeToolTab is answered");
}

void TestDroppedFileIsStoredAndRecalled()
{
    Harness harness;
    const auto bytes = SampleScore();
    const std::string data = guitarfx::util::EncodeBase64(bytes);

    harness.Send({{"type", "importPracticeToolTabData"}, {"fileName", "C:\\songs\\riff.atex"}, {"data", data}});
    Check(harness.sent.size() == 1, "a dropped score is answered once");
    const auto loaded = harness.sent.empty() ? nlohmann::json::object() : harness.sent.back();
    Check(loaded.value("type", "") == "practiceToolTabLoaded", "answered with practiceToolTabLoaded");
    Check(loaded.value("fileName", "") == "riff.atex", "the folders are dropped from the name");
    Check(loaded.value("data", "") == data, "the bytes come straight back");
    const std::string id = loaded.value("id", "");
    Check(!id.empty(), "the score has an id");

    harness.Send({{"type", "importPracticeToolTabData"}, {"fileName", "copy.atex"}, {"data", data}});
    Check(harness.sent.back().value("id", "") == id, "the same bytes get the same id");
    Check(harness.store.Count(guitarfx::storage::ItemType::kPracticeTab) == 1, "and are stored once");

    harness.Send({{"type", "requestPracticeToolTab"}, {"id", id}});
    Check(harness.sent.back().value("type", "") == "practiceToolTabLoaded", "a stored id is answered");
    Check(harness.sent.back().value("data", "") == data, "with its bytes");
    Check(harness.sent.back().value("fileName", "") == "copy.atex", "and the name it was last opened under");

    harness.Send({{"type", "requestPracticeToolTab"}, {"id", "not-there"}});
    Check(harness.sent.back().value("type", "") == "practiceToolTabFailed", "a missing id fails");
    Check(harness.sent.back().value("id", "") == "not-there", "naming the id it was asked for");
    Check(harness.errors.empty(), "none of that reported an error");
}

void TestBrowse()
{
    Harness harness;
    harness.Send({{"type", "browsePracticeToolTab"}});
    Check(harness.sent.empty() && harness.errors.empty(), "a cancelled dialog does nothing");

    const fs::path file = harness.folder / "song.gp5";
    const auto bytes = SampleScore();
    {
        std::ofstream out(file, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    harness.nextBrowse = {true, file};
    harness.Send({{"type", "browsePracticeToolTab"}});
    Check(harness.sent.size() == 1 && harness.sent.back().value("fileName", "") == "song.gp5",
          "a browsed file is stored and answered under its name");
    Check(harness.store.Count(guitarfx::storage::ItemType::kPracticeTab) == 1, "and kept");
}

void TestRefusals()
{
    Harness harness;
    harness.Send({{"type", "importPracticeToolTabData"}, {"fileName", "empty.gp"}, {"data", ""}});
    Check(harness.sent.empty() && harness.errors.size() == 1, "an empty file is refused");

    const std::vector<std::uint8_t> huge(guitarfx::kMaxPracticeTabBytes + 1, 0x41);
    harness.Send(
        {{"type", "importPracticeToolTabData"}, {"fileName", "huge.gp"}, {"data", guitarfx::util::EncodeBase64(huge)}});
    Check(harness.sent.empty() && harness.errors.size() == 2, "a file over the cap is refused");
    Check(harness.store.Count(guitarfx::storage::ItemType::kPracticeTab) == 0, "and nothing is stored");
}
} // namespace

int main()
{
    TestRegistersEveryMessage();
    TestDroppedFileIsStoredAndRecalled();
    TestBrowse();
    TestRefusals();

    if (gFailures == 0)
    {
        std::cout << "PracticeTabMessagesTests passed\n";
        return 0;
    }

    std::cerr << gFailures << " check(s) failed\n";
    return 1;
}
