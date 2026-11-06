#pragma once

// The resource chooser's (ResourceBrowser.cpp) own pieces: its settings, the line under an entry's
// name, a list row, and which kind of slot it is serving.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "dsp/EffectGuids.h"
#include "uiclient/NodeLabels.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nanoq::ui::resource_browser
{
using namespace guitarfx::uiclient;

inline constexpr const char* kRootsSetting = "resources.folderBrowser.roots";
inline constexpr const char* kActiveRootSetting = "resources.folderBrowser.activeRootId";
inline constexpr const char* kLastLocationsSetting = "resources.folderBrowser.lastLocationByContext";

inline std::string MetaText(const nlohmann::json& metadata, const char* key)
{
    const auto it = metadata.find(key);

    if (it == metadata.end())
    {
        return {};
    }

    if (it->is_string())
    {
        return it->get<std::string>();
    }

    if (it->is_number())
    {
        return it->dump();
    }

    return {};
}

/// A cached thumbnail file (the engine's resource-thumbnails cache), read once. Kept for the
/// process and never destroyed: the drawing backend may be gone when statics are destroyed.
inline std::shared_ptr<artist::image> CachedThumbnail(const std::string& path)
{
    static auto* images = new std::map<std::string, std::shared_ptr<artist::image>>();
    auto it = images->find(path);

    if (it == images->end())
    {
        std::shared_ptr<artist::image> image;

        try
        {
            image = std::make_shared<artist::image>(guitarfx::util::PathFromUtf8(path));

            if (image->size().x <= 0.0f)
            {
                image.reset();
            }
        }
        catch (...)
        {
            image.reset(); // a backend that cannot decode it throws
        }

        it = images->emplace(path, std::move(image)).first;
    }

    return it->second;
}

/// "by creator · A2 · gear", the line under an entry's name.
inline std::string Describe(const nlohmann::json& metadata, const std::string& category)
{
    std::vector<std::string> parts;
    auto creator = MetaText(metadata, "creatorName");

    if (creator.empty())
    {
        creator = MetaText(metadata, "authorUsername");
    }

    if (creator.empty())
    {
        creator = MetaText(metadata, "modeled_by");
    }

    if (!creator.empty())
    {
        parts.push_back("by " + creator);
    }

    if (auto arch = MetaText(metadata, "architectureVersion"); !arch.empty() && arch != "custom")
    {
        parts.push_back(arch.front() == 'A' ? arch : "A" + arch);
    }

    if (const auto rate = MetaText(metadata, "sample_rate"); !rate.empty())
    {
        parts.push_back(rate + " Hz");
    }

    if (!category.empty())
    {
        parts.push_back(category);
    }

    std::string text;

    for (const auto& p : parts)
    {
        text += (text.empty() ? "" : " \xC2\xB7 ") + p;
    }

    return text;
}

/// One row of a list: a name with a line under it, an optional icon or picture at the left, and
/// at the right a note or a button. A click calls `onClick`; a double click `onDouble`.
class BrowserRow final : public el::element, public Focusable, public TapTarget
{
  public:
    BrowserRow(const Theme& theme, Icons& icons, std::string title, std::string detail)
        : mTheme(theme), mIcons(icons), mTitle(std::move(title)), mDetail(std::move(detail))
    {
    }

    // With focus: Enter takes it (its button, or what a double click does), Space picks it as a
    // click does, F stars it, and the Menu key opens its menu.
    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.role = AccessRole::ListItem;
        info.name = NameOr(mTitle);
        info.value = mDetail;

        if (!note.empty())
        {
            info.value += (info.value.empty() ? "" : ", ") + note;
        }

        if (star > 0)
        {
            info.value += info.value.empty() ? "favourite" : ", favourite";
        }

        info.selected = selected;
        return info;
    }

    bool Activate(const el::rect&) override
    {
        const auto& f = onButton ? onButton : (onDouble ? onDouble : onClick);
        return RunFromCopy(f);
    }

    bool wants_focus() const override
    {
        return FocusWanted(onClick || onDouble || onButton);
    }

    void begin_focus(focus_request) override
    {
        NoteFocus(true);
    }

    bool end_focus() override
    {
        NoteFocus(false);
        return true;
    }

    bool key(const el::context& ctx, el::key_info k) override
    {
        if (IsMenuKey(k))
        {
            return RunFromCopy(onMenu);
        }

        if (k.action != el::key_action::press || (k.modifiers & (el::mod_control | el::mod_alt | el::mod_super)) != 0)
        {
            return false;
        }

        if (k.key == el::key_code::space)
        {
            return RunFromCopy(onClick ? onClick : onDouble);
        }

        if (k.key == el::key_code::f && star >= 0)
        {
            return RunFromCopy(onStar);
        }

        return (k.key == el::key_code::enter || k.key == el::key_code::kp_enter) && Activate(ctx.bounds);
    }

    std::string icon;                                        // ui-folder, a chevron...
    std::function<std::shared_ptr<artist::image>()> picture; // a tone's picture, read as it draws (it may arrive later)
    std::string note;                                        // muted text at the right ("In library")
    el::color noteColour;
    std::string button; // a button at the right ("Select")
    bool selected = false;
    bool heading = false; // a group's caption
    bool indent = false;  // a model under its tone
    int star = -1;        // -1: none; 0 or 1: the favourite star, off or on
    std::function<void()> onClick, onDouble, onButton, onStar, onMenu;

    /// Icon buttons shown at the right while the row is hovered or chosen (.resource-browser-item-actions:
    /// edit, details, delete, select). Each is told where the row is, for a menu to hang from it.
    struct Action
    {
        std::string icon;
        std::function<void(el::rect row)> onClick;
        bool enabled = true;
    };

    std::vector<Action> actions;

    el::view_limits limits(const el::basic_context&) const override
    {
        const float h = heading ? 30.0f : (mDetail.empty() ? 36.0f : 44.0f);
        return {{100.0f, h}, {el::full_extent, h}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        auto b = ctx.bounds;
        bounds = b;

        if (heading)
        {
            cnv.fill_style(
                mTheme.Pick(el::rgba(255, 255, 255, mHover ? 14 : 8), mTheme.Text().opacity(mHover ? 0.08f : 0.05f)));
            cnv.fill_round_rect(b.inset(0.0f, 2.0f), 6.0f);
            mIcons.Draw(cnv, icon.empty() ? "ui-chevron-right" : icon,
                        el::rect{b.left + 10.0f, b.top + 9.0f, b.left + 22.0f, b.top + 21.0f}, mTheme.TextSecondary());
            float titleLeft = b.left + 30.0f;

            // A Tone3000 tone's group shows the tone's picture after the chevron, as the web UI's.
            if (const auto image = picture ? picture() : nullptr)
            {
                const el::rect r{titleLeft, b.top + 5.0f, titleLeft + 20.0f, b.top + 25.0f};
                auto state = cnv.new_state();
                cnv.begin_path();
                cnv.add_round_rect(r, 3.0f);
                cnv.clip();
                DrawImageCovering(cnv, *image, r);
                titleLeft = r.right + 8.0f;
            }

            FillText(cnv, mTitle, Font(Weight::SemiBold).size(12.5f), mTheme.Ink(),
                     el::rect{titleLeft, b.top, b.right - 60.0f, b.bottom}, HAlign::Left);

            if (!note.empty())
            {
                const auto font = Font(Weight::SemiBold).size(9.5f);
                const float w = el::measure_text(cnv, note, font).x + 14.0f;
                const el::rect pill{b.right - 12.0f - w, b.top + 7.0f, b.right - 12.0f, b.bottom - 7.0f};
                cnv.fill_style(mTheme.Pick(el::rgba(255, 255, 255, 22), mTheme.Panel()));
                cnv.fill_round_rect(pill, 8.0f);
                FillText(cnv, note, font, mTheme.TextSecondary(), pill, HAlign::Centre);
            }

            return;
        }

        if (indent)
        {
            b.left += 16.0f;
        }

        const auto face = b.inset(0.0f, 2.0f);
        cnv.fill_style(selected ? mTheme.Accent().opacity(0.22f).level(0.8f)
                                : mTheme.Pick(el::rgba(255, 255, 255, mHover ? 14 : 7),
                                              mHover ? mTheme.Elevated() : mTheme.Card()));
        cnv.fill_round_rect(face, 6.0f);

        if (selected)
        {
            cnv.stroke_style(mTheme.Accent().opacity(0.45f));
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(face.inset(0.5f, 0.5f), 6.0f);
        }

        float x = face.left + 12.0f;

        if (const auto image = picture ? picture() : nullptr)
        {
            const el::rect r{x - 2.0f, face.top + 4.0f, x + face.height() - 10.0f, face.bottom - 4.0f};
            auto state = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(r, 4.0f);
            cnv.clip();
            DrawImageCovering(cnv, *image, r);
            x = r.right + 10.0f;
        }
        else if (!icon.empty())
        {
            const float cy = face.top + face.height() * 0.5f;
            mIcons.Draw(cnv, icon, el::rect{x, cy - 8.0f, x + 16.0f, cy + 8.0f}, mTheme.TextSecondary());
            x += 26.0f;
        }

        float right = face.right - 12.0f;

        // The actions, last first from the right, while hovered or chosen.
        mActionRects.assign(actions.size(), el::rect{});

        if (mHover || selected)
        {
            const float cy = face.top + face.height() * 0.5f;

            for (std::size_t i = actions.size(); i-- > 0;)
            {
                const el::rect r{right - 26.0f, cy - 13.0f, right, cy + 13.0f};
                mActionRects[i] = r;
                const bool hot = static_cast<int>(i) == mHoverAction && actions[i].enabled;

                if (hot)
                {
                    cnv.fill_style(mTheme.Text().opacity(0.1f));
                    cnv.fill_round_rect(r, 6.0f);
                }

                const bool play = actions[i].icon == "ui-play";
                mIcons.Draw(cnv, actions[i].icon, r.inset(6.5f, 6.5f),
                            !actions[i].enabled ? mTheme.TextMuted().opacity(0.4f)
                            : play              ? mTheme.Accent()
                            : hot               ? mTheme.Ink()
                                                : mTheme.TextSecondary());
                right = r.left - 4.0f;
            }

            right -= 6.0f;
        }

        if (star >= 0)
        {
            const float cy = face.top + face.height() * 0.5f;
            mStar = el::rect{right - 16.0f, cy - 8.0f, right, cy + 8.0f};
            mIcons.Draw(cnv, "star", mStar,
                        star > 0 ? mTheme.Accent() : mTheme.TextMuted().opacity(mHover ? 0.8f : 0.4f));
            right = mStar.left - 10.0f;
        }
        else
        {
            mStar = {};
        }

        if (!button.empty())
        {
            const auto font = Font(Weight::SemiBold).size(11.0f);
            const float w = el::measure_text(cnv, button, font).x + 22.0f;
            mButton = el::rect{right - w, face.top + (face.height() - 26.0f) * 0.5f, right,
                               face.top + (face.height() + 26.0f) * 0.5f};
            cnv.fill_style(mTheme.Pick(mHoverButton ? el::rgba(72, 72, 86, 255) : el::rgba(48, 48, 58, 255),
                                       mHoverButton ? mTheme.Input() : mTheme.Elevated()));
            cnv.fill_round_rect(mButton, 6.0f);
            cnv.stroke_style(mTheme.Pick(el::rgba(255, 255, 255, 30), mTheme.BorderStrong()));
            cnv.stroke_round_rect(mButton.inset(0.5f, 0.5f), 6.0f);
            FillText(cnv, button, font, mTheme.Ink(), mButton, HAlign::Centre);
            right = mButton.left - 10.0f;
        }
        else
        {
            mButton = {};
        }

        if (!note.empty())
        {
            const auto font = Font(Weight::SemiBold).size(10.5f);
            const float w = el::measure_text(cnv, note, font).x;
            FillText(cnv, note, font, noteColour.alpha > 0.0f ? noteColour : mTheme.TextMuted(),
                     el::rect{right - w, face.top, right, face.bottom}, HAlign::Right);
            right -= w + 10.0f;
        }

        const auto titleFont = Font(Weight::SemiBold).size(12.0f);
        const auto detailFont = Font(Weight::Regular).size(10.5f);

        if (mDetail.empty())
        {
            FillText(cnv, Ellipsize(cnv, mTitle, titleFont, right - x), titleFont, mTheme.Ink(),
                     el::rect{x, face.top, right, face.bottom}, HAlign::Left);
        }
        else
        {
            FillText(cnv, Ellipsize(cnv, mTitle, titleFont, right - x), titleFont, mTheme.Ink(),
                     el::rect{x, face.top + 5.0f, right, face.top + 21.0f}, HAlign::Left);
            FillText(cnv, Ellipsize(cnv, mDetail, detailFont, right - x), detailFont, mTheme.TextMuted(),
                     el::rect{x, face.top + 21.0f, right, face.bottom - 4.0f}, HAlign::Left);
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state == el::mouse_button::right && onMenu)
        {
            if (btn.down)
            {
                onMenu();
            }

            return true;
        }

        if (btn.state != el::mouse_button::left || btn.down)
        {
            return btn.state == el::mouse_button::left;
        }

        if (ReleasedAway(btn))
        {
            return true; // the list scrolled from here instead
        }

        for (std::size_t i = 0; i < mActionRects.size() && i < actions.size(); ++i)
        {
            if (mActionRects[i].includes(btn.pos))
            {
                if (actions[i].enabled && actions[i].onClick)
                {
                    actions[i].onClick(bounds);
                }

                return true;
            }
        }

        if (star >= 0 && mStar.inset(-6.0f, -6.0f).includes(btn.pos))
        {
            if (onStar)
            {
                onStar();
            }

            return true;
        }

        if (!button.empty() && mButton.includes(btn.pos))
        {
            if (onButton)
            {
                onButton();
            }

            return true;
        }

        if (btn.num_clicks >= 2 && onDouble)
        {
            onDouble();
        }
        else if (btn.num_clicks < 2 && onClick)
        {
            onClick();
        }

        (void)ctx;
        return true;
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool inside = status != el::cursor_tracking::leaving;
        const bool overButton = inside && !button.empty() && mButton.includes(p);
        int overAction = -1;

        for (std::size_t i = 0; inside && i < mActionRects.size(); ++i)
        {
            if (mActionRects[i].includes(p))
            {
                overAction = static_cast<int>(i);
            }
        }

        if (inside != mHover || overButton != mHoverButton || overAction != mHoverAction)
        {
            mHover = inside;
            mHoverButton = overButton;
            mHoverAction = overAction;
            ctx.view.refresh(ctx);
        }

        return false;
    }

    bool wants_control() const override
    {
        return true;
    }

  private:
    const Theme& mTheme;
    Icons& mIcons;
    std::string mTitle, mDetail;
    bool mHover = false, mHoverButton = false;
    int mHoverAction = -1;
    el::rect mButton, mStar;
    std::vector<el::rect> mActionRects;

  public:
    el::rect bounds; // where it was last drawn, for a menu to hang from
};

/// Which kind of slot the browser serves: its title, its Tone3000 gear and its folder context.
struct SlotRole
{
    std::string title;
    std::string context;
    std::string gear;
    std::vector<std::pair<std::string, std::string>> gears; // value, label
};

inline SlotRole RoleFor(const ClientState& state, const guitarfx::GraphNode& node, const std::string& resourceType,
                        int resourceIndex)
{
    namespace guids = guitarfx::EffectGuids;
    SlotRole role;

    if (resourceType == "ir")
    {
        const bool reverb = node.type == guids::kReverbIr || NodeCategory(state, node) == "reverb";
        role.title = reverb ? "Select Reverb IR" : "Select IR Cabinet";
        role.context =
            reverb ? "ir-reverb" : (resourceIndex > 0 ? "ir-cab#" + std::to_string(resourceIndex) : "ir-cab");
        role.gear = reverb ? "space" : "cab";
        role.gears = {{"cab", "Cabinets"}, {"space", "Rooms"}, {"ir", "All IRs"}};
        return role;
    }

    if (node.type == guids::kFxNam)
    {
        role.title = "Select Pedal Model";
        role.context = "nam-fx";
        role.gear = "pedal";
    }
    else
    {
        // An amp with a cab IR after it wants an amp alone; otherwise a full rig.
        bool cabInPath = false;

        if (state.activePreset)
        {
            for (const auto& n : state.activePreset->graph.nodes)
            {
                cabInPath = cabInPath || n.type == guids::kCabIr;
            }
        }

        role.title = "Select Amp Model";
        role.context = "nam-amp";
        role.gear = cabInPath ? "amp" : "full-rig";
    }

    role.gears = {
        {"full-rig", "Full Rigs"}, {"amp", "Amps"}, {"pedal", "Pedals"}, {"outboard", "Preamps"}, {"", "Any gear"}};
    return role;
}
} // namespace nanoq::ui::resource_browser
