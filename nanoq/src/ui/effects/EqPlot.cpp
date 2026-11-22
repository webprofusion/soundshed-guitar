// EqPlot.cpp - the response curves: the parametric and graphic EQ nodes' and the Simple
// Cabinet's in the effect editor, and the Global EQ's in its dialog (the web UI's
// signalPath/paramsPanel/eq.ts, cabResponse.ts and eqPanel.ts), with the cabinet's IR tools and
// Pickup Match's design blocks (pickupResponse.ts). The graphic EQ's sliders are GraphicEq.cpp's.
//
// The plot (ResponsePlot) draws what the engine reports for the EQ's parameters, asked again
// whenever they change; a parametric EQ's bands get handles that set its parameters as the
// knobs do. Behind the curve is the spectrum of the EQ's input, a lease Analysis renews while a
// plot is on screen. The engine keeps one spectrum watch, so a dialog's plot takes it from the
// effect editor's while it is open.

#include "ui/shell/Shell.h"

#include "dsp/EffectGuids.h"
#include "uiclient/Analysis.h"
#include "uiclient/ParamFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>

namespace nanoq::ui
{
namespace guids = guitarfx::EffectGuids;
using guitarfx::uiclient::EffectParamInfo;
using guitarfx::uiclient::NodeAddress;

namespace
{
/// The parametric EQ's bands as its parameters name them (ParametricEQEffect).
struct BandKeys
{
    const char* prefix;
    const char* label;
    const char* shape; // the shelf/bell switch, for the outer bands
};

constexpr BandKeys kParametricBands[] = {{"low", "Low", "lowShape"},
                                         {"lowMid", "Low Mid", nullptr},
                                         {"highMid", "High Mid", nullptr},
                                         {"high", "High", "highShape"}};

const EffectParamInfo* FindParam(const guitarfx::uiclient::EffectTypeInfo* type, const std::string& key)
{
    if (type == nullptr)
    {
        return nullptr;
    }

    for (const auto& param : type->parameters)
    {
        if (param.key == key)
        {
            return &param;
        }
    }

    return nullptr;
}

double ParamOr(const std::map<std::string, double>& params, const EffectParamInfo* info, const std::string& key,
               double fallback)
{
    if (const auto it = params.find(key); it != params.end() && std::isfinite(it->second))
    {
        return it->second;
    }

    return info != nullptr ? info->defaultValue : fallback;
}
} // namespace

bool Shell::IsEqPlotType(const std::string& type)
{
    return type == guids::kEqParametric || type == guids::kEqGraphic || type == guids::kCabSimple ||
           type == guids::kPickupMatch;
}

Ptr Shell::MakeEqPlot(EqPlotSource source, bool overlay)
{
    auto plot = std::make_shared<ResponsePlot>(mTheme);

    // The web UI's EQ plots: +-18 dB, a line every 9 dB. A cabinet lifts its low resonance by up
    // to ~15 dB and rolls the top off by far more, so its axis leans low.
    // In the effect editor, each in the web UI's card with its title and range over it.
    if (source.effectType == guids::kCabSimple)
    {
        plot->Range(-36.0, 18.0, 12.0);

        if (!overlay)
        {
            plot->Card("Cabinet Response", "+18 / -36 dB");
        }
    }
    else if (source.effectType == guids::kPickupMatch)
    {
        plot->Range(-24.0, 24.0, 12.0); // measured pickups differ by up to ~25 dB

        if (!overlay)
        {
            plot->Card("Pickup Conversion", "\xC2\xB1"
                                            "24 dB");
        }
    }
    else
    {
        plot->Range(-18.0, 18.0, 9.0);

        // The graphic EQ's curve is drawn behind its sliders without a card (BuildGraphicEq).
        if (!overlay && source.effectType != guids::kEqGraphic)
        {
            plot->Card("EQ Curve", "\xC2\xB1"
                                   "18 dB");
        }
    }

    auto& slot = overlay ? mOverlayPlot : mEffectPlot;
    slot = EqPlotState{plot, std::move(source), {}};

    const auto writeBand = [this, overlay](int band, double freq, double gainDb, double q, bool commit) {
        auto& state = overlay ? mOverlayPlot : mEffectPlot;

        if (!state || band < 0 || band >= static_cast<int>(std::size(kParametricBands)))
        {
            return;
        }

        const std::string prefix = kParametricBands[band].prefix;
        state->source.write(prefix + "Freq", freq, commit);
        state->source.write(prefix + "Gain", gainDb, commit);
        state->source.write(prefix + "Q", q, commit);
        SyncEqPlot(*state);
    };
    plot->onBandChange = writeBand;
    plot->onBandReset = [this, overlay](int band) {
        auto& state = overlay ? mOverlayPlot : mEffectPlot;

        if (!state || band < 0 || band >= static_cast<int>(std::size(kParametricBands)))
        {
            return;
        }

        const auto* type = mState.FindEffectType(state->source.effectType);
        const std::string prefix = kParametricBands[band].prefix;

        for (const char* suffix : {"Freq", "Gain", "Q"})
        {
            if (const auto* info = FindParam(type, prefix + suffix))
            {
                state->source.write(prefix + suffix, info->defaultValue, true);
            }
        }

        SyncEqPlot(*state);
    };

    SyncEqPlot(*slot);
    return plot;
}

void Shell::SyncEqPlot(EqPlotState& state)
{
    const auto params = state.source.read();
    const bool parametric = state.source.effectType == guids::kEqParametric;

    if (parametric)
    {
        const auto* type = mState.FindEffectType(state.source.effectType);
        std::vector<PlotBand> bands;

        for (const auto& keys : kParametricBands)
        {
            const std::string prefix = keys.prefix;
            const auto* freq = FindParam(type, prefix + "Freq");
            const auto* gain = FindParam(type, prefix + "Gain");
            const auto* q = FindParam(type, prefix + "Q");
            PlotBand band;
            band.label = keys.label;
            band.freq = ParamOr(params, freq, prefix + "Freq", 1000.0);
            band.gainDb = ParamOr(params, gain, prefix + "Gain", 0.0);
            band.q = ParamOr(params, q, prefix + "Q", 1.0);
            band.hasQ = q != nullptr;

            if (freq != nullptr)
            {
                band.freqMin = freq->minValue, band.freqMax = freq->maxValue;
            }

            if (gain != nullptr)
            {
                band.gainMin = gain->minValue, band.gainMax = gain->maxValue;
            }

            if (q != nullptr)
            {
                band.qMin = q->minValue, band.qMax = q->maxValue;
            }

            bands.push_back(std::move(band));
        }

        state.plot->SetBands(std::move(bands));
    }

    // The curve, asked for only when the parameters changed since the last request.
    nlohmann::json query = nlohmann::json::object();

    for (const auto& [key, value] : params)
    {
        if (std::isfinite(value))
        {
            query[key] = value;
        }
    }

    auto signature = query.dump();

    if (signature == state.lastQuery)
    {
        return;
    }

    state.lastQuery = std::move(signature);

    const bool overlay = &state == (mOverlayPlot ? &*mOverlayPlot : nullptr);
    std::weak_ptr<ResponsePlot> weak = state.plot;
    mSession.Client().GetAnalysis().RequestResponse(
        overlay ? "overlay-eq" : "effect-eq", state.source.effectType, query, parametric,
        [this, alive = std::weak_ptr<bool>(mAlive), weak, type = state.source.effectType](
            const std::optional<guitarfx::uiclient::ResponseCurve>& curve) {
            const auto plot = weak.lock();

            if (!alive.lock() || !plot)
            {
                return;
            }

            // A reply that failed leaves the cabinet's and Pickup Match's last curve up, as the web
            // UI keeps the last one it had; an EQ's goes.
            if (curve || type == guids::kEqParametric || type == guids::kEqGraphic)
            {
                plot->SetCurve(curve);
                mSession.Repaint(*plot);
            }

            // Pickup Match's custom designs are described from the same reply.
            if (curve && type == guids::kPickupMatch)
            {
                NotePickupDesigns(curve->summary);
            }
        });
    mSession.Repaint(*state.plot);
}

void Shell::SyncEqPlotFeeds()
{
    auto& analysis = mSession.Client().GetAnalysis();
    const bool effectShown = mEffectPlot && PlayShown() && !mOverlayPlot;
    EqPlotState* watched = mOverlayPlot ? &*mOverlayPlot : effectShown ? &*mEffectPlot : nullptr;

    // The Practice Tool's EQ shapes the backing track, which no spectrum feed watches.
    // With no EQ plot to feed, a multi-band effect's plot may have the watch instead.
    if (watched == nullptr || watched->source.feedKeys.empty())
    {
        if (!SyncMultibandFeed())
        {
            analysis.StopSpectrum();
        }

        return;
    }

    analysis.WatchSpectrum(watched->source.address);

    // A spectrum that stopped arriving is cleared rather than left frozen, as the web UI does.
    const auto& spectra = mState.analysis.spectra;
    const guitarfx::uiclient::NodeSpectrum* spectrum = nullptr;

    for (const auto& key : watched->source.feedKeys)
    {
        if (const auto it = spectra.find(key); it != spectra.end() && UiSession::Now() - it->second.receivedAt < 0.5)
        {
            spectrum = &it->second;
        }
    }

    if (watched->plot->SetSpectrum(spectrum))
    {
        mSession.Repaint(*watched->plot);
    }
}

namespace
{
// Pickup Match's design blocks (pickupResponse.ts, css/signal-path/enum-picker.css).
constexpr const char* kPickupSides[] = {"source", "target"};
constexpr const char* kDesignSuffixes[] = {"DesignType", "Resistance", "Gauge", "Magnet",
                                           "Cover",      "Wiring",     "CapacitanceTrim"};
constexpr const char* kDesignNote =
    "Predicted from construction, using fits to 397 measured pickups: the peak to about a semitone and its height to "
    "about 0.7 dB. Magnet grade changes inductance only. Capacitance Trim stands in for insulation and wind pattern, "
    "which measurements cannot predict. The brass cover comes from two measured pickups. Coil wiring applies to "
    "humbuckers and assumes matched coils.";
// The effect page's group box (EffectPage.cpp): cells 72 high, 4 apart, padded 11 at the sides,
// a title over them.
constexpr float kDesignCellHeight = 72.0f;
constexpr float kDesignCellGap = 4.0f;
constexpr float kDesignPadX = 11.0f;
constexpr float kDesignTitled = 6.0f + 14.0f + 4.0f;
constexpr float kDesignBelow = 8.0f;
constexpr float kDesignRowGap = 8.0f;
} // namespace

bool Shell::IsPickupDesignParam(const std::string& key)
{
    for (const char* side : kPickupSides)
    {
        for (const char* suffix : kDesignSuffixes)
        {
            if (key == std::string(side) + suffix)
            {
                return true;
            }
        }
    }

    return false;
}

namespace
{
// The summary (.pickup-design-summary): 12 px text in a pill padded 6 by 10; the note under it
// (.pickup-design-note): 10 px, 1.45 apart.
constexpr float kSummarySize = 12.0f;
constexpr float kSummaryLine = 16.0f;
constexpr float kNoteSize = 10.0f;
constexpr float kNoteLine = 14.5f;

/// `text` broken at spaces into lines no wider than `width` in `font`.
std::vector<std::string> WrapWords(const std::string& text, const el::font_descr& font, float width)
{
    std::vector<std::string> lines;
    std::string line;
    std::size_t i = 0;

    while (i < text.size())
    {
        const auto space = text.find(' ', i);
        const auto word = text.substr(i, space == std::string::npos ? std::string::npos : space - i);
        i = space == std::string::npos ? text.size() : space + 1;

        if (word.empty())
        {
            continue;
        }

        const auto joined = line.empty() ? word : line + " " + word;

        if (!line.empty() && MeasureTextWidth(joined, font) > width)
        {
            lines.push_back(line);
            line = word;
        }
        else
        {
            line = joined;
        }
    }

    if (!line.empty())
    {
        lines.push_back(line);
    }

    return lines;
}

/// Text wrapped to its width, read as it draws: the design summary in its pill (nothing at all
/// while there is no summary), or the note.
class DesignText final : public el::element, public Drawn
{
  public:
    DesignText(const Theme& theme, std::function<std::string()> text, float size, float line, el::color ink, bool pill)
        : mTheme(theme), mText(std::move(text)), mSize(size), mLine(line), mInk(ink), mPill(pill)
    {
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        const auto text = mText ? mText() : std::string();

        if (text.empty())
        {
            return;
        }

        auto& cnv = ctx.canvas;
        auto box = ctx.bounds;

        if (mPill)
        {
            const auto& tokens = *guitarfx::uiclient::theme::FindTheme(mTheme.Name()).tokens;
            cnv.fill_style(Theme::Argb(tokens.overlayLight));
            cnv.fill_round_rect(box, 6.0f);
            box = el::rect{box.left + 10.0f, box.top + 6.0f, box.right - 10.0f, box.bottom - 6.0f};
        }

        const auto font = Font(Weight::Regular).size(mSize);
        float top = box.top;

        for (const auto& line : WrapWords(text, font, box.width()))
        {
            FillText(cnv, line, font, mInk, el::rect{box.left, top, box.right, top + mLine}, HAlign::Left);
            top += mLine;
        }
    }

  private:
    const Theme& mTheme;
    std::function<std::string()> mText;
    float mSize, mLine;
    el::color mInk;
    bool mPill;
};

/// Whether `side` ("source" or "target") is set to Custom Design: its choice is in the picker
/// group the engine files Custom Design under.
bool IsCustomSide(const guitarfx::uiclient::EffectTypeInfo* type, const guitarfx::GraphNode& node,
                  const std::string& side)
{
    const auto* info = FindParam(type, side);

    if (info == nullptr || info->labels.empty() || info->labelGroups.size() != info->labels.size())
    {
        return false;
    }

    const auto it = node.params.find(side);
    const int index =
        guitarfx::uiclient::EnumLabelIndex(*info, it != node.params.end() ? it->second : info->defaultValue);
    return index >= 0 && static_cast<std::size_t>(index) < info->labelGroups.size() &&
           info->labelGroups[static_cast<std::size_t>(index)] == "Custom";
}
} // namespace

Ptr Shell::BuildPickupDesignBlocks(const guitarfx::GraphNode& node, float width, const ParamCellMaker& makeCell,
                                   float& height)
{
    // A block per side set to Custom Design: its design controls in their group's box, what the
    // engine predicts it is built as (from the curve's reply), and the note on the predictions.
    const auto* type = mState.FindEffectType(node.type);

    if (mPickupDesignNodeId != node.id)
    {
        mPickupDesignText.clear();
        mPickupDesignNodeId = node.id;
    }

    mPickupDesignPills.clear();
    height = 0.0f;
    std::vector<Ptr> blocks;

    for (const char* sideKey : kPickupSides)
    {
        const std::string side = sideKey;

        if (!IsCustomSide(type, node, side))
        {
            continue;
        }

        // The controls, wrapped into rows across the box.
        std::vector<std::vector<std::pair<float, Ptr>>> rows(1);
        float used = 0.0f;
        std::string title;
        const float inner = std::max(0.0f, width - 2.0f * kDesignPadX);

        for (const char* suffix : kDesignSuffixes)
        {
            const auto* info = FindParam(type, side + suffix);

            if (info == nullptr)
            {
                continue;
            }

            if (title.empty())
            {
                title = info->group;
            }

            auto cell = makeCell(*info, info->name);

            if (!rows.back().empty() && used + kDesignCellGap + cell.first > inner)
            {
                rows.emplace_back();
                used = 0.0f;
            }

            used += (rows.back().empty() ? 0.0f : kDesignCellGap) + cell.first;
            rows.back().push_back(std::move(cell));
        }

        if (rows.front().empty())
        {
            continue;
        }

        std::vector<Ptr> rowElements;

        for (const auto& row : rows)
        {
            std::vector<Ptr> cells;

            for (const auto& [w, cell] : row)
            {
                if (!cells.empty())
                {
                    cells.push_back(HGap(kDesignCellGap));
                }

                cells.push_back(FixedWidth(w, cell));
            }

            cells.push_back(Stretch());

            if (!rowElements.empty())
            {
                rowElements.push_back(VGap(kDesignRowGap));
            }

            rowElements.push_back(FixedHeight(kDesignCellHeight, Row(cells)));
        }

        const float count = static_cast<float>(rows.size());
        const float boxHeight =
            kDesignTitled + count * kDesignCellHeight + (count - 1.0f) * kDesignRowGap + kDesignBelow;
        auto box = Over(Pad(kDesignPadX, kDesignTitled, kDesignPadX, kDesignBelow, Col(rowElements)),
                        std::make_shared<InsetBox>(mTheme, title));

        std::vector<Ptr> parts{FixedHeight(boxHeight, box)};
        float blockHeight = boxHeight;

        // The summary, where the engine has described this side: its pill is not there without one.
        const auto known = mPickupDesignText.find(side);

        if (known != mPickupDesignText.end() && !known->second.empty())
        {
            const auto lines =
                WrapWords(known->second, Font(Weight::Regular).size(kSummarySize), std::max(1.0f, width - 20.0f));
            const float pillHeight = 12.0f + static_cast<float>(std::max<std::size_t>(1, lines.size())) * kSummaryLine;
            auto pill = std::make_shared<DesignText>(
                mTheme,
                [this, side] {
                    const auto it = mPickupDesignText.find(side);
                    return it != mPickupDesignText.end() ? it->second : std::string();
                },
                kSummarySize, kSummaryLine, mTheme.Text(), true);
            mPickupDesignPills.push_back(pill);
            parts.push_back(VGap(6));
            parts.push_back(FixedHeight(pillHeight, pill));
            blockHeight += 6.0f + pillHeight;
        }

        const float noteHeight = std::ceil(
            static_cast<float>(WrapWords(kDesignNote, Font(Weight::Regular).size(kNoteSize), width).size()) * kNoteLine);
        parts.push_back(VGap(6));
        parts.push_back(FixedHeight(noteHeight, std::make_shared<DesignText>(
                                                    mTheme, [] { return std::string(kDesignNote); }, kNoteSize,
                                                    kNoteLine, mTheme.TextSecondary(), false)));
        blockHeight += 6.0f + noteHeight;

        if (!blocks.empty())
        {
            blocks.push_back(VGap(10));
            height += 10.0f;
        }

        blocks.push_back(FixedHeight(blockHeight, Col(parts)));
        height += blockHeight;
    }

    return blocks.empty() ? nullptr : Col(blocks);
}

void Shell::NotePickupDesigns(const std::map<std::string, double>& summary)
{
    // The figures follow in place; a side whose summary comes or goes changes the page's
    // signature (EqSectionSignature), which builds the block again with or without its pill.
    bool emptied = false;

    for (const char* side : kPickupSides)
    {
        auto text = guitarfx::uiclient::PickupDesignSummary(summary, side);
        auto& kept = mPickupDesignText[side];
        emptied = emptied || kept.empty() != text.empty();
        kept = std::move(text);
    }

    for (const auto& pill : mPickupDesignPills)
    {
        if (const auto p = pill.lock())
        {
            mSession.Repaint(*p);
        }
    }

    if (emptied)
    {
        Mark(kPage);
    }
}

std::string Shell::EqSectionSignature(const guitarfx::GraphNode& node) const
{
    std::string s;
    const auto param = [&node](const std::string& key, double fallback) {
        const auto it = node.params.find(key);
        return it != node.params.end() && std::isfinite(it->second) ? it->second : fallback;
    };

    // The graphic EQ shows a slider per band in use and turned on.
    if (node.type == guids::kEqGraphic)
    {
        s += "|geq" + std::to_string(std::lround(param("bandCount", 10.0)));

        for (int band = 1; band <= 10; ++band)
        {
            s += param("band" + std::to_string(band) + "Enabled", 1.0) >= 0.5 ? "1" : "0";
        }
    }

    // Pickup Match shows a design block per side set to Custom Design, its summary once known.
    if (node.type == guids::kPickupMatch)
    {
        const auto* type = mState.FindEffectType(node.type);
        s += "|pd";

        for (const char* side : kPickupSides)
        {
            const auto text = mPickupDesignNodeId == node.id ? mPickupDesignText.find(side) : mPickupDesignText.end();
            s += IsCustomSide(type, node, side)
                     ? (text != mPickupDesignText.end() && !text->second.empty() ? "S" : "c")
                     : "-";
        }
    }

    return s;
}

Ptr Shell::BuildCabTools(const guitarfx::GraphNode& node)
{
    // The web UI's cabinet tools: match the voicing to a library IR, and save it as one. Each is
    // disabled while it runs, and the status beside them says what is running.
    const std::string nodeId = node.id;
    auto status = std::make_shared<Text>(mTheme, "", 11.0f);
    status->Colour(mTheme.TextSecondary());
    mIrExportStatus = status;

    auto match = std::make_shared<PlotToolButton>(mTheme, "Match an IR\xE2\x80\xA6");
    mCabMatchButton = match;
    match->onClick = [this, nodeId] {
        OpenResourceBrowser(
            nodeId, "ir", -1,
            [this, nodeId](const std::string& irId) {
                const auto* ir = mState.FindLibraryResource("ir", irId);
                const std::string irName = ir != nullptr && !ir->name.empty() ? ir->name : irId;
                mCabMatchPending = true;
                ShowCabToolsState();
                mSession.Client().GetAnalysis().MatchSimpleCabToIr(
                    irId,
                    [this, alive = std::weak_ptr<bool>(mAlive), nodeId, irName](
                        const std::optional<guitarfx::uiclient::SimpleCabMatch>& result, const std::string& error) {
                        if (!alive.lock())
                        {
                            return;
                        }

                        mCabMatchPending = false;
                        ShowCabToolsState();

                        if (!result)
                        {
                            ShowToast("Match failed", error, true);
                            return;
                        }

                        for (const auto& [key, value] : result->params)
                        {
                            mSession.Commands().SetNodeParam(nodeId, key, value);
                        }

                        NoteChainEdit();
                        Mark(kValues);
                        char detail[64];
                        std::snprintf(detail, sizeof(detail), " (shape within %.1f dB)", result->rmsErrorDb);
                        ShowToast("Cabinet matched", irName + (std::isfinite(result->rmsErrorDb) ? detail : ""), false);
                    });
            },
            "Match an IR", "cab");
    };

    auto exportIr = std::make_shared<PlotToolButton>(mTheme, "Export as IR");
    mIrExportButton = exportIr;
    exportIr->onClick = [this, nodeId] {
        const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;

        if (n == nullptr || !mPendingIrExport.empty())
        {
            return;
        }

        // "Simple Cab 2x12 Open Ribbon": what the export is called until the user renames it.
        std::string name = "Simple Cab";

        if (const auto* type = mState.FindEffectType(n->type))
        {
            for (const char* key : {"cabinet", "micType"})
            {
                for (const auto& info : type->parameters)
                {
                    if (info.key == key && !info.labels.empty())
                    {
                        const auto it = n->params.find(key);
                        const int index = guitarfx::uiclient::EnumLabelIndex(
                            info, it != n->params.end() ? it->second : info.defaultValue);

                        if (index >= 0 && static_cast<std::size_t>(index) < info.labels.size())
                        {
                            name += " " + info.labels[static_cast<std::size_t>(index)];
                        }
                    }
                }
            }
        }

        nlohmann::json params = nlohmann::json::object();

        for (const auto& [key, value] : n->params)
        {
            params[key] = value;
        }

        mPendingIrExport = mSession.Client().GetAnalysis().ExportEffectAsIr(n->type, params, name);
        ShowCabToolsState();
    };

    // Built while one is running (the page rebuilt under it), the tools show it.
    ShowCabToolsState();
    return Row({FixedWidth(match->Width(), Middle(match)), HGap(8), FixedWidth(exportIr->Width(), Middle(exportIr)),
                HGap(8), Middle(status), Stretch()});
}

void Shell::ShowCabToolsState()
{
    const bool exporting = !mPendingIrExport.empty();

    if (const auto status = mIrExportStatus.lock())
    {
        status->Set(mCabMatchPending ? "Matching\xE2\x80\xA6" : exporting ? "Exporting\xE2\x80\xA6" : "");
        mSession.Repaint(*status);
    }

    if (const auto match = mCabMatchButton.lock())
    {
        match->Enabled(!mCabMatchPending);
        mSession.Repaint(*match);
    }

    if (const auto exportIr = mIrExportButton.lock())
    {
        exportIr->Enabled(!exporting);
        mSession.Repaint(*exportIr);
    }
}

void Shell::SyncIrExport()
{
    // An export as IR is answered by resourceImported or resourceImportFailed with its id.
    if (mPendingIrExport.empty())
    {
        return;
    }

    const auto it = mState.resourceImports.find(mPendingIrExport);

    if (it == mState.resourceImports.end() ||
        it->second.status == guitarfx::uiclient::ResourceImport::Status::Importing)
    {
        return;
    }

    const auto result = it->second;
    mPendingIrExport.clear();
    ShowCabToolsState();

    if (result.status == guitarfx::uiclient::ResourceImport::Status::Imported)
    {
        const auto* ir = mState.FindLibraryResource("ir", result.resourceId);
        ShowToast("IR saved to your library", ir != nullptr ? ir->name : result.resourceId, false);
    }
    else
    {
        ShowToast("IR export failed", result.detail, true);
    }
}

Shell::EqPlotSource Shell::NodeEqPlotSource(const guitarfx::GraphNode& node)
{
    EqPlotSource source;
    source.effectType = node.type;
    source.address = NodeAddress{"preset", node.id, {}};
    // The engine names the preset in its answer when it knows it; without, the key has none.
    source.feedKeys = {mState.activePresetId + "::" + node.id, "::" + node.id};
    const std::string nodeId = node.id;
    source.read = [this, nodeId]() -> std::map<std::string, double> {
        if (mState.activePreset)
        {
            if (const auto* n = mState.activePreset->graph.FindNode(nodeId))
            {
                return n->params;
            }
        }

        return {};
    };
    source.write = [this, nodeId](const std::string& key, double value, bool commit) {
        mSession.Commands().SetNodeParam(nodeId, key, value);
        NoteChainEdit();

        if (commit)
        {
            Mark(kValues);
        }
    };
    return source;
}
} // namespace nanoq::ui
