// IrAlignment.cpp - the IR Cabinet's Alignment section (the web UI's
// signalPath/paramsPanel/irAlignmentPanel.ts and irAlignment.ts): where IR B sits in time against
// IR A.
//
// Two blended IRs sum like two mics on one speaker. Lined up they reinforce; a fraction of a
// millisecond apart they comb-filter, which can be a fault to fix or a tone to choose, so any
// offset in reach can be set. The section draws the start of both IRs with B moved by the offset
// (drag it), how alike the two are at every offset (click or drag along it), and the two summed
// against the two added as if they could not interfere, so a cancellation shows as a dip. Auto
// Align moves B, and flips it if it lines up inverted, to where the engine finds the best fit.
//
// The engine answers both questions: analyzeIrAlignment once per pair of IRs (the waveforms and
// how alike they are at each offset), and getIrAlignmentResponse on every offset or parameter
// change (the summed curve, from the cab's own designs). With one IR loaded there is nothing to
// align, and the section shows that IR's response alone.

#include "ui/Drawn.h"
#include "ui/shell/Shell.h"

#include "dsp/EffectGuids.h"
#include "presets/PresetTypesJson.h"
#include "ui/widgets/TextDraw.h"
#include "uiclient/Analysis.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
using guitarfx::uiclient::IrAlignment;
using guitarfx::uiclient::IrAlignmentResponse;
using guitarfx::uiclient::ResponseCurve;

namespace
{
using cycfi::artist::canvas;

/// slotBOffset's reach either way, in ms (IrSlotAlignment::kMaxOffsetMs).
constexpr double kMaxOffsetMs = 10.0;
/// Sound travels about 34.3 cm in a millisecond: how far a mic moves for an offset.
constexpr double kCmPerMs = 34.3;

// The section's rows, top to bottom.
constexpr float kTitleH = 14.0f, kHeaderH = 16.0f, kWavesH = 84.0f, kStripH = 28.0f, kControlsH = 26.0f,
                kReadoutH = 26.0f, kPlotLabelH = 14.0f, kPlotH = 150.0f, kNoteH = 14.0f, kGap = 6.0f;

const el::color kColourA = el::rgba(72, 168, 224, 242);
const el::color kColourB = el::rgba(240, 160, 64, 242);

/// The slot's ref as the node holds it, or null for an empty slot (irAlignmentPanel.ts slotRef).
const guitarfx::ResourceRef* SlotRef(const guitarfx::GraphNode& node, std::size_t index)
{
    if (index >= node.resources.size())
    {
        return nullptr;
    }

    const auto& ref = node.resources[index];
    return !ref.resourceId.empty() || !ref.filePath.empty() || !ref.embeddedId.empty() ? &ref : nullptr;
}

nlohmann::json RefJson(const guitarfx::ResourceRef* ref)
{
    return ref != nullptr ? guitarfx::SerializeResourceRef(*ref) : nlohmann::json();
}

/// Which IRs an analysis is for: a new one in either slot needs a new analysis.
std::string PairKey(const guitarfx::ResourceRef* a, const guitarfx::ResourceRef* b)
{
    const auto one = [](const guitarfx::ResourceRef* ref) {
        return ref != nullptr
                   ? ref->resourceId + "|" + guitarfx::util::PathToUtf8(ref->filePath) + "|" + ref->embeddedId
                   : std::string("-");
    };
    return one(a) + "::" + one(b);
}

double Param(const guitarfx::GraphNode& node, const char* key, double fallback)
{
    const auto it = node.params.find(key);
    return it != node.params.end() && std::isfinite(it->second) ? it->second : fallback;
}

double OffsetOf(const guitarfx::GraphNode& node)
{
    return std::clamp(Param(node, "slotBOffset", 0.0), -kMaxOffsetMs, kMaxOffsetMs);
}

/// Each slot's polarity as the node's params set it: +1 or -1.
double PolarityA(const guitarfx::GraphNode& node)
{
    return Param(node, "slotAPolarity", 0.0) >= 0.5 ? -1.0 : 1.0;
}

double PolarityB(const guitarfx::GraphNode& node)
{
    return Param(node, "slotBPolarity", 0.0) >= 0.5 ? -1.0 : 1.0;
}

/// The signed match with B moved by `offsetMs`, between whole samples by straight line, held at
/// the ends beyond the analysis's reach (irAlignment.ts matchAtOffset).
double MatchAtOffset(const IrAlignment& analysis, double offsetMs)
{
    const auto& curve = analysis.matchByOffset;

    if (curve.empty())
    {
        return 0.0;
    }

    const double position = std::clamp(offsetMs * analysis.sampleRate / 1000.0 + analysis.maxOffsetSamples, 0.0,
                                       static_cast<double>(curve.size() - 1));
    const auto below = static_cast<std::size_t>(std::floor(position));
    const std::size_t above = std::min(curve.size() - 1, below + 1);
    return curve[below] + (position - static_cast<double>(below)) * (curve[above] - curve[below]);
}

/// "+0.42 ms", to the microsecond when it is that fine.
std::string FormatOffset(double ms)
{
    char text[32];
    std::snprintf(text, sizeof(text), std::abs(ms) < 0.1 && ms != 0.0 ? "%+.3f ms" : "%+.2f ms", ms);
    return ms == 0.0 ? std::string(text + 1) : std::string(text);
}

/// What the offset means as a mic move: "B 14.4 cm closer".
std::string DescribeDistance(double ms)
{
    const double cm = std::abs(ms) * kCmPerMs;

    if (cm < 0.05)
    {
        return "B where it was captured";
    }

    char text[48];

    if (cm < 10.0)
    {
        std::snprintf(text, sizeof(text), "B %.1f cm %s", cm, ms < 0.0 ? "closer" : "further away");
    }
    else
    {
        std::snprintf(text, sizeof(text), "B %ld cm %s", std::lround(cm), ms < 0.0 ? "closer" : "further away");
    }

    return text;
}

/// Blend at one end, or L/R Split, changes what the offset does: say so.
std::string NoteFor(const guitarfx::GraphNode& node)
{
    if (Param(node, "lrSplit", 0.0) >= 0.5)
    {
        return "L/R Split is on: A plays left and B right, so the offset sets the stereo width.";
    }

    const double blend = Param(node, "irBlend", 0.0);

    if (blend <= 0.01 || blend >= 0.99)
    {
        return std::string("IR Blend is all ") + (blend <= 0.01 ? "A" : "B") + ", so the offset is not heard yet.";
    }

    return {};
}

/// What the waveforms and the match strip draw, shared with the section.
struct DrawState
{
    std::optional<IrAlignment> analysis;
    double offsetMs = 0.0;
    double polarityA = 1.0, polarityB = 1.0;
    /// While B is dragged the view holds still, so the waveform does not slide from under the pointer.
    std::optional<std::pair<double, double>> heldView; // start, span (ms)
    std::function<void(double offsetMs, bool commit)> onOffset;
};

/// The stretch of time the waveforms show, in ms of A's time (irAlignmentPanel.ts waveViewFor).
std::pair<double, double> WaveView(const IrAlignment& analysis, double offsetMs)
{
    const double startB = analysis.onsetBMs + offsetMs;
    return {std::min(analysis.onsetAMs, startB) - 0.5, std::max(4.0, std::abs(startB - analysis.onsetAMs) + 3.5)};
}

void Well(canvas& cnv, const Theme& theme, const el::rect& b)
{
    cnv.fill_style(theme.Pick(el::rgba(0, 0, 0, 70), el::rgba(255, 255, 255, 120)));
    cnv.add_round_rect(b, 6.0f);
    cnv.fill();
}

void DashedVLine(canvas& cnv, float x, float top, float bottom)
{
    for (float y = top; y < bottom; y += 6.0f)
    {
        cnv.begin_path();
        cnv.move_to(x, y);
        cnv.line_to(x, std::min(bottom, y + 3.0f));
        cnv.stroke();
    }
}

/// The start of both IRs, B moved by the offset, each drawn to its own peak: the shapes and their
/// timing are what line up, not their levels. Dragging moves B.
class AlignmentWaves final : public el::element, public Drawn
{
  public:
    AlignmentWaves(const Theme& theme, std::shared_ptr<DrawState> state) : mTheme(theme), mState(std::move(state))
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{200.0f, 40.0f}, {el::full_extent, el::full_extent}};
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
        Well(cnv, mTheme, b);

        if (!mState->analysis)
        {
            return;
        }

        const auto& analysis = *mState->analysis;
        const auto [startMs, spanMs] = mState->heldView.value_or(WaveView(analysis, mState->offsetMs));
        const float middle = (b.top + b.bottom) * 0.5f;
        const auto toX = [&](double ms) { return b.left + static_cast<float>((ms - startMs) / spanMs) * b.width(); };

        auto clip = cnv.new_state();
        cnv.add_round_rect(b, 6.0f);
        cnv.clip();

        cnv.stroke_style(mTheme.PlotGrid());
        cnv.line_width(1.0f);
        cnv.begin_path();
        cnv.move_to(b.left, middle);
        cnv.line_to(b.right, middle);

        for (double ms = std::ceil(startMs); ms <= startMs + spanMs; ms += 1.0)
        {
            cnv.move_to(toX(ms), b.top);
            cnv.line_to(toX(ms), b.bottom);
        }

        cnv.stroke();

        const auto trace = [&](const std::vector<float>& samples, double shiftMs, double sign, el::color colour) {
            float peak = 0.0f;

            for (const float s : samples)
            {
                peak = std::max(peak, std::abs(s));
            }

            if (peak <= 0.0f)
            {
                return;
            }

            const float scale = static_cast<float>(sign) * b.height() * 0.44f / peak;
            const double msPerSample = 1000.0 / analysis.sampleRate;
            cnv.stroke_style(colour);
            cnv.line_width(1.5f);
            cnv.begin_path();
            bool penDown = false;

            for (float px = b.left; px < b.right; px += 1.0f)
            {
                const double ms = startMs + (px - b.left) / b.width() * spanMs - shiftMs;
                const double position = ms / msPerSample;
                const auto index = static_cast<long>(std::floor(position));

                if (index < 0 || index + 1 >= static_cast<long>(samples.size()))
                {
                    penDown = false;
                    continue;
                }

                const auto i = static_cast<std::size_t>(index);
                const float value = samples[i] + static_cast<float>(position - static_cast<double>(index)) *
                                                     (samples[i + 1] - samples[i]);
                const float y = middle - value * scale;

                if (penDown)
                {
                    cnv.line_to(px, y);
                }
                else
                {
                    cnv.move_to(px, y);
                }

                penDown = true;
            }

            cnv.stroke();
        };
        trace(analysis.waveformA, 0.0, mState->polarityA, kColourA);
        trace(analysis.waveformB, mState->offsetMs, mState->polarityB, kColourB);

        // Where each IR starts: a short mark at the top in its colour.
        const auto onset = [&](double ms, el::color colour) {
            const float x = toX(ms);
            cnv.fill_style(colour);
            cnv.fill_rect(el::rect{x - 1.0f, b.top, x + 1.0f, b.top + 8.0f});
        };
        onset(analysis.onsetAMs, kColourA);
        onset(analysis.onsetBMs + mState->offsetMs, kColourB);
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state != el::mouse_button::left || !mState->analysis)
        {
            return false;
        }

        if (btn.down)
        {
            mStartX = btn.pos.x;
            mStartOffset = mState->offsetMs;
            mState->heldView = WaveView(*mState->analysis, mState->offsetMs);
            mDragging = true;
            return true;
        }

        if (mDragging)
        {
            mDragging = false;
            mState->heldView.reset();

            if (mState->onOffset)
            {
                mState->onOffset(mState->offsetMs, true);
            }

            ctx.view.refresh(ctx);
        }

        return true;
    }

    void drag(const el::context& ctx, el::mouse_button btn) override
    {
        if (!mDragging || !mState->heldView)
        {
            return;
        }

        // Shift for fine moves, a tenth of the pointer's travel.
        const double msPerPixel = mState->heldView->second / std::max(1.0f, ctx.bounds.width()) *
                                  ((btn.modifiers & el::mod_shift) != 0 ? 0.1 : 1.0);

        if (mState->onOffset)
        {
            mState->onOffset(mStartOffset + (btn.pos.x - mStartX) * msPerPixel, false);
        }

        ctx.view.refresh(ctx);
    }

  private:
    const Theme& mTheme;
    std::shared_ptr<DrawState> mState;
    bool mDragging = false;
    float mStartX = 0.0f;
    double mStartOffset = 0.0;
};

/// How alike A and B are at every offset in reach, above the line where they reinforce and below
/// where they cancel, as the polarities stand; the aligned offset dashed, the current one solid.
/// A click or a drag along it moves B there.
class AlignmentStrip final : public el::element, public Drawn
{
  public:
    AlignmentStrip(const Theme& theme, std::shared_ptr<DrawState> state) : mTheme(theme), mState(std::move(state))
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{200.0f, 20.0f}, {el::full_extent, el::full_extent}};
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
        Well(cnv, mTheme, b);
        const float middle = (b.top + b.bottom) * 0.5f;
        const auto toX = [&](double ms) {
            return b.left + static_cast<float>((ms + kMaxOffsetMs) / (2.0 * kMaxOffsetMs)) * b.width();
        };

        cnv.stroke_style(mTheme.PlotGrid());
        cnv.line_width(1.0f);
        cnv.begin_path();
        cnv.move_to(b.left, middle);
        cnv.line_to(b.right, middle);

        for (double ms = -kMaxOffsetMs; ms <= kMaxOffsetMs; ms += 2.0)
        {
            cnv.move_to(toX(ms), middle - 3.0f);
            cnv.line_to(toX(ms), middle + 3.0f);
        }

        cnv.stroke();

        if (!mState->analysis)
        {
            return;
        }

        const double sign = mState->polarityA * mState->polarityB;
        cnv.fill_style(kColourA.opacity(0.45f));
        cnv.begin_path();
        cnv.move_to(b.left, middle);

        for (float px = b.left; px <= b.right; px += 1.0f)
        {
            const double ms = (px - b.left) / b.width() * 2.0 * kMaxOffsetMs - kMaxOffsetMs;
            cnv.line_to(px, middle - static_cast<float>(sign * MatchAtOffset(*mState->analysis, ms)) *
                                         (middle - b.top - 2.0f));
        }

        cnv.line_to(b.right, middle);
        cnv.close_path();
        cnv.fill();

        cnv.stroke_style(mTheme.TextMuted());
        cnv.line_width(1.0f);
        DashedVLine(cnv, std::round(toX(mState->analysis->alignedOffsetMs)) + 0.5f, b.top, b.bottom);
        cnv.stroke_style(kColourB);
        cnv.line_width(2.0f);
        cnv.begin_path();
        const float x = std::round(toX(mState->offsetMs)) + 0.5f;
        cnv.move_to(x, b.top);
        cnv.line_to(x, b.bottom);
        cnv.stroke();
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state != el::mouse_button::left || !mState->analysis)
        {
            return false;
        }

        mDragging = btn.down;

        if (mState->onOffset)
        {
            mState->onOffset(OffsetAt(ctx, btn.pos.x), !btn.down);
        }

        ctx.view.refresh(ctx);
        return true;
    }

    void drag(const el::context& ctx, el::mouse_button btn) override
    {
        if (!mDragging)
        {
            return;
        }

        if (mState->onOffset)
        {
            mState->onOffset(OffsetAt(ctx, btn.pos.x), false);
        }

        ctx.view.refresh(ctx);
    }

  private:
    static double OffsetAt(const el::context& ctx, float x)
    {
        return (x - ctx.bounds.left) / std::max(1.0f, ctx.bounds.width()) * 2.0 * kMaxOffsetMs - kMaxOffsetMs;
    }

    const Theme& mTheme;
    std::shared_ptr<DrawState> mState;
    bool mDragging = false;
};

std::optional<ResponseCurve> Curve(const std::vector<double>& frequencies, const std::vector<double>& db)
{
    if (frequencies.size() < 2 || db.size() != frequencies.size())
    {
        return std::nullopt;
    }

    ResponseCurve curve;
    curve.frequencies = frequencies;
    curve.magnitudesDb = db;
    return curve;
}
} // namespace

/// The section's state across rebuilds of the effect page: the last analysis and the pair it was
/// for, and the views of the section on screen now.
struct Shell::IrAlignmentPanel
{
    // The analysis, kept for the pair it was made from, as the web UI keeps `shown`.
    std::string shownKey;
    std::optional<IrAlignment> shown;
    std::string failedKey, failedError;
    std::string requestedKey;

    // The section on screen.
    std::string nodeId;
    std::string pairKey;
    bool single = false;
    nlohmann::json irA, irB;
    std::shared_ptr<DrawState> draw = std::make_shared<DrawState>();
    std::weak_ptr<el::element> waves, strip;
    std::weak_ptr<ResponsePlot> plot;
    std::weak_ptr<Text> status, distance, match, note;
    std::weak_ptr<IconButton> autoAlign, invertA, invertB;
    std::string lastQuery; // the response last asked for
};

float Shell::IrAlignmentHeight(const guitarfx::GraphNode& node)
{
    const bool hasA = SlotRef(node, 0) != nullptr;
    const bool hasB = SlotRef(node, 1) != nullptr;

    if (node.type != guitarfx::EffectGuids::kCabIr || (!hasA && !hasB))
    {
        return 0.0f;
    }

    if (!hasA || !hasB)
    {
        return kTitleH + kGap + kHeaderH + kPlotH;
    }

    return kTitleH + kGap + kHeaderH + kGap + kWavesH + kGap + kStripH + kGap + kControlsH + kGap + kReadoutH + kGap +
           kPlotLabelH + kPlotH + kGap + kNoteH;
}

Ptr Shell::BuildIrAlignment(const guitarfx::GraphNode& node)
{
    if (!mIrAlignment)
    {
        mIrAlignment = std::make_shared<IrAlignmentPanel>();
    }

    auto& panel = *mIrAlignment;
    const auto* refA = SlotRef(node, 0);
    const auto* refB = SlotRef(node, 1);
    panel.nodeId = node.id;
    panel.pairKey = PairKey(refA, refB);
    panel.single = refA == nullptr || refB == nullptr;
    panel.irA = RefJson(refA);
    panel.irB = RefJson(refB);
    panel.lastQuery.clear();
    panel.draw = std::make_shared<DrawState>();
    const std::string nodeId = node.id;

    const auto text = [this](float size, el::color colour) {
        auto t = std::make_shared<Text>(mTheme, "", size);
        t->Colour(colour);
        return t;
    };
    auto title = std::make_shared<Text>(mTheme, panel.single ? "IR RESPONSE" : "ALIGNMENT", 9.0f, Weight::SemiBold);
    title->Colour(mTheme.TextSecondary());
    auto status = text(11.0f, mTheme.TextSecondary());
    status->Alignment(Text::Align::Right);
    panel.status = status;

    auto plot = std::make_shared<ResponsePlot>(mTheme);
    plot->Range(-36.0, 18.0, 12.0); // the web UI's: a cabinet rolls the top off by far more than it lifts
    panel.plot = plot;

    if (panel.single)
    {
        auto label = text(11.0f, mTheme.TextMuted());
        label->Set(refA != nullptr ? "IR A" : "IR B");
        SyncIrAlignment();
        return Col({FixedHeight(kTitleH, title), VGap(kGap), FixedHeight(kHeaderH, Row({label, Stretch(), status})),
                    FixedHeight(kPlotH, plot)});
    }

    // ── Both IRs: the alignment controls ──
    panel.draw->onOffset = [this, nodeId](double offsetMs, bool commit) {
        // Rounded to the microsecond, which keeps a saved preset readable.
        const double value = std::round(std::clamp(offsetMs, -kMaxOffsetMs, kMaxOffsetMs) * 1000.0) / 1000.0;
        mSession.Commands().SetNodeParam(nodeId, "slotBOffset", value);
        NoteChainEdit();

        if (commit)
        {
            Mark(kValues);
        }

        SyncIrAlignment();
    };
    const auto setOffset = [this](double offsetMs) {
        if (mIrAlignment && mIrAlignment->draw->onOffset)
        {
            mIrAlignment->draw->onOffset(offsetMs, true);
        }
    };
    const auto setParam = [this, nodeId](const char* key, double value) {
        mSession.Commands().SetNodeParam(nodeId, key, value);
        NoteChainEdit();
        Mark(kValues);
        SyncIrAlignment();
    };
    const auto offsetNow = [this, nodeId] {
        const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;
        return n != nullptr ? OffsetOf(*n) : 0.0;
    };

    auto keyA = text(11.0f, kColourA);
    keyA->Set("IR A");
    auto keyB = text(11.0f, kColourB);
    keyB->Set("IR B");

    auto waves = std::make_shared<AlignmentWaves>(mTheme, panel.draw);
    auto strip = std::make_shared<AlignmentStrip>(mTheme, panel.draw);
    panel.waves = waves;
    panel.strip = strip;

    std::vector<Ptr> controls;

    for (const auto& [label, step] : std::vector<std::pair<const char*, double>>{{"\xE2\x88\x92"
                                                                                  "0.1",
                                                                                  -0.1},
                                                                                 {"\xE2\x88\x92"
                                                                                  ".01",
                                                                                  -0.01},
                                                                                 {"+.01", 0.01},
                                                                                 {"+0.1", 0.1}})
    {
        auto nudge = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        nudge->Size(44, kControlsH).Label(label, 11.0f);
        nudge->onClick = [setOffset, offsetNow, step = step] { setOffset(offsetNow() + step); };
        controls.push_back(nudge);
        controls.push_back(HGap(4));
    }

    auto autoAlign = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    autoAlign->Size(84, kControlsH).Label("Auto Align", 11.0f);
    autoAlign->onClick = [this, nodeId, setOffset, setParam] {
        if (!mIrAlignment || !mIrAlignment->shown || mIrAlignment->shownKey != mIrAlignment->pairKey)
        {
            return;
        }

        const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;

        if (n == nullptr)
        {
            return;
        }

        // B's polarity is set against A's as it stands, so A is never flipped behind the user's back.
        const bool invertA = PolarityA(*n) < 0.0;
        const bool invertB = invertA != mIrAlignment->shown->invertB;
        const double aligned = mIrAlignment->shown->alignedOffsetMs;

        if ((PolarityB(*n) < 0.0) != invertB)
        {
            setParam("slotBPolarity", invertB ? 1.0 : 0.0);
        }

        setOffset(aligned);
    };
    panel.autoAlign = autoAlign;
    auto zero = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    zero->Size(52, kControlsH).Label("Zero", 11.0f);
    zero->onClick = [setOffset] { setOffset(0.0); };
    controls.push_back(HGap(8));
    controls.push_back(autoAlign);
    controls.push_back(HGap(4));
    controls.push_back(zero);
    controls.push_back(Stretch());

    const auto invert = [this, nodeId, setParam](const char* label, const char* key) {
        auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        button->Size(76, kReadoutH).Label(label, 11.0f);
        button->onClick = [this, nodeId, setParam, key] {
            const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;
            setParam(key, n != nullptr && Param(*n, key, 0.0) >= 0.5 ? 0.0 : 1.0);
        };
        return button;
    };
    auto invertA = invert("Invert A", "slotAPolarity");
    auto invertB = invert("Invert B", "slotBPolarity");
    panel.invertA = invertA;
    panel.invertB = invertB;
    auto distance = text(11.0f, mTheme.TextSecondary());
    auto match = text(11.0f, mTheme.TextSecondary());
    match->Alignment(Text::Align::Right);
    panel.distance = distance;
    panel.match = match;

    auto plotLabel = text(10.0f, mTheme.TextMuted());
    plotLabel->Set("A + B Response");
    auto plotKey = text(10.0f, mTheme.TextMuted());
    plotKey->Set("Dashed: without interference").Alignment(Text::Align::Right);
    auto note = text(10.5f, mTheme.Warning());
    panel.note = note;

    SyncIrAlignment();
    return Col({FixedHeight(kTitleH, title), VGap(kGap),
                FixedHeight(kHeaderH, Row({FixedWidth(40, keyA), FixedWidth(40, keyB), Stretch(), status})), VGap(kGap),
                FixedHeight(kWavesH, waves), VGap(kGap), FixedHeight(kStripH, strip), VGap(kGap),
                FixedHeight(kControlsH, Row(controls)), VGap(kGap),
                FixedHeight(kReadoutH, Row({invertA, HGap(6), invertB, HGap(12), distance, Stretch(), match})),
                VGap(kGap), FixedHeight(kPlotLabelH, Row({plotLabel, Stretch(), plotKey})), FixedHeight(kPlotH, plot),
                VGap(kGap), FixedHeight(kNoteH, note)});
}

void Shell::SyncIrAlignment()
{
    if (!mIrAlignment)
    {
        return;
    }

    auto& panel = *mIrAlignment;
    const auto plot = panel.plot.lock();

    if (!plot)
    {
        return; // the section is not on screen
    }

    const auto* node = mState.activePreset ? mState.activePreset->graph.FindNode(panel.nodeId) : nullptr;

    if (node == nullptr)
    {
        return;
    }

    // A pair not analysed yet is asked about once; a new IR in either slot rebuilds the page with
    // a new pair, which is asked about in turn.
    if (panel.shownKey != panel.pairKey && panel.failedKey != panel.pairKey && panel.requestedKey != panel.pairKey)
    {
        panel.requestedKey = panel.pairKey;
        mSession.Client().GetAnalysis().AnalyzeIrAlignment(
            panel.irA, panel.irB,
            [this, alive = std::weak_ptr<bool>(mAlive), key = panel.pairKey](const std::optional<IrAlignment>& result,
                                                                             const std::string& error) {
                if (!alive.lock() || !mIrAlignment)
                {
                    return;
                }

                auto& p = *mIrAlignment;

                if (p.requestedKey == key)
                {
                    p.requestedKey.clear();
                }

                if (result)
                {
                    p.shownKey = key;
                    p.shown = result;
                }
                else
                {
                    p.failedKey = key;
                    p.failedError = error;
                }

                SyncIrAlignment();
            });
    }

    const double offset = OffsetOf(*node);
    const bool ready = panel.shownKey == panel.pairKey && panel.shown;
    auto& draw = *panel.draw;
    draw.analysis = ready ? panel.shown : std::nullopt;
    draw.offsetMs = offset;
    draw.polarityA = PolarityA(*node);
    draw.polarityB = PolarityB(*node);

    const auto setText = [this](const std::weak_ptr<Text>& weak, const std::string& value) {
        if (const auto t = weak.lock(); t && t->Value() != value)
        {
            t->Set(value);
            mSession.Repaint(*t);
        }
    };

    std::string status;

    if (!ready)
    {
        status = panel.failedKey == panel.pairKey ? panel.failedError : "Analysing\xE2\x80\xA6";
    }
    else if (!panel.single)
    {
        const bool lined = std::abs(offset - panel.shown->alignedOffsetMs) < 0.02 &&
                           ((draw.polarityA * draw.polarityB < 0.0) == panel.shown->invertB);
        status = lined ? "Lined up"
                       : "Lines up at " + FormatOffset(panel.shown->alignedOffsetMs) +
                             (panel.shown->invertB ? ", B inverted" : "");
    }

    setText(panel.status, status);

    if (!panel.single)
    {
        setText(panel.distance, FormatOffset(offset) + " \xC2\xB7 " + DescribeDistance(offset));
        std::string matchText;

        if (ready)
        {
            const double m = MatchAtOffset(*panel.shown, offset) * draw.polarityA * draw.polarityB;
            const long percent = std::lround(std::abs(m) * 100.0);
            const bool cancelling = m < 0.0 && percent > 0; // not "-0%"
            char text[64];
            std::snprintf(text, sizeof(text), "Match %s%ld%%%s", cancelling ? "\xE2\x88\x92" : "+", percent,
                          cancelling ? " (cancelling)" : "");
            matchText = text;
        }

        setText(panel.match, matchText);
        setText(panel.note, NoteFor(*node));

        if (const auto b = panel.autoAlign.lock())
        {
            b->Disabled(!ready);
            mSession.Repaint(*b);
        }

        for (const auto& [weak, lit] :
             {std::pair{panel.invertA, draw.polarityA < 0.0}, std::pair{panel.invertB, draw.polarityB < 0.0}})
        {
            if (const auto b = weak.lock())
            {
                b->Lit(lit);
                mSession.Repaint(*b);
            }
        }

        for (const auto& weak : {panel.waves, panel.strip})
        {
            if (const auto e = weak.lock())
            {
                mSession.Repaint(*e);
            }
        }
    }

    // The summed response, asked for only when the IRs, the parameters or the offset changed.
    nlohmann::json params = nlohmann::json::object();

    for (const auto& [key, value] : node->params)
    {
        if (std::isfinite(value))
        {
            params[key] = value;
        }
    }

    auto query = nlohmann::json{panel.pairKey, params, offset}.dump();

    if (query == panel.lastQuery)
    {
        return;
    }

    panel.lastQuery = std::move(query);
    std::weak_ptr<ResponsePlot> weak = plot;
    mSession.Client().GetAnalysis().RequestIrAlignmentResponse(
        panel.irA, panel.irB, params, offset,
        [this, alive = std::weak_ptr<bool>(mAlive), weak](const std::optional<IrAlignmentResponse>& response,
                                                          const std::string&) {
            const auto target = weak.lock();

            // A failure keeps the last curve on screen: the next change asks again.
            if (!alive.lock() || !target || !response)
            {
                return;
            }

            target->SetCurve(Curve(response->frequencies, response->summedDb));
            target->SetReference(Curve(response->frequencies, response->unrelatedDb));
            mSession.Repaint(*target);
        });
}
} // namespace nanoq::ui
