// DspStatusView.cpp - the DSP status readouts (the web UI's dspStatusReadout.ts): an effect's,
// under its header while the DSP chip is on, and the app output's, from the Controls strip. Each
// is a row of tiles the user chose (a x takes one away, the + adds one, by group), averaged and
// redrawn twice a second by uiclient's DspStatusMeter. Loudness is measured only while a
// loudness tile is on screen. The tiles are drawn as css/dsp-status.css draws them, which is the
// same in every theme.

#include "ui/shell/Shell.h"

#include "ui/widgets/ControlSkin.h"
#include "ui/widgets/Graphics.h"
#include "ui/widgets/Pressable.h"
#include "uiclient/Analysis.h"

#include <algorithm>
#include <cmath>

namespace nanoq::ui
{
using guitarfx::uiclient::DspStatusItem;

namespace
{
namespace artist = cycfi::artist;

constexpr float kRadius = 6.0f; // --radius-md
constexpr float kLabelSize = 7.0f;
constexpr float kValueSize = 10.0f;
constexpr float kLineHeight = 1.21f; // the face's normal line height, as CSS sets a line
constexpr float kPadLeft = 7.0f, kPadTop = 5.0f, kPadRight = 15.0f, kPadBottom = 5.0f;
constexpr float kPadRightTouch = 20.0f; // room for the larger x a finger gets
constexpr float kLabelGap = 2.0f;
constexpr float kTileHeight =
    1.0f + kPadTop + kLabelSize * kLineHeight + kLabelGap + kValueSize * kLineHeight + kPadBottom + 1.0f;
constexpr float kTileGap = 4.0f;
constexpr float kAddWidth = 24.0f;

/// A tile's name: 7 px bold capitals, 0.1em apart, in rgba(198, 206, 225, 0.68).
ControlText LabelSkin(const Theme& theme)
{
    return {0xADC6CEE1u, kLabelSize, 700, 0.1f, true, theme.Controls().body.family};
}

el::font_descr ValueFont()
{
    return Font(Weight::SemiBold).size(kValueSize);
}

/// The value with every digit a 0, which its width is measured by: the web UI sets the figures
/// tabular, so a tile keeps its width while its digits change and grows only for another one.
std::string Tabular(std::string text)
{
    std::replace_if(text.begin(), text.end(), [](char c) { return c >= '1' && c <= '9'; }, '0');
    return text;
}

float PadRight()
{
    return metrics::Touch() ? kPadRightTouch : kPadRight;
}

/// A tile's width: as wide as its name or its value, whichever is wider, and its padding.
float TileWidth(const Theme& theme, const std::string& label, const std::string& value)
{
    const float name = MeasureControlText(label, LabelSkin(theme));
    const float figure = MeasureTextWidth(Tabular(value), ValueFont());
    return std::ceil(std::max(name, figure)) + kPadLeft + PadRight() + 2.0f;
}

/// What the output's figures usually read, to size its flyout before they arrive.
std::string TypicalOutputValue(DspStatusItem item)
{
    switch (item)
    {
    case DspStatusItem::Peak:
    case DspStatusItem::Rms:
        return "-00.0 dBFS";
    case DspStatusItem::Headroom:
        return "00.0 dB";
    case DspStatusItem::Momentary:
    case DspStatusItem::ShortTerm:
    case DspStatusItem::Integrated:
        return "-00.0 LUFS";
    case DspStatusItem::Processing:
        return "000.0 \xCE\xBCs";
    case DspStatusItem::Latency:
        return "000 smp (0.00 ms)";
    default:
        return "00.0%";
    }
}

/// The tile's face: its shadow (0 5px 14px rgba(0, 0, 0, 0.28), outside it only, as CSS draws a
/// box's shadow), the glass, and the hairline round it.
void DrawTileFace(artist::canvas& cnv, el::rect r)
{
    {
        auto state = cnv.new_state();
        cnv.begin_path();
        cnv.add_rect(r.inset(-32.0f, -32.0f));
        cnv.add_round_rect(r, kRadius);
        cnv.fill_rule(artist::path::fill_odd_even);
        cnv.clip();
        cnv.fill_rule(artist::path::fill_winding);
        cnv.shadow_style(el::point{0.0f, 5.0f}, 14.0f, el::rgba(0, 0, 0, 71));
        cnv.begin_path();
        cnv.add_round_rect(r, kRadius);
        cnv.fill_style(el::rgba(0, 0, 0, 255));
        cnv.fill();
    }

    // linear-gradient(160deg, rgba(46, 54, 74, 0.48), rgba(20, 24, 35, 0.58)): down and a little to
    // the right, along a line long enough to reach the far corners, as CSS lays an angled gradient.
    constexpr float kSin = 0.34202014f, kCos = 0.93969262f;
    const float half = 0.5f * (r.width() * kSin + r.height() * kCos);
    const auto c = artist::center_point(r);
    artist::canvas::linear_gradient glass{el::point{c.x - kSin * half, c.y - kCos * half},
                                          el::point{c.x + kSin * half, c.y + kCos * half}};
    glass.add_color_stop(0.0f, el::rgba(46, 54, 74, 122));
    glass.add_color_stop(1.0f, el::rgba(20, 24, 35, 148));
    cnv.begin_path();
    cnv.add_round_rect(r, kRadius);
    cnv.fill_style(glass);
    cnv.fill();

    cnv.begin_path();
    cnv.add_round_rect(r.inset(0.5f, 0.5f), kRadius - 0.5f);
    cnv.stroke_style(el::rgba(182, 198, 233, 51));
    cnv.line_width(1.0f);
    cnv.stroke();
}

/// One figure's tile (.dsp-status-tile): its name over its value, sized to them, with a quiet x
/// in the top corner (.dsp-status-tile-remove) that comes up while the tile is pointed at and
/// turns red under the pointer. Only the x takes the tile away, by a click or, with the tile
/// focused, Enter. The value is a Text of its own, so the readout redraws just that.
class DspTile final : public Pressable
{
  public:
    DspTile(const Theme& theme, std::string label, std::shared_ptr<Text> value)
        : mTheme(theme), mLabel(std::move(label)), mValue(std::move(value))
    {
        AccessName("Remove " + mLabel);
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.value = mValue->Value();
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        // Short of room it gives way, its value cut with an ellipsis, as the web UI's does.
        const float w = TileWidth(mTheme, mLabel, mValue->Value());
        const float least = std::min(w, kPadLeft + PadRight() + 2.0f + 16.0f);
        return {{least, kTileHeight}, {w, kTileHeight}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        DrawTileFace(cnv, b);

        const el::rect content{b.left + 1.0f + kPadLeft, b.top + 1.0f + kPadTop, b.right - 1.0f - PadRight(),
                               b.bottom - 1.0f - kPadBottom};
        const float labelBottom = content.top + kLabelSize * kLineHeight;
        DrawControlText(cnv, mLabel, LabelSkin(mTheme), el::rgba(198, 206, 225, 173),
                        el::rect{content.left, content.top, content.right, labelBottom}, HAlign::Left);
        mValue->draw(el::context{ctx, mValue.get(),
                                 el::rect{content.left, labelBottom + kLabelGap, content.right, content.bottom}});

        // The x: rgba(214, 222, 240, 0.4), rgba(235, 241, 255, 0.85) while the tile is pointed at
        // (0.75 always for a finger, which has no hover), white on red under the pointer.
        const auto box = RemoveBox(b);
        el::color ink = metrics::Touch() ? el::rgba(235, 241, 255, 191) : el::rgba(214, 222, 240, 102);

        if (mHover)
        {
            ink = el::rgba(235, 241, 255, 217);
        }

        if (mOverRemove)
        {
            cnv.begin_path();
            cnv.add_circle(artist::circle(artist::center_point(box), box.width() * 0.5f));
            cnv.fill_style(el::rgba(196, 59, 59, 230));
            cnv.fill();
            ink = el::rgba(255, 255, 255, 255);
        }

        FillText(cnv, "\xC3\x97", Font(Weight::Regular).size(10.0f), ink, box, HAlign::Centre);
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state != el::mouse_button::left)
        {
            return false;
        }

        const bool onRemove = RemoveBox(ctx.bounds).includes(btn.pos);

        if (btn.down)
        {
            // A press elsewhere on the tile is left to what is under it.
            if (!onRemove)
            {
                return false;
            }

            mDown = true;
            return true;
        }

        const bool fire = mDown && onRemove;
        mDown = false;
        mBounds = ctx.bounds;

        if (fire)
        {
            RunFromCopy(onClick); // it rebuilds the row this sits in
        }

        return true;
    }

    bool cursor(const el::context& ctx, el::point p, el::cursor_tracking status) override
    {
        const bool over = status != el::cursor_tracking::leaving && RemoveBox(ctx.bounds).includes(p);

        if (over != mOverRemove)
        {
            mOverRemove = over;
            ctx.view.refresh(ctx);
        }

        return Pressable::cursor(ctx, p, status);
    }

  private:
    /// The x, 2 px inside the tile's border at its top right: 11 px, 16 for a finger.
    [[nodiscard]] static el::rect RemoveBox(const el::rect& b)
    {
        const float size = metrics::Touch() ? 16.0f : 11.0f;
        return el::rect{b.right - 3.0f - size, b.top + 3.0f, b.right - 3.0f, b.top + 3.0f + size};
    }

    const Theme& mTheme;
    std::string mLabel;
    std::shared_ptr<Text> mValue;
    bool mOverRemove = false;
};

/// The + after the tiles (.dsp-status-add): a dashed outline as tall as the row, which takes the
/// accent under the pointer, and opens the menu of the figures not shown.
class DspAddButton final : public Pressable
{
  public:
    explicit DspAddButton(const Theme& theme) : mTheme(theme)
    {
        AccessName("Add a figure");
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{kAddWidth, kTileHeight}, {kAddWidth, kTileHeight}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const bool lit = mHover || mDown;

        cnv.begin_path();
        cnv.add_round_rect(b, kRadius);
        cnv.fill_style(lit ? el::rgba(46, 54, 74, 153) : el::rgba(20, 24, 35, 107));
        cnv.fill();

        cnv.stroke_style(lit ? mTheme.Accent() : el::rgba(182, 198, 233, 82));
        cnv.line_width(1.0f);
        StrokeDashedRoundRect(cnv, b.inset(0.5f, 0.5f), kRadius - 0.5f, 3.0f, 3.0f);

        FillText(cnv, "+", Font(Weight::Regular).size(14.0f), lit ? mTheme.Text() : el::rgba(214, 222, 240, 179), b,
                 HAlign::Centre);
    }

  private:
    const Theme& mTheme;
};

/// One line of letter-spaced capitals in `colour`: the output flyout's title.
class TrackedLabel final : public el::element
{
  public:
    TrackedLabel(std::string text, ControlText skin, el::color colour)
        : mText(std::move(text)), mSkin(skin), mColour(colour)
    {
    }

    void draw(const el::context& ctx) override
    {
        DrawControlText(ctx.canvas, mText, mSkin, mColour, ctx.bounds, HAlign::Left);
    }

  private:
    std::string mText;
    ControlText mSkin;
    el::color mColour;
};
} // namespace

void Shell::FillDspStatus(const std::shared_ptr<DspStatusView>& view)
{
    const auto shown = guitarfx::uiclient::ShownDspStatusItems(mState, *view->readout);
    view->values.clear();
    std::vector<Ptr> cells; // the tiles, then the +

    for (const auto item : shown)
    {
        const auto& info = guitarfx::uiclient::DspStatusInfo(item);
        // The value: 10 px bold, #edf4ff.
        auto value = std::make_shared<Text>(mTheme, view->meter.Value(item), kValueSize, Weight::SemiBold);
        value->Colour(el::rgba(237, 244, 255, 255));
        view->values[item] = value;
        auto tile = std::make_shared<DspTile>(mTheme, info.label, value);
        tile->onClick = [this, weak = std::weak_ptr<DspStatusView>(view), item] {
            const auto v = weak.lock();

            if (!v)
            {
                return;
            }

            auto items = guitarfx::uiclient::ShownDspStatusItems(mState, *v->readout);
            items.erase(std::remove(items.begin(), items.end(), item), items.end());
            SetDspStatusItems(v, items);
        };
        cells.push_back(tile);
    }

    // The + lists what is not shown, by group.
    std::vector<DspStatusItem> hidden;

    for (const auto item : view->readout->available)
    {
        if (std::find(shown.begin(), shown.end(), item) == shown.end())
        {
            hidden.push_back(item);
        }
    }

    if (!hidden.empty())
    {
        auto add = std::make_shared<DspAddButton>(mTheme);
        add->onClick = [this, weak = std::weak_ptr<DspStatusView>(view), hidden, a = add.get()] {
            const auto v = weak.lock();

            if (!v)
            {
                return;
            }

            std::vector<MenuItem> items;
            std::string group;

            for (const auto& info : guitarfx::uiclient::DspStatusItems())
            {
                if (std::find(hidden.begin(), hidden.end(), info.id) == hidden.end())
                {
                    continue;
                }

                if (group != info.group)
                {
                    group = info.group;
                    items.push_back({group, false, true, {}});
                }

                items.push_back({info.label, false, false, [this, weak, id = info.id] {
                                     if (const auto v2 = weak.lock())
                                     {
                                         auto shownNow = guitarfx::uiclient::ShownDspStatusItems(mState, *v2->readout);
                                         shownNow.push_back(id);
                                         SetDspStatusItems(v2, shownNow);
                                     }
                                 }});
            }

            OpenDropdown(a->Bounds(), std::move(items), 200.0f);
        };
        cells.push_back(add);
    }

    // 4 px apart. The effect's readout lines up at the right, as the web UI's sits in the corner
    // of its header (2 px in); the output's flyout lists from the left.
    const bool output = view->address.scope.empty();
    std::vector<Ptr> row;

    if (!output)
    {
        row.push_back(Stretch());
    }

    for (std::size_t i = 0; i < cells.size(); ++i)
    {
        if (i > 0)
        {
            row.push_back(HGap(kTileGap));
        }

        row.push_back(cells[i]);
    }

    if (output)
    {
        row.push_back(Stretch());
    }

    std::vector<Ptr> column;

    if (!output)
    {
        column.push_back(VGap(2));
    }

    column.push_back(FixedHeight(kTileHeight, Row(row)));
    column.push_back(std::make_shared<el::element>());
    Fill(*view->slot, Col(column));
    mSession.View().layout();
    mSession.View().refresh();
}

void Shell::SetDspStatusItems(const std::shared_ptr<DspStatusView>& view, const std::vector<DspStatusItem>& items)
{
    // The choice is an app setting, shared with the web UI; mirrored at once so the tiles follow.
    const auto value = guitarfx::uiclient::DspStatusSettingValue(items);
    mSession.Client().MutableState().appSettings[view->readout->settingKey] = value;
    mSession.Commands().SetSetting(view->readout->settingKey, value);
    mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), weak = std::weak_ptr<DspStatusView>(view)] {
        if (const auto v = weak.lock(); v && alive.lock())
        {
            FillDspStatus(v);
        }
    });
}

Ptr Shell::BuildDspStatus(const guitarfx::uiclient::DspStatusReadout& readout, const std::string& nodeKey,
                          const guitarfx::uiclient::NodeAddress& address, bool overlay)
{
    auto view = std::make_shared<DspStatusView>();
    view->readout = &readout;
    view->meter = guitarfx::uiclient::DspStatusMeter(nodeKey);
    view->address = address;
    view->meter.Update(mState, UiSession::Now());
    (overlay ? mOutputDspStatus : mEffectDspStatus) = view;
    FillDspStatus(view);
    return view->slot;
}

void Shell::SyncDspStatus()
{
    auto& analysis = mSession.Client().GetAnalysis();
    const double now = UiSession::Now();

    // The effect's loudness is measured while its readout shows a loudness tile, and so is the
    // output's; neither costs anything otherwise.
    bool effectLoudness = false, outputLoudness = false;

    for (auto* slot : {&mEffectDspStatus, &mOutputDspStatus})
    {
        const auto& view = *slot;

        if (!view)
        {
            continue;
        }

        const auto shown = guitarfx::uiclient::ShownDspStatusItems(mState, *view->readout);
        const bool loud = guitarfx::uiclient::ShowsLoudness(shown);
        (view->address.scope.empty() ? outputLoudness : effectLoudness) |= loud;

        if (!view->address.scope.empty() && loud)
        {
            analysis.WatchLoudness(view->address);
        }

        view->meter.Update(mState, now);

        if (!view->meter.TakeRenderTurn(now))
        {
            continue;
        }

        // A tile is as wide as its value, so the row is laid out again when a value gains or loses
        // a character; otherwise only the values are redrawn.
        bool relayout = false;

        for (const auto& [item, weak] : view->values)
        {
            const auto value = view->meter.Value(item);

            if (const auto text = weak.lock(); text && text->Value() != value)
            {
                relayout = relayout || Tabular(text->Value()) != Tabular(value);
                text->Set(value);
                mSession.Repaint(*text);
            }
        }

        if (relayout)
        {
            mSession.View().layout();
            mSession.View().refresh();
        }
    }

    if (!effectLoudness)
    {
        analysis.StopLoudness();
    }

    analysis.WatchOutputLoudness(outputLoudness);
}

void Shell::OpenOutputStatus(el::rect anchor)
{
    CloseOverlay();
    // The flyout (.output-status-flyout): padded 8 10 10, its title (10 px bold capitals, 0.1em
    // apart) 8 px over the tiles.
    const auto& body = mTheme.Controls().body;
    auto title = std::make_shared<TrackedLabel>("Output status", ControlText{0u, 10.0f, 700, 0.1f, true, body.family},
                                                mTheme.DarkOr(el::rgba(220, 227, 243, 173), mTheme.TextSecondary()));
    const float titleHeight = std::ceil(10.0f * kLineHeight);
    const auto& readout = guitarfx::uiclient::OutputDspStatus();
    auto tiles = BuildDspStatus(readout, {}, {}, true);

    // As wide as the tiles it shows once their figures arrive, and the +. It does not wrap them
    // as the web UI's does, so it is not held to its 420 px.
    const auto shown = guitarfx::uiclient::ShownDspStatusItems(mState, readout);
    float row = 0.0f;

    for (const auto item : shown)
    {
        const std::string now = mOutputDspStatus ? mOutputDspStatus->meter.Value(item) : std::string();
        const std::string typical = TypicalOutputValue(item);
        const auto& label = guitarfx::uiclient::DspStatusInfo(item).label;
        row += std::max(TileWidth(mTheme, label, now), TileWidth(mTheme, label, typical)) + kTileGap;
    }

    row += shown.size() < readout.available.size() ? kAddWidth : 0.0f;
    const float width = std::min(std::max(row + 20.0f, 160.0f), ViewSize().x - 24.0f);
    ShowPopover(Pad(10, 8, 10, 10, Col({FixedHeight(titleHeight, title), VGap(8), FixedHeight(kTileHeight, tiles)})),
                anchor, width, 8.0f + titleHeight + 8.0f + kTileHeight + 10.0f);
    mOnOverlayClosed = [this] { mOutputDspStatus.reset(); };
}
} // namespace nanoq::ui
