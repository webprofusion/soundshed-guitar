// GraphicEq.cpp - the graphic EQ's section of the effect editor, as the web UI draws it
// (signalPath/paramsPanel/panel.ts and eq.ts, css/signal-path/resource-slots.css): a Reset that
// flattens every band, and a column per band in use and turned on, its gain in dB over a vertical
// slider over its frequency in Hz, both of which can be typed in. Behind them, faint, is the EQ's
// curve over its input's spectrum (ResponsePlot), its frequency axis pinned to the sliders so
// each band's frequency sits under its own slider.

#include "ui/shell/Shell.h"

#include "dsp/EffectGuids.h"
#include "uiclient/ParamFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
namespace
{
using cycfi::artist::canvas;

// The web UI's measurements: the section padded 16, Reset's toolbar 12 over the bands; the bands
// at least 72 wide and 10 apart, each its gain (14), the 132 px slider and its frequency (16),
// 6 apart.
constexpr int kMaxBands = 10;
constexpr double kDefaultFrequencies[kMaxBands] = {31.25, 62.5, 125.0, 250.0, 500.0,
                                                   1000.0, 2000.0, 4000.0, 8000.0, 16000.0};
constexpr double kMaxGainDb = 18.0;
constexpr float kSectionPad = 16.0f;
constexpr float kToolbarGap = 12.0f;
constexpr float kBandMinWidth = 72.0f;
constexpr float kBandGap = 10.0f;
constexpr float kGainHeight = 14.0f;
constexpr float kSliderWidth = 18.0f;
constexpr float kSliderHeight = 132.0f;
constexpr float kFreqHeight = 16.0f;
constexpr float kBandHeight = kGainHeight + 6.0f + kSliderHeight + 6.0f + kFreqHeight;
// The slider's thumb (16 x 10) and the travel it has along the track.
constexpr float kThumbWidth = 16.0f;
constexpr float kThumbHeight = 10.0f;

/// `a` mixed with `b`, `share` of it `a` (color-mix in srgb).
el::color MixColour(el::color a, el::color b, float share)
{
    return el::color{a.red * share + b.red * (1.0f - share), a.green * share + b.green * (1.0f - share),
                     a.blue * share + b.blue * (1.0f - share), a.alpha * share + b.alpha * (1.0f - share)};
}

std::string BandKey(int band, const char* suffix)
{
    return "band" + std::to_string(band) + suffix;
}

double ParamOr(const std::map<std::string, double>& params, const std::string& key, double fallback)
{
    const auto it = params.find(key);
    return it != params.end() && std::isfinite(it->second) ? it->second : fallback;
}

/// A band's frequency as its neighbours see it (graphicEqFrequencyValue).
double FrequencyValue(const std::map<std::string, double>& params, int index)
{
    const auto it = params.find(BandKey(index + 1, "Freq"));
    return it != params.end() && std::isfinite(it->second) ? std::clamp(it->second, 20.0, 20000.0)
                                                            : kDefaultFrequencies[index];
}

/// The frequency band `band` (1-based) may take: between its neighbours, a hertz clear of each
/// (graphicEqFrequencyBounds).
std::pair<double, double> FrequencyBounds(const std::map<std::string, double>& params, int band)
{
    const int index = band - 1;
    const double min = index > 0 ? FrequencyValue(params, index - 1) + 1.0 : 20.0;
    const double max = index < kMaxBands - 1 ? FrequencyValue(params, index + 1) - 1.0 : 20000.0;
    return min <= max ? std::pair{min, max} : std::pair{20.0, 20000.0};
}

std::string GainText(double gain)
{
    char text[16];
    std::snprintf(text, sizeof(text), "%.1f", gain);
    return text;
}

/// A band's gain (.graphic-eq-gain): a vertical slider from -18 to +18 dB in tenths, its track
/// filled with the accent up to the gain, a small thumb on it. A double click puts it back to 0.
class GainSlider final : public el::element, public Drawn, public Focusable
{
  public:
    explicit GainSlider(const Theme& theme) : mTheme(theme)
    {
    }

    /// Each move of the slider, with the new gain.
    std::function<void(double gain)> onChange;
    std::function<void()> onReset;

    void Set(double gain)
    {
        mGain = gain;
    }

    [[nodiscard]] bool Dragging() const
    {
        return mDragging;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{kSliderWidth, 40.0f}, {kSliderWidth, el::full_extent}};
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.role = AccessRole::Slider;
        info.name = NameOr("Gain");
        info.value = GainText(mGain) + " dB";
        info.position = Position();
        return info;
    }

    bool SetPosition(double position) override
    {
        Move(-kMaxGainDb + 2.0 * kMaxGainDb * std::clamp(position, 0.0, 1.0));
        return true;
    }

    bool wants_focus() const override
    {
        return FocusWanted(true);
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

    bool wants_control() const override
    {
        return true;
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const float cx = std::round((b.left + b.right) * 0.5f);
        const float p = static_cast<float>(Position());

        // The track: 6 px wide, filled with the accent from the foot up to the gain.
        const el::rect track{cx - 3.0f, b.top, cx + 3.0f, b.bottom};
        {
            auto state = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(track, 3.0f);
            cnv.clip();
            cnv.fill_style(el::rgba(125, 140, 170, 51));
            cnv.fill_rect(track);
            cnv.fill_style(mTheme.Accent());
            cnv.fill_rect(el::rect{track.left, track.bottom - p * track.height(), track.right, track.bottom});
        }

        cnv.stroke_style(el::rgba(185, 199, 228, 51));
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(track.inset(0.5f, 0.5f), 2.5f);

        // The thumb: a small rounded bar, light at the top, its edge tinted with the accent.
        const float cy = ThumbY(b);
        const el::rect thumb{cx - kThumbWidth / 2.0f, cy - kThumbHeight / 2.0f, cx + kThumbWidth / 2.0f,
                             cy + kThumbHeight / 2.0f};
        {
            auto state = cnv.new_state();
            cnv.shadow_style(el::point{0.0f, 2.0f}, 5.0f, el::rgba(0, 0, 0, 115));
            canvas::linear_gradient face{el::point{thumb.left, thumb.top}, el::point{thumb.left, thumb.bottom}};
            face.add_color_stop(0.0f, el::rgba(229, 235, 248, 255));
            face.add_color_stop(1.0f, el::rgba(130, 146, 178, 255));
            cnv.fill_style(face);
            cnv.fill_round_rect(thumb, 4.0f);
        }

        cnv.stroke_style(MixColour(mTheme.Accent(), el::rgba(220, 231, 255, 255), 0.6f));
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(thumb.inset(0.5f, 0.5f), 3.5f);
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state != el::mouse_button::left)
        {
            return false;
        }

        if (!btn.down)
        {
            mDragging = false;
            ctx.view.refresh(ctx);
            return true;
        }

        if (btn.num_clicks == 2)
        {
            mDragging = false;

            if (onReset)
            {
                mGain = 0.0;
                onReset();
            }

            ctx.view.refresh(ctx);
            return true;
        }

        // Taken by the thumb, it moves from where it was held; anywhere else on the track, the
        // thumb comes to the pointer.
        const float thumbY = ThumbY(ctx.bounds);
        mGrab = std::abs(btn.pos.y - thumbY) <= kThumbHeight / 2.0f + 1.0f ? btn.pos.y - thumbY : 0.0f;
        mDragging = true;
        MoveTo(ctx, btn.pos.y);
        return true;
    }

    void drag(const el::context& ctx, el::mouse_button btn) override
    {
        if (mDragging)
        {
            MoveTo(ctx, btn.pos.y);
        }
    }

    bool cursor(const el::context&, el::point, el::cursor_tracking status) override
    {
        if (status != el::cursor_tracking::leaving)
        {
            el::set_cursor(el::cursor_type::v_resize);
        }

        return true;
    }

    bool key(const el::context& ctx, el::key_info k) override
    {
        using el::key_code;

        if (k.action != el::key_action::press && k.action != el::key_action::repeat)
        {
            return false;
        }

        // The arrows a step, Page Up and Down a tenth of the range, Home and End its ends.
        double to = mGain;

        switch (k.key)
        {
        case key_code::up:
        case key_code::right:
            to += 0.1;
            break;
        case key_code::down:
        case key_code::left:
            to -= 0.1;
            break;
        case key_code::page_up:
            to += 3.6;
            break;
        case key_code::page_down:
            to -= 3.6;
            break;
        case key_code::home:
            to = -kMaxGainDb;
            break;
        case key_code::end:
            to = kMaxGainDb;
            break;
        default:
            return false;
        }

        Move(to);
        ctx.view.refresh(ctx);
        return true;
    }

  private:
    [[nodiscard]] double Position() const
    {
        return std::clamp((mGain + kMaxGainDb) / (2.0 * kMaxGainDb), 0.0, 1.0);
    }

    [[nodiscard]] float ThumbY(const el::rect& b) const
    {
        return b.bottom - kThumbHeight / 2.0f - static_cast<float>(Position()) * (b.height() - kThumbHeight);
    }

    void MoveTo(const el::context& ctx, float y)
    {
        const auto b = ctx.bounds;
        const double p = (b.bottom - kThumbHeight / 2.0f - (y - mGrab)) / std::max(1.0f, b.height() - kThumbHeight);
        Move(-kMaxGainDb + 2.0 * kMaxGainDb * std::clamp(p, 0.0, 1.0));
        ctx.view.refresh(ctx);
    }

    void Move(double gain)
    {
        const double to = std::clamp(std::round(gain * 10.0) / 10.0, -kMaxGainDb, kMaxGainDb);

        if (to == mGain)
        {
            return;
        }

        mGain = to;

        if (onChange)
        {
            onChange(to);
        }
    }

    const Theme& mTheme;
    double mGain = 0.0;
    bool mDragging = false;
    float mGrab = 0.0f;
};

/// A number typed in place (.graphic-eq-gain-value-input, .graphic-eq-frequency): its value
/// right-aligned over an underline, its unit after it. A click opens it for typing; Enter or
/// leaving it sets the value, Esc puts it back.
class NumberField final : public el::element, public Drawn, public Focusable
{
  public:
    NumberField(const Theme& theme, float fieldWidth, std::string unit, el::color ink, el::color unitInk,
                Weight weight)
        : mTheme(theme), mFieldWidth(fieldWidth), mUnit(std::move(unit)), mInk(ink), mUnitInk(unitInk),
          mWeight(weight)
    {
        mUnitWidth = std::ceil(MeasureTextWidth(mUnit, Font(mWeight).size(10.0f)));
    }

    /// What the field opens with, and what was typed into it.
    std::function<std::string()> editText;
    std::function<void(const std::string& typed)> onTyped;

    void Set(std::string text)
    {
        mText = std::move(text);
    }

    /// Typing: the bound value waits until the field is done with.
    [[nodiscard]] bool Dragging() const
    {
        return mEdit.Active();
    }

    [[nodiscard]] float Width() const
    {
        return mFieldWidth + 2.0f + mUnitWidth;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{Width(), 12.0f}, {Width(), el::full_extent}};
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.role = AccessRole::Edit;
        info.name = NameOr(mUnit == "Hz" ? "Frequency" : "Gain");
        info.value = (mEdit.Active() ? mEdit.Value() : mText) + " " + mUnit;
        return info;
    }

    [[nodiscard]] bool Editing() const override
    {
        return mEdit.Active();
    }

    bool wants_focus() const override
    {
        return FocusWanted(true) || mEdit.Active();
    }

    void begin_focus(focus_request) override
    {
        NoteFocus(true);
    }

    bool end_focus() override
    {
        InlineEdit::Leave(mEdit, onTyped, [this] { RepaintSoon(DrawnArea()); });
        NoteFocus(false);
        return true;
    }

    bool wants_control() const override
    {
        return true;
    }

    bool key(const el::context& ctx, el::key_info k) override
    {
        bool changed = false;
        const bool used = InlineEdit::FieldKey(mEdit, k, editText, onTyped, changed);

        if (changed)
        {
            ctx.view.refresh(ctx);
        }

        return used;
    }

    bool text(const el::context& ctx, el::text_info info) override
    {
        if (!mEdit.Char(info))
        {
            return false;
        }

        ctx.view.refresh(ctx);
        return true;
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state != el::mouse_button::left || !btn.down)
        {
            return btn.state == el::mouse_button::left;
        }

        if (!mEdit.Active() && editText && onTyped)
        {
            // The number turns into a field where it is, and takes focus for the keys once this
            // click is done with.
            mEdit.Begin(editText());
            ctx.view.refresh(ctx);
            ctx.view.post([self = weak_from_this(), &view = ctx.view] {
                if (const auto field = self.lock())
                {
                    FocusInView(view, *field);
                }
            });
        }

        return true;
    }

    bool cursor(const el::context& ctx, el::point, el::cursor_tracking status) override
    {
        const bool hover = status != el::cursor_tracking::leaving;

        if (hover != mHover)
        {
            mHover = hover;
            ctx.view.refresh(ctx);
        }

        if (hover)
        {
            el::set_cursor(el::cursor_type::ibeam);
        }

        return true;
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto font = Font(mWeight).size(10.0f);
        const float midY = std::round((b.top + b.bottom) * 0.5f);
        const el::rect field{b.left, midY - 7.0f, b.left + mFieldWidth, midY + 7.0f};

        if (mEdit.Active())
        {
            mEdit.Draw(cnv, field, mTheme, font);
        }
        else
        {
            FillText(cnv, mText, font, mInk, el::rect{field.left, field.top, field.right - 1.0f, field.bottom - 1.0f},
                     HAlign::Right);
            // The underline, stronger under the pointer.
            cnv.stroke_style(mHover ? mTheme.TextSecondary().opacity(mTheme.TextSecondary().alpha * 0.6f)
                                    : mTheme.BorderStrong().opacity(mTheme.BorderStrong().alpha * 0.65f));
            cnv.line_width(1.0f);
            cnv.begin_path();
            cnv.move_to(field.left, field.bottom - 0.5f);
            cnv.line_to(field.right, field.bottom - 0.5f);
            cnv.stroke();
        }

        FillText(cnv, mUnit, font, mUnitInk, el::rect{field.right + 2.0f, field.top, b.right, field.bottom},
                 HAlign::Left);
    }

  private:
    const Theme& mTheme;
    float mFieldWidth;
    std::string mUnit;
    el::color mInk, mUnitInk;
    Weight mWeight;
    float mUnitWidth = 0.0f;
    std::string mText;
    bool mHover = false;
    InlineEdit mEdit;
};

/// `content` centred across its row at its own width.
Ptr Across(float width, Ptr content)
{
    return Row({Stretch(), FixedWidth(width, std::move(content)), Stretch()});
}
} // namespace

Ptr Shell::BuildGraphicEq(const guitarfx::GraphNode& node, float width, float& height)
{
    const std::string nodeId = node.id;

    // The node's parameters now, by id: the controls outlive the node they were built with.
    const auto params = [this, nodeId]() -> std::map<std::string, double> {
        const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;
        return n != nullptr ? n->params : std::map<std::string, double>{};
    };
    const auto write = [this, nodeId](const std::string& key, double value) {
        mSession.Commands().SetNodeParam(nodeId, key, value);
        NoteChainEdit();
        Mark(kValues); // the curve, and the other controls bound to the values
    };

    // The bands in use (bandCount) and turned on, a column each.
    const int bandCount = static_cast<int>(std::lround(ParamOr(node.params, "bandCount", kMaxBands)));
    std::vector<int> bands;

    for (int band = 1; band <= kMaxBands; ++band)
    {
        if (band <= bandCount && ParamOr(node.params, BandKey(band, "Enabled"), 1.0) >= 0.5)
        {
            bands.push_back(band);
        }
    }

    // The columns: as many at least 72 wide as fit, a row of them, more rows when they do not.
    const float inner = std::max(0.0f, width - 2.0f * kSectionPad);
    const int tracks = std::max(1, static_cast<int>(std::floor((inner + kBandGap) / (kBandMinWidth + kBandGap))));
    const int columns = std::max(1, std::min(static_cast<int>(bands.size()), tracks));
    const int rows = bands.empty() ? 0 : (static_cast<int>(bands.size()) + columns - 1) / columns;
    const float column = std::max(0.0f, (inner - static_cast<float>(columns - 1) * kBandGap) / static_cast<float>(columns));

    // The curve behind, faint, its axis pinned to the sliders while they stand in one row.
    auto plotElement = MakeEqPlot(NodeEqPlotSource(node), false);
    auto& plot = *mEffectPlot->plot;
    plot.Behind(0.5f, 0.7f);

    if (rows == 1)
    {
        plot.AnchorAxis([params, bands, column](const el::rect& bounds) {
            const auto values = params();
            std::vector<AxisAnchor> anchors;

            for (std::size_t i = 0; i < bands.size(); ++i)
            {
                const int band = bands[i];
                const float x = bounds.left + kSectionPad + static_cast<float>(i) * (column + kBandGap) + column / 2.0f;
                anchors.push_back({ParamOr(values, BandKey(band, "Freq"), kDefaultFrequencies[band - 1]), x});
            }

            return anchors;
        });
    }

    // Reset flattens the curve: every band's gain back to 0 dB, its layout left as it is.
    auto reset = std::make_shared<PlotToolButton>(mTheme, "Reset");
    reset->AccessName("Reset all band gains to 0 dB");
    reset->onClick = [params, write] {
        const int count = static_cast<int>(std::lround(ParamOr(params(), "bandCount", kMaxBands)));

        for (int band = 1; band <= std::min(count, kMaxBands); ++band)
        {
            write(BandKey(band, "Gain"), 0.0);
        }
    };

    std::vector<Ptr> content{FixedHeight(PlotToolButton::kHeight, Row({Stretch(), FixedWidth(reset->Width(), reset)}))};
    height = kSectionPad + PlotToolButton::kHeight + kSectionPad;

    const auto gainInk = MixColour(mTheme.Text(), mTheme.TextSecondary(), 0.72f);
    const auto freqInk = MixColour(mTheme.Text(), mTheme.TextSecondary(), 0.76f);
    std::vector<Ptr> row;
    int inRow = 0;

    // A row of bands under the toolbar, or under the row before it.
    const auto flush = [&] {
        if (row.empty())
        {
            return;
        }

        const float gap = content.size() == 1 ? kToolbarGap : kBandGap;
        row.push_back(Stretch());
        content.push_back(VGap(gap));
        content.push_back(FixedHeight(kBandHeight, Row(row)));
        height += gap + kBandHeight;
        row.clear();
        inRow = 0;
    };

    for (const int band : bands)
    {
        const std::string gainKey = BandKey(band, "Gain");
        const std::string freqKey = BandKey(band, "Freq");
        const auto gainOf = [params, gainKey] { return ParamOr(params(), gainKey, 0.0); };
        const auto freqOf = [params, freqKey, band] {
            return ParamOr(params(), freqKey, kDefaultFrequencies[band - 1]);
        };

        // The gain in dB, typed in place.
        auto gain = std::make_shared<NumberField>(mTheme, 40.0f, "dB", gainInk, gainInk, Weight::SemiBold);
        gain->AccessName("Band " + std::to_string(band) + " gain");
        mLive.Bind(gain, [gainOf] { return GainText(gainOf()); }, &NumberField::Set);
        gain->editText = [gainOf] { return GainText(gainOf()); };
        gain->onTyped = [this, write, gainKey](const std::string& typed) {
            const auto value = guitarfx::uiclient::ParseTypedNumber(typed);

            if (!value || !std::isfinite(*value))
            {
                Mark(kValues); // the value comes back as it was
                return;
            }

            write(gainKey, std::clamp(*value, -kMaxGainDb, kMaxGainDb));
        };

        // The slider; a double click puts it back to 0 dB.
        auto slider = std::make_shared<GainSlider>(mTheme);
        slider->AccessName("Band " + std::to_string(band) + " gain");
        mLive.Bind(slider, gainOf, &GainSlider::Set);
        slider->onChange = [write, gainKey](double value) { write(gainKey, value); };
        slider->onReset = [write, gainKey] { write(gainKey, 0.0); };

        // The frequency in Hz, typed in place and kept between its neighbours'.
        auto freq = std::make_shared<NumberField>(mTheme, 48.0f, "Hz", freqInk, mTheme.TextSecondary(), Weight::Regular);
        freq->AccessName("Band " + std::to_string(band) + " frequency");
        mLive.Bind(freq, [freqOf] { return std::to_string(std::lround(freqOf())); }, &NumberField::Set);
        freq->editText = [freqOf] { return std::to_string(std::lround(freqOf())); };
        freq->onTyped = [this, params, write, freqKey, band](const std::string& typed) {
            const auto value = guitarfx::uiclient::ParseTypedNumber(typed);

            if (!value || !std::isfinite(*value))
            {
                Mark(kValues);
                return;
            }

            const auto [min, max] = FrequencyBounds(params(), band);
            write(freqKey, std::clamp(*value, min, max));
        };

        if (!row.empty())
        {
            row.push_back(HGap(kBandGap));
        }

        row.push_back(FixedWidth(column, Col({FixedHeight(kGainHeight, Across(gain->Width(), gain)), VGap(6),
                                              FixedHeight(kSliderHeight, Across(kSliderWidth, slider)), VGap(6),
                                              FixedHeight(kFreqHeight, Across(freq->Width(), freq))})));

        if (++inRow == columns)
        {
            flush();
        }
    }

    flush();
    return Over(Pad(kSectionPad, kSectionPad, kSectionPad, kSectionPad, Col(content)), plotElement);
}
} // namespace nanoq::ui
