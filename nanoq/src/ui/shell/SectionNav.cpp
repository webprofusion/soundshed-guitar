// The section list the Tones and Jam pages share, as the web UI's sidebars
// (.tone-sharing-nav-btn, .jam-nav-btn, .tab-btn-vertical): in the full layout a column at the
// left (the account, the sections, the page's tools, then a help card), and at the compact size a
// row of chips across the top (the account, the sections, the tools at the right), as the web
// UI's compact panels. Also the pointer to publishing, which stays in Soundshed Guitar.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "uiclient/FeatureFlags.h"

namespace nanoq::ui
{
namespace
{
/// One section's button: a row of the side list (outlined in the accent when chosen), a chip
/// across the top, or the account's pill.
class SectionTab final : public Pressable
{
  public:
    enum class Style
    {
        Side,
        Chip,
        Account
    };

    SectionTab(const Theme& theme, std::string label, bool active, Style style)
        : mTheme(theme), mLabel(std::move(label)), mActive(active), mStyle(style)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = mStyle == Style::Account ? AccessRole::Button : AccessRole::Tab;
        info.name = NameOr(mLabel);
        info.selected = mActive;
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        if (mStyle == Style::Side)
        {
            return {{100.0f, 33.0f}, {el::full_extent, 33.0f}};
        }

        const float w = MeasureTextWidth(mLabel, LabelFont()) + (mStyle == Style::Chip ? 24.0f : 28.0f);
        return {{w, 30.0f}, {w, 30.0f}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;

        if (mStyle == Style::Account)
        {
            const auto pill = b.inset(0.0f, std::max(0.0f, (b.height() - 25.0f) * 0.5f));
            cnv.fill_style(mTheme.Pick(el::rgba(40, 40, 47, 255), mTheme.Card()).level(mHover ? 1.15f : 1.0f));
            cnv.fill_round_rect(pill, pill.height() * 0.5f);
            cnv.stroke_style(mTheme.Border());
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(pill.inset(0.5f, 0.5f), pill.height() * 0.5f);
            FillText(cnv, mLabel, LabelFont(), mTheme.Ink(), pill, HAlign::Centre);
            return;
        }

        if (mActive || mHover)
        {
            cnv.fill_style(mActive ? mTheme.Card() : mTheme.Text().opacity(0.04f));
            cnv.fill_round_rect(b, 6.0f);
        }

        if (mActive)
        {
            cnv.stroke_style(mTheme.Accent());
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 6.0f);
        }

        const auto ink = mActive ? mTheme.Ink() : mTheme.TextSecondary();

        if (mStyle == Style::Side)
        {
            FillText(cnv, mLabel, LabelFont(), ink, el::rect{b.left + 12.0f, b.top, b.right - 8.0f, b.bottom},
                     HAlign::Left);
        }
        else
        {
            FillText(cnv, mLabel, LabelFont(), ink, b, HAlign::Centre);
        }
    }

  private:
    el::font_descr LabelFont() const
    {
        if (mStyle == Style::Chip)
        {
            return Font(Weight::Medium).size(13.0f);
        }

        return Font(mActive ? Weight::SemiBold : Weight::Medium).size(12.0f);
    }

    const Theme& mTheme;
    std::string mLabel;
    bool mActive;
    Style mStyle;
};
} // namespace

Ptr Shell::WithSectionNav(SectionNav nav, Ptr content)
{
    const auto rule = [this](bool vertical) {
        auto line = Fill(mTheme.Text().opacity(0.06f), std::make_shared<el::element>());
        return vertical ? FixedWidth(1, line) : FixedHeight(1, line);
    };

    std::shared_ptr<SectionTab> account;

    if (!nav.account.empty())
    {
        account = std::make_shared<SectionTab>(mTheme, nav.account, false, SectionTab::Style::Account);
        account->onClick = [onAccount = nav.onAccount, a = account.get()] {
            if (onAccount)
            {
                onAccount(a->Bounds());
            }
        };
    }

    if (mWide)
    {
        constexpr float kWidth = 208.0f;
        std::vector<Ptr> column{VGap(24)};

        if (account)
        {
            column.push_back(FixedHeight(25, account));
            column.push_back(VGap(9));
        }

        for (auto& item : nav.items)
        {
            auto tab = std::make_shared<SectionTab>(mTheme, item.label, item.active, SectionTab::Style::Side);
            tab->onClick = std::move(item.onClick);
            column.push_back(FixedHeight(33, tab));
            column.push_back(VGap(6));
        }

        for (auto& tool : nav.tools)
        {
            column.push_back(VGap(6));
            column.push_back(std::move(tool));
        }

        if (!nav.help.empty())
        {
            auto text = std::make_shared<Paragraph>(mTheme, nav.help, kWidth - 20.0f, 12.0f);
            text->Colour(mTheme.Ink());
            column.push_back(VGap(12));
            column.push_back(Over(Pad(10, 10, 10, 10, text), el::share(Surface(mTheme.Card(), mTheme.Border(), 8.0f))));
        }

        column.push_back(std::make_shared<el::element>());
        return Row({FixedWidth(kWidth + 36.0f, Pad(24, 0, 12, 0, Col(column))), rule(true), content});
    }

    // At the compact size: the chips across the top, the tools at their right.
    std::vector<Ptr> row{HGap(10)};

    if (account)
    {
        row.push_back(account);
        row.push_back(HGap(8));
    }

    for (auto& item : nav.items)
    {
        auto tab = std::make_shared<SectionTab>(mTheme, item.label, item.active, SectionTab::Style::Chip);
        tab->onClick = std::move(item.onClick);
        row.push_back(tab);
        row.push_back(HGap(4));
    }

    row.push_back(Stretch());

    for (auto& tool : nav.compactTools)
    {
        row.push_back(std::move(tool));
        row.push_back(HGap(6));
    }

    row.push_back(HGap(4));
    return Col({FixedHeight(46, Pad(0, 8, 0, 8, Row(row))), rule(false), content});
}

void Shell::OpenPublishInfo(bool pack)
{
    // Nano Q installs and previews shared tones, but publishing them is Soundshed Guitar's (its
    // Publish button, its pack editor and the review they go through); what is published shows
    // here under Tones > My Content.
    const std::string what = pack ? "A pack" : "A preset";
    auto text = std::make_shared<Paragraph>(
        mTheme,
        what +
            " is published to Soundshed tone sharing from Soundshed Guitar, the full app or plugin, with the "
            "same account and the same library. " +
            (pack ? std::string("There, choose + Create Pack on its Tones page, add your published presets, and "
                                "submit it.")
                  : std::string("There, load the preset and choose Publish; once it is approved, Share on its card "
                                "gives a link to it.")) +
            " What you publish appears here under Tones > My Content.",
        420.0f, 13.0f);
    text->Colour(mTheme.ModalText());
    const bool tones = guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::ToneSharing);
    std::vector<Ptr> actions;

    if (tones)
    {
        actions.push_back(DialogButton(
            "My Content",
            [this] {
                CloseOverlay();
                mTonesTab = TonesTab::Mine;
                mTonesPage = 1;
                LoadTones(false);
                ShowPage(Page::Tones);
                Mark(kPage);
            },
            false));
    }

    actions.push_back(DialogButton("OK", [this] { CloseOverlay(); }, true));
    ShowDialog({.title = pack ? "Create a pack" : "Publish",
                .body = text,
                .actions = std::move(actions),
                .width = 480.0f,
                .height = 250.0f});
}
} // namespace nanoq::ui
