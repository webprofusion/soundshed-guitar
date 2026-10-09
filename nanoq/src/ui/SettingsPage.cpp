// The settings page: the theme, the output, the amp model (NAM) quality settings, and what this
// build is. The controls patch themselves from the engine's settings in place (SyncSettings), so
// dragging a slider is not interrupted by the setting it just changed coming back.

#include "ui/Shell.h"

#include "Paths.h"
#include "uiclient/ParamFormat.h"

#include <algorithm>
#include <cmath>

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

double NumberSetting(const nlohmann::json& settings, const char* key, double fallback)
{
    const auto it = settings.find(key);
    return it != settings.end() && it->is_number() ? it->get<double>() : fallback;
}

bool BoolSetting(const nlohmann::json& settings, const char* key, bool fallback)
{
    const auto it = settings.find(key);
    return it != settings.end() && it->is_boolean() ? it->get<bool>() : fallback;
}
} // namespace

Ptr Shell::BuildSettingsPage()
{
    auto& commands = mSession.Commands();
    mSettingsSyncs.clear();

    std::vector<Ptr> page;

    auto heading = [this, &page](const char* text) {
        auto t = std::make_shared<Text>(mTheme, text, 11.0f, Weight::SemiBold);
        t->Colour(mTheme.TextMuted());
        page.push_back(FixedHeight(32, PadXY(0, 6, t)));
    };

    auto note = [this, &page](const char* text) {
        auto t = std::make_shared<Text>(mTheme, text, metrics::kSmallText);
        t->Colour(mTheme.TextMuted());
        page.push_back(FixedHeight(24, t));
    };

    auto label = [this](const char* text) {
        auto l = std::make_shared<Text>(mTheme, text, metrics::kTextSize);
        l->Colour(mTheme.TextSecondary());
        return FixedWidth(190, l);
    };

    // A row of options, one lit: `get` says which, `set` is told which was tapped.
    auto choice = [&](const char* title, const std::vector<std::string>& options, std::function<int()> get, std::function<void(int)> set) {
        auto buttons = std::make_shared<std::vector<std::shared_ptr<Button>>>();
        std::vector<Ptr> row{label(title)};
        for (std::size_t i = 0; i < options.size(); ++i)
        {
            auto b = std::make_shared<Button>(mTheme, options[i], [set, i] { set(static_cast<int>(i)); }, Button::Style::Plain);
            b->MinWidth(76).TextSize(metrics::kSmallText);
            buttons->push_back(b);
            row.push_back(b);
            row.push_back(HGap(6));
        }
        row.push_back(Stretch());
        page.push_back(FixedHeight(44, Row(row)));
        mSettingsSyncs.push_back([this, buttons, get] {
            const int selected = get();
            for (std::size_t i = 0; i < buttons->size(); ++i)
            {
                (*buttons)[i]->Selected(static_cast<int>(i) == selected);
                mSession.View().refresh(*(*buttons)[i]);
            }
        });
    };

    auto toggle = [&](const char* title, const char* text, std::function<bool()> get, std::function<void(bool)> set) {
        auto b = std::make_shared<Button>(mTheme, text, [get, set] { set(!get()); }, Button::Style::Plain);
        b->Dot(true).MinWidth(220);
        page.push_back(FixedHeight(44, Row({label(title), b, Stretch()})));
        mSettingsSyncs.push_back([this, b, get] {
            b->On(get());
            mSession.View().refresh(*b);
        });
    };

    // ── Appearance ──
    heading("APPEARANCE");
    choice("Theme", {"Dark", "Light", "Classic"},
           [this] { return mState.theme == "light" ? 1 : (mState.theme == "classic" ? 2 : 0); },
           [&commands](int i) { commands.SetTheme(i == 1 ? "light" : (i == 2 ? "classic" : "dark")); });

    // ── Output ──
    heading("OUTPUT");
    toggle("Limiter", "Soft limit the output", [this] { return BoolSetting(mState.appSettings, kOutputLimiterSetting, false); },
           [&commands](bool on) { commands.SetSetting(kOutputLimiterSetting, on); });
    choice("Preset switch tails", {"Off", "1 bar", "2 bars", "3 bars", "4 bars"},
           [this] { return std::clamp(static_cast<int>(NumberSetting(mState.appSettings, kPresetSwitchTailSetting, kPresetSwitchTailDefault)), 0, 4); },
           [&commands](int i) { commands.SetSetting(kPresetSwitchTailSetting, i); });
    note("How many bars of delay and reverb tail carry over when you change preset.");

    // ── Amp models ──
    heading("AMP MODELS (NAM)");
    {
        auto quality = std::make_shared<ParamSlider>(mTheme, "Quality");
        quality->onChange = [&commands](double v) { commands.SetSetting(kNamSlimmableSizeSetting, std::round(v * 100.0) / 100.0); };
        quality->onReset = [&commands] { commands.SetSetting(kNamSlimmableSizeSetting, 1.0); };
        page.push_back(FixedHeight(60, Row({FixedWidth(420, quality), Stretch()})));
        mSettingsSyncs.push_back([this, quality] {
            const double v = std::clamp(NumberSetting(mState.appSettings, kNamSlimmableSizeSetting, 1.0), 0.0, 1.0);
            quality->Update(v, std::to_string(static_cast<int>(std::lround(v * 100.0))) + "%");
            mSession.View().refresh(*quality);
        });
    }
    note("Lower quality reduces CPU use on models that support it. Watch the DSP load while you adjust it.");
    choice("Oversampling", {"Off", "2x", "4x", "8x", "16x", "32x"},
           [this] { return std::clamp(static_cast<int>(NumberSetting(mState.appSettings, kNamOversamplingSetting, 0)), 0, 5); },
           [&commands](int i) { commands.SetSetting(kNamOversamplingSetting, i); });
    choice("Anti-alias filter", {"Minimum phase", "Linear short", "Linear long"},
           [this] { return std::clamp(static_cast<int>(NumberSetting(mState.appSettings, kNamAntiAliasPhaseSetting, 0)), 0, 2); },
           [&commands](int i) { commands.SetSetting(kNamAntiAliasPhaseSetting, i); });

    // ── About ──
    heading("ABOUT");
    auto line = [this, &page](const std::string& labelText, const std::string& value) {
        auto l = std::make_shared<Text>(mTheme, labelText, metrics::kTextSize);
        l->Colour(mTheme.TextSecondary());
        auto v = std::make_shared<Text>(mTheme, value, metrics::kTextSize, Weight::Medium);
        page.push_back(FixedHeight(26, Row({FixedWidth(190, l), v})));
    };
    line("Product", "Soundshed Nano Q");
    line("Engine", mState.environment.version.empty() ? "Soundshed engine" : mState.environment.version);
    line("Interface", "QPlug and Elements (no JUCE)");
    line("Profile", ProfileFolder().string());
    {
        auto site = std::make_shared<Button>(mTheme, "soundshed.com", [&commands] { commands.OpenUrl(kWebsite); }, Button::Style::Flat);
        site->MinWidth(150);
        page.push_back(FixedHeight(44, Row({FixedWidth(190, std::make_shared<el::element>()), site, Stretch()})));
    }
    note("Tone sharing, riff capture, the JAM tools, plugin hosting and preset blending are in Soundshed Guitar,");
    note("which shares this app's presets, library and settings.");
    if (!mState.environment.audioDeviceSettings)
        note("The audio device is set by the host, or in the window menu (Alt+Space) of the standalone app.");

    SyncSettings();
    return Fill(mTheme.Background(), Pad(24, 8, 24, 12, VScroll(Col(page))));
}

void Shell::SyncSettings()
{
    for (auto& sync : mSettingsSyncs)
        sync();
}
} // namespace nanoq::ui
