// The tuner's intonation panel (see IntonationPanel.h); the words are tunerIntonationView.ts's.

#include "ui/performance/IntonationPanel.h"

#include "ui/widgets/TextDraw.h"

namespace nanoq::ui
{
namespace intonation = guitarfx::uiclient::intonation;
using cycfi::artist::canvas;

el::color IntonationPanel::QualityColour(intonation::Quality quality) const
{
    switch (quality)
    {
    case intonation::Quality::Excellent:
    case intonation::Quality::Good:
        return mTheme.Success();
    case intonation::Quality::Fair:
        return mTheme.Accent();
    case intonation::Quality::Poor:
        return mTheme.Warning();
    }

    return mTheme.ModalRule();
}

std::string IntonationPanel::Hint() const
{
    const auto& v = mView;

    if (!v.hasString)
    {
        if (!v.candidate)
        {
            return "Tune an open string, pick it gently and let it ring, then set it as the reference.";
        }

        return "Heard " + v.candidate->name + " (" + intonation::FormatCents(v.candidate->cents) +
               "). Set it as the open string, or retune and play it again.";
    }

    if (v.offString)
    {
        return "That note isn't on the " + v.string.name + " string (below it, or past fret " +
               std::to_string(intonation::kMaxFret) + "). Play this string, or start a new one.";
    }

    return "Play the " + v.string.name +
           " string at any fret, picking gently and letting it ring; the 12th is the usual check. The fret is "
           "found from the pitch.";
}

void IntonationPanel::draw(const el::context& ctx)
{
    NoteDrawn(ctx);

    if (mPart == Part::Status)
    {
        DrawStatus(ctx);
    }
    else
    {
        DrawStrings(ctx);
    }
}

void IntonationPanel::DrawStatus(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const float left = b.left + 20.0f, right = b.right - 20.0f;
    const auto body = Font(Weight::Regular).size(13.0f);
    const float line = LineHeight(cnv, body);

    // The rule between the tuner's face and the panel (.tuner-intonation's border-top).
    cnv.fill_style(mTheme.ModalRule());
    cnv.fill_rect(el::rect{b.left, b.top, b.right, b.top + 1.0f});

    float y = b.top + 14.0f;
    const auto paragraph = [&](const std::string& text, el::color colour, int maxLines) {
        for (const auto& l : WrapLines(cnv, text, body, right - left, maxLines))
        {
            FillText(cnv, l, body, colour, el::rect{left, y, right, y + line}, HAlign::Left);
            y += line;
        }
    };
    paragraph(Hint(), mTheme.ModalTextSecondary(), 3);
    y += 8.0f;

    // The measurement card (.tuner-intonation-result): the fret, how far out, the grade.
    if (mView.hasString)
    {
        const el::rect card{left, y, right, y + 40.0f};
        const auto& m = mView.measurement;
        const el::color colour = m ? QualityColour(m->quality) : mTheme.ModalRule();
        cnv.fill_style(mTheme.ModalRaised());
        cnv.begin_path();
        cnv.add_round_rect(card, 8.0f);
        cnv.fill();
        cnv.stroke_style(mTheme.ModalRule());
        cnv.line_width(1.0f);
        cnv.begin_path();
        cnv.add_round_rect(card, 8.0f);
        cnv.stroke();
        cnv.fill_style(colour);
        cnv.fill_rect(el::rect{card.left, card.top + 4.0f, card.left + 4.0f, card.bottom - 4.0f});

        const auto big = Font(Weight::SemiBold).size(16.0f);
        const el::rect text{card.left + 16.0f, card.top, card.right - 14.0f, card.bottom};

        if (m)
        {
            const std::string fret = m->fret == 0 ? "Open" : "Fret " + std::to_string(m->fret);
            FillText(cnv, fret, big, mTheme.ModalText(), text, HAlign::Left);
            const float fretWidth = el::measure_text(cnv, fret, big).x;
            FillText(cnv, intonation::FormatCents(m->cents), Font(Weight::Regular).size(16.0f), mTheme.ModalText(),
                     el::rect{text.left + fretWidth + 12.0f, text.top, text.right, text.bottom}, HAlign::Left);
            FillText(cnv, intonation::QualityLabel(m->quality), Font(Weight::SemiBold).size(13.0f), colour, text,
                     HAlign::Right);
        }
        else
        {
            FillText(cnv, "Fret \xE2\x80\x93", big, mTheme.ModalText(), text, HAlign::Left);
            FillText(cnv, "Waiting for a steady note", body, mTheme.ModalTextMuted(), text, HAlign::Right);
        }

        y = card.bottom + 8.0f;

        // What to do: about this fret, then what the string's frets say together.
        std::string advice = m ? intonation::Advice(m->fret, m->cents) : std::string();

        if (advice.empty())
        {
            for (const auto& s : intonation::StringAdvice(mView.string.frets))
            {
                advice += (advice.empty() ? "" : " ") + s;
            }
        }

        paragraph(advice, mTheme.ModalTextSecondary(), 2);
    }
}

void IntonationPanel::DrawStrings(const el::context& ctx)
{
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    const float left = b.left + 20.0f, right = b.right - 20.0f;

    // Each string checked (.tuner-intonation-strings): its name and its frets as chips.
    float rowY = b.top + 4.0f;
    const auto nameFont = Font(Weight::SemiBold).size(13.0f);
    const auto chipFont = Font(Weight::Regular).size(12.0f);
    int shown = 0;

    for (const auto& s : mView.strings)
    {
        if (shown++ == kRows)
        {
            break;
        }

        const el::rect row{left - 8.0f, rowY, right + 8.0f, rowY + kRowHeight - 2.0f};

        if (mView.hasString && s.name == mView.string.name)
        {
            cnv.fill_style(mTheme.ModalRaised());
            cnv.begin_path();
            cnv.add_round_rect(row, 6.0f);
            cnv.fill();
        }

        FillText(cnv, s.name, nameFont, mTheme.ModalText(), el::rect{left, row.top, left + 36.0f, row.bottom},
                 HAlign::Left);
        float x = left + 46.0f;

        if (s.frets.empty())
        {
            FillText(cnv, "No frets checked yet", chipFont, mTheme.ModalTextMuted(),
                     el::rect{x, row.top, right, row.bottom}, HAlign::Left);
        }

        for (const auto& f : s.frets)
        {
            const std::string label = std::to_string(f.fret) + " " + intonation::FormatCents(f.cents);
            const float w = el::measure_text(cnv, label, chipFont).x + 14.0f;

            if (x + w > right)
            {
                break; // the rest do not fit on the row
            }

            const el::rect chip{x, row.top + 3.0f, x + w, row.bottom - 3.0f};
            cnv.stroke_style(QualityColour(f.quality));
            cnv.line_width(1.0f);
            cnv.begin_path();
            cnv.add_round_rect(chip, chip.height() * 0.5f);
            cnv.stroke();
            FillText(cnv, label, chipFont, mTheme.ModalText(), chip, HAlign::Centre);
            x += w + 4.0f;
        }

        rowY += kRowHeight;
    }
}
} // namespace nanoq::ui
