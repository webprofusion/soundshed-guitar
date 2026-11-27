// DevicePage.cpp - the standalone app's audio and MIDI devices, at the top of Settings: the web
// UI's settings/audioDevice.ts. The engine owns every device decision (StandaloneDevices over
// clap-wrapper's host) and answers each `audioDevice` request with the whole setup, which
// uiclient keeps in ClientState::device; this page keeps no device state of its own and is
// redrawn from that snapshot. Only the standalone app shows it (environment.audioDeviceSettings);
// in a DAW the host owns the devices.
//
// While the page is on screen the input meter runs on uiclient's level lease, which it renews
// itself; leaving the page lets it go.

#include "ui/dialogs/DialogChrome.h"
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

Ptr Shell::BuildDeviceSection(bool midi)
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

    if (midi)
    {
        return BuildDeviceContent(true);
    }

    page.slot = MakeSlot();
    page.signature = mState.device.state.dump() + "\n" + page.error + (mState.device.known ? "1" : "0");
    Fill(*page.slot, BuildDeviceContent(false));
    return page.slot;
}

Ptr Shell::BuildDeviceContent(bool midi)
{
    auto& page = *mDevicePage;
    const auto& device = mState.device;
    const auto& s = device.state;
    std::vector<Ptr> rows;

    // The card's inner width, as SettingsPage.cpp works it out. The labels are Settings' 230 px
    // capitals while the device lists (at least 200) and Test fit beside them; narrower, they
    // close to 120 to leave the lists room.
    const float room = StageWidth() - (mWide ? 245.0f : 0.0f) - (mWide ? 48.0f : metrics::kPagePad + 14.0f) - 46.0f;
    const float labelWidth = room >= 242.0f + 200.0f + 104.0f ? 242.0f : 132.0f;
    const float wideWidth = std::clamp(room - labelWidth - 104.0f, 200.0f, 340.0f);
    const float narrowWidth = std::min(220.0f, wideWidth);

    // The label in capitals at the left, as every Settings row has it.
    auto label = [&](const std::string& text) {
        auto l = std::make_shared<Text>(mTheme, Upper(text), 11.0f, Weight::SemiBold);
        l->Colour(mTheme.TextSecondary());
        return FixedWidth(labelWidth, Middle(l));
    };
    // A muted line under a control, indented to the controls' column; it wraps.
    auto hint = [&](const std::string& text, bool warning = false) {
        auto p = std::make_shared<Paragraph>(mTheme, text, std::max(160.0f, room - labelWidth), 11.5f);
        p->Colour(warning ? mTheme.Warning() : mTheme.TextSecondary());
        rows.push_back(Row({HGap(labelWidth), el::share(el::hold_any(p)), Stretch()}));
        rows.push_back(VGap(8));
    };
    auto select = [&](const std::vector<Option>& options, const std::string& selected, float width, bool enabled,
                      std::function<void(const std::string&)> onPick, const std::string& name) -> Ptr {
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
        box->AccessName(name);
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
    auto toggle = [&](const std::string& name, bool on, std::function<void()> onClick) {
        auto sw = std::make_shared<ToggleSwitch>(mTheme, on);
        sw->AccessName(name);
        sw->onClick = std::move(onClick);
        return Middle(sw);
    };

    if (midi)
    {
        // ── MIDI Devices: one full-width row per input (their names are long), then the output ──
        {
            auto title = std::make_shared<Text>(mTheme, "MIDI INPUTS", 11.0f, Weight::SemiBold);
            title->Colour(mTheme.TextSecondary());
            rows.push_back(FixedHeight(28, Middle(title)));
        }
        const auto inputs = s.find("midiInputs");
        bool anyPort = false;

        if (inputs != s.end() && inputs->is_array())
        {
            for (const auto& port : *inputs)
            {
                if (!port.is_object() || !port.contains("identifier"))
                {
                    continue;
                }

                anyPort = true;
                const auto identifier = port.value("identifier", std::string());
                const bool enabled = port.value("enabled", false);
                const auto name = port.value("name", identifier);
                auto text = std::make_shared<Text>(mTheme, name, metrics::kTextSize);
                rows.push_back(FixedHeight(40, Row({HGap(12), Middle(text), Stretch(),
                                                    toggle(name, enabled,
                                                           [this, identifier, enabled] {
                                                               DeviceRequest(
                                                                   "setMidiInputEnabled",
                                                                   {{"identifier", identifier}, {"enabled", !enabled}});
                                                           }),
                                                    HGap(4)})));
            }
        }

        if (!anyPort)
        {
            auto none = std::make_shared<Text>(mTheme, "None", metrics::kTextSize);
            none->Colour(mTheme.TextMuted());
            rows.push_back(FixedHeight(32, Row({HGap(12), Middle(none)})));
        }

        {
            auto p = std::make_shared<Paragraph>(mTheme,
                                                 "Enabled inputs drive MIDI learn, automation and setlist switching.",
                                                 std::max(160.0f, room), 11.5f);
            p->Colour(mTheme.TextSecondary());
            rows.push_back(VGap(4));
            rows.push_back(Row({el::share(el::hold_any(p)), Stretch()}));
            rows.push_back(VGap(8));
        }

        if (Flag(s, "showMidiOutput"))
        {
            std::vector<Option> outputs;
            const auto chosen = Str(s, "midiOutput");
            const auto ports = s.find("midiOutputs");
            bool listed = false;

            if (ports != s.end() && ports->is_array())
            {
                for (const auto& port : *ports)
                {
                    if (port.is_object() && port.contains("identifier"))
                    {
                        const auto identifier = port.value("identifier", std::string());
                        listed = listed || identifier == chosen;
                        outputs.emplace_back(identifier, port.value("name", std::string()));
                    }
                }
            }

            // A chosen output that has gone stays listed, so the picker shows what broke.
            if (!chosen.empty() && !listed)
            {
                outputs.insert(outputs.begin(), Option{chosen, "(disconnected)"});
            }

            outputs.emplace_back("", "None");
            row("MIDI Output",
                {select(
                    outputs, chosen, wideWidth, true,
                    [this](const std::string& v) { DeviceRequest("setMidiOutput", {{"identifier", v}}); },
                    "MIDI Output")});
            hint("Where the preset name goes for a controller's display.");
        }

        return Col(rows);
    }

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

        row("Audio Driver", {select(
                                options, Str(s, "deviceType"), narrowWidth, true,
                                [this](const std::string& v) { DeviceRequest("setDeviceType", {{"deviceType", v}}); },
                                "Audio Driver")});
    }

    const bool separate = Flag(s, "separateIO", true);

    if (separate)
    {
        const auto input = Str(s, "inputDevice");
        row("Input Device",
            {select(
                DeviceOptions(Strings(s, "inputDevices"), input, "No input"), input, wideWidth, true,
                [this](const std::string& v) { DeviceRequest("setDevice", {{"kind", "input"}, {"name", v}}); },
                "Input Device")});
    }

    {
        const auto output = Str(s, "outputDevice");
        const bool open = Flag(s, "deviceOpen");
        auto test = button("Test", [this] { DeviceRequest("playTestTone"); }, open && !output.empty());
        const char* title = separate ? "Output Device" : "Device";
        row(title, {select(
                        DeviceOptions(Strings(s, "outputDevices"), output, separate ? "No output" : "No device"),
                        output, wideWidth, true,
                        [this, separate](const std::string& v) {
                            DeviceRequest("setDevice", {{"kind", separate ? "output" : "linked"}, {"name", v}});
                        },
                        title),
                    HGap(8), test});
    }

    // ── Channels: the input's with its meter beside them (while this page shows), then the output's ──
    const auto groups = [&](const char* key) {
        std::vector<Option> options{{"-1", "None"}};
        int active = -1;
        const auto it = s.find(key);

        if (it != s.end() && it->is_array())
        {
            for (const auto& g : *it)
            {
                if (!g.is_object())
                {
                    continue;
                }

                if (g.value("active", false) && active < 0)
                {
                    active = static_cast<int>(options.size()) - 1;
                }

                options.emplace_back(std::to_string(options.size() - 1), g.value("name", std::string()));
            }
        }

        return std::pair{options, active};
    };
    auto meter = [&]() -> Ptr {
        auto bar = std::make_shared<MeterBar>(mTheme);
        mLive.Feed(
            bar, [this](MeterBar& m) { return m.Set(mState.device.inputLevelDb, mState.device.inputLevelDb > -0.5); });
        auto level = std::make_shared<Text>(mTheme, "", metrics::kSmallText);
        level->Colour(mTheme.TextMuted());
        // In whole dB; below -99 as -inf.
        mLive.Bind(
            level,
            [this] { return mState.device.inputLevelDb <= -99.0 ? -100L : std::lround(mState.device.inputLevelDb); },
            [](Text& t, long db) { t.Set(db <= -100L ? std::string("-inf dB") : std::to_string(db) + " dB"); });
        return Row({FixedWidth(std::max(80.0f, wideWidth - narrowWidth - 8.0f), Middle(bar)), HGap(10),
                    FixedWidth(56, Middle(level))});
    };

    {
        const auto [options, active] = groups("inputChannels");

        if (options.size() > 1)
        {
            row("Input Channels",
                {select(
                     options, std::to_string(active), narrowWidth, true,
                     [this](const std::string& v) { DeviceRequest("setInputChannels", {{"group", std::stoi(v)}}); },
                     "Input Channels"),
                 HGap(8), meter()});
        }
        else if (!Str(s, "inputDevice").empty())
        {
            row("Input Level", {meter()});
        }
    }

    {
        // A two-channel output has nothing to choose, unless it has somehow been switched off.
        const auto [options, active] = groups("outputChannels");

        if (options.size() > 2 || (options.size() > 1 && active < 0))
        {
            row("Output Channels",
                {select(
                    options, std::to_string(active), narrowWidth, true,
                    [this](const std::string& v) { DeviceRequest("setOutputChannels", {{"group", std::stoi(v)}}); },
                    "Output Channels")});
        }
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

        row("Sample Rate",
            {select(
                rateOptions, Integer(rate), narrowWidth, rates.size() > 1,
                [this](const std::string& v) { DeviceRequest("setSampleRate", {{"sampleRate", std::stod(v)}}); },
                "Sample Rate")});

        if (Numbers(s, "sampleRates").size() <= 1)
        {
            hint(Str(s, "deviceType") == "Windows Audio"
                     ? "Windows fixes the rate in shared mode. Choose Windows Audio (Exclusive Mode) as the driver "
                       "for other rates."
                     : "The audio driver sets the sample rate.");
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
            {select(
                sizeOptions, Integer(buffer), narrowWidth, sizes.size() > 1,
                [this](const std::string& v) { DeviceRequest("setBufferSize", {{"bufferSize", std::stoi(v)}}); },
                "Buffer Size")});

        if (Numbers(s, "bufferSizes").size() <= 1)
        {
            hint(Flag(s, "hasControlPanel")
                     ? "This driver sets its own buffer size. Change it in the driver's Control Panel below."
                     : "The audio driver sets the buffer size.");
        }

        const double in = Num(s, "inputLatency");
        const double out = Num(s, "outputLatency");

        if ((in > 0.0 || out > 0.0) && rate > 0.0)
        {
            hint("Latency reported by the driver: " + Ms(in + out, rate) + " ms (" + Ms(in, rate) + " ms in, " +
                 Ms(out, rate) + " ms out).");
        }
    }

    // ── The mute ──
    if (Flag(s, "canMuteInput"))
    {
        const bool muted = Flag(s, "inputMuted");
        const bool risk = Flag(s, "feedbackRisk");
        row("Mute Input",
            {toggle("Mute Input", muted, [this, muted] { DeviceRequest("setInputMuted", {{"muted", !muted}}); })});
        hint(MuteHint(muted, risk), muted || risk);
    }

    // ── The driver's own: its control panel, and a reopen ──
    {
        std::vector<Ptr> buttons;

        if (Flag(s, "hasControlPanel"))
        {
            buttons.push_back(button("Control Panel", [this] { DeviceRequest("showControlPanel"); }));
            buttons.push_back(HGap(8));
        }

        buttons.push_back(button("Reset Device", [this] { DeviceRequest("resetDevice"); }));
        buttons.push_back(HGap(8));
        buttons.push_back(button("Rescan", [this] { DeviceRequest("getState", {{"rescan", true}}); }));
        row("Driver", std::move(buttons));
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
