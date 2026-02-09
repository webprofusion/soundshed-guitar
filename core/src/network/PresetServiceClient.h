#pragma once

#include <functional>
#include <string>
#include <vector>

namespace guitarfx
{
  struct Preset;

  struct PresetSearchRequest
  {
    std::string query;
    std::string category;
  };

  class PresetServiceClient
  {
  public:
    using ResultCallback = std::function<void(std::vector<Preset>)>;

    void SetBaseUrl(std::string baseUrl);
    void SearchPresets(const PresetSearchRequest &request, ResultCallback callback);
    void DownloadPreset(const std::string &presetId, ResultCallback callback);

  private:
    std::string mBaseUrl;
  };
} // namespace guitarfx
