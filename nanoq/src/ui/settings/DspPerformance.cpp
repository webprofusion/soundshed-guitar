// DspPerformance.cpp - Settings → DSP Performance's live figures, as the web UI shows them
// (settings-performance.html; views.ts updateDSPPerformancePlot and renderSignalDiagnosticsPanel;
// dspNodeTable.ts): DSP Load, Peak Load and I/O Latency over the section's tabs, and on its
// Diagnostics tab the chain's input and output levels, the load over the last ~20 seconds, and a
// compact table of every node running, by chain and in signal order. The figures come from
// uiclient's DspDiagnostics and are redrawn a few times a second (bound with mLive, so a figure
// that did not change costs nothing); a new set of rows rebuilds the page.

#include "ui/shell/Shell.h"

#include <algorithm>

namespace nanoq::ui
{
namespace
{
using guitarfx::uiclient::DspDiagnosticsGroup;
using guitarfx::uiclient::DspDiagnosticsRow;

/// The groups and row labels: what the table's layout depends on, not its figures.
std::string TableShape(const std::vector<DspDiagnosticsGroup>& table)
{
    std::string shape;

    for (const auto& group : table)
    {
        shape += group.title + '\n';

        for (const auto& row : group.rows)
        {
            shape += '\t' + row.label + '\n';
        }
    }

    return shape;
}

/// The load over time (#performance-plot): a filled line from 0 to at least 100%, gridded at
/// quarters.
class LoadPlot final : public el::element, public Drawn
{
  public:
    LoadPlot(const Theme& theme, const guitarfx::uiclient::DspDiagnostics& diagnostics)
        : mTheme(theme), mDiagnostics(diagnostics)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{120.0f, 96.0f}, {el::full_extent, 96.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto& history = mDiagnostics.LoadHistory();

        auto state = cnv.new_state();
        cnv.begin_path();
        cnv.add_round_rect(b, 6.0f);
        cnv.fill_style(mTheme.Input());
        cnv.fill();

        const double top = std::max(100.0, history.empty() ? 0.0 : *std::max_element(history.begin(), history.end()));
        const auto yOf = [&](double percent) {
            return b.bottom - 4.0f - static_cast<float>(percent / top) * (b.height() - 8.0f);
        };

        cnv.line_width(1.0f);
        cnv.stroke_style(mTheme.PlotGrid());

        for (const double percent : {25.0, 50.0, 75.0})
        {
            cnv.begin_path();
            cnv.move_to(b.left + 4.0f, yOf(percent));
            cnv.line_to(b.right - 4.0f, yOf(percent));
            cnv.stroke();
        }

        if (history.size() < 2)
        {
            return;
        }

        const float step = (b.width() - 8.0f) / static_cast<float>(history.size() - 1);
        const auto trace = [&] {
            cnv.begin_path();

            for (std::size_t i = 0; i < history.size(); ++i)
            {
                const float x = b.left + 4.0f + step * static_cast<float>(i);
                i == 0 ? cnv.move_to(x, yOf(history[i])) : cnv.line_to(x, yOf(history[i]));
            }
        };

        trace();
        cnv.line_to(b.right - 4.0f, b.bottom - 4.0f);
        cnv.line_to(b.left + 4.0f, b.bottom - 4.0f);
        cnv.close_path();
        cnv.fill_style(mTheme.PlotCurve().opacity(0.18f));
        cnv.fill();
        trace();
        cnv.line_width(1.5f);
        cnv.stroke_style(mTheme.PlotCurve());
        cnv.stroke();
    }

    /// Whether the history moved since the last call.
    bool Changed()
    {
        const auto& history = mDiagnostics.LoadHistory();
        const auto seen = std::make_pair(history.size(), history.empty() ? 0.0 : history.back());

        if (seen == mSeen)
        {
            return false;
        }

        mSeen = seen;
        return true;
    }

  private:
    const Theme& mTheme;
    const guitarfx::uiclient::DspDiagnostics& mDiagnostics;
    std::pair<std::size_t, double> mSeen{0, 0.0};
};
} // namespace

void Shell::SyncDspDiagnostics(bool refresh)
{
    const double now = UiSession::Now();
    mDspDiagnostics.Update(mState, now);

    if (!refresh && (mDspFiguresShown.expired() || !mDspDiagnostics.TakeRenderTurn(now)))
    {
        return;
    }

    mDspSnapshot.load = mDspDiagnostics.Load();
    mDspSnapshot.peakLoad = mDspDiagnostics.PeakLoad();
    mDspSnapshot.latency = guitarfx::uiclient::DspDiagnostics::Latency(mState);
    mDspSnapshot.input = mDspDiagnostics.Input(mState);
    mDspSnapshot.output = mDspDiagnostics.Output(mState);
    mDspSnapshot.table = mDspDiagnostics.Table(mState);

    // A node added or removed: the table is laid out again with the page.
    if (!refresh && mSettingsSection == SettingsSection::Dsp && mDspSettingsTab == 0 &&
        TableShape(mDspSnapshot.table) != mDspTableShape)
    {
        Mark(kPage);
    }
}

Ptr Shell::BuildDspFigures()
{
    SyncDspDiagnostics(true);

    const auto tile = [this](const char* label, std::string DspSnapshot::* field, float width) {
        auto l = std::make_shared<Text>(mTheme, label, 9.0f, Weight::SemiBold);
        l->Colour(mTheme.TextMuted());
        auto v = std::make_shared<Text>(mTheme, mDspSnapshot.*field, 16.0f, Weight::SemiBold);
        v->Colour(mTheme.Ink());
        mLive.Bind(v, [this, field] { return mDspSnapshot.*field; }, [](Text& t, const std::string& s) { t.Set(s); });
        return FixedWidth(width, Over(Pad(10, 6, 10, 6, Col({FixedHeight(14, l), FixedHeight(22, v)})),
                                      Rounded(mTheme.Text().opacity(0.05f), 6.0f, std::make_shared<el::element>())));
    };

    auto figures =
        Row({tile("DSP LOAD", &DspSnapshot::load, 120.0f), HGap(8), tile("PEAK LOAD", &DspSnapshot::peakLoad, 120.0f),
             HGap(8), tile("I/O LATENCY", &DspSnapshot::latency, 190.0f), Stretch()});
    mDspFiguresShown = figures;
    return FixedHeight(46, figures);
}

Ptr Shell::BuildDspDiagnostics(float width)
{
    using Row_ = DspDiagnosticsRow;
    std::vector<Ptr> parts;

    // A cell bound to a figure; `align` right for numbers.
    const auto cell = [this](std::function<std::string()> read, float size, bool right, el::color colour) {
        auto t = std::make_shared<Text>(mTheme, read(), size, Weight::Regular);
        t->Colour(colour).Alignment(right ? HAlign::Right : HAlign::Left);
        mLive.Bind(t, std::move(read), [](Text& text, const std::string& s) { text.Set(s); });
        return t;
    };
    const auto heading = [this](const std::string& text, bool right) {
        auto t = std::make_shared<Text>(mTheme, Upper(text), 9.5f, Weight::SemiBold);
        t->Colour(mTheme.TextSecondary()).Alignment(right ? HAlign::Right : HAlign::Left);
        return t;
    };
    // The clip light: green, or red with the count.
    const auto clipCell = [this](std::function<std::pair<bool, int>()> read) {
        auto t = std::make_shared<Text>(mTheme, "", 11.0f, Weight::SemiBold);
        t->Alignment(HAlign::Right);
        mLive.Bind(t, std::move(read), [this](Text& text, const std::pair<bool, int>& clip) {
            text.Set(clip.first ? "\xE2\x97\x8F " + std::to_string(clip.second) : "\xE2\x97\x8F");
            text.Colour(clip.first ? mTheme.Error() : mTheme.Success());
        });
        return t;
    };
    const auto rule = [this] { return FixedHeight(1, Fill(mTheme.Border(), std::make_shared<el::element>())); };

    // ── The chain's input and output (.signal-diagnostics-summary), as two rows ──
    {
        const auto summaryRow = [&](const char* name, guitarfx::uiclient::DspLevelSummary DspSnapshot::* which) {
            const auto field = [this, which](std::string guitarfx::uiclient::DspLevelSummary::* f) {
                return [this, which, f] { return mDspSnapshot.*which.*f; };
            };
            auto label = std::make_shared<Text>(mTheme, name, 11.5f, Weight::SemiBold);
            label->Colour(mTheme.Ink());
            return FixedHeight(22,
                               Row({Middle(label),
                                    FixedWidth(96, Middle(cell(field(&guitarfx::uiclient::DspLevelSummary::peak), 11.5f,
                                                               true, mTheme.Ink()))),
                                    FixedWidth(96, Middle(cell(field(&guitarfx::uiclient::DspLevelSummary::rms), 11.5f,
                                                               true, mTheme.Ink()))),
                                    FixedWidth(86, Middle(cell(field(&guitarfx::uiclient::DspLevelSummary::headroom),
                                                               11.5f, true, mTheme.Ink()))),
                                    FixedWidth(56, Middle(clipCell([this, which] {
                                                   const auto& s = mDspSnapshot.*which;
                                                   return std::make_pair(s.clipped, s.clipCount);
                                               })))}));
        };

        auto t = std::make_shared<Text>(mTheme, "SIGNAL DIAGNOSTICS", 13.0f, Weight::SemiBold);
        parts.push_back(FixedHeight(24, Middle(t)));
        parts.push_back(FixedHeight(
            18, Row({Middle(heading("", false)), FixedWidth(96, Middle(heading("Peak", true))),
                     FixedWidth(96, Middle(heading("RMS", true))), FixedWidth(86, Middle(heading("Headroom", true))),
                     FixedWidth(56, Middle(heading("Clip", true)))})));
        parts.push_back(rule());
        parts.push_back(summaryRow("Input", &DspSnapshot::input));
        parts.push_back(summaryRow("Output", &DspSnapshot::output));
        parts.push_back(VGap(12));
    }

    // ── The load over time ──
    {
        auto plot = std::make_shared<LoadPlot>(mTheme, mDspDiagnostics);
        mLive.Feed(plot, [](LoadPlot& p) { return p.Changed(); });
        parts.push_back(FixedHeight(96, plot));
        parts.push_back(VGap(14));
    }

    // ── Per node: narrow pages leave out the share and the latency ──
    const bool narrow = width < 460.0f;

    struct Column
    {
        const char* title;
        std::string Row_::* field;
        float width;
    };

    std::vector<Column> columns = {{"Ch", &Row_::channels, 30.0f}, {"\xC2\xB5s", &Row_::time, 54.0f}};

    if (!narrow)
    {
        columns.push_back({"%", &Row_::share, 44.0f});
        columns.push_back({"Latency", &Row_::latency, 70.0f});
    }

    columns.push_back({"Peak", &Row_::peak, 54.0f});

    auto title = std::make_shared<Text>(mTheme, "PER-NODE DIAGNOSTICS", 13.0f, Weight::SemiBold);
    parts.push_back(FixedHeight(24, Middle(title)));

    {
        std::vector<Ptr> header = {Middle(heading("Node", false))};

        for (const auto& c : columns)
        {
            header.push_back(FixedWidth(c.width, Middle(heading(c.title, true))));
        }

        header.push_back(FixedWidth(52, Middle(heading("Clip", true))));
        parts.push_back(FixedHeight(18, Pad(6, 0, 6, 0, Row(header))));
        parts.push_back(rule());
    }

    mDspTableShape = TableShape(mDspSnapshot.table);

    for (std::size_t g = 0; g < mDspSnapshot.table.size(); ++g)
    {
        const auto& group = mDspSnapshot.table[g];

        if (!group.title.empty())
        {
            parts.push_back(FixedHeight(22, Pad(6, 6, 6, 0, heading(group.title, false))));
        }

        for (std::size_t r = 0; r < group.rows.size(); ++r)
        {
            const auto rowValue = [this, g, r](std::string Row_::* field) {
                return [this, g, r, field] {
                    const auto& table = mDspSnapshot.table;
                    return g < table.size() && r < table[g].rows.size() ? table[g].rows[r].*field : std::string{};
                };
            };
            std::vector<Ptr> cells = {Middle(cell(rowValue(&Row_::label), 11.0f, false, mTheme.Ink()))};

            for (const auto& c : columns)
            {
                const auto colour = c.field == &Row_::channels && group.rows[r].stereo ? mTheme.Accent() : mTheme.Ink();
                cells.push_back(FixedWidth(c.width, Middle(cell(rowValue(c.field), 11.0f, true, colour))));
            }

            cells.push_back(FixedWidth(52, Middle(clipCell([this, g, r] {
                                           const auto& table = mDspSnapshot.table;

                                           if (g >= table.size() || r >= table[g].rows.size())
                                           {
                                               return std::make_pair(false, 0);
                                           }

                                           return std::make_pair(table[g].rows[r].clipped, table[g].rows[r].clipCount);
                                       }))));

            auto line = FixedHeight(18, Pad(6, 1, 6, 1, Row(cells)));
            // Every other row shaded, as .dsp-node-row.odd.
            parts.push_back(r % 2 == 1 ? Over(line, Fill(mTheme.Text().opacity(0.04f), std::make_shared<el::element>()))
                                       : line);
        }
    }

    return Col(parts);
}
} // namespace nanoq::ui
