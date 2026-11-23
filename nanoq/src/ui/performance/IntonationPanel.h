#pragma once

// The tuner's intonation check, drawn as the web UI's panel (tuner-modal.html .tuner-intonation,
// css/modals/tuner.css): the step to take, the measurement of the note being played with its
// grade, what to do about it, and each string checked with its frets. The maths is
// uiclient/Intonation.h; Instruments.cpp lays this out under the tuner's face with the buttons.

#include "ui/Drawn.h"
#include "ui/widgets/Widgets.h"

#include "uiclient/Intonation.h"

namespace nanoq::ui
{
class IntonationPanel final : public el::element, public Drawn
{
  public:
    /// The two parts, with the buttons between them as the web UI has them: the status (the step,
    /// the measurement card and the advice under it) and the strings checked.
    enum class Part
    {
        Status,
        Strings
    };

    static constexpr float kStatusHeight = 160.0f;
    /// One string's row in the list, and how many show.
    static constexpr float kRowHeight = 26.0f;
    static constexpr int kRows = 6;
    static constexpr float kStringsHeight = kRows * kRowHeight + 8.0f;

    IntonationPanel(const Theme& theme, Part part) : mTheme(theme), mPart(part)
    {
    }

    [[nodiscard]] float Height() const
    {
        return mPart == Part::Status ? kStatusHeight : kStringsHeight;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{300.0f, Height()}, {el::full_extent, Height()}};
    }

    void draw(const el::context& ctx) override;

    void Set(guitarfx::uiclient::intonation::View view)
    {
        mView = std::move(view);
    }

  private:
    [[nodiscard]] el::color QualityColour(guitarfx::uiclient::intonation::Quality quality) const;
    [[nodiscard]] std::string Hint() const;

    void DrawStatus(const el::context& ctx);
    void DrawStrings(const el::context& ctx);

    const Theme& mTheme;
    Part mPart;
    guitarfx::uiclient::intonation::View mView;
};
} // namespace nanoq::ui
