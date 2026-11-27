#pragma once

// PanelBackground.h - the player's own picture behind the effect panel (Settings > User Interface
// > Effect Panel Background; uiclient's VisualizerBackground.h), drawn in place of the theme's
// backdrop under every theme as the web UI's custom-background.css does, with its Fade veiling it
// toward the theme's own ground (black on the dark themes, white on the light ones).

#include "ui/widgets/Theme.h"
#include "ui/widgets/Graphics.h"

#include "controller/UiBackgroundService.h"
#include "uiclient/VisualizerBackground.h"

#include <elements.hpp>

#include <map>
#include <memory>
#include <string>

namespace nanoq::ui
{
/// The picture behind the effect panel for these settings, or null when there is none or it cannot
/// be read. Each is read once: its name is a hash of its bytes, so a new picture is a new name.
/// Kept for the process and never destroyed: the drawing backend may be gone when statics are.
inline std::shared_ptr<artist::image> PanelBackgroundImage(const nlohmann::json& appSettings)
{
    const auto url = guitarfx::uiclient::visualizer_background::Url(appSettings);

    if (!url)
    {
        return nullptr;
    }

    static auto* images = new std::map<std::string, std::shared_ptr<artist::image>>();
    auto it = images->find(*url);

    if (it == images->end())
    {
        std::shared_ptr<artist::image> image;

        if (const auto file = guitarfx::UiBackgroundService::ResolveUiResource(*url))
        {
            try
            {
                image = std::make_shared<artist::image>(file->first);

                if (image->size().x <= 0.0f)
                {
                    image.reset();
                }
            }
            catch (...)
            {
                image.reset(); // a backend that cannot decode it throws
            }
        }

        // Not kept when the file is missing, so a picture still being copied is read next time.
        if (!image)
        {
            return nullptr;
        }

        it = images->emplace(*url, std::move(image)).first;
    }

    return it->second;
}

/// Draws the picture over `bounds`, covering it, under its Fade's veil.
inline void DrawPanelBackground(artist::canvas& cnv, const Theme& theme, const artist::image& image, el::rect bounds,
                                int fadePercent)
{
    cnv.fill_style(theme.Background());
    cnv.fill_rect(bounds);
    {
        auto state = cnv.new_state();
        cnv.begin_path();
        cnv.add_rect(bounds);
        cnv.clip();
        DrawImageCovering(cnv, image, bounds);
    }
    const float veil = static_cast<float>(fadePercent) / 100.0f;

    if (veil > 0.0f)
    {
        cnv.fill_style((theme.IsLight() ? el::rgba(255, 255, 255, 255) : el::rgba(0, 0, 0, 255)).opacity(veil));
        cnv.fill_rect(bounds);
    }
}
} // namespace nanoq::ui
