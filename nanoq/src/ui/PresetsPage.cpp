// The presets page: the library with a search, the All / Favourites / Recents views and the
// user's folders (filtered and ordered as the web UI's library: uiclient::FilterPresets), and the
// setlists. A tap loads a preset, asking first when there are unsaved changes; a right click offers
// favourite and delete.

#include "ui/Shell.h"

#include "uiclient/PresetBrowse.h"

#include <algorithm>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

void Shell::LoadWithConfirm(const std::string& presetId)
{
    if (presetId == mState.activePresetId)
    {
        ShowPage(Page::Effect);
        return;
    }

    const auto load = [this, presetId] {
        mSession.Commands().LoadPreset(presetId);
        ShowPage(Page::Effect);
    };

    if (!mState.activePresetDirty)
    {
        load();
        return;
    }

    const auto* summary = mState.FindPresetSummary(presetId);
    OpenConfirm("Unsaved changes",
                "Load " + (summary != nullptr ? summary->name : std::string("this preset")) + " and lose the changes to the current one?",
                "Load", load);
}

void Shell::DeleteWithConfirm(const std::string& presetId)
{
    const auto* summary = mState.FindPresetSummary(presetId);
    OpenConfirm("Delete preset", "Delete " + (summary != nullptr ? summary->name : std::string("this preset")) + "? It cannot be undone.",
                "Delete", [this, presetId] {
                    const bool wasActive = presetId == mState.activePresetId;
                    mSession.Commands().DeletePreset(presetId);

                    // The one playing went with it: load the first one left, as Soundshed Guitar does.
                    if (wasActive)
                    {
                        for (const auto& p : mState.presetList)
                            if (p.id != presetId)
                            {
                                mSession.Commands().LoadPreset(p.id);
                                break;
                            }
                    }
                });
}

void Shell::OpenPresetRowMenu(const PresetSummary& preset)
{
    const bool favourite = IsPresetFavorite(mState, preset.id);
    const std::string id = preset.id;
    std::vector<MenuItem> items;
    items.push_back({favourite ? "Remove from favourites" : "Add to favourites", false, false,
                     [this, id, favourite] { mSession.Commands().SetPresetFavorite(id, !favourite); }});
    if (preset.source == "user")
        items.push_back({"Delete...", false, false, [this, id] { DeleteWithConfirm(id); }});
    OpenMenu(preset.name, std::move(items));
}

namespace
{
void FlattenFolders(const std::vector<PresetFolder>& folders, int depth, std::vector<std::pair<const PresetFolder*, int>>& out)
{
    for (const auto& f : folders)
    {
        out.emplace_back(&f, depth);
        FlattenFolders(f.children, depth + 1, out);
    }
}
} // namespace

Ptr Shell::BuildPresetListContent()
{
    const auto presets = FilterPresets(mState, mQuery);
    std::vector<Ptr> rows;
    for (const auto& p : presets)
    {
        auto row = std::make_shared<ListRow>(mTheme, p.name, mQuery.folderId == kPresetFolderAll ? p.category : std::string(),
                                             p.id == mState.activePresetId, false, [this, id = p.id] { LoadWithConfirm(id); });
        row->onMenu = [this, p] { OpenPresetRowMenu(p); };
        rows.push_back(row);
    }

    if (rows.empty())
    {
        auto none = std::make_shared<Text>(mTheme, mQuery.text.empty() ? "Nothing here yet." : "No presets match.", 14.0f);
        none->Colour(mTheme.TextMuted());
        rows.push_back(FixedHeight(32, PadXY(12, 0, none)));
    }

    if (mPresetCount)
        mPresetCount->Set(std::to_string(presets.size()) + (presets.size() == 1 ? " preset" : " presets"));

    return VScroll(Col(rows));
}

void Shell::RebuildPresetList()
{
    // Typing in the search box: only the list changes, so the box keeps its focus.
    if (!mPresetListSlot)
        return;
    Fill(*mPresetListSlot, BuildPresetListContent());
    mSession.View().layout();
    mSession.View().refresh();
}

Ptr Shell::BuildPresetsPage()
{
    auto tab = [this](const char* label, bool setlists) {
        auto b = std::make_shared<Button>(mTheme, label, [this, setlists] {
            mShowSetlists = setlists;
            Mark(kPage);
        }, Button::Style::Flat);
        b->Selected(mShowSetlists == setlists).MinWidth(100);
        return b;
    };
    Ptr tabs = FixedHeight(44, Row({tab("Presets", false), HGap(4), tab("Setlists", true), Stretch()}));

    if (mShowSetlists)
    {
        auto& commands = mSession.Commands();

        const Setlist* active = nullptr;
        for (const auto& s : mState.setlists)
            if (s.id == mState.activeSetlistId)
                active = &s;
        if (active == nullptr && !mState.setlists.empty())
            active = &mState.setlists.front();

        auto picker = std::make_shared<Button>(mTheme, active != nullptr ? active->name : std::string("No setlists"), [this] {
            std::vector<MenuItem> items;
            for (const auto& s : mState.setlists)
                items.push_back({s.name, s.id == mState.activeSetlistId, false, [this, id = s.id] {
                                     mSession.Commands().SelectSetlist(id);
                                 }});
            if (items.empty())
                items.push_back({"Setlists are made in Soundshed Guitar.", false, true, {}});
            OpenMenu("Setlist", std::move(items));
        }, Button::Style::Plain);
        picker->MinWidth(240);

        const int cursor = mState.setlistCursor;
        const int count = active != nullptr ? static_cast<int>(active->presetIds.size()) : 0;
        auto prev = std::make_shared<Button>(mTheme, "\xE2\x80\xB9", [&commands, cursor, count] {
            if (count > 0)
                commands.SetSetlistCursor((cursor - 1 + count) % count);
        }, Button::Style::Plain);
        auto next = std::make_shared<Button>(mTheme, "\xE2\x80\xBA", [&commands, cursor, count] {
            if (count > 0)
                commands.SetSetlistCursor((cursor + 1) % count);
        }, Button::Style::Plain);
        prev->MinWidth(48).TextSize(20);
        next->MinWidth(48).TextSize(20);

        std::vector<Ptr> rows;
        if (active != nullptr)
        {
            for (int i = 0; i < count; ++i)
            {
                const auto& id = active->presetIds[static_cast<std::size_t>(i)];
                const auto* summary = mState.FindPresetSummary(id);
                rows.push_back(std::make_shared<ListRow>(mTheme, std::to_string(i + 1) + ".  " + (summary != nullptr ? summary->name : id),
                                                         summary != nullptr ? summary->category : std::string(), i == cursor, false,
                                                         [&commands, i] { commands.SetSetlistCursor(i); }));
            }
        }
        if (rows.empty())
        {
            auto none = std::make_shared<Text>(mTheme, "This setlist is empty.", 14.0f);
            none->Colour(mTheme.TextMuted());
            rows.push_back(FixedHeight(32, PadXY(12, 0, none)));
        }

        return Fill(mTheme.Background(),
                    Pad(16, 8, 16, 8,
                        Col({tabs, FixedHeight(48, PadXY(0, 4, Row({picker, HGap(8), prev, HGap(4), next, Stretch()}))), VScroll(Col(rows))})));
    }

    // ── The library ──
    if (!mSearchBox)
    {
        // Made once and kept: the page is rebuilt when the library changes, and a search box
        // made afresh each time would lose what is typed in it, and its focus.
        auto [element, box] = el::input_box("Search presets", Font(Weight::Regular), 1.0f);
        mSearchBox = box;
        mSearchElement = el::share(std::move(element));
        mSearchBox->on_text = [this](std::string_view text) {
            mQuery.text = std::string(text);
            RebuildPresetList();
        };
    }

    auto chip = [this](const char* label, const std::string& folderId) {
        auto b = std::make_shared<Button>(mTheme, label, [this, folderId] {
            mQuery.folderId = folderId;
            Mark(kPage);
        }, Button::Style::Flat);
        b->Selected(mQuery.folderId == folderId).MinWidth(96).TextSize(metrics::kSmallText);
        return b;
    };

    // Folders: the user's own, as a flat list with the nesting shown in the name.
    const auto* folder = FindPresetFolder(mState.presetFolders, mQuery.folderId);
    auto folders = std::make_shared<Button>(mTheme, folder != nullptr ? folder->name : std::string("Folders"), [this] {
        std::vector<std::pair<const PresetFolder*, int>> flat;
        FlattenFolders(mState.presetFolders, 0, flat);
        std::vector<MenuItem> items;
        for (const auto& [f, depth] : flat)
            items.push_back({std::string(static_cast<std::size_t>(depth) * 2, ' ') + f->name, f->id == mQuery.folderId, false,
                             [this, id = f->id] {
                                 mQuery.folderId = id;
                                 Mark(kPage);
                             }});
        if (items.empty())
            items.push_back({"No folders yet", false, true, {}});
        OpenMenu("Folders", std::move(items));
    }, Button::Style::Flat);
    folders->Selected(folder != nullptr).MinWidth(120).TextSize(metrics::kSmallText);

    mPresetCount = std::make_shared<Text>(mTheme, "", metrics::kSmallText);
    mPresetCount->Colour(mTheme.TextMuted()).Alignment(Text::Align::Right);

    auto newPreset = std::make_shared<Button>(mTheme, "New", [this] { mSession.Commands().NewPreset(); }, Button::Style::Plain);
    newPreset->MinWidth(72);

    mPresetListSlot = MakeSlot();
    Fill(*mPresetListSlot, BuildPresetListContent());

    auto page = Col({tabs, FixedHeight(48, PadXY(0, 4, Row({mSearchElement, HGap(8), newPreset}))),
                     FixedHeight(44, Row({chip("All", kPresetFolderAll), HGap(4), chip("Favourites", kPresetFolderFavorites), HGap(4),
                                          chip("Recents", kPresetFolderRecents), HGap(4), folders, Stretch(), FixedWidth(110, mPresetCount)})),
                     mPresetListSlot});
    return Fill(mTheme.Background(), Pad(16, 8, 16, 8, page));
}
} // namespace nanoq::ui
