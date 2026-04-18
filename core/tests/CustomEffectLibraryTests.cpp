#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

#include "resources/CustomEffectLibrary.h"

namespace fs = std::filesystem;

namespace
{
bool TestRoundTrip()
{
  guitarfx::CustomEffectLibrary library;
  guitarfx::CustomEffectLibraryEntry entry;
  entry.id = "airy-widen";
  entry.name = "Airy Widen";
  entry.category = "modulation";
  entry.description = "Gentle stereo widener with subtle movement.";
  entry.baseEffectType = "b96693c3-b1f4-44f8-99b4-016620f89d95";
  entry.moduleResourceType = "wasm";
  entry.moduleResourceId = "custom-effect:airy-widen:rev-0003";
  entry.latestRevisionId = "rev-0003";
  entry.thumbnailDataUrl = "data:image/png;base64,AAAA";
  entry.tags = {"custom", "generated", "stereo"};
  entry.defaultParams = {{"depth", 0.35}, {"rate", 0.22}};
  entry.descriptorSummary = {
    {"displayName", "Airy Widen"},
    {"category", "modulation"},
    {"parameterCount", 2},
    {"resourceCount", 0},
  };
  entry.origin = "generated";
  entry.createdAt = "2026-04-17T12:34:56Z";
  entry.updatedAt = "2026-04-17T13:10:00Z";

  library.UpsertEntry(entry);

  const auto uniqueId = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const fs::path tempDir = fs::temp_directory_path() / ("soundshed-custom-effect-library-test-" + uniqueId);
  const fs::path indexPath = tempDir / "custom-effects" / "indexes" / "custom-effects-index.json";

  std::error_code removeEc;
  fs::remove_all(tempDir, removeEc);
  library.SaveToFile(indexPath);

  guitarfx::CustomEffectLibrary loaded;
  loaded.LoadFromFile(indexPath);

  const auto* loadedEntry = loaded.GetEntry("airy-widen");
  const bool ok = loadedEntry
    && loadedEntry->moduleResourceId == entry.moduleResourceId
    && loadedEntry->moduleResourceType == entry.moduleResourceType
    && loadedEntry->defaultParams == entry.defaultParams
    && loadedEntry->descriptorSummary.value("parameterCount", -1) == 2
    && loadedEntry->tags.size() == 3;

  if (!ok)
  {
    std::cerr << "CustomEffectLibrary failed to round-trip saved entry data.\n";
  }

  fs::remove_all(tempDir, removeEc);
  return ok;
}

bool TestRemoveEntry()
{
  guitarfx::CustomEffectLibrary library;

  guitarfx::CustomEffectLibraryEntry entry;
  entry.id = "tape-drift";
  entry.name = "Tape Drift";
  entry.category = "modulation";
  entry.baseEffectType = "b96693c3-b1f4-44f8-99b4-016620f89d95";
  entry.moduleResourceType = "wasm";
  entry.moduleResourceId = "custom-effect:tape-drift:rev-0001";

  library.UpsertEntry(entry);
  if (!library.RemoveEntry(entry.id))
  {
    std::cerr << "CustomEffectLibrary could not remove an existing entry.\n";
    return false;
  }
  if (library.GetEntry(entry.id) != nullptr)
  {
    std::cerr << "CustomEffectLibrary still returned an entry after removal.\n";
    return false;
  }
  if (library.RemoveEntry(entry.id))
  {
    std::cerr << "CustomEffectLibrary reported removal for a missing entry.\n";
    return false;
  }
  return true;
}
} // namespace

int main()
{
  bool allPassed = true;
  const auto runTest = [&](bool (*test)(), const char* label)
  {
    const bool passed = test();
    std::cout << label << (passed ? " PASS" : " FAIL") << std::endl;
    allPassed = allPassed && passed;
  };

  runTest(TestRoundTrip, "CustomEffectLibrary round-trips entries:");
  runTest(TestRemoveEntry, "CustomEffectLibrary removes entries:");

  return allPassed ? 0 : 1;
}