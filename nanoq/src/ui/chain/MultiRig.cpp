// MultiRig.cpp - Multi-Rig (the web UI's signalPath/mixer.ts and multiPresetMixer.ts): with two or
// more presets in the mix, a tab per preset over the chain (the editor shows the one in focus;
// M and S mark a muted or soloed one; x takes it out of the mix) and a Mix tab whose page is
// the inline mixer: a strip per preset (Mix, Pan, Mute, Solo) and the master (the Multi-Rig's
// own Out level, Save and Delete). Saved Multi-Rigs are listed in the preset chooser.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "uiclient/FeatureFlags.h"
#include "uiclient/ParamFormat.h"
#include "uiclient/PresetBrowse.h"
#include "uiclient/ToneSharing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
namespace
{
constexpr double kMixGainMinDb = -24.0; // multiPresetMixerSupport.ts
constexpr double kMixGainMaxDb = 12.0;

/// A saved Multi-Rig in the chooser (.composite-preset-chip): its name, "N presets" and a x at
/// the right; its presets' names as chips; its description; its tags as badges. The one loaded is
/// outlined in the accent. A tap (or Enter) loads it; the x (or Delete) asks to delete it.
class MultiRigCard final : public Pressable
{
  public:
    MultiRigCard(const Theme& theme, const guitarfx::uiclient::CompositePreset& rig, std::vector<std::string> slots,
                 bool active)
        : mTheme(theme), mName(rig.name), mDescription(rig.description), mTags(rig.tags), mSlots(std::move(slots)),
          mCount(rig.presetIds.size()), mActive(active)
    {
        AccessName(mName);
        Tooltip("Load Multi-Rig \"" + mName + "\"");
    }

    std::function<void()> onDelete;

    [[nodiscard]] float Height() const
    {
        return 8.0f + 30.0f + (mSlots.empty() ? 0.0f : 24.0f) + (mDescription.empty() ? 0.0f : 20.0f) +
               (mTags.empty() ? 0.0f : 24.0f) + 8.0f;
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::ListItem;
        info.value = CountText();
        info.selected = mActive;
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        const float h = Height();
        return {{240.0f, h}, {el::full_extent, h}};
    }

    bool key(const el::context& ctx, el::key_info k) override
    {
        if (onDelete && k.action == el::key_action::press && k.key == el::key_code::_delete)
        {
            onDelete();
            return true;
        }

        return Pressable::key(ctx, k);
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state == el::mouse_button::left &&
            CrossRect(ctx.bounds.inset(0.0f, 4.0f)).inset(-4.0f, -4.0f).includes(btn.pos))
        {
            if (!btn.down && onDelete)
            {
                onDelete();
            }

            return true;
        }

        return Pressable::click(ctx, btn);
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds.inset(0.0f, 4.0f);
        cnv.fill_style(mActive ? mTheme.AccentTint() : (mHover ? mTheme.Elevated() : mTheme.Card()));
        cnv.fill_round_rect(b, 8.0f);
        cnv.stroke_style(mActive ? mTheme.Accent().opacity(0.7f) : (HasFocus() ? mTheme.Accent() : mTheme.Border()));
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 8.0f);

        // The header: the name, the count, the x.
        const auto cross = CrossRect(b);
        const auto countFont = Font(Weight::Regular).size(11.0f);
        const auto count = CountText();
        const float countWidth = MeasureTextWidth(count, countFont);
        const float countRight = cross.left - 8.0f;
        const auto nameFont = Font(Weight::SemiBold).size(13.5f);
        const float nameRight = countRight - countWidth - 10.0f;
        const el::rect header{b.left + 12.0f, b.top + 8.0f, b.right - 12.0f, b.top + 30.0f};
        FillText(cnv, Ellipsize(cnv, mName, nameFont, nameRight - header.left), nameFont, mTheme.Text(),
                 el::rect{header.left, header.top, nameRight, header.bottom}, HAlign::Left);
        FillText(cnv, count, countFont, mTheme.TextMuted(),
                 el::rect{countRight - countWidth, header.top, countRight, header.bottom}, HAlign::Left);
        FillText(cnv, "\xC3\x97", Font(Weight::Regular).size(15.0f), mCrossHover ? mTheme.Error() : mTheme.TextMuted(),
                 cross, HAlign::Centre);

        float y = header.bottom + 4.0f;

        // Its presets as chips, as many as fit on the line.
        if (!mSlots.empty())
        {
            const auto chipFont = Font(Weight::Medium).size(10.5f);
            float x = b.left + 12.0f;

            for (const auto& name : mSlots)
            {
                const float w = std::min(MeasureTextWidth(name, chipFont) + 14.0f, b.right - 12.0f - x);

                if (w < 40.0f)
                {
                    break;
                }

                const el::rect chip{x, y, x + w, y + 18.0f};
                cnv.fill_style(mTheme.Text().opacity(0.08f));
                cnv.fill_round_rect(chip, 9.0f);
                FillText(cnv, Ellipsize(cnv, name, chipFont, w - 14.0f), chipFont, mTheme.TextSecondary(), chip,
                         HAlign::Centre);
                x += w + 4.0f;
            }

            y += 24.0f;
        }

        if (!mDescription.empty())
        {
            const auto descFont = Font(Weight::Regular).size(11.5f);
            FillText(cnv, Ellipsize(cnv, mDescription, descFont, b.width() - 24.0f), descFont, mTheme.TextSecondary(),
                     el::rect{b.left + 12.0f, y, b.right - 12.0f, y + 16.0f}, HAlign::Left);
            y += 20.0f;
        }

        // Its tags, as the preset cards' category badge.
        if (!mTags.empty())
        {
            const auto tagFont = Font(Weight::SemiBold).size(9.5f);
            float x = b.left + 12.0f;

            for (const auto& tag : mTags)
            {
                const float w = MeasureTextWidth(tag, tagFont) + 14.0f;

                if (x + w > b.right - 12.0f)
                {
                    break;
                }

                const el::rect badge{x, y, x + w, y + 16.0f};
                cnv.fill_style(mTheme.Text().opacity(0.1f));
                cnv.fill_round_rect(badge, 8.0f);
                FillText(cnv, tag, tagFont, mTheme.TextSecondary(), badge, HAlign::Centre);
                x += w + 4.0f;
            }
        }
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool over = status != el::cursor_tracking::leaving &&
                          CrossRect(ctx.bounds.inset(0.0f, 4.0f)).inset(-4.0f, -4.0f).includes(p);

        if (over != mCrossHover)
        {
            mCrossHover = over;
            ctx.view.refresh(ctx.bounds);
        }

        return Pressable::cursor(ctx, p, status);
    }

  private:
    [[nodiscard]] std::string CountText() const
    {
        return std::to_string(mCount) + (mCount == 1 ? " preset" : " presets");
    }

    [[nodiscard]] static el::rect CrossRect(const el::rect& b)
    {
        return el::rect{b.right - 32.0f, b.top + 8.0f, b.right - 10.0f, b.top + 30.0f};
    }

    const Theme& mTheme;
    std::string mName, mDescription;
    std::vector<std::string> mTags, mSlots;
    std::size_t mCount;
    bool mActive;
    bool mCrossHover = false;
};

std::string Percent(double value)
{
    return std::to_string(static_cast<int>(std::lround(value * 100.0))) + "%";
}

std::string Pan(double value)
{
    if (std::abs(value) < 0.01)
    {
        return "C";
    }

    return (value < 0.0 ? "L" : "R") + std::to_string(static_cast<int>(std::lround(std::abs(value) * 100.0)));
}

std::string GainDb(double db)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%+.1f dB", db);
    return text;
}
} // namespace

bool Shell::MultiRigShown() const
{
    return mState.mixer.activePresetIds.size() > 1;
}

std::string Shell::MixerSignature() const
{
    if (!MultiRigShown())
    {
        return {};
    }

    std::string s = std::string(mMixTab ? "mix|" : "rig|") + mState.activePresetId;

    for (const auto& id : mState.mixer.activePresetIds)
    {
        const auto it = mState.mixer.slots.find(id);
        s += "|" + id + ":" +
             (it != mState.mixer.slots.end()
                  ? it->second.name + (it->second.mute ? "M" : "") + (it->second.solo ? "S" : "")
                  : "");
    }

    if (const auto* loaded = guitarfx::uiclient::LoadedComposite(mState.mixer))
    {
        s += "|=" + loaded->id + loaded->name;
    }

    return s;
}

Ptr Shell::BuildMixerTabs()
{
    if (!MultiRigShown())
    {
        mMixTab = false;
        return nullptr;
    }

    auto tray = std::make_shared<el::htile_composite>();
    tray->push_back(HGap(2));

    for (const auto& id : mState.mixer.activePresetIds)
    {
        const auto it = mState.mixer.slots.find(id);
        std::string label = it != mState.mixer.slots.end() && !it->second.name.empty() ? it->second.name : id;

        if (it != mState.mixer.slots.end())
        {
            if (it->second.mute)
            {
                label += "  M";
            }

            if (it->second.solo)
            {
                label += "  S";
            }
        }

        auto tab = std::make_shared<SceneChip>(mTheme, label, !mMixTab && id == mState.activePresetId);
        tab->onClick = [this, id] {
            mMixTab = false;

            if (id != mState.activePresetId)
            {
                mSession.Commands().FocusMixerPreset(id);
            }

            mChainSignature.clear();
            mSceneSignature.clear();
            Mark(kScenes | kChain | kPage);
        };
        tab->onRemove = [this, id] { mSession.Commands().RemoveActivePreset(id); };
        tray->push_back(tab);
        tray->push_back(HGap(4));
    }

    auto mix = std::make_shared<SceneChip>(mTheme, "Mix", mMixTab);
    mix->onClick = [this] {
        mMixTab = true;
        mChainSignature.clear();
        mSceneSignature.clear();
        Mark(kScenes | kChain);
    };
    tray->push_back(mix);
    tray->push_back(HGap(2));
    return Row(
        {HGap(26),
         Rounded(mTheme.DarkOr(el::rgba(24, 24, 30, 184), mTheme.Text().opacity(0.06f)), 11.0f, PadXY(0, 2, tray)),
         Stretch()});
}

Ptr Shell::BuildInlineMixer()
{
    auto& commands = mSession.Commands();

    // A knob at `position` (0..1), which follows the mixer's state as the engine mirrors it.
    const auto knob = [this](const char* name, std::function<double()> position, std::function<void(double)> onChange,
                             std::function<std::string(double)> format, double defaultPosition) {
        auto k = std::make_shared<ParamKnob>(mTheme, name);
        k->onChange = [onChange, format, raw = k.get()](double p) {
            raw->SetValueText(format(p));
            onChange(p);
        };
        k->onReset = [onChange, defaultPosition, raw = k.get(), format] {
            raw->Update(defaultPosition, format(defaultPosition));
            onChange(defaultPosition);
        };
        mLive.Bind(k, std::move(position), [format](ParamKnob& raw, double p) { raw.Update(p, format(p)); });
        return k;
    };
    // A strip's preset in the mixer's state now, if it is still there.
    const auto slotOf = [this](const std::string& id) -> const guitarfx::uiclient::MixerSlot* {
        const auto it = mState.mixer.slots.find(id);
        return it != mState.mixer.slots.end() ? &it->second : nullptr;
    };

    std::vector<Ptr> strips;

    for (const auto& id : mState.mixer.activePresetIds)
    {
        const auto* slot = slotOf(id);
        auto name = std::make_shared<Text>(mTheme, slot != nullptr && !slot->name.empty() ? slot->name : id, 11.5f,
                                           Weight::SemiBold);
        name->Colour(mTheme.Ink()).Alignment(Text::Align::Centre);
        auto mixKnob = knob(
            "Mix",
            [slotOf, id] {
                const auto* s = slotOf(id);
                return s != nullptr ? s->mix : 1.0;
            },
            [&commands, id](double p) { commands.SetPresetMix(id, p); }, Percent, 1.0);
        auto panKnob = knob(
            "Pan",
            [slotOf, id] {
                const auto* s = slotOf(id);
                return s != nullptr ? (s->pan + 1.0) / 2.0 : 0.5;
            },
            [&commands, id](double p) { commands.SetPresetPan(id, p * 2.0 - 1.0); },
            [](double p) { return Pan(p * 2.0 - 1.0); }, 0.5);
        // Mute and Solo are patched in place: the chain region does not rebuild under the pointer.
        auto mute = std::make_shared<ParamToggle>(mTheme, "Mute");
        mute->onClick = [this, slotOf, id] {
            const auto* s = slotOf(id);
            mSession.Commands().SetPresetMute(id, s == nullptr || !s->mute);
        };
        mLive.Bind(
            mute,
            [slotOf, id] {
                const auto* s = slotOf(id);
                return s != nullptr && s->mute;
            },
            &ParamToggle::Set);
        auto solo = std::make_shared<ParamToggle>(mTheme, "Solo");
        solo->onClick = [this, slotOf, id] {
            const auto* s = slotOf(id);
            mSession.Commands().SetPresetSolo(id, s == nullptr || !s->solo);
        };
        mLive.Bind(
            solo,
            [slotOf, id] {
                const auto* s = slotOf(id);
                return s != nullptr && s->solo;
            },
            &ParamToggle::Set);
        auto body = Col({FixedHeight(20, name), VGap(4), FixedHeight(92, Row({Stretch(), mixKnob, panKnob, Stretch()})),
                         VGap(6), FixedHeight(63, Row({Stretch(), mute, solo, Stretch()}))});
        strips.push_back(FixedWidth(140, Over(PadXY(4, 8, body), std::make_shared<InsetBox>(mTheme))));
        strips.push_back(HGap(10));
    }

    // The master: the Multi-Rig's own level, and saving it as a Multi-Rig preset (or updating
    // the one it is, while its presets are the same).
    const auto toGain = [](double p) { return kMixGainMinDb + p * (kMixGainMaxDb - kMixGainMinDb); };
    auto out = knob(
        "Out", [this] { return (mState.mixer.mixGainDb - kMixGainMinDb) / (kMixGainMaxDb - kMixGainMinDb); },
        [&commands, toGain](double p) { commands.SetMixGain(std::round(toGain(p) * 10.0) / 10.0); },
        [toGain](double p) { return GainDb(std::round(toGain(p) * 10.0) / 10.0); },
        (0.0 - kMixGainMinDb) / (kMixGainMaxDb - kMixGainMinDb));
    const auto* loaded = guitarfx::uiclient::LoadedComposite(mState.mixer);
    auto save = std::make_shared<IconButton>(mTheme, mIcons, "ui-save", IconButton::Look::Boxed);
    save->Size(30, 30).IconSize(14).Ink(loaded != nullptr ? mTheme.Accent() : mTheme.TextSecondary());
    save->onClick = [this] { SaveMultiRig(); };
    auto remove = std::make_shared<IconButton>(mTheme, mIcons, "ui-trash", IconButton::Look::Boxed);
    remove->Size(30, 30).IconSize(14).Disabled(loaded == nullptr);

    if (loaded != nullptr)
    {
        remove->onClick = [this, id = loaded->id, name = loaded->name] {
            OpenConfirm("Delete Multi-Rig", "Delete the Multi-Rig " + name + "?", "Delete",
                        [this, id] { mSession.Commands().RemoveCompositePreset(id); });
        };
    }

    auto masterName = std::make_shared<Text>(mTheme, loaded != nullptr ? loaded->name : std::string("Master"), 11.5f,
                                             Weight::SemiBold);
    masterName->Colour(mTheme.Ink()).Alignment(Text::Align::Centre);
    auto master =
        Col({FixedHeight(20, masterName), VGap(4), FixedHeight(92, Row({Stretch(), out, Stretch()})), VGap(6),
             FixedHeight(36, Row({Stretch(), save, HGap(8), remove, Stretch()})), std::make_shared<el::element>()});
    strips.push_back(FixedWidth(120, Over(PadXY(4, 8, master), std::make_shared<InsetBox>(mTheme))));

    auto row = std::make_shared<el::htile_composite>();

    for (auto& s : strips)
    {
        row->push_back(s);
    }

    return Col({VGap(16), FixedHeight(210, Row({HGap(24), HScroll(row), Stretch()})), std::make_shared<el::element>()});
}

void Shell::SaveMultiRig()
{
    // The web UI's Save Multi-Rig Mix (save-multi-rig-modal): Title, a three-row Description and
    // the tags, the standard ones and any the Multi-Rigs use. A Multi-Rig already loaded is
    // edited in place ("Edit Multi-Rig Mix", Update Mix). It takes two presets or more.
    if (mState.mixer.activePresetIds.size() < 2)
    {
        ShowToast("Add at least 2 presets to the mixer before saving a Multi-Rig.", "", false);
        return;
    }

    const auto* loaded = guitarfx::uiclient::LoadedComposite(mState.mixer);
    const std::string id = loaded != nullptr ? loaded->id : std::string();

    struct RigForm
    {
        std::vector<std::string> tags;
    };

    auto form = std::make_shared<RigForm>();

    if (loaded != nullptr)
    {
        form->tags = loaded->tags;
    }

    const auto font = Font(Weight::Regular).size(metrics::kSmallText);
    auto nameBox = el::share(el::basic_input_box{"My Live Mix", font});
    nameBox->set_text(loaded != nullptr ? loaded->name : std::string());
    auto nameElement = el::share(el::input_box(el::hold(nameBox), el::rect{10.0f, 8.0f, 10.0f, 8.0f}));
    auto descriptionBox = el::share(el::basic_text_box{loaded != nullptr ? loaded->description : std::string(), font});
    auto descriptionElement =
        VScroll(el::share(el::margin(el::rect{10.0f, 8.0f, 10.0f, 8.0f}, el::hold(descriptionBox))));

    const auto label = [this](const std::string& text) {
        auto t = std::make_shared<Text>(mTheme, Upper(text), 11.0f, Weight::SemiBold);
        t->Colour(mTheme.ModalText());
        return FixedHeight(22, Middle(t));
    };

    // The tag chips, refilled in place as they are switched.
    auto tagSlot = MakeSlot();
    auto fillTags = std::make_shared<std::function<void()>>();
    std::weak_ptr<std::function<void()>> weakFill = fillTags;
    *fillTags = [this, form, tagSlot, weakFill] {
        auto all = guitarfx::uiclient::tones::StandardTags();

        for (const auto& t : guitarfx::uiclient::CompositePresetTags(mState.mixer.compositePresets))
        {
            if (std::find(all.begin(), all.end(), t) == all.end())
            {
                all.push_back(t);
            }
        }

        for (const auto& t : form->tags)
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
            const bool on = std::find(form->tags.begin(), form->tags.end(), tag) != form->tags.end();
            auto chip = std::make_shared<Button>(
                mTheme, tag,
                [this, form, tag, on, weakFill] {
                    if (on)
                    {
                        std::erase(form->tags, tag);
                    }
                    else
                    {
                        form->tags.push_back(tag);
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

    auto save = [this, id, form, nameBox, descriptionBox] {
        const auto trim = [](std::string text) {
            const auto first = text.find_first_not_of(" \t\r\n");
            const auto last = text.find_last_not_of(" \t\r\n");
            return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        };
        const auto name = trim(nameBox->get_utf8());

        if (name.empty())
        {
            ShowToast("Error", "A Multi-Rig needs a title", true);
            return;
        }

        CloseOverlay();
        mSession.Commands().SaveCompositePreset(name, trim(descriptionBox->get_utf8()), form->tags, id);
    };
    nameBox->on_enter = [save](std::string_view) {
        save();
        return true;
    };

    const auto box = [this](Ptr element, float h) { return FixedHeight(h, Rounded(mTheme.Tertiary(), 3.0f, element)); };
    ShowDialog(
        {.title = loaded != nullptr ? "Edit Multi-Rig Mix" : "Save Multi-Rig Mix",
         .body = Col({label("Title"), box(nameElement, 32), VGap(12), label("Description"), box(descriptionElement, 64),
                      VGap(12), label("Tags"), tagSlot, std::make_shared<el::element>()}),
         .actions = {DialogButton(
                         "Cancel", [this] { CloseOverlay(); }, false),
                     DialogButton(loaded != nullptr ? "Update Mix" : "Save Mix", save, true)},
         .width = 520.0f,
         .height = kDialogHeaderHeight + kDialogFooterHeight + 300.0f});
}

Ptr Shell::BuildMultiRigList()
{
    // renderCompositePresetList: the saved Multi-Rigs as cards, filtered by the library search.
    std::vector<Ptr> rows;

    if (!mState.mixer.compositePresetsKnown)
    {
        mSession.Commands().GetCompositePresetList();
    }

    const auto* loaded = guitarfx::uiclient::LoadedComposite(mState.mixer);

    const auto none = [this, &rows](const std::string& text) {
        const float width = std::min(760.0f, ViewSize().x - 24.0f) - 28.0f - 24.0f;
        auto t = std::make_shared<Paragraph>(mTheme, text, width, 13.0f);
        t->Colour(mTheme.TextMuted());
        rows.push_back(PadXY(12, 10, t));
    };

    if (mState.mixer.compositePresets.empty())
    {
        none("No Multi-Rig presets saved yet. Switch to the Presets tab, click + Mixer on two or more presets, then "
             "click Save in the mixer's Mix tab.");
        return VScroll(Col(rows));
    }

    const auto visible = guitarfx::uiclient::FilterCompositePresets(mState, mQuery.text);

    for (const auto* rig : visible)
    {
        const std::string id = rig->id, name = rig->name;
        auto card = std::make_shared<MultiRigCard>(mTheme, *rig, guitarfx::uiclient::CompositeSlotNames(mState, *rig),
                                                   loaded != nullptr && loaded->id == id);
        card->onClick = [this, id] {
            // Loading replaces every preset in the mix, so unsaved edits to the one shown would go.
            if (mState.activePresetDirty)
            {
                OpenConfirm("Unsaved changes", "Discard unsaved changes?", "Discard",
                            [this, id] { mSession.Commands().LoadCompositePreset(id); });
            }
            else
            {
                mSession.Commands().LoadCompositePreset(id);
            }
        };
        card->onDelete = [this, id, name] {
            mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), id, name] {
                if (alive.lock())
                {
                    OpenConfirm("Delete Multi-Rig", "Delete Multi-Rig \"" + name + "\"? This cannot be undone.",
                                "Delete", [this, id] { mSession.Commands().RemoveCompositePreset(id); });
                }
            });
        };
        card->onMenu = [this, id, name, card = std::weak_ptr<MultiRigCard>(card)](el::point) {
            PostMenu(name, {{"Load", false, false,
                             [card] {
                                 if (const auto c = card.lock(); c && c->onClick)
                                 {
                                     c->onClick();
                                 }
                             }},
                            {"Delete...", false, false, [card] {
                                 if (const auto c = card.lock(); c && c->onDelete)
                                 {
                                     c->onDelete();
                                 }
                             }}});
        };
        rows.push_back(FixedHeight(card->Height(), card));
    }

    if (visible.empty())
    {
        auto query = mQuery.text;
        const auto first = query.find_first_not_of(" \t");
        query =
            first == std::string::npos ? std::string() : query.substr(first, query.find_last_not_of(" \t") - first + 1);
        none("No Multi-Rig presets match \"" + query + "\".");
    }

    return VScroll(Col(rows));
}
} // namespace nanoq::ui
