// Automation.cpp - MIDI and automation in Nano Q, as the web UI has them (midiLearnMenu.ts,
// automationPanel.ts, midiSetlistOut.ts): a control's right-click MIDI Learn menu, the MIDI &
// Automation dialog (Mappings, Keyboard, MIDI Log, and the options for the MIDI device), and the
// keyboard mappings' way in.
//
// The slots, the learns and the keyboard mappings are the engine's; which address a control
// drives, the menu's items and how a mapping reads come from uiclient's Automation.h, which the
// web UI's midiMapping.ts is the twin of. Keys are run by the engine (automationKey): this file
// only passes on the ones a slot maps, or every one while a key learn waits.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "uiclient/Automation.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

namespace
{
/// Sent as SysEx whenever the preset changes; absent means on (ControllerDisplayFeed).
constexpr const char* kControllerDisplaySetting = "midi.controllerDisplay.presetName";
/// The setlist's program change out (SetlistMidiOut): on/off (absent off), channel 1-16, and how
/// the bank is sent.
constexpr const char* kSetlistOutEnabledSetting = "midi.setlistOut.enabled";
constexpr const char* kSetlistOutChannelSetting = "midi.setlistOut.channel";
constexpr const char* kSetlistOutBankSelectSetting = "midi.setlistOut.bankSelect";

const std::pair<const char*, const char*> kBankSelectModes[] = {
    {"msbLsb", "CC0 + CC32"}, {"msb", "CC0 only"}, {"lsb", "CC32 only"}, {"none", "Don't send"}};

/// How the DOM's KeyboardEvent.key names an Elements key: the engine's keyboard mappings, and the
/// web UI's, are written in those names. A printable key is its unshifted character.
std::string DomKeyName(el::key_code key)
{
    using el::key_code;
    const int code = static_cast<int>(key);

    if (code >= static_cast<int>(key_code::space) && code <= static_cast<int>(key_code::grave_accent))
    {
        const char c = static_cast<char>(code);
        return std::string(1, c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }

    if (code >= static_cast<int>(key_code::f1) && code <= static_cast<int>(key_code::f25))
    {
        return "F" + std::to_string(code - static_cast<int>(key_code::f1) + 1);
    }

    if (code >= static_cast<int>(key_code::kp_0) && code <= static_cast<int>(key_code::kp_9))
    {
        return std::string(1, static_cast<char>('0' + code - static_cast<int>(key_code::kp_0)));
    }

    switch (key)
    {
    case key_code::escape:
        return "Escape";
    case key_code::enter:
    case key_code::kp_enter:
        return "Enter";
    case key_code::tab:
        return "Tab";
    case key_code::backspace:
        return "Backspace";
    case key_code::insert:
        return "Insert";
    case key_code::_delete:
        return "Delete";
    case key_code::right:
        return "ArrowRight";
    case key_code::left:
        return "ArrowLeft";
    case key_code::down:
        return "ArrowDown";
    case key_code::up:
        return "ArrowUp";
    case key_code::page_up:
        return "PageUp";
    case key_code::page_down:
        return "PageDown";
    case key_code::home:
        return "Home";
    case key_code::end:
        return "End";
    case key_code::caps_lock:
        return "CapsLock";
    case key_code::pause:
        return "Pause";
    case key_code::kp_decimal:
        return ".";
    case key_code::kp_divide:
        return "/";
    case key_code::kp_multiply:
        return "*";
    case key_code::kp_subtract:
        return "-";
    case key_code::kp_add:
        return "+";
    case key_code::kp_equal:
        return "=";
    case key_code::left_shift:
    case key_code::right_shift:
        return "Shift";
    case key_code::left_control:
    case key_code::right_control:
        return "Control";
    case key_code::left_alt:
    case key_code::right_alt:
        return "Alt";
    case key_code::left_super:
    case key_code::right_super:
        return "Meta";
    default:
        return {};
    }
}

float DialogWidthFor(const el::extent& view, float wanted)
{
    return std::min(wanted, std::max(240.0f, view.x - 48.0f));
}

/// Where the target picker hangs from: the Target button last pressed.
el::rect gTargetAnchor;

/// The shells with a redraw of the dialog already posted: a burst of MIDI Log entries redraws it once.
std::set<const void*> gRebuildPosted;

bool BoolSettingOr(const nlohmann::json& settings, const char* key, bool fallback)
{
    const auto it = settings.find(key);
    return it != settings.end() && it->is_boolean() ? it->get<bool>() : fallback;
}
} // namespace

// ── The right-click menu ─────────────────────────────────────────────────────────

void Shell::OpenNodeParamMidiMenu(const std::string& nodeType, const std::string& paramKey, el::point at)
{
    if (const auto target = NodeParamLearnTarget(mState.FindEffectType(nodeType), paramKey))
    {
        OpenMidiLearnMenu(target->address, target->label, at);
    }
}

void Shell::OpenGlobalKnobMidiMenu(const std::string& knob, el::point at)
{
    if (const auto target = GlobalKnobLearnTarget(mState.automation, knob))
    {
        OpenMidiLearnMenu(target->address, target->label, at);
    }
}

void Shell::OpenMidiLearnMenu(const std::string& address, const std::string& label, el::point at)
{
    // Until the engine has sent its slots there is nothing to arm, and no telling which slot
    // already drives the control.
    if (!mState.automation.loaded || mState.automation.registry.empty())
    {
        mSession.Commands().GetAutomation();
        return;
    }

    const MidiLearnTarget target{address, label};
    const auto& automation = mState.automation;
    std::vector<MenuItem> items;
    items.push_back({MidiLearnMenuTitle(automation, target, mState.activePresetId), false, true, nullptr});

    for (const auto& item : BuildMidiLearnMenuItems(automation, target, mState.activePresetId))
    {
        MenuItem row;
        row.label = item.label;
        row.detail = item.hint;

        if (!item.disabled)
        {
            row.onChoose = [this, target, action = item.action] {
                const bool learning =
                    action == MidiLearnMenuAction::Learn || action == MidiLearnMenuAction::LearnPreset;
                const auto message = RunMidiLearnMenuAction(mSession.Commands(), mState, action, target);

                if (!message.empty())
                {
                    const bool refused = message.rfind("MIDI Learn unavailable", 0) == 0;
                    ShowToast(refused ? "MIDI Learn unavailable" : "MIDI mapping cleared",
                              refused ? message.substr(std::string("MIDI Learn unavailable: ").size()) : message,
                              refused);
                    return;
                }

                if (learning)
                {
                    // Announced when the engine captures it (SyncAutomation).
                    mMenuLearnSlotId = mState.automation.midiLearnSlotId;
                    const auto* slot = mState.automation.FindSlot(mMenuLearnSlotId);
                    mMenuLearnLabel = slot != nullptr && !slot->label.empty() ? slot->label : target.label;
                    const bool forPreset = action == MidiLearnMenuAction::LearnPreset;
                    const auto* preset = forPreset ? mState.FindPresetSummary(mState.activePresetId) : nullptr;
                    ShowToast("MIDI Learn",
                              "Move a MIDI control to map " + mMenuLearnLabel +
                                  (forPreset ? " for " + (preset != nullptr ? preset->name : mState.activePresetId)
                                             : std::string()) +
                                  " (Esc cancels)",
                              false);
                }
            };
        }

        items.push_back(std::move(row));
    }

    OpenDropdown(el::rect{at.x, at.y, at.x + 1.0f, at.y + 1.0f}, std::move(items), 280.0f);
}

void Shell::SyncAutomation()
{
    auto& automation = mSession.Client().MutableState().automation;

    // A learn finished: say what it mapped, once.
    if (automation.capture)
    {
        const auto capture = *automation.capture;
        automation.capture.reset();
        const auto* slot = automation.FindSlot(capture.slotId);
        const std::string label = capture.slotId == mMenuLearnSlotId ? mMenuLearnLabel
                                  : slot != nullptr                  ? slot->label
                                                                     : capture.slotId;
        ShowToast("MIDI mapped",
                  DescribeMidiMap(capture.eventType, capture.channel, capture.controller, std::nullopt) + " to " +
                      label,
                  false);
        mMenuLearnSlotId.clear();
    }

    // Redrawn once the event in hand is done: a change made by one of the dialog's own buttons
    // arrives here while that button is still handling its click, and must not be pulled out
    // from under it.
    if (mMidiDialogOpen && mMidiDialogBody && gRebuildPosted.insert(this).second)
    {
        mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive)] {
            if (!alive.lock())
            {
                return;
            }

            gRebuildPosted.erase(this);

            if (!mMidiDialogOpen || !mMidiDialogBody)
            {
                return;
            }

            Fill(*mMidiDialogBody, BuildMidiAutomationBody());
            mSession.View().layout();
            mSession.View().refresh();
        });
    }
}

// ── Keys ─────────────────────────────────────────────────────────────────────────

bool Shell::RouteAutomationKey(const el::key_info& k)
{
    if (k.action != el::key_action::press && k.action != el::key_action::repeat)
    {
        return false;
    }

    const std::string key = DomKeyName(k.key);

    if (key.empty())
    {
        return false;
    }

    const bool repeat = k.action == el::key_action::repeat;
    auto& commands = mSession.Commands();
    const auto& automation = mState.automation;

    // A key learn takes every key, while the dialog that started it shows it; the engine maps
    // it, backs out on Escape, and waits through Space and a bare modifier.
    if (mMidiDialogOpen && !automation.keyLearnSlotId.empty())
    {
        commands.AutomationKey(key, repeat);
        return true;
    }

    // Escape cancels a MIDI learn the menu started, once nothing is on top to close.
    if (key == "Escape" && !automation.midiLearnSlotId.empty() && !mOverlay && !mDropdown)
    {
        commands.CancelMidiLearn();
        mMenuLearnSlotId.clear();
        ShowToast("MIDI Learn cancelled", "", false);
        return true;
    }

    // Space is the transport's and a chord is a shortcut (Ctrl+Z is undo, not "z").
    if (key == " " ||
        (k.modifiers & (el::mod_control | el::mod_alt | el::mod_super | el::mod_command | el::mod_action)) != 0)
    {
        return false;
    }

    if (!KeyIsMapped(automation, key))
    {
        return false;
    }

    commands.AutomationKey(key, repeat);
    return true;
}

// ── The dialog ───────────────────────────────────────────────────────────────────

void Shell::OpenMidiAutomation()
{
    auto& commands = mSession.Commands();
    mMidiDialogOpen = true;
    mMidiDialogBody = MakeSlot();
    // The engine forwards MIDI to the log only while it is on screen and enabled.
    commands.SetMidiLogEnabled(mState.automation.midiLogEnabled);
    commands.GetAutomation();

    Fill(*mMidiDialogBody, BuildMidiAutomationBody());

    const auto view = ViewSize();
    ShowDialog({.title = "MIDI & Automation",
                .body = mMidiDialogBody,
                .width = DialogWidthFor(view, 860.0f),
                .height = std::max(320.0f, view.y - 60.0f),
                .padY = 12.0f});

    mOnOverlayClosed = [this] {
        // Nothing listens or logs once the dialog has gone.
        mMidiDialogOpen = false;
        mMidiDialogBody.reset();
        auto& c = mSession.Commands();
        c.SetMidiLogEnabled(false);
        mSession.Client().MutableState().automation.midiLogEnabled = false;
        c.CancelMidiLearn();

        if (!mState.automation.keyLearnSlotId.empty())
        {
            c.ArmKeyLearn("");
        }
    };
}

Ptr Shell::BuildMidiAutomationBody()
{
    auto& commands = mSession.Commands();
    const auto& automation = mState.automation;
    const auto text = [this](const std::string& value, float size = 11.0f, el::color colour = el::color()) {
        auto t = std::make_shared<Text>(mTheme, value, size);
        t->Colour(colour.alpha > 0.0f ? colour : mTheme.Text());
        return t;
    };
    const auto button = [this](const std::string& label, std::function<void()> onClick, float width = 0.0f) {
        auto b = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        b->Size(width > 0.0f ? width : 40.0f, 24).Label(label, 10.5f);
        b->onClick = std::move(onClick);
        return b;
    };
    const auto rebuild = [this] { SyncAutomation(); };

    // ── Tabs ──
    std::vector<Ptr> tabs;

    for (int i = 0; i < 3; ++i)
    {
        const char* labels[] = {"Mappings", "Keyboard", "MIDI Log"};
        auto tab = std::make_shared<UnderlineTab>(mTheme, labels[i], mMidiDialogTab == i);
        tab->onClick = [this, i, rebuild] {
            mMidiDialogTab = i;
            rebuild();
        };
        tabs.push_back(tab);
        tabs.push_back(HGap(4));
    }

    tabs.push_back(Stretch());

    std::vector<Ptr> rows;
    const auto help = [&](const std::string& line) {
        rows.push_back(FixedHeight(18, text(line, 10.5f, mTheme.TextMuted())));
    };

    if (!automation.loaded)
    {
        rows.push_back(FixedHeight(24, text("Loading automation\xE2\x80\xA6", 11.0f, mTheme.TextMuted())));
    }
    else if (mMidiDialogTab == 0)
    {
        // ── The MIDI device options ──
        const auto& settings = mState.appSettings;
        const bool showName = BoolSettingOr(settings, kControllerDisplaySetting, true);
        auto nameToggle = std::make_shared<CheckBox>(mTheme, "Show the preset name on the MIDI controller's display");
        nameToggle->Set(showName);
        nameToggle->onClick = [&commands, showName] { commands.SetSetting(kControllerDisplaySetting, !showName); };
        rows.push_back(FixedHeight(28, Row({nameToggle, Stretch()})));

        const bool outOn = BoolSettingOr(settings, kSetlistOutEnabledSetting, false);
        auto outToggle = std::make_shared<CheckBox>(
            mTheme, "Send a program change to the MIDI device when a bank or setlist slot is picked here");
        outToggle->Set(outOn);
        outToggle->onClick = [&commands, outOn] { commands.SetSetting(kSetlistOutEnabledSetting, !outOn); };
        rows.push_back(VGap(4));
        rows.push_back(FixedHeight(28, Row({outToggle, Stretch()})));

        int channel = 1;

        if (const auto it = settings.find(kSetlistOutChannelSetting); it != settings.end() && it->is_number_integer())
        {
            channel = std::clamp(it->get<int>(), 1, 16);
        }

        std::string bank = "msbLsb";

        if (const auto it = settings.find(kSetlistOutBankSelectSetting); it != settings.end() && it->is_string())
        {
            bank = it->get<std::string>();
        }

        std::string bankLabel = kBankSelectModes[0].second;

        for (const auto& [id, label] : kBankSelectModes)
        {
            if (bank == id)
            {
                bankLabel = label;
            }
        }

        auto channelButton = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        channelButton->Size(70, 26).Label("Ch " + std::to_string(channel), 11.0f).Caret(true).Disabled(!outOn);
        channelButton->onClick = [this, &commands, channel, b = channelButton.get()] {
            std::vector<MenuItem> items;

            for (int c = 1; c <= 16; ++c)
            {
                items.push_back({std::to_string(c), c == channel, false,
                                 [&commands, c] { commands.SetSetting(kSetlistOutChannelSetting, c); }});
            }

            OpenDropdown(b->Bounds(), std::move(items), 90.0f);
        };
        auto bankButton = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        bankButton->Size(120, 26).Label(bankLabel, 11.0f).Caret(true).Disabled(!outOn);
        bankButton->onClick = [this, &commands, bank, b = bankButton.get()] {
            std::vector<MenuItem> items;

            for (const auto& [id, label] : kBankSelectModes)
            {
                items.push_back({label, bank == id, false, [&commands, id = std::string(id)] {
                                     commands.SetSetting(kSetlistOutBankSelectSetting, id);
                                 }});
            }

            OpenDropdown(b->Bounds(), std::move(items), 150.0f);
        };
        rows.push_back(VGap(4));
        rows.push_back(FixedHeight(
            28, Row({HGap(20), FixedWidth(56, text("Channel", 11.0f, mTheme.TextSecondary())), channelButton, HGap(16),
                     FixedWidth(76, text("Bank select", 11.0f, mTheme.TextSecondary())), bankButton, Stretch()})));
        rows.push_back(VGap(8));
        help(
            "Map automation slots to MIDI control messages, keyboard keys, or DAW parameters. Default slots are always available;");
        help("custom slots can target any effect parameter. Right-click a knob for MIDI Learn.");

        // ── The slots ──
        const auto slotRow = [&](const AutomationSlotInfo& slot) {
            const auto* entry = automation.FindRegistryEntry(slot.address);
            std::string address = slot.address;

            if (!slot.presetId.empty())
            {
                const auto* preset = mState.FindPresetSummary(slot.presetId);
                address = (preset != nullptr ? preset->name : slot.presetId) + " \xC2\xB7 " + slot.address;
            }

            const std::string range = DescribeRange(entry);
            const std::string slotId = slot.slotId;
            const bool listening = automation.midiLearnSlotId == slotId;

            std::vector<Ptr> cells{
                FixedWidth(150, text(slot.label, 11.0f)), HGap(6),
                FixedWidth(210, text(address + (range.empty() ? "" : " " + range), 10.5f, mTheme.TextMuted())),
                HGap(6)};
            cells.push_back(
                FixedWidth(104, text(slot.midiMap ? DescribeMidiMap(*slot.midiMap) : "\xE2\x80\x94", 10.5f)));

            if (slot.midiMap)
            {
                cells.push_back(
                    button("\xC3\x97", [&commands, slotId] { commands.ClearAutomationMidiMap(slotId); }, 24.0f));
            }
            else
            {
                cells.push_back(HGap(24));
            }

            cells.push_back(HGap(6));
            cells.push_back(FixedWidth(90, text(DescribeKeyMaps(slot), 10.5f)));
            cells.push_back(Stretch());

            auto learn = button(
                listening ? "Listening\xE2\x80\xA6" : "Learn",
                [&commands, slotId, listening] {
                    if (listening)
                    {
                        commands.CancelMidiLearn();
                    }
                    else
                    {
                        commands.ArmMidiLearn(slotId);
                    }
                },
                74.0f);
            learn->Lit(listening);
            cells.push_back(learn);

            if (ShowsTestButton(automation, slot))
            {
                cells.push_back(HGap(4));
                cells.push_back(button(
                    "Test",
                    [this, &commands, slotId] {
                        if (const auto* s = mState.automation.FindSlot(slotId))
                        {
                            commands.SetAutomationValue(slotId, TestValue(mState.automation, *s));
                        }
                    },
                    44.0f));
            }

            if (!slot.isDefault && slot.presetId.empty())
            {
                cells.push_back(HGap(4));
                auto target = button("Target", nullptr, 56.0f);
                target->onClick = [this, slotId, t = target.get()] {
                    gTargetAnchor = t->Bounds();
                    OpenAutomationTargetPicker(slotId);
                };
                cells.push_back(target);
            }

            if (!slot.isDefault)
            {
                cells.push_back(HGap(4));
                cells.push_back(
                    button("\xC3\x97", [&commands, slotId] { commands.RemoveAutomationSlot(slotId); }, 24.0f));
            }

            cells.push_back(HGap(12)); // clear of the scroll bar
            rows.push_back(FixedHeight(28, Row(cells)));
        };
        const auto section = [&](const std::string& title) {
            rows.push_back(VGap(8));
            auto t = std::make_shared<Text>(mTheme, title, 11.5f, Weight::SemiBold);
            t->Colour(mTheme.TextSecondary());
            rows.push_back(FixedHeight(20, t));
        };

        section("DEFAULT SLOTS");

        for (const auto& slot : automation.slots)
        {
            if (slot.isDefault)
            {
                slotRow(slot);
            }
        }

        const int customCount = CountCustomSlots(automation.slots);
        section("CUSTOM SLOTS (" + std::to_string(customCount) + "/" + std::to_string(automation.maxCustomSlots) + ")");

        for (const auto& slot : automation.slots)
        {
            if (!slot.isDefault && slot.presetId.empty())
            {
                slotRow(slot);
            }
        }

        if (customCount < automation.maxCustomSlots)
        {
            rows.push_back(VGap(4));
            rows.push_back(FixedHeight(26, Row({button(
                                                    "+ Add Custom Slot",
                                                    [this, &commands] {
                                                        commands.SetAutomationSlot(
                                                            NextCustomSlotId(mState.automation.slots),
                                                            std::string("New Slot"), std::string());
                                                    },
                                                    130.0f),
                                                Stretch()})));
        }

        if (std::any_of(automation.slots.begin(), automation.slots.end(),
                        [](const AutomationSlotInfo& s) { return !s.presetId.empty(); }))
        {
            section("Preset Mappings");

            for (const auto& slot : automation.slots)
            {
                if (!slot.presetId.empty())
                {
                    slotRow(slot);
                }
            }
        }
    }
    else if (mMidiDialogTab == 1)
    {
        help(
            "Map computer keyboard keys to automation slots. Click Learn, then press a key. Trigger mode fires on key press;");
        help(
            "Set Value sets a specific level. Mapped keys work whether or not this dialog is open. Space is left for the transport.");
        const bool staleSpace =
            std::any_of(automation.slots.begin(), automation.slots.end(), [](const AutomationSlotInfo& s) {
                return std::any_of(s.keyMap.begin(), s.keyMap.end(),
                                   [](const AutomationKeyMap& k) { return k.key == " "; });
            });

        if (staleSpace)
        {
            rows.push_back(FixedHeight(18, text("A slot is mapped to Space, which never fires: use Clear to remove it.",
                                                10.5f, mTheme.Warning())));
        }

        rows.push_back(VGap(6));

        // Keys are global, so per-preset mappings take none.
        for (const auto& slot : automation.slots)
        {
            if (!slot.presetId.empty())
            {
                continue;
            }

            const std::string slotId = slot.slotId;
            const bool learning = automation.keyLearnSlotId == slotId;
            auto learn = button(
                learning ? "Press a key\xE2\x80\xA6" : "Learn",
                [&commands, slotId, learning] { commands.ArmKeyLearn(learning ? std::string() : slotId); }, 96.0f);
            learn->Lit(learning);
            std::vector<Ptr> cells{FixedWidth(220, text(slot.label, 11.0f)), HGap(8),
                                   FixedWidth(220, text(DescribeKeyMaps(slot), 10.5f)), Stretch(), learn};

            if (!slot.keyMap.empty())
            {
                cells.push_back(HGap(4));
                cells.push_back(
                    button("Clear", [&commands, slotId] { commands.ClearAutomationKeyMap(slotId); }, 52.0f));
            }

            cells.push_back(HGap(12)); // clear of the scroll bar
            rows.push_back(FixedHeight(28, Row(cells)));
        }
    }
    else
    {
        const bool logging = automation.midiLogEnabled;
        auto enable = std::make_shared<CheckBox>(mTheme, "Enable MIDI logging");
        enable->Set(logging);
        enable->onClick = [&commands, logging] { commands.SetMidiLogEnabled(!logging); };
        auto clear = button(
            "Clear",
            [this, rebuild] {
                mSession.Client().MutableState().automation.midiLog.clear();
                rebuild();
            },
            60.0f);
        rows.push_back(FixedHeight(28, Row({enable, Stretch(), clear})));
        rows.push_back(VGap(6));

        if (!logging)
        {
            rows.push_back(FixedHeight(20, text("MIDI logging is disabled. Enable it to see incoming MIDI events.",
                                                11.0f, mTheme.TextMuted())));
        }
        else if (automation.midiLog.empty())
        {
            rows.push_back(FixedHeight(20, text("No MIDI events received yet.", 11.0f, mTheme.TextMuted())));
        }

        // The newest first, the last 200.
        const auto& log = automation.midiLog;
        const std::size_t shown = std::min<std::size_t>(log.size(), 200);

        for (std::size_t i = 0; i < shown; ++i)
        {
            const auto& entry = log[log.size() - 1 - i];
            const std::string data = "ch=" + FormatMidiChannel(entry.channel) + " d1=" + std::to_string(entry.data1) +
                                     " d2=" + std::to_string(entry.data2);
            rows.push_back(
                FixedHeight(20, Row({FixedWidth(100, text(entry.clock, 10.5f, mTheme.TextMuted())),
                                     FixedWidth(80, text(entry.type, 10.5f, mTheme.Accent())), text(data, 10.5f)})));
        }
    }

    return Col({FixedHeight(28, Row(tabs)), VGap(8), VScroll(Col(rows))});
}

void Shell::OpenAutomationTargetPicker(const std::string& slotId)
{
    const auto* slot = mState.automation.FindSlot(slotId);

    if (slot == nullptr)
    {
        return;
    }

    const auto options =
        WithCurrentTargetOption(BuildTargetOptions(mState), slot->address, DeriveSlotLabel(mState, slot->address));

    // Two steps, as the web UI's optgroups: the group, then the target in it. Lists over the
    // dialog, which stays open.
    std::vector<std::string> groups;

    for (const auto& option : options)
    {
        if (std::find(groups.begin(), groups.end(), option.group) == groups.end())
        {
            groups.push_back(option.group);
        }
    }

    std::vector<MenuItem> items;

    for (const auto& group : groups)
    {
        const bool holdsCurrent = std::any_of(options.begin(), options.end(), [&](const AutomationTargetOption& o) {
            return o.group == group && o.address == slot->address;
        });
        items.push_back({group, holdsCurrent, false, [this, slotId, group, options] {
                             std::vector<MenuItem> targets;
                             const auto* current = mState.automation.FindSlot(slotId);

                             for (const auto& option : options)
                             {
                                 if (option.group != group)
                                 {
                                     continue;
                                 }

                                 targets.push_back(
                                     {option.label, current != nullptr && current->address == option.address, false,
                                      [this, slotId, option] {
                                          // A slot keeps a label the user gave it; a new one takes the target's.
                                          const auto* s = mState.automation.FindSlot(slotId);
                                          const bool named = s != nullptr && !s->label.empty() &&
                                                             s->label != "New Slot" &&
                                                             s->label != DeriveSlotLabel(mState, s->address);
                                          mSession.Commands().SetAutomationSlot(slotId, named ? s->label : option.label,
                                                                                option.address);
                                      }});
                             }

                             mSession.View().post(
                                 [this, alive = std::weak_ptr<bool>(mAlive), targets = std::move(targets)]() mutable {
                                     if (alive.lock())
                                     {
                                         OpenDropdown(gTargetAnchor, std::move(targets), 300.0f);
                                     }
                                 });
                         }});
    }

    OpenDropdown(gTargetAnchor, std::move(items), 220.0f);
}
} // namespace nanoq::ui
