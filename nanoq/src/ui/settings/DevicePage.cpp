// DevicePage.cpp - the standalone app's audio and MIDI devices, at the top of Settings: the web
// UI's settings/audioDevice.ts. The engine owns every device decision (StandaloneDevices over
// clap-wrapper's host) and answers each `audioDevice` request with the whole setup, which
// uiclient keeps in ClientState::device; this page keeps no device state of its own and is
// redrawn from that snapshot. Only the standalone app shows it (environment.audioDeviceSettings);
// in a DAW the host owns the devices.
//
// While the page is on screen the input meter runs on uiclient's level lease, which it renews
// itself; leaving the page lets it go.

#include "ui/shell/Shell.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <utility>

namespace nanoq::ui
{
using guitarfx::uiclient::Topic;

struct Shell::DevicePage
{
    SlotPtr slot; // the section in the settings page now built
    std::vector<guitarfx::uiclient::Subscription> subs;
    std::string signature; // what the section was last drawn from
    std::string error;     // the last request's failure, kept until the next request
    bool dirty = false;    // a new snapshot arrived
    bool watching = false; // the page is showing: the level lease is held
};

namespace
{
using Json = nlohmann::json;

/// One choice in a drop-down: what is sent, and what is shown.
using Option = std::pair<std::string, std::string>;

/// A select: the choice shown in a box with a caret, opening a drop-down list when tapped.
class SelectBox final : public Pressable
{
  public:
    SelectBox(const Theme& theme, Icons& icons, std::string text, float width)
        : mTheme(theme), mIcons(icons), mText(std::move(text)), mWidth(width)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::ComboBox;
        info.value = mText;
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{mWidth, 30.0f}, {mWidth, 30.0f}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(mHover && mEnabled ? mTheme.Elevated() : mTheme.Bar());
        cnv.fill_round_rect(b, 6.0f);
        cnv.stroke_style(mTheme.BorderStrong());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 6.0f);

        const auto ink = mEnabled ? mTheme.Text() : mTheme.TextMuted();
        const auto font = Font(Weight::Medium).size(metrics::kSmallText);
        const el::rect text{b.left + 10.0f, b.top, b.right - 26.0f, b.bottom};
        FillText(cnv, Ellipsize(cnv, mText, font, text.width()), font, ink, text, HAlign::Left);

        if (mEnabled)
        {
            const float cy = b.top + b.height() * 0.5f;
            mIcons.Draw(cnv, "ui-caret", el::rect{b.right - 20.0f, cy - 5.0f, b.right - 10.0f, cy + 5.0f},
                        ink.opacity(0.8f));
        }
    }

  private:
    const Theme& mTheme;
    Icons& mIcons;
    std::string mText;
    float mWidth;
};

std::vector<std::string> Strings(const Json& state, const char* key)
{
    std::vector<std::string> out;
    const auto it = state.find(key);

    if (it != state.end() && it->is_array())
    {
        for (const auto& v : *it)
        {
            if (v.is_string())
            {
                out.push_back(v.get<std::string>());
            }
        }
    }

    return out;
}

std::vector<double> Numbers(const Json& state, const char* key)
{
    std::vector<double> out;
    const auto it = state.find(key);

    if (it != state.end() && it->is_array())
    {
        for (const auto& v : *it)
        {
            if (v.is_number())
            {
                out.push_back(v.get<double>());
            }
        }
    }

    return out;
}

std::string Str(const Json& state, const char* key)
{
    const auto it = state.find(key);
    return it != state.end() && it->is_string() ? it->get<std::string>() : std::string();
}

double Num(const Json& state, const char* key)
{
    const auto it = state.find(key);
    return it != state.end() && it->is_number() ? it->get<double>() : 0.0;
}

bool Flag(const Json& state, const char* key, bool fallback = false)
{
    const auto it = state.find(key);
    return it != state.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

bool AnyActive(const Json& state, const char* key)
{
    const auto it = state.find(key);

    if (it == state.end() || !it->is_array())
    {
        return false;
    }

    return std::any_of(it->begin(), it->end(), [](const Json& g) { return g.is_object() && g.value("active", false); });
}

std::string Integer(double v)
{
    return std::to_string(static_cast<long long>(std::llround(v)));
}

std::string Ms(double samples, double rate)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f", samples * 1000.0 / rate);
    return text;
}

/// Devices, then "none". A chosen device that has gone stays listed, so the picker shows what broke.
std::vector<Option> DeviceOptions(const std::vector<std::string>& devices, const std::string& selected,
                                  const std::string& noneLabel)
{
    std::vector<Option> options;

    if (!selected.empty() && std::find(devices.begin(), devices.end(), selected) == devices.end())
    {
        options.emplace_back(selected, selected + " (disconnected)");
    }

    for (const auto& name : devices)
    {
        options.emplace_back(name, name);
    }

    options.emplace_back("", noneLabel);
    return options;
}

/// The device's values, with the one it runs at added if the list lacks it.
std::vector<double> WithCurrent(std::vector<double> values, double current)
{
    if (current > 0.0 && std::find(values.begin(), values.end(), current) == values.end())
    {
        values.push_back(current);
        std::sort(values.begin(), values.end());
    }

    return values;
}

struct Alert
{
    std::string text;
    std::string actionLabel, action; // a button that fixes it, if any
};

/// The one thing most worth saying about the setup, if anything (audioDeviceAlert).
std::optional<Alert> DeviceAlert(const Json& s, const std::string& error)
{
    if (Str(s, "inputPermission") == "denied")
    {
        return Alert{"Microphone access is off, so the amp can't hear your instrument.", "Allow Access",
                     "requestInputPermission"};
    }

    if (!error.empty())
    {
        return Alert{error, "Reset Device", "resetDevice"};
    }

    if (Str(s, "inputDevice").empty() && Str(s, "outputDevice").empty())
    {
        return Alert{"No audio device is selected.", {}, {}};
    }

    if (!Flag(s, "deviceOpen"))
    {
        return Alert{"The audio device couldn't be opened. Plug it back in and retry, or choose another device.",
                     "Retry", "resetDevice"};
    }

    if (Str(s, "outputDevice").empty() || !AnyActive(s, "outputChannels"))
    {
        return Alert{"No output is selected, so nothing will be heard.", {}, {}};
    }

    if (Str(s, "inputDevice").empty() || !AnyActive(s, "inputChannels"))
    {
        return Alert{"No input is selected, so the amp hears nothing.", {}, {}};
    }

    return std::nullopt;
}

std::string MuteHint(bool muted, bool risk)
{
    if (muted && risk)
    {
        return "Muted because the built-in microphone can hear the speakers. Plug in headphones or an interface, then turn this off.";
    }

    if (muted)
    {
        return "The amp hears nothing while the input is muted.";
    }

    if (risk)
    {
        return "The built-in microphone can hear the speakers, which can howl. Use headphones, or mute the input.";
    }

    return "Silences the input, to stop feedback when a microphone can hear the speakers.";
}
} // namespace

void Shell::DeviceRequest(const std::string& action, nlohmann::json args)
{
    // An error belongs to the request that caused it, and stays until the next one.
    if (mDevicePage)
    {
        mDevicePage->error.clear();
    }

    mSession.Commands().AudioDevice(action, std::move(args));
}

Ptr Shell::BuildDeviceSection()
{
    if (!mDevicePage)
    {
        mDevicePage = std::make_shared<DevicePage>();
    }

    auto& page = *mDevicePage;

    if (!mState.environment.audioDeviceSettings)
    {
        // A plugin: the host owns the devices. SyncDevicePage rebuilds the page if the engine
        // says otherwise once Settings is up.
        page.slot.reset();
        return VGap(0);
    }

    page.slot = MakeSlot();
    page.signature = mState.device.state.dump() + "\n" + page.error + (mState.device.known ? "1" : "0");
    Fill(*page.slot, BuildDeviceContent());
    return page.slot;
}

Ptr Shell::BuildDeviceContent()
{
    auto& page = *mDevicePage;
    const auto& device = mState.device;
    const auto& s = device.state;
    std::vector<Ptr> rows;

    constexpr float kLabelWidth = 190.0f;
    constexpr float kWide = 340.0f;
    constexpr float kNarrow = 220.0f;

    auto heading = [&](const char* text) {
        auto t = std::make_shared<Text>(mTheme, text, 11.0f, Weight::SemiBold);
        t->Colour(mTheme.TextMuted());
        rows.push_back(FixedHeight(32, PadXY(0, 6, t)));
    };
    auto label = [&](const std::string& text) {
        auto l = std::make_shared<Text>(mTheme, text, metrics::kTextSize);
        l->Colour(mTheme.TextSecondary());
        return FixedWidth(kLabelWidth, Middle(l));
    };
    // A muted line under a control, indented to the controls' column.
    auto hint = [&](const std::string& text, bool warning = false) {
        auto t = std::make_shared<Text>(mTheme, text, metrics::kSmallText);
        t->Colour(warning ? mTheme.Warning() : mTheme.TextMuted());
        rows.push_back(FixedHeight(22, Row({HGap(kLabelWidth), t})));
    };
    auto select = [&](const std::vector<Option>& options, const std::string& selected, float width, bool enabled,
                      std::function<void(const std::string&)> onPick) -> Ptr {
        std::string shown = selected;

        for (const auto& [value, text] : options)
        {
            if (value == selected)
            {
                shown = text;
            }
        }

        auto box = std::make_shared<SelectBox>(mTheme, mIcons, shown, width);
        box->Enabled(enabled);
        box->onClick = [this, b = box.get(), options, selected, width, onPick] {
            std::vector<MenuItem> items;

            for (const auto& [value, text] : options)
            {
                items.push_back({text, value == selected, false, [onPick, value] { onPick(value); }});
            }

            OpenDropdown(b->Bounds(), std::move(items), width);
        };
        return Middle(box);
    };
    auto row = [&](const std::string& title, std::vector<Ptr> controls) {
        std::vector<Ptr> parts{label(title)};

        for (auto& c : controls)
        {
            parts.push_back(std::move(c));
        }

        parts.push_back(Stretch());
        rows.push_back(FixedHeight(44, Row(parts)));
    };
    auto button = [&](const std::string& text, std::function<void()> onClick, bool enabled = true) {
        auto b = std::make_shared<Button>(mTheme, text, std::move(onClick), Button::Style::Plain);
        b->MinWidth(96).TextSize(metrics::kSmallText).Enabled(enabled);
        return Middle(b);
    };

    heading("AUDIO DEVICE");

    if (!device.known)
    {
        hint("Looking for audio devices...");
        return Col(rows);
    }

    // ── The alert: what is wrong, and the button that fixes it ──
    if (const auto alert = DeviceAlert(s, page.error))
    {
        auto text = std::make_shared<Text>(mTheme, alert->text, metrics::kSmallText, Weight::Medium);
        text->Colour(mTheme.Warning());
        // Text in align_middle, here and in every row: a row is no taller than its children allow,
        // and a line of text alone is one line high.
        std::vector<Ptr> parts{Middle(text), Stretch(nullptr, 0.01)};

        if (!alert->action.empty())
        {
            auto fix = std::make_shared<Button>(
                mTheme, alert->actionLabel, [this, action = alert->action] { DeviceRequest(action); },
                Button::Style::Plain);
            fix->MinWidth(96).TextSize(metrics::kSmallText);
            parts.push_back(HGap(8));
            parts.push_back(Middle(fix));
        }

        rows.push_back(
            Pad(0, 4, 0, 8, FixedHeight(40, Rounded(mTheme.Warning().opacity(0.12f), 6.0f, PadXY(12, 0, Row(parts))))));
    }

    // ── Driver and devices ──
    const auto types = Strings(s, "deviceTypes");

    if (types.size() > 1)
    {
        std::vector<Option> options;

        for (const auto& t : types)
        {
            options.emplace_back(t, t);
        }

        row("Driver", {select(options, Str(s, "deviceType"), kNarrow, true,
                              [this](const std::string& v) { DeviceRequest("setDeviceType", {{"deviceType", v}}); })});
    }

    const bool separate = Flag(s, "separateIO", true);

    if (separate)
    {
        const auto input = Str(s, "inputDevice");
        row("Input Device",
            {select(DeviceOptions(Strings(s, "inputDevices"), input, "No input"), input, kWide, true,
                    [this](const std::string& v) { DeviceRequest("setDevice", {{"kind", "input"}, {"name", v}}); })});
    }

    {
        const auto output = Str(s, "outputDevice");
        const bool open = Flag(s, "deviceOpen");
        auto test = button("Test", [this] { DeviceRequest("playTestTone"); }, open && !output.empty());
        row(separate ? "Output Device" : "Device",
            {select(DeviceOptions(Strings(s, "outputDevices"), output, separate ? "No output" : "No device"), output,
                    kWide, true,
                    [this, separate](const std::string& v) {
                        DeviceRequest("setDevice", {{"kind", separate ? "output" : "linked"}, {"name", v}});
                    }),
             HGap(8), test});
    }

    // ── Rate and buffer: what the device runs at, from its own lists ──
    if (Flag(s, "deviceOpen"))
    {
        const double rate = Num(s, "sampleRate");
        const auto rates = WithCurrent(Numbers(s, "sampleRates"), rate);
        std::vector<Option> rateOptions;

        for (const double r : rates)
        {
            rateOptions.emplace_back(Integer(r), Integer(r) + " Hz");
        }

        row("Sample Rate", {select(rateOptions, Integer(rate), kNarrow, rates.size() > 1, [this](const std::string& v) {
                DeviceRequest("setSampleRate", {{"sampleRate", std::stod(v)}});
            })});

        if (Numbers(s, "sampleRates").size() <= 1)
        {
            hint("The audio driver sets the sample rate.");
        }

        const double buffer = Num(s, "bufferSize");
        const auto sizes = WithCurrent(Numbers(s, "bufferSizes"), buffer);
        std::vector<Option> sizeOptions;

        for (const double b : sizes)
        {
            sizeOptions.emplace_back(Integer(b), Integer(b) + " samples" +
                                                     (rate > 0.0 ? " (" + Ms(b, rate) + " ms)" : std::string()));
        }

        row("Buffer Size",
            {select(sizeOptions, Integer(buffer), kNarrow, sizes.size() > 1,
                    [this](const std::string& v) { DeviceRequest("setBufferSize", {{"bufferSize", std::stoi(v)}}); })});

        if (Numbers(s, "bufferSizes").size() <= 1)
        {
            hint("The audio driver sets the buffer size.");
        }

        const double latency = Num(s, "inputLatency") + Num(s, "outputLatency");

        if (latency > 0.0 && rate > 0.0)
        {
            hint("Latency reported by the driver: " + Ms(latency, rate) + " ms.");
        }
    }

    // ── The input: its level while this page shows, and the mute ──
    if (!Str(s, "inputDevice").empty())
    {
        auto meter = std::make_shared<MeterBar>(mTheme);
        mLive.Feed(meter, [this](MeterBar& m) {
            return m.Set(mState.device.inputLevelDb, mState.device.inputLevelDb > -0.5);
        });
        auto level = std::make_shared<Text>(mTheme, "", metrics::kSmallText);
        level->Colour(mTheme.TextMuted());
        // In whole dB; below -99 as -inf.
        mLive.Bind(
            level,
            [this] { return mState.device.inputLevelDb <= -99.0 ? -100L : std::lround(mState.device.inputLevelDb); },
            [](Text& t, long db) { t.Set(db <= -100L ? std::string("-inf dB") : std::to_string(db) + " dB"); });
        row("Input Level", {FixedWidth(kWide, Middle(meter)), HGap(10), FixedWidth(70, Middle(level))});
    }

    if (Flag(s, "canMuteInput"))
    {
        const bool muted = Flag(s, "inputMuted");
        const bool risk = Flag(s, "feedbackRisk");
        auto toggle = std::make_shared<PowerSwitch>(mTheme, muted);
        toggle->AccessName("Mute Input");
        toggle->onClick = [this, muted] { DeviceRequest("setInputMuted", {{"muted", !muted}}); };
        row("Mute Input", {Middle(toggle)});
        hint(MuteHint(muted, risk), muted || risk);
    }

    row("", {button("Reset Device", [this] { DeviceRequest("resetDevice"); }), HGap(8),
             button("Rescan", [this] { DeviceRequest("getState", {{"rescan", true}}); })});

    // ── MIDI ──
    heading("MIDI DEVICES");
    {
        auto title = std::make_shared<Text>(mTheme, "MIDI Inputs", metrics::kTextSize);
        title->Colour(mTheme.TextSecondary());
        rows.push_back(FixedHeight(30, title));
    }
    const auto inputs = s.find("midiInputs");
    bool anyPort = false;

    if (inputs != s.end() && inputs->is_array())
    {
        // One full-width row per port: their names are long.
        for (const auto& port : *inputs)
        {
            if (!port.is_object() || !port.contains("identifier"))
            {
                continue;
            }

            anyPort = true;
            const auto identifier = port.value("identifier", std::string());
            const bool enabled = port.value("enabled", false);
            auto name = std::make_shared<Text>(mTheme, port.value("name", identifier), metrics::kTextSize);
            auto toggle = std::make_shared<PowerSwitch>(mTheme, enabled);
            toggle->AccessName(port.value("name", identifier));
            toggle->onClick = [this, identifier, enabled] {
                DeviceRequest("setMidiInputEnabled", {{"identifier", identifier}, {"enabled", !enabled}});
            };
            rows.push_back(FixedHeight(40, Row({HGap(16), Middle(name), HGap(12), Middle(toggle), HGap(4)})));
        }
    }

    if (!anyPort)
    {
        hint("None");
    }

    if (Flag(s, "showMidiOutput"))
    {
        std::vector<Option> outputs;
        const auto chosen = Str(s, "midiOutput");
        const auto ports = s.find("midiOutputs");

        if (ports != s.end() && ports->is_array())
        {
            for (const auto& port : *ports)
            {
                if (port.is_object() && port.contains("identifier"))
                {
                    outputs.emplace_back(port.value("identifier", std::string()), port.value("name", std::string()));
                }
            }
        }

        outputs.emplace_back("", "None");
        row("MIDI Output", {select(outputs, chosen, kWide, true, [this](const std::string& v) {
                DeviceRequest("setMidiOutput", {{"identifier", v}});
            })});
    }

    return Col(rows);
}

void Shell::SyncDevicePage()
{
    if (!mDevicePage)
    {
        mDevicePage = std::make_shared<DevicePage>();
    }

    auto& page = *mDevicePage;

    if (page.subs.empty())
    {
        page.subs.push_back(mSession.Client().Subscribe(Topic::Device, [this] { mDevicePage->dirty = true; }));
    }

    // The level lease is held while the devices show (Settings > Audio & MIDI); uiclient renews it.
    const bool shown =
        mPage == Page::Settings && mSettingsSection == SettingsSection::Audio && mState.environment.audioDeviceSettings;

    if (shown != page.watching)
    {
        page.watching = shown;
        mSession.Client().SetDeviceLevelWatch(shown);

        if (shown)
        {
            mSession.Commands().AudioDevice("getState", {{"rescan", true}});
        }
    }

    if (!shown)
    {
        return;
    }

    if (!page.slot)
    {
        // Settings was built before the engine said this app has device controls.
        Mark(kPage);
        return;
    }

    if (page.dirty)
    {
        page.dirty = false;

        if (!mState.device.error.empty())
        {
            page.error = mState.device.error;
        }

        const auto signature = mState.device.state.dump() + "\n" + page.error + (mState.device.known ? "1" : "0");

        // The whole page is rebuilt: a section refilled in place inside the page's scroller kept
        // the height it had been laid out with, and its rows were squeezed into that.
        if (signature != page.signature)
        {
            Mark(kPage);
        }
    }
}
} // namespace nanoq::ui
