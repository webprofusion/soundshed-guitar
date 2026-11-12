// Popovers and drop-downs: a panel hung from the control that opened it, with a tap outside
// closing it, and the menu rows the drop-downs are made of.

#include "ui/shell/Shell.h"

#include <algorithm>

namespace nanoq::ui
{
namespace
{
/// Takes a tap that lands outside a popover, and closes it; draws nothing, so what is under it
/// stays in view.
class ClickCatcher final : public el::element
{
  public:
    explicit ClickCatcher(std::function<void()> onClick) : mOnClick(std::move(onClick))
    {
    }

    bool click(const el::context&, el::mouse_button btn) override
    {
        if (!btn.down && mOnClick)
        {
            mOnClick();
        }

        return true;
    }

    bool wants_control() const override
    {
        return true;
    }

  private:
    std::function<void()> mOnClick;
};

/// A row of a drop-down (.preset-extra-action-item): 12 px text, a hover wash, a tick when chosen.
class MenuRow final : public Pressable
{
  public:
    MenuRow(const Theme& theme, std::string label, bool selected, bool heading, el::color ink)
        : mTheme(theme), mLabel(std::move(label)), mSelected(selected), mHeading(heading), mInk(ink)
    {
        Enabled(!heading);
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::MenuItem;
        info.name = NameOr(mLabel);
        info.value = mSubtitle;
        info.selected = mSelected;
        return info;
    }

    /// An icon at the left (the input modes' routing pictures), 30 by 20 in its own tile.
    void Icon(Icons* icons, std::string icon)
    {
        mIcons = icons;
        mIcon = std::move(icon);
    }

    /// A second, muted line under the label: the row is a two-line one.
    void Subtitle(std::string subtitle)
    {
        mSubtitle = std::move(subtitle);
    }

    [[nodiscard]] static float Height(bool heading, bool emptyHeading, bool twoLine)
    {
        return heading ? (emptyHeading ? 9.0f : 24.0f) : (twoLine ? 44.0f : 31.0f);
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        const float h = Height(mHeading, mLabel.empty(), !mSubtitle.empty());
        return {{60.0f, h}, {el::full_extent, h}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;

        if (mHeading)
        {
            if (mLabel.empty())
            {
                cnv.fill_style(mTheme.Text().opacity(0.08f));
                cnv.fill_rect(el::rect{b.left + 6.0f, b.top + 4.0f, b.right - 6.0f, b.top + 5.0f});
                return;
            }

            FillText(cnv, Upper(mLabel), Font(Weight::SemiBold).size(9.0f), mTheme.TextMuted(),
                     el::rect{b.left + 12.0f, b.top + 6.0f, b.right - 8.0f, b.bottom}, HAlign::Left);
            return;
        }

        if (mHover || mSelected)
        {
            cnv.fill_style(mHover ? mTheme.Text().opacity(0.07f) : mTheme.Accent().opacity(0.1f));
            cnv.fill_round_rect(b, 6.0f);
        }

        float left = b.left + 12.0f;

        if (mIcons != nullptr && !mIcon.empty())
        {
            // The icon on a tile of its own (.input-mode-option-icon).
            const float cy = b.top + b.height() * 0.5f;
            const el::rect tile{b.left + 8.0f, cy - 15.0f, b.left + 48.0f, cy + 15.0f};
            cnv.fill_style(mSelected ? mTheme.Accent().opacity(0.14f) : mTheme.Text().opacity(0.06f));
            cnv.fill_round_rect(tile, 6.0f);
            mIcons->Draw(cnv, mIcon, el::rect{tile.left + 5.0f, cy - 10.0f, tile.left + 35.0f, cy + 10.0f},
                         mSelected ? mTheme.Accent() : mTheme.TextSecondary());
            left = tile.right + 10.0f;
        }

        const auto font = Font(Weight::SemiBold).size(12.0f);
        const el::color labelInk = mSelected ? mTheme.Accent() : mInk;

        if (mSubtitle.empty())
        {
            FillText(cnv, Ellipsize(cnv, mLabel, font, b.right - 22.0f - left), font, labelInk,
                     el::rect{left, b.top, b.right - 22.0f, b.bottom}, HAlign::Left);
        }
        else
        {
            const float mid = b.top + b.height() * 0.5f;
            const auto small = Font(Weight::Regular).size(10.5f);
            FillText(cnv, Ellipsize(cnv, mLabel, font, b.right - 22.0f - left), font, labelInk,
                     el::rect{left, mid - 16.0f, b.right - 22.0f, mid}, HAlign::Left);
            FillText(cnv, Ellipsize(cnv, mSubtitle, small, b.right - 22.0f - left), small, mTheme.TextMuted(),
                     el::rect{left, mid, b.right - 22.0f, mid + 14.0f}, HAlign::Left);
        }

        if (mSelected)
        {
            cnv.stroke_style(mTheme.Accent());
            cnv.line_width(1.6f);
            cnv.begin_path();
            const float x = b.right - 18.0f, y = b.top + b.height() * 0.5f;
            cnv.move_to(x - 4.0f, y);
            cnv.line_to(x - 1.0f, y + 3.0f);
            cnv.line_to(x + 4.0f, y - 3.0f);
            cnv.stroke();
        }
    }

  private:
    const Theme& mTheme;
    std::string mLabel;
    bool mSelected, mHeading;
    el::color mInk;
    Icons* mIcons = nullptr;
    std::string mIcon;
    std::string mSubtitle;
};
} // namespace

Ptr Shell::PlacePanel(Ptr panel, el::rect anchor, float width, float height, bool above,
                      std::function<void()> onOutside)
{
    const auto view = ViewSize();
    width = std::min(width, view.x - 16.0f);
    height = std::min(height, view.y - 16.0f);
    // Right-aligned to its button where the button is on the right half, else left-aligned.
    float x = anchor.left + anchor.width() * 0.5f > view.x * 0.5f ? anchor.right - width : anchor.left;
    x = std::clamp(x, 8.0f, std::max(8.0f, view.x - width - 8.0f));
    float y = above ? anchor.top - height - 6.0f : anchor.bottom + 6.0f;
    y = std::clamp(y, 8.0f, std::max(8.0f, view.y - height - 8.0f));

    auto face =
        Over(std::move(panel),
             el::share(Surface(mTheme.Bar(), mTheme.Pick(el::rgba(58, 58, 72, 255), mTheme.BorderStrong()), 10.0f)));
    auto placed = Pad(x, y, std::max(0.0f, view.x - x - width), std::max(0.0f, view.y - y - height),
                      FixedWidth(width, FixedHeight(height, Expand(face))));
    return Over(placed, std::make_shared<ClickCatcher>(std::move(onOutside)));
}

void Shell::ShowPopover(Ptr panel, el::rect anchor, float width, float height, bool above)
{
    CloseOverlay();

    // A chooser: what is listed comes first, and its search box waits for a tap.
    if (mKeyboard)
    {
        mKeyboard->OpenQuietly();
    }

    mOverlay = KeyLayer(PlacePanel(std::move(panel), anchor, width, height, above, [this] { CloseOverlay(); }));
    mSession.View().add(mOverlay);
}

void Shell::CloseDropdown()
{
    if (!mDropdown)
    {
        return;
    }

    // As CloseOverlay: taken out once the event that closed it is done.
    auto dropdown = std::move(mDropdown);
    mDropdown.reset();
    RemoveLayer(std::move(dropdown));
}

void Shell::OpenDropdown(el::rect anchor, std::vector<MenuItem> items, float width)
{
    OpenDropdownColumns(anchor, {std::move(items)}, width);
}

void Shell::OpenDropdownColumns(el::rect anchor, std::vector<std::vector<MenuItem>> columns, float width)
{
    std::vector<Ptr> lists;
    float height = 12.0f;

    for (auto& items : columns)
    {
        std::vector<Ptr> rows;
        float columnHeight = 12.0f;

        for (auto& item : items)
        {
            const bool heading = item.heading || !item.onChoose;
            const el::color ink = item.tint != 0 ? Theme::Argb(item.tint) : mTheme.Ink();
            auto row = std::make_shared<MenuRow>(mTheme, item.label, item.selected, heading, ink);

            if (!item.icon.empty())
            {
                row->Icon(&mIcons, item.icon);
            }

            row->Subtitle(item.subtitle);
            auto choose = item.onChoose;
            row->onClick = [this, choose] {
                // Close first: the choice may open another dialog. Only the list closes: a list
                // opened from a dialog (a filter, a folder) leaves the dialog open.
                CloseDropdown();

                if (choose)
                {
                    choose();
                }
            };
            columnHeight += MenuRow::Height(heading, item.label.empty(), !item.subtitle.empty());
            rows.push_back(row);
        }

        // A shorter column keeps its rows at the top.
        rows.push_back(std::make_shared<el::element>());

        if (!lists.empty())
        {
            lists.push_back(HGap(4));
        }

        lists.push_back(FixedWidth(width, Col(rows)));
        height = std::max(height, columnHeight);
    }

    // Near the bottom of the window (the footer's selects) the list opens upwards.
    const bool above = anchor.top > ViewSize().y * 0.6f;
    const float maxHeight = above ? anchor.top - 16.0f : ViewSize().y - anchor.bottom - 16.0f;
    const float n = static_cast<float>(columns.size());
    // The panel holds the 6 px padding either side as well, one column or more. Given the
    // columns' width alone, a single column's panel was 12 px wider than PlacePanel placed it:
    // right-aligned at the window's edge (the preset menu, the + menu) it ran 12 px past it, and
    // the window grew to hold it and stayed grown.
    width = width * n + 4.0f * (n - 1.0f) + 12.0f;
    Ptr list = lists.size() == 1 ? lists.front() : Row(lists);

    if (height > maxHeight)
    {
        list = VScroll(list);
    }

    CloseDropdown();
    mDropdown = KeyLayer(PlacePanel(PadXY(6, 6, list), anchor, width, std::min(height, std::max(120.0f, maxHeight)),
                                    above, [this] { CloseDropdown(); }));
    mSession.View().add(mDropdown);
}
} // namespace nanoq::ui
