/**
 * AutomationKeyboard.cpp - Keyboard mappings: which slot a key press fires, and what a key
 * learn stores (see AutomationSlotTable.h). Until 2026-10-27 the web UI ran these itself
 * (automationPanel.ts); the engine runs them now, so a native UI maps keys the same way.
 */

#include "automation/AutomationSlotTable.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace guitarfx
{
namespace
{
/// The length in bytes of the UTF-8 sequence `lead` starts, or 0 for a byte that cannot start one.
std::size_t Utf8SequenceLength(unsigned char lead)
{
    if (lead < 0x80)
    {
        return 1;
    }

    if ((lead & 0xE0) == 0xC0)
    {
        return 2;
    }

    if ((lead & 0xF0) == 0xE0)
    {
        return 3;
    }

    if ((lead & 0xF8) == 0xF0)
    {
        return 4;
    }

    return 0;
}
} // namespace

std::string AutomationSlotTable::NormalizeMappedKey(const std::string& key)
{
    // One character ("A", "1", "é") rather than a named key: compared without regard to case,
    // as the web UI compared event.key.
    const bool oneCharacter = !key.empty() && Utf8SequenceLength(static_cast<unsigned char>(key[0])) == key.size();

    if (!oneCharacter || key.size() != 1)
    {
        return key;
    }

    return std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(key[0]))));
}

bool AutomationSlotTable::IsUnmappableKey(const std::string& key)
{
    static const std::array<const char*, 7> kModifiers = {" ",    "Shift",    "Control", "Alt",
                                                          "Meta", "AltGraph", "CapsLock"};
    return std::any_of(kModifiers.begin(), kModifiers.end(), [&key](const char* name) { return key == name; });
}

std::optional<AutomationSlotTable::KeyMatch> AutomationSlotTable::MatchKey(const std::string& key, bool repeat) const
{
    const std::string pressed = NormalizeMappedKey(key);

    if (pressed.empty() || IsUnmappableKey(pressed))
    {
        return std::nullopt;
    }

    for (const auto& slot : mSlots)
    {
        for (const auto& map : slot.keyMaps)
        {
            if (NormalizeMappedKey(map.key) != pressed || (map.mode == KeyboardMap::Mode::Trigger && repeat))
            {
                continue;
            }

            const bool trigger = map.mode == KeyboardMap::Mode::Trigger;
            return KeyMatch{slot.slotId, trigger ? 1.0f : map.value, trigger};
        }
    }

    return std::nullopt;
}

std::vector<KeyboardMap> AutomationSlotTable::LearnedKeyMaps(const AutomationSlot& slot, const std::string& key) const
{
    const std::string learned = NormalizeMappedKey(key);
    std::vector<KeyboardMap> maps;

    for (const auto& map : slot.keyMaps)
    {
        if (NormalizeMappedKey(map.key) != learned)
        {
            maps.push_back(map);
        }
    }

    const bool trigger = IsTriggerSlot(slot) || IsBypassSlot(slot);
    KeyboardMap map;
    map.key = learned;
    map.mode = trigger ? KeyboardMap::Mode::Trigger : KeyboardMap::Mode::SetValue;
    map.value = trigger ? 1.0f : 0.5f;
    maps.push_back(std::move(map));
    return maps;
}
} // namespace guitarfx
