// A settings flyout's parameter rows, as the web UI builds them (flyoutParamControls.ts): one row
// per parameter from the engine's own definitions, label | control | value. The control is a
// small knob, a switch for a two-way parameter, or a picker for one the engine marks
// `control: "picker"`, which spans the control and value columns. A binding says where values
// come from and where a change goes, so the same rows serve the global gate and Pickup Match.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "uiclient/ParamFormat.h"

namespace nanoq::ui
{
using guitarfx::uiclient::EffectParamInfo;

Ptr Shell::BuildFlyoutParamRows(const std::vector<EffectParamInfo>& params, const FlyoutBinding& binding)
{
    namespace pf = guitarfx::uiclient;
    std::vector<Ptr> rows;

    for (const auto& info : params)
    {
        auto label = std::make_shared<Text>(mTheme, info.name.empty() ? info.key : info.name, 12.0f, Weight::Medium);
        label->Colour(mTheme.TextSecondary());
        auto value = std::make_shared<Text>(mTheme, "", 11.0f, Weight::Medium);
        value->Colour(mTheme.TextMuted()).Alignment(HAlign::Right);
        const auto current = [info, read = binding.value] { return read(info); };
        mLive.Bind(value, [current, info] { return pf::FormatFlyoutParamValue(info, current()); }, &Text::Set);
        Ptr control;

        if (info.control == "picker" && !info.labels.empty())
        {
            auto picker = std::make_shared<PickerField>(mTheme, mIcons);
            picker->AccessName(info.name);
            picker->onClick = [this, info, current, apply = binding.apply, weak = std::weak_ptr<PickerField>(picker)] {
                const auto p = weak.lock();

                if (!p)
                {
                    return;
                }

                std::vector<MenuItem> items;
                const int chosen = pf::EnumLabelIndex(info, current());

                for (std::size_t i = 0; i < info.labels.size(); ++i)
                {
                    items.push_back({info.labels[i], static_cast<int>(i) == chosen, false,
                                     [info, apply, i] { apply(info.key, pf::EnumLabelValue(info, i), true); }});
                }

                OpenDropdown(p->Bounds(), std::move(items), std::max(180.0f, p->Bounds().width()));
            };
            mLive.Bind(
                picker, [current, info] { return pf::FormatFlyoutParamValue(info, current()); }, &PickerField::Set);
            rows.push_back(FixedHeight(38, Row({Middle(label), HGap(8), FixedWidth(150, Middle(picker))})));
            continue;
        }

        if (pf::IsSwitchParam(info))
        {
            auto toggle = std::make_shared<ToggleSwitch>(mTheme);
            toggle->AccessName(info.name);
            toggle->onClick = [info, current, apply = binding.apply] {
                apply(info.key, current() >= 0.5 ? info.minValue : info.maxValue, true);
            };
            mLive.Bind(toggle, [current] { return current() >= 0.5; }, &ToggleSwitch::Set);
            control = toggle;
        }
        else
        {
            auto knob = std::make_shared<ParamKnob>(mTheme, info.name);
            knob->Bare(30.0f).DefaultPosition(pf::ParamValueToPosition(info, info.defaultValue));
            knob->onChange = [info, apply = binding.apply](double position) {
                apply(info.key, pf::SnapParamValue(info, pf::ParamPositionToValue(info, position)), true);
            };
            knob->onReset = [info, apply = binding.apply] { apply(info.key, info.defaultValue, true); };
            mLive.Bind(knob, current, [info](ParamKnob& k, double v) {
                k.Update(pf::ParamValueToPosition(info, v), pf::FormatFlyoutParamValue(info, v));
            });
            control = knob;
        }

        rows.push_back(FixedHeight(
            38, Row({Middle(label), HGap(8), FixedWidth(40, Centred(control)), FixedWidth(76, Middle(value))})));
    }

    return Col(rows);
}
} // namespace nanoq::ui
