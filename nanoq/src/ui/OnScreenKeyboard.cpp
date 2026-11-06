#include "ui/OnScreenKeyboard.h"

#include "ui/TextDraw.h"
#include "ui/UiSession.h"

#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace nanoq::ui
{
namespace
{
using keyboard::KeyDef;
using keyboard::KeyKind;
using keyboard::PlacedKey;

constexpr double kRepeatDelay = 0.45;
constexpr double kRepeatInterval = 0.06;
constexpr float kKeyRadius = 6.0f;

el::rect ToRect(const keyboard::Box& b)
{
    return {b.left, b.top, b.right, b.bottom};
}

/// A held Backspace or arrow goes on working; a held letter does not, or a slow finger would type
/// it twice.
bool Repeats(KeyKind kind)
{
    return kind == KeyKind::Backspace || kind == KeyKind::CaretLeft || kind == KeyKind::CaretRight;
}

bool IsChrome(KeyKind kind)
{
    return kind != KeyKind::Character && kind != KeyKind::Space;
}

/// A small left- or right-pointing triangle.
void DrawArrow(el::canvas& cnv, el::point c, float direction, el::color colour)
{
    constexpr float kHalf = 7.0f;
    cnv.fill_style(colour);
    cnv.begin_path();
    cnv.move_to(c.x + direction * kHalf, c.y);
    cnv.line_to(c.x - direction * kHalf * 0.7f, c.y - kHalf);
    cnv.line_to(c.x - direction * kHalf * 0.7f, c.y + kHalf);
    cnv.close_path();
    cnv.fill();
}

/// The outline of an up arrow, filled while capitals are locked.
void DrawShift(el::canvas& cnv, el::point c, bool on, bool locked, el::color colour)
{
    constexpr float kHalf = 8.0f;
    constexpr float kStem = 3.5f;
    cnv.begin_path();
    cnv.move_to(c.x, c.y - kHalf);
    cnv.line_to(c.x + kHalf, c.y);
    cnv.line_to(c.x + kStem, c.y);
    cnv.line_to(c.x + kStem, c.y + kHalf);
    cnv.line_to(c.x - kStem, c.y + kHalf);
    cnv.line_to(c.x - kStem, c.y);
    cnv.line_to(c.x - kHalf, c.y);
    cnv.close_path();

    if (locked)
    {
        cnv.fill_style(colour);
        cnv.fill();
        return;
    }

    cnv.line_width(on ? 2.5f : 1.5f);
    cnv.stroke_style(colour);
    cnv.stroke();
}

/// A key with a cross in it, the shape every keyboard gives Backspace.
void DrawBackspace(el::canvas& cnv, el::point c, el::color colour)
{
    constexpr float kHalfWidth = 13.0f;
    constexpr float kHalfHeight = 9.0f;
    constexpr float kNose = 7.0f;
    constexpr float kCross = 3.5f;
    const float x = c.x + kNose * 0.5f;
    cnv.stroke_style(colour);
    cnv.line_width(1.5f);
    cnv.begin_path();
    cnv.move_to(c.x - kHalfWidth, c.y);
    cnv.line_to(c.x - kHalfWidth + kNose, c.y - kHalfHeight);
    cnv.line_to(c.x + kHalfWidth, c.y - kHalfHeight);
    cnv.line_to(c.x + kHalfWidth, c.y + kHalfHeight);
    cnv.line_to(c.x - kHalfWidth + kNose, c.y + kHalfHeight);
    cnv.close_path();
    cnv.move_to(x - kCross, c.y - kCross);
    cnv.line_to(x + kCross, c.y + kCross);
    cnv.move_to(x + kCross, c.y - kCross);
    cnv.line_to(x - kCross, c.y + kCross);
    cnv.stroke();
}
} // namespace

bool OnScreenKeyboard::Wanted()
{
    if (const char* setting = std::getenv("SOUNDSHED_NANOQ_KEYBOARD"))
    {
        return std::string_view(setting) != "0";
    }

    return std::getenv("SOUNDSHED_KIOSK") != nullptr;
}

el::rect OnScreenKeyboard::Panel(const el::rect& bounds)
{
    return {bounds.left, bounds.bottom - keyboard::PanelHeight(bounds.height()), bounds.right, bounds.bottom};
}

std::vector<PlacedKey> OnScreenKeyboard::Keys(const el::rect& bounds) const
{
    const auto panel = Panel(bounds);
    return keyboard::PlaceKeys(mMode, panel.width(), panel.top, panel.height());
}

void OnScreenKeyboard::Update(el::view& view, const void* host, const std::optional<el::rect>& field,
                              const el::rect& viewBounds, bool valueField)
{
    Repeat(view);

    // Nothing moves under a finger: a tap on a dialog's button takes focus from its field as it
    // lands, and with the panel gone the button would be elsewhere when the finger lifts.
    if (view.current_button().down && !mTracking)
    {
        return;
    }

    if (!field)
    {
        mArmed = false;
        mDismissed = false;
    }

    const bool show = field.has_value() && (mArmed || (valueField && !mDismissed));
    const void* nextHost = show ? host : nullptr;
    float nextPan = 0.0f;

    if (show)
    {
        // Where the field is with its layer at rest.
        const float moved = HostedBy(host) ? mPan : 0.0f;
        const auto panel = Panel(viewBounds);
        nextPan = keyboard::FieldPan(field->top + moved, field->bottom + moved, panel.top, panel.height());
    }

    if (nextHost != mHost || nextPan != mPan)
    {
        if (nextHost != mHost)
        {
            mTracking = false;
            mPressed = -1;
            mPressedDef.reset();

            if (nextHost == nullptr)
            {
                mMode = keyboard::Mode::Letters;
                mShift = keyboard::Shift::Off;
            }
        }

        mHost = nextHost;
        mPan = nextPan;
        // The layer moved, with everything in it, or the panel came or went.
        view.refresh();
    }
}

void OnScreenKeyboard::Repeat(el::view& view)
{
    if (!mPressedDef || !Repeats(mPressedDef->kind) || UiSession::Now() < mNextRepeat)
    {
        return;
    }

    mNextRepeat = UiSession::Now() + kRepeatInterval;
    Press(view, *mPressedDef);
}

void OnScreenKeyboard::SendKey(el::view& view, el::key_code key)
{
    mPost([&view, key] {
        view.key({key, el::key_action::press, 0});
        view.key({key, el::key_action::release, 0});
    });
}

void OnScreenKeyboard::SendText(el::view& view, char32_t codepoint)
{
    mPost([&view, codepoint] { view.text({static_cast<std::uint32_t>(codepoint), 0}); });
}

// What happens as a finger lands: the keys that repeat, which should answer at once.
void OnScreenKeyboard::Press(el::view& view, const KeyDef& def)
{
    switch (def.kind)
    {
    case KeyKind::Backspace:
        SendKey(view, el::key_code::backspace);
        break;
    case KeyKind::CaretLeft:
        SendKey(view, el::key_code::left);
        break;
    case KeyKind::CaretRight:
        SendKey(view, el::key_code::right);
        break;
    default:
        break;
    }
}

// What happens as it lifts on a key: the rest, so a finger that slides off changes nothing.
void OnScreenKeyboard::Release(el::view& view, const KeyDef& def)
{
    switch (def.kind)
    {
    case KeyKind::Character:
    case KeyKind::Space:
        SendText(view, mShift == keyboard::Shift::Off ? def.lower : def.upper);
        mShift = keyboard::ShiftAfterCharacter(mShift);
        break;
    case KeyKind::Enter:
        // A field may keep focus through Enter, and a dialog gives it back to its first text box.
        mArmed = false;
        SendKey(view, el::key_code::enter);
        break;
    case KeyKind::Shift:
        mShift = keyboard::NextShift(mShift);
        break;
    case KeyKind::Mode:
        mMode = mMode == keyboard::Mode::Letters ? keyboard::Mode::Symbols : keyboard::Mode::Letters;
        mShift = keyboard::Shift::Off;
        break;
    case KeyKind::Hide:
        mArmed = false;
        mDismissed = true;
        break;
    default:
        break;
    }
}

void OnScreenKeyboard::Click(const el::context& ctx, el::mouse_button btn)
{
    const auto keys = Keys(ctx.bounds);
    const auto panel = Panel(ctx.bounds);
    const int at = keyboard::KeyAt(keys, btn.pos.x, btn.pos.y);

    if (btn.down)
    {
        mTracking = true;
        mPressed = at;
        mPressedDef.reset();

        if (at >= 0)
        {
            mPressedDef = keys[static_cast<std::size_t>(at)].def;
            mNextRepeat = UiSession::Now() + kRepeatDelay;
            Press(ctx.view, *mPressedDef);
        }

        ctx.view.refresh(ctx, panel);
        return;
    }

    mTracking = false;
    mPressed = -1;
    mPressedDef.reset();

    if (at >= 0)
    {
        Release(ctx.view, keys[static_cast<std::size_t>(at)].def);
    }

    ctx.view.refresh(ctx, panel);
}

void OnScreenKeyboard::Drag(const el::context& ctx, el::mouse_button btn)
{
    if (!mTracking)
    {
        return;
    }

    // The key lit follows the finger, and the one it lifts from counts.
    const auto keys = Keys(ctx.bounds);
    const int at = keyboard::KeyAt(keys, btn.pos.x, btn.pos.y);

    if (at != mPressed)
    {
        mPressed = at;

        if (at >= 0)
        {
            mPressedDef = keys[static_cast<std::size_t>(at)].def;
            mNextRepeat = UiSession::Now() + kRepeatDelay;
        }
        else
        {
            mPressedDef.reset();
        }

        ctx.view.refresh(ctx, Panel(ctx.bounds));
    }
}

void OnScreenKeyboard::DrawKey(el::canvas& cnv, const PlacedKey& key, bool pressed)
{
    const auto box = ToRect(key.box);
    const auto kind = key.def.kind;
    const bool enter = kind == KeyKind::Enter;
    const bool shiftOn = kind == KeyKind::Shift && mShift != keyboard::Shift::Off;
    const auto fill = pressed || enter ? mTheme.Accent() : (IsChrome(kind) ? mTheme.Tertiary() : mTheme.Elevated());
    const auto ink = pressed || enter ? mTheme.TextOnDark() : (shiftOn ? mTheme.Accent() : mTheme.Text());
    const el::point centre{(box.left + box.right) * 0.5f, (box.top + box.bottom) * 0.5f};
    const auto label = [&](const std::string& text, float size) {
        FillText(cnv, text, Font(Weight::Regular).size(size), ink, box, HAlign::Centre);
    };

    cnv.fill_style(fill);
    cnv.fill_round_rect(box, kKeyRadius);

    switch (kind)
    {
    case KeyKind::Character:
        label(keyboard::Utf8(mShift == keyboard::Shift::Off ? key.def.lower : key.def.upper), 18.0f);
        break;
    case KeyKind::Space:
        break;
    case KeyKind::Shift:
        DrawShift(cnv, centre, shiftOn, mShift == keyboard::Shift::Locked, ink);
        break;
    case KeyKind::Mode:
        label(mMode == keyboard::Mode::Letters ? "?123" : "ABC", metrics::kTextSize);
        break;
    case KeyKind::Backspace:
        DrawBackspace(cnv, centre, ink);
        break;
    case KeyKind::CaretLeft:
        DrawArrow(cnv, centre, -1.0f, ink);
        break;
    case KeyKind::CaretRight:
        DrawArrow(cnv, centre, 1.0f, ink);
        break;
    case KeyKind::Hide:
        label("Hide", metrics::kTextSize);
        break;
    case KeyKind::Enter:
        label("Enter", metrics::kTextSize);
        break;
    case KeyKind::Gap:
        break;
    }
}

void OnScreenKeyboard::Draw(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    const auto panel = Panel(ctx.bounds);

    // A meter beside the panel repaints every few frames, and nothing here has changed.
    if (!intersects(panel, cnv.clip_extent()))
    {
        return;
    }

    cnv.fill_style(mTheme.Bar());
    cnv.fill_rect(panel);
    cnv.fill_style(mTheme.BorderStrong());
    cnv.fill_rect(el::rect{panel.left, panel.top, panel.right, panel.top + 1.0f});

    const auto keys = Keys(ctx.bounds);

    for (std::size_t i = 0; i < keys.size(); ++i)
    {
        DrawKey(cnv, keys[i], static_cast<int>(i) == mPressed);
    }
}
} // namespace nanoq::ui
