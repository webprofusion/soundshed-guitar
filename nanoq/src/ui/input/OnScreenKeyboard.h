#pragma once

// OnScreenKeyboard - typing on a touch screen with no keyboard (the Pi kiosk, tools/linux-kiosk).
//
// Drawn with Elements inside the app, not a program beside it: the kiosk's bare X server has no
// window manager to place a second window, and no program there would know where the field is.
// Off unless SOUNDSHED_KIOSK is set, or SOUNDSHED_NANOQ_KEYBOARD says otherwise (Wanted), so the
// desktop and DAW builds do no more than test one pointer.
//
// It shows in the layer holding focus, and it is not a layer of its own: Elements ends the focus
// of the layer on top the moment a press lands on any other layer, and the text box would lose
// its caret (and commit) on the first key. So the KeyRouter at the root of the focused layer draws
// it over that layer and takes the presses on it before they reach anything else, which leaves
// focus where it was. Keys reach the field as a physical keyboard's do (view::text and view::key),
// so every field works without knowing.
//
// Having focus is not enough to show it: a dialog's router gives its first text box focus again
// after every tap, and a chooser's search box has it from the moment it opens, when the list under
// it is what the user came for. It shows for a press on a field, for a value being typed into a
// knob, and for a dialog that opens to ask for text. A chooser opens with OpenQuietly, so its search
// box waits for a tap. Enter and Hide put it away, with focus left in place, and so does a tap
// anywhere but a field.
//
// The panel sits across the foot of the view and the layer moves up by Pan() when the field would
// otherwise be under it: KeyRouter::prepare_subject shifts its subject's bounds, which moves
// drawing and hit testing together.

#include "ui/input/KeyboardLayout.h"
#include "ui/widgets/Theme.h"

#include <elements.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace nanoq::ui
{
class OnScreenKeyboard
{
  public:
    /// Posts work to run between UI events, as a key reaching a field may close its dialog.
    using Post = std::function<void(std::function<void()>)>;

    OnScreenKeyboard(const Theme& theme, Post post) : mTheme(theme), mPost(std::move(post))
    {
    }

    /// Whether this process should have one. SOUNDSHED_NANOQ_KEYBOARD=0 turns it off on a kiosk
    /// with a real keyboard, =1 turns it on anywhere; unset, it follows SOUNDSHED_KIOSK.
    [[nodiscard]] static bool Wanted();

    /// Each tick. `host` is the focused layer's router, `field` where its field is now (with the
    /// layer moved by Pan() if `host` is hosting), and null with nothing being typed in.
    /// `valueField`: a knob's value, which is only ever open to be typed into.
    void Update(el::view& view, const void* host, const std::optional<el::rect>& field, const el::rect& viewBounds,
                bool valueField);

    /// A press landed on a field.
    void Arm()
    {
        mArmed = true;
    }

    /// A layer with a text box opened. It shows, unless the layer opened quietly.
    void NoteLayerOpened()
    {
        mArmed = !mQuiet;
        mQuiet = false;
    }

    /// The next layer to open is a chooser: its search box waits for a tap.
    void OpenQuietly()
    {
        mQuiet = true;
    }

    /// The view is going: nothing draws this again.
    void Detach()
    {
        mHost = nullptr;
    }

    [[nodiscard]] bool HostedBy(const void* router) const
    {
        return mHost != nullptr && mHost == router;
    }

    /// Showing, in some layer.
    [[nodiscard]] bool Showing() const
    {
        return mHost != nullptr;
    }

    [[nodiscard]] float Pan() const
    {
        return mPan;
    }

    /// The height of the panel in a view this tall.
    [[nodiscard]] static float PanelHeight(float viewHeight)
    {
        return keyboard::PanelHeight(viewHeight);
    }

    [[nodiscard]] bool Tracking() const
    {
        return mTracking;
    }

    [[nodiscard]] bool Contains(const el::rect& bounds, el::point p) const
    {
        return bounds.includes(p) && p.y >= Panel(bounds).top;
    }

    void Draw(const el::context& ctx);
    void Click(const el::context& ctx, el::mouse_button btn);
    void Drag(const el::context& ctx, el::mouse_button btn);

    /// A press anywhere but the panel: what it lands on says what happens next.
    void NotePointerDown()
    {
        mArmed = false;
        mQuiet = false;
    }

  private:
    [[nodiscard]] static el::rect Panel(const el::rect& bounds);
    [[nodiscard]] std::vector<keyboard::PlacedKey> Keys(const el::rect& bounds) const;

    void Press(el::view& view, const keyboard::KeyDef& def);
    void Release(el::view& view, const keyboard::KeyDef& def);
    void SendKey(el::view& view, el::key_code key);
    void SendText(el::view& view, char32_t codepoint);
    void Repeat(el::view& view);
    void DrawKey(el::canvas& cnv, const keyboard::PlacedKey& key, bool pressed);

    const Theme& mTheme;
    Post mPost;
    const void* mHost = nullptr; // only ever compared: the router may have gone
    float mPan = 0.0f;
    bool mArmed = false;
    bool mQuiet = false;
    bool mDismissed = false; // Hide, on a value field: it has no tap of its own to come back with
    keyboard::Mode mMode = keyboard::Mode::Letters;
    keyboard::Shift mShift = keyboard::Shift::Off;
    bool mTracking = false;
    int mPressed = -1;
    std::optional<keyboard::KeyDef> mPressedDef;
    double mNextRepeat = 0.0;
};

/// A layer that shares the foot of the view with the panel (a player bar): it steps aside while
/// the panel shows, as it would otherwise be drawn over the keys and take their presses.
class BesideKeyboard final : public el::proxy_base
{
  public:
    BesideKeyboard(el::element_ptr subject, std::shared_ptr<OnScreenKeyboard> keyboard)
        : mSubject(std::move(subject)), mKeyboard(std::move(keyboard))
    {
    }

    el::element& subject() override
    {
        return *mSubject;
    }

    const el::element& subject() const override
    {
        return *mSubject;
    }

    void draw(const el::context& ctx) override
    {
        if (!mKeyboard->Showing())
        {
            proxy_base::draw(ctx);
        }
    }

    bool wants_control() const override
    {
        return !mKeyboard->Showing() && proxy_base::wants_control();
    }

  private:
    el::element_ptr mSubject;
    std::shared_ptr<OnScreenKeyboard> mKeyboard;
};
} // namespace nanoq::ui
