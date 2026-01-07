#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "presets/PresetTypes.h"

namespace fs = std::filesystem;

namespace
{

// Minimal IByteChunk implementation for testing serialization
class TestByteChunk
{
public:
  int PutBytes(const void* data, int size)
  {
    const auto* bytes = static_cast<const char*>(data);
    mData.insert(mData.end(), bytes, bytes + size);
    return static_cast<int>(mData.size());
  }

  int GetBytes(void* dest, int size, int startPos) const
  {
    if (startPos < 0 || startPos + size > static_cast<int>(mData.size()))
    {
      return -1;
    }
    std::memcpy(dest, mData.data() + startPos, size);
    return startPos + size;
  }

  [[nodiscard]] int Size() const { return static_cast<int>(mData.size()); }
  void Clear() { mData.clear(); }
  [[nodiscard]] const std::vector<char>& Data() const { return mData; }

private:
  std::vector<char> mData;
};

// JSON serialization functions matching PresetStorage implementation
nlohmann::json SerializePreset(const namguitar::Preset& preset)
{
  nlohmann::json jsonPreset;
  jsonPreset["id"] = preset.id;
  jsonPreset["name"] = preset.name;
  jsonPreset["category"] = preset.category;
  jsonPreset["description"] = preset.description;
  jsonPreset["audioFxModelId"] = preset.audioFxModelId;
  jsonPreset["irId"] = preset.irId;
  jsonPreset["fxChain"] = preset.fxChain;

  nlohmann::json attachments = nlohmann::json::array();
  for (const auto& attachment : preset.attachments)
  {
    nlohmann::json jsonAttachment = {
        {"type", attachment.type},
        {"id", attachment.id},
        {"filePath", attachment.filePath.generic_string()},
        {"hash", attachment.hash},
    };
    // Only include data field if non-empty to avoid bloating JSON
    if (!attachment.data.empty())
    {
      jsonAttachment["data"] = attachment.data;
    }
    attachments.push_back(std::move(jsonAttachment));
  }
  jsonPreset["attachments"] = std::move(attachments);

  nlohmann::json parameters = nlohmann::json::array();
  for (const auto& parameter : preset.parameters)
  {
    parameters.push_back({
        {"id", parameter.id},
        {"value", parameter.value},
    });
  }
  jsonPreset["parameters"] = std::move(parameters);

  return jsonPreset;
}

nlohmann::json SerializeAllPresets(const std::vector<namguitar::Preset>& presets)
{
  nlohmann::json jsonRoot;
  jsonRoot["presets"] = nlohmann::json::array();
  for (const auto& preset : presets)
  {
    jsonRoot["presets"].push_back(SerializePreset(preset));
  }
  return jsonRoot;
}

namguitar::Preset DeserializePreset(const nlohmann::json& jsonPreset)
{
  namguitar::Preset preset;
  preset.id = jsonPreset.value("id", "");
  preset.name = jsonPreset.value("name", "");
  preset.category = jsonPreset.value("category", "");
  preset.description = jsonPreset.value("description", "");
  preset.audioFxModelId = jsonPreset.value("audioFxModelId", "");
  preset.irId = jsonPreset.value("irId", "");

  if (jsonPreset.contains("fxChain") && jsonPreset["fxChain"].is_array())
  {
    for (const auto& fx : jsonPreset["fxChain"])
    {
      preset.fxChain.push_back(fx.get<std::string>());
    }
  }

  if (jsonPreset.contains("attachments") && jsonPreset["attachments"].is_array())
  {
    for (const auto& jsonAttachment : jsonPreset["attachments"])
    {
      namguitar::PresetAttachment attachment;
      attachment.type = jsonAttachment.value("type", "");
      attachment.id = jsonAttachment.value("id", "");
      attachment.hash = jsonAttachment.value("hash", "");
      attachment.filePath = jsonAttachment.value("filePath", "");
      attachment.data = jsonAttachment.value("data", "");
      preset.attachments.push_back(std::move(attachment));
    }
  }

  if (jsonPreset.contains("parameters") && jsonPreset["parameters"].is_array())
  {
    for (const auto& jsonParameter : jsonPreset["parameters"])
    {
      namguitar::PresetParameter parameter;
      parameter.id = jsonParameter.value("id", "");
      parameter.value = jsonParameter.value("value", 0.0);
      preset.parameters.push_back(parameter);
    }
  }

  return preset;
}

std::vector<namguitar::Preset> DeserializeAllPresets(const std::string& serialized)
{
  std::vector<namguitar::Preset> presets;
  if (serialized.empty())
  {
    return presets;
  }

  nlohmann::json jsonRoot = nlohmann::json::parse(serialized, nullptr, false);
  if (jsonRoot.is_discarded())
  {
    return presets;
  }

  if (!jsonRoot.contains("presets") || !jsonRoot["presets"].is_array())
  {
    return presets;
  }

  for (const auto& jsonPreset : jsonRoot["presets"])
  {
    presets.push_back(DeserializePreset(jsonPreset));
  }

  return presets;
}

// Helper to create a test preset with attachments
namguitar::Preset CreateTestPreset(
    const std::string& id,
    const std::string& name,
    const std::string& modelId,
    const std::string& irId,
    bool includeAttachments = true)
{
  namguitar::Preset preset;
  preset.id = id;
  preset.name = name;
  preset.category = "Test";
  preset.description = "Test preset for unit testing";
  preset.audioFxModelId = modelId;
  preset.irId = irId;
  preset.fxChain = {"noise_gate", "eq"};

  if (includeAttachments)
  {
    // NAM model attachment
    namguitar::PresetAttachment modelAttachment;
    modelAttachment.type = "nam";
    modelAttachment.id = modelId;
    modelAttachment.filePath = fs::path("amps") / (modelId + ".nam");
    modelAttachment.hash = "abc123def456";
    preset.attachments.push_back(modelAttachment);

    // IR attachment
    namguitar::PresetAttachment irAttachment;
    irAttachment.type = "ir";
    irAttachment.id = irId;
    irAttachment.filePath = fs::path("ir") / (irId + ".wav");
    irAttachment.hash = "789xyz012abc";
    preset.attachments.push_back(irAttachment);
  }

  // Parameters
  preset.parameters = {
      {"input_trim", -3.0},
      {"output_trim", 0.0},
      {"drive", 0.5},
      {"tone", 0.6},
  };

  return preset;
}

// Comparison helpers
bool AttachmentsEqual(const namguitar::PresetAttachment& a, const namguitar::PresetAttachment& b)
{
  return a.type == b.type && a.id == b.id && a.filePath == b.filePath && a.hash == b.hash && a.data == b.data;
}

bool ParametersEqual(const namguitar::PresetParameter& a, const namguitar::PresetParameter& b)
{
  constexpr double kEpsilon = 1e-9;
  return a.id == b.id && std::abs(a.value - b.value) < kEpsilon;
}

bool PresetsEqual(const namguitar::Preset& a, const namguitar::Preset& b)
{
  if (a.id != b.id || a.name != b.name || a.category != b.category || a.description != b.description ||
      a.audioFxModelId != b.audioFxModelId || a.irId != b.irId)
  {
    return false;
  }

  if (a.fxChain != b.fxChain)
  {
    return false;
  }

  if (a.attachments.size() != b.attachments.size())
  {
    return false;
  }

  for (size_t i = 0; i < a.attachments.size(); ++i)
  {
    if (!AttachmentsEqual(a.attachments[i], b.attachments[i]))
    {
      return false;
    }
  }

  if (a.parameters.size() != b.parameters.size())
  {
    return false;
  }

  for (size_t i = 0; i < a.parameters.size(); ++i)
  {
    if (!ParametersEqual(a.parameters[i], b.parameters[i]))
    {
      return false;
    }
  }

  return true;
}

// Test framework
int gTestsPassed = 0;
int gTestsFailed = 0;

void ReportTest(const std::string& testName, bool passed, const std::string& message = "")
{
  if (passed)
  {
    std::cout << "  [PASS] " << testName << "\n";
    ++gTestsPassed;
  }
  else
  {
    std::cout << "  [FAIL] " << testName;
    if (!message.empty())
    {
      std::cout << " - " << message;
    }
    std::cout << "\n";
    ++gTestsFailed;
  }
}

// Test cases
void TestSinglePresetSerializationRoundTrip()
{
  std::cout << "\n=== Single Preset Serialization Round-Trip ===\n";

  const auto original = CreateTestPreset("test-001", "Test Preset", "model-abc", "ir-xyz");

  const nlohmann::json serialized = SerializePreset(original);
  const auto deserialized = DeserializePreset(serialized);

  ReportTest("Basic fields preserved", original.id == deserialized.id && original.name == deserialized.name &&
                                           original.category == deserialized.category &&
                                           original.description == deserialized.description);

  ReportTest("Model and IR IDs preserved",
             original.audioFxModelId == deserialized.audioFxModelId && original.irId == deserialized.irId);

  ReportTest("FX chain preserved", original.fxChain == deserialized.fxChain);

  ReportTest("Attachment count preserved", original.attachments.size() == deserialized.attachments.size());

  if (original.attachments.size() == deserialized.attachments.size() && !original.attachments.empty())
  {
    bool allAttachmentsMatch = true;
    for (size_t i = 0; i < original.attachments.size(); ++i)
    {
      if (!AttachmentsEqual(original.attachments[i], deserialized.attachments[i]))
      {
        allAttachmentsMatch = false;
        std::cout << "    Attachment " << i << " mismatch:\n";
        std::cout << "      Original: type=" << original.attachments[i].type << ", id=" << original.attachments[i].id
                  << ", path=" << original.attachments[i].filePath << ", hash=" << original.attachments[i].hash
                  << "\n";
        std::cout << "      Deserialized: type=" << deserialized.attachments[i].type
                  << ", id=" << deserialized.attachments[i].id << ", path=" << deserialized.attachments[i].filePath
                  << ", hash=" << deserialized.attachments[i].hash << "\n";
      }
    }
    ReportTest("All attachments match", allAttachmentsMatch);
  }

  ReportTest("Parameter count preserved", original.parameters.size() == deserialized.parameters.size());

  if (original.parameters.size() == deserialized.parameters.size() && !original.parameters.empty())
  {
    bool allParamsMatch = true;
    for (size_t i = 0; i < original.parameters.size(); ++i)
    {
      if (!ParametersEqual(original.parameters[i], deserialized.parameters[i]))
      {
        allParamsMatch = false;
      }
    }
    ReportTest("All parameters match", allParamsMatch);
  }

  ReportTest("Full preset equality", PresetsEqual(original, deserialized));
}

void TestMultiplePresetsSerializationRoundTrip()
{
  std::cout << "\n=== Multiple Presets Serialization Round-Trip ===\n";

  std::vector<namguitar::Preset> originals = {CreateTestPreset("preset-1", "Clean Tone", "model-clean", "ir-neutral"),
                                              CreateTestPreset("preset-2", "Crunch", "model-crunch", "ir-warm"),
                                              CreateTestPreset("preset-3", "High Gain", "model-metal", "ir-bright")};

  const nlohmann::json serialized = SerializeAllPresets(originals);
  const std::string jsonStr = serialized.dump();
  const auto deserialized = DeserializeAllPresets(jsonStr);

  ReportTest("Preset count preserved", originals.size() == deserialized.size());

  bool allPresetsMatch = true;
  for (size_t i = 0; i < originals.size() && i < deserialized.size(); ++i)
  {
    if (!PresetsEqual(originals[i], deserialized[i]))
    {
      allPresetsMatch = false;
      std::cout << "    Preset " << i << " (" << originals[i].id << ") mismatch\n";
    }
  }
  ReportTest("All presets match", allPresetsMatch);
}

void TestEmptyAttachmentsHandling()
{
  std::cout << "\n=== Empty Attachments Handling ===\n";

  const auto original = CreateTestPreset("no-attach", "No Attachments", "model-x", "ir-y", false);

  ReportTest("Original has no attachments", original.attachments.empty());

  const nlohmann::json serialized = SerializePreset(original);
  const auto deserialized = DeserializePreset(serialized);

  ReportTest("Deserialized has no attachments", deserialized.attachments.empty());
  ReportTest("Full preset equality (no attachments)", PresetsEqual(original, deserialized));
}

void TestAttachmentPathNormalization()
{
  std::cout << "\n=== Attachment Path Normalization ===\n";

  namguitar::Preset preset;
  preset.id = "path-test";
  preset.name = "Path Test";

  // Test with various path formats
  namguitar::PresetAttachment attachment1;
  attachment1.type = "nam";
  attachment1.id = "model1";
  attachment1.filePath = fs::path("path/to/model.nam");
  attachment1.hash = "hash1";

  namguitar::PresetAttachment attachment2;
  attachment2.type = "ir";
  attachment2.id = "ir1";
  attachment2.filePath = fs::path("path\\to\\ir.wav"); // Windows-style path
  attachment2.hash = "hash2";

  preset.attachments = {attachment1, attachment2};

  const nlohmann::json serialized = SerializePreset(preset);
  const auto deserialized = DeserializePreset(serialized);

  // Paths should be normalized to generic (forward-slash) format in JSON
  const std::string serializedStr = serialized.dump();
  const bool pathsNormalized = serializedStr.find("path/to/model.nam") != std::string::npos;
  ReportTest("Paths normalized in JSON", pathsNormalized);

  ReportTest("Attachment count preserved after path handling", preset.attachments.size() == deserialized.attachments.size());
}

void TestEmptyAndMissingFields()
{
  std::cout << "\n=== Empty and Missing Fields ===\n";

  // Test with minimal preset (empty optional fields)
  namguitar::Preset minimal;
  minimal.id = "minimal";
  minimal.name = "Minimal Preset";
  // Leave other fields empty/default

  const nlohmann::json serialized = SerializePreset(minimal);
  const auto deserialized = DeserializePreset(serialized);

  ReportTest("Minimal preset ID preserved", minimal.id == deserialized.id);
  ReportTest("Minimal preset name preserved", minimal.name == deserialized.name);
  ReportTest("Empty category handled", minimal.category == deserialized.category);
  ReportTest("Empty audioFxModelId handled", minimal.audioFxModelId == deserialized.audioFxModelId);
  ReportTest("Empty irId handled", minimal.irId == deserialized.irId);
  ReportTest("Empty fxChain handled", minimal.fxChain == deserialized.fxChain);
  ReportTest("Empty attachments handled", minimal.attachments.size() == deserialized.attachments.size());
  ReportTest("Empty parameters handled", minimal.parameters.size() == deserialized.parameters.size());
}

void TestMissingAttachmentFieldsInJSON()
{
  std::cout << "\n=== Missing Attachment Fields in JSON ===\n";

  // Simulate JSON that's missing some attachment fields (e.g., from older format)
  const std::string jsonStr = R"({
    "presets": [{
      "id": "partial-attach",
      "name": "Partial Attachment",
      "audioFxModelId": "model-1",
      "irId": "ir-1",
      "attachments": [
        {"type": "nam", "id": "model-1"},
        {"type": "ir", "filePath": "ir/test.wav", "hash": "hashvalue"}
      ]
    }]
  })";

  const auto presets = DeserializeAllPresets(jsonStr);

  ReportTest("Preset parsed with partial attachments", presets.size() == 1);

  if (!presets.empty())
  {
    const auto& preset = presets[0];
    ReportTest("Attachment count is 2", preset.attachments.size() == 2);

    if (preset.attachments.size() >= 2)
    {
      // First attachment: has type and id, missing filePath and hash
      ReportTest("First attachment type preserved", preset.attachments[0].type == "nam");
      ReportTest("First attachment id preserved", preset.attachments[0].id == "model-1");
      ReportTest("First attachment missing filePath is empty", preset.attachments[0].filePath.empty());
      ReportTest("First attachment missing hash is empty", preset.attachments[0].hash.empty());

      // Second attachment: has type, filePath, hash, missing id
      ReportTest("Second attachment type preserved", preset.attachments[1].type == "ir");
      ReportTest("Second attachment missing id is empty", preset.attachments[1].id.empty());
      ReportTest("Second attachment filePath preserved", preset.attachments[1].filePath == fs::path("ir/test.wav"));
      ReportTest("Second attachment hash preserved", preset.attachments[1].hash == "hashvalue");
    }
  }
}

void TestAttachmentDataField()
{
  std::cout << "\n=== Attachment Data Field ===\n";

  // The 'data' field is used for inline payload (e.g., base64)
  namguitar::PresetAttachment attachment;
  attachment.type = "nam";
  attachment.id = "inline-model";
  attachment.data = "base64encodeddata...";
  attachment.hash = "datahash";

  namguitar::Preset preset;
  preset.id = "data-test";
  preset.name = "Data Field Test";
  preset.attachments = {attachment};

  const nlohmann::json serialized = SerializePreset(preset);
  
  // Check if data field is serialized
  const std::string serializedStr = serialized.dump();
  const bool dataFieldSerialized = serializedStr.find("base64encodeddata") != std::string::npos;
  
  ReportTest("Data field serialized when non-empty", dataFieldSerialized);
  
  // Verify round-trip
  const auto deserialized = DeserializePreset(serialized);
  ReportTest("Data field preserved after round-trip", 
             !deserialized.attachments.empty() && deserialized.attachments[0].data == "base64encodeddata...");
  
  // Test that empty data field is not serialized
  namguitar::PresetAttachment attachmentNoData;
  attachmentNoData.type = "ir";
  attachmentNoData.id = "ir-no-data";
  attachmentNoData.hash = "hash123";
  
  namguitar::Preset presetNoData;
  presetNoData.id = "no-data-test";
  presetNoData.name = "No Data Field Test";
  presetNoData.attachments = {attachmentNoData};
  
  const nlohmann::json serializedNoData = SerializePreset(presetNoData);
  const std::string serializedNoDataStr = serializedNoData.dump();
  const bool dataKeyAbsent = serializedNoDataStr.find("\"data\"") == std::string::npos;
  ReportTest("Data key omitted when empty", dataKeyAbsent);
}

void TestInvalidJsonHandling()
{
  std::cout << "\n=== Invalid JSON Handling ===\n";

  // Empty string
  auto result1 = DeserializeAllPresets("");
  ReportTest("Empty string returns empty vector", result1.empty());

  // Invalid JSON
  auto result2 = DeserializeAllPresets("{not valid json}");
  ReportTest("Invalid JSON returns empty vector", result2.empty());

  // Valid JSON but wrong structure
  auto result3 = DeserializeAllPresets(R"({"not_presets": []})");
  ReportTest("Missing 'presets' key returns empty vector", result3.empty());

  // Presets key is not an array
  auto result4 = DeserializeAllPresets(R"({"presets": "not an array"})");
  ReportTest("Non-array 'presets' returns empty vector", result4.empty());
}

void TestByteChunkSerializationSimulation()
{
  std::cout << "\n=== Byte Chunk Serialization Simulation ===\n";

  // Simulate what PresetStorage::Serialize does
  const std::vector<namguitar::Preset> originals = {
      CreateTestPreset("chunk-1", "Chunk Test 1", "model-a", "ir-a"),
      CreateTestPreset("chunk-2", "Chunk Test 2", "model-b", "ir-b"),
  };

  // Serialize to chunk
  TestByteChunk chunk;
  const nlohmann::json serialized = SerializeAllPresets(originals);
  const std::string payload = serialized.dump();
  const uint32_t size = static_cast<uint32_t>(payload.size());
  chunk.PutBytes(&size, sizeof(size));
  chunk.PutBytes(payload.data(), static_cast<int>(payload.size()));

  ReportTest("Chunk has data", chunk.Size() > 0);

  // Deserialize from chunk (simulating Unserialize)
  uint32_t readSize = 0;
  int position = chunk.GetBytes(&readSize, sizeof(readSize), 0);
  ReportTest("Size read correctly", readSize == size && position == sizeof(size));

  std::string readPayload(readSize, '\0');
  position = chunk.GetBytes(readPayload.data(), static_cast<int>(readSize), position);
  ReportTest("Payload read correctly", position == static_cast<int>(sizeof(size) + readSize));

  const auto deserialized = DeserializeAllPresets(readPayload);
  ReportTest("Deserialized preset count matches", deserialized.size() == originals.size());

  bool allMatch = true;
  for (size_t i = 0; i < originals.size() && i < deserialized.size(); ++i)
  {
    if (!PresetsEqual(originals[i], deserialized[i]))
    {
      allMatch = false;
    }
  }
  ReportTest("All presets match after byte chunk round-trip", allMatch);
}

void TestModelAndIRConsistency()
{
  std::cout << "\n=== Model and IR ID/Attachment Consistency ===\n";

  // Test that audioFxModelId and irId are independent from attachments
  namguitar::Preset preset;
  preset.id = "consistency-test";
  preset.name = "Consistency Test";
  preset.audioFxModelId = "main-model";
  preset.irId = "main-ir";

  // Attachments reference different IDs (this is valid - presets can have multiple models)
  namguitar::PresetAttachment extraModel;
  extraModel.type = "nam";
  extraModel.id = "alternate-model";
  extraModel.filePath = "amps/alternate.nam";

  preset.attachments = {extraModel};

  const nlohmann::json serialized = SerializePreset(preset);
  const auto deserialized = DeserializePreset(serialized);

  ReportTest("audioFxModelId preserved", deserialized.audioFxModelId == "main-model");
  ReportTest("irId preserved", deserialized.irId == "main-ir");
  ReportTest("Attachment ID preserved (different from audioFxModelId)",
             !deserialized.attachments.empty() && deserialized.attachments[0].id == "alternate-model");
}

} // namespace

int main()
{
  std::cout << "================================================\n";
  std::cout << "   Preset Storage Serialization Tests\n";
  std::cout << "================================================\n";

  TestSinglePresetSerializationRoundTrip();
  TestMultiplePresetsSerializationRoundTrip();
  TestEmptyAttachmentsHandling();
  TestAttachmentPathNormalization();
  TestEmptyAndMissingFields();
  TestMissingAttachmentFieldsInJSON();
  TestAttachmentDataField();
  TestInvalidJsonHandling();
  TestByteChunkSerializationSimulation();
  TestModelAndIRConsistency();

  std::cout << "\n================================================\n";
  std::cout << "Results: " << gTestsPassed << " passed, " << gTestsFailed << " failed\n";
  std::cout << "================================================\n";

  return gTestsFailed > 0 ? 1 : 0;
}
