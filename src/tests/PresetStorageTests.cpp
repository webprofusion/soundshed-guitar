#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "presets/PresetTypes.h"
#include "presets/PresetStorage.h"

namespace fs = std::filesystem;

namespace
{

// Helper to create a test preset with signal graph
guitarfx::Preset CreateTestPreset(
    const std::string& id,
    const std::string& name,
    const std::string& modelPath = "",
    const std::string& irPath = "")
{
  guitarfx::Preset preset;
  preset.id = id;
  preset.name = name;
  preset.category = "Test";
  preset.description = "Test preset for unit testing";
  preset.version = 2;
  preset.author = "Test Author";

  // Global settings
  preset.global.inputTrim = -3.0;
  preset.global.outputTrim = 1.5;
  preset.global.transpose = 0;

  // Build signal graph
  
  // Input node
  guitarfx::GraphNode inputNode;
  inputNode.id = "input";
  inputNode.type = guitarfx::kNodeTypeInput;
  inputNode.category = "routing";
  preset.graph.nodes.push_back(inputNode);

  // Noise gate node
  guitarfx::GraphNode gateNode;
  gateNode.id = "gate";
  gateNode.type = "dynamics_gate";
  gateNode.category = "dynamics";
  gateNode.enabled = true;
  gateNode.params["threshold"] = -60.0;
  preset.graph.nodes.push_back(gateNode);

  // NAM amp node (if model path provided)
  if (!modelPath.empty())
  {
    guitarfx::GraphNode ampNode;
    ampNode.id = "amp";
    ampNode.type = "amp_nam";
    ampNode.category = "amp";
    ampNode.enabled = true;
    ampNode.params["drive"] = 0.5;
    ampNode.params["tone"] = 0.6;
    guitarfx::ResourceRef ampRef;
    ampRef.filePath = fs::path(modelPath);
    ampNode.resources.push_back(ampRef);
    preset.graph.nodes.push_back(ampNode);
  }

  // IR cab node (if IR path provided)
  if (!irPath.empty())
  {
    guitarfx::GraphNode cabNode;
    cabNode.id = "cab";
    cabNode.type = "ir_cab";
    cabNode.category = "cab";
    cabNode.enabled = true;
    guitarfx::ResourceRef cabRef;
    cabRef.filePath = fs::path(irPath);
    cabNode.resources.push_back(cabRef);
    preset.graph.nodes.push_back(cabNode);
  }

  // EQ node
  guitarfx::GraphNode eqNode;
  eqNode.id = "eq";
  eqNode.type = "eq_parametric";
  eqNode.category = "eq";
  eqNode.enabled = false;
  eqNode.params["lowGain"] = 0.0;
  eqNode.params["lowFreq"] = 100.0;
  preset.graph.nodes.push_back(eqNode);

  // Output node
  guitarfx::GraphNode outputNode;
  outputNode.id = "output";
  outputNode.type = guitarfx::kNodeTypeOutput;
  outputNode.category = "routing";
  preset.graph.nodes.push_back(outputNode);

  // Build edges (linear chain)
  for (size_t i = 0; i < preset.graph.nodes.size() - 1; ++i)
  {
    guitarfx::GraphEdge edge;
    edge.from = preset.graph.nodes[i].id;
    edge.to = preset.graph.nodes[i + 1].id;
    preset.graph.edges.push_back(edge);
  }

  return preset;
}

// Helper to create a minimal preset
guitarfx::Preset CreateMinimalPreset(const std::string& id, const std::string& name)
{
  guitarfx::Preset preset;
  preset.id = id;
  preset.name = name;
  preset.version = 2;
  return preset;
}

// Test fixture base for preset tests
class PresetTestFixture
{
public:
  PresetTestFixture()
  {
    mTempDir = fs::temp_directory_path() / "nam_preset_tests";
    fs::create_directories(mTempDir);
  }

  ~PresetTestFixture()
  {
    try
    {
      fs::remove_all(mTempDir);
    }
    catch (...)
    {
    }
  }

  fs::path mTempDir;
};

// Test: Serialize and deserialize a preset
bool TestSerializeDeserialize()
{
  std::cout << "Test: SerializeDeserialize... ";

  auto original = CreateTestPreset("test-1", "Test Preset", "models/amp.nam", "ir/cab.wav");

  // Serialize
  std::string json = guitarfx::PresetStorage::SerializeToJson(original);
  if (json.empty())
  {
    std::cout << "FAIL (serialization returned empty string)" << std::endl;
    return false;
  }

  // Deserialize
  auto deserialized = guitarfx::PresetStorage::DeserializeFromJson(json);
  if (!deserialized)
  {
    std::cout << "FAIL (deserialization failed)" << std::endl;
    return false;
  }

  // Verify metadata
  if (deserialized->id != original.id ||
      deserialized->name != original.name ||
      deserialized->category != original.category ||
      deserialized->description != original.description ||
      deserialized->version != original.version)
  {
    std::cout << "FAIL (metadata mismatch)" << std::endl;
    return false;
  }

  // Verify global settings
  if (deserialized->global.inputTrim != original.global.inputTrim ||
      deserialized->global.outputTrim != original.global.outputTrim)
  {
    std::cout << "FAIL (global settings mismatch)" << std::endl;
    return false;
  }

  // Verify graph node count
  if (deserialized->graph.nodes.size() != original.graph.nodes.size())
  {
    std::cout << "FAIL (node count mismatch: " << deserialized->graph.nodes.size() 
              << " vs " << original.graph.nodes.size() << ")" << std::endl;
    return false;
  }

  // Verify graph edge count
  if (deserialized->graph.edges.size() != original.graph.edges.size())
  {
    std::cout << "FAIL (edge count mismatch)" << std::endl;
    return false;
  }

  std::cout << "PASS" << std::endl;
  return true;
}

// Test: Save and load preset from file
bool TestSaveLoadFile()
{
  std::cout << "Test: SaveLoadFile... ";

  PresetTestFixture fixture;
  auto original = CreateTestPreset("file-test", "File Test Preset");

  fs::path filePath = fixture.mTempDir / "test_preset.json";

  // Save
  if (!guitarfx::PresetStorage::SaveToFile(original, filePath))
  {
    std::cout << "FAIL (save failed)" << std::endl;
    return false;
  }

  // Verify file exists
  if (!fs::exists(filePath))
  {
    std::cout << "FAIL (file not created)" << std::endl;
    return false;
  }

  // Load
  auto loaded = guitarfx::PresetStorage::LoadFromFile(filePath);
  if (!loaded)
  {
    std::cout << "FAIL (load failed)" << std::endl;
    return false;
  }

  // Verify
  if (loaded->id != original.id || loaded->name != original.name)
  {
    std::cout << "FAIL (loaded preset does not match)" << std::endl;
    return false;
  }

  std::cout << "PASS" << std::endl;
  return true;
}

// Test: Load all presets from directory
bool TestLoadAllFromDirectory()
{
  std::cout << "Test: LoadAllFromDirectory... ";

  PresetTestFixture fixture;
  fs::path presetDir = fixture.mTempDir / "presets";
  fs::create_directories(presetDir);

  // Save multiple presets
  auto preset1 = CreateMinimalPreset("preset-1", "Preset One");
  auto preset2 = CreateMinimalPreset("preset-2", "Preset Two");
  auto preset3 = CreateMinimalPreset("preset-3", "Preset Three");

  (void)guitarfx::PresetStorage::SaveToFile(preset1, presetDir / "preset1.json");
  (void)guitarfx::PresetStorage::SaveToFile(preset2, presetDir / "preset2.json");
  (void)guitarfx::PresetStorage::SaveToFile(preset3, presetDir / "preset3.json");

  // Create a non-preset file to ensure it's ignored
  std::ofstream(presetDir / "readme.txt") << "not a preset";

  // Load all
  auto allPresets = guitarfx::PresetStorage::LoadAllFromDirectory(presetDir);

  if (allPresets.size() != 3)
  {
    std::cout << "FAIL (expected 3 presets, got " << allPresets.size() << ")" << std::endl;
    return false;
  }

  std::cout << "PASS" << std::endl;
  return true;
}

// Test: Signal graph node finding
bool TestGraphNodeFinding()
{
  std::cout << "Test: GraphNodeFinding... ";

  auto preset = CreateTestPreset("find-test", "Find Test", "test.nam");

  // Find existing node
  const auto* foundNode = preset.graph.FindNode("amp");
  if (!foundNode)
  {
    std::cout << "FAIL (could not find 'amp' node)" << std::endl;
    return false;
  }

  if (foundNode->type != "amp_nam")
  {
    std::cout << "FAIL (found node has wrong type)" << std::endl;
    return false;
  }

  // Find non-existing node
  const auto* notFound = preset.graph.FindNode("nonexistent");
  if (notFound != nullptr)
  {
    std::cout << "FAIL (found non-existent node)" << std::endl;
    return false;
  }

  std::cout << "PASS" << std::endl;
  return true;
}

// Test: ResourceRef validation
bool TestResourceRefValidation()
{
  std::cout << "Test: ResourceRefValidation... ";

  // Empty ref should be invalid
  guitarfx::ResourceRef emptyRef;
  if (emptyRef.IsValid())
  {
    std::cout << "FAIL (empty ref should be invalid)" << std::endl;
    return false;
  }

  // File path ref
  guitarfx::ResourceRef fileRef;
  fileRef.filePath = "path/to/model.nam";
  if (!fileRef.IsFilePath() || !fileRef.IsValid())
  {
    std::cout << "FAIL (file path ref should be valid)" << std::endl;
    return false;
  }

  // Library ref
  guitarfx::ResourceRef libRef;
  libRef.resourceType = "nam";
  libRef.resourceId = "plexi-bright";
  if (!libRef.IsLibraryRef() || !libRef.IsValid())
  {
    std::cout << "FAIL (library ref should be valid)" << std::endl;
    return false;
  }

  // Embedded ref
  guitarfx::ResourceRef embRef;
  embRef.embeddedId = "emb-001";
  if (!embRef.IsEmbedded() || !embRef.IsValid())
  {
    std::cout << "FAIL (embedded ref should be valid)" << std::endl;
    return false;
  }

  std::cout << "PASS" << std::endl;
  return true;
}

// Test: Deserialize invalid JSON
bool TestDeserializeInvalidJson()
{
  std::cout << "Test: DeserializeInvalidJson... ";

  // Invalid JSON
  auto result1 = guitarfx::PresetStorage::DeserializeFromJson("not valid json");
  if (result1)
  {
    std::cout << "FAIL (should fail on invalid JSON)" << std::endl;
    return false;
  }

  // Empty JSON
  auto result2 = guitarfx::PresetStorage::DeserializeFromJson("");
  if (result2)
  {
    std::cout << "FAIL (should fail on empty string)" << std::endl;
    return false;
  }

  std::cout << "PASS" << std::endl;
  return true;
}

// Test: Node parameters serialization
bool TestNodeParams()
{
  std::cout << "Test: NodeParams... ";

  guitarfx::Preset original;
  original.id = "params-test";
  original.name = "Params Test";
  original.version = 2;

  guitarfx::GraphNode node;
  node.id = "test-node";
  node.type = "test_effect";
  node.enabled = true;
  node.params["gain"] = 0.75;
  node.params["frequency"] = 1000.0;
  node.params["q"] = 1.414;
  node.config["mode"] = "stereo";
  node.config["algorithm"] = "vintage";
  original.graph.nodes.push_back(node);

  // Serialize and deserialize
  std::string json = guitarfx::PresetStorage::SerializeToJson(original);
  auto deserialized = guitarfx::PresetStorage::DeserializeFromJson(json);

  if (!deserialized)
  {
    std::cout << "FAIL (deserialization failed)" << std::endl;
    return false;
  }

  if (deserialized->graph.nodes.empty())
  {
    std::cout << "FAIL (no nodes in deserialized preset)" << std::endl;
    return false;
  }

  const auto& deserializedNode = deserialized->graph.nodes[0];
  
  // Check params
  if (deserializedNode.params.count("gain") == 0 ||
      deserializedNode.params.at("gain") != 0.75)
  {
    std::cout << "FAIL (gain param mismatch)" << std::endl;
    return false;
  }

  if (deserializedNode.params.count("frequency") == 0 ||
      deserializedNode.params.at("frequency") != 1000.0)
  {
    std::cout << "FAIL (frequency param mismatch)" << std::endl;
    return false;
  }

  // Check config
  if (deserializedNode.config.count("mode") == 0 ||
      deserializedNode.config.at("mode") != "stereo")
  {
    std::cout << "FAIL (mode config mismatch)" << std::endl;
    return false;
  }

  std::cout << "PASS" << std::endl;
  return true;
}

// Test: Embedded resources
bool TestEmbeddedResources()
{
  std::cout << "Test: EmbeddedResources... ";

  guitarfx::Preset original;
  original.id = "embedded-test";
  original.name = "Embedded Test";
  original.version = 2;

  // Add embedded resource
  guitarfx::EmbeddedResource embedded;
  embedded.id = "emb-model-1";
  embedded.type = "nam";
  embedded.name = "Embedded Model";
  embedded.hash = "abc123def456";
  embedded.originalPath = "original/path/model.nam";
  original.embeddedResources.push_back(embedded);

  // Add node referencing embedded resource
  guitarfx::GraphNode node;
  node.id = "amp";
  node.type = "amp_nam";
  guitarfx::ResourceRef embeddedRef;
  embeddedRef.embeddedId = "emb-model-1";
  node.resources.push_back(embeddedRef);
  original.graph.nodes.push_back(node);

  // Serialize and deserialize
  std::string json = guitarfx::PresetStorage::SerializeToJson(original);
  auto deserialized = guitarfx::PresetStorage::DeserializeFromJson(json);

  if (!deserialized)
  {
    std::cout << "FAIL (deserialization failed)" << std::endl;
    return false;
  }

  if (deserialized->embeddedResources.size() != 1)
  {
    std::cout << "FAIL (embedded resource count mismatch)" << std::endl;
    return false;
  }

  const auto& embRes = deserialized->embeddedResources[0];
  if (embRes.id != "emb-model-1" || embRes.type != "nam" || embRes.hash != "abc123def456")
  {
    std::cout << "FAIL (embedded resource data mismatch)" << std::endl;
    return false;
  }

  std::cout << "PASS" << std::endl;
  return true;
}

} // anonymous namespace

int main()
{
  std::cout << "========================================" << std::endl;
  std::cout << "PresetStorage V2 Tests" << std::endl;
  std::cout << "========================================" << std::endl;

  int passed = 0;
  int failed = 0;

  auto runTest = [&](bool (*testFn)())
  {
    if (testFn())
    {
      ++passed;
    }
    else
    {
      ++failed;
    }
  };

  runTest(TestSerializeDeserialize);
  runTest(TestSaveLoadFile);
  runTest(TestLoadAllFromDirectory);
  runTest(TestGraphNodeFinding);
  runTest(TestResourceRefValidation);
  runTest(TestDeserializeInvalidJson);
  runTest(TestNodeParams);
  runTest(TestEmbeddedResources);

  std::cout << "========================================" << std::endl;
  std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
  std::cout << "========================================" << std::endl;

  return failed > 0 ? 1 : 0;
}
