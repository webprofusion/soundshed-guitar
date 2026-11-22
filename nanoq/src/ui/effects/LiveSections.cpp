// LiveSections.cpp - effect editor sections fed by the engine's live analysis: the 3D Spatial
// node's panner (spatialPosition) and the Signal Analyzer's readouts, spectrogram and Bark bands
// (sldA), as the web UI's params panel shows them.

#include "ui/Drawn.h"
#include "ui/shell/Shell.h"

#include "dsp/EffectGuids.h"
#include "dsp/FiniteCheck.h"
#include "ui/effects/SpatialPanner.h"
#include "ui/widgets/ControlSkin.h"
#include "ui/widgets/Graphics.h"
#include "ui/widgets/Panels.h"
#include "uiclient/Analysis.h"
#include "uiclient/EffectLayouts.h"
#include "uiclient/NodeLabels.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <deque>
#include <string>
#include <vector>

namespace nanoq::ui
{
namespace guids = guitarfx::EffectGuids;

namespace
{
/// The web UI's analyzer colours: hsl, rising from deep blue through to warm as the level does.
el::color Hsl(double hue, double saturation, double lightness)
{
    const double c = (1.0 - std::abs(2.0 * lightness - 1.0)) * saturation;
    const double hp = std::fmod(hue / 60.0, 6.0);
    const double x = c * (1.0 - std::abs(std::fmod(hp, 2.0) - 1.0));
    double r = 0, g = 0, b = 0;

    if (hp < 1)
    {
        r = c, g = x;
    }
    else if (hp < 2)
    {
        r = x, g = c;
    }
    else if (hp < 3)
    {
        g = c, b = x;
    }
    else if (hp < 4)
    {
        g = x, b = c;
    }
    else if (hp < 5)
    {
        r = x, b = c;
    }
    else
    {
        r = c, b = x;
    }

    const double m = lightness - c / 2.0;
    return el::color{static_cast<float>(r + m), static_cast<float>(g + m), static_cast<float>(b + m), 1.0f};
}

// The analyzer's fixed colours (params-panel.css): the same in every theme, as the web UI's.
const ControlBox kTileBox{0xB8080C14, 0x9E0F121C, 1.0f, 0x1AAAB7D6, 10.0f, 0.0f, 0, 0, 0.0f, 0.0f, 0, 0};
const ControlBox kWellBox{0xEB090C13, 0xDB0B0E16, 1.0f, 0x24AAB7D6, 12.0f, 0.0f, 0, 0, 0.0f, 0.0f, 0x0AFFFFFF, 0};
const el::color kCanvasFill = el::color{8.0f / 255.0f, 10.0f / 255.0f, 18.0f / 255.0f, 0.92f};

// Its measures: the lines of its 10 and 12 px text (Inter's normal line height), the grid's
// gap and least column, a tile's border and padding, and the wells'.
constexpr float kLine10 = 12.0f;
constexpr float kLine12 = 14.5f;
constexpr float kStackGap = 12.0f;
constexpr float kGridGap = 8.0f;
constexpr float kTileMin = 130.0f;
constexpr float kTileInsetX = 11.0f;
constexpr float kTileInsetY = 9.0f;
constexpr float kWellInset = 9.0f;
constexpr float kSpectrogramHeight = 170.0f;
constexpr float kBarkHeight = 92.0f;
constexpr float kSpectrogramWell = 2.0f * kWellInset + kSpectrogramHeight;
constexpr float kBarkWell = 2.0f * kWellInset + kLine10 + 6.0f + kBarkHeight;

/// A tile's label (.input-analyzer-label): 10 px capitals spaced 0.08 em, in the page's face.
ControlText LabelText(const Theme& theme, std::uint32_t colour)
{
    const auto& body = theme.Controls().body;
    return ControlText{colour, 10.0f, body.weight, 0.08f, true, body.family};
}

/// `text` broken at spaces into lines of `width` in `skin`, as the browser wraps a label.
std::vector<std::string> WrapControlText(const std::string& text, const ControlText& skin, float width)
{
    std::vector<std::string> lines;
    std::string line;
    std::size_t i = 0;

    while (i < text.size())
    {
        auto end = text.find(' ', i);

        if (end == std::string::npos)
        {
            end = text.size();
        }

        const auto word = text.substr(i, end - i);
        const auto candidate = line.empty() ? word : line + " " + word;

        if (!line.empty() && MeasureControlText(candidate, skin) > width)
        {
            lines.push_back(line);
            line = word;
        }
        else
        {
            line = candidate;
        }

        i = end + 1;
    }

    if (!line.empty() || lines.empty())
    {
        lines.push_back(line);
    }

    return lines;
}

/// The time of day `ms` (since the epoch) falls at, in this computer's zone.
std::string LocalTime(double ms)
{
    const auto seconds = static_cast<std::time_t>(ms / 1000.0);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%X", &local);
    return text;
}

/// The Signal Analyzer (.input-analyzer-panel's content): the header with when the last frame
/// came, a grid of readout tiles, the spectrogram (one pixel per frame from the left, scrolling
/// once it is full) and the Bark bands as bars, each in its well.
class AnalyzerPanel final : public el::element, public Drawn
{
  public:
    static constexpr std::size_t kHistory = 160; // ANALYZER_SPECTROGRAM_HISTORY_FRAMES

    explicit AnalyzerPanel(const Theme& theme) : mTheme(theme)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{200.0f, 200.0f}, {el::full_extent, el::full_extent}};
    }

    /// The content's height at `width`: its tiles laid out as the web UI's grid lays them.
    static float ContentHeight(const Theme& theme, float width)
    {
        return kLine10 + kStackGap + LayTiles(theme, guitarfx::uiclient::AnalyzerReadouts(nullptr, 0), width).height +
               kStackGap + kSpectrogramWell + kStackGap + kBarkWell;
    }

    /// Takes a frame, or nothing when there is none; returns true when there is something new
    /// to draw.
    bool Set(const guitarfx::uiclient::AnalyzerFrame* frame, const guitarfx::uiclient::AnalyzerRanges& ranges,
             int configuredChannels)
    {
        mReadouts = guitarfx::uiclient::AnalyzerReadouts(frame, configuredChannels);
        mRanges = ranges;

        if (frame == nullptr)
        {
            // The frame has gone: the wells are drawn empty and the readouts go back to dashes.
            // The history is kept, as the web UI keeps it, and carries on when frames return.
            const bool had = mHasFrame;
            mHasFrame = false;
            return had;
        }

        const bool returned = !mHasFrame;
        mHasFrame = true;

        if (frame->timeMs == mLastTime && !mHistory.empty())
        {
            return returned;
        }

        mLastTime = frame->timeMs;
        mHistory.push_back(frame->spectrogramDb);

        while (mHistory.size() > kHistory)
        {
            mHistory.pop_front();
        }

        mBark = frame->barkDb;
        return true;
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;

        // The header: the title, and when the last frame came on the right.
        const auto& body = mTheme.Controls().body;
        const ControlText title{0xBDDCE3F3, 10.0f, 800, 0.12f, true, body.family};
        const auto statusFont = Font().size(10.0f);
        const std::string status = !mHasFrame      ? "Waiting for live analyzer data\xE2\x80\xA6"
                                   : mLastTime > 0.0 ? "Updated: " + LocalTime(mLastTime)
                                                     : "Updated: live";
        const float statusWidth = std::min(b.width() * 0.6f, MeasureTextWidth(status, statusFont));
        FillText(cnv, Ellipsize(cnv, status, statusFont, statusWidth), statusFont, el::rgba(201, 209, 225, 173),
                 el::rect{b.right - statusWidth, b.top, b.right, b.top + kLine10}, HAlign::Right);
        DrawControlText(cnv, "Signal Analyzer", title,
                        el::rect{b.left, b.top, b.right - statusWidth - 8.0f, b.top + kLine10}, HAlign::Left);

        // The readout tiles.
        const auto label = LabelText(mTheme, 0xA8C2CBDE);
        const auto valueFont = FaceFor(body.family, 700).size(12.0f);
        const auto hintFont = Font().size(10.0f);
        const float gridTop = b.top + kLine10 + kStackGap;
        const auto tiles = LayTiles(mTheme, mReadouts, b.width());
        float rowTop = gridTop;

        for (std::size_t row = 0; row < tiles.rows.size(); ++row)
        {
            for (int col = 0; col < tiles.columns; ++col)
            {
                const std::size_t i = row * static_cast<std::size_t>(tiles.columns) + static_cast<std::size_t>(col);

                if (i >= mReadouts.size())
                {
                    break;
                }

                const auto& r = mReadouts[i];
                const float x = b.left + static_cast<float>(col) * (tiles.width + kGridGap);
                const el::rect tile{x, rowTop, x + tiles.width, rowTop + tiles.rows[row]};
                DrawControlBox(cnv, tile, kTileBox);

                const float left = tile.left + kTileInsetX, right = tile.right - kTileInsetX;
                float y = tile.top + kTileInsetY;

                for (const auto& line : WrapControlText(r.label, label, right - left))
                {
                    DrawControlText(cnv, line, label, el::rect{left, y, right, y + kLine10}, HAlign::Left);
                    y += kLine10;
                }

                y += 4.0f;
                FillText(cnv, Ellipsize(cnv, r.value, valueFont, right - left), valueFont,
                         el::rgba(239, 244, 255, 240), el::rect{left, y, right, y + kLine12}, HAlign::Left);

                if (HasHint(r))
                {
                    y += kLine12 + 4.0f;
                    FillText(cnv, Ellipsize(cnv, r.hint, hintFont, right - left), hintFont,
                             el::rgba(194, 203, 222, 168), el::rect{left, y, right, y + kLine10}, HAlign::Left);
                }
            }

            rowTop += tiles.rows[row] + kGridGap;
        }

        // The spectrogram's well, and the Bark bands' with its title.
        const float specTop = gridTop + tiles.height + kStackGap;
        const el::rect specWell{b.left, specTop, b.right, specTop + kSpectrogramWell};
        DrawControlBox(cnv, specWell, kWellBox);
        DrawSpectrogram(cnv, specWell.inset(kWellInset, kWellInset));

        const float barkTop = specWell.bottom + kStackGap;
        const el::rect barkWell{b.left, barkTop, b.right, barkTop + kBarkWell};
        DrawControlBox(cnv, barkWell, kWellBox);
        const float inner = barkWell.top + kWellInset;
        DrawControlText(cnv, "Bark Perception", LabelText(mTheme, 0xB8C2CBDE),
                        el::rect{barkWell.left + kWellInset, inner, barkWell.right - kWellInset, inner + kLine10},
                        HAlign::Left);
        DrawBark(cnv, el::rect{barkWell.left + kWellInset, inner + kLine10 + 6.0f, barkWell.right - kWellInset,
                               inner + kLine10 + 6.0f + kBarkHeight});
    }

  private:
    struct Tiles
    {
        int columns = 1;
        float width = 0.0f;
        std::vector<float> rows; // each row's height
        float height = 0.0f;
    };

    /// Only the Channels tile keeps a line for its hint, filled or not.
    static bool HasHint(const guitarfx::uiclient::AnalyzerReadout& r)
    {
        return r.label == "Channels";
    }

    /// The grid (repeat(auto-fit, minmax(130px, 1fr)), gap 8): as many columns as fit, never
    /// more than there are tiles, and each row as tall as its tallest tile.
    static Tiles LayTiles(const Theme& theme, const std::vector<guitarfx::uiclient::AnalyzerReadout>& readouts,
                          float width)
    {
        Tiles t;
        const int count = std::max(1, static_cast<int>(readouts.size()));
        t.columns = std::clamp(static_cast<int>(std::floor((width + kGridGap) / (kTileMin + kGridGap))), 1, count);
        t.width = (width - kGridGap * static_cast<float>(t.columns - 1)) / static_cast<float>(t.columns);
        const auto label = LabelText(theme, 0);

        for (std::size_t i = 0; i < readouts.size(); ++i)
        {
            const auto lines = WrapControlText(readouts[i].label, label, t.width - 2.0f * kTileInsetX).size();
            const float h = 2.0f * kTileInsetY + kLine10 * static_cast<float>(lines) + 4.0f + kLine12 +
                            (HasHint(readouts[i]) ? 4.0f + kLine10 : 0.0f);

            if (i % static_cast<std::size_t>(t.columns) == 0)
            {
                t.rows.push_back(h);
            }
            else
            {
                t.rows.back() = std::max(t.rows.back(), h);
            }
        }

        for (const float h : t.rows)
        {
            t.height += h;
        }

        t.height += kGridGap * static_cast<float>(std::max<std::size_t>(t.rows.size(), 1) - 1);
        return t;
    }

    /// A range's low and high, or the web UI's own when it has none.
    static std::pair<double, double> Range(const std::array<double, 4>& range, double low)
    {
        return {guitarfx::IsFinite(range[0]) ? range[0] : low, guitarfx::IsFinite(range[1]) ? range[1] : 0.0};
    }

    /// The spectrogram as the web UI draws it: a pixel's column per frame from the left, the
    /// oldest dropped once it is full; the bins up the height, coloured by level.
    void DrawSpectrogram(cycfi::artist::canvas& cnv, const el::rect& r) const
    {
        auto state = cnv.new_state();
        cnv.begin_path();
        cnv.add_round_rect(r, 8.0f);
        cnv.clip();
        cnv.fill_style(kCanvasFill);
        cnv.fill_rect(r);

        if (!mHasFrame || mHistory.empty() || mHistory.front().empty())
        {
            return;
        }

        const auto [minDb, maxDb] = Range(mRanges.spectrogram, -120.0);
        const double range = std::max(1.0, maxDb - minDb);
        const float width = std::max(1.0f, std::floor(r.width()));
        const float height = std::max(1.0f, std::floor(r.height()));
        const std::size_t bins = mHistory.front().size();
        const std::size_t columns = std::min(static_cast<std::size_t>(width), mHistory.size());
        const std::size_t start = mHistory.size() - columns;
        const float rowHeight = height / static_cast<float>(bins);

        for (std::size_t x = 0; x < columns; ++x)
        {
            const auto& frame = mHistory[start + x];

            for (std::size_t y = 0; y < bins && y < frame.size(); ++y)
            {
                const double norm = std::clamp((frame[y] - minDb) / range, 0.0, 1.0);

                if (norm <= 0.001)
                {
                    continue;
                }

                cnv.fill_style(Hsl(230.0 - std::round(norm * 190.0), (76.0 + std::round(norm * 18.0)) / 100.0,
                                   (12.0 + std::round(norm * 56.0)) / 100.0));
                cnv.fill_rect(r.left + static_cast<float>(x), r.top + height - static_cast<float>(y + 1) * rowHeight,
                              1.0f, std::max(1.0f, rowHeight + 0.5f));
            }
        }
    }

    /// The Bark bands as bars across the width, a pixel apart, each as tall as its level.
    void DrawBark(cycfi::artist::canvas& cnv, const el::rect& r) const
    {
        auto state = cnv.new_state();
        cnv.begin_path();
        cnv.add_round_rect(r, 8.0f);
        cnv.clip();
        cnv.fill_style(kCanvasFill);
        cnv.fill_rect(r);

        if (!mHasFrame || mBark.empty())
        {
            return;
        }

        const auto [minDb, maxDb] = Range(mRanges.bark, -96.0);
        const double range = std::max(1.0, maxDb - minDb);
        const float width = std::max(1.0f, std::floor(r.width()));
        const float height = std::max(1.0f, std::floor(r.height()));
        const float count = static_cast<float>(mBark.size());
        const float barWidth = std::max(1.0f, (width - (count - 1.0f)) / count);

        for (std::size_t i = 0; i < mBark.size(); ++i)
        {
            const double norm = std::clamp((mBark[i] - minDb) / range, 0.0, 1.0);
            const float barHeight = static_cast<float>(norm) * height;
            cnv.fill_style(Hsl(232.0 - std::round(norm * 168.0), (76.0 + std::round(norm * 14.0)) / 100.0,
                               (14.0 + std::round(norm * 58.0)) / 100.0));
            cnv.fill_rect(r.left + static_cast<float>(i) * (barWidth + 1.0f), r.top + height - barHeight, barWidth,
                          barHeight);
        }
    }

    const Theme& mTheme;
    std::vector<guitarfx::uiclient::AnalyzerReadout> mReadouts = guitarfx::uiclient::AnalyzerReadouts(nullptr, 0);
    guitarfx::uiclient::AnalyzerRanges mRanges;
    std::deque<std::vector<double>> mHistory;
    std::vector<double> mBark;
    double mLastTime = -1.0;
    bool mHasFrame = false;
};
} // namespace

Ptr Shell::BuildLiveSection(const guitarfx::GraphNode& node, float& height)
{
    mSpatialPanner.reset();
    mAnalyzerPanel.reset();
    mLiveNodeId = node.id;

    // The width the section gets, as the effect page works it out (EffectPage.cpp): these nodes
    // take the whole stage, less the panel's and the section's padding, and the equipment
    // artwork beside the section when that shows.
    const auto sectionWidth = [this, &node] {
        const float stage = StageWidth();
        float width = std::max(448.0f, stage - 20.0f) - 2.0f * (10.0f + 14.0f);

        if (PlayStageShown() && stage >= 860.0f && guitarfx::uiclient::ResolveLayoutForNode(mState, node) == nullptr &&
            mImages.Find(mPresentation.EquipmentImage(node.type, guitarfx::uiclient::NodeCategory(mState, node))) !=
                nullptr)
        {
            width -= 300.0f + 12.0f;
        }

        return width;
    };

    if (node.type == guids::kSpatial3D)
    {
        // The panner in its card (.spatial-visualizer), the card as wide as the section.
        auto card = std::make_shared<SpatialCard>(mTheme);
        const std::string nodeId = node.id;
        card->Panner().onChange = [this, nodeId](const SpatialPoint& p, bool commit) {
            mSession.Commands().SetNodeParam(nodeId, "azimuth", p.azimuth);
            mSession.Commands().SetNodeParam(nodeId, "elevation", p.elevation);
            mSession.Commands().SetNodeParam(nodeId, "distance", p.distance);
            NoteChainEdit();

            if (commit)
            {
                Mark(kValues);
            }
        };
        mSpatialPanner = card;
        SyncLiveSection();
        height = SpatialCard::HeightFor(mTheme, sectionWidth());
        const float below = height - SpatialCard::kPannerTop - SpatialPanner::kHeight;
        return Over(Pad(SpatialCard::kSide, SpatialCard::kPannerTop, SpatialCard::kSide, below, card->PannerPtr()),
                    card);
    }

    if (node.type == guids::kInputAnalyzer)
    {
        // In the effect view's inset box (.default-effect-shell .input-analyzer-panel: a 1 px
        // border and 16 px of padding), as tall as its tiles' rows need at this width.
        constexpr float kInset = 17.0f;
        auto panel = std::make_shared<AnalyzerPanel>(mTheme);
        mAnalyzerPanel = panel;
        SyncLiveSection();
        height = 2.0f * kInset + AnalyzerPanel::ContentHeight(mTheme, sectionWidth() - 2.0f * kInset);
        return Over(Pad(kInset, kInset, kInset, kInset, panel), std::make_shared<InsetBox>(mTheme));
    }

    height = 0.0f;
    return nullptr;
}

void Shell::SyncLiveSection()
{
    const auto* node = mState.activePreset ? mState.activePreset->graph.FindNode(mLiveNodeId) : nullptr;

    if (node == nullptr)
    {
        return;
    }

    const std::string key = mState.activePresetId + "::" + node->id;

    if (const auto card = std::static_pointer_cast<SpatialCard>(mSpatialPanner.lock()))
    {
        auto& panner = card->Panner();
        const auto param = [node](const char* k, double fallback) {
            const auto it = node->params.find(k);
            return it != node->params.end() ? it->second : fallback;
        };
        panner.SetAnchor({param("azimuth", 0.0), param("elevation", 0.0), param("distance", 1.5)});
        const bool modeChanged = panner.SetSpeakerMode(param("listenMode", 0.0) >= 0.5);
        const auto& spatial = mState.analysis.spatial;
        auto live = spatial.find(key);

        if (live == spatial.end())
        {
            live = spatial.find("::" + node->id);
        }

        panner.SetLive(live != spatial.end() ? &live->second : nullptr);

        // The card's hint follows the listening mode; otherwise only the panner changes.
        if (modeChanged)
        {
            mSession.Repaint(*card);
        }
        else
        {
            mSession.Repaint(panner);
        }
    }

    if (const auto panel = std::static_pointer_cast<AnalyzerPanel>(mAnalyzerPanel.lock()))
    {
        const auto& analyzers = mState.analysis.analyzers;
        const auto frame = analyzers.find(node->id);
        const auto levels = mState.telemetry.nodes.find(key);
        const int channels = levels != mState.telemetry.nodes.end() ? levels->second.channelCount : 0;

        if (panel->Set(frame != analyzers.end() ? &frame->second : nullptr, mState.analysis.analyzerRanges, channels))
        {
            mSession.Repaint(*panel);
        }
    }
}
} // namespace nanoq::ui
