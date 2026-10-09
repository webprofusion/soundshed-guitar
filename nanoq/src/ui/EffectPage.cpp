// The effect page: the selected effect's name, its on/off switch, its presets, and a slider
// (or switch) for each parameter, with the page's own Main / Advanced tabs when it has both.

#include "ui/Shell.h"

#include "uiclient/NodeLabels.h"
#include "uiclient/ParamFormat.h"

#include <algorithm>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

namespace
{
/// The one control the input and output nodes carry: a trim in dB, which the effect catalog
/// does not list since they are not effects.
EffectParamInfo BoundaryTrim()
{
    EffectParamInfo trim;
    trim.key = guitarfx::kBoundaryGainParam;
    trim.name = "Trim";
    trim.unit = "dB";
    trim.minValue = guitarfx::kBoundaryGainMinDb;
    trim.maxValue = guitarfx::kBoundaryGainMaxDb;
    trim.defaultValue = 0.0;
    return trim;
}

constexpr float kCellWidth = 84.0f; // a knob's width; switches and enum steppers match it
constexpr float kCellGap = 10.0f;

/// How many knobs fit a row of the view (less the page's 16 px margins).
int ColumnsFor(float width)
{
    return std::clamp(static_cast<int>((width - 32.0f + kCellGap) / (kCellWidth + kCellGap)), 1, 12);
}
} // namespace

std::string Shell::EffectPageSignature() const
{
    const auto* node = SelectedNode();
    if (node == nullptr)
        return "none";

    std::string s = node->id + "|" + node->type + "|" + NodeDisplayName(mState, *node) + "|" +
                    (node->enabled ? "1" : "0") + "|" + (mShowAdvanced ? "adv" : "main") + "|" +
                    std::to_string(ColumnsFor(mSession.View().size().x));

    if (const auto* type = mState.FindEffectType(node->type))
        s += "|" + std::to_string(type->parameters.size()) + "|" + std::to_string(type->presets.size());
    return s;
}

Ptr Shell::BuildEffectPage()
{
    mControls.clear();
    mBypass.reset();

    const auto* node = SelectedNode();
    if (node == nullptr)
    {
        auto hint = std::make_shared<Text>(mTheme, "Choose an effect in the chain above to see its controls.", 14.0f);
        hint->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
        return Fill(mTheme.Background(), PadXY(24, 48, FixedHeight(24, hint)));
    }

    auto& commands = mSession.Commands();
    const std::string nodeId = node->id;
    const bool boundary = IsBoundaryNode(*node);
    const auto* type = mState.FindEffectType(node->type);

    // ── Header ──
    auto title = std::make_shared<Text>(mTheme, NodeDisplayName(mState, *node), 20.0f, Weight::SemiBold);
    std::string sub = boundary ? "Level trim" : (type != nullptr ? type->name : node->type);
    if (sub == title->Value())
        sub.clear();
    if (const auto badge = NodeArchitectureBadge(mState, *node); !badge.empty())
        sub += (sub.empty() ? "" : "  \xC2\xB7  ") + std::string("NAM ") + badge;
    auto subtitle = std::make_shared<Text>(mTheme, sub, metrics::kSmallText);
    subtitle->Colour(mTheme.TextMuted());

    std::vector<Ptr> header{Col({FixedHeight(26, title), FixedHeight(18, subtitle)}), Stretch(nullptr, 0.001)};

    if (!boundary)
    {
        if (type != nullptr)
        {
            auto presets = std::make_shared<Button>(mTheme, "Presets", [this] { OpenEffectPresets(); }, Button::Style::Plain);
            presets->MinWidth(88);
            header.push_back(presets);
            header.push_back(HGap(8));
        }

        mBypass = std::make_shared<Button>(
            mTheme, node->enabled ? "On" : "Off", [&commands, nodeId, enabled = node->enabled] {
                commands.SetNodeBypassed(nodeId, enabled);
            },
            Button::Style::Plain);
        mBypass->Dot(true).On(node->enabled).MinWidth(84);
        header.push_back(mBypass);
    }

    // ── Which controls ──
    std::vector<EffectParamInfo> params;
    bool anyAdvanced = false;
    if (boundary)
    {
        params.push_back(BoundaryTrim());
    }
    else if (type != nullptr)
    {
        for (const auto& param : type->parameters)
        {
            anyAdvanced = anyAdvanced || param.advanced;
            if (param.advanced == mShowAdvanced)
                params.push_back(param);
        }
    }
    if (!anyAdvanced)
        mShowAdvanced = false;

    std::vector<Ptr> page{FixedHeight(52, PadXY(0, 6, Row(header)))};

    if (anyAdvanced)
    {
        auto tab = [this](const char* label, bool advanced) {
            auto b = std::make_shared<Button>(mTheme, label, [this, advanced] {
                mShowAdvanced = advanced;
                Mark(kPage);
            }, Button::Style::Flat);
            b->Selected(mShowAdvanced == advanced).MinWidth(92);
            return b;
        };
        page.push_back(FixedHeight(44, Row({tab("Main", false), HGap(4), tab("Advanced", true), Stretch()})));
    }

    // ── The controls, in rows of columns ──
    const int columns = ColumnsFor(mSession.View().size().x);
    std::vector<Ptr> rows;
    std::vector<Ptr> current;
    std::string group;

    auto flush = [&] {
        if (current.empty())
            return;
        std::vector<Ptr> spaced;
        for (auto& c : current)
        {
            if (!spaced.empty())
                spaced.push_back(HGap(kCellGap));
            spaced.push_back(c);
        }
        rows.push_back(Row(spaced));
        rows.push_back(VGap(6));
        current.clear();
    };

    for (const auto& info : params)
    {
        // A new group starts a new row under its heading, as in the web app.
        if (info.group != group)
        {
            flush();
            group = info.group;
            if (!group.empty())
                rows.push_back(std::make_shared<GroupHeading>(mTheme, group));
        }

        ParamControl control;
        control.info = info;

        if (IsToggleParam(info) || IsEnumParam(info))
        {
            // A switch, or the label of the enum's current value: a tap steps to the next one.
            control.toggle = std::make_shared<Button>(mTheme, "", [this, nodeId, info] {
                const auto* n = SelectedNode();
                if (n == nullptr)
                    return;
                const double now = NodeParamValue(*n, info);
                double next = 0.0;
                if (IsToggleParam(info))
                {
                    next = now > 0.5 * (info.minValue + info.maxValue) ? info.minValue : info.maxValue;
                }
                else
                {
                    const auto count = std::max<std::size_t>(1, info.labels.size());
                    const auto index = EnumLabelIndex(info, now);
                    next = EnumLabelValue(info, static_cast<std::size_t>((index + 1) % static_cast<int>(count)));
                }
                mSession.Commands().SetNodeParam(nodeId, info.key, next);
                Mark(kValues);
            }, Button::Style::Plain);
            control.toggle->MinWidth(kCellWidth).TextSize(metrics::kSmallText);

            // The name over the button, in a cell as wide as a knob's.
            auto name = std::make_shared<Text>(mTheme, info.name, metrics::kSmallText, Weight::Medium);
            name->Colour(mTheme.TextSecondary()).Alignment(Text::Align::Centre);
            current.push_back(FixedWidth(kCellWidth, Col({FixedHeight(20, name), VGap(6), FixedHeight(32, control.toggle)})));
        }
        else
        {
            control.knob = std::make_shared<ParamKnob>(mTheme, info.name);
            control.knob->DefaultPosition(ParamValueToPosition(info, info.defaultValue));
            control.knob->onChange = [this, nodeId, info, knob = control.knob.get()](double position) {
                const double value = SnapParamValue(info, ParamPositionToValue(info, position));
                knob->SetValueText(FormatParamValue(info, value));
                mSession.Commands().SetNodeParam(nodeId, info.key, value);
            };
            control.knob->onReset = [this, nodeId, info] {
                mSession.Commands().SetNodeParam(nodeId, info.key, info.defaultValue);
                Mark(kValues);
            };
            current.push_back(control.knob);
        }

        mControls.push_back(std::move(control));
        if (static_cast<int>(current.size()) == columns)
            flush();
    }
    flush();

    if (rows.empty())
    {
        auto none = std::make_shared<Text>(mTheme, "This effect has no controls.", 14.0f);
        none->Colour(mTheme.TextMuted());
        rows.push_back(FixedHeight(24, none));
    }

    page.push_back(VScroll(PadXY(0, 4, Col(rows))));

    SyncEffectValues();
    return Fill(mTheme.Background(), Pad(16, 4, 16, 8, Col(page)));
}

void Shell::SyncEffectValues()
{
    const auto* node = SelectedNode();
    if (node == nullptr)
        return;

    for (auto& control : mControls)
    {
        const double value = NodeParamValue(*node, control.info);
        if (control.knob)
        {
            const bool wasDragging = control.knob->Dragging();
            control.knob->Update(ParamValueToPosition(control.info, value), FormatParamValue(control.info, value));
            if (!wasDragging)
                mSession.View().refresh(*control.knob);
        }
        else if (control.toggle)
        {
            const bool toggle = IsToggleParam(control.info);
            const bool on = toggle && value > 0.5 * (control.info.minValue + control.info.maxValue);
            std::string text = toggle ? (on ? "On" : "Off") : std::string();
            if (!toggle)
            {
                if (const int index = EnumLabelIndex(control.info, value);
                    index >= 0 && static_cast<std::size_t>(index) < control.info.labels.size())
                    text = control.info.labels[static_cast<std::size_t>(index)];
            }
            control.toggle->Set(text).On(on).Dot(toggle);
            mSession.View().refresh(*control.toggle);
        }
    }

    if (mBypass)
    {
        mBypass->Set(node->enabled ? "On" : "Off").On(node->enabled);
        mSession.View().refresh(*mBypass);
    }
}

void Shell::OpenEffectPresets()
{
    const auto* node = SelectedNode();
    const auto* type = node != nullptr ? mState.FindEffectType(node->type) : nullptr;
    if (type == nullptr)
        return;

    const std::string nodeId = node->id;

    // The effect's own, and a factory archive's (these choose its models, IRs and blends), as
    // one list in name order; then the user's.
    auto factory = type->presets;
    if (const auto pack = mState.factoryPackEffectPresets.find(node->type); pack != mState.factoryPackEffectPresets.end())
        factory.insert(factory.end(), pack->second.begin(), pack->second.end());
    std::stable_sort(factory.begin(), factory.end(), [](const auto& a, const auto& b) { return a.name < b.name; });

    std::vector<MenuItem> items;
    for (const auto& preset : factory)
        items.push_back({preset.name, false, false, [this, nodeId, preset] {
                             mSession.Commands().ApplyEffectPreset(nodeId, preset);
                         }});

    if (const auto custom = mState.customEffectPresets.find(node->type);
        custom != mState.customEffectPresets.end() && !custom->second.empty())
    {
        items.push_back({"Yours", false, true, {}});
        for (const auto& preset : custom->second)
            items.push_back({preset.name, false, false, [this, nodeId, preset] {
                                 mSession.Commands().ApplyEffectPreset(nodeId, preset);
                             }});
    }

    if (items.empty())
        items.push_back({"This effect has no presets", false, true, {}});

    OpenMenu(type->name + " presets", std::move(items));
}
} // namespace nanoq::ui
