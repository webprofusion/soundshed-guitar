// The settings page, as the web UI's Settings (settings-panel.html, settings.css): the sections in
// the shared section list (General, Audio & MIDI, DSP Performance, UI, Help; chips across the top at
// the compact size), each a heading over cards, and in DSP Performance and UI a row of tabs under
// the heading. A card holds rows, a label in capitals at the left and its control at the right (a
// drop-down, a switch, a slider), with a line under it saying what it does. General has software
// updates, Tone3000 and the library export; Audio & MIDI the standalone app's devices
// (DevicePage.cpp); DSP Performance the load over its tabs, then Diagnostics (DspPerformance.cpp),
// Processing (the cores and the amp model (NAM) quality) and Levels & Switching (preset switching
// and the level targets); UI the Interface (layout, controls, the native UI switch) and the
// Features every UI follows; Help the guides and what this build is. The controls are bound to the
// engine's settings (mLive), so they follow a change made anywhere, and dragging a slider is not
// interrupted by the setting it just changed coming back. The theme is the footer's, as the web UI's.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/FrameParts.h"
#include "ui/shell/Shell.h"
#include "ui/widgets/ControlSkin.h"

#include "IPluginHost.h"
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

/// A value shown in a read-only field (.version-display): fixed-pitch 12 px text in the secondary
/// ink, on the second background with a 1 px border.
class ReadOnlyField final : public el::element
{
  public:
    ReadOnlyField(const Theme& theme, std::string text) : mTheme(theme), mText(std::move(text))
    {
    }

    el::view_limits limits(const el::basic_context& ctx) const override
    {
        const float w = el::measure_text(ctx.canvas, mText, MonoFont().size(12.0f)).x + 22.0f;
        return {{w, 28.0f}, {w, 28.0f}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(mTheme.Bar());
        cnv.fill_round_rect(b, 4.0f);
        cnv.stroke_style(mTheme.Border());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 4.0f);
        FillText(cnv, mText, MonoFont().size(12.0f), mTheme.TextSecondary(), b.inset(10.0f, 0.0f), HAlign::Left);
    }

  private:
    const Theme& mTheme;
    std::string mText;
};
} // namespace

Ptr Shell::BuildSettingsTabs(std::initializer_list<const char*> labels, int& tab)
{
    std::vector<Ptr> row;
    int index = 0;

    for (const char* label : labels)
    {
        auto t = std::make_shared<TextTab>(mTheme, label, tab == index, 13.0f, 28.0f, 34.0f);
        t->onClick = [this, which = index, current = &tab] {
            if (*current != which)
            {
                *current = which;
                Mark(kPage);
            }
        };
        row.push_back(t);
        ++index;
    }

    row.push_back(Stretch());
    return FixedHeight(34, Row(row));
}

Ptr Shell::BuildSettingsPage()
{
    auto& commands = mSession.Commands();
    const bool standalone = mState.environment.audioDeviceSettings;
    // As Soundshed Guitar's native editor, this is Soundshed Guitar.
    const std::string productName = mState.environment.nativeEditor ? "Soundshed Guitar" : "Soundshed Nano Q";

    // ── The sections ──
    SectionNav nav;

    for (const auto& [label, which] :
         {std::pair{"General", SettingsSection::General}, std::pair{"Audio & MIDI", SettingsSection::Audio},
          std::pair{"DSP Performance", SettingsSection::Dsp}, std::pair{"User Interface", SettingsSection::Ui},
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

    // An update waiting: its banner over every section.
    if (auto banner = BuildUpdateBanner(textWidth))
    {
        page.push_back(banner);
        page.push_back(VGap(18));
    }

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
    const auto picker = [&](const char* title, std::vector<std::string> options, std::function<int()> get,
                            std::function<void(int)> set) -> Ptr {
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
        return Middle(field);
    };
    const auto pick = [&](const char* title, std::vector<std::string> options, std::function<int()> get,
                          std::function<void(int)> set) { row(title, picker(title, std::move(options), get, set)); };
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
    // A number field with its step arrows and unit (the web UI's number inputs), for a setting
    // over [lo, hi]: the arrows step it by `step` (Shift a tenth of that), or a number is typed in.
    const auto number = [&](const char* title, const char* key, double lo, double hi, double def, double step,
                            const char* unit, int decimals) {
        auto field = std::make_shared<Stepper>(mTheme, 96.0f);
        field->AccessName(std::string(title) + ", " + unit);
        const auto set = [&commands, key, lo, hi, decimals](double v) {
            const double scale = std::pow(10.0, decimals);
            commands.SetSetting(key, std::round(std::clamp(v, lo, hi) * scale) / scale);
        };
        const auto get = [this, key, lo, hi, def] {
            return std::clamp(NumberSetting(mState.appSettings, key, def), lo, hi);
        };
        const auto format = [decimals](double v) {
            char text[32];
            std::snprintf(text, sizeof(text), "%.*f", decimals, v);
            return std::string(text);
        };
        field->onStep = [get, set, step](int direction, bool fine) {
            set(get() + direction * (fine ? step * 0.1 : step));
        };
        field->editText = [get, format] { return format(get()); };
        field->onTyped = [set](const std::string& typed) {
            char* end = nullptr;
            const double v = std::strtod(typed.c_str(), &end);

            if (end != typed.c_str() && std::isfinite(v))
            {
                set(v);
            }
        };
        mLive.Bind(field, get, [format](Stepper& f, double v) { f.Set(format(v)); });
        auto u = std::make_shared<Text>(mTheme, unit, metrics::kSmallText);
        u->Colour(mTheme.TextSecondary());
        row(title, Row({Middle(field), HGap(8), Middle(u), Stretch()}), 44.0f);
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
        heading("Software Updates");
        beginCard("Current Version");
        row(productName.c_str(),
            Row({Middle(std::make_shared<ReadOnlyField>(
                     mTheme, mState.environment.version.empty() ? std::string("Unknown") : mState.environment.version)),
                 Stretch()}));
        endCard();
        beginCard("App Updates");
        toggle(
            "Check for updates on startup", [boolOr] { return boolOr("app.updateCheckEnabled", true); },
            setBool("app.updateCheckEnabled"));
        describe("Automatically checks if a new version of " + productName + " is available.");

        if (mUpdate)
        {
            card.push_back(FixedHeight(
                44, button("Version " + mUpdate->version + " is available...", [this] { OpenUpdateDialog(); }, true)));
        }

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

        // While the Resource Library is on: the drop-down, the button and the file's kind on one row.
        if (guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::ResourceLibrary))
        {
            heading("Library");
            beginCard("Export Presets & Blends");
            {
                auto export_ = std::make_shared<Button>(
                    mTheme, "Export Library", [this, &commands] { commands.ExportLibraryArchive(mLibraryExportAll); },
                    Button::Style::Plain);
                export_->TextSize(metrics::kSmallText);
                auto kind = std::make_shared<Text>(mTheme, ".soundshed-library.zip", metrics::kSmallText);
                kind->Colour(mTheme.TextSecondary());
                row("Resources", Row({picker(
                                          "Resources", {"Only resources used by presets", "All resources in library"},
                                          [this] { return mLibraryExportAll ? 1 : 0; },
                                          [this](int i) { mLibraryExportAll = i == 1; }),
                                      HGap(8), Middle(FixedHeight(32, export_)), HGap(8), Middle(kind), Stretch()}));
            }
            describe("Exports your presets and every blend, plus IR/NAM files from the library based on the selected "
                     "scope.");
            endCard();
        }

        break;
    }
    case SettingsSection::Audio: {
        heading("Audio & MIDI");

        // The devices are the standalone app's (DevicePage.cpp); in a plugin the host sets them.
        // The Signal Path Test is under them in both.
        beginCard("Audio Device");

        if (standalone)
        {
            card.push_back(BuildDeviceSection());
        }
        else
        {
            describe("The host chooses the audio device. The standalone app has its own device settings here.");
        }

        row("Signal Path Test", button("Run Signal Path Test", [&commands] { commands.RunSignalPathTest(); }), 44.0f);
        endCard();

        if (standalone && mState.device.known)
        {
            beginCard("MIDI Devices");
            card.push_back(BuildDeviceSection(true));
            endCard();
        }

        beginCard("Signal Path");
        toggle(
            "Show Effect Channels Choice", [boolOr] { return boolOr("audio.signalPath.channelModeControl", false); },
            setBool("audio.signalPath.channelModeControl"));
        describe("Adds a Channels choice to each effect's header: Follow takes the stereo or mono signal it is "
                 "given, Mono folds a stereo input to one channel and runs the effect mono, at half the CPU.");
        endCard();
        break;
    }
    case SettingsSection::Dsp: {
        heading("DSP Performance");
        page.push_back(BuildDspFigures());
        page.push_back(VGap(4));
        {
            auto p =
                std::make_shared<Paragraph>(mTheme,
                                            "DSP Load is the share of the audio thread's time per block (typically "
                                            "one core), not your whole CPU. Peak Load is the worst block lately.",
                                            textWidth + 32.0f, 11.5f);
            p->Colour(mTheme.TextSecondary());
            page.push_back(Row({el::share(el::hold_any(p)), Stretch()}));
        }
        page.push_back(VGap(10));
        page.push_back(BuildSettingsTabs({"Diagnostics", "Processing", "Levels & Switching"}, mDspSettingsTab));
        page.push_back(VGap(12));

        if (mDspSettingsTab == 0)
        {
            {
                auto p = std::make_shared<Paragraph>(mTheme,
                                                     "Signal levels, headroom and clipping, reported continuously for "
                                                     "the chain's input and output and for each node.",
                                                     textWidth + 32.0f, 11.5f);
                p->Colour(mTheme.TextSecondary());
                page.push_back(Row({el::share(el::hold_any(p)), Stretch()}));
                page.push_back(VGap(10));
            }
            beginCard("");
            card.push_back(BuildDspDiagnostics(textWidth));
            endCard();
        }
        else if (mDspSettingsTab == 1)
        {
            beginCard("Processing");
            {
                // The standalone app and the plugins keep their own choice, as in the web UI.
                const char* key = mState.environment.standalone ? "audio.processing.multiCore.standalone"
                                                                : "audio.processing.multiCore.plugin";
                // On by default in the standalone app only, as the engine has it (SettingsKeys.h).
                const bool byDefault = mState.environment.standalone;
                toggle(
                    "Use Multiple CPU Cores", [boolOr, key, byDefault] { return boolOr(key, byDefault); },
                    setBool(key));
                describe("Spreads heavy chains (stereo NAM and IR, split chains, several presets at once) over extra "
                         "helper threads. On by default in the standalone app. Off by default in a DAW, which "
                         "already shares its tracks across your cores: there the extra threads compete with it and "
                         "can cause crackles. The standalone app and the plugin keep separate settings, and the "
                         "plugin's applies to every instance.");
            }
            toggle(
                "Fast NAM Engine", [boolOr] { return boolOr("audio.nam.fusedEngine", true); },
                setBool("audio.nam.fusedEngine"));
            describe("Runs NAM amp and effect models on Soundshed's optimised engine, which typically uses a half to "
                     "a fifth of the CPU of the standard NAM engine for the same sound. Turn it off to compare the "
                     "two, or if a model behaves differently. Changing it rebuilds the running presets, and in a DAW "
                     "it applies to every instance.");
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
                    [](ParamSlider& s, double v) { s.Update(v, ""); });
                // Min and Max at its ends (.nam-quality-endpoint), not a figure.
                const auto end = [this](const char* text) {
                    auto t = std::make_shared<Text>(mTheme, text, metrics::kSmallText);
                    t->Colour(mTheme.TextSecondary());
                    // Level with the track, under the slider's (empty) value line.
                    return Pad(0, 16, 0, 0, Middle(t));
                };
                row("Adjust Quality",
                    Row({end("Min"), HGap(10),
                         FixedWidth(std::min(360.0f, textWidth - labelWidth - 80.0f), Middle(quality)), HGap(10),
                         end("Max"), Stretch()}),
                    52.0f);
            }
            describe("Adjust quality while watching DSP Load and Peak Load above. Lower quality reduces CPU use.");
            pick(
                "Oversampling", {"Off", "2x", "4x", "8x", "16x", "32x"},
                [this] {
                    return std::clamp(static_cast<int>(NumberSetting(mState.appSettings, kNamOversamplingSetting, 0)),
                                      0, 5);
                },
                [&commands](int i) { commands.SetSetting(kNamOversamplingSetting, i); });
            describe("Renders NAM models at a multiple of their native rate to reduce aliasing on high-gain models. "
                     "Each step roughly multiplies NAM CPU cost, and applies to every NAM node in every preset.");
            pick(
                "AA Filter", {"Minimum Phase", "Linear Short", "Linear Long"},
                [this] {
                    return std::clamp(static_cast<int>(NumberSetting(mState.appSettings, kNamAntiAliasPhaseSetting, 0)),
                                      0, 2);
                },
                [&commands](int i) { commands.SetSetting(kNamAntiAliasPhaseSetting, i); });
            describe("Anti-alias filter for the NAM resampler. Minimum Phase is the low-latency real-time default; "
                     "the linear options are phase-linear but add latency. This also applies when oversampling is "
                     "Off if your sample rate differs from the model's native rate.");
            endCard();
        }
        else
        {
            beginCard("Preset Switching");
            pick(
                "Carry Delay & Reverb Tails", {"Off", "1 bar", "2 bars", "3 bars", "4 bars"},
                [this] {
                    return std::clamp(static_cast<int>(NumberSetting(mState.appSettings, kPresetSwitchTailSetting,
                                                                     kPresetSwitchTailDefault)),
                                      0, 4);
                },
                [&commands](int i) { commands.SetSetting(kPresetSwitchTailSetting, i); });
            describe(
                "Lets the preset you just left keep ringing out over the new one. Its input fades away, and its "
                "delay and reverb state is kept for the selected number of bars before fading out. Bars are counted "
                "at the current tempo, and each preset still ringing costs CPU. Switching again may end an older tail "
                "sooner.");
            endCard();

            beginCard("Advanced DSP Level Targets");
            describe("Target level when auto levelling signal chain elements.");
            number("Output Protection Ceiling", "audio.dsp.outputProtectionCeilingDbfs", -6.0, 0.0, -1.0, 0.1, "dBFS",
                   1);
            describe("The level the Output Limiter holds the mix under.");
            toggle(
                "Output Limiter", [boolOr] { return boolOr(kOutputLimiterSetting, false); },
                setBool(kOutputLimiterSetting));
            describe(
                "Catches peaks at the very end of the chain, at the ceiling above. Applies to the whole output, not "
                "to any one preset.");
            toggle(
                "Full Level on Mono Outputs", [boolOr] { return boolOr("audio.dsp.monoOutputUnity", false); },
                setBool("audio.dsp.monoOutputUnity"));
            {
                // In a plugin, what this instance runs: it keeps the value it was added with.
                std::string text =
                    "A centred rig sits 3 dB below its input, so that it keeps its loudness when panned across a "
                    "stereo output. A mono output, such as a mono DAW track or a single interface output, has nothing "
                    "to pan across: this brings the rig back to its input level there. Stereo outputs are unchanged. "
                    "In a DAW it applies to instances you add after turning it on, and saved projects keep the level "
                    "they were made with.";
                const auto instance = mState.mixer.monoOutputUnity;

                if (!mState.environment.standalone && instance)
                {
                    const bool setting = boolOr("audio.dsp.monoOutputUnity", false);
                    text += *instance == setting
                                ? std::string(" This instance: ") + (*instance ? "on." : "off.")
                                : std::string(" This instance keeps its own value (") + (*instance ? "on" : "off") +
                                      "); the setting applies to instances added from now on.";
                }

                describe(text);
            }
            toggle(
                "NAM Auto Input Calibration", [boolOr] { return boolOr("audio.nam.autoInputCalibration", true); },
                setBool("audio.nam.autoInputCalibration"));
            describe("Automatically correct NAM input level using model metadata and the interface calibration level "
                     "below.");
            number("Interface Calibration Level", "audio.nam.interfaceCalibrationLevelDbu", 0.0, 24.0, 12.0, 0.5, "dBu",
                   1);
            describe("Reference level of your audio interface (12 dBu = typical prosumer, 18 dBu = professional).");
            endCard();
        }

        break;
    }
    case SettingsSection::Ui: {
        heading("User Interface");
        page.push_back(BuildSettingsTabs({"Interface", "Features"}, mUiSettingsTab));
        page.push_back(VGap(12));

        if (mUiSettingsTab == 0)
        {
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
            describe("Auto adapts the interface to the available window size. Choose Compact or Full only to "
                     "override it.");
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
            describe("Turn off to stop the scroll wheel changing knobs and sliders as the pointer passes over them, so "
                     "it only scrolls the page.");
            endCard();

            // Hosted in Soundshed Guitar, which can draw itself with either UI: the way back to the web
            // UI is the theme flyout's App UI choice (ThemeFlyout.cpp), as the web UI's way here is.
            if (mState.environment.nativeEditorOnly)
            {
                beginCard("Native UI");
                describe("This copy of Soundshed Guitar uses only the native UI: it was installed that way, or "
                         "the WebView2 runtime the standard UI needs is not installed.");
                endCard();
            }
        }
        else
        {
            {
                auto p = std::make_shared<Paragraph>(mTheme,
                                                     "Simplify the options available in the app depending on your "
                                                     "preferences for trying online or advanced features.",
                                                     textWidth + 32.0f, 11.5f);
                p->Colour(mTheme.TextSecondary());
                page.push_back(Row({el::share(el::hold_any(p)), Stretch()}));
                page.push_back(VGap(10));
            }

            // Core features are on unless switched off; power features off unless switched on.
            for (const bool core : {true, false})
            {
                beginCard(core ? "Core Features" : "Power Features");
                describe(core ? "These are the default features enabled in the app which you can optionally switch off."
                              : "These features are optional and may be under development or designed for advanced "
                                "users.");

                for (const auto feature : guitarfx::uiclient::AllFeatures())
                {
                    if (guitarfx::uiclient::FeatureDefault(feature) != core)
                    {
                        continue;
                    }

                    // The Resource Library row shows only while Blend Tools is on, as the web UI's.
                    if (feature == guitarfx::uiclient::Feature::ResourceLibrary &&
                        !guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::BlendTools))
                    {
                        continue;
                    }

                    const char* key = guitarfx::uiclient::FeatureSettingKey(feature);
                    toggle(
                        guitarfx::uiclient::FeatureLabel(feature),
                        [this, feature] { return guitarfx::uiclient::IsFeatureEnabled(mState, feature); },
                        setBool(key));
                    describe(guitarfx::uiclient::FeatureDescription(feature));
                }

                endCard();
            }

            if (guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::FactoryPresetArchives))
            {
                beginCard("Factory Preset Archives");
                toggle(
                    "Load Factory Preset Archives on Startup",
                    [boolOr] { return boolOr("factoryPresets.archiveLoadingEnabled", true); },
                    setBool("factoryPresets.archiveLoadingEnabled"));
                describe("Disable this while authoring factory preset archives in development so bundled preset packs "
                         "stay on disk without being imported at startup.");
                toggle(
                    "Include My Effect Presets in Preset Exports",
                    [boolOr] { return boolOr("factoryPresets.exportIncludesEffectPresets", false); },
                    setBool("factoryPresets.exportIncludesEffectPresets"));
                describe("Folder and all-preset exports then carry your saved effect presets, with the models, IRs and "
                         "blends they choose. Bundled as a factory archive, they become factory presets for those "
                         "effects.");
                endCard();
            }

            beginCard("");
            describe("Shared with Soundshed Guitar. Some features (the AI Tone Builder, the editors) are there only.");
            endCard();
        }

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
        topic("Jam", "Backing tracks search YouTube and play in a player docked beside the results, which stays open "
                     "across the rest of the app. Riff captures are written as measure-aligned DI WAV takes, with the "
                     "tempo, time signature, beat pattern and preset context stored alongside the audio.");
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
        line("Product", productName);
        line("Engine", mState.environment.version.empty() ? "Soundshed engine" : mState.environment.version);
        line("Interface", mState.environment.nativeEditor ? "Native editor" : "Native (no JUCE)");
        // As UTF-8, which the text expects: path::string() converts through the ANSI code page on
        // Windows, where a user name that page cannot spell throws, and opening Settings took the
        // app down with it.
        line("Profile", guitarfx::util::PathToUtf8(ProfileFolder()));
        card.push_back(VGap(6));
        card.push_back(FixedHeight(40, button("soundshed.com", [&commands] { commands.OpenUrl(kWebsite); })));

        if (!mState.environment.nativeEditor)
        {
            describe("Publishing tones and making packs are in Soundshed Guitar, which shares this app's presets, "
                     "library and settings.");
        }

        endCard();
        break;
    }
    }

    auto content = VScroll(mWide ? Pad(24, 20, 24, 12, Col(page))
                                 : Pad(metrics::kPagePad, metrics::kPagePadTop, 14, 12, Col(page)));
    return Dressed(mTheme, mTheme.Background(), &Backdrops::main, WithSectionNav(std::move(nav), content));
}
} // namespace nanoq::ui
