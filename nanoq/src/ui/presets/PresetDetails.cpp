// The preset details dialog: naming, describing and tagging the active preset, or a new one.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "uiclient/FeatureFlags.h"
#include "uiclient/PresetBrowse.h"
#include "uiclient/PresetInspect.h"
#include "uiclient/ToneSharing.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <utility>

namespace nanoq::ui
{
void Shell::EditActivePreset()
{
    if (mState.activePreset && guitarfx::uiclient::CanModifyActivePreset(mState))
    {
        OpenPresetDetails(false);
    }
}

void Shell::OpenPresetDetails(bool asNew)
{
    // The web UI's save dialog (save-preset-modal.html). Details: the folder it is filed in, its
    // name, category, description and tags, and the input peak it was designed at. Advanced: the
    // preset's JSON with Validate and Clean Up (presets/inspectModal.ts). Saving as new names the
    // copy; otherwise it saves over the preset playing.
    if (!mState.activePreset)
    {
        return;
    }

    using guitarfx::uiclient::PresetFolder;
    const auto& preset = *mState.activePreset;
    const auto* summary = mState.FindPresetSummary(mState.activePresetId);
    const bool userPreset = summary != nullptr && summary->source == "user";

    struct DetailsState
    {
        std::vector<std::string> tags;
        std::string folderId; // "" for none
        std::optional<double> stagedPeak;
        std::optional<nlohmann::json> cleaned;
        nlohmann::json json;
        guitarfx::uiclient::PeakHold hold;
        int tab = 0;
    };

    auto st = std::make_shared<DetailsState>();
    st->tags = preset.tags;
    st->json = guitarfx::uiclient::InspectablePresetJson(preset);

    // Saved as new it goes in the folder being browsed; otherwise it stays in its own.
    const std::function<const PresetFolder*(const std::vector<PresetFolder>&)> holding =
        [&](const std::vector<PresetFolder>& folders) -> const PresetFolder* {
        for (const auto& f : folders)
        {
            if (std::find(f.presetIds.begin(), f.presetIds.end(), mState.activePresetId) != f.presetIds.end())
            {
                return &f;
            }

            if (const auto* inner = holding(f.children))
            {
                return inner;
            }
        }

        return nullptr;
    };
    const auto* current = asNew ? guitarfx::uiclient::FindPresetFolder(mState.presetFolders, mQuery.folderId)
                                : holding(mState.presetFolders);

    if (current != nullptr)
    {
        st->folderId = current->id;
    }

    // A text field as tall as the Save Folder drop-down (32) with the same 10 px inset: Elements'
    // own box is its text's height plus a third of it (about 22 px) whatever height it is given.
    const auto field = [](const char* placeholder, const std::string& text) {
        const auto font = Font(Weight::Regular).size(metrics::kSmallText);
        auto box = el::share(el::basic_input_box{placeholder, font});
        box->set_text(text);
        return std::make_pair(el::share(el::input_box(el::hold(box), el::rect{10.0f, 8.0f, 10.0f, 8.0f})), box);
    };
    auto [nameElement, nameBox] = field("My Custom Preset", asNew && userPreset ? preset.name + " copy" : preset.name);
    auto [categoryElement, categoryBox] = field("User", preset.category);
    auto [descriptionElement, descriptionBox] = field("Optional description...", preset.description);

    // A form label (.form-group label): small, bold, in capitals.
    const auto label = [this](const std::string& text) {
        auto t = std::make_shared<Text>(mTheme, Upper(text), 11.0f, Weight::SemiBold);
        t->Colour(mTheme.ModalText());
        return FixedHeight(22, Middle(t));
    };

    // ── Save Folder ──
    auto folder = std::make_shared<PickerField>(mTheme, mIcons);
    folder->AccessName("Save Folder");
    folder->Set(current != nullptr ? current->name : std::string("No folder"));
    folder->onClick = [this, st, weak = std::weak_ptr<PickerField>(folder)] {
        const auto picker = weak.lock();

        if (!picker)
        {
            return;
        }

        const auto choose = [this, st, weak](const std::string& id, const std::string& name) {
            st->folderId = id;

            if (const auto p = weak.lock())
            {
                p->Set(name);
                mSession.Repaint(*p);
            }
        };
        std::vector<MenuItem> items;
        items.push_back({"No folder", st->folderId.empty(), false, [choose] { choose({}, "No folder"); }});
        std::function<void(const std::vector<PresetFolder>&, int)> add = [&](const std::vector<PresetFolder>& folders,
                                                                             int depth) {
            for (const auto& f : folders)
            {
                items.push_back({std::string(static_cast<std::size_t>(depth) * 2, ' ') + f.name, f.id == st->folderId,
                                 false, [choose, id = f.id, name = f.name] { choose(id, name); }});
                add(f.children, depth + 1);
            }
        };
        add(mState.presetFolders, 0);
        OpenDropdown(picker->Bounds(), std::move(items), std::max(260.0f, picker->Bounds().width()));
    };

    // ── Tags: the standard ones, and any others the preset already has ──
    auto tagSlot = MakeSlot();
    auto fillTags = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakFill = fillTags;
    *fillTags = [this, st, tagSlot, weakFill] {
        auto all = guitarfx::uiclient::tones::StandardTags();

        for (const auto& t : st->tags)
        {
            if (std::find(all.begin(), all.end(), t) == all.end())
            {
                all.push_back(t);
            }
        }

        std::vector<Ptr> rows, row;
        float used = 0.0f;

        for (const auto& tag : all)
        {
            const bool on = std::find(st->tags.begin(), st->tags.end(), tag) != st->tags.end();
            auto chip = std::make_shared<Button>(
                mTheme, tag,
                [this, st, tag, on, weakFill] {
                    if (on)
                    {
                        st->tags.erase(std::remove(st->tags.begin(), st->tags.end(), tag), st->tags.end());
                    }
                    else
                    {
                        st->tags.push_back(tag);
                    }

                    mSession.View().post([alive = std::weak_ptr<bool>(mAlive), weakFill] {
                        if (const auto fill = weakFill.lock(); fill && alive.lock())
                        {
                            (*fill)();
                        }
                    });
                },
                Button::Style::Flat);
            chip->Selected(on).TextSize(metrics::kSmallText);
            const float w = MeasureTextWidth(tag, Font(Weight::Medium).size(metrics::kSmallText)) + 26.0f;

            if (used + w > 460.0f && !row.empty())
            {
                row.push_back(Stretch());
                rows.push_back(FixedHeight(30, Row(row)));
                row.clear();
                used = 0.0f;
            }

            row.push_back(chip);
            row.push_back(HGap(4));
            used += w + 4.0f;
        }

        if (!row.empty())
        {
            row.push_back(Stretch());
            rows.push_back(FixedHeight(30, Row(row)));
        }

        Fill(*tagSlot, Col(rows));
        mSession.View().layout();
        mSession.View().refresh();
    };
    (*fillTags)();
    mDetailsTags = fillTags; // kept while the dialog is open: the chips hold it only weakly

    // ── Input Peak Level: the raw input's peak over ten seconds, and the one the preset keeps ──
    using guitarfx::uiclient::FormatPeakDb;
    const auto cell = [this](const std::string& text, bool value) {
        auto t = std::make_shared<Text>(mTheme, text, metrics::kSmallText, value ? Weight::SemiBold : Weight::Medium);
        t->Colour(value ? mTheme.ModalText() : mTheme.ModalTextSecondary());

        if (value)
        {
            t->Alignment(HAlign::Right);
        }

        return t;
    };
    auto rawPeak = cell(FormatPeakDb(std::nullopt), true);
    auto designedPeak = cell(FormatPeakDb(preset.designedPeakInputDbfs), true);
    auto apply = std::make_shared<Button>(mTheme, "Apply Current", std::function<void()>{}, Button::Style::Plain);
    apply->TextSize(11.0f).MinWidth(104);
    apply->OnClick([this, st, designedPeak] {
        const auto peak = st->hold.Peak();

        if (!peak)
        {
            ShowToast("No peak data available yet", "", false);
            return;
        }

        st->stagedPeak = *peak;
        designedPeak->Set(FormatPeakDb(peak));
        mSession.Repaint(*designedPeak);
    });
    auto peakBox =
        Over(PadXY(14, 8,
                   Col({FixedHeight(26, Row({cell("Raw Input Peak (10s)", false), rawPeak})),
                        FixedHeight(32, Row({cell("Designed Peak", false), designedPeak, HGap(10), Middle(apply)}))})),
             el::share(Surface(mTheme.ModalRaised(), mTheme.ModalRule(), 8.0f)));

    // On the drop-down's fill, so the two read as one kind of field.
    const auto box = [this](Ptr element) { return FixedHeight(32, Rounded(mTheme.Tertiary(), 3.0f, element)); };
    auto details =
        Col({label("Save Folder"), FixedHeight(34, folder), VGap(12), label("Preset Name"), box(nameElement), VGap(12),
             label("Category"), box(categoryElement), VGap(12), label("Description"), box(descriptionElement), VGap(12),
             label("Tags"), tagSlot, VGap(12), label("Input Peak Level"), FixedHeight(74, peakBox)});

    // ── Advanced: the JSON, read-only, with Validate and Clean Up ──
    auto code = std::make_shared<CodeBlock>(mTheme);
    code->Set(st->json.dump(2));
    auto report = std::make_shared<CodeBlock>(mTheme);
    auto reportSlot = MakeSlot();
    const auto showReport = [this, report, reportSlot](const std::vector<std::string>& lines) {
        std::string text;

        for (const auto& line : lines)
        {
            text += (text.empty() ? "" : "\n") + line;
        }

        report->Set(text);
        Fill(*reportSlot, Col({report, VGap(10)}));
        mSession.View().layout();
        mSession.View().refresh();
    };
    auto validate = std::make_shared<Button>(
        mTheme, "Validate",
        [st, showReport] {
            auto issues = guitarfx::uiclient::ValidatePresetJson(st->cleaned ? *st->cleaned : st->json);
            showReport(issues.empty() ? std::vector<std::string>{"No issues found."} : issues);
        },
        Button::Style::Plain);
    auto cleanUp = std::make_shared<Button>(
        mTheme, "Clean Up",
        [this, st, code, showReport] {
            auto result = guitarfx::uiclient::CleanupPresetJson(st->json);
            code->Set(result.cleaned.dump(2));
            st->cleaned = std::move(result.cleaned);
            showReport(guitarfx::uiclient::CleanupReport(result));
            mSession.View().refresh();
        },
        Button::Style::Plain);
    validate->MinWidth(88);
    cleanUp->MinWidth(88);
    auto advanced =
        Col({label("Preset JSON (read-only)"), FixedHeight(34, Row({validate, HGap(8), cleanUp, Stretch()})), VGap(10),
             reportSlot, FixedHeight(380, VScroll(code))});

    // ── The tabs (.preset-modal-tabs) ──
    auto panel = MakeSlot();
    auto detailsTab = std::make_shared<Button>(mTheme, "Details", std::function<void()>{}, Button::Style::Flat);
    auto advancedTab = std::make_shared<Button>(mTheme, "Advanced", std::function<void()>{}, Button::Style::Flat);
    const auto show = [this, st, panel, details, advanced, d = detailsTab.get(), a = advancedTab.get()](int tab) {
        st->tab = tab;
        d->Selected(tab == 0);
        a->Selected(tab == 1);
        Fill(*panel, tab == 0 ? details : advanced);
        mSession.View().layout();
        mSession.View().refresh();
    };
    detailsTab->OnClick([show] { show(0); });
    advancedTab->OnClick([show] { show(1); });
    detailsTab->Selected(true);
    Fill(*panel, details);

    auto save = [this, st, asNew, nameBox = nameBox, categoryBox = categoryBox, descriptionBox = descriptionBox] {
        const auto trim = [](std::string text) {
            const auto first = text.find_first_not_of(" \t");
            const auto last = text.find_last_not_of(" \t");
            return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        };
        guitarfx::uiclient::UiCommands::PresetDetails details;
        details.name = trim(nameBox->get_utf8());

        if (details.name.empty())
        {
            return;
        }

        details.category = trim(categoryBox->get_utf8());
        details.description = trim(descriptionBox->get_utf8());
        details.tags = st->tags;
        details.folderId = st->folderId;
        details.designedPeakInputDbfs = st->stagedPeak;
        details.preset = st->cleaned;
        CloseOverlay();
        mSession.Commands().SavePresetWithDetails(details, asNew);
    };

    // Enter in any of the fields saves, as a web form's does: with the on-screen keyboard up the
    // footer's Save is under it, and its Enter key is the way to commit.
    for (const auto& box : {nameBox, categoryBox, descriptionBox})
    {
        box->on_enter = [save](std::string_view) {
            save();
            return true;
        };
    }

    ShowDialog({.title = asNew ? "Save Preset As" : "Overwrite Preset",
                .body = Col({FixedHeight(32, Row({detailsTab, HGap(6), advancedTab, Stretch()})), VGap(12), panel}),
                .actions = {DialogButton(
                                "Cancel", [this] { CloseOverlay(); }, false),
                            DialogButton(asNew ? "Save New Preset" : "Overwrite Preset", save, true)},
                .width = 520.0f,
                .height = kDialogHeaderHeight + kDialogFooterHeight + 36.0f + 44.0f + 504.0f});

    // The raw input's peak follows the meters while the dialog is open.
    mInstrumentSubs.push_back(mSession.Client().Subscribe(guitarfx::uiclient::Topic::Telemetry, [this, st, rawPeak] {
        st->hold.Push(mState.telemetry.rawInput.peakDb, UiSession::Now());
        const auto text = FormatPeakDb(st->hold.Peak());

        if (text != rawPeak->Value())
        {
            rawPeak->Set(text);
            mSession.Repaint(*rawPeak);
        }
    }));
}
} // namespace nanoq::ui
