#include "uiclient/VisualizerBackground.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace guitarfx::uiclient::visualizer_background
{
bool IsStoredUrl(const std::string& url)
{
    // ^ui-backgrounds/[0-9a-f]{16}\.(?:png|jpg|gif|webp)$
    static constexpr std::string_view kPrefix = "ui-backgrounds/";

    if (url.rfind(kPrefix, 0) != 0 || url.size() < kPrefix.size() + 17)
    {
        return false;
    }

    for (std::size_t i = 0; i < 16; ++i)
    {
        const char c = url[kPrefix.size() + i];

        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        {
            return false;
        }
    }

    const auto extension = std::string_view(url).substr(kPrefix.size() + 16);
    return extension == ".png" || extension == ".jpg" || extension == ".gif" || extension == ".webp";
}

std::optional<std::string> Url(const nlohmann::json& appSettings)
{
    const auto it = appSettings.find(kImageSetting);

    if (it == appSettings.end() || !it->is_string() || !IsStoredUrl(it->get<std::string>()))
    {
        return std::nullopt;
    }

    return it->get<std::string>();
}

int Fade(const nlohmann::json& appSettings)
{
    const auto it = appSettings.find(kFadeSetting);
    double value = kFadeDefault;

    if (it != appSettings.end() && it->is_number())
    {
        value = it->get<double>();
    }
    else if (it != appSettings.end() && it->is_string())
    {
        try
        {
            value = std::stod(it->get<std::string>());
        }
        catch (...)
        {
            value = kFadeDefault;
        }
    }

    if (!std::isfinite(value))
    {
        value = kFadeDefault;
    }

    return static_cast<int>(std::lround(std::clamp(value, 0.0, static_cast<double>(kFadeMax))));
}
} // namespace guitarfx::uiclient::visualizer_background
