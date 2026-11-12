// The performance pads, as the web UI's (performancePads.ts, performance-pads.css): the active
// setlist as large pads, one per slot, under a bank strip that steps to the setlist before and
// after it and edits this one. A tap on a pad plays its preset; its save button puts the preset
// playing on it, asking first when that replaces another. Add bank makes the next setlist, and
// the PADS picker shows 4, 6 or 8 of them (performancePads.padCount, shared with the web UI). In
// the full layout the pads take the editor's place under the chain; in the compact one they are
// the Setlist page.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "uiclient/PresetBrowse.h"

#include <algorithm>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

namespace
{
constexpr float kPadRadius = 19.0f;

/// The green of the pad playing (.performance-setlist-pad.is-active) and its CURRENT.
el::color CurrentInk(const Theme& theme)
{
    return theme.Pick(el::rgba(143, 196, 143, 240), theme.Success());
}

/// One pad (.performance-setlist-pad): its number in a chip at the top, the preset's name large
/// at the foot with CURRENT, TAP TO LOAD or ASSIGN IN SETLIST under it, and the save button at
/// the right. The one playing is green; an empty one is drawn dashed.
class PerformancePad final : public Pressable
{
  public:
    enum class State
    {
        Current,
        Loadable,
        Empty
    };

    PerformancePad(const Theme& theme, Icons& icons, int number, std::string name, State state)
        : mTheme(theme), mIcons(icons), mNumber(number), mName(std::move(name)), mState(state)
    {
    }

    /// The save button: put the preset playing on this pad.
    std::function<void()> onAssign;

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::ListItem;
        info.name = NameOr("Pad " + std::to_string(mNumber));
        info.value = mState == State::Empty ? std::string("empty") : mName;
        info.selected = mState == State::Current;
        return info;
    }

    // S saves the preset playing to the pad, as its save button does.
    bool key(const el::context& ctx, el::key_info k) override
    {
        if (onAssign && k.action == el::key_action::press && k.key == el::key_code::s &&
            (k.modifiers & (el::mod_control | el::mod_alt | el::mod_super)) == 0)
        {
            onAssign();
            return true;
        }

        return Pressable::key(ctx, k);
    }

    // Even an empty pad takes focus: S assigns it.
    bool wants_focus() const override
    {
        return FocusWanted(mEnabled && (onClick != nullptr || onAssign != nullptr));
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{120.0f, 96.0f}, {el::full_extent, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const bool light = mTheme.IsLight();

        if (mState == State::Empty)
        {
            cnv.fill_style(mTheme.Card().opacity(mHover ? 0.8f : 0.62f));
            cnv.fill_round_rect(b, kPadRadius);
            cnv.stroke_style(mTheme.TextMuted().opacity(mHover ? 1.0f : 0.85f));
            cnv.line_width(2.5f);
            StrokeDashedRoundRect(cnv, b.inset(1.5f, 1.5f), kPadRadius - 1.5f, 8.0f, 5.0f);
        }
        else
        {
            {
                auto state = cnv.new_state();
                cnv.shadow_style(el::point{0.0f, 4.0f}, 16.0f, el::rgba(0, 0, 0, light ? 40 : 150));
                cnv.fill_style(mTheme.Card());
                cnv.fill_round_rect(b, kPadRadius);
            }

            artist::canvas::linear_gradient face{b.top_left(), b.bottom_left()};

            if (mState == State::Current)
            {
                face.add_color_stop(0.0f, mTheme.Pick(el::rgba(60, 88, 66, 255), mTheme.Success().opacity(0.3f)));
                face.add_color_stop(1.0f, mTheme.Pick(el::rgba(37, 50, 42, 255), mTheme.Success().opacity(0.16f)));
            }
            else
            {
                face.add_color_stop(0.0f, mTheme.Elevated().opacity(mHover ? 1.0f : 0.8f));
                face.add_color_stop(1.0f, mTheme.Card());
            }

            cnv.fill_style(face);
            cnv.fill_round_rect(b, kPadRadius);
            cnv.stroke_style(mState == State::Current ? CurrentInk(mTheme).opacity(0.55f)
                                                      : mTheme.Text().opacity(mHover ? 0.16f : 0.06f));
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(b.inset(0.5f, 0.5f), kPadRadius);
        }

        // The number, in its chip.
        const auto numberFont = Font(Weight::SemiBold).size(12.0f);
        const auto number = std::to_string(mNumber);
        const float chipWidth = std::max(34.0f, MeasureTextWidth(number, numberFont) + 16.0f);
        const el::rect chip{b.left + 16.0f, b.top + 16.0f, b.left + 16.0f + chipWidth, b.top + 43.0f};
        cnv.fill_style(mTheme.Text().opacity(0.08f));
        cnv.fill_round_rect(chip, chip.height() * 0.5f);
        FillText(cnv, number, numberFont, mTheme.TextSecondary(), chip, HAlign::Centre);

        // The name, large, and what a tap does under it.
        const auto save = SaveRect(b);
        const float textRight = save.left - 10.0f;
        // As large as the pad's height allows, smaller (down to 15) before a long name is cut short.
        const std::string name = mState == State::Empty ? std::string("Empty Path") : mName;
        float nameSize = std::clamp(b.height() * 0.16f, 17.0f, 24.5f);

        while (nameSize > 15.0f &&
               MeasureTextWidth(name, Font(Weight::Medium).size(nameSize)) > textRight - b.left - 16.0f)
        {
            nameSize -= 1.0f;
        }

        const auto nameFont = Font(Weight::Medium).size(nameSize);
        FillText(cnv, Ellipsize(cnv, name, nameFont, textRight - b.left - 16.0f), nameFont,
                 mState == State::Empty ? mTheme.TextSecondary() : mTheme.Text(),
                 el::rect{b.left + 16.0f, b.bottom - 34.0f - nameSize * 1.4f, textRight, b.bottom - 34.0f},
                 HAlign::Left);
        const char* sub = mState == State::Current    ? "CURRENT"
                          : mState == State::Loadable ? "TAP TO LOAD"
                                                      : "ASSIGN IN SETLIST";
        FillText(cnv, sub, Font(Weight::SemiBold).size(11.0f),
                 mState == State::Current ? CurrentInk(mTheme) : mTheme.TextSecondary(),
                 el::rect{b.left + 16.0f, b.bottom - 32.0f, textRight, b.bottom - 15.0f}, HAlign::Left);

        // The save button (.performance-pad-assign-btn).
        {
            auto state = cnv.new_state();
            cnv.shadow_style(el::point{0.0f, 2.0f}, 8.0f, el::rgba(0, 0, 0, light ? 30 : 128));
            cnv.fill_style(mTheme.Pick(el::rgba(69, 43, 36, 255), mTheme.AccentTint()));
            cnv.fill_round_rect(save, save.height() * 0.5f);
        }

        cnv.stroke_style(mTheme.Accent().opacity(mSaveHover ? 1.0f : 0.62f));
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(save.inset(0.5f, 0.5f), save.height() * 0.5f);
        mIcons.Draw(cnv, "ui-pad-save", save.inset(10.0f, 10.0f), mTheme.Text());
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state == el::mouse_button::left && SaveRect(ctx.bounds).inset(-3.0f, -3.0f).includes(btn.pos))
        {
            if (!btn.down && onAssign)
            {
                onAssign();
            }

            return true;
        }

        return Pressable::click(ctx, btn);
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool saveHover = status != el::cursor_tracking::leaving && SaveRect(ctx.bounds).includes(p);

        if (saveHover != mSaveHover)
        {
            mSaveHover = saveHover;
            ctx.view.refresh(ctx);
        }

        return Pressable::cursor(ctx, p, status);
    }

  private:
    static el::rect SaveRect(const el::rect& b)
    {
        return el::rect{b.right - 16.0f - 38.0f, b.bottom - 16.0f - 38.0f, b.right - 16.0f, b.bottom - 16.0f};
    }

    const Theme& mTheme;
    Icons& mIcons;
    int mNumber;
    std::string mName;
    State mState;
    bool mSaveHover = false;
};

/// The bank before or after (.performance-bank-nav-btn): a triangle pointing down for the one
/// before and up for the one after, over its name, or First / Last at the ends.
class BankStep final : public Pressable
{
  public:
    BankStep(const Theme& theme, bool up, std::string label) : mTheme(theme), mUp(up), mLabel(std::move(label))
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.name = NameOr(std::string(mUp ? "Next bank, " : "Previous bank, ") + mLabel);
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{72.0f, 60.0f}, {el::full_extent, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        {
            auto state = cnv.new_state();
            cnv.shadow_style(el::point{0.0f, 2.0f}, 8.0f, el::rgba(0, 0, 0, mTheme.IsLight() ? 30 : 128));
            cnv.fill_style(mHover && mEnabled ? mTheme.Elevated() : mTheme.Card());
            cnv.fill_round_rect(b, kPadRadius);
        }

        const float fade = mEnabled ? 1.0f : 0.45f;
        const float cx = b.left + b.width() * 0.5f;
        const float cy = b.top + b.height() * 0.5f - 12.0f;
        cnv.begin_path();

        if (mUp)
        {
            cnv.move_to(cx, cy - 7.0f);
            cnv.line_to(cx + 11.0f, cy + 7.0f);
            cnv.line_to(cx - 11.0f, cy + 7.0f);
        }
        else
        {
            cnv.move_to(cx - 11.0f, cy - 7.0f);
            cnv.line_to(cx + 11.0f, cy - 7.0f);
            cnv.line_to(cx, cy + 7.0f);
        }

        cnv.close_path();
        cnv.fill_style(mTheme.Accent().opacity(fade));
        cnv.fill();
        const auto font = Font(Weight::Regular).size(12.0f);
        FillText(cnv, Ellipsize(cnv, mLabel, font, b.width() - 24.0f), font, mTheme.TextSecondary().opacity(fade),
                 el::rect{b.left + 12.0f, cy + 16.0f, b.right - 12.0f, cy + 34.0f}, HAlign::Centre);
    }

  private:
    const Theme& mTheme;
    bool mUp;
    std::string mLabel;
};

/// The bank playing (.performance-bank-current): its number in the accent, its name, how many of
/// its paths the pads show, and Edit at the top right.
class BankCurrent final : public el::element, public Drawn
{
  public:
    BankCurrent(const Theme& theme, std::string bank, std::string name, std::string detail, bool editable)
        : mTheme(theme), mBank(std::move(bank)), mName(std::move(name)), mDetail(std::move(detail)), mEditable(editable)
    {
    }

    std::function<void()> onEdit;

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{160.0f, 60.0f}, {el::full_extent, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        {
            auto state = cnv.new_state();
            cnv.shadow_style(el::point{0.0f, 2.0f}, 8.0f, el::rgba(0, 0, 0, mTheme.IsLight() ? 30 : 128));
            cnv.fill_style(mTheme.Card());
            cnv.fill_round_rect(b, kPadRadius);
        }

        const float cy = b.top + b.height() * 0.5f;
        const el::rect line{b.left + 60.0f, 0.0f, b.right - 60.0f, 0.0f};
        FillText(cnv, ToUpper(mBank), Font(Weight::SemiBold).size(12.0f), mTheme.Accent(),
                 el::rect{line.left, cy - 31.0f, line.right, cy - 15.0f}, HAlign::Centre);
        const auto nameFont = Font(Weight::SemiBold).size(20.0f);
        FillText(cnv, Ellipsize(cnv, mName, nameFont, line.width()), nameFont, mTheme.Text(),
                 el::rect{line.left, cy - 15.0f, line.right, cy + 9.0f}, HAlign::Centre);
        FillText(cnv, mDetail, Font(Weight::Regular).size(12.0f), mTheme.TextSecondary(),
                 el::rect{line.left, cy + 12.0f, line.right, cy + 30.0f}, HAlign::Centre);

        if (mEditable)
        {
            const auto e = EditRect(b);
            cnv.fill_style(mHover ? mTheme.Elevated() : mTheme.Bar());
            cnv.fill_round_rect(e, e.height() * 0.5f);
            cnv.stroke_style(mHover ? mTheme.Accent() : mTheme.BorderStrong());
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(e.inset(0.5f, 0.5f), e.height() * 0.5f);
            FillText(cnv, "Edit", Font(Weight::SemiBold).size(11.0f), mTheme.TextSecondary(), e, HAlign::Centre);
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (!mEditable || btn.state != el::mouse_button::left || !EditRect(ctx.bounds).includes(btn.pos))
        {
            return false;
        }

        if (!btn.down && onEdit)
        {
            onEdit();
        }

        return true;
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool hover = mEditable && status != el::cursor_tracking::leaving && EditRect(ctx.bounds).includes(p);

        if (hover != mHover)
        {
            mHover = hover;
            ctx.view.refresh(ctx);
        }

        return false;
    }

    bool wants_control() const override
    {
        return mEditable;
    }

  private:
    static el::rect EditRect(const el::rect& b)
    {
        return el::rect{b.right - 52.0f, b.top + 10.0f, b.right - 10.0f, b.top + 36.0f};
    }

    static std::string ToUpper(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return text;
    }

    const Theme& mTheme;
    std::string mBank, mName, mDetail;
    bool mEditable;
    bool mHover = false;
};
} // namespace

Ptr Shell::BuildPadsView()
{
    const Setlist* setlist = ActiveSetlist(mState);
    const int padCount = PerformancePadCount(mState);
    int activeIndex = -1;

    for (std::size_t i = 0; i < mState.setlists.size(); ++i)
    {
        if (setlist == &mState.setlists[i])
        {
            activeIndex = static_cast<int>(i);
        }
    }

    const int setlistCount = static_cast<int>(mState.setlists.size());

    // A bank just added opens in its editor once the engine has made it.
    if (mPadsEditAfterCreate && setlist != nullptr && setlist->id != *mPadsEditAfterCreate)
    {
        mPadsEditAfterCreate.reset();
        mPadsEditingId = setlist->id;
        mPadsNameBox.reset();
    }

    const bool editing = setlist != nullptr && mPadsEditingId == setlist->id;

    // ── The header (.performance-pad-header): Add bank at the left; the mode and PADS at the right ──
    std::vector<Ptr> header;

    if (activeIndex < 0 || activeIndex == setlistCount - 1)
    {
        auto add = std::make_shared<Button>(
            mTheme, "Add bank",
            [this] {
                const int bank = NextSetlistBank(mState.setlists);
                const auto* current = ActiveSetlist(mState);
                mPadsEditAfterCreate = current != nullptr ? current->id : std::string();
                mSession.Commands().CreateSetlist("Bank " + std::to_string(bank), bank);
            },
            Button::Style::Plain);
        add->TextSize(11.0f).MinWidth(80);
        header.push_back(FixedWidth(80, Middle(FixedHeight(30, add))));
    }

    header.push_back(Stretch());
    auto mode = std::make_shared<Button>(mTheme, "Setlist", nullptr, Button::Style::Primary);
    mode->TextSize(11.0f).MinWidth(70);
    header.push_back(FixedWidth(70, Middle(FixedHeight(32, mode))));
    header.push_back(HGap(12));
    auto padsLabel = std::make_shared<Text>(mTheme, "PADS", 11.0f, Weight::SemiBold);
    padsLabel->Colour(mTheme.TextSecondary());
    header.push_back(FixedWidth(32, Middle(padsLabel)));
    header.push_back(HGap(8));
    auto count = std::make_shared<PickerField>(mTheme, mIcons);
    count->AccessName("Pads");
    count->Set(std::to_string(padCount));
    count->onClick = [this, padCount, weak = std::weak_ptr<PickerField>(count)] {
        auto field = weak.lock();

        if (!field)
        {
            return;
        }

        std::vector<MenuItem> items;

        for (const int n : {4, 6, 8})
        {
            items.push_back({std::to_string(n), n == padCount, false, [this, n] {
                                 mSession.Commands().SetSetting(kPerformancePadCountSetting, n);
                                 Mark(kPage);
                             }});
        }

        OpenDropdown(field->Bounds(), std::move(items), std::max(80.0f, field->Bounds().width()));
    };
    header.push_back(FixedWidth(56, Middle(count)));

    // ── The bank strip (.performance-bank-strip) ──
    const auto step = [this, setlistCount](int to, bool up, const char* end) {
        const bool can = to >= 0 && to < setlistCount;
        auto b = std::make_shared<BankStep>(
            mTheme, up, can ? mState.setlists[static_cast<std::size_t>(to)].name : std::string(end));
        b->Enabled(can);

        if (can)
        {
            b->onClick = [this, id = mState.setlists[static_cast<std::size_t>(to)].id] {
                mPadsEditingId.clear();
                mSession.Commands().SelectSetlist(id);
            };
        }

        return Stretch(b, 0.62);
    };

    Ptr current;

    if (editing)
    {
        // The bank's editor (.performance-bank-editor): its name, then Save, Cancel and Delete.
        if (!mPadsNameBox)
        {
            auto [element, box] = el::input_box("Bank name", Font(Weight::Regular), 1.0f);
            mPadsNameBox = box;
            mPadsNameElement = el::share(std::move(element));
            mPadsNameBox->set_text(setlist->name);
        }

        const std::string id = setlist->id;
        const std::optional<int> bank = setlist->bank;
        auto saveEdit = std::make_shared<Button>(
            mTheme, "Save",
            [this, id, bank] {
                const auto name = mPadsNameBox ? mPadsNameBox->get_utf8() : std::string();
                // The bank keeps its number; one without takes the next free (saveEditedSetlist).
                std::vector<Setlist> others;

                for (const auto& s : mState.setlists)
                {
                    if (s.id != id)
                    {
                        others.push_back(s);
                    }
                }

                mSession.Commands().UpdateSetlist(id, name, bank ? *bank : NextSetlistBank(others));
                mPadsEditingId.clear();
                mPadsNameBox.reset();
                Mark(kPage);
            },
            Button::Style::Primary);
        auto cancel = std::make_shared<Button>(
            mTheme, "Cancel",
            [this] {
                mPadsEditingId.clear();
                mPadsNameBox.reset();
                Mark(kPage);
            },
            Button::Style::Plain);
        auto remove = std::make_shared<Button>(
            mTheme, "Delete",
            [this, id, name = setlist->name] {
                OpenConfirm("Delete setlist", "Delete \"" + name + "\"?", "Delete", [this, id] {
                    mSession.Commands().DeleteSetlist(id);
                    mPadsEditingId.clear();
                    mPadsNameBox.reset();
                    Mark(kPage);
                });
            },
            Button::Style::Plain);

        for (auto* b : {saveEdit.get(), cancel.get(), remove.get()})
        {
            b->TextSize(12.0f).MinWidth(64);
        }

        auto label = std::make_shared<Text>(mTheme, "NAME", 11.0f, Weight::SemiBold);
        label->Colour(mTheme.TextSecondary());
        auto field = Over(PadXY(10, 0, Middle(mPadsNameElement)),
                          el::share(Surface(mTheme.Input(), mTheme.BorderStrong(), 8.0f)));
        current = Over(Pad(17, 14, 17, 12,
                           Col({FixedHeight(16, label), VGap(6), FixedHeight(40, field), VGap(10),
                                FixedHeight(32, Row({Stretch(), saveEdit, HGap(8), cancel, HGap(8), remove}))})),
                       el::share(Surface(mTheme.Card(), mTheme.Card(), kPadRadius)));
    }
    else if (setlist != nullptr)
    {
        const int paths = static_cast<int>(setlist->presetIds.size());
        auto panel = std::make_shared<BankCurrent>(mTheme, SetlistBankLabel(setlist), setlist->name,
                                                   paths > 0 ? std::to_string(std::min(padCount, paths)) + " of " +
                                                                   std::to_string(paths) + " paths visible"
                                                             : std::string("No paths assigned"),
                                                   true);
        panel->onEdit = [this, id = setlist->id] {
            mPadsEditingId = id;
            mPadsNameBox.reset();
            Mark(kPage);
        };
        current = panel;
    }
    else
    {
        current = std::make_shared<BankCurrent>(mTheme, "No bank", "No setlist selected",
                                                "Create a bank to start assigning paths.", false);
    }

    auto strip = Row({step(activeIndex - 1, false, "First"), HGap(11), Stretch(current, 1.0), HGap(11),
                      step(activeIndex < 0 ? -1 : activeIndex + 1, true, "Last")});

    // ── The pads (.performance-pad-grid): two rows of four, three or two; two across when narrow ──
    Ptr pads;

    if (setlist != nullptr)
    {
        // From the stage, not the window: the rail and the full layout's header take their share.
        const bool narrow = StageWidth() < 640.0f;
        const int columns = padCount == 4 ? 2 : narrow ? 2 : padCount / 2;
        const int cursor = mState.setlistCursor;
        const std::string id = setlist->id;
        std::vector<Ptr> rows;

        for (int start = 0; start < padCount; start += columns)
        {
            std::vector<Ptr> cells;

            for (int index = start; index < std::min(padCount, start + columns); ++index)
            {
                const std::string presetId = index < static_cast<int>(setlist->presetIds.size())
                                                 ? setlist->presetIds[static_cast<std::size_t>(index)]
                                                 : std::string();
                const auto* summary = presetId.empty() ? nullptr : mState.FindPresetSummary(presetId);
                const auto state = presetId.empty()  ? PerformancePad::State::Empty
                                   : index == cursor ? PerformancePad::State::Current
                                                     : PerformancePad::State::Loadable;
                auto pad = std::make_shared<PerformancePad>(mTheme, mIcons, index + 1,
                                                            summary != nullptr ? summary->name : presetId, state);

                if (!presetId.empty())
                {
                    pad->onClick = [this, index] { SelectSetlistSlot(index); };
                }

                pad->onAssign = [this, index] { AssignSetlistPad(index); };
                pad->onMenu = [this, id, index, presetId](el::point) {
                    std::vector<MenuItem> items;

                    if (!mState.activePresetId.empty())
                    {
                        items.push_back(
                            {"Save current preset here", false, false, [this, index] { AssignSetlistPad(index); }});
                    }

                    if (!presetId.empty())
                    {
                        items.push_back({"Clear this pad", false, false, [this, index] { ClearSetlistPad(index); }});
                    }

                    if (!items.empty())
                    {
                        OpenMenu("Pad " + std::to_string(index + 1), std::move(items));
                    }
                };

                if (!cells.empty())
                {
                    cells.push_back(HGap(11));
                }

                cells.push_back(pad);
            }

            if (!rows.empty())
            {
                rows.push_back(VGap(11));
            }

            rows.push_back(Row(cells));
        }

        pads = Col(rows);
    }
    else
    {
        pads = EmptyState("No setlist selected", "Add a bank, or make a setlist in the preset chooser.");
    }

    // A short stage tightens the spacing and the bank strip, as the web UI's max-height: 760px
    // (a 760 px window leaves the full layout's stage about 545).
    const bool short_ = StageHeight() < 560.0f;
    const float gap = short_ ? 10.0f : 14.0f;
    const float edge = short_ ? 12.0f : 16.0f;
    std::vector<Ptr> column{FixedHeight(32, Row(header)), VGap(gap)};

    // With no setlist the bank strip has nothing to step through, and the page says so once,
    // under the header.
    if (setlist != nullptr || editing)
    {
        column.push_back(FixedHeight(editing ? 136.0f : (short_ ? 72.0f : 95.0f), strip));
        column.push_back(VGap(gap));
    }

    column.push_back(pads);
    return Fill(mTheme.Background(), Pad(18, edge, 18, edge, Col(column)));
}

void Shell::AssignSetlistPad(int index)
{
    const Setlist* setlist = ActiveSetlist(mState);
    const std::string presetId = mState.activePresetId;

    if (setlist == nullptr || presetId.empty())
    {
        ShowToast("No preset playing", "Load a preset to put on this pad.", false);
        return;
    }

    const auto nameOf = [this](const std::string& id) {
        const auto* summary = mState.FindPresetSummary(id);
        return summary != nullptr ? summary->name : id;
    };
    const std::string existing =
        index < static_cast<int>(setlist->presetIds.size()) ? setlist->presetIds[static_cast<std::size_t>(index)] : "";
    const std::string pad = "pad " + std::to_string(index + 1);

    if (existing == presetId)
    {
        ShowToast("Path unchanged", "Pad " + std::to_string(index + 1) + " already plays " + nameOf(presetId) + ".",
                  false);
        return;
    }

    const auto assign = [this, id = setlist->id, index, presetId] {
        mSession.Commands().SetSetlistSlot(id, index, presetId);
    };

    if (existing.empty())
    {
        assign();
        return;
    }

    OpenConfirm("Replace setlist path",
                "Replace " + pad + " from \"" + nameOf(existing) + "\" to \"" + nameOf(presetId) + "\"?", "Replace",
                assign);
}

void Shell::ClearSetlistPad(int index)
{
    const Setlist* setlist = ActiveSetlist(mState);

    if (setlist == nullptr || index < 0 || index >= static_cast<int>(setlist->presetIds.size()) ||
        setlist->presetIds[static_cast<std::size_t>(index)].empty())
    {
        return;
    }

    const auto* summary = mState.FindPresetSummary(setlist->presetIds[static_cast<std::size_t>(index)]);
    OpenConfirm("Clear pad assignment",
                "Clear pad " + std::to_string(index + 1) + " assignment for \"" +
                    (summary != nullptr ? summary->name : setlist->presetIds[static_cast<std::size_t>(index)]) + "\"?",
                "Clear", [this, id = setlist->id, index] { mSession.Commands().SetSetlistSlot(id, index, {}); });
}
} // namespace nanoq::ui
