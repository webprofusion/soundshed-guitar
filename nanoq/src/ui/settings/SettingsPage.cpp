// The settings page, as the web UI's Settings (settings-panel.html, settings.css): the sections in
// the shared section list (General, Audio & MIDI, DSP Performance, Features, Help; chips across the
// top at the compact size), each a heading over cards. A card holds rows, a label in capitals at
// the left and its control at the right (a drop-down, a switch, a slider), with a line under it
// saying what it does. General has preset switching, the level targets, Tone3000, the library
// export, software updates and the interface; Audio & MIDI the standalone app's devices
// (DevicePage.cpp); DSP Performance the processing and the amp model (NAM) quality; Features the
// switches every UI follows; Help the guides and what this build is. The controls are bound to the
// engine's settings (mLive), so they follow a change made anywhere, and dragging a slider is not
// interrupted by the setting it just changed coming back. The theme is the footer's, as the web UI's.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/FrameParts.h"
#include "ui/shell/Shell.h"

#include "Paths.h"
#include "uiclient/FeatureFlags.h"
#include "uiclient/ParamFormat.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nanoq::ui
{
namespace
{
constexpr const char* kOutputLimiterSetting = "audio.dsp.outputLimiterEnabled";
constexpr const char* kPresetSwitchTailSetting = "audio.presetSwitch.tailBars";
constexpr int kPresetSwitchTailDefault = 2;
constexpr const char* kNamSlimmableSizeSetting = "audio.nam.slimmableSize";
constexpr const char* kNamOversamplingSetting = "audio.nam.oversampling";
constexpr const char* kNamAntiAliasPhaseSetting = "audio.nam.antiAliasPhase";
constexpr const char* kWebsite = "https://soundshed.com";
constexpr const char* kGuides = "https://guitar.soundshed.com";
constexpr float kLabelWidth = 230.0f; // the longest label, INTERFACE CALIBRATION LEVEL, on one line

using frame::BoolSetting;
using frame::kDensitySetting;
using frame::kTouchControlsSetting;
using frame::NumberSetting;
using frame::StringSetting;
} // namespace

Ptr Shell::BuildSettingsPage()
{
    auto& commands = mSession.Commands();
    const bool standalone = mState.environment.audioDeviceSettings;

    // ── The sections ──
    SectionNav nav;

    for (const auto& [label, which] :
         {std::pair{"General", SettingsSection::General}, std::pair{"Audio & MIDI", SettingsSection::Audio},
          std::pair{"DSP Performance", SettingsSection::Dsp}, std::pair{"Features", SettingsSection::Features},
          std::pair{"Help", SettingsSection::Help}})
    {
        nav.items.push_back({label, mSettingsSection == which, [this, which] {
                                 if (mSettingsSection != which)
                                 {
                                     mSettingsSection = which;
                                     Mark(kPage);
                                 }
                             }});
    }

    // The width a card's text wraps to: the page, less the side list, its margins and the card's.
    const float pageWidth = StageWidth() - (mWide ? 245.0f : 0.0f);
    const float pageSides = mWide ? 2.0f * 24.0f : metrics::kPagePad + 14.0f;
    const float textWidth = std::max(240.0f, pageWidth - pageSides - 2.0f * 16.0f - 14.0f);
    // Below this, a row's label sits over its control instead of beside it: at 640 wide the
    // 230 px labels left the controls about 260.
    const bool stacked = textWidth - kLabelWidth - 12.0f < 300.0f;
    const float labelWidth = stacked ? 0.0f : kLabelWidth + 12.0f;

    std::vector<Ptr> page;
    std::vector<Ptr> card; // the card being filled
    std::string cardTitle;

    // A section's heading (.settings-section h2).
    const auto heading = [&](const char* text) {
        auto t = std::make_shared<Text>(mTheme, text, 20.0f, Weight::SemiBold);
        page.push_back(FixedHeight(40, Middle(t)));
        page.push_back(VGap(6));
    };
    // A card (.settings-card): its title in capitals over its rows, on a raised panel.
    const auto beginCard = [&](const std::string& title) {
        card.clear();
        cardTitle = title;
    };
    const auto endCard = [&] {
        std::vector<Ptr> column;

        if (!cardTitle.empty())
        {
            auto t = std::make_shared<Text>(mTheme, Upper(cardTitle), 13.0f, Weight::SemiBold);
            column.push_back(FixedHeight(24, Middle(t)));
            column.push_back(VGap(6));
        }

        for (auto& part : card)
        {
            column.push_back(std::move(part));
        }

        card.clear();
        // Drawn only: a Surface would take the wheel, and the page would not scroll over a card.
        page.push_back(Rounded(mTheme.Card(), 8.0f, Pad(16, 14, 16, 14, Col(column))));
        page.push_back(VGap(18));
    };
    // A row: the label in capitals at the left, the control at the right.
    const auto row = [&](const char* title, Ptr control, float height = 40.0f) {
        auto l = std::make_shared<Text>(mTheme, Upper(title), 11.0f, Weight::SemiBold);
        l->Colour(mTheme.TextSecondary());

        if (stacked)
        {
            card.push_back(FixedHeight(18, l));
            card.push_back(FixedHeight(height, control));
            return;
        }

        card.push_back(FixedHeight(height, Row({FixedWidth(kLabelWidth, Middle(l)), HGap(12), control})));
    };
    // What a row does, under it.
    const auto describe = [&](const std::string& text) {
        auto p = std::make_shared<Paragraph>(mTheme, text, textWidth, 11.5f);
        p->Colour(mTheme.TextSecondary());
        card.push_back(Row({el::share(el::hold_any(p)), Stretch()}));
        card.push_back(VGap(10));
    };
    // A drop-down: `get` says which option shows, `set` hears the one chosen.
    const auto pick = [&](const char* title, std::vector<std::string> options, std::function<int()> get,
                          std::function<void(int)> set) {
        auto field = std::make_shared<PickerField>(mTheme, mIcons);
        field->AccessName(title);
        mLive.Bind(field, get, [options](PickerField& f, int i) {
            f.Set(i >= 0 && i < static_cast<int>(options.size()) ? options[static_cast<std::size_t>(i)] : "");
        });
        field->onClick = [this, options, get, set, weak = std::weak_ptr<PickerField>(field)] {
            auto f = weak.lock();

            if (!f)
            {
                return;
            }

            std::vector<MenuItem> items;
            const int current = get();

            for (int i = 0; i < static_cast<int>(options.size()); ++i)
            {
                items.push_back({options[static_cast<std::size_t>(i)], i == current, false, [set, i] { set(i); }});
            }

            OpenDropdown(f->Bounds(), std::move(items), std::max(160.0f, f->Bounds().width()));
        };
        row(title, Middle(field));
    };
    // A switch (.toggle-switch).
    const auto toggle = [&](const char* title, std::function<bool()> get, std::function<void(bool)> set) {
        auto sw = std::make_shared<ToggleSwitch>(mTheme);
        sw->AccessName(title);
        mLive.Bind(sw, get, &ToggleSwitch::Set);
        sw->onClick = [get, set] { set(!get()); };
        row(title, Row({Middle(sw), Stretch()}));
    };
    const auto boolOr = [this](const char* key, bool def) { return BoolSetting(mState.appSettings, key, def); };
    const auto setBool = [&commands](const char* key) {
        return [&commands, key](bool on) { commands.SetSetting(key, on); };
    };
    // A slider over [lo, hi] for a number setting, with its unit and decimals; a double click on the
    // slider puts `def` back.
    const auto slider = [&](const char* title, const char* key, double lo, double hi, double def, const char* unit,
                            int decimals) {
        auto sl = std::make_shared<ParamSlider>(mTheme, "");
        sl->AccessName(title);
        sl->onChange = [&commands, key, lo, hi, decimals](double v) {
            const double scale = std::pow(10.0, decimals);
            commands.SetSetting(key, std::round((lo + v * (hi - lo)) * scale) / scale);
        };
        sl->onReset = [&commands, key, def] { commands.SetSetting(key, def); };
        mLive.Bind(
            sl, [this, key, lo, hi, def] { return std::clamp(NumberSetting(mState.appSettings, key, def), lo, hi); },
            [lo, hi, unit, decimals](ParamSlider& s, double v) {
                char text[48];
                std::snprintf(text, sizeof(text), "%.*f %s", decimals, v, unit);
                s.Update((v - lo) / (hi - lo), text);
            });
        row(title, Row({FixedWidth(std::min(420.0f, textWidth - labelWidth), Middle(sl)), Stretch()}), 52.0f);
    };
    const auto button = [&](const std::string& text, std::function<void()> onClick, bool primary = false) {
        auto b = std::make_shared<Button>(mTheme, text, std::move(onClick),
                                          primary ? Button::Style::Primary : Button::Style::Plain);
        b->MinWidth(160).TextSize(metrics::kSmallText);
        return Row({Middle(FixedHeight(32, b)), Stretch()});
    };

    switch (mSettingsSection)
    {
    case SettingsSection::General: {
        heading("DSP");
        beginCard("Preset Switching");
        pick(
            "Carry Delay & Reverb Tails", {"Off", "1 bar", "2 bars", "3 bars", "4 bars"},
            [this] {
                return std::clamp(static_cast<int>(NumberSetting(mState.appSettings, kPresetSwitchTailSetting,
                                                                 kPresetSwitchTailDefault)),
                                  0, 4);
            },
            [&commands](int i) { commands.SetSetting(kPresetSwitchTailSetting, i); });
        describe("Lets the preset you just left keep ringing out over the new one. Its input fades away, and its "
                 "delay and reverb state is kept for the selected number of bars before fading out. Bars are counted "
                 "at the current tempo, and each preset still ringing costs CPU.");
        endCard();

        beginCard("Advanced DSP Level Targets");
        describe("Target level when auto levelling signal chain elements.");
        slider("Output Protection Ceiling", "audio.dsp.outputProtectionCeilingDbfs", -6.0, 0.0, -1.0, "dBFS", 1);
        describe("The level the Output Limiter holds the mix under.");
        toggle(
            "Output Limiter", [boolOr] { return boolOr(kOutputLimiterSetting, false); },
            setBool(kOutputLimiterSetting));
        describe("Catches peaks at the very end of the chain, at the ceiling above. Applies to the whole output, not "
                 "to any one preset.");
        slider("Nominal Level", "audio.dsp.nominalOperatingLevelDbfs", -30.0, -6.0, -18.0, "dBFS", 1);
        describe("The level the effects are voiced for.");
        toggle(
            "Full Level on Mono Outputs", [boolOr] { return boolOr("audio.dsp.monoOutputUnity", false); },
            setBool("audio.dsp.monoOutputUnity"));
        describe("A centred rig sits 3 dB below its input, so that it keeps its loudness when panned across a stereo "
                 "output. A mono output has nothing to pan across: this brings the rig back to its input level there. "
                 "Stereo outputs are unchanged.");
        toggle(
            "NAM Auto Input Calibration", [boolOr] { return boolOr("audio.nam.autoInputCalibration", true); },
            setBool("audio.nam.autoInputCalibration"));
        describe("Automatically correct NAM input level using model metadata and the interface calibration level "
                 "below.");
        slider("Interface Calibration Level", "audio.nam.interfaceCalibrationLevelDbu", 0.0, 24.0, 12.0, "dBu", 1);
        describe("Reference level of your audio interface (12 dBu = typical prosumer, 18 dBu = professional).");
        endCard();

        if (guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::Tone3000))
        {
            heading("Tone3000");
            beginCard("Tone3000 API");
            toggle(
                "Use Soundshed Tone Search API",
                [boolOr] { return boolOr("tone3000.useSoundshedToneSearchApi", true); },
                setBool("tone3000.useSoundshedToneSearchApi"));
            describe("Tone3000 searches go through Soundshed's relay. Off, they use your own Tone3000 API key, set in "
                     "Soundshed Guitar's Settings.");
            endCard();
        }

        heading("Library");
        beginCard("Export Presets & Blends");
        pick(
            "Resources", {"Only resources used by presets", "All resources in library"},
            [this] { return mLibraryExportAll ? 1 : 0; }, [this](int i) { mLibraryExportAll = i == 1; });
        card.push_back(FixedHeight(
            40, button("Export Library", [this, &commands] { commands.ExportLibraryArchive(mLibraryExportAll); })));
        describe("Your presets and every blend, with their models and IRs or the whole library, as a "
                 ".soundshed-library.zip.");
        endCard();

        heading("Software Updates");
        beginCard("Current Version");
        {
            auto v = std::make_shared<Text>(mTheme,
                                            mState.environment.version.empty() ? std::string("Soundshed engine")
                                                                               : mState.environment.version,
                                            metrics::kTextSize, Weight::Medium);
            row("Soundshed Nano Q", Middle(v));
        }
        endCard();
        beginCard("App Updates");
        toggle(
            "Check for updates on startup", [boolOr] { return boolOr("app.updateCheckEnabled", true); },
            setBool("app.updateCheckEnabled"));

        if (mUpdate)
        {
            card.push_back(FixedHeight(
                44, button("Version " + mUpdate->version + " is available...", [this] { OpenUpdateDialog(); }, true)));
        }

        endCard();

        heading("UI");
        beginCard("Layout");
        pick(
            "Layout Density", {"Auto layout", "Compact", "Full"},
            [this] {
                const auto density = StringSetting(mState.appSettings, kDensitySetting, "auto");
                return density == "compact" ? 1 : density == "full" ? 2 : 0;
            },
            [&commands](int i) {
                commands.SetSetting(kDensitySetting, i == 1 ? "compact" : i == 2 ? "full" : "auto");
            });
        // Hit areas for a finger (Shell::TouchSizes): Auto has them on a touch-first device.
        pick(
            "Touch Sizes", {"Auto", "On", "Off"},
            [this] {
                const auto touch = StringSetting(mState.appSettings, kTouchControlsSetting, "auto");
                return touch == "on" ? 1 : touch == "off" ? 2 : 0;
            },
            [&commands](int i) {
                commands.SetSetting(kTouchControlsSetting, i == 1 ? "on" : i == 2 ? "off" : "auto");
            });
        endCard();
        beginCard("Controls");
        toggle(
            "Mouse wheel adjusts controls", [boolOr] { return boolOr("ui.mouseWheelAdjustsControls", true); },
            setBool("ui.mouseWheelAdjustsControls"));
        endCard();
        beginCard("Signal Chain");
        toggle(
            "Animate signal flow", [boolOr] { return boolOr("ui.signalFlowAnimation", false); },
            setBool("ui.signalFlowAnimation"));
        endCard();
        break;
    }
    case SettingsSection::Audio: {
        heading("Audio & MIDI");

        // The devices are the standalone app's (DevicePage.cpp); in a plugin the host sets them.
        beginCard("");

        if (standalone)
        {
            card.push_back(BuildDeviceSection());
        }
        else
        {
            card.push_back(
                FixedHeight(24, Middle(std::make_shared<Text>(mTheme, "AUDIO DEVICE", 13.0f, Weight::SemiBold))));
            describe("The audio device is set by the host, or in the window menu (Alt+Space) of the standalone app.");
        }

        endCard();
        beginCard("Signal Path");
        toggle(
            "Show Effect Channels Choice", [boolOr] { return boolOr("audio.signalPath.channelModeControl", false); },
            setBool("audio.signalPath.channelModeControl"));
        describe("Shows the channel choice (stereo, left, right) on each effect.");
        endCard();
        break;
    }
    case SettingsSection::Dsp: {
        heading("DSP Performance");
        beginCard("Processing");
        {
            // The standalone app and the plugins keep their own choice, as in the web UI.
            const char* key = mState.environment.standalone ? "audio.processing.multiCore.standalone"
                                                            : "audio.processing.multiCore.plugin";
            toggle("Use Multiple CPU Cores", [boolOr, key] { return boolOr(key, true); }, setBool(key));
            describe("Spreads the chain over more cores where it can run in parallel.");
        }
        toggle(
            "Fast NAM Engine", [boolOr] { return boolOr("audio.nam.fusedEngine", true); },
            setBool("audio.nam.fusedEngine"));
        describe("A fused pass through a model where the model allows it.");
        endCard();

        beginCard("NAM Processing Quality");
        {
            auto quality = std::make_shared<ParamSlider>(mTheme, "");
            quality->AccessName("NAM Processing Quality");
            quality->onChange = [&commands](double v) {
                commands.SetSetting(kNamSlimmableSizeSetting, std::round(v * 100.0) / 100.0);
            };
            quality->onReset = [&commands] { commands.SetSetting(kNamSlimmableSizeSetting, 1.0); };
            mLive.Bind(
                quality,
                [this] {
                    return std::clamp(NumberSetting(mState.appSettings, kNamSlimmableSizeSetting, 1.0), 0.0, 1.0);
                },
                [](ParamSlider& s, double v) {
                    s.Update(v, std::to_string(static_cast<int>(std::lround(v * 100.0))) + "%");
                });
            row("Adjust Quality",
                Row({FixedWidth(std::min(420.0f, textWidth - labelWidth), Middle(quality)), Stretch()}), 52.0f);
        }
        describe("Lower quality reduces CPU use on models that support it. Watch the DSP load while you adjust it.");
        pick(
            "Oversampling", {"Off", "2x", "4x", "8x", "16x", "32x"},
            [this] {
                return std::clamp(static_cast<int>(NumberSetting(mState.appSettings, kNamOversamplingSetting, 0)), 0,
                                  5);
            },
            [&commands](int i) { commands.SetSetting(kNamOversamplingSetting, i); });
        pick(
            "AA Filter", {"Minimum Phase", "Linear Short", "Linear Long"},
            [this] {
                return std::clamp(static_cast<int>(NumberSetting(mState.appSettings, kNamAntiAliasPhaseSetting, 0)), 0,
                                  2);
            },
            [&commands](int i) { commands.SetSetting(kNamAntiAliasPhaseSetting, i); });
        endCard();
        break;
    }
    case SettingsSection::Features: {
        heading("Feature Toggles");

        // Core features are on unless switched off; power features off unless switched on.
        for (const bool core : {true, false})
        {
            beginCard(core ? "Core Features" : "Power Features");

            for (const auto feature : guitarfx::uiclient::AllFeatures())
            {
                if (guitarfx::uiclient::FeatureDefault(feature) != core)
                {
                    continue;
                }

                const char* key = guitarfx::uiclient::FeatureSettingKey(feature);
                toggle(
                    guitarfx::uiclient::FeatureLabel(feature),
                    [this, feature] { return guitarfx::uiclient::IsFeatureEnabled(mState, feature); }, setBool(key));
            }

            endCard();
        }

        beginCard("");
        describe("Shared with Soundshed Guitar. Some features (the AI Tone Builder, the editors) are there only.");
        endCard();
        break;
    }
    case SettingsSection::Help: {
        heading("Help");
        const auto topic = [&](const char* title, const char* text) {
            beginCard(title);
            describe(text);
            endCard();
        };
        topic("Signal Path",
              "Click a card to edit its effect, and drag it to move it, onto a cable or into a branch. The power "
              "button bypasses an effect: it stays in the preset, faded, and is saved with it; Delete takes it out. "
              "In the full layout, drag the bar under the chain up for icon tiles and down again for cards.");
        topic("Presets and Scenes",
              "Scenes are variations stored inside one preset, so switching between them does not reload it. "
              "Multi-Rig runs two or more presets at once, as tabs over the chain with a Mix tab; saving the mix "
              "stores it as a Multi-Rig preset.");
        topic("Keys", "Ctrl+Z and Ctrl+Y undo and redo chain edits. Space bypasses the effect clicked last, or taps "
                      "the tempo; Delete removes it, or clears the pad playing on the pads; F2 renames the scene; Esc "
                      "closes a menu or dialog.");
        topic("Library and Sharing",
              "Tone3000 results come through the Soundshed API by default; your own Tone3000 key is set in Soundshed "
              "Guitar's Settings. Tones installed from tone sharing bring the models they need with them. Publishing "
              "is in Soundshed Guitar.");
        topic("Performance and Diagnostics",
              "DSP load measures the audio thread against its block deadline, not the whole CPU, so it can sit near "
              "100% while the machine reports a low overall load. In an effect's DSP readouts a blank figure means it "
              "did not run in the last block (bypassed, or fed no signal) rather than a measured zero.");

        beginCard("More Guides");
        describe("Full walkthroughs and release notes are at guitar.soundshed.com.");
        card.push_back(FixedHeight(40, button("guitar.soundshed.com", [&commands] { commands.OpenUrl(kGuides); })));
        endCard();

        beginCard("About");
        const auto line = [&](const char* title, const std::string& value) {
            auto v = std::make_shared<Text>(mTheme, value, metrics::kTextSize, Weight::Medium);
            row(title, Middle(v), 30.0f);
        };
        line("Product", "Soundshed Nano Q");
        line("Engine", mState.environment.version.empty() ? "Soundshed engine" : mState.environment.version);
        line("Interface", "QPlug and Elements (no JUCE)");
        // As UTF-8, which the text expects: path::string() converts through the ANSI code page on
        // Windows, where a user name that page cannot spell throws, and opening Settings took the
        // app down with it.
        line("Profile", guitarfx::util::PathToUtf8(ProfileFolder()));
        card.push_back(VGap(6));
        card.push_back(FixedHeight(40, button("soundshed.com", [&commands] { commands.OpenUrl(kWebsite); })));
        describe("Riff capture, plugin hosting, preset blending and publishing are in Soundshed Guitar, which shares "
                 "this app's presets, library and settings.");
        endCard();
        break;
    }
    }

    auto content = VScroll(mWide ? Pad(24, 20, 24, 12, Col(page))
                                 : Pad(metrics::kPagePad, metrics::kPagePadTop, 14, 12, Col(page)));
    return Fill(mTheme.Background(), WithSectionNav(std::move(nav), content));
}
} // namespace nanoq::ui
