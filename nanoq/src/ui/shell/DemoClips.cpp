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

    // The full footer renders from its own more button (OpenDemoActions); the compact one has no
    // room for it, so its chooser renders.
    if (current != nullptr && !mWide)
    {
        items.push_back({"Render", false, true, {}});
        items.push_back({"Render WAV", false, false, [this] {
                             const auto* c = SelectedDemoClip();
                             const int rate =
                                 static_cast<int>(NumberSetting(mState.appSettings, kDemoRenderRateSetting, 0.0));

                             if (c == nullptr)
                             {
                                 return;
                             }

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

void Shell::OpenDemoActions(el::rect anchor)
{
    // The web UI's demo actions menu: the render's sample rate (demoAudio.ts's options; "Device
    // rate" renders at the engine's) and Render WAV, which renders the chosen clip through the
    // preset.
    static constexpr struct
    {
        int rate;
        const char* label;
    } kRates[] = {{0, "Device rate"}, {44100, "44.1 kHz"},   {48000, "48 kHz"},  {88200, "88.2 kHz"},
                  {96000, "96 kHz"},  {176400, "176.4 kHz"}, {192000, "192 kHz"}};

    const int chosen = static_cast<int>(NumberSetting(mState.appSettings, kDemoRenderRateSetting, 0.0));
    std::vector<MenuItem> items{{"Rate", false, true, {}}};

    for (const auto& r : kRates)
    {
        items.push_back({r.label, r.rate == chosen, false,
                         [this, rate = r.rate] { mSession.Commands().SetSetting(kDemoRenderRateSetting, rate); }});
    }

    items.push_back({"Render", false, true, {}});
    MenuItem render{"Render WAV", false, false, [this, chosen] {
                        const auto* c = SelectedDemoClip();

                        if (c == nullptr)
                        {
                            return;
                        }

                        if (c->takeId.empty())
                        {
                            mSession.Commands().RenderDemoClip(c->id, chosen);
                        }
                        else
                        {
                            mSession.Commands().RenderRiffTake(c->takeId, chosen);
                        }
                    }};
    render.subtitle = "Through the current preset";
    items.push_back(std::move(render));
    OpenDropdown(anchor, std::move(items), 200.0f, true);
}
} // namespace nanoq::ui
