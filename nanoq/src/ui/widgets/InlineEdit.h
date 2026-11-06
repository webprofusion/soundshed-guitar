#pragma once

// A value typed in place: the field a knob or a number shows where its value text was, as the
// web UI's knob turns its value into a number field (.node-param-value-input), rather than a
// dialog over everything. The control that owns it hands it keys and characters while it is
// active and draws it over its value; Enter commits, Esc cancels, and losing focus commits.
//
// One line, a caret, and "all selected" as the field opens so the first key replaces the value.

#include "ui/widgets/TextDraw.h"
#include "ui/widgets/Theme.h"

#include <elements.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>

namespace nanoq::ui
{
class InlineEdit
{
  public:
    enum class Outcome
    {
        Unused, // not a key for the field
        Used,   // the text or caret moved
        Commit, // Enter
        Cancel, // Esc
    };

    [[nodiscard]] bool Active() const
    {
        return mActive;
    }

    /// Opens the field on `text`, all of it selected, or empty for a first character to come.
    void Begin(std::string text)
    {
        mActive = true;
        mText = std::move(text);
        mCaret = mText.size();
        mAllSelected = !mText.empty();
    }

    void End()
    {
        mActive = false;
        mText.clear();
        mCaret = 0;
        mAllSelected = false;
    }

    [[nodiscard]] const std::string& Value() const
    {
        return mText;
    }

    Outcome Key(const el::key_info& k)
    {
        using el::key_code;

        if (!mActive || (k.action != el::key_action::press && k.action != el::key_action::repeat))
        {
            return Outcome::Unused;
        }

        switch (k.key)
        {
        case key_code::enter:
        case key_code::kp_enter:
            return Outcome::Commit;
        case key_code::escape:
            return Outcome::Cancel;
        case key_code::backspace:

            if (mAllSelected)
            {
                Clear();
            }
            else if (mCaret > 0)
            {
                const auto start = PreviousChar(mCaret);
                mText.erase(start, mCaret - start);
                mCaret = start;
            }

            return Outcome::Used;
        case key_code::_delete:

            if (mAllSelected)
            {
                Clear();
            }
            else if (mCaret < mText.size())
            {
                mText.erase(mCaret, NextChar(mCaret) - mCaret);
            }

            return Outcome::Used;
        case key_code::left:
            mCaret = mAllSelected ? 0 : PreviousChar(mCaret);
            mAllSelected = false;
            return Outcome::Used;
        case key_code::right:
            mCaret = mAllSelected ? mText.size() : NextChar(mCaret);
            mAllSelected = false;
            return Outcome::Used;
        case key_code::home:
            mCaret = 0;
            mAllSelected = false;
            return Outcome::Used;
        case key_code::end:
            mCaret = mText.size();
            mAllSelected = false;
            return Outcome::Used;
        case key_code::a:

            if (k.modifiers & (el::mod_control | el::mod_action))
            {
                mAllSelected = !mText.empty();
                return Outcome::Used;
            }

            break;
        default:
            break;
        }

        // Characters arrive as text(); the field keeps their keys from the shortcuts.
        return (k.modifiers & (el::mod_control | el::mod_alt | el::mod_super)) == 0 ? Outcome::Used : Outcome::Unused;
    }

    /// A character typed. Returns whether it was taken.
    bool Char(const el::text_info& info)
    {
        if (!mActive || info.codepoint < 32 || info.codepoint == 127)
        {
            return false;
        }

        if (mAllSelected)
        {
            Clear();
        }

        const auto utf8 = Encode(info.codepoint);
        mText.insert(mCaret, utf8);
        mCaret += utf8.size();
        return true;
    }

    /// The field over `box`: a filled box with an accent border, the text centred in it (as the
    /// value text was), the selection or the caret.
    void Draw(cycfi::artist::canvas& cnv, el::rect box, const Theme& theme, const el::font_descr& font) const
    {
        cnv.fill_style(theme.Input());
        cnv.fill_round_rect(box, 4.0f);
        cnv.stroke_style(theme.Accent());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(box.inset(0.5f, 0.5f), 4.0f);

        const float textW = el::measure_text(cnv, mText, font).x;
        const float left = box.left + std::max(3.0f, (box.width() - textW) * 0.5f);
        const float midY = box.top + box.height() * 0.5f;
        const float half = std::min(box.height() * 0.5f - 2.0f, 7.0f);

        if (mAllSelected)
        {
            cnv.fill_style(theme.Accent().opacity(0.35f));
            cnv.fill_rect(el::rect{left - 1.0f, midY - half, left + textW + 1.0f, midY + half});
        }

        FillText(cnv, mText, font, theme.Text(), el::rect{left, box.top, box.right - 2.0f, box.bottom}, HAlign::Left);

        if (!mAllSelected)
        {
            const float x = left + el::measure_text(cnv, mText.substr(0, mCaret), font).x;
            cnv.fill_style(theme.Accent());
            cnv.fill_rect(el::rect{x, midY - half, x + 1.5f, midY + half});
        }
    }

    /// The keys of a control whose value is typed in place (a number field), with `edit` its
    /// field: while it is open, the field's keys, `commit` on Enter (and the field closed);
    /// otherwise Enter, F2 or a digit opens it on `initial()` (empty for a digit, which follows as
    /// text). Returns whether the key was used; `changed` says the control needs redrawing.
    static bool FieldKey(InlineEdit& edit, const el::key_info& k, const std::function<std::string()>& initial,
                         const std::function<void(const std::string&)>& commit, bool& changed)
    {
        using el::key_code;
        changed = false;

        if (edit.Active())
        {
            switch (edit.Key(k))
            {
            case Outcome::Commit: {
                const auto typed = edit.Value();
                edit.End();
                changed = true;

                if (commit)
                {
                    commit(typed);
                }

                return true;
            }
            case Outcome::Cancel:
                edit.End();
                changed = true;
                return true;
            case Outcome::Used:
                changed = true;
                return true;
            case Outcome::Unused:
                return false;
            }
        }

        if (!initial || !commit || k.action != el::key_action::press ||
            (k.modifiers & (el::mod_control | el::mod_alt | el::mod_super)) != 0)
        {
            return false;
        }

        if (k.key == key_code::enter || k.key == key_code::kp_enter || k.key == key_code::f2)
        {
            edit.Begin(initial());
            changed = true;
            return true;
        }

        const bool digit = (k.key >= key_code::_0 && k.key <= key_code::_9 && (k.modifiers & el::mod_shift) == 0) ||
                           (k.key >= key_code::kp_0 && k.key <= key_code::kp_9);

        if (digit || k.key == key_code::period || k.key == key_code::kp_decimal || k.key == key_code::minus ||
            k.key == key_code::kp_subtract)
        {
            edit.Begin({});
            changed = true;
            return true;
        }

        return false;
    }

    /// A field losing focus keeps what was typed: ends `edit`, repaints, and hands the text on.
    static void Leave(InlineEdit& edit, const std::function<void(const std::string&)>& onTyped,
                      const std::function<void()>& repaint)
    {
        if (!edit.Active())
        {
            return;
        }

        const auto typed = edit.Value();
        edit.End();
        repaint();

        if (onTyped)
        {
            const auto typedTo = onTyped; // it may rebuild the region the field sits in
            typedTo(typed);
        }
    }

  private:
    void Clear()
    {
        mText.clear();
        mCaret = 0;
        mAllSelected = false;
    }

    [[nodiscard]] std::size_t PreviousChar(std::size_t at) const
    {
        while (at > 0)
        {
            --at;

            if ((static_cast<unsigned char>(mText[at]) & 0xC0) != 0x80)
            {
                break;
            }
        }

        return at;
    }

    [[nodiscard]] std::size_t NextChar(std::size_t at) const
    {
        if (at >= mText.size())
        {
            return mText.size();
        }

        ++at;

        while (at < mText.size() && (static_cast<unsigned char>(mText[at]) & 0xC0) == 0x80)
        {
            ++at;
        }

        return at;
    }

    static std::string Encode(std::uint32_t cp)
    {
        std::string out;

        if (cp < 0x80)
        {
            out += static_cast<char>(cp);
        }
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }

        return out;
    }

    bool mActive = false;
    std::string mText;
    std::size_t mCaret = 0;
    bool mAllSelected = false;
};
} // namespace nanoq::ui
