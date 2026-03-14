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

  auto expected = original;
  guitarfx::EnsurePresetBoundaryGainNodes(expected.graph);

  // Verify graph node count
  if (deserialized->graph.nodes.size() != expected.graph.nodes.size())
  {
    std::cout << "FAIL (node count mismatch: " << deserialized->graph.nodes.size() 
              << " vs " << expected.graph.nodes.size() << ")" << std::endl;
    return false;
  }

  // Verify graph edge count
  if (deserialized->graph.edges.size() != expected.graph.edges.size())
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

bool TestLegacyGraphCreatesSingleScene()
{
  std::cout << "Test: LegacyGraphCreatesSingleScene... ";

  const std::string json = R"JSON({
    "id": "legacy-scene-test",
    "name": "Legacy Scene Test",
    "version": 2,
    "graph": {
      "nodes": [
        {"id": "__input__", "type": "input"},
        {"id": "amp", "type": "amp_nam"},
        {"id": "__output__", "type": "output"}
      ],
      "edges": [
        {"from": "__input__", "to": "amp"},
        {"from": "amp", "to": "__output__"}
      ]
    }
  })JSON";

  auto preset = guitarfx::PresetStorage::DeserializeFromJson(json);
  if (!preset)
  {
    std::cout << "FAIL (deserialization failed)" << std::endl;
    return false;
  }

  if (preset->scenes.size() != 1)
  {
    std::cout << "FAIL (expected 1 synthesized scene, got " << preset->scenes.size() << ")" << std::endl;
    return false;
  }

  if (preset->scenes[0].id != "scene-1" || preset->scenes[0].title != "Scene 1")
  {
    std::cout << "FAIL (legacy scene defaults incorrect)" << std::endl;
    return false;
  }

  if (preset->graph.nodes.size() != preset->scenes[0].graph.nodes.size())
  {
    std::cout << "FAIL (legacy graph and synthesized scene graph differ)" << std::endl;
    return false;
  }

  const auto migratedJson = nlohmann::json::parse(guitarfx::PresetStorage::SerializeToJson(*preset));
  if (migratedJson.contains("graph"))
  {
    std::cout << "FAIL (legacy graph should be removed from serialized preset once scenes exist)" << std::endl;
    return false;
  }
  if (!migratedJson.contains("scenes") || !migratedJson["scenes"].is_array() || migratedJson["scenes"].size() != 1)
  {
    std::cout << "FAIL (serialized preset missing migrated scene)" << std::endl;
    return false;
  }

  std::cout << "PASS" << std::endl;
  return true;
}

bool TestSerializeDeserializeScenes()
{
  std::cout << "Test: SerializeDeserializeScenes... ";

  auto preset = CreateTestPreset("scene-roundtrip", "Scene Roundtrip", "models/amp.nam");
  guitarfx::PresetScene cleanScene;
  cleanScene.id = "scene-clean";
  cleanScene.title = "Clean";
  cleanScene.graph = preset.graph;

  guitarfx::PresetScene leadScene = cleanScene;
  leadScene.id = "scene-lead";
  leadScene.title = "Lead";
  if (auto* ampNode = leadScene.graph.FindNode("amp"))
  {
    ampNode->params["drive"] = 0.9;
  }
  else
  {
    std::cout << "FAIL (test preset missing amp node)" << std::endl;
    return false;
  }

  preset.scenes = { cleanScene, leadScene };
  preset.graph = leadScene.graph;

  const std::string json = guitarfx::PresetStorage::SerializeToJson(preset);
  const auto serializedJson = nlohmann::json::parse(json);
  if (serializedJson.contains("graph"))
  {
    std::cout << "FAIL (serialized scene preset should not duplicate top-level graph)" << std::endl;
    return false;
  }
  auto roundTripped = guitarfx::PresetStorage::DeserializeFromJson(json);
  if (!roundTripped)
  {
    std::cout << "FAIL (round-trip deserialize failed)" << std::endl;
    return false;
  }

  if (roundTripped->scenes.size() != 2)
  {
    std::cout << "FAIL (expected 2 scenes, got " << roundTripped->scenes.size() << ")" << std::endl;
    return false;
  }

  const auto* lead = guitarfx::FindPresetScene(*roundTripped, "scene-lead");
  if (!lead)
  {
    std::cout << "FAIL (lead scene missing after round-trip)" << std::endl;
    return false;
  }

  const auto* ampNode = lead->graph.FindNode("amp");
  if (!ampNode || ampNode->params.find("drive") == ampNode->params.end() || ampNode->params.at("drive") != 0.9)
  {
    std::cout << "FAIL (lead scene graph contents not preserved)" << std::endl;
    return false;
  }

  std::cout << "PASS" << std::endl;
  return true;
}

bool TestSingleSceneNormalizationPreservesSceneGraph()
{
  std::cout << "Test: SingleSceneNormalizationPreservesSceneGraph... ";

  auto preset = CreateTestPreset("single-scene-normalize", "Single Scene Normalize", "models/amp.nam");

  guitarfx::PresetScene scene;
  scene.id = "scene-1";
  scene.title = "Scene 1";
  scene.graph = preset.graph;
  if (auto* ampNode = scene.graph.FindNode("amp"))
  {
    ampNode->params["drive"] = 0.91;
  }
  else
  {
    std::cout << "FAIL (scene graph missing amp node)" << std::endl;
    return false;
  }

  if (auto* presetAmpNode = preset.graph.FindNode("amp"))
  {
    presetAmpNode->params["drive"] = 0.12;
  }
  else
  {
    std::cout << "FAIL (preset graph missing amp node)" << std::endl;
    return false;
  }

  preset.scenes = { scene };
  guitarfx::NormalizePresetScenes(preset);

  const auto* normalizedScene = guitarfx::FindPresetScene(preset, "scene-1");
  if (!normalizedScene)
  {
    std::cout << "FAIL (normalized scene missing)" << std::endl;
    return false;
  }

  const auto* normalizedAmpNode = normalizedScene->graph.FindNode("amp");
  if (!normalizedAmpNode)
  {
    std::cout << "FAIL (normalized scene amp missing)" << std::endl;
    return false;
  }

  const auto driveIt = normalizedAmpNode->params.find("drive");
  if (driveIt == normalizedAmpNode->params.end() || driveIt->second != 0.91)
  {
    std::cout << "FAIL (single scene graph was overwritten during normalization)" << std::endl;
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

bool TestPresetFilePathNormalizationOnDisk()
{
  std::cout << "Test: PresetFilePathNormalizationOnDisk... ";

  PresetTestFixture fixture;
  const fs::path presetDir = fixture.mTempDir / "presets";
  const fs::path inPresetDir = presetDir / "assets";
  const fs::path outsideDir = fixture.mTempDir / "outside";
  fs::create_directories(inPresetDir);
  fs::create_directories(outsideDir);

  const fs::path inPresetFile = inPresetDir / "inside.nam";
  const fs::path outsideFile = outsideDir / "outside.nam";
  {
    std::ofstream insideOut(inPresetFile, std::ios::binary);
    insideOut << "inside";
  }
  {
    std::ofstream outsideOut(outsideFile, std::ios::binary);
    outsideOut << "outside";
  }

  guitarfx::Preset preset;
  preset.id = "path-normalization";
  preset.name = "Path Normalization";
  preset.version = 2;

  guitarfx::GraphNode node;
  node.id = "amp";
  node.type = "amp_nam";
  node.enabled = true;

  guitarfx::ResourceRef inPresetRef;
  inPresetRef.filePath = inPresetFile;
  node.resources.push_back(inPresetRef);

  guitarfx::ResourceRef outsideRef;
  outsideRef.filePath = outsideFile;
  node.resources.push_back(outsideRef);

  preset.graph.nodes.push_back(node);

  guitarfx::EmbeddedResource embedded;
  embedded.id = "emb-1";
  embedded.type = "nam";
  embedded.name = "Embedded";
  embedded.originalPath = inPresetFile;
  preset.embeddedResources.push_back(embedded);

  const fs::path presetPath = presetDir / "path-normalization.json";
  if (!guitarfx::PresetStorage::SaveToFile(preset, presetPath))
  {
    std::cout << "FAIL (save failed)" << std::endl;
    return false;
  }

  nlohmann::json raw;
  {
    std::ifstream input(presetPath);
    if (!input)
    {
      std::cout << "FAIL (unable to open saved preset json)" << std::endl;
      return false;
    }
    raw = nlohmann::json::parse(input, nullptr, false);
  }

  if (raw.is_discarded() || !raw.is_object())
  {
    std::cout << "FAIL (invalid saved preset json)" << std::endl;
    return false;
  }

  nlohmann::json nodes = nlohmann::json::array();
  if (raw.contains("graph") && raw["graph"].is_object())
  {
    nodes = raw["graph"].value("nodes", nlohmann::json::array());
  }
  else if (raw.contains("scenes") && raw["scenes"].is_array() && !raw["scenes"].empty() && raw["scenes"][0].is_object())
  {
    nodes = raw["scenes"][0].value("graph", nlohmann::json::object()).value("nodes", nlohmann::json::array());
  }
  if (!nodes.is_array() || nodes.empty())
  {
    std::cout << "FAIL (missing serialized nodes)" << std::endl;
    return false;
  }

  const auto resources = nodes[0].value("resources", nlohmann::json::array());
  if (!resources.is_array() || resources.size() != 2)
  {
    std::cout << "FAIL (serialized resources missing)" << std::endl;
    return false;
  }

  const auto storedInside = fs::path(resources[0].value("filePath", ""));
  const auto storedOutside = fs::path(resources[1].value("filePath", ""));
  if (storedInside.empty() || storedInside.is_absolute())
  {
    std::cout << "FAIL (inside path should be stored as relative)" << std::endl;
    return false;
  }
  if (storedOutside.empty() || !storedOutside.is_absolute())
  {
    std::cout << "FAIL (outside path should remain absolute)" << std::endl;
    return false;
  }

  const auto embeddedArray = raw.value("embeddedResources", nlohmann::json::array());
  if (!embeddedArray.is_array() || embeddedArray.empty())
  {
    std::cout << "FAIL (missing embedded resources)" << std::endl;
    return false;
  }

  const auto storedOriginalPath = fs::path(embeddedArray[0].value("originalPath", ""));
  if (storedOriginalPath.empty() || storedOriginalPath.is_absolute())
  {
    std::cout << "FAIL (embedded originalPath should be stored as relative)" << std::endl;
    return false;
  }

  const auto loaded = guitarfx::PresetStorage::LoadFromFile(presetPath);
  if (!loaded)
  {
    std::cout << "FAIL (load failed)" << std::endl;
    return false;
  }

  if (loaded->graph.nodes.empty() || loaded->graph.nodes[0].resources.size() != 2)
  {
    std::cout << "FAIL (loaded resources missing)" << std::endl;
    return false;
  }

  if (loaded->graph.nodes[0].resources[0].filePath != inPresetFile.lexically_normal())
  {
    std::cout << "FAIL (inside path did not resolve back to absolute)" << std::endl;
    return false;
  }
  if (loaded->graph.nodes[0].resources[1].filePath != outsideFile.lexically_normal())
  {
    std::cout << "FAIL (outside path changed unexpectedly)" << std::endl;
    return false;
  }

  if (loaded->embeddedResources.empty() || loaded->embeddedResources[0].originalPath != inPresetFile.lexically_normal())
  {
    std::cout << "FAIL (embedded originalPath did not resolve back to absolute)" << std::endl;
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
  runTest(TestLegacyGraphCreatesSingleScene);
  runTest(TestSerializeDeserializeScenes);
  runTest(TestSingleSceneNormalizationPreservesSceneGraph);
  runTest(TestLoadAllFromDirectory);
  runTest(TestGraphNodeFinding);
  runTest(TestResourceRefValidation);
  runTest(TestDeserializeInvalidJson);
  runTest(TestNodeParams);
  runTest(TestEmbeddedResources);
  runTest(TestPresetFilePathNormalizationOnDisk);

  std::cout << "========================================" << std::endl;
  std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
  std::cout << "========================================" << std::endl;

  return failed > 0 ? 1 : 0;
}
