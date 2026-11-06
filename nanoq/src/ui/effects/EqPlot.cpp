// EqPlot.cpp - the response curves: the parametric and graphic EQ nodes' and the Simple
// Cabinet's in the effect editor, and the Global EQ's in its dialog (the web UI's
// signalPath/paramsPanel/eq.ts, cabResponse.ts and eqPanel.ts), with the cabinet's IR tools.
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

#include <cmath>
#include <cstdio>

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
    if (source.effectType == guids::kCabSimple)
    {
        plot->Range(-36.0, 18.0, 12.0);
    }
    else if (source.effectType == guids::kPickupMatch)
    {
        plot->Range(-24.0, 24.0, 12.0); // measured pickups differ by up to ~25 dB
    }
    else
    {
        plot->Range(-18.0, 18.0, 9.0);
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
        [this, alive = std::weak_ptr<bool>(mAlive), weak, pickup = state.source.effectType == guids::kPickupMatch](
            const std::optional<guitarfx::uiclient::ResponseCurve>& curve) {
            const auto plot = weak.lock();

            if (!alive.lock() || !plot)
            {
                return;
            }

            plot->SetCurve(curve);
            mSession.Repaint(*plot);

            // Pickup Match's custom designs are described from the same reply.
            if (pickup)
            {
                for (const auto& [side, text] : mPickupDesignLines)
                {
                    if (const auto line = text.lock())
                    {
                        line->Set(curve ? guitarfx::uiclient::PickupDesignSummary(curve->summary, side)
                                        : std::string());
                        mSession.Repaint(*line);
                    }
                }
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
    if (watched == nullptr || watched->source.feedKeys.empty())
    {
        analysis.StopSpectrum();
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

Ptr Shell::BuildPickupDesignLines()
{
    // What each side set to Custom Design is predicted to be built as; empty otherwise.
    mPickupDesignLines.clear();
    std::vector<Ptr> rows;

    for (const char* side : {"source", "target"})
    {
        auto line = std::make_shared<Text>(mTheme, "", 11.0f);
        line->Colour(mTheme.TextSecondary());
        mPickupDesignLines.emplace_back(side, line);
        auto label =
            std::make_shared<Text>(mTheme, side == std::string("source") ? "SOURCE" : "TARGET", 9.0f, Weight::SemiBold);
        label->Colour(mTheme.TextMuted());
        rows.push_back(FixedHeight(18, Row({FixedWidth(56, label), line})));
    }

    return Col(rows);
}

Ptr Shell::BuildCabTools(const guitarfx::GraphNode& node)
{
    // The web UI's cabinet tools: match the voicing to a library IR, and save it as one.
    const std::string nodeId = node.id;
    auto status = std::make_shared<Text>(mTheme, "", 11.0f);
    status->Colour(mTheme.TextMuted());

    auto match = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    match->Size(112, 28).Label("Match an IR...", 11.0f);
    match->onClick = [this, nodeId, status] {
        OpenResourceBrowser(
            nodeId, "ir", -1,
            [this, nodeId, status](const std::string& irId) {
                const auto* ir = mState.FindLibraryResource("ir", irId);
                const std::string irName = ir != nullptr && !ir->name.empty() ? ir->name : irId;
                status->Set("Matching...");
                mSession.Repaint(*status);
                mSession.Client().GetAnalysis().MatchSimpleCabToIr(
                    irId,
                    [this, alive = std::weak_ptr<bool>(mAlive), nodeId, irName, status](
                        const std::optional<guitarfx::uiclient::SimpleCabMatch>& result, const std::string& error) {
                        if (!alive.lock())
                        {
                            return;
                        }

                        status->Set("");
                        mSession.Repaint(*status);

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

    auto exportIr = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    exportIr->Size(96, 28).Label("Export as IR", 11.0f);
    exportIr->onClick = [this, nodeId, status] {
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
        status->Set("Exporting...");
        mSession.Repaint(*status);
        mIrExportStatus = status;
    };
    return Row({match, HGap(8), exportIr, HGap(10), status, Stretch()});
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

    if (const auto status = mIrExportStatus.lock())
    {
        status->Set("");
        mSession.Repaint(*status);
    }

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
