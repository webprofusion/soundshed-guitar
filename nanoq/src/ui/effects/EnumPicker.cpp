// The flyout for an enum with too many choices for a knob (the web UI's paramsPanel/enumPicker.ts):
// Pickup Match's pickups, Verbal's machines. The engine opts a parameter in with
// `control: "picker"` and can describe each label (`labelInfo`: a group, a subgroup, a picture and
// a line of detail). The trigger shows the current choice's picture, name and kind; the flyout has
// a search box, the groups as tiles with their counts, and the choices under subgroup headings.

#include "ui/effects/EnumPicker.h"

#include "ui/shell/Shell.h"

#include "uiclient/ParamFormat.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

namespace
{
/// The view box the pictures are drawn in (pickerIcons.ts), wider than tall like the pickups.
constexpr float kIconBoxW = 32.0f, kIconBoxH = 20.0f;
constexpr float kFlyoutWidth = 420.0f, kFlyoutPreferredHeight = 480.0f;

/// Six pole pieces across the pickup at height `y`, in view-box units.
void Poles(artist::canvas& cnv, const std::function<el::point(float, float)>& at, float scale, float y, float radius)
{
    for (const float x : {6.5f, 10.3f, 14.1f, 17.9f, 21.7f, 25.5f})
    {
        cnv.begin_path();
        cnv.add_circle(el::circle{at(x, y), radius * scale});
        cnv.fill();
    }
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

const std::string& At(const std::vector<std::string>& list, std::size_t i)
{
    static const std::string none;
    return i < list.size() ? list[i] : none;
}

/// Whether choice `i` is in `group` (empty: every group) and holds every word of `query`.
bool Matches(const EffectParamInfo& info, std::size_t i, const std::string& group, const std::string& query)
{
    if (!group.empty() && At(info.labelGroups, i) != group)
    {
        return false;
    }

    const auto text = Lower(info.labels[i] + " " + At(info.labelSubgroups, i) + " " + At(info.labelGroups, i));
    std::size_t start = 0;
    const auto words = Lower(query);

    while (start < words.size())
    {
        const auto end = std::min(words.find(' ', start), words.size());

        if (end > start && text.find(words.substr(start, end - start)) == std::string::npos)
        {
            return false;
        }

        start = end + 1;
    }

    return true;
}

/// The groups in first-appearance order, each with its picture (the first choice's).
std::vector<std::pair<std::string, std::string>> Groups(const EffectParamInfo& info)
{
    std::vector<std::pair<std::string, std::string>> groups;

    for (std::size_t i = 0; i < info.labels.size(); ++i)
    {
        const auto& group = At(info.labelGroups, i);

        if (!group.empty() &&
            std::none_of(groups.begin(), groups.end(), [&](const auto& g) { return g.first == group; }))
        {
            groups.emplace_back(group, At(info.labelIcons, i));
        }
    }

    return groups;
}

void DrawCheck(artist::canvas& cnv, el::point c, el::color colour)
{
    cnv.stroke_style(colour);
    cnv.line_width(1.8f);
    cnv.begin_path();
    cnv.move_to({c.x - 5.0f, c.y});
    cnv.line_to({c.x - 1.5f, c.y + 3.5f});
    cnv.line_to({c.x + 5.0f, c.y - 3.5f});
    cnv.stroke();
}

/// "All" (the web's sliders icon): three rules, each with a knob on it.
void DrawAllIcon(artist::canvas& cnv, el::rect r, el::color colour)
{
    const float side = std::min(r.width(), r.height());
    const el::rect box{r.left + (r.width() - side) * 0.5f, r.top + (r.height() - side) * 0.5f,
                       r.left + (r.width() + side) * 0.5f, r.top + (r.height() + side) * 0.5f};
    cnv.stroke_style(colour);
    cnv.fill_style(colour);
    cnv.line_width(1.6f);
    const float knobs[3] = {0.65f, 0.3f, 0.55f};

    for (int i = 0; i < 3; ++i)
    {
        const float y = box.top + side * (0.22f + 0.28f * static_cast<float>(i));
        cnv.begin_path();
        cnv.move_to({box.left + 2.0f, y});
        cnv.line_to({box.right - 2.0f, y});
        cnv.stroke();
        cnv.begin_path();
        cnv.add_circle(el::circle{box.left + 2.0f + (side - 4.0f) * knobs[i], y, 2.4f});
        cnv.fill();
    }
}

/// A group's tile (.enum-picker-group-tile): its picture, name and how many choices it has.
class GroupTile final : public Pressable
{
  public:
    GroupTile(const Theme& theme, std::string name, std::string icon, int count, bool chosen, bool all)
        : mTheme(theme), mName(std::move(name)), mIcon(std::move(icon)), mCount(count), mChosen(chosen), mAll(all)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo access = Pressable::Describe();
        access.role = AccessRole::Tab;
        access.name = mName + ", " + std::to_string(mCount);
        access.selected = mChosen;
        return access;
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        cnv.fill_style(mChosen ? mTheme.Accent().opacity(0.14f) : mTheme.Ink().opacity(mHover ? 0.1f : 0.05f));
        cnv.fill_round_rect(b, 8.0f);

        if (mChosen)
        {
            cnv.stroke_style(mTheme.Accent().opacity(0.6f));
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 8.0f);
        }

        const float cx = b.left + b.width() * 0.5f;
        const el::rect icon{cx - 18.0f, b.top + 7.0f, cx + 18.0f, b.top + 29.0f};

        if (mAll)
        {
            DrawAllIcon(cnv, icon, mTheme.Accent());
        }
        else
        {
            DrawPickerIcon(cnv, icon, mIcon, mTheme.Accent());
        }

        const auto nameFont = Font(Weight::SemiBold).size(10.0f);
        FillText(cnv, Ellipsize(cnv, mName, nameFont, b.width() - 6.0f), nameFont, mTheme.Ink(),
                 el::rect{b.left + 3.0f, b.top + 32.0f, b.right - 3.0f, b.top + 45.0f}, HAlign::Centre);
        const auto countFont = Font(Weight::Regular).size(9.0f);
        FillText(cnv, std::to_string(mCount), countFont, mTheme.TextSecondary(),
                 el::rect{b.left, b.top + 45.0f, b.right, b.top + 56.0f}, HAlign::Centre);
    }

  private:
    const Theme& mTheme;
    std::string mName, mIcon;
    int mCount;
    bool mChosen, mAll;
};

/// A choice (.enum-picker-option): its picture, its name over its line of detail, a check on the
/// current one.
class PickerOption final : public Pressable
{
  public:
    PickerOption(const Theme& theme, std::string label, std::string detail, std::string icon, bool selected)
        : mTheme(theme), mLabel(std::move(label)), mDetail(std::move(detail)), mIcon(std::move(icon)),
          mSelected(selected)
    {
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo access = Pressable::Describe();
        access.role = AccessRole::ListItem;
        access.name = mDetail.empty() ? mLabel : mLabel + ", " + mDetail;
        access.selected = mSelected;
        return access;
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;

        if (mSelected || mHover)
        {
            cnv.fill_style(mSelected ? mTheme.Accent().opacity(0.12f) : mTheme.Ink().opacity(0.07f));
            cnv.fill_round_rect(b, 8.0f);
        }

        const float cy = b.top + b.height() * 0.5f;
        DrawPickerIcon(cnv, el::rect{b.left + 5.0f, cy - 10.5f, b.left + 39.0f, cy + 10.5f}, mIcon, mTheme.Accent());
        const float left = b.left + 48.0f, right = b.right - (mSelected ? 30.0f : 8.0f);
        const auto font = Font(Weight::SemiBold).size(12.0f);

        if (mDetail.empty())
        {
            FillText(cnv, Ellipsize(cnv, mLabel, font, right - left), font, mTheme.Ink(),
                     el::rect{left, b.top, right, b.bottom}, HAlign::Left);
        }
        else
        {
            const auto small = Font(Weight::Regular).size(10.0f);
            FillText(cnv, Ellipsize(cnv, mLabel, font, right - left), font, mTheme.Ink(),
                     el::rect{left, cy - 15.0f, right, cy + 1.0f}, HAlign::Left);
            FillText(cnv, Ellipsize(cnv, mDetail, small, right - left), small, mTheme.TextSecondary(),
                     el::rect{left, cy + 1.0f, right, cy + 14.0f}, HAlign::Left);
        }

        if (mSelected)
        {
            DrawCheck(cnv, {b.right - 16.0f, cy}, mTheme.Accent());
        }
    }

  private:
    const Theme& mTheme;
    std::string mLabel, mDetail, mIcon;
    bool mSelected;
};
} // namespace

void DrawPickerIcon(artist::canvas& cnv, el::rect r, const std::string& name, el::color colour)
{
    // The 32 x 20 view box fitted into `r`, centred.
    const float scale = std::min(r.width() / kIconBoxW, r.height() / kIconBoxH);
    const float ox = r.left + (r.width() - kIconBoxW * scale) * 0.5f;
    const float oy = r.top + (r.height() - kIconBoxH * scale) * 0.5f;
    const auto at = [=](float x, float y) { return el::point{ox + x * scale, oy + y * scale}; };
    const auto box = [&](float x, float y, float w, float h) { return el::rect{at(x, y), at(x + w, y + h)}; };
    auto state = cnv.new_state();
    cnv.stroke_style(colour);
    cnv.fill_style(colour);
    cnv.line_width(1.4f * scale);

    if (name == "pickup-single")
    {
        cnv.stroke_round_rect(box(2.5f, 6.0f, 27.0f, 8.0f), 4.0f * scale);
        Poles(cnv, at, scale, 10.0f, 1.15f);
    }
    else if (name == "pickup-p90")
    {
        cnv.stroke_round_rect(box(3.0f, 3.5f, 26.0f, 13.0f), 2.6f * scale);
        Poles(cnv, at, scale, 10.0f, 1.3f);
    }
    else if (name == "pickup-humbucker")
    {
        cnv.stroke_round_rect(box(2.0f, 2.5f, 28.0f, 15.0f), 2.0f * scale);
        cnv.line_width(1.0f * scale);
        cnv.stroke_round_rect(box(3.6f, 4.0f, 24.8f, 5.2f), 2.6f * scale);
        cnv.stroke_round_rect(box(3.6f, 10.8f, 24.8f, 5.2f), 2.6f * scale);
        Poles(cnv, at, scale, 6.6f, 0.95f);
        Poles(cnv, at, scale, 13.4f, 0.95f);
    }
    else if (name == "pickup-active")
    {
        cnv.fill_style(colour.opacity(colour.alpha * 0.18f));
        cnv.fill_round_rect(box(2.0f, 2.5f, 28.0f, 15.0f), 2.0f * scale);
        cnv.stroke_round_rect(box(2.0f, 2.5f, 28.0f, 15.0f), 2.0f * scale);
        // The bolt, for the battery.
        cnv.fill_style(colour);
        cnv.begin_path();
        cnv.move_to(at(17.0f, 4.8f));
        cnv.line_to(at(12.8f, 10.6f));
        cnv.line_to(at(16.4f, 10.6f));
        cnv.line_to(at(15.0f, 15.2f));
        cnv.line_to(at(19.2f, 9.4f));
        cnv.line_to(at(15.6f, 9.4f));
        cnv.close_path();
        cnv.fill();
    }
    else if (name == "pickup-custom")
    {
        cnv.stroke_round_rect(box(2.5f, 4.0f, 27.0f, 12.0f), 3.0f * scale);
        // Its winding, drawn through it.
        cnv.line_width(1.2f * scale);
        cnv.begin_path();
        cnv.move_to(at(5.5f, 10.0f));

        for (int i = 0; i < 5; ++i)
        {
            const float x = 5.5f + 4.2f * static_cast<float>(i);
            const float bulge = i % 2 == 0 ? -3.2f : 3.2f;
            cnv.bezier_curve_to(at(x + 1.4f, 10.0f + bulge), at(x + 2.8f, 10.0f + bulge), at(x + 4.2f, 10.0f));
        }

        cnv.stroke();
    }
    else
    {
        // "pickup-other", and any name this table does not know: a dashed outline.
        StrokeDashedRoundRect(cnv, box(3.0f, 4.0f, 26.0f, 12.0f), 3.0f * scale, 2.5f * scale, 2.0f * scale);
    }
}

EnumPickerTrigger::EnumPickerTrigger(const Theme& theme, Icons& icons, EffectParamInfo info)
    : mTheme(theme), mIcons(icons), mInfo(std::move(info))
{
}

AccessInfo EnumPickerTrigger::Describe() const
{
    AccessInfo access = Pressable::Describe();
    access.role = AccessRole::ComboBox;
    access.name = NameOr(mInfo.name);
    access.value = Label();
    return access;
}

el::view_limits EnumPickerTrigger::limits(const el::basic_context&) const
{
    return {{180.0f, 40.0f}, {280.0f, 40.0f}};
}

void EnumPickerTrigger::draw(const el::context& ctx)
{
    NoteDrawn(ctx);
    auto& cnv = ctx.canvas;
    const auto b = ctx.bounds;
    cnv.fill_style(mTheme.Input());
    cnv.fill_round_rect(b, 8.0f);
    cnv.stroke_style(mHover ? mTheme.Accent() : mTheme.Border());
    cnv.line_width(1.0f);
    cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 8.0f);

    const auto index = static_cast<std::size_t>(std::max(0, mIndex));
    const float cy = b.top + b.height() * 0.5f;
    DrawPickerIcon(cnv, el::rect{b.left + 8.0f, cy - 12.5f, b.left + 48.0f, cy + 12.5f}, At(mInfo.labelIcons, index),
                   mTheme.Accent());

    const float left = b.left + 56.0f, right = b.right - 28.0f;
    const auto label = Label();
    const auto& subgroup = At(mInfo.labelSubgroups, index);
    const auto subtitle = !subgroup.empty() && subgroup != label ? subgroup : At(mInfo.labelGroups, index);
    const auto font = Font(Weight::SemiBold).size(12.0f);

    if (subtitle.empty())
    {
        FillText(cnv, Ellipsize(cnv, label, font, right - left), font, mTheme.Ink(),
                 el::rect{left, b.top, right, b.bottom}, HAlign::Left);
    }
    else
    {
        const auto small = Font(Weight::Regular).size(10.0f);
        FillText(cnv, Ellipsize(cnv, label, font, right - left), font, mTheme.Ink(),
                 el::rect{left, cy - 15.0f, right, cy + 1.0f}, HAlign::Left);
        FillText(cnv, Ellipsize(cnv, subtitle, small, right - left), small, mTheme.TextSecondary(),
                 el::rect{left, cy + 1.0f, right, cy + 14.0f}, HAlign::Left);
    }

    mIcons.Draw(cnv, "ui-caret", el::rect{b.right - 21.0f, cy - 5.0f, b.right - 11.0f, cy + 5.0f},
                mTheme.TextSecondary());
}

std::string EnumPickerTrigger::Label() const
{
    return mIndex >= 0 && static_cast<std::size_t>(mIndex) < mInfo.labels.size()
               ? mInfo.labels[static_cast<std::size_t>(mIndex)]
               : std::string();
}

void Shell::OpenEnumPicker(const std::string& nodeId, const EffectParamInfo& info, el::rect anchor)
{
    const auto* node = SelectedNode();

    if (node == nullptr || node->id != nodeId || info.labels.empty())
    {
        return;
    }

    struct PickerState
    {
        std::string query;
        std::string group;
        int selected = -1;
        SlotPtr groups = MakeSlot();
        SlotPtr list = MakeSlot();
        std::function<void()> fill;
    };

    auto state = std::make_shared<PickerState>();
    auto* s = state.get();
    s->selected = EnumLabelIndex(info, NodeParamValue(*node, info));
    s->group = s->selected >= 0 ? At(info.labelGroups, static_cast<std::size_t>(s->selected)) : std::string();
    const auto groups = Groups(info);
    const bool anyDetail =
        std::any_of(info.labelDetails.begin(), info.labelDetails.end(), [](const auto& d) { return !d.empty(); });
    const float rowHeight = anyDetail ? 36.0f : 30.0f;
    constexpr float kTileHeight = 58.0f, kTileGap = 6.0f, kInner = kFlyoutWidth - 20.0f;
    // As the web's grid: tiles at least 54 px, as many as fit across.
    const int perRow = std::max(1, static_cast<int>((kInner + kTileGap) / (54.0f + kTileGap)));
    const int tileCount = static_cast<int>(groups.size()) + 1;
    const int tileRows = groups.size() > 1 ? (tileCount + perRow - 1) / perRow : 0;
    const float groupsHeight = tileRows > 0 ? tileRows * kTileHeight + (tileRows - 1) * kTileGap : 0.0f;

    auto choose = [this, nodeId, info, weak = std::weak_ptr<PickerState>(state)](std::size_t index) {
        if (weak.expired())
        {
            return; // a second tap on the closed flyout, before its layer has gone
        }

        // Closed first: nothing may rebuild the row that was tapped while its tap is handled.
        CloseOverlay();
        mSession.Commands().SetNodeParam(nodeId, info.key, EnumLabelValue(info, index));
        NoteChainEdit();
        Mark(kValues | kPage);
    };

    s->fill = [this, s, info, groups, rowHeight, perRow, tileRows, choose] {
        // The tiles count what the search leaves.
        if (tileRows > 0)
        {
            std::vector<Ptr> rows;
            std::vector<Ptr> row;
            const auto tile = [&](const std::string& name, const std::string& icon, bool all) {
                int count = 0;

                for (std::size_t i = 0; i < info.labels.size(); ++i)
                {
                    count += Matches(info, i, all ? std::string() : name, s->query) ? 1 : 0;
                }

                auto t = std::make_shared<GroupTile>(mTheme, all ? "All" : name, icon, count,
                                                     all ? s->group.empty() : s->group == name, all);
                t->onClick = [s, value = all ? std::string() : name] {
                    s->group = value;

                    if (s->fill)
                    {
                        s->fill();
                    }
                };

                if (!row.empty())
                {
                    row.push_back(HGap(kTileGap));
                }

                row.push_back(Stretch(t));

                if (static_cast<int>((row.size() + 1) / 2) == perRow)
                {
                    rows.push_back(FixedHeight(kTileHeight, Row(row)));
                    row.clear();
                }
            };

            tile("", "", true);

            for (const auto& [name, icon] : groups)
            {
                tile(name, icon, false);
            }

            if (!row.empty())
            {
                // A short last row keeps the tiles' width.
                const int missing = perRow - static_cast<int>((row.size() + 1) / 2);

                for (int i = 0; i < missing; ++i)
                {
                    row.push_back(HGap(kTileGap));
                    row.push_back(Stretch());
                }

                rows.push_back(FixedHeight(kTileHeight, Row(row)));
            }

            std::vector<Ptr> column;

            for (auto& r : rows)
            {
                if (!column.empty())
                {
                    column.push_back(VGap(kTileGap));
                }

                column.push_back(std::move(r));
            }

            Fill(*s->groups, Col(column));
        }

        // The choices in index order, under their subgroup headings.
        std::vector<Ptr> list;
        std::string lastSubgroup = "\x01";

        for (std::size_t i = 0; i < info.labels.size(); ++i)
        {
            if (!Matches(info, i, s->group, s->query))
            {
                continue;
            }

            const auto& subgroup = At(info.labelSubgroups, i);

            if (subgroup != lastSubgroup && !subgroup.empty())
            {
                auto heading = std::make_shared<Text>(mTheme, Upper(subgroup), 10.0f, Weight::SemiBold);
                heading->AccessHeading(3); // a section title, to a screen reader
                heading->Colour(mTheme.TextSecondary()).Tracking(0.06f);
                list.push_back(FixedHeight(22, Pad(6, 6, 6, 3, heading)));
            }

            lastSubgroup = subgroup;
            auto option = std::make_shared<PickerOption>(mTheme, info.labels[i], At(info.labelDetails, i),
                                                         At(info.labelIcons, i), static_cast<int>(i) == s->selected);
            option->onClick = [choose, i] { choose(i); };
            list.push_back(FixedHeight(rowHeight, option));
            list.push_back(VGap(1));
        }

        if (list.empty())
        {
            auto none = std::make_shared<Text>(mTheme, "Nothing matches", 12.0f);
            none->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
            list.push_back(FixedHeight(40, none));
        }

        list.push_back(std::make_shared<el::element>());
        Fill(*s->list, VScroll(Pad(0, 0, 6, 0, Col(list))));
        mSession.View().layout();
        mSession.View().refresh();
    };
    s->fill();

    // The header (.enum-picker-header): the parameter's name and the search.
    auto title = std::make_shared<Text>(mTheme, Upper(info.name), 11.0f, Weight::SemiBold);
    title->AccessHeading(2); // a section title, to a screen reader
    title->Colour(mTheme.TextSecondary()).Tracking(0.06f).Fixed(true);
    auto [searchElement, searchBox] =
        el::input_box("Search " + std::to_string(info.labels.size()) + " choices", Font(Weight::Regular), 1.0f);
    searchBox->on_text = [weak = std::weak_ptr<PickerState>(state)](std::string_view text) {
        if (const auto live = weak.lock(); live && live->fill)
        {
            live->query = std::string(text);
            live->fill();
        }
    };
    auto search = Over(PadXY(9, 0, Middle(el::share(std::move(searchElement)))),
                       el::share(Surface(mTheme.Input(), mTheme.Border(), 6.0f)));
    const float titleWidth = MeasureTextWidth(Upper(info.name), Font(Weight::SemiBold).size(11.0f)) * 1.1f + 4.0f;
    auto header = Row({FixedWidth(titleWidth, Middle(FixedHeight(14, title))), HGap(10), search});

    // Under the trigger, or over it where there is more room above and too little below.
    const auto view = ViewSize();
    const float below = view.y - anchor.bottom - 6.0f - 8.0f;
    const float aboveRoom = anchor.top - 6.0f - 8.0f;
    const bool above = below < kFlyoutPreferredHeight && aboveRoom > below;
    const float listRows = static_cast<float>(info.labels.size()) * (rowHeight + 1.0f);
    const float natural = 10.0f + 30.0f + 8.0f + (tileRows > 0 ? groupsHeight + 8.0f : 0.0f) + listRows + 10.0f;
    const float height =
        std::clamp(std::min(natural, kFlyoutPreferredHeight), 160.0f, std::max(160.0f, above ? aboveRoom : below));

    std::vector<Ptr> column{FixedHeight(30, header), VGap(8)};

    if (tileRows > 0)
    {
        column.push_back(FixedHeight(groupsHeight, s->groups));
        column.push_back(VGap(8));
    }

    column.push_back(s->list);
    ShowPopover(Pad(10, 10, 10, 10, Col(column)), anchor, kFlyoutWidth, height, above);
    // The state lives as long as the flyout.
    mOnOverlayClosed = [state] { state->fill = nullptr; };
}
} // namespace nanoq::ui
