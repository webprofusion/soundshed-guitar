// Demo audio: the clips the footer offers, and the chooser that plays and renders them.

#include "ui/shell/Shell.h"

#include "ui/shell/FrameParts.h"

#include "uiclient/FeatureFlags.h"

#include <algorithm>
#include <cstdio>

namespace nanoq::ui
{
using namespace frame;

std::vector<const std::vector<guitarfx::uiclient::DemoClip>*> Shell::DemoClipLists() const
{
    // The built-in clips, and the favourite riffs unless the riff library is switched off.
    if (guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::RiffLibrary))
    {
        return {&mState.demo.clips, &mState.demo.riffClips};
    }

    return {&mState.demo.clips};
}

const guitarfx::uiclient::DemoClip* Shell::SelectedDemoClip() const
{
    const auto selected = mState.appSettings.value(kDemoClipSetting, std::string{});

    for (const auto* list : DemoClipLists())
    {
        for (const auto& clip : *list)
        {
            if (clip.id == selected)
            {
                return &clip;
            }
        }
    }

    return mState.demo.clips.empty() ? nullptr : &mState.demo.clips.front();
}

void Shell::OpenDemoClips()
{
    const auto* current = SelectedDemoClip();
    std::vector<MenuItem> items;

    for (const auto* list : DemoClipLists())
    {
        if (list == &mState.demo.riffClips && !list->empty())
        {
            items.push_back({"Favourite riffs", false, true, {}});
        }

        for (const auto& clip : *list)
        {
            items.push_back({clip.title, current != nullptr && current->id == clip.id, false,
                             [this, id = clip.id] { mSession.Commands().SetSetting(kDemoClipSetting, id); }});
        }
    }

    if (current != nullptr)
    {
        items.push_back({"Render", false, true, {}});
        items.push_back({"Render this clip through the preset", false, false, [this] {
                             const auto* c = SelectedDemoClip();

                             if (c == nullptr)
                             {
                                 return;
                             }

                             const int rate =
                                 static_cast<int>(NumberSetting(mState.appSettings, kDemoRenderRateSetting, 0.0));

                             if (c->takeId.empty())
                             {
                                 mSession.Commands().RenderDemoClip(c->id, rate);
                             }
                             else
                             {
                                 mSession.Commands().RenderRiffTake(c->takeId, rate);
                             }
                         }});
    }

    if (guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::RiffLibrary))
    {
        items.push_back({"Riffs", false, true, {}});
        items.push_back({"Riff library...", false, false, [this] { OpenRiffLibrary(); }});
    }

    OpenMenu("Demo audio", std::move(items));
}
} // namespace nanoq::ui
