// The preset chooser's parts (PresetChooser.cpp puts them together): the folder column, with
// Recents, Favourites, All and the user's folders as a tree with their counts (a right click edits
// a folder); the presets, filtered and ordered as the web UI's library (uiclient::FilterPresets),
// as cards; and the Setlists section, which chooses, steps and edits setlists as a list (the pads
// are PadsView.cpp). A tap loads a preset, asking first when there are unsaved changes; a right
// click rates, files, adds to the setlist or deletes it. Every edit is an engine command
// (folders, setlists, ratings), answered with the new state.

#include "ui/shell/Shell.h"
#include "ui/widgets/ControlSkin.h"
#include "uiclient/FeatureFlags.h"

#include "uiclient/PresetBrowse.h"
#include "uiclient/ToneSharing.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

void Shell::LoadWithConfirm(const std::string& presetId)
{
    if (presetId == mState.activePresetId)
    {
        ShowPage(Page::Chain);
        return;
    }

    const auto* summary = mState.FindPresetSummary(presetId);
    ConfirmDiscardThen("Load " + (summary != nullptr ? summary->name : std::string("this preset")), [this, presetId] {
        mSession.Commands().LoadPreset(presetId);
        ShowPage(Page::Chain);
    });
}

void Shell::ConfirmDiscardThen(const std::string& what, std::function<void()> proceed)
{
    if (!mState.activePresetDirty)
    {
        proceed();
        return;
    }

    OpenConfirm("Unsaved changes", what + " and lose the changes to the current one?", "Load", std::move(proceed));
}

void Shell::StepPresetWithConfirm(int direction)
{
    ConfirmDiscardThen(direction < 0 ? "Load the previous preset" : "Load the next preset",
                       [this, direction] { mSession.Commands().StepPreset(direction); });
}

void Shell::SelectSetlistSlot(int index)
{
    const auto* setlist = ActiveSetlist(mState);

    if (setlist == nullptr || index < 0 || index >= static_cast<int>(setlist->presetIds.size()))
    {
        return;
    }

    const auto& presetId = setlist->presetIds[static_cast<std::size_t>(index)];
    const auto select = [this, index] { mSession.Commands().SetSetlistCursor(index); };

    // A slot holding the preset playing moves the cursor only: nothing is lost.
    if (presetId.empty() || presetId == mState.activePresetId)
    {
        select();
        return;
    }

    const auto* summary = mState.FindPresetSummary(presetId);
    ConfirmDiscardThen("Load " + (summary != nullptr ? summary->name : presetId), select);
}

void Shell::DeleteWithConfirm(const std::string& presetId)
{
    const auto* summary = mState.FindPresetSummary(presetId);
    const std::string name = summary != nullptr ? summary->name : std::string();
    OpenConfirm("Delete preset",
                "Delete " + (name.empty() ? std::string("this preset") : name) + "? It cannot be undone.", "Delete",
                [this, presetId, name] {
                    const bool wasActive = presetId == mState.activePresetId;
                    mSession.Commands().DeletePreset(presetId);
                    ShowToast("Preset deleted", name, false);

                    // The one playing went with it: load the first one left, as Soundshed Guitar does.
                    if (wasActive)
                    {
                        for (const auto& p : mState.presetList)
                        {
                            if (p.id != presetId)
                            {
                                mSession.Commands().LoadPreset(p.id);
                                break;
                            }
                        }
                    }
                });
}

void Shell::PostMenu(const std::string& title, std::vector<MenuItem> items)
{
    // A menu opened from another menu's item: once that menu has closed.
    mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), title, items = std::move(items)]() mutable {
        if (alive.lock())
        {
            OpenMenu(title, std::move(items));
        }
    });
}

void Shell::PostPrompt(const std::string& title, const std::string& initial, const std::string& confirmLabel,
                       std::function<void(const std::string&)> onSubmit)
{
    PostPrompt(title, initial, confirmLabel, PromptOptions{}, std::move(onSubmit));
}

void Shell::PostPrompt(const std::string& title, const std::string& initial, const std::string& confirmLabel,
                       PromptOptions options, std::function<void(const std::string&)> onSubmit)
{
    mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), title, initial, confirmLabel,
                          options = std::move(options), onSubmit = std::move(onSubmit)] {
        if (alive.lock())
        {
            OpenPrompt(title, initial, confirmLabel, options, onSubmit);
        }
    });
}

namespace
{
/// A bank number typed into a prompt: a whole number from 0, or -1 for none.
int TypedBank(const std::string& text)
{
    char* end = nullptr;
    const long bank = std::strtol(text.c_str(), &end, 10);
    return end != text.c_str() && bank >= 0 && bank < 10000 ? static_cast<int>(bank) : -1;
}

/// A setlist in the Setlists list (.setlist-item): its name and a pill with its bank ("Bank 2",
/// "No Bank"); the one chosen filled with the accent.
class SetlistItemRow final : public Pressable
{
  public:
    SetlistItemRow(const Theme& theme, std::string name, std::string bank, bool active)
        : mTheme(theme), mName(std::move(name)), mBank(std::move(bank)), mActive(active)
    {
        AccessName(mName);
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::ListItem;
        info.value = mBank;
        info.selected = mActive;
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{120.0f, 32.0f}, {el::full_extent, 32.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(0.0f, 2.0f);

        if (mActive || mHover || HasFocus())
        {
            cnv.fill_style(mActive ? mTheme.Accent() : mTheme.Text().opacity(0.06f));
            cnv.fill_round_rect(b, 6.0f);
        }

        const auto ink = mActive ? mTheme.OnAccent() : mTheme.Text();
        const auto pillFont = Font(Weight::Medium).size(10.0f);
        const float pillWidth = MeasureTextWidth(mBank, pillFont) + 12.0f;
        const float cy = b.top + b.height() * 0.5f;
        const el::rect pill{b.right - 8.0f - pillWidth, cy - 9.0f, b.right - 8.0f, cy + 9.0f};
        cnv.fill_style(ink.opacity(0.14f));
        cnv.fill_round_rect(pill, 9.0f);
        FillText(cnv, mBank, pillFont, ink, pill, HAlign::Centre);
        const auto nameFont = Font(Weight::Regular).size(12.5f);
        FillText(cnv, Ellipsize(cnv, mName, nameFont, pill.left - b.left - 16.0f), nameFont, ink,
                 el::rect{b.left + 8.0f, b.top, pill.left - 8.0f, b.bottom}, HAlign::Left);
    }

  private:
    const Theme& mTheme;
    std::string mName, mBank;
    bool mActive;
};

/// A slot of the setlist being edited (.setlist-slot): a dashed box with the preset's name and a
/// x that takes it out; the slot the setlist is on is filled with the accent. A tap loads it.
class SetlistSlotRow final : public Pressable
{
  public:
    SetlistSlotRow(const Theme& theme, std::string title, bool active)
        : mTheme(theme), mTitle(std::move(title)), mActive(active)
    {
        AccessName(mTitle);
    }

    std::function<void()> onRemove;

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::ListItem;
        info.selected = mActive;
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{120.0f, 40.0f}, {el::full_extent, 40.0f}};
    }

    bool key(const el::context& ctx, el::key_info k) override
    {
        if (onRemove && k.action == el::key_action::press && k.key == el::key_code::_delete)
        {
            onRemove();
            return true;
        }

        return Pressable::key(ctx, k);
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state == el::mouse_button::left && Cross(ctx.bounds).inset(-4.0f, -4.0f).includes(btn.pos))
        {
            if (!btn.down && onRemove)
            {
                onRemove();
            }

            return true;
        }

        return Pressable::click(ctx, btn);
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool over = status != el::cursor_tracking::leaving && Cross(ctx.bounds).inset(-4.0f, -4.0f).includes(p);

        if (over != mCrossHover)
        {
            mCrossHover = over;
            ctx.view.refresh(ctx.bounds);
        }

        return Pressable::cursor(ctx, p, status);
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(0.0f, 3.0f);

        if (mActive)
        {
            cnv.fill_style(mTheme.Accent());
            cnv.fill_round_rect(b, 8.0f);
        }
        else
        {
            if (mHover)
            {
                cnv.fill_style(mTheme.Text().opacity(0.05f));
                cnv.fill_round_rect(b, 8.0f);
            }

            cnv.stroke_style(HasFocus() ? mTheme.Accent() : mTheme.BorderStrong());
            cnv.line_width(1.0f);
            StrokeDashedRoundRect(cnv, b.inset(0.5f, 0.5f), 8.0f, 4.0f, 3.0f);
        }

        const auto ink = mActive ? mTheme.OnAccent() : mTheme.Text();
        const auto cross = Cross(ctx.bounds);
        const auto font = Font(Weight::Medium).size(12.0f);
        FillText(cnv, Ellipsize(cnv, mTitle, font, cross.left - b.left - 16.0f), font, ink,
                 el::rect{b.left + 8.0f, b.top, cross.left - 4.0f, b.bottom}, HAlign::Left);
        FillText(cnv, "\xC3\x97", Font(Weight::Regular).size(15.0f),
                 mCrossHover ? (mActive ? ink : mTheme.Error()) : ink.opacity(0.7f), cross, HAlign::Centre);
    }

  private:
    [[nodiscard]] static el::rect Cross(const el::rect& bounds)
    {
        const float cy = bounds.top + bounds.height() * 0.5f;
        return el::rect{bounds.right - 30.0f, cy - 11.0f, bounds.right - 8.0f, cy + 11.0f};
    }

    const Theme& mTheme;
    std::string mTitle;
    bool mActive;
    bool mCrossHover = false;
};
} // namespace

void Shell::OpenPresetRowMenu(const PresetSummary& preset)
{
    const bool favourite = IsPresetFavorite(mState, preset.id);
    const std::string id = preset.id;
    const std::string name = preset.name;
    auto& commands = mSession.Commands();
    std::vector<MenuItem> items;
    items.push_back({"Load", false, false, [this, id] { LoadWithConfirm(id); }});
    items.push_back({favourite ? "Remove from favourites" : "Add to favourites", false, false,
                     [&commands, id, favourite] { commands.SetPresetFavorite(id, !favourite); }});

    const int rating = PresetRating(mState, id);
    items.push_back({"Rate...", false, false, [this, id, name, rating] {
                         std::vector<MenuItem> rates;

                         for (int stars = 5; stars >= 1; --stars)
                         {
                             rates.push_back({std::to_string(stars) + (stars == 1 ? " star" : " stars"),
                                              rating == stars, false,
                                              [this, id, stars] { mSession.Commands().SetPresetRating(id, stars); }});
                         }

                         rates.push_back({"No rating", rating == 0, false,
                                          [this, id] { mSession.Commands().SetPresetRating(id, 0); }});
                         PostMenu("Rate " + name, std::move(rates));
                     }});

    items.push_back({"Move to folder...", false, false, [this, id, name] {
                         const auto flat = FlattenPresetFolders(mState.presetFolders);
                         std::vector<MenuItem> rows;
                         rows.push_back({"No folder", false, false,
                                         [this, id] { mSession.Commands().MovePresetToFolder(id, kPresetFolderAll); }});

                         for (const auto& [f, depth] : flat)
                         {
                             const bool here =
                                 std::find(f->presetIds.begin(), f->presetIds.end(), id) != f->presetIds.end();
                             rows.push_back({std::string(static_cast<std::size_t>(depth) * 2, ' ') + f->name, here,
                                             false, [this, id, folderId = f->id] {
                                                 mSession.Commands().MovePresetToFolder(id, folderId);
                                             }});
                         }

                         PostMenu("Move " + name, std::move(rows));
                     }});

    for (const auto& s : mState.setlists)
    {
        if (s.id == mState.activeSetlistId)
        {
            items.push_back({"Add to " + s.name, false, false, [&commands, id] { commands.AddSetlistSlot({}, id); }});
        }
    }

    // Multi-Rig: play it alongside the presets already in the mix (the web UI's + Mixer).
    const auto& inMix = mState.mixer.activePresetIds;

    if (guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::MultiRig) &&
        std::find(inMix.begin(), inMix.end(), id) == inMix.end())
    {
        items.push_back({"Add to the mix", false, false, [&commands, id] { commands.AddActivePreset(id); }});
    }

    if (preset.source == "user")
    {
        items.push_back({"Delete...", false, false, [this, id] { DeleteWithConfirm(id); }});
    }

    OpenMenu(preset.name, std::move(items));
}

void Shell::OpenFolderMenu(const std::string& folderId)
{
    const auto* folder = FindPresetFolder(mState.presetFolders, folderId);

    if (folder == nullptr)
    {
        return;
    }

    const std::string name = folder->name;
    std::vector<MenuItem> items;
    items.push_back({"New folder inside...", false, false, [this, folderId] {
                         PostPrompt("New folder", "", "Create", [this, folderId](const std::string& entered) {
                             mSession.Commands().CreatePresetFolder(entered, folderId);
                         });
                     }});
    items.push_back({"Rename...", false, false, [this, folderId, name] {
                         PostPrompt("Rename folder", name, "Rename", [this, folderId](const std::string& entered) {
                             mSession.Commands().RenamePresetFolder(folderId, entered);
                         });
                     }});
    items.push_back(
        {"Move into...", false, false, [this, folderId, name] {
             const auto flat = FlattenPresetFolders(mState.presetFolders);
             std::vector<MenuItem> rows;
             rows.push_back({"The top level", false, false,
                             [this, folderId] { mSession.Commands().MovePresetFolder(folderId, {}); }});
             // Not into itself or a folder inside it: the engine refuses those anyway.
             const auto* self = FindPresetFolder(mState.presetFolders, folderId);

             for (const auto& [f, depth] : flat)
             {
                 if (f->id == folderId || (self != nullptr && FindPresetFolder(self->children, f->id) != nullptr))
                 {
                     continue;
                 }

                 rows.push_back(
                     {std::string(static_cast<std::size_t>(depth) * 2, ' ') + f->name, false, false,
                      [this, folderId, target = f->id] { mSession.Commands().MovePresetFolder(folderId, target); }});
             }

             PostMenu("Move " + name, std::move(rows));
         }});
    items.push_back({"Delete...", false, false, [this, folderId, name] {
                         mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), folderId, name] {
                             if (alive.lock())
                             {
                                 OpenConfirm("Delete folder",
                                             "Delete " + name +
                                                 " and the folders in it? Its presets stay in the library.",
                                             "Delete", [this, folderId] {
                                                 mQuery.folderId = kPresetFolderAll;
                                                 mSession.Commands().DeletePresetFolder(folderId);
                                             });
                             }
                         });
                     }});
    OpenMenu(name, std::move(items));
}

void Shell::SelectPresetBrowserFolder(const std::string& folderId)
{
    mQuery.folderId = folderId;
    mShowMultiRigs = false;
    mSession.Commands().SelectPresetFolder(folderId);
    Mark(kPage);
    RebuildPresetChooser();
}

Ptr Shell::BuildFolderColumn()
{
    // The virtual folders first, then the user's as a plain tree indented 12 px a level, as the
    // web's (.preset-folder-item): no icons, no heading. The virtual ones count what they list; a
    // folder counts its own presets, not those in folders inside it. Multi-Rigs are the chooser's
    // other tab, and new folders come from its folder tools.
    // Each takes a preset or another folder dragged onto it, and a folder of the user's can be
    // dragged onto another (presets/drag.ts, folderDrag.ts).
    std::vector<Ptr> rows;
    std::vector<std::string> parents; // the folder at each depth above the one being added
    const auto add = [&](const std::string& id, const std::string& label, std::size_t count, int depth, bool editable) {
        const bool chosen = mQuery.folderId == id && !mShowMultiRigs;
        auto row = std::make_shared<ListRow>(mTheme, label, std::to_string(count), chosen, false,
                                             [this, id] { SelectPresetBrowserFolder(id); });
        Ptr content = row;

        if (editable)
        {
            row->onMenu = [this, id] { OpenFolderMenu(id); };
            LibraryDrag drag;
            drag.what = LibraryDrag::What::Folder;
            drag.id = id;
            drag.name = label;
            drag.fromChooser = true;
            content = LibraryDragSource(std::move(drag), content);
        }

        parents.resize(static_cast<std::size_t>(depth));
        DropSpot spot;
        spot.kind = DropSpot::Kind::Folder;
        spot.id = id;
        spot.depth = depth;
        spot.parentId = depth > 0 ? parents.back() : std::string();
        spot.inChooser = true;
        parents.push_back(id);
        rows.push_back(
            FixedHeight(36, DropTarget(std::move(spot), Pad(static_cast<float>(depth) * 12.0f, 0, 0, 0, content))));
    };
    const auto listed = [this](const std::string& folderId) {
        PresetQuery q;
        q.folderId = folderId;
        return FilterPresets(mState, q).size();
    };
    add(kPresetFolderRecents, "Recents", listed(kPresetFolderRecents), 0, false);
    add(kPresetFolderFavorites, "Favourites", listed(kPresetFolderFavorites), 0, false);
    add(kPresetFolderAll, "All Presets", listed(kPresetFolderAll), 0, false);

    for (const auto& [f, depth] : FlattenPresetFolders(mState.presetFolders))
    {
        add(f->id, f->name, f->presetIds.size(), depth, true);
    }

    return VScroll(Col(rows));
}

Ptr Shell::BuildPresetListContent()
{
    if (mShowMultiRigs)
    {
        return BuildMultiRigList();
    }

    mQuery.tags = mPresetTags;
    const auto presets = FilterPresets(mState, mQuery);
    std::vector<Ptr> rows;

    for (const auto& p : presets)
    {
        rows.push_back(BuildPresetCard(p));
    }

    if (rows.empty())
    {
        // As the web's (.preset-list-empty): an empty library offers Tone Sharing's presets.
        const bool libraryEmpty = mState.presetList.empty();
        auto none = std::make_shared<Text>(
            mTheme, libraryEmpty ? "No presets available yet." : "No presets match your current search or folder.",
            14.0f);
        none->Colour(mTheme.TextMuted());
        rows.push_back(FixedHeight(32, PadXY(12, 0, none)));

        if (libraryEmpty && IsFeatureEnabled(mState, Feature::ToneSharing))
        {
            auto browse = std::make_shared<Button>(
                mTheme, "Browse Tone Sharing",
                [this] {
                    CloseOverlay();
                    OpenTonesCommunity(TonesTab::Presets);
                },
                Button::Style::Primary);
            rows.push_back(FixedHeight(36, PadXY(12, 0, Row({browse, Stretch()}))));
        }
    }

    return VScroll(Col(rows));
}

void Shell::RebuildPresetList()
{
    // Typing in the search box: only the list changes, so the box keeps its focus.
    if (!mPresetListSlot)
    {
        return;
    }

    Fill(*mPresetListSlot, BuildPresetListContent());
    mSession.View().layout();
    mSession.View().refresh();
}

Ptr Shell::BuildSetlistsView(Ptr tabs)
{
    // The web UI's setlist panel (setlist-panel, presets/setlists.ts): a new setlist's name and
    // bank with Create Setlist; then the setlists as a 220 px list with their banks, beside the
    // one chosen: "Name (Bank N)" over its slots, each a dashed box with a x. A right click on a
    // setlist edits or deletes it; on a slot it moves or replaces it.
    const Setlist* active = nullptr;

    for (const auto& s : mState.setlists)
    {
        if (s.id == mState.activeSetlistId)
        {
            active = &s;
        }
    }

    // ── The header (.setlist-header): name, bank (120 px), Create Setlist ──
    // As tall as the button beside them, with the dialogs' 10 px inset (PresetDetails.cpp).
    const auto field = [](const char* placeholder, std::shared_ptr<el::basic_input_box>& box, Ptr& element) {
        if (!box)
        {
            box = el::share(el::basic_input_box{placeholder, Font(Weight::Regular).size(metrics::kSmallText)});
            element = el::share(el::input_box(el::hold(box), el::rect{10.0f, 8.0f, 10.0f, 8.0f}));
        }
    };
    field("New setlist name", mSetlistNameBox, mSetlistNameElement);
    field("Bank", mSetlistBankBox, mSetlistBankElement);
    const auto createSetlist = [this] {
        auto name = mSetlistNameBox->get_utf8();
        const auto first = name.find_first_not_of(" \t");
        name =
            first == std::string::npos ? std::string() : name.substr(first, name.find_last_not_of(" \t") - first + 1);

        if (name.empty())
        {
            ShowToast("Setlist name required", "Enter a setlist name.", true);
            return;
        }

        const int bank = TypedBank(mSetlistBankBox->get_utf8());

        if (bank >= 0 && std::any_of(mState.setlists.begin(), mState.setlists.end(),
                                     [bank](const Setlist& s) { return s.bank && *s.bank == bank; }))
        {
            ShowToast("Bank already used", "Only one setlist can use a bank number.", true);
            return;
        }

        mSession.Commands().CreateSetlist(name, bank);
        mSetlistNameBox->set_text("");
        mSetlistBankBox->set_text("");
    };
    mSetlistNameBox->on_enter = [createSetlist](std::string_view) {
        createSetlist();
        return true;
    };
    auto create = std::make_shared<Button>(mTheme, "Create Setlist", createSetlist, Button::Style::Primary);
    create->TextSize(11.0f);
    const auto inputBox = [this](Ptr e) { return Rounded(mTheme.Tertiary(), 6.0f, e); };
    auto header = FixedHeight(34, Row({inputBox(mSetlistNameElement), HGap(8),
                                       FixedWidth(120, inputBox(mSetlistBankElement)), HGap(8), Middle(create)}));

    // ── The setlists (.setlist-list) ──
    std::vector<Ptr> list;

    for (const auto& s : mState.setlists)
    {
        const std::string id = s.id, name = s.name;
        const std::string bank = s.bank ? std::to_string(*s.bank) : std::string();
        auto row = std::make_shared<SetlistItemRow>(mTheme, s.name,
                                                    s.bank ? "Bank " + std::to_string(*s.bank) : std::string("No Bank"),
                                                    active != nullptr && active->id == s.id);
        row->onClick = [this, id] {
            if (id != mState.activeSetlistId)
            {
                mSession.Commands().SelectSetlist(id);
            }
        };
        row->onMenu = [this, id, name, bank](el::point) {
            OpenMenu(name, {{"Edit...", false, false,
                             [this, id, name, bank] {
                                 PostPrompt("Setlist name", name, "Next", [this, id, bank](const std::string& entered) {
                                     if (entered.find_first_not_of(" \t") == std::string::npos)
                                     {
                                         ShowToast("Setlist name required", "Enter a setlist name.", true);
                                         return;
                                     }

                                     PostPrompt("Setlist bank", bank, "Save",
                                                {.message = "Its bank number; leave it blank for none.",
                                                 .placeholder = "Bank",
                                                 .allowEmpty = true},
                                                [this, id, entered](const std::string& b) {
                                                    mSession.Commands().UpdateSetlist(id, entered, TypedBank(b));
                                                });
                                 });
                             }},
                            {"Delete...", false, false, [this, id, name] {
                                 OpenConfirm("Delete setlist", "Delete " + name + "? Its presets stay in the library.",
                                             "Delete", [this, id] { mSession.Commands().DeleteSetlist(id); });
                             }}});
        };
        list.push_back(row);
    }

    if (list.empty())
    {
        auto none = std::make_shared<Text>(mTheme, "No setlists yet.", 12.0f);
        none->Colour(mTheme.TextMuted());
        list.push_back(FixedHeight(28, PadXY(8, 0, none)));
    }

    // ── The one chosen (.setlist-editor) ──
    std::vector<Ptr> editor;
    auto title = std::make_shared<Text>(
        mTheme,
        active != nullptr
            ? active->name + (active->bank ? " (Bank " + std::to_string(*active->bank) + ")" : std::string())
            : std::string("Select a setlist"),
        13.0f, Weight::SemiBold);
    std::vector<Ptr> titleRow{title};

    if (active != nullptr)
    {
        const std::string id = active->id;
        auto addCurrent = std::make_shared<Button>(
            mTheme, "+ Current preset",
            [this, id] {
                if (!mState.activePresetId.empty())
                {
                    mSession.Commands().AddSetlistSlot(id, mState.activePresetId);
                }
            },
            Button::Style::Plain);
        addCurrent->TextSize(11.0f).Enabled(!mState.activePresetId.empty());
        auto pads = std::make_shared<Button>(
            mTheme, "Pads",
            [this] {
                CloseOverlay();
                ShowPage(Page::Pads);
            },
            Button::Style::Flat);
        pads->TextSize(11.0f);
        titleRow.insert(titleRow.end(), {HGap(8), Middle(addCurrent), HGap(4), Middle(pads)});
    }

    editor.push_back(FixedHeight(32, Row(titleRow)));
    editor.push_back(VGap(6));

    if (active != nullptr)
    {
        const std::string id = active->id;
        const int count = static_cast<int>(active->presetIds.size());

        for (int i = 0; i < count; ++i)
        {
            const auto& presetId = active->presetIds[static_cast<std::size_t>(i)];
            const auto* summary = mState.FindPresetSummary(presetId);
            const std::string title = summary != nullptr ? summary->name : presetId;
            auto slot = std::make_shared<SetlistSlotRow>(mTheme, title, i == mState.setlistCursor);
            slot->onClick = [this, i] { SelectSetlistSlot(i); };
            slot->onRemove = [this, id, i] {
                mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), id, i] {
                    if (alive.lock())
                    {
                        mSession.Commands().RemoveSetlistSlot(id, i);
                    }
                });
            };
            slot->onMenu = [this, id, i, count](el::point) {
                std::vector<MenuItem> items;

                if (i > 0)
                {
                    items.push_back({"Move up", false, false,
                                     [this, id, i] { mSession.Commands().MoveSetlistSlot(id, i, i - 1); }});
                }

                if (i + 1 < count)
                {
                    items.push_back({"Move down", false, false,
                                     [this, id, i] { mSession.Commands().MoveSetlistSlot(id, i, i + 1); }});
                }

                if (!mState.activePresetId.empty())
                {
                    items.push_back({"Put the current preset here", false, false, [this, id, i] {
                                         mSession.Commands().SetSetlistSlot(id, i, mState.activePresetId);
                                     }});
                }

                items.push_back({"Remove from the setlist", false, false,
                                 [this, id, i] { mSession.Commands().RemoveSetlistSlot(id, i); }});
                OpenMenu("Slot " + std::to_string(i + 1), std::move(items));
            };
            // Dragged onto another slot, it moves there (setlists.ts).
            LibraryDrag drag;
            drag.what = LibraryDrag::What::SetlistSlot;
            drag.id = id;
            drag.name = title;
            drag.index = i;
            drag.fromChooser = !tabs;
            DropSpot spot;
            spot.kind = DropSpot::Kind::SetlistSlot;
            spot.id = id;
            spot.index = i;
            spot.inChooser = !tabs;
            editor.push_back(DropTarget(std::move(spot), LibraryDragSource(std::move(drag), slot)));
        }

        if (count == 0)
        {
            auto none = std::make_shared<Text>(mTheme, "Load a preset, then + Current preset adds it here.", 12.0f);
            none->Colour(mTheme.TextMuted());
            editor.push_back(FixedHeight(28, PadXY(4, 0, none)));
        }
    }

    // Each half on a faint face with an edge (.setlist-list, .setlist-editor).
    const auto panel = [this](Ptr content) {
        return Over(Pad(10, 10, 10, 10, VScroll(content)),
                    el::share(Surface(mTheme.Text().opacity(0.03f), mTheme.Border(), 10.0f)));
    };
    // A preset dropped on the setlist's side joins the setlist (#setlist-slots).
    DropSpot setlistSpot;
    setlistSpot.kind = DropSpot::Kind::SetlistPanel;
    setlistSpot.id = active != nullptr ? active->id : std::string();
    setlistSpot.inChooser = !tabs;
    auto body =
        Row({FixedWidth(220, panel(Col(list))), HGap(12), DropTarget(std::move(setlistSpot), panel(Col(editor)))});
    auto content = Col({header, VGap(10), body});

    if (!tabs)
    {
        return content;
    }

    return Dressed(mTheme, mTheme.Background(), &Backdrops::main, Pad(16, 8, 16, 8, Col({tabs, VGap(8), content})));
}
} // namespace nanoq::ui
