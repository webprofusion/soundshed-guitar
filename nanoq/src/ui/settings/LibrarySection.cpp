// Settings > Library, as the web UI's settings-library.html and settings/libraryView.ts: the
// Resource Library manager. Every model and IR in the library, filtered by a search, type and
// creator (and, under More filters, source, view and category), one per row or grouped by the
// Tone3000 tone or pack they came from, with each row's Edit, path and menu, and Remove unused for
// imported entries no preset or blend uses. The section shows while Blend Tools is on, the manager
// while the Resource Library is (uiclient's LibraryManager.h holds the rules).
//
// The search box is kept across rebuilds and refills only the results, so it keeps its focus;
// the other filters rebuild the page.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "uiclient/FeatureFlags.h"
#include "uiclient/LibraryManager.h"

#include <algorithm>

namespace nanoq::ui
{
namespace lm = guitarfx::uiclient::library_manager;

namespace
{
/// Rows drawn at once; "Show more" adds as many again.
constexpr std::size_t kRowsPerPage = 60;
} // namespace

Ptr Shell::BuildLibrarySection(float textWidth)
{
    auto& ui = mLibraryManager;
    std::vector<Ptr> column;
    const auto text = [this](const std::string& value, float size, el::color colour, Weight weight = Weight::Regular) {
        auto t = std::make_shared<Text>(mTheme, value, size, weight);
        t->Colour(colour);
        return t;
    };
    const auto title = text("Resource Library", 13.0f, mTheme.Text(), Weight::SemiBold);
    column.push_back(FixedHeight(24, Middle(title)));
    auto lead = std::make_shared<Paragraph>(
        mTheme, "Manage imported NAM models and IRs used across presets and blends.", textWidth, 11.5f);
    lead->Colour(mTheme.TextSecondary());
    column.push_back(Row({el::share(el::hold_any(lead)), Stretch()}));
    column.push_back(VGap(10));

    // A drop-down button: its choice sets `target` and rebuilds the page.
    const auto chooser = [this](const std::string& label, float width,
                                std::vector<std::pair<std::string, std::string>> choices, std::string* target) {
        std::string shown = label;

        for (const auto& [name, value] : choices)
        {
            if (*target == value)
            {
                shown = name;
            }
        }

        auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        button->Size(width, 30).Label(shown, 11.0f).Caret(true);
        button->AccessName(label);
        button->onClick = [this, choices, target, b = button.get()] {
            std::vector<MenuItem> items;

            for (const auto& [name, value] : choices)
            {
                items.push_back({name, *target == value, false, [this, target, value] {
                                     *target = value;
                                     mLibraryManager.shownRows = kRowsPerPage;
                                     Mark(kPage);
                                 }});
            }

            OpenDropdown(b->Bounds(), std::move(items), std::max(180.0f, b->Bounds().width()));
        };
        return button;
    };

    // ── The filters ──
    const auto all = lm::AllItems(mState);
    const auto ofType = lm::OfType(all, ui.filter.type);
    std::vector<std::pair<std::string, std::string>> creators{{"All Creators", "all"}};

    for (const auto& c : lm::Creators(ofType))
    {
        creators.emplace_back(c, c);
    }

    if (!ui.searchBox)
    {
        auto [element, box] = el::input_box("Search models and IRs\xE2\x80\xA6", Font(Weight::Regular), 1.0f);
        ui.searchBox = box;
        ui.searchElement = el::share(std::move(element));
        ui.searchBox->on_text = [this](std::string_view typed) {
            mLibraryManager.filter.query = std::string(typed);
            mLibraryManager.shownRows = kRowsPerPage;

            if (mLibraryManager.results)
            {
                Fill(*mLibraryManager.results, BuildLibraryResults(mLibraryManager.textWidth));
                mSession.View().layout();
                mSession.View().refresh();
            }
        };
    }

    auto search =
        Over(PadXY(10, 0, Middle(ui.searchElement)), el::share(Surface(mTheme.Input(), mTheme.Border(), 6.0f)));
    const bool narrow = textWidth < 620.0f;
    auto type = chooser("Type", 130, {{"All Types", "all"}, {"Amp Models", "nam"}, {"IRs", "ir"}}, &ui.filter.type);
    auto creator = chooser("Creator", 160, creators, &ui.filter.creator);

    if (narrow)
    {
        column.push_back(FixedHeight(32, search));
        column.push_back(VGap(8));
        column.push_back(FixedHeight(30, Row({type, HGap(8), creator, Stretch()})));
    }
    else
    {
        column.push_back(FixedHeight(32, Row({Stretch(search, 1.0), HGap(8), Middle(type), HGap(8), Middle(creator)})));
    }

    column.push_back(VGap(8));

    // More filters and cleanup (a disclosure, as the web's <details>).
    auto more = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
    more->Size(190, 24).Label(
        std::string(ui.moreFilters ? "\xE2\x96\xBE" : "\xE2\x96\xB8") + " More filters and cleanup", 11.0f);
    more->onClick = [this] {
        mLibraryManager.moreFilters = !mLibraryManager.moreFilters;
        Mark(kPage);
    };
    column.push_back(FixedHeight(24, Row({more, Stretch()})));

    if (ui.moreFilters)
    {
        std::vector<std::pair<std::string, std::string>> categories{{"All Categories", "all"}};

        for (const auto& c : lm::Categories(ofType))
        {
            categories.emplace_back(c, c);
        }

        std::string view = ui.filter.grouped ? "grouped" : "list";
        auto viewButton = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        viewButton->Size(140, 30).Label(ui.filter.grouped ? "Group by Tone" : "List View", 11.0f).Caret(true);
        viewButton->AccessName("View");
        viewButton->onClick = [this, b = viewButton.get()] {
            std::vector<MenuItem> items;

            for (const bool grouped : {false, true})
            {
                items.push_back({grouped ? "Group by Tone" : "List View", mLibraryManager.filter.grouped == grouped,
                                 false, [this, grouped] {
                                     mLibraryManager.filter.grouped = grouped;
                                     mLibraryManager.shownRows = kRowsPerPage;
                                     Mark(kPage);
                                 }});
            }

            OpenDropdown(b->Bounds(), std::move(items), 180.0f);
        };
        column.push_back(VGap(6));
        column.push_back(FixedHeight(30, Row({chooser("Source", 130,
                                                      {{"All Sources", "all"},
                                                       {"Built-in", "built-in"},
                                                       {"Local", "local"},
                                                       {"Tone3000", "tone3000"},
                                                       {"Imported", "imported"}},
                                                      &ui.filter.source),
                                              HGap(8), viewButton, HGap(8),
                                              chooser("Category", 160, categories, &ui.filter.category), Stretch()})));

        // Cleanup unused: imported entries no preset or blend uses (Resource Cleanup Tools).
        if (guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::ResourceCleanup))
        {
            auto label = text("CLEANUP UNUSED", 11.0f, mTheme.TextSecondary(), Weight::SemiBold);
            auto remove = std::make_shared<Button>(
                mTheme, "Remove unused",
                [this] {
                    const auto scope = mLibraryManager.cleanupScope;
                    const auto candidates = lm::CleanupCandidates(mState, scope);

                    if (candidates.empty())
                    {
                        ShowToast("Cleanup", "No unused imported resources found.", false);
                        return;
                    }

                    const std::string what = scope == "all"   ? "unused resources"
                                             : scope == "nam" ? "unused amp models"
                                                              : "unused IRs";
                    OpenConfirm("Cleanup",
                                "Remove the " + what + " among " + std::to_string(candidates.size()) +
                                    " imported? Any a preset or blend uses are kept, and built-in and local ones "
                                    "are never removed.",
                                "Remove", [this, scope] {
                                    lm::RemoveUnused(mSession.Commands(), scope, lm::CleanupCandidates(mState, scope));
                                });
                },
                Button::Style::Plain);
            remove->TextSize(metrics::kSmallText);
            column.push_back(VGap(8));
            column.push_back(FixedHeight(
                30, Row({FixedWidth(130, Middle(label)),
                         Middle(chooser("Cleanup scope", 160,
                                        {{"All types", "all"}, {"Amp models only", "nam"}, {"IRs only", "ir"}},
                                        &ui.cleanupScope)),
                         HGap(8), Middle(FixedHeight(30, remove)), Stretch()})));
        }
    }

    column.push_back(VGap(10));

    // ── The results: their own slot, so the search refills them alone ──
    ui.textWidth = textWidth;
    ui.results = MakeSlot();
    Fill(*ui.results, BuildLibraryResults(textWidth));
    column.push_back(ui.results);

    return Rounded(mTheme.Card(), 8.0f, Pad(16, 14, 16, 14, Col(column)));
}

Ptr Shell::BuildLibraryResults(float textWidth)
{
    auto& ui = mLibraryManager;
    const auto all = lm::AllItems(mState);
    const auto filtered = lm::Filtered(mState, ui.filter);
    std::vector<Ptr> rows;
    const auto text = [this](const std::string& value, float size, el::color colour, Weight weight = Weight::Regular) {
        auto t = std::make_shared<Text>(mTheme, value, size, weight);
        t->Colour(colour);
        return t;
    };
    const auto action = [this](const std::string& label, std::function<void(el::rect)> onClick, float width) {
        auto b = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        b->Size(width, 26).Label(label, 11.0f);
        b->onClick = [onClick = std::move(onClick), at = b.get()] { onClick(at->Bounds()); };
        return b;
    };
    const auto rule = [this] { return FixedHeight(1, Fill(mTheme.Border(), std::make_shared<el::element>())); };
    // Whether a preset or blend uses it: the engine's index, asked once per entry shown.
    const auto inUse = [this](const std::string& type, const std::string& id) {
        const auto it = mState.resourceUsage.find(type + ":" + id);

        if (it == mState.resourceUsage.end())
        {
            mSession.Commands().QueryResourceUsage(type, id);
            return false;
        }

        return it->second.inUse;
    };
    const auto titleRow = [&](const std::string& name, bool used, const std::string& usedTitle) {
        std::vector<Ptr> cells{text(name, 12.5f, mTheme.Text(), Weight::SemiBold)};

        if (used)
        {
            auto pill = text("IN USE", 9.0f, mTheme.Accent(), Weight::SemiBold);
            pill->Fixed(true);
            cells.push_back(HGap(8));
            cells.push_back(pill);
            (void)usedTitle;
        }

        cells.push_back(Stretch());
        return FixedHeight(18, Row(cells));
    };

    if (ui.filter.grouped)
    {
        const auto groups = lm::GroupByTone(filtered);
        auto summary = text(lm::Summary(groups.size(), all.size(), true), 11.0f, mTheme.TextSecondary());
        rows.push_back(FixedHeight(20, summary));

        if (groups.empty())
        {
            rows.push_back(FixedHeight(
                40, Middle(text("No grouped resources match the current filters.", 11.5f, mTheme.TextMuted()))));
        }

        std::size_t shown = 0;

        for (const auto& group : groups)
        {
            if (shown++ >= ui.shownRows)
            {
                break;
            }

            std::size_t used = 0;

            for (const auto& item : group.items)
            {
                used += inUse(item.type, item.resource->id) ? 1 : 0;
            }

            // Delete Group: the group's entries a preset or blend does not use, and their files.
            std::vector<lm::Item> items = group.items;
            const std::string groupTitle = group.title;
            auto remove = action(
                "Delete Group",
                [this, items, groupTitle](el::rect) {
                    OpenConfirm("Delete Group",
                                "Delete " + groupTitle + "'s " + std::to_string(items.size()) +
                                    " resources from the library, and their files? Any a preset or blend uses are "
                                    "kept.",
                                "Delete", [this, items] { lm::RemoveUnused(mSession.Commands(), "all", items); });
                },
                100.0f);
            rows.push_back(rule());
            rows.push_back(VGap(8));
            rows.push_back(
                Row({Stretch(Col({titleRow(group.title, used > 0, std::to_string(used) + " used by presets or blends"),
                                  VGap(2), FixedHeight(16, text(lm::GroupMeta(group), 10.5f, mTheme.TextSecondary())),
                                  FixedHeight(15, text(group.groupId, 10.0f, mTheme.TextMuted()))}),
                             1.0),
                     HGap(8), FixedWidth(100, Middle(remove))}));
            rows.push_back(VGap(8));
        }

        if (groups.size() > ui.shownRows)
        {
            rows.push_back(FixedHeight(30, Row({action(
                                                    "Show more",
                                                    [this](el::rect) {
                                                        mLibraryManager.shownRows += kRowsPerPage;
                                                        Mark(kPage);
                                                    },
                                                    100.0f),
                                                Stretch()})));
        }

        return Col(rows);
    }

    auto summary = text(lm::Summary(filtered.size(), all.size(), false), 11.0f, mTheme.TextSecondary());
    rows.push_back(FixedHeight(20, summary));

    if (filtered.empty())
    {
        rows.push_back(
            FixedHeight(40, Middle(text("No resources match the current filters.", 11.5f, mTheme.TextMuted()))));
    }

    std::size_t shown = 0;

    for (const auto& item : filtered)
    {
        if (shown++ >= ui.shownRows)
        {
            break;
        }

        const auto& r = *item.resource;
        const std::string type = item.type, id = r.id;
        // Right-aligned in a column wide enough for all three, so the rows' text lines up.
        constexpr float actionsWidth = 52.0f + 6.0f + 96.0f + 6.0f + 60.0f;
        std::vector<Ptr> actions{Stretch()};

        if (lm::CanEdit(item))
        {
            actions.push_back(
                action("Edit", [this, type, id](el::rect at) { EditLibraryResource(type, id, at); }, 52.0f));
            actions.push_back(HGap(6));
        }

        if (const auto label = lm::PathActionLabel(item); !label.empty())
        {
            actions.push_back(action(
                label, [this, type, id](el::rect) { mSession.Commands().BrowseLibraryResourcePath(type, id); }, 96.0f));
            actions.push_back(HGap(6));
        }

        actions.push_back(action(
            "More\xE2\x80\xA6", [this, type, id](el::rect at) { OpenLibraryResourceMenu(type, id, at); }, 60.0f));

        std::vector<Ptr> meta{text(lm::RowMeta(item), 10.5f, mTheme.TextSecondary())};

        if (r.fileMissing)
        {
            auto missing = text("Missing File", 10.5f, mTheme.Error(), Weight::SemiBold);
            missing->Fixed(true);
            meta.insert(meta.begin(), HGap(8));
            meta.insert(meta.begin(), missing);
        }

        rows.push_back(rule());
        rows.push_back(VGap(8));
        rows.push_back(
            Row({Stretch(Col({titleRow(r.name.empty() ? r.id : r.name, inUse(type, id), "Used by preset"), VGap(2),
                              FixedHeight(16, Row(meta)),
                              FixedHeight(15, text(r.filePath.empty() ? std::string("(no file path)") : r.filePath,
                                                   10.0f, mTheme.TextMuted()))}),
                         1.0),
                 HGap(8), FixedWidth(actionsWidth, Middle(Row(actions)))}));
        rows.push_back(VGap(8));
    }

    if (filtered.size() > ui.shownRows)
    {
        rows.push_back(FixedHeight(30, Row({action(
                                                "Show more",
                                                [this](el::rect) {
                                                    mLibraryManager.shownRows += kRowsPerPage;
                                                    Mark(kPage);
                                                },
                                                100.0f),
                                            Stretch()})));
    }

    (void)textWidth;
    return Col(rows);
}

void Shell::RefreshLibraryResults()
{
    if (mLibraryManager.refillPosted || !mLibraryManager.results)
    {
        return;
    }

    mLibraryManager.refillPosted = true;
    mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive)] {
        if (!alive.lock())
        {
            return;
        }

        mLibraryManager.refillPosted = false;

        if (mPage == Page::Settings && mSettingsSection == SettingsSection::Library && mLibraryManager.results)
        {
            Fill(*mLibraryManager.results, BuildLibraryResults(mLibraryManager.textWidth));
            mSession.View().layout();
            mSession.View().refresh();
        }
    });
}
} // namespace nanoq::ui
