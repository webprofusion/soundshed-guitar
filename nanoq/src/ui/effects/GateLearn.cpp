// GateLearn.cpp - the noise gate's Learn (the web UI's gateLearn.ts, gateLearnPanel.ts and
// gateSettings.ts): a meter of the noise it heard against Threshold and its hysteresis, the
// button that listens for two seconds with the strings muted, and what it found. A finished
// listen sets Threshold the way the knob would; the engine only measures and suggests.
//
// Shown in a Noise Gate node's editor and in the global gate's settings, which also carry the
// gate's other parameters. The Multi-band Gate's editor draws the same meter once per band,
// labelled, under one button; its listen sets all six thresholds.

#include "ui/Drawn.h"
#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "dsp/EffectGuids.h"
#include "uiclient/Analysis.h"
#include "uiclient/ParamFormat.h"

#include <algorithm>
#include <cmath>
#include <tuple>

namespace nanoq::ui
{
namespace guids = guitarfx::EffectGuids;
using guitarfx::uiclient::GateLearnBandStatus;
using guitarfx::uiclient::GateLearnStatus;
using guitarfx::uiclient::NodeAddress;

namespace
{
using cycfi::artist::canvas;

/// The meter: the threshold's range across, the noise floor heard filled from the left, the
/// band between Threshold and where the gate closes again, Threshold as a line, and while
/// listening the live level and the listen's progress.
class GateLearnMeter final : public el::element, public Drawn
{
  public:
    explicit GateLearnMeter(const Theme& theme, float height = 14.0f) : mTheme(theme), mHeight(height)
    {
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{120.0f, mHeight}, {el::full_extent, mHeight}};
    }

    /// The meter of a gate, or of one band of a Multi-band Gate (`band`, which has no
    /// hysteresis). Returns true if anything it draws changed.
    bool Set(double minDb, double maxDb, double threshold, double hysteresis, const GateLearnStatus* status,
             const GateLearnBandStatus* band = nullptr)
    {
        const bool listening = status != nullptr && status->state == "listening";
        const auto noise = band != nullptr     ? band->noiseFloorDb
                           : status != nullptr ? status->noiseFloorDb
                                               : std::nullopt;
        const auto level = band != nullptr ? band->levelDb : status != nullptr ? status->levelDb : std::nullopt;
        const auto next =
            std::make_tuple(minDb, std::max(minDb + 1.0, maxDb), threshold, threshold - std::max(0.0, hysteresis),
                            noise, listening ? level : std::nullopt, listening ? status->progress : 0.0);

        if (next == std::tie(mMin, mMax, mThreshold, mClose, mNoise, mLevel, mProgress))
        {
            return false;
        }

        std::tie(mMin, mMax, mThreshold, mClose, mNoise, mLevel, mProgress) = next;
        return true;
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const auto at = [&](double db) {
            return b.left + static_cast<float>(std::clamp((db - mMin) / (mMax - mMin), 0.0, 1.0)) * b.width();
        };

        cnv.fill_style(mTheme.Pick(el::rgba(0, 0, 0, 90), el::rgba(0, 0, 0, 28)));
        cnv.add_round_rect(b, 3.0f);
        cnv.fill();

        auto state = cnv.new_state();
        cnv.add_round_rect(b, 3.0f);
        cnv.clip();

        if (mNoise)
        {
            cnv.fill_style(mTheme.TextMuted().opacity(0.35f));
            cnv.fill_rect(el::rect{b.left, b.top, at(*mNoise), b.bottom});
        }

        cnv.fill_style(mTheme.Accent().opacity(0.22f));
        cnv.fill_rect(el::rect{at(mClose), b.top, at(mThreshold), b.bottom});

        if (mLevel)
        {
            cnv.fill_style(mTheme.Success().opacity(0.7f));
            cnv.fill_rect(el::rect{b.left, b.top + 4.0f, at(*mLevel), b.bottom - 4.0f});
        }

        if (mProgress > 0.0)
        {
            cnv.fill_style(mTheme.Accent());
            cnv.fill_rect(el::rect{b.left, b.bottom - 2.0f,
                                   b.left + static_cast<float>(std::clamp(mProgress, 0.0, 1.0)) * b.width(), b.bottom});
        }

        cnv.fill_style(mTheme.Accent());
        const float x = at(mThreshold);
        cnv.fill_rect(el::rect{x - 1.0f, b.top, x + 1.0f, b.bottom});
    }

  private:
    const Theme& mTheme;
    float mHeight;
    double mMin = -80.0, mMax = 0.0, mThreshold = -60.0, mClose = -64.0;
    std::optional<double> mNoise, mLevel;
    double mProgress = 0.0;
};

std::string WholeDb(double value)
{
    const long rounded = std::lround(value);
    return (rounded < 0 ? std::string("\xE2\x88\x92") + std::to_string(-rounded) : std::to_string(rounded)) + " dB";
}

// The Multi-band Gate's meters: their height, the gap between them, and their labels' column.
constexpr float kBandMeterHeight = 12.0f, kBandGap = 3.0f, kBandLabelWidth = 56.0f, kBandLabelGap = 6.0f;

/// A node's parameter as stored, else its effect's default, else `fallback`. The effect type
/// is looked up on each call, not kept: the catalog is replaced whenever the engine sends it
/// again, and a pointer into the old one read freed memory on the next sync (seen when a Bars
/// loop started recording).
double NodeParam(const guitarfx::uiclient::ClientState& state, const std::string& nodeId, const std::string& typeId,
                 const std::string& key, double fallback)
{
    const auto* n = state.activePreset ? state.activePreset->graph.FindNode(nodeId) : nullptr;

    if (n != nullptr)
    {
        if (const auto it = n->params.find(key); it != n->params.end())
        {
            return it->second;
        }
    }

    if (const auto* type = state.FindEffectType(typeId))
    {
        for (const auto& p : type->parameters)
        {
            if (p.key == key)
            {
                return p.defaultValue;
            }
        }
    }

    return fallback;
}
} // namespace

float Shell::GateLearnHeight(const GateLearnTarget& target)
{
    const auto bands = static_cast<float>(target.bandLabels.size());
    const float meters = bands > 0.0f ? bands * kBandMeterHeight + (bands - 1.0f) * kBandGap : 14.0f;
    return meters + 2.0f + 12.0f + 6.0f + 26.0f;
}

Ptr Shell::BuildGateLearn(GateLearnTarget target)
{
    const bool multiband = !target.bandLabels.empty();
    auto meter = std::make_shared<GateLearnMeter>(mTheme, multiband ? kBandMeterHeight : 14.0f);
    auto low = std::make_shared<Text>(mTheme, WholeDb(target.minDb), 9.0f);
    auto high = std::make_shared<Text>(mTheme, WholeDb(target.maxDb), 9.0f);
    low->Colour(mTheme.TextMuted());
    high->Colour(mTheme.TextMuted()).Alignment(Text::Align::Right);
    auto status = std::make_shared<Text>(mTheme, "", 11.0f);
    status->Colour(mTheme.TextSecondary());
    auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    button->Size(72, 26);

    const auto key =
        guitarfx::uiclient::NodeFeedKey(target.address.scope, target.address.presetId, target.address.nodeId);
    // What the engine last said of this gate's listen, if anything.
    const auto learnStatus = [this, key]() -> const GateLearnStatus* {
        const auto it = mState.analysis.gateLearn.find(key);
        return it != mState.analysis.gateLearn.end() ? &it->second : nullptr;
    };
    button->onClick = [this, key, address = target.address, ensureEnabled = target.ensureEnabled] {
        auto& analysis = mSession.Client().GetAnalysis();
        auto& learn = mSession.Client().MutableState().analysis.gateLearn;

        if (const auto it = learn.find(key); it != learn.end() && it->second.state == "listening")
        {
            analysis.CancelGateLearn(address);
            return;
        }

        if (ensureEnabled)
        {
            ensureEnabled();
        }

        // Listening at once, as the web UI shows it, until the engine's first status. A
        // Multi-band Gate's bands keep their learned noise floors under the live levels.
        GateLearnStatus listening{"listening"};

        for (const auto& band : learn[key].bands)
        {
            listening.bands.push_back(GateLearnBandStatus{{}, band.noiseFloorDb, {}, {}});
        }

        learn[key] = std::move(listening);
        mGateLearnSeen[key] = "listening";
        analysis.StartGateLearn(address);
    };

    // A listen that just finished sets Threshold, or each band's threshold when every band has
    // one, once, where it measured. Run by the first meter's feed.
    const auto applyFinished = [this, key, target](const GateLearnStatus* s) {
        if (s == nullptr)
        {
            return;
        }

        auto& seen = mGateLearnSeen[key];

        if (s->state == "done" && seen == "listening")
        {
            if (!target.bandLabels.empty())
            {
                const bool everyBand =
                    !s->bands.empty() && std::all_of(s->bands.begin(), s->bands.end(),
                                                     [](const auto& b) { return b.thresholdDb.has_value(); });

                if (everyBand && target.applyBands)
                {
                    target.applyBands(s->bands);
                }
            }
            else if (s->thresholdDb && target.apply)
            {
                target.apply(*s->thresholdDb);
            }
        }

        seen = s->state;
    };

    // A band's threshold as the gate applies it, and its part of the status.
    const auto bandThreshold = [target](size_t band) {
        const auto thresholds = target.bandThresholds ? target.bandThresholds() : std::vector<double>{};
        return band < thresholds.size() ? thresholds[band] : target.minDb;
    };
    const auto bandStatus = [](const GateLearnStatus* s, size_t band) -> const GateLearnBandStatus* {
        static const GateLearnBandStatus none;
        return s == nullptr ? nullptr : band < s->bands.size() ? &s->bands[band] : &none;
    };

    mLive.Feed(meter, [learnStatus, applyFinished, multiband, bandThreshold, bandStatus, target](GateLearnMeter& m) {
        const auto* s = learnStatus();
        applyFinished(s);

        if (multiband)
        {
            return m.Set(target.minDb, target.maxDb, bandThreshold(0), 0.0, s, bandStatus(s, 0));
        }

        return m.Set(target.minDb, target.maxDb, target.threshold ? target.threshold() : -60.0,
                     target.hysteresis ? target.hysteresis() : 0.0, s);
    });
    mLive.Bind(
        status,
        [learnStatus, minDb = target.minDb] { return guitarfx::uiclient::GateLearnStatusText(learnStatus(), minDb); },
        &Text::Set);
    mLive.Bind(
        button,
        [learnStatus] {
            const auto* s = learnStatus();
            return s != nullptr && s->state == "listening";
        },
        [](IconButton& b, bool listening) { b.Label(listening ? "Cancel" : "Learn", 11.0f); });
    auto controls = FixedHeight(26, Row({button, HGap(10), status, Stretch()}));

    if (!multiband)
    {
        return Col({FixedHeight(14, meter), VGap(2), FixedHeight(12, Row({low, Stretch(), high})), VGap(6), controls});
    }

    // One labelled meter per band (.gate-learn-band), the scale under the meters alone.
    std::vector<Ptr> rows;

    for (size_t band = 0; band < target.bandLabels.size(); ++band)
    {
        auto label = std::make_shared<Text>(mTheme, target.bandLabels[band], 9.0f);
        label->Colour(mTheme.TextSecondary()).Alignment(Text::Align::Right);
        auto bandMeter = band == 0 ? meter : std::make_shared<GateLearnMeter>(mTheme, kBandMeterHeight);

        if (band > 0)
        {
            mLive.Feed(bandMeter, [learnStatus, bandThreshold, bandStatus, band, target](GateLearnMeter& m) {
                const auto* s = learnStatus();
                return m.Set(target.minDb, target.maxDb, bandThreshold(band), 0.0, s, bandStatus(s, band));
            });
            rows.push_back(VGap(kBandGap));
        }

        rows.push_back(FixedHeight(kBandMeterHeight,
                                   Row({FixedWidth(kBandLabelWidth, Middle(label)), HGap(kBandLabelGap), bandMeter})));
    }

    rows.push_back(VGap(2));
    rows.push_back(FixedHeight(12, Row({HGap(kBandLabelWidth + kBandLabelGap), low, Stretch(), high})));
    rows.push_back(VGap(6));
    rows.push_back(controls);
    return Col(rows);
}

Shell::GateLearnTarget Shell::NodeGateLearnTarget(const guitarfx::GraphNode& node)
{
    const std::string nodeId = node.id;
    GateLearnTarget target;
    target.address = NodeAddress{"preset", nodeId, mState.activePresetId};
    const auto* type = mState.FindEffectType(node.type);
    const auto param = [this, nodeId, typeId = node.type](const char* key, double fallback) {
        return NodeParam(mState, nodeId, typeId, key, fallback);
    };
    target.threshold = [param] { return param("threshold", -60.0); };
    target.hysteresis = [param] { return param("hysteresis", 4.0); };

    if (type != nullptr)
    {
        for (const auto& p : type->parameters)
        {
            if (p.key == "threshold")
            {
                target.minDb = p.minValue, target.maxDb = p.maxValue;
            }
        }
    }

    const std::string presetId = mState.activePresetId;
    target.apply = [this, nodeId, presetId](double thresholdDb) {
        // Only into the preset it measured: one switched away from while listening is left alone.
        if (mState.activePresetId != presetId)
        {
            return;
        }

        mSession.Commands().SetNodeParam(nodeId, "threshold", thresholdDb);
        NoteChainEdit();
        Mark(kValues);
    };
    target.ensureEnabled = [this, nodeId] {
        const auto* n = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;

        if (n != nullptr && !n->enabled)
        {
            mSession.Commands().SetNodeBypassed(nodeId, false);
        }
    };
    return target;
}

Shell::GateLearnTarget Shell::MultibandGateLearnTarget(const guitarfx::GraphNode& node)
{
    // The web UI's gateLearnPanel.ts: the bands' thresholds, low band first, under their labels.
    static const std::vector<std::string> keys{"threshold1", "threshold2", "threshold3",
                                               "threshold4", "threshold5", "threshold6"};
    const std::string nodeId = node.id;
    auto target = NodeGateLearnTarget(node); // its address and ensureEnabled
    target.threshold = nullptr, target.hysteresis = nullptr, target.apply = nullptr;
    target.bandLabels = {"< 150 Hz",
                         "150\xE2\x80\x93"
                         "400",
                         "400\xE2\x80\x93"
                         "1k",
                         "1k\xE2\x80\x93"
                         "2.5k",
                         "2.5k\xE2\x80\x93"
                         "6k",
                         "> 6 kHz"};
    target.minDb = -96.0, target.maxDb = 0.0;

    if (const auto* type = mState.FindEffectType(node.type))
    {
        for (const auto& p : type->parameters)
        {
            if (p.key == keys.front())
            {
                target.minDb = p.minValue, target.maxDb = p.maxValue;
            }
        }
    }

    target.bandThresholds = [this, nodeId, typeId = node.type] {
        // Offset moves every band's threshold, so the meters show where each one really sits.
        const double offset = NodeParam(mState, nodeId, typeId, "offset", 0.0);
        std::vector<double> thresholds;

        for (const auto& key : keys)
        {
            thresholds.push_back(NodeParam(mState, nodeId, typeId, key, -65.0) + offset);
        }

        return thresholds;
    };

    const std::string presetId = mState.activePresetId;
    target.applyBands = [this, nodeId, presetId](const std::vector<GateLearnBandStatus>& bands) {
        // Only into the preset it measured: one switched away from while listening is left alone.
        if (mState.activePresetId != presetId)
        {
            return;
        }

        for (size_t band = 0; band < bands.size() && band < keys.size(); ++band)
        {
            if (bands[band].thresholdDb)
            {
                mSession.Commands().SetNodeParam(nodeId, keys[band], *bands[band].thresholdDb);
            }
        }

        NoteChainEdit();
        Mark(kValues);
    };
    return target;
}

void Shell::OpenGlobalGate(el::rect anchor)
{
    // The web UI's gate settings flyout (gateSettings.ts): the title with the gate's switch,
    // Learn, then the gate's parameters from the engine's own definitions as rows (the main
    // ones, then Advanced), sent as gate.<key>, and Reset to defaults.
    CloseOverlay();
    auto& commands = mSession.Commands();
    const auto* type = mState.FindEffectType(guids::kDynamicsGate);
    const auto gateNode = [this] { return mState.globalChain.preChainGraph.FindNode("global_gate"); };
    const auto gateParam = [gateNode](const std::string& key, double fallback) {
        if (const auto* node = gateNode())
        {
            if (const auto it = node->params.find(key); it != node->params.end())
            {
                return it->second;
            }
        }

        return fallback;
    };

    GateLearnTarget target;
    target.address = NodeAddress{"global", "global_gate", {}};
    target.threshold = [gateParam] { return gateParam("threshold", -60.0); };
    target.hysteresis = [gateParam] { return gateParam("hysteresis", 4.0); };
    target.apply = [&commands](double thresholdDb) { commands.SetGlobalChainParam("gate.threshold", thresholdDb); };
    target.ensureEnabled = [gateNode, &commands] {
        if (const auto* node = gateNode(); node != nullptr && !node->enabled)
        {
            commands.SetGlobalChainToggle("gate.enabled", true);
        }
    };

    std::vector<guitarfx::uiclient::EffectParamInfo> main, advanced;

    if (type != nullptr)
    {
        for (const auto& info : type->parameters)
        {
            if (info.key == "threshold")
            {
                target.minDb = info.minValue, target.maxDb = info.maxValue;
            }

            (info.advanced ? advanced : main).push_back(info);
        }
    }

    FlyoutBinding binding;
    binding.value = [gateParam](const guitarfx::uiclient::EffectParamInfo& info) {
        return gateParam(info.key, info.defaultValue);
    };
    binding.apply = [&commands](const std::string& key, double value, bool) {
        commands.SetGlobalChainParam("gate." + key, value);
    };

    // The header (.flyout-settings-header): the title and the gate's on/off.
    const auto heading = [this](const std::string& text, float size) {
        auto t = std::make_shared<Text>(mTheme, Upper(text), size, Weight::SemiBold);
        t->Colour(mTheme.Pick(el::rgba(220, 227, 243, 200), mTheme.TextSecondary()));
        return t;
    };
    auto enabled = std::make_shared<ToggleSwitch>(mTheme);
    enabled->AccessName("Noise gate on");
    enabled->onClick = [gateNode, &commands] {
        const auto* node = gateNode();
        commands.SetGlobalChainToggle("gate.enabled", node == nullptr || !node->enabled);
    };
    mLive.Bind(
        enabled,
        [gateNode] {
            const auto* n = gateNode();
            return n != nullptr && n->enabled;
        },
        &ToggleSwitch::Set);

    const auto rule = [this] { return FixedHeight(1, Fill(mTheme.Border(), std::make_shared<el::element>())); };
    auto reset = std::make_shared<Button>(
        mTheme, "Reset to defaults",
        [main, advanced, &commands] {
            for (const auto* list : {&main, &advanced})
            {
                for (const auto& info : *list)
                {
                    commands.SetGlobalChainParam("gate." + info.key, info.defaultValue);
                }
            }
        },
        Button::Style::Flat);
    reset->TextSize(11.0f);

    std::vector<Ptr> column{FixedHeight(26, Row({Middle(heading("Noise Gate", 11.0f)), Stretch(), Middle(enabled)})),
                            VGap(8), FixedHeight(60, BuildGateLearn(std::move(target))), VGap(6),
                            BuildFlyoutParamRows(main, binding)};
    float height = 26.0f + 8.0f + 60.0f + 6.0f + 38.0f * static_cast<float>(main.size());

    if (!advanced.empty())
    {
        column.push_back(VGap(6));
        column.push_back(FixedHeight(20, Middle(heading("Advanced", 10.0f))));
        column.push_back(BuildFlyoutParamRows(advanced, binding));
        height += 6.0f + 20.0f + 38.0f * static_cast<float>(advanced.size());
    }

    column.push_back(VGap(8));
    column.push_back(rule());
    column.push_back(FixedHeight(34, Row({Stretch(), reset})));
    height += 8.0f + 1.0f + 34.0f;

    ShowPopover(Pad(12, 10, 12, 8, Col(column)), anchor, std::min(300.0f, ViewSize().x - 24.0f), height + 18.0f);
}
} // namespace nanoq::ui
