// LiveSections.cpp - effect editor sections fed by the engine's live analysis: the 3D Spatial
// node's panner (spatialPosition) and the Signal Analyzer's readouts, spectrogram and Bark bands
// (sldA), as the web UI's params panel shows them.

#include "ui/Drawn.h"
#include "ui/shell/Shell.h"

#include "dsp/EffectGuids.h"
#include "ui/effects/SpatialPanner.h"
#include "uiclient/Analysis.h"

#include <algorithm>
#include <cmath>
#include <deque>

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

/// The Signal Analyzer: a grid of readouts over a scrolling spectrogram (newest at the right)
/// and the Bark bands as bars.
class AnalyzerPanel final : public el::element, public Drawn
{
  public:
    static constexpr std::size_t kHistory = 160; // ANALYZER_SPECTROGRAM_HISTORY_FRAMES

    explicit AnalyzerPanel(const Theme& theme) : mTheme(theme)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{300.0f, 260.0f}, {el::full_extent, el::full_extent}};
    }

    /// Takes a frame; returns true when it was a new one.
    bool Set(const guitarfx::uiclient::AnalyzerFrame* frame, const guitarfx::uiclient::AnalyzerRanges& ranges,
             int configuredChannels)
    {
        mReadouts = guitarfx::uiclient::AnalyzerReadouts(frame, configuredChannels);
        mRanges = ranges;

        if (frame == nullptr)
        {
            return false;
        }

        if (frame->timeMs == mLastTime && !mHistory.empty())
        {
            return false;
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
        const auto labelFont = Font(Weight::SemiBold).size(9.0f);
        const auto valueFont = Font(Weight::SemiBold).size(12.0f);

        // Readouts: six to a row.
        constexpr int kColumns = 6;
        constexpr float kRow = 40.0f;
        const float cell = b.width() / kColumns;

        for (std::size_t i = 0; i < mReadouts.size(); ++i)
        {
            const float x = b.left + cell * static_cast<float>(i % kColumns);
            const float y = b.top + kRow * static_cast<float>(i / kColumns);
            const auto& r = mReadouts[i];
            FillText(cnv, Ellipsize(cnv, r.label, labelFont, cell - 8.0f), labelFont, mTheme.TextMuted(),
                     el::rect{x + 4.0f, y, x + cell - 4.0f, y + 12.0f}, HAlign::Left);
            FillText(cnv, Ellipsize(cnv, r.value, valueFont, cell - 8.0f), valueFont, mTheme.Ink(),
                     el::rect{x + 4.0f, y + 13.0f, x + cell - 4.0f, y + 28.0f}, HAlign::Left);

            if (!r.hint.empty())
            {
                FillText(cnv, r.hint, Font().size(9.0f), mTheme.TextSecondary(),
                         el::rect{x + 4.0f, y + 28.0f, x + cell - 4.0f, y + 39.0f}, HAlign::Left);
            }
        }

        const float top = b.top + 2.0f * kRow + 6.0f;
        const float barkHeight = 48.0f;
        const el::rect spec{b.left, top, b.right, b.bottom - barkHeight - 18.0f};
        const el::rect bark{b.left, b.bottom - barkHeight, b.right, b.bottom};
        const el::color well = el::rgba(8, 10, 18, 235);

        cnv.fill_style(well);
        cnv.fill_rect(spec);

        if (!mHistory.empty() && !mHistory.back().empty())
        {
            const auto [minDb, maxDb, minHz, maxHz] = mRanges.spectrogram;
            const double range = std::max(1.0, maxDb - minDb);
            const std::size_t bins = mHistory.back().size();
            const float rowHeight = spec.height() / static_cast<float>(bins);
            const float columnWidth = std::max(1.0f, spec.width() / static_cast<float>(kHistory));
            const std::size_t start = kHistory > mHistory.size() ? kHistory - mHistory.size() : 0;

            for (std::size_t c = 0; c < mHistory.size(); ++c)
            {
                const auto& frame = mHistory[c];
                const float x = spec.left + columnWidth * static_cast<float>(start + c);

                for (std::size_t y = 0; y < frame.size() && y < bins; ++y)
                {
                    const double norm = std::clamp((frame[y] - minDb) / range, 0.0, 1.0);

                    if (norm <= 0.001)
                    {
                        continue;
                    }

                    cnv.fill_style(Hsl(230.0 - std::round(norm * 190.0), (76.0 + std::round(norm * 18.0)) / 100.0,
                                       (12.0 + std::round(norm * 56.0)) / 100.0));
                    cnv.fill_rect(el::rect{x, spec.bottom - static_cast<float>(y + 1) * rowHeight, x + columnWidth,
                                           spec.bottom - static_cast<float>(y) * rowHeight + 0.5f});
                }
            }
        }

        FillText(cnv, "BARK PERCEPTION", labelFont, mTheme.TextMuted(),
                 el::rect{b.left, bark.top - 15.0f, b.right, bark.top - 3.0f}, HAlign::Left);
        cnv.fill_style(well);
        cnv.fill_rect(bark);

        if (!mBark.empty())
        {
            const auto [minDb, maxDb, minHz, maxHz] = mRanges.bark;
            const double range = std::max(1.0, maxDb - minDb);
            const float gap = 1.0f;
            const float width = std::max(1.0f, (bark.width() - gap * static_cast<float>(mBark.size() - 1)) /
                                                   static_cast<float>(mBark.size()));

            for (std::size_t i = 0; i < mBark.size(); ++i)
            {
                const double norm = std::clamp((mBark[i] - minDb) / range, 0.0, 1.0);
                const float x = bark.left + static_cast<float>(i) * (width + gap);
                cnv.fill_style(Hsl(232.0 - std::round(norm * 168.0), (76.0 + std::round(norm * 14.0)) / 100.0,
                                   (14.0 + std::round(norm * 58.0)) / 100.0));
                cnv.fill_rect(
                    el::rect{x, bark.bottom - static_cast<float>(norm) * bark.height(), x + width, bark.bottom});
            }
        }
    }

  private:
    const Theme& mTheme;
    std::vector<guitarfx::uiclient::AnalyzerReadout> mReadouts = guitarfx::uiclient::AnalyzerReadouts(nullptr, 0);
    guitarfx::uiclient::AnalyzerRanges mRanges;
    std::deque<std::vector<double>> mHistory;
    std::vector<double> mBark;
    double mLastTime = -1.0;
};
} // namespace

Ptr Shell::BuildLiveSection(const guitarfx::GraphNode& node, float& height)
{
    mSpatialPanner.reset();
    mAnalyzerPanel.reset();
    mLiveNodeId = node.id;

    if (node.type == guids::kSpatial3D)
    {
        auto panner = std::make_shared<SpatialPanner>(mTheme);
        const std::string nodeId = node.id;
        panner->onChange = [this, nodeId](const SpatialPoint& p, bool commit) {
            mSession.Commands().SetNodeParam(nodeId, "azimuth", p.azimuth);
            mSession.Commands().SetNodeParam(nodeId, "elevation", p.elevation);
            mSession.Commands().SetNodeParam(nodeId, "distance", p.distance);
            NoteChainEdit();

            if (commit)
            {
                Mark(kValues);
            }
        };
        mSpatialPanner = panner;
        SyncLiveSection();
        height = 200.0f;
        // The radar is as big as the height allows; wider only parts it from its height arc.
        return Row({Stretch(), FixedWidth(440, panner), Stretch()});
    }

    if (node.type == guids::kInputAnalyzer)
    {
        auto panel = std::make_shared<AnalyzerPanel>(mTheme);
        mAnalyzerPanel = panel;
        SyncLiveSection();
        height = 340.0f;
        return panel;
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

    if (const auto panner = std::static_pointer_cast<SpatialPanner>(mSpatialPanner.lock()))
    {
        const auto param = [node](const char* k, double fallback) {
            const auto it = node->params.find(k);
            return it != node->params.end() ? it->second : fallback;
        };
        panner->SetPosition({param("azimuth", 0.0), param("elevation", 0.0), param("distance", 1.5)});
        panner->SetSpeakerMode(param("listenMode", 0.0) >= 0.5);
        const auto& spatial = mState.analysis.spatial;
        auto live = spatial.find(key);

        if (live == spatial.end())
        {
            live = spatial.find("::" + node->id);
        }

        panner->SetLive(live != spatial.end() ? &live->second : nullptr);
        mSession.Repaint(*panner);
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
