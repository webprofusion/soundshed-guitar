#pragma once

// What the on-screen keyboard is, apart from how it is drawn (OnScreenKeyboard.h): its keys, where
// they go in a panel, what Shift does, and how far a field has to move to stay above it. Nothing
// here needs Elements, so the tests can reach it.

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace nanoq::ui::keyboard
{
enum class KeyKind
{
    Character,
    Space,
    Shift,
    Mode, // letters <-> symbols
    Backspace,
    CaretLeft,
    CaretRight,
    Hide,
    Enter,
    Gap, // nothing there: the half key that staggers a row
};

struct KeyDef
{
    KeyKind kind = KeyKind::Gap;
    char32_t lower = 0;
    char32_t upper = 0;
    float width = 1.0f; // of the ten a row holds
};

enum class Mode
{
    Letters,
    Symbols,
};

/// One tap is the next capital, a second keeps capitals on, a third turns them off.
enum class Shift
{
    Off,
    Once,
    Locked,
};

[[nodiscard]] inline Shift NextShift(Shift shift)
{
    switch (shift)
    {
    case Shift::Off:
        return Shift::Once;
    case Shift::Once:
        return Shift::Locked;
    case Shift::Locked:
        return Shift::Off;
    }

    return Shift::Off;
}

[[nodiscard]] inline Shift ShiftAfterCharacter(Shift shift)
{
    return shift == Shift::Once ? Shift::Off : shift;
}

inline constexpr float kRowUnits = 10.0f;
inline constexpr float kPanelPad = 6.0f;
inline constexpr float kKeyGap = 4.0f;
inline constexpr float kMinPanelHeight = 190.0f;
inline constexpr float kMaxPanelHeight = 320.0f;
/// Keys past this are too far apart for two thumbs; a wide screen centres them.
inline constexpr float kMaxKeysWidth = 1120.0f;
/// Clear space kept between a field and the panel.
inline constexpr float kFieldGap = 8.0f;
/// Under half the view at 800x480, leaving room for a field and what is around it.
inline constexpr float kPanelShare = 0.47f;

[[nodiscard]] inline float PanelHeight(float viewHeight)
{
    return std::clamp(viewHeight * kPanelShare, kMinPanelHeight, kMaxPanelHeight);
}

/// How far the layer goes up so the field clears a panel starting at `panelTop`. `fieldTop` and
/// `fieldBottom` are where the field is with the layer at rest. The field's top stays on screen
/// (it matters more than its bottom), and the layer never rises by more than `panelHeight`, so
/// the panel always covers what it leaves bare.
[[nodiscard]] inline float FieldPan(float fieldTop, float fieldBottom, float panelTop, float panelHeight)
{
    const float wanted = fieldBottom + kFieldGap - panelTop;
    return std::max(0.0f, std::min({wanted, fieldTop - kFieldGap, panelHeight}));
}

namespace detail
{
inline void AddCharacters(std::vector<KeyDef>& row, const std::u32string& keys)
{
    for (const char32_t c : keys)
    {
        const char32_t upper = c >= U'a' && c <= U'z' ? c - U'a' + U'A' : c;
        row.push_back({KeyKind::Character, c, upper, 1.0f});
    }
}

inline std::vector<KeyDef> BottomRow()
{
    return {
        {KeyKind::Mode, 0, 0, 1.25f},
        {KeyKind::CaretLeft, 0, 0, 0.85f},
        {KeyKind::CaretRight, 0, 0, 0.85f},
        {KeyKind::Space, U' ', U' ', 3.0f},
        {KeyKind::Character, U'.', U'.', 0.85f},
        {KeyKind::Character, U'@', U'@', 0.85f},
        {KeyKind::Hide, 0, 0, 1.0f},
        {KeyKind::Enter, 0, 0, 1.35f},
    };
}

inline std::vector<std::vector<KeyDef>> BuildRows(Mode mode)
{
    std::vector<std::vector<KeyDef>> rows(5);

    if (mode == Mode::Letters)
    {
        AddCharacters(rows[0], U"1234567890");
        AddCharacters(rows[1], U"qwertyuiop");
        rows[2].push_back({KeyKind::Gap, 0, 0, 0.5f});
        AddCharacters(rows[2], U"asdfghjkl");
        rows[2].push_back({KeyKind::Gap, 0, 0, 0.5f});
        rows[3].push_back({KeyKind::Shift, 0, 0, 1.5f});
        AddCharacters(rows[3], U"zxcvbnm");
    }
    else
    {
        AddCharacters(rows[0], U"!@#$%^&*()");
        AddCharacters(rows[1], U"-_=+/\\|:;\"");
        rows[2].push_back({KeyKind::Gap, 0, 0, 0.5f});
        AddCharacters(rows[2], U"'`~<>[]{}");
        rows[2].push_back({KeyKind::Gap, 0, 0, 0.5f});
        rows[3].push_back({KeyKind::Gap, 0, 0, 1.5f});
        AddCharacters(rows[3], U",?€£°§…");
    }

    rows[3].push_back({KeyKind::Backspace, 0, 0, 1.5f});
    rows[4] = BottomRow();
    return rows;
}
} // namespace detail

[[nodiscard]] inline const std::vector<std::vector<KeyDef>>& Rows(Mode mode)
{
    static const auto letters = detail::BuildRows(Mode::Letters);
    static const auto symbols = detail::BuildRows(Mode::Symbols);
    return mode == Mode::Letters ? letters : symbols;
}

struct Box
{
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    [[nodiscard]] bool Holds(float x, float y) const
    {
        return x >= left && x < right && y >= top && y < bottom;
    }
};

struct PlacedKey
{
    KeyDef def;
    Box box;
};

/// The keys in a panel `width` by `height`, its top left at `top`. A gap in the rows is left out.
[[nodiscard]] inline std::vector<PlacedKey> PlaceKeys(Mode mode, float width, float top, float height)
{
    const float keysWidth = std::min(width - 2.0f * kPanelPad, kMaxKeysWidth);
    const float left = (width - keysWidth) * 0.5f;
    const auto& rows = Rows(mode);
    const float rowHeight = (height - 2.0f * kPanelPad) / static_cast<float>(rows.size());
    const float unit = keysWidth / kRowUnits;
    std::vector<PlacedKey> keys;

    for (std::size_t r = 0; r < rows.size(); ++r)
    {
        const float rowTop = top + kPanelPad + rowHeight * static_cast<float>(r);
        float x = left;

        for (const auto& def : rows[r])
        {
            const float next = x + def.width * unit;

            if (def.kind != KeyKind::Gap)
            {
                keys.push_back({def, Box{x + kKeyGap * 0.5f, rowTop + kKeyGap * 0.5f, next - kKeyGap * 0.5f,
                                         rowTop + rowHeight - kKeyGap * 0.5f}});
            }

            x = next;
        }
    }

    return keys;
}

/// The key under a finger, or -1. The gaps between keys are shared out between the keys beside
/// them, so a tap between two lands on one: on a touch screen a miss is worse than a near miss.
[[nodiscard]] inline int KeyAt(const std::vector<PlacedKey>& keys, float x, float y)
{
    const float reach = kKeyGap * 0.5f;

    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        const auto& b = keys[i].box;

        if (Box{b.left - reach, b.top - reach, b.right + reach, b.bottom + reach}.Holds(x, y))
        {
            return static_cast<int>(i);
        }
    }

    return -1;
}

/// A code point as UTF-8, for a key's label.
[[nodiscard]] inline std::string Utf8(char32_t c)
{
    std::string out;

    if (c < 0x80)
    {
        out += static_cast<char>(c);
    }
    else if (c < 0x800)
    {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    else if (c < 0x10000)
    {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }

    return out;
}
} // namespace nanoq::ui::keyboard
