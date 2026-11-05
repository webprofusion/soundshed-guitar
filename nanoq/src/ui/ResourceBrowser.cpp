// The model and IR chooser, as the web UI's resource browser (ui-components/modals/
// resource-browser-modal.html, ts/resourceBrowser/): a dialog with three sources for a node's
// slot - the Resource Library, a Folder on disk, and Tone 3000 through Soundshed's relay. A click
// loads the choice into the node at once, so it can be heard; OK keeps it, Cancel puts back what
// the slot held.
//
// - Library: the library's entries of the slot's type, by category, with a search and a
//   category filter; File... asks the engine for a file dialog.
// - Folder: the folders the user added (the web UI's setting, so both products share them),
//   listed by the engine (listResourceFolder). A file is heard where it is; OK adds it to the
//   library in place (saveLocalLibraryResource), or picks the library entry it already is.
// - Tone 3000: a search by gear, sort and architecture; a tone opens to its models; Select
//   downloads one, stores it in the library and loads it (ToneService::ImportTone3000).

#include "ui/DialogChrome.h"
#include "ui/Shell.h"

#include "dsp/EffectGuids.h"
#include "uiclient/FeatureFlags.h"
#include "uiclient/NodeLabels.h"
#include "uiclient/ResourceIndex.h"
#include "uiclient/ResourceThumbnails.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <map>
#include <optional>
#include <cctype>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;
namespace t3 = guitarfx::uiclient::tone3000;

namespace
{
constexpr const char* kRootsSetting = "resources.folderBrowser.roots";
constexpr const char* kActiveRootSetting = "resources.folderBrowser.activeRootId";
constexpr const char* kLastLocationsSetting = "resources.folderBrowser.lastLocationByContext";

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string MetaText(const nlohmann::json& metadata, const char* key)
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
std::shared_ptr<artist::image> CachedThumbnail(const std::string& path)
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
std::string Describe(const nlohmann::json& metadata, const std::string& category)
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
class BrowserRow final : public el::element, public Focusable
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
        return Run(f);
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
            return Run(onMenu);
        }

        if (k.action != el::key_action::press || (k.modifiers & (el::mod_control | el::mod_alt | el::mod_super)) != 0)
        {
            return false;
        }

        if (k.key == el::key_code::space)
        {
            return Run(onClick ? onClick : onDouble);
        }

        if (k.key == el::key_code::f && star >= 0)
        {
            return Run(onStar);
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

    /// Runs a callback from a copy: it may rebuild the list this row sits in.
    static bool Run(const std::function<void()>& f)
    {
        if (!f)
        {
            return false;
        }

        const auto run = f;
        run();
        return true;
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

/// The tab strip's tabs (.resource-browser-tab-btn): 13 px, the open one in the accent and underlined.
class BrowserTab final : public Pressable
{
  public:
    BrowserTab(const Theme& theme, std::string label, bool active)
        : mTheme(theme), mLabel(std::move(label)), mActive(active)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::Tab;
        info.name = NameOr(mLabel);
        info.selected = mActive;
        return info;
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        const float w = el::measure_text(ctx.canvas, mLabel, Font(Weight::SemiBold).size(13.0f)).x + 36.0f;
        return {{w, 38.0f}, {w, 38.0f}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        FillText(cnv, mLabel, Font(Weight::SemiBold).size(13.0f),
                 mActive ? mTheme.Accent() : (mHover ? mTheme.Text() : mTheme.TextSecondary()),
                 el::rect{b.left, b.top, b.right, b.bottom - 2.0f}, HAlign::Centre);

        if (mActive)
        {
            cnv.fill_style(mTheme.Accent());
            cnv.fill_rect(el::rect{b.left, b.bottom - 2.0f, b.right, b.bottom});
        }
    }

  private:
    const Theme& mTheme;
    std::string mLabel;
    bool mActive;
};

/// Which kind of slot the browser serves: its title, its Tone3000 gear and its folder context.
struct SlotRole
{
    std::string title;
    std::string context;
    std::string gear;
    std::vector<std::pair<std::string, std::string>> gears; // value, label
};

SlotRole RoleFor(const ClientState& state, const guitarfx::GraphNode& node, const std::string& resourceType,
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
} // namespace

void Shell::OpenResourceBrowser(const std::string& nodeId, const std::string& resourceType, int resourceIndex,
                                std::function<void(const std::string& resourceId)> onPick, const std::string& title,
                                const std::string& category)
{
    // With onPick the chooser only picks a library entry for the caller (the Simple Cabinet's
    // Match an IR): nothing is loaded into the node, and only the library is offered, filtered
    // to `category` (the web UI's libraryCategoryHint).
    const auto* node = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;

    if (node == nullptr)
    {
        return;
    }

    enum class Tab
    {
        Library,
        Folder,
        Tone3000
    };

    struct Browser
    {
        std::string nodeId, resourceType;
        int index = -1;
        SlotRole role;
        guitarfx::ResourceRef original;
        bool hadOriginal = false;
        bool previewed = false;
        std::string selectedId, selectedPath; // the choice being heard
        Tab tab = Tab::Library;

        std::string query, category; // Library
        std::string arch, creator;   // "" for all
        std::vector<std::string> tags;
        bool favouritesOnly = false;
        std::map<std::string, bool> openPacks; // a pack the user opened or closed
        std::string filter;                    // Folder
        std::string rootId;
        std::string t3Query, t3Gear, t3Sort = "downloads-all-time", t3Arch;
        int t3Page = 1, t3Pages = 1;
        bool t3Loading = false;
        std::string t3Error;
        std::vector<t3::Tone> t3Tones;
        std::string t3Open;    // the tone showing its models
        std::string t3Hearing; // a Tone3000 model being auditioned (previewRemoteResource), not imported
        std::optional<t3::Tone> t3HearingTone;
        std::optional<t3::Model> t3HearingModel;
        std::vector<t3::Model> t3Models;
        bool t3ModelsLoading = false;

        SlotPtr tabs = MakeSlot(), controls = MakeSlot(), list = MakeSlot(), footer = MakeSlot();
        std::function<void()> fillTabs, fillControls, fillList, fillFooter;
        std::function<void(int)> searchTone3000;
        std::shared_ptr<el::basic_input_box> searchBox, filterBox, t3Box;
        Ptr searchElement, filterElement, t3Element;
        std::vector<Subscription> subscriptions;
        int toneListener = 0;
        // The Tone3000 pictures the Library and Folder rows show (uiclient/ResourceThumbnails.h).
        std::unique_ptr<ResourceThumbnailFetcher> thumbnails;
        std::function<std::shared_ptr<artist::image>(const std::string&)> thumbnailFor;
        std::weak_ptr<Browser> self;
        std::function<void(const std::string&)> onPick; // pick only: what to do with the choice
    };

    auto state = std::make_shared<Browser>();
    auto* b = state.get();
    b->self = state;
    b->nodeId = nodeId;
    b->resourceType = resourceType;
    b->index = resourceIndex;
    b->role = RoleFor(mState, *node, resourceType, resourceIndex);
    b->onPick = std::move(onPick);
    b->category = category;
    b->thumbnails = std::make_unique<ResourceThumbnailFetcher>(mSession.Client(), mSession.Tones());
    // A library entry's tone picture: the cached file, else the download while it arrives.
    b->thumbnailFor = [this, b](const std::string& resourceId) -> std::shared_ptr<artist::image> {
        const auto* picture = b->thumbnails ? b->thumbnails->Find(b->resourceType, resourceId) : nullptr;

        if (picture == nullptr)
        {
            return nullptr;
        }

        return !picture->path.empty() ? CachedThumbnail(picture->path) : RemoteImage(picture->fetchUrl);
    };
    b->t3Gear = b->role.gear;

    if (resourceType == "nam")
    {
        b->t3Arch = "2";
    }

    const auto slot = static_cast<std::size_t>(std::max(0, resourceIndex));

    if (!b->onPick && slot < node->resources.size())
    {
        b->original = node->resources[slot];
        b->hadOriginal = b->original.IsLibraryRef() || b->original.IsFilePath();
        b->selectedId = b->original.resourceId;
        b->selectedPath = guitarfx::util::PathToUtf8(b->original.filePath);
    }

    // Everything below runs on the UI thread; a refill asked for from inside a list waits for
    // the click that asked to finish (the row it replaces must outlive its own handler).
    const auto later = [this](std::weak_ptr<Browser> weak, auto member) {
        mSession.View().post([alive = std::weak_ptr<bool>(mAlive), weak, member] {
            if (const auto st = weak.lock(); st && alive.lock() && (st.get()->*member))
            {
                (st.get()->*member)();
            }
        });
    };
    const auto refillList = [later, weak = std::weak_ptr<Browser>(state)] { later(weak, &Browser::fillList); };

    auto& commands = mSession.Commands();
    const auto preview = [this, b](const std::string& id, const std::string& path) {
        // A choice from the library or a folder ends a Tone3000 audition, the slot taking it.
        if (!b->t3Hearing.empty())
        {
            mSession.Commands().CancelPreviewResource(b->nodeId, b->index, false);
            b->t3Hearing.clear();
            b->t3HearingTone.reset();
            b->t3HearingModel.reset();
        }

        b->previewed = true;
        b->selectedId = id;
        b->selectedPath = path;

        if (b->onPick)
        {
            b->fillFooter();
            return;
        }

        if (!id.empty())
        {
            mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, id, b->index);
        }
        else
        {
            mSession.Commands().SetNodeResourceFile(b->nodeId, b->resourceType, path, b->index);
        }

        b->fillFooter();
    };

    // ── Library ─────────────────────────────────────────────────────────────────
    // The web UI's Library tab (resourceBrowser.ts), from uiclient::ResourceIndex: each file
    // once, the filters, favourites first, and packs grouped when that is on.
    const auto libraryFilter = [b] {
        ResourceFilter filter;
        filter.query = b->query;
        filter.category = b->category.empty() ? std::string("all") : b->category;
        filter.architecture = b->arch.empty() ? std::string("all") : b->arch;
        filter.creator = b->creator.empty() ? std::string("all") : b->creator;
        filter.tags = b->tags;
        filter.favouritesOnly = b->favouritesOnly;
        return filter;
    };
    const auto libraryList = [this, b, preview, refillList, libraryFilter]() -> Ptr {
        const auto entries = FilterLibrary(mState, b->resourceType, libraryFilter(), b->selectedId);

        // Which tone pictures these show: asked once a session for each (the answer redraws).
        std::vector<std::string> ids;

        for (const auto* r : entries)
        {
            ids.push_back(r->id);
        }

        if (b->thumbnails)
        {
            b->thumbnails->Request(b->resourceType, ids);
        }

        const auto rowFor = [this, b, preview, refillList](const LibraryResource* r, const std::string& title) {
            // The category is left out while the list is filtered to one: every row would say it.
            auto row = std::make_shared<BrowserRow>(mTheme, mIcons, title,
                                                    ResourceMetaLine(*r, b->resourceType, b->category.empty()));
            row->selected = r->id == b->selectedId || (!b->selectedPath.empty() && r->filePath == b->selectedPath);
            row->picture = [b, id = r->id] { return b->thumbnailFor ? b->thumbnailFor(id) : nullptr; };
            row->star = IsResourceFavorite(mState, r->id) ? 1 : 0;
            row->onStar = [this, refillList, id = r->id] {
                mSession.Commands().SetResourceFavorite(id, !IsResourceFavorite(mState, id));
                refillList();
            };
            row->onMenu = [this, b, id = r->id, rowPtr = row.get()] {
                OpenLibraryResourceMenu(b->resourceType, id, rowPtr->bounds);
            };
            row->onClick = [preview, refillList, id = r->id] {
                preview(id, {});
                refillList();
            };
            row->onDouble = [this, b, preview, id = r->id] {
                preview(id, {});
                auto pick = b->onPick;
                CloseOverlay();

                if (pick)
                {
                    pick(id);
                }
            };

            // Edit (not a built-in one's), details, delete, and select (it plays in the chain).
            const auto provider = MetaText(r->metadata, "provider");
            const bool builtIn = provider == "built-in" || provider == "builtin" || provider == "factory";
            const auto type = b->resourceType;

            if (!builtIn)
            {
                row->actions.push_back(
                    {"ui-edit", [this, type, id = r->id](el::rect at) { EditLibraryResource(type, id, at); }});
            }

            row->actions.push_back(
                {"ui-info", [this, type, id = r->id](el::rect at) { OpenLibraryResourceMenu(type, id, at); }});
            row->actions.push_back(
                {"ui-trash", [this, type, id = r->id](el::rect at) { ConfirmDeleteLibraryResource(type, id, at); }});
            row->actions.push_back({"ui-play", [preview, refillList, id = r->id](el::rect) {
                                        preview(id, {});
                                        refillList();
                                    }});
            return row;
        };

        std::vector<Ptr> rows;

        if (IsPackGroupingEnabled(mState))
        {
            for (const auto& entry : GroupLibraryByPack(mState, entries))
            {
                if (!entry.pack)
                {
                    const auto* r = entry.items.front();
                    rows.push_back(rowFor(r, r->name.empty() ? r->id : r->name));
                    continue;
                }

                // A pack is open while the user leaves it open, and by default when it holds the
                // choice or the list is searched or narrowed to favourites.
                const auto key = entry.pack->key;
                const bool holdsSelected =
                    std::any_of(entry.items.begin(), entry.items.end(),
                                [b](const LibraryResource* r) { return r->id == b->selectedId; });
                const auto toggled = b->openPacks.find(key);
                const bool open = toggled != b->openPacks.end()
                                      ? toggled->second
                                      : (holdsSelected || !b->query.empty() || b->favouritesOnly);
                auto head = std::make_shared<BrowserRow>(mTheme, mIcons, entry.pack->title, "");
                head->heading = true;
                head->icon = open ? "ui-caret" : "ui-chevron-right";
                head->note = std::to_string(entry.items.size());
                std::vector<std::string> members;

                for (const auto* r : entry.items)
                {
                    members.push_back(r->id);
                }

                // The first of its models with a tone picture lends it to the group's caption.
                head->picture = [b, members]() -> std::shared_ptr<artist::image> {
                    for (const auto& id : members)
                    {
                        if (auto image = b->thumbnailFor ? b->thumbnailFor(id) : nullptr)
                        {
                            return image;
                        }
                    }

                    return nullptr;
                };
                head->onClick = [b, refillList, key, open] {
                    b->openPacks[key] = !open;
                    refillList();
                };
                rows.push_back(head);

                if (open)
                {
                    for (const auto* r : entry.items)
                    {
                        auto row = rowFor(r, NameWithinPack(r->name.empty() ? r->id : r->name, *entry.pack));
                        row->indent = true;
                        rows.push_back(row);
                    }
                }
            }
        }
        else
        {
            for (const auto* r : entries)
            {
                rows.push_back(rowFor(r, r->name.empty() ? r->id : r->name));
            }
        }

        if (rows.empty())
        {
            const bool narrowed = !b->query.empty() || !b->category.empty() || !b->arch.empty() ||
                                  !b->creator.empty() || !b->tags.empty() || b->favouritesOnly;
            auto none =
                std::make_shared<Text>(mTheme,
                                       !narrowed ? "The library has none yet."
                                                 : std::string("No ") + (b->resourceType == "ir" ? "IRs" : "models") +
                                                       " match the current filters.",
                                       12.0f);
            none->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
            rows.push_back(FixedHeight(60, none));
        }

        return Col(rows);
    };

    const auto libraryControls = [this, b, &commands, refillList, preview, libraryFilter]() -> Ptr {
        if (!b->searchBox)
        {
            auto [element, box] = el::input_box(b->resourceType == "ir" ? "Search IRs..." : "Search models...",
                                                Font(Weight::Regular), 1.0f);
            b->searchBox = box;
            b->searchElement = el::share(std::move(element));
            b->searchBox->on_text = [b](std::string_view text) {
                b->query = std::string(text);

                if (b->fillList)
                {
                    b->fillList(); // the list is its own slot: the box keeps its focus
                }
            };
        }

        // A drop-down button whose choice refills the controls and the list.
        const auto chooser = [this, b, refillList](const std::string& label, float width,
                                                   std::vector<std::pair<std::string, std::string>> choices,
                                                   std::string* target) {
            auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            button->Size(width, 30).Label(label, 11.0f).Caret(true);
            button->onClick = [this, b, refillList, choices, target, c = button.get()] {
                std::vector<MenuItem> items;

                for (const auto& [text, value] : choices)
                {
                    items.push_back({text, *target == value, false, [b, refillList, target, value] {
                                         *target = value;
                                         b->fillControls();
                                         refillList();
                                     }});
                }

                OpenDropdown(c->Bounds(), std::move(items), 220.0f);
            };
            return button;
        };

        std::vector<std::pair<std::string, std::string>> categories{{"All categories", ""}};

        for (const auto& name : LibraryCategories(mState, b->resourceType))
        {
            categories.emplace_back(name, name);
        }

        std::vector<Ptr> row{
            b->searchElement, HGap(8),
            chooser(b->category.empty() ? "All categories" : b->category, 140, categories, &b->category)};

        if (b->resourceType == "nam")
        {
            row.push_back(HGap(6));
            row.push_back(chooser(b->arch.empty() ? "A1 / A2" : b->arch, 84,
                                  {{"A1 and A2", ""}, {"A1", "A1"}, {"A2", "A2"}}, &b->arch));
        }

        // Creators and tags: the facets of what the library holds (getResourceLibraryFacets).
        std::vector<const LibraryResource*> all;

        if (const auto it = mState.resources.find(b->resourceType); it != mState.resources.end())
        {
            for (const auto* r : DeduplicateResources(it->second).deduped)
            {
                all.push_back(r);
            }
        }

        const auto facets = LibraryFacets(all);

        if (!facets.creators.empty())
        {
            std::vector<std::pair<std::string, std::string>> creators{{"All creators", ""}};

            for (const auto& c : facets.creators)
            {
                creators.emplace_back(c, c);
            }

            row.push_back(HGap(6));
            row.push_back(chooser(b->creator.empty() ? "All creators" : b->creator, 120, creators, &b->creator));
        }

        if (!facets.tags.empty())
        {
            auto tags = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            tags->Size(76, 30)
                .Label(b->tags.empty() ? std::string("Tags") : "Tags (" + std::to_string(b->tags.size()) + ")", 11.0f)
                .Caret(true);
            tags->onClick = [this, b, refillList, list = facets.tags, c = tags.get()] {
                std::vector<MenuItem> items{{"All tags", b->tags.empty(), false, [b, refillList] {
                                                 b->tags.clear();
                                                 b->fillControls();
                                                 refillList();
                                             }}};

                for (const auto& tag : list)
                {
                    const bool on = std::find(b->tags.begin(), b->tags.end(), tag) != b->tags.end();
                    items.push_back({tag, on, false, [b, refillList, tag, on] {
                                         if (on)
                                         {
                                             b->tags.erase(std::remove(b->tags.begin(), b->tags.end(), tag),
                                                           b->tags.end());
                                         }
                                         else
                                         {
                                             b->tags.push_back(tag);
                                         }

                                         b->fillControls();
                                         refillList();
                                     }});
                }

                OpenDropdown(c->Bounds(), std::move(items), 200.0f);
            };
            row.push_back(HGap(6));
            row.push_back(tags);
        }

        // All | Favourites (.resource-browser-favorites-filter).
        auto favourites = std::make_shared<Segmented>(mTheme, std::vector<std::string>{"All", "Favourites"});
        favourites->Select(b->favouritesOnly ? 1 : 0);
        favourites->onSelect = [b, refillList](int index) {
            b->favouritesOnly = index == 1;
            b->fillControls();
            refillList();
        };
        const bool grouped = IsPackGroupingEnabled(mState);
        auto group = std::make_shared<IconButton>(mTheme, mIcons, "ui-box", IconButton::Look::Boxed);
        group->Size(32, 32).IconSize(15).Lit(grouped);
        group->onClick = [this, refillList, grouped] {
            mSession.Commands().SetSetting("resources.library.groupPacks", !grouped);
            refillList();
        };

        // Previous and next through the list as it is drawn, each heard in the chain.
        const auto step = [this, b, preview, refillList, libraryFilter](int delta) {
            const auto order =
                NavigationOrder(mState, FilterLibrary(mState, b->resourceType, libraryFilter(), b->selectedId));

            if (order.empty())
            {
                return;
            }

            const auto at = std::find_if(order.begin(), order.end(),
                                         [b](const LibraryResource* r) { return r->id == b->selectedId; });
            const int count = static_cast<int>(order.size());
            const int index = at == order.end()
                                  ? (delta > 0 ? 0 : count - 1)
                                  : std::clamp(static_cast<int>(at - order.begin()) + delta, 0, count - 1);
            preview(order[static_cast<std::size_t>(index)]->id, {});
            refillList();
        };
        auto previous = std::make_shared<IconButton>(mTheme, mIcons, "ui-prev", IconButton::Look::Boxed);
        previous->Size(32, 32).IconSize(14);
        previous->onClick = [step] { step(-1); };
        auto next = std::make_shared<IconButton>(mTheme, mIcons, "ui-next", IconButton::Look::Boxed);
        next->Size(32, 32).IconSize(14);
        next->onClick = [step] { step(1); };
        auto file = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        file->Size(64, 30).Label("File...", 11.0f);
        file->onClick = [this, b, &commands] {
            commands.BrowseNodeResource(b->nodeId, b->resourceType, b->index);
            b->previewed = true; // a file chosen there is the new choice; Cancel puts back the old
            b->fillFooter();     // which OK keeps
        };
        row.insert(row.end(),
                   {HGap(6), favourites, HGap(6), Middle(group), HGap(6), Middle(previous), HGap(4), Middle(next)});

        if (!b->onPick)
        {
            row.insert(row.end(), {HGap(6), file});
        }

        return Row(row);
    };

    // ── Folder ──────────────────────────────────────────────────────────────────
    const auto roots = [this]() {
        std::vector<std::tuple<std::string, std::string, std::string>> list; // id, label, path

        if (const auto it = mState.appSettings.find(kRootsSetting); it != mState.appSettings.end() && it->is_array())
        {
            for (const auto& r : *it)
            {
                if (r.is_object())
                {
                    list.emplace_back(r.value("id", ""), r.value("label", ""), r.value("path", ""));
                }
            }
        }

        return list;
    };
    const auto rememberLocation = [this, b](const std::string& path) {
        auto locations = mState.appSettings.value(kLastLocationsSetting, nlohmann::json::object());

        if (!locations.is_object())
        {
            locations = nlohmann::json::object();
        }

        locations[b->role.context] = {{"rootId", b->rootId}, {"path", path}};
        mSession.Commands().SetSetting(kLastLocationsSetting, locations);
    };
    const auto openFolder = [this, b, rememberLocation](const std::string& path) {
        mSession.Commands().ListResourceFolder(path);
        rememberLocation(path);
    };

    const auto folderList = [this, b, preview, refillList, openFolder]() -> Ptr {
        const auto& listing = mState.resourceFolder;
        std::vector<Ptr> rows;

        if (b->rootId.empty())
        {
            auto hint = std::make_shared<Text>(mTheme, "Add a folder of models or IRs to browse it here.", 12.0f);
            hint->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
            return FixedHeight(60, hint);
        }

        if (!listing.error.empty())
        {
            auto error = std::make_shared<Text>(mTheme, listing.error, 12.0f);
            error->Colour(mTheme.Error()).Alignment(Text::Align::Centre);
            return FixedHeight(60, error);
        }

        const auto filter = Lower(b->filter);
        std::vector<std::string> inLibrary; // the files' library ids, for their tone pictures

        for (const auto& dir : listing.dirs)
        {
            if (!filter.empty() && Lower(dir.name).find(filter) == std::string::npos)
            {
                continue;
            }

            auto row = std::make_shared<BrowserRow>(mTheme, mIcons, dir.name, "");
            row->icon = "ui-folder";
            row->onClick = [openFolder, path = dir.path] { openFolder(path); };
            rows.push_back(row);
        }

        for (const auto& file : listing.files)
        {
            if (file.resourceType != b->resourceType ||
                (!filter.empty() && Lower(file.name).find(filter) == std::string::npos))
            {
                continue;
            }

            auto row = std::make_shared<BrowserRow>(mTheme, mIcons, file.name, Describe(file.metadata, ""));
            row->selected = !b->selectedPath.empty() && b->selectedPath == file.path;

            if (file.inLibrary)
            {
                row->note = "In library";
                row->noteColour = mTheme.Success();
            }

            // A file in the library shows its entry's tone picture, as the Library tab does.
            if (file.inLibrary && !file.libraryId.empty() && file.resourceType == b->resourceType)
            {
                inLibrary.push_back(file.libraryId);
                row->picture = [b, id = file.libraryId] { return b->thumbnailFor ? b->thumbnailFor(id) : nullptr; };
            }

            row->onClick = [preview, refillList, path = file.path] {
                preview({}, path);
                refillList();
            };
            row->onDouble = [this, preview, path = file.path] {
                preview({}, path);
                CloseOverlay();
            };
            rows.push_back(row);
        }

        if (b->thumbnails)
        {
            b->thumbnails->Request(b->resourceType, inLibrary);
        }

        if (rows.empty())
        {
            auto none =
                std::make_shared<Text>(mTheme, listing.path.empty() ? "Loading..." : "No models or IRs here.", 12.0f);
            none->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
            rows.push_back(FixedHeight(60, none));
        }

        return Col(rows);
    };

    const auto folderControls = [this, b, roots, openFolder, refillList]() -> Ptr {
        const auto list = roots();
        std::string label = "Choose a folder";

        for (const auto& [id, name, path] : list)
        {
            if (id == b->rootId)
            {
                label = name.empty() ? path : name;
            }
        }

        auto rootButton = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        rootButton->Size(140, 30).Label(label, 11.0f).Caret(true).Disabled(list.empty());
        rootButton->onClick = [this, b, list, openFolder, r = rootButton.get()] {
            std::vector<MenuItem> items;

            for (const auto& [id, name, path] : list)
            {
                items.push_back(
                    {name.empty() ? path : name, id == b->rootId, false, [this, b, id = id, path = path, openFolder] {
                         b->rootId = id;
                         mSession.Commands().SetSetting(kActiveRootSetting, id);
                         b->fillControls();
                         openFolder(path);
                     }});
            }

            OpenDropdown(r->Bounds(), std::move(items), 220.0f);
        };
        auto remove = std::make_shared<IconButton>(mTheme, mIcons, "ui-trash", IconButton::Look::Boxed);
        remove->Size(30, 30).IconSize(14).Disabled(b->rootId.empty());
        remove->onClick = [this, b] {
            auto rootsJson = mState.appSettings.value(kRootsSetting, nlohmann::json::array());
            nlohmann::json kept = nlohmann::json::array();

            for (const auto& r : rootsJson)
            {
                if (r.is_object() && r.value("id", "") != b->rootId)
                {
                    kept.push_back(r);
                }
            }

            mSession.Commands().SetSetting(kRootsSetting, kept);
            b->rootId.clear();
            // Refilled once this click is over: the button is in what the refill replaces.
            mSession.View().post([weak = b->self] {
                if (const auto st = weak.lock(); st && st->fillControls)
                {
                    st->fillControls();
                    st->fillList();
                }
            });
        };
        auto add = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        add->Size(100, 30).Label("Add Folder...", 11.0f);
        add->onClick = [this] { mSession.Commands().BrowseResourceFolder(); };

        if (!b->filterBox)
        {
            auto [element, box] = el::input_box("Filter files...", Font(Weight::Regular), 1.0f);
            b->filterBox = box;
            b->filterElement = el::share(std::move(element));
            b->filterBox->on_text = [b](std::string_view text) {
                b->filter = std::string(text);

                if (b->fillList)
                {
                    b->fillList();
                }
            };
        }

        // Where the listing is, with the way up.
        const auto& listing = mState.resourceFolder;
        auto up = std::make_shared<IconButton>(mTheme, mIcons, "ui-up", IconButton::Look::Boxed);
        up->Size(30, 30).IconSize(14).Disabled(listing.parent.empty() || b->rootId.empty());
        up->onClick = [openFolder, parent = listing.parent] { openFolder(parent); };
        auto where =
            std::make_shared<Text>(mTheme, b->rootId.empty() ? std::string() : listing.path, 11.0f, Weight::Medium);
        where->Colour(mTheme.TextSecondary());
        std::size_t files = 0;

        for (const auto& f : listing.files)
        {
            files += f.resourceType == b->resourceType ? 1 : 0;
        }

        auto counts =
            std::make_shared<Text>(mTheme,
                                   b->rootId.empty() ? std::string()
                                                     : std::to_string(listing.dirs.size()) +
                                                           (listing.dirs.size() == 1 ? " folder  " : " folders  ") +
                                                           std::to_string(files) + (files == 1 ? " file" : " files"),
                                   10.5f);
        counts->Colour(mTheme.TextMuted()).Fixed(true);
        return Col({FixedHeight(30, Row({rootButton, HGap(6), remove, HGap(6), add, HGap(8), b->filterElement})),
                    VGap(8), FixedHeight(30, Row({up, HGap(10), where, HGap(10), counts}))});
    };

    // ── Tone 3000 ───────────────────────────────────────────────────────────────
    const auto search = [this, b, refillList](int page) {
        b->t3Loading = true;
        b->t3Error.clear();
        b->t3Page = std::max(1, page);
        b->t3Open.clear();
        b->fillList();
        mSession.Tones().SearchTone3000(b->t3Query, b->t3Gear, b->t3Sort,
                                        b->resourceType == "nam" ? b->t3Arch : std::string(), b->t3Page,
                                        [b, refillList](const t3::Page<t3::Tone>& result, const std::string& error) {
                                            b->t3Loading = false;
                                            b->t3Error = error;
                                            b->t3Tones = result.items;
                                            b->t3Pages = result.totalPages;

                                            if (b->fillList)
                                            {
                                                b->fillList();
                                            }

                                            if (b->fillFooter)
                                            {
                                                b->fillFooter();
                                            }
                                        });
    };

    b->searchTone3000 = search;

    const auto tone3000List = [this, b, preview, refillList]() -> Ptr {
        std::vector<Ptr> rows;
        const auto message = [this](const std::string& text, el::color colour) {
            auto t = std::make_shared<Text>(mTheme, text, 12.0f);
            t->Colour(colour).Alignment(Text::Align::Centre);
            return FixedHeight(60, t);
        };

        if (b->t3Loading)
        {
            return message("Searching Tone3000...", mTheme.TextMuted());
        }

        if (!b->t3Error.empty())
        {
            return message(b->t3Error, mTheme.Error());
        }

        if (b->t3Tones.empty())
        {
            return message("Search Tone3000 for models to add to your library.", mTheme.TextMuted());
        }

        for (const auto& tone : b->t3Tones)
        {
            const int count = b->resourceType == "ir" ? tone.irsCount : tone.modelsCount;
            std::string detail = (tone.creatorName.empty() ? std::string() : "by " + tone.creatorName + " \xC2\xB7 ") +
                                 std::to_string(count) + (b->resourceType == "ir" ? " IRs" : " models") +
                                 (tone.platform.empty() ? "" : " \xC2\xB7 " + tone.platform);
            auto row = std::make_shared<BrowserRow>(mTheme, mIcons, tone.title, detail);
            row->picture = [this, url = tone.imageUrl] { return RemoteImage(url); };
            row->note = b->t3Open == tone.id ? "\xE2\x96\xB4" : "\xE2\x96\xBE";
            row->onClick = [this, b, tone, refillList] {
                if (b->t3Open == tone.id)
                {
                    b->t3Open.clear();
                    refillList();
                    return;
                }

                b->t3Open = tone.id;
                b->t3Models.clear();
                b->t3ModelsLoading = true;
                refillList();
                mSession.Tones().Tone3000Models(
                    tone.id, [b, id = tone.id](const std::vector<t3::Model>& models, const std::string&) {
                        if (b->t3Open != id)
                        {
                            return;
                        }

                        b->t3Models = models;
                        b->t3ModelsLoading = false;

                        if (b->fillList)
                        {
                            b->fillList();
                        }
                    });
            };
            rows.push_back(row);

            // The web UI's Import All: every model of the open tone into the library.
            if (b->t3Open == tone.id && !b->t3Models.empty())
            {
                const auto missing =
                    std::count_if(b->t3Models.begin(), b->t3Models.end(), [this, b](const t3::Model& m) {
                        return t3::FindImported(mState, b->resourceType, m) == nullptr;
                    });

                if (missing > 0)
                {
                    auto all = std::make_shared<BrowserRow>(
                        mTheme, mIcons, "Import all " + std::to_string(missing) + " into the library", "");
                    all->indent = true;
                    all->icon = "ui-download";
                    all->onClick = [this, b, tone, refillList] {
                        for (const auto& m : b->t3Models)
                        {
                            if (t3::FindImported(mState, b->resourceType, m) == nullptr &&
                                !mSession.Tones().IsImporting(m.id))
                            {
                                mSession.Tones().ImportTone3000(
                                    tone, m, b->resourceType, b->resourceType == "ir" ? b->role.gear : std::string(),
                                    [this, refillList](const std::string&, const std::string& error) {
                                        if (!error.empty())
                                        {
                                            ShowToast("Could not import a model", error, true);
                                        }

                                        refillList();
                                    });
                            }
                        }

                        refillList();
                    };
                    rows.push_back(all);
                }
            }

            if (b->t3Open != tone.id)
            {
                continue;
            }

            if (b->t3ModelsLoading)
            {
                auto loading = std::make_shared<BrowserRow>(mTheme, mIcons, "Loading models...", "");
                loading->indent = true;
                rows.push_back(loading);
                continue;
            }

            for (const auto& model : b->t3Models)
            {
                auto modelRow = std::make_shared<BrowserRow>(
                    mTheme, mIcons, model.name, model.architecture.empty() ? std::string() : "A" + model.architecture);
                modelRow->indent = true;
                const auto* imported = t3::FindImported(mState, b->resourceType, model);
                modelRow->selected = imported != nullptr && imported->id == b->selectedId;

                if (mSession.Tones().IsImporting(model.id))
                {
                    modelRow->note = "Importing...";
                }
                else if (imported != nullptr)
                {
                    modelRow->note = "In library";
                    modelRow->noteColour = mTheme.Success();
                    modelRow->button = "Select";
                    modelRow->onButton = [preview, refillList, id = imported->id] {
                        preview(id, {});
                        refillList();
                    };
                }
                else
                {
                    // A click plays it on the slot without importing it (the web UI's preview);
                    // Select (or OK while it plays) imports it and keeps it.
                    modelRow->selected = b->t3Hearing == model.id;

                    if (b->t3Hearing == model.id)
                    {
                        modelRow->note = "Playing";
                    }

                    modelRow->onClick = [this, b, tone, model, refillList] {
                        b->t3Hearing = model.id;
                        b->t3HearingTone = tone;
                        b->t3HearingModel = model;
                        b->previewed = true;
                        b->selectedId.clear();
                        b->selectedPath.clear();
                        refillList();

                        if (b->fillFooter)
                        {
                            b->fillFooter();
                        }

                        mSession.Tones().DownloadTone3000(
                            model, b->resourceType,
                            [this, b, id = model.id](const std::string& base64, const std::string& error) {
                                if (b->t3Hearing != id)
                                {
                                    return;
                                }

                                if (!error.empty())
                                {
                                    ShowToast("Could not play the model", error, true);
                                    return;
                                }

                                mSession.Commands().PreviewRemoteResource(b->resourceType, "tone3000-preview:" + id,
                                                                          b->nodeId, b->index, false, base64);
                            });
                    };
                    modelRow->button = "Select";
                    modelRow->onButton = [this, b, tone, model, preview, refillList] {
                        mSession.Tones().ImportTone3000(
                            tone, model, b->resourceType, b->resourceType == "ir" ? b->role.gear : std::string(),
                            [this, preview, refillList](const std::string& id, const std::string& error) {
                                if (!error.empty())
                                {
                                    ShowToast("Could not import the model", error, true);
                                    refillList();
                                    return;
                                }

                                preview(id, {});
                                refillList();
                            });
                        refillList();
                    };
                }

                rows.push_back(modelRow);
            }
        }

        return Col(rows);
    };

    const auto tone3000Controls = [this, b, search]() -> Ptr {
        if (!b->t3Box)
        {
            auto [element, box] =
                el::input_box(b->resourceType == "ir" ? "Search cabinets and rooms..." : "Search amps and pedals...",
                              Font(Weight::Regular), 1.0f);
            b->t3Box = box;
            b->t3Element = el::share(std::move(element));
            b->t3Box->on_text = [b](std::string_view text) { b->t3Query = std::string(text); };
            b->t3Box->on_enter = [b, search](std::string_view text) {
                b->t3Query = std::string(text);
                search(1);
                return true;
            };
        }

        auto go = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        go->Size(70, 30).Label("Search", 11.0f);
        go->onClick = [search] { search(1); };

        const auto choice = [this, b, search](const std::string& label,
                                              std::vector<std::pair<std::string, std::string>> options,
                                              std::string Browser::* field, float width) {
            auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            button->Size(width, 30).Label(label, 11.0f).Caret(true);
            button->onClick = [this, b, options, field, search, width, me = button.get()] {
                std::vector<MenuItem> items;

                for (const auto& [value, text] : options)
                {
                    items.push_back({text, b->*field == value, false, [b, field, value = value, search] {
                                         b->*field = value;
                                         b->fillControls();
                                         search(1);
                                     }});
                }

                OpenDropdown(me->Bounds(), std::move(items), std::max(150.0f, width));
            };
            return button;
        };
        const auto labelOf = [](const std::vector<std::pair<std::string, std::string>>& options,
                                const std::string& value) {
            for (const auto& [v, text] : options)
            {
                if (v == value)
                {
                    return text;
                }
            }

            return options.empty() ? std::string() : options.front().second;
        };
        const std::vector<std::pair<std::string, std::string>> sorts{{"downloads-all-time", "Popular"},
                                                                     {"newest", "Recent"},
                                                                     {"trending", "Trending"},
                                                                     {"best-match", "Best match"}};
        const std::vector<std::pair<std::string, std::string>> architectures{
            {"", "Any Architecture"}, {"1", "A1"}, {"2", "A2"}};

        std::vector<Ptr> row{b->t3Element,
                             HGap(8),
                             go,
                             HGap(8),
                             choice(labelOf(b->role.gears, b->t3Gear), b->role.gears, &Browser::t3Gear, 110),
                             HGap(6),
                             choice(labelOf(sorts, b->t3Sort), sorts, &Browser::t3Sort, 96)};

        if (b->resourceType == "nam")
        {
            row.push_back(HGap(6));
            row.push_back(choice(labelOf(architectures, b->t3Arch), architectures, &Browser::t3Arch, 132));
        }

        return Row(row);
    };

    // ── The dialog ──────────────────────────────────────────────────────────────
    b->fillTabs = [this, b] {
        auto tab = [this, b](const char* label, Tab which) {
            auto t = std::make_shared<BrowserTab>(mTheme, label, b->tab == which);
            t->onClick = [this, b, which] {
                if (b->tab == which)
                {
                    return;
                }

                b->tab = which;

                // Tone 3000 opens on its popular tones for the slot's gear, as the web UI's does.
                if (which == Tab::Tone3000 && b->t3Tones.empty() && !b->t3Loading && b->searchTone3000)
                {
                    b->searchTone3000(1);
                }

                mSession.View().post([weak = b->self] {
                    if (const auto st = weak.lock())
                    {
                        st->fillTabs();
                        st->fillControls();
                        st->fillList();
                        st->fillFooter();
                    }
                });
            };
            return t;
        };
        // Tone 3000 can be switched off in Settings > Features, as in the web UI.
        std::vector<Ptr> tabs{HGap(4), tab("Resource Library", Tab::Library)};

        if (!b->onPick)
        {
            tabs.push_back(tab("Folder", Tab::Folder));
        }

        if (!b->onPick && guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::Tone3000))
        {
            tabs.push_back(tab("Tone 3000", Tab::Tone3000));
        }

        tabs.push_back(Stretch());
        Fill(*b->tabs, Row(tabs));
    };
    b->fillControls = [this, b, libraryControls, folderControls, tone3000Controls] {
        const float height = b->tab == Tab::Folder ? 68.0f : b->tab == Tab::Library ? 34.0f : 30.0f;
        Fill(*b->controls, FixedHeight(height, b->tab == Tab::Library  ? libraryControls()
                                               : b->tab == Tab::Folder ? folderControls()
                                                                       : tone3000Controls()));
        mSession.View().layout();
        mSession.View().refresh();
    };
    b->fillList = [this, b, libraryList, folderList, tone3000List] {
        Fill(*b->list, VScroll(Pad(0, 0, 10, 0,
                                   b->tab == Tab::Library  ? libraryList()
                                   : b->tab == Tab::Folder ? folderList()
                                                           : tone3000List())));
        mSession.View().layout();
        mSession.View().refresh();
    };
    b->fillFooter = [this, b, search] {
        std::vector<Ptr> row;

        if (b->tab == Tab::Tone3000 && !b->t3Tones.empty())
        {
            auto prev = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
            prev->Size(60, 30).Label("Prev", 11.0f).Disabled(b->t3Page <= 1);
            prev->onClick = [b, search] { search(b->t3Page - 1); };
            auto next = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
            next->Size(60, 30).Label("Next", 11.0f).Disabled(b->t3Page >= b->t3Pages);
            next->onClick = [b, search] { search(b->t3Page + 1); };
            auto page = std::make_shared<Text>(
                mTheme, "Page " + std::to_string(b->t3Page) + " of " + std::to_string(b->t3Pages), 11.0f);
            page->Colour(mTheme.TextSecondary()).Fixed(true);
            row = {Stretch(), prev, HGap(6), page, HGap(6), next, Stretch()};
        }
        else
        {
            // What a click and a double click do (.resource-browser-footer-hint).
            auto hint = std::make_shared<Text>(mTheme,
                                               b->onPick ? "Double click an item to choose it."
                                                         : "Select an item to preview it in the current signal chain, "
                                                           "double click to select and return to the effect.",
                                               11.5f);
            hint->Colour(mTheme.TextSecondary());
            row = {Middle(hint), HGap(12)};
        }

        auto cancel = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Primary);
        cancel->Size(64, 30).Label("Cancel", 11.5f);
        cancel->onClick = [this, b] {
            // A Tone3000 audition ends and the engine puts back what the slot held.
            if (!b->t3Hearing.empty())
            {
                mSession.Commands().CancelPreviewResource(b->nodeId, b->index, true);
                b->t3Hearing.clear();
                b->previewed = false;
            }

            // Put back what the slot held before anything was heard.
            if (b->previewed && !b->onPick)
            {
                if (b->hadOriginal && b->original.IsLibraryRef())
                {
                    mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, b->original.resourceId, b->index);
                }
                else if (b->hadOriginal && b->original.IsFilePath())
                {
                    mSession.Commands().SetNodeResourceFile(b->nodeId, b->resourceType,
                                                            guitarfx::util::PathToUtf8(b->original.filePath), b->index);
                }
                else
                {
                    mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, {}, b->index);
                }
            }

            b->previewed = false;
            CloseOverlay();
        };
        auto ok = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Primary);
        ok->Size(44, 30).Label("OK", 11.5f).Disabled(!b->previewed);
        ok->onClick = [this, b] {
            if (b->onPick)
            {
                auto pick = b->onPick;
                const auto id = b->selectedId;
                CloseOverlay();

                if (!id.empty())
                {
                    pick(id);
                }

                return;
            }

            // A Tone3000 model being auditioned is imported and kept.
            if (!b->t3Hearing.empty() && b->t3HearingTone && b->t3HearingModel)
            {
                const auto nodeId = b->nodeId, resourceType = b->resourceType;
                const int index = b->index;
                mSession.Commands().CancelPreviewResource(nodeId, index, false);
                mSession.Tones().ImportTone3000(
                    *b->t3HearingTone, *b->t3HearingModel, resourceType,
                    resourceType == "ir" ? b->role.gear : std::string(),
                    [this, nodeId, resourceType, index](const std::string& id, const std::string& error) {
                        if (!error.empty())
                        {
                            ShowToast("Could not import the model", error, true);
                        }
                        else
                        {
                            mSession.Commands().SetNodeResource(nodeId, resourceType, id, index);
                        }
                    });
                b->t3Hearing.clear();
                b->previewed = false;
                CloseOverlay();
                return;
            }

            // A file heard from a folder joins the library (in place), or picks the entry it is.
            if (b->previewed && b->selectedId.empty() && !b->selectedPath.empty())
            {
                std::string libraryId;
                std::string name = b->selectedPath;

                for (const auto& f : mState.resourceFolder.files)
                {
                    if (f.path == b->selectedPath)
                    {
                        libraryId = f.inLibrary ? f.libraryId : std::string();
                        name = f.name;
                    }
                }

                if (!libraryId.empty())
                {
                    mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, libraryId, b->index);
                }
                else
                {
                    const auto dot = name.rfind('.');
                    mSession.Commands().SaveLocalLibraryResource(b->resourceType, b->selectedPath,
                                                                 dot == std::string::npos ? name : name.substr(0, dot),
                                                                 {{"sourceFolder", mState.resourceFolder.path},
                                                                  {"sourceFileName", name},
                                                                  {"origin", "folder-browser"}},
                                                                 b->nodeId, b->index);
                }
            }

            b->previewed = false;
            CloseOverlay();
        };
        row.push_back(cancel);
        row.push_back(HGap(8));
        row.push_back(ok);
        Fill(*b->footer, Row(row));
        mSession.View().layout();
        mSession.View().refresh();
    };

    // The folder to show first: where this kind of slot was browsed last, else the active root.
    {
        const auto list = roots();
        const auto active = mState.appSettings.value(kActiveRootSetting, std::string());
        std::string path;

        if (const auto locations = mState.appSettings.find(kLastLocationsSetting);
            locations != mState.appSettings.end() && locations->is_object() && locations->contains(b->role.context))
        {
            const auto& last = (*locations)[b->role.context];
            b->rootId = last.value("rootId", std::string());
            path = last.value("path", std::string());
        }

        if (b->rootId.empty())
        {
            b->rootId = active;
        }

        bool known = false;

        for (const auto& [id, name, rootPath] : list)
        {
            if (id == b->rootId)
            {
                known = true;

                if (path.empty())
                {
                    path = rootPath;
                }
            }
        }

        if (!known && !list.empty())
        {
            b->rootId = std::get<0>(list.front());
            path = std::get<2>(list.front());
        }
        else if (!known)
        {
            b->rootId.clear();
        }

        if (!path.empty())
        {
            mSession.Commands().ListResourceFolder(path);
        }
    }

    // The engine's answers: a folder listed or picked, the library grown (an import done).
    b->subscriptions.push_back(mSession.Client().Subscribe(Topic::ResourceFolders, [this, b, openFolder] {
        if (!mState.pickedResourceFolder.empty())
        {
            // A folder just added: kept as a root, shared with the web UI, and opened.
            const auto path = mState.pickedResourceFolder;
            mSession.Client().MutableState().pickedResourceFolder.clear();
            auto rootsJson = mState.appSettings.value(kRootsSetting, nlohmann::json::array());

            if (!rootsJson.is_array())
            {
                rootsJson = nlohmann::json::array();
            }

            std::string id;

            for (const auto& r : rootsJson)
            {
                if (r.is_object() && r.value("path", "") == path)
                {
                    id = r.value("id", "");
                }
            }

            if (id.empty())
            {
                id = "root-" + std::to_string(std::hash<std::string>{}(path) % 1000000007ull);
                const auto slash = path.find_last_of("/\\");
                rootsJson.push_back({{"id", id},
                                     {"label", slash == std::string::npos ? path : path.substr(slash + 1)},
                                     {"path", path}});
                mSession.Commands().SetSetting(kRootsSetting, rootsJson);
            }

            b->rootId = id;
            mSession.Commands().SetSetting(kActiveRootSetting, id);
            b->tab = Tab::Folder;
            b->fillTabs();
            openFolder(path);
        }

        if (b->tab == Tab::Folder)
        {
            b->fillControls();
            b->fillList();
        }
    }));
    b->subscriptions.push_back(mSession.Client().Subscribe(Topic::Resources, [b] {
        if (b->tab != Tab::Folder && b->fillList)
        {
            b->fillList();
        }
    }));
    // The engine saying which tone pictures the Library and Folder rows show: what it lacks is
    // fetched (and handed back to it to keep), and the rows redraw.
    b->subscriptions.push_back(mSession.Client().Subscribe(Topic::Thumbnails, [this, b] {
        if (b->thumbnails)
        {
            b->thumbnails->Update();
        }

        mSession.View().refresh();
    }));
    // A tone's picture arriving (a Tone3000 result's, or a library row's thumbnail), or an import
    // moving on.
    b->toneListener = mSession.Tones().AddListener([this, b] {
        if (b->thumbnails)
        {
            b->thumbnails->Update();
        }

        mSession.View().refresh();
    });

    b->fillTabs();
    b->fillControls();
    b->fillList();
    b->fillFooter();

    auto cancel = [this, b] {
        // The x is Cancel, as in the web UI.
        if (b->previewed && !b->onPick)
        {
            if (b->hadOriginal && b->original.IsLibraryRef())
            {
                mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, b->original.resourceId, b->index);
            }
            else if (b->hadOriginal && b->original.IsFilePath())
            {
                mSession.Commands().SetNodeResourceFile(b->nodeId, b->resourceType,
                                                        guitarfx::util::PathToUtf8(b->original.filePath), b->index);
            }
            else
            {
                mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, {}, b->index);
            }
        }

        CloseOverlay();
    };

    const auto view = ViewSize();
    ShowDialog({.title = title.empty() ? b->role.title : title,
                .body = Col({FixedHeight(38, b->tabs),
                             FixedHeight(1, Fill(mTheme.ModalRule(), std::make_shared<el::element>())), VGap(12),
                             b->controls, VGap(10), b->list}),
                .footerLeft = b->footer,
                .width = std::min(944.0f, view.x - 16.0f),
                .height = std::max(320.0f, view.y - 40.0f),
                .padX = 16.0f,
                .padY = 10.0f,
                .onClose = cancel,
                .stretch = true});
    mOnOverlayClosed = [this, state] {
        mSession.Tones().RemoveListener(state->toneListener);
        state->subscriptions.clear();
        state->thumbnailFor = nullptr;
        state->thumbnails.reset();
        state->fillTabs = state->fillControls = state->fillList = state->fillFooter = nullptr;
        state->searchTone3000 = nullptr;
    };
    (void)commands;
}

void Shell::OpenInlinePrompt(el::rect anchor, const std::string& title, const std::string& initial,
                             std::function<void(const std::string&)> onSubmit)
{
    // A prompt on the drop-down layer, so it stacks over a dialog (the resource chooser) rather
    // than taking its place as OpenPrompt does.
    auto heading = std::make_shared<Text>(mTheme, title, 12.5f, Weight::SemiBold);
    auto [boxElement, box] = el::input_box("", Font(Weight::Regular), 1.0f);
    box->set_text(initial);
    auto submit = [this, box, onSubmit] {
        auto text = box->get_utf8();
        const auto first = text.find_first_not_of(" \t");
        const auto last = text.find_last_not_of(" \t");
        text = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        CloseDropdown();
        onSubmit(text);
    };
    box->on_enter = [submit](std::string_view) {
        submit();
        return true;
    };
    box->on_escape = [this] { CloseDropdown(); };
    auto ok = std::make_shared<Button>(mTheme, "OK", submit, Button::Style::Primary);
    ok->MinWidth(64);
    auto panel = PadXY(
        10, 8, Col({FixedHeight(22, heading), FixedHeight(36, Row({el::share(std::move(boxElement)), HGap(6), ok}))}));
    CloseDropdown();
    mDropdown = KeyLayer(
        PlacePanel(panel, anchor, 320.0f, 84.0f, anchor.top > ViewSize().y * 0.6f, [this] { CloseDropdown(); }));
    mSession.View().add(mDropdown);
}

void Shell::OpenLibraryResourceMenu(const std::string& resourceType, const std::string& resourceId, el::rect anchor)
{
    // The web UI's row actions (resourceBrowser.ts): details, edit, copy the path, delete.
    const auto* r = mState.FindLibraryResource(resourceType, resourceId);

    if (r == nullptr)
    {
        return;
    }

    const std::string name = r->name.empty() ? r->id : r->name;
    const std::string category = r->category;
    const std::string path = r->filePath;
    const auto tags = ResourceTags(*r);
    mSession.Commands().QueryResourceUsage(resourceType, resourceId);

    std::vector<MenuItem> items;
    std::string detail = "Id " + r->id;

    if (const auto creator = ResourceCreator(*r); !creator.empty())
    {
        detail += "  \xC2\xB7  by " + creator;
    }

    if (!tags.empty())
    {
        detail += "  \xC2\xB7 ";

        for (const auto& t : tags)
        {
            detail += " #" + t;
        }
    }

    items.push_back({name, false, true, {}});
    items.push_back({detail, false, true, {}});
    items.push_back(
        {"Rename...", false, false, [this, resourceType, resourceId, name, category, tags, anchor] {
             mSession.View().post(
                 [this, alive = std::weak_ptr<bool>(mAlive), resourceType, resourceId, name, category, tags, anchor] {
                     if (alive.lock())
                     {
                         OpenInlinePrompt(anchor, "Name", name,
                                          [this, resourceType, resourceId, category, tags](const std::string& entered) {
                                              if (!entered.empty())
                                              {
                                                  mSession.Commands().UpdateLibraryResource(resourceType, resourceId,
                                                                                            entered, category, tags);
                                              }
                                          });
                     }
                 });
         }});
    items.push_back(
        {"Change category...", false, false, [this, resourceType, resourceId, name, category, tags, anchor] {
             mSession.View().post(
                 [this, alive = std::weak_ptr<bool>(mAlive), resourceType, resourceId, name, category, tags, anchor] {
                     if (alive.lock())
                     {
                         OpenInlinePrompt(anchor, "Category", category,
                                          [this, resourceType, resourceId, name, tags](const std::string& entered) {
                                              mSession.Commands().UpdateLibraryResource(resourceType, resourceId, name,
                                                                                        entered, tags);
                                          });
                     }
                 });
         }});

    if (!path.empty())
    {
        items.push_back({"Copy the file's path", false, false, [path] { el::set_clipboard({{"text/plain", path}}); }});
    }

    items.push_back({"Delete...", false, false, [this, resourceType, resourceId, anchor] {
                         // Asked once more, on the same layer, once this menu is gone.
                         mSession.View().post(
                             [this, alive = std::weak_ptr<bool>(mAlive), resourceType, resourceId, anchor] {
                                 if (alive.lock())
                                 {
                                     ConfirmDeleteLibraryResource(resourceType, resourceId, anchor);
                                 }
                             });
                     }});
    OpenDropdown(anchor, std::move(items), 300.0f);
}

void Shell::ConfirmDeleteLibraryResource(const std::string& resourceType, const std::string& resourceId,
                                         el::rect anchor)
{
    const auto* r = mState.FindLibraryResource(resourceType, resourceId);

    if (r == nullptr)
    {
        return;
    }

    // The engine refuses one in use; the usage asked for when the row's menu opened says so here.
    mSession.Commands().QueryResourceUsage(resourceType, resourceId);
    const std::string name = r->name.empty() ? r->id : r->name;
    const auto usage = mState.resourceUsage.find(resourceType + ":" + resourceId);
    std::vector<MenuItem> confirm{{"Delete " + name + " from the library?", false, true, {}}};

    if (usage != mState.resourceUsage.end() && usage->second.inUse)
    {
        confirm.push_back({usage->second.detail + ": it cannot be deleted while in use.", false, true, {}});
    }
    else
    {
        confirm.push_back({"Delete", false, false, [this, resourceType, resourceId] {
                               mSession.Commands().DeleteLibraryResource(resourceType, resourceId);
                           }});
    }

    confirm.push_back({"Cancel", false, false, [] {}});
    OpenDropdown(anchor, std::move(confirm), 300.0f);
}

void Shell::EditLibraryResource(const std::string& resourceType, const std::string& resourceId, el::rect anchor)
{
    // The web UI's edit (resourceBrowser.ts: name, category, tags), as a drop-down of fields under
    // the row: the browser stays open behind it.
    const auto* r = mState.FindLibraryResource(resourceType, resourceId);

    if (r == nullptr)
    {
        return;
    }

    struct Fields
    {
        std::shared_ptr<el::basic_input_box> name, category, tags;
    };

    auto fields = std::make_shared<Fields>();
    std::string tagText;

    for (const auto& t : ResourceTags(*r))
    {
        tagText += (tagText.empty() ? "" : ", ") + t;
    }

    const auto field = [this](const char* label, const std::string& value,
                              std::shared_ptr<el::basic_input_box>& box) -> Ptr {
        auto [element, input] = el::input_box("", Font(Weight::Regular), 1.0f);
        input->set_text(value);
        box = input;
        auto title = std::make_shared<Text>(mTheme, label, 11.0f, Weight::SemiBold);
        title->Colour(mTheme.TextSecondary());
        auto boxed = Over(PadXY(8, 0, Middle(el::share(std::move(element)))),
                          el::share(Surface(mTheme.Input(), mTheme.Border(), 6.0f)));
        return Col({FixedHeight(18, title), FixedHeight(32, boxed), VGap(8)});
    };

    auto save = std::make_shared<Button>(
        mTheme, "Save",
        [this, fields, resourceType, resourceId] {
            std::vector<std::string> tags;
            std::string text = fields->tags->get_utf8();
            std::replace(text.begin(), text.end(), ';', ',');
            std::size_t at = 0;

            while (at <= text.size())
            {
                const auto end = std::min(text.find(',', at), text.size());
                auto tag = text.substr(at, end - at);
                tag.erase(0, tag.find_first_not_of(" \t"));
                tag.erase(tag.find_last_not_of(" \t") + 1);

                if (!tag.empty())
                {
                    tags.push_back(tag);
                }

                at = end + 1;
            }

            const auto name = fields->name->get_utf8();

            if (!name.empty())
            {
                mSession.Commands().UpdateLibraryResource(resourceType, resourceId, name, fields->category->get_utf8(),
                                                          tags);
            }

            CloseDropdown();
        },
        Button::Style::Primary);
    save->MinWidth(80);
    auto cancel = std::make_shared<Button>(mTheme, "Cancel", [this] { CloseDropdown(); }, Button::Style::Plain);
    cancel->MinWidth(80);

    auto panel = Pad(
        12, 10, 12, 10,
        Col({field("NAME", r->name.empty() ? r->id : r->name, fields->name),
             field("CATEGORY", r->category, fields->category), field("TAGS (COMMA SEPARATED)", tagText, fields->tags),
             FixedHeight(32, Row({Stretch(), cancel, HGap(8), save}))}));
    CloseDropdown();
    mDropdown = KeyLayer(PlacePanel(panel, el::rect{anchor.right - 360.0f, anchor.top, anchor.right, anchor.bottom},
                                    360.0f, 220.0f, false, [this] { CloseDropdown(); }));
    mSession.View().add(mDropdown);
}
} // namespace nanoq::ui
