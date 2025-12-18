#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace namguitar
{
/**
 * Represents an attachment (NAM model or IR) for a preset.
 * Attachments can be referenced by library ID or by direct file path.
 */
struct PresetAttachment
{
  std::string type;                   // "nam" or "ir"
  std::string id;                     // optional library ID for known models/IRs
  std::filesystem::path filePath;     // file path (relative or absolute)
  std::string hash;                   // SHA-256 hash for verification
  std::string data;                   // optional inline payload (e.g. base64) supplied by the UI
};

struct PresetParameter
{
  std::string id;
  double value = 0.0;
};

struct Preset
{
  std::string id;
  std::string name;
  std::string category;
  std::string description;
  std::string namModelId;             // library ID for NAM model
  std::string irId;                   // library ID for IR
  std::vector<std::string> fxChain;
  std::vector<PresetAttachment> attachments;
  std::vector<PresetParameter> parameters;
};
} // namespace namguitar
