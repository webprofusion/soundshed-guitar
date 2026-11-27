// The footers' Theme button and the flyout it opens, as the web UI's (ts/themeFlyout.ts,
// css/theme-flyout.css): the UI zoom at the top, and every theme as a tile with a picture of the
// app wearing it (images/themes/thumbnails/<theme>.jpg). Choosing a tile applies the theme at once
// and leaves the flyout open, so themes can be tried one after another; a tap outside, Escape or
// the button again closes it.
//
// Under the tiles, where Soundshed Guitar hosts this UI and can draw itself with either: the App
// UI choice, this one (beta) or the web UI (ts/settings/nativeEditor.ts). The first time this UI
// opens there, it says it is a beta and asks for bug reports.
//
// The zoom is the view's scale (the presenter's, in QPlug's hosts), kept in uiSettings.native.zoom
// with Nano Q's other UI settings: in the app settings and the host's state, so a reopened editor,
// a restored project and the next launch come back at it.

#include "ui/shell/Shell.h"

#include "IPluginHost.h"
#include "ui/dialogs/DialogChrome.h"
#include "ui/input/AccessBridge.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>

namespace nanoq::ui
{
namespace
{
namespace theme = guitarfx::uiclient::theme;

// The web UI's choices (settings/keys.ts ZOOM_LEVEL_OPTIONS, ZOOM_MIN, ZOOM_MAX).
constexpr float kZoomStops[] = {0.75f, 0.9f, 1.0f, 1.1f, 1.25f, 1.35f, 1.5f, 1.75f, 2.0f};
constexpr float kZoomMin = 0.5f;
constexpr float kZoomMax = 2.0f;

// The flyout's measures (css/theme-flyout.css).
constexpr float kFlyoutWidth = 432.0f;
constexpr float kFlyoutPad = 10.0f;
constexpr float kGap = 8.0f;
constexpr float kTileMin = 124.0f; // grid-template-columns: repeat(auto-fill, minmax(124px, 1fr))
constexpr float kTilePad = 5.0f;
constexpr float kNameHeight = 16.0f;
constexpr float kHeaderHeight = 28.0f;
constexpr float kAppUiRowHeight = 28.0f; // the joined Segmented's own height (.app-ui-choice)
constexpr float kAppUiHintHeight = 16.0f;

// Set once the beta notice has been seen (ShowNativeUiBetaNoticeOnce), in the shared app settings
// so every instance and the next launch know.
constexpr const char* kBetaNoticeSetting = "ui.nativeEditorBetaNoticeShown";
constexpr const char* kBugReports = "https://github.com/webprofusion/soundshed-guitar/issues";

[[nodiscard]] bool FlagSetting(const nlohmann::json& settings, const char* key, bool fallback)
{
    const auto it = settings.find(key);
    return it != settings.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

[[nodiscard]] std::optional<float> StoredZoom(const nlohmann::json& uiSettings)
{
    const auto native = uiSettings.find("native");

    if (native == uiSettings.end() || !native->is_object())
    {
        return std::nullopt;
    }

    const auto zoom = native->find("zoom");

    if (zoom == native->end() || !zoom->is_number())
    {
        return std::nullopt;
    }

    const float value = zoom->get<float>();

    if (!std::isfinite(value))
    {
        return std::nullopt;
    }

    return std::clamp(value, kZoomMin, kZoomMax);
}

[[nodiscard]] bool SameZoom(float a, float b)
{
    return std::abs(a - b) < 0.001f;
}

[[nodiscard]] int ZoomPercent(float zoom)
{
    return static_cast<int>(std::lround(zoom * 100.0f));
}

/// One theme in the flyout (.theme-flyout-tile): its picture in a square frame, its name under
/// it, and the current one ringed in the accent with a tick after its name.
class ThemeTile final : public Pressable
{
  public:
    ThemeTile(const Theme& theme, const theme::ThemeInfo& info, std::shared_ptr<artist::image> picture,
              const std::string& current, float width)
        : mTheme(theme), mInfo(info), mPicture(std::move(picture)), mCurrent(current), mWidth(width)
    {
        AccessName(std::string(info.label));
    }

    [[nodiscard]] static float Height(float width)
    {
        // The frame is square inside the tile's padding; the name and a gap sit under it.
        return kTilePad + (width - 2.0f * kTilePad) + kTilePad + kNameHeight + kTilePad;
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info = Pressable::Describe();
        info.role = AccessRole::Tab; // one of a radio group
        info.selected = Selected();
        return info;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        const float h = Height(mWidth);
        return {{mWidth, h}, {mWidth, h}};
    }

    void draw(const el::context& ctx) override
    {
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        const bool selected = Selected();

        if (selected)
        {
            cnv.fill_style(mTheme.Accent().opacity(0.12f));
            cnv.fill_round_rect(b, 8.0f);
            cnv.stroke_style(mTheme.Accent());
            cnv.line_width(1.0f);
            cnv.stroke_round_rect(b.inset(0.5f, 0.5f), 8.0f);
        }
        else if (mHover)
        {
            cnv.fill_style(mTheme.Text().opacity(0.06f));
            cnv.fill_round_rect(b, 8.0f);
        }

        // The picture, cropped to the frame's rounded square; an empty frame where a theme has
        // no picture yet (.is-missing).
        const float side = b.width() - 2.0f * kTilePad;
        const el::rect frame{b.left + kTilePad, b.top + kTilePad, b.left + kTilePad + side, b.top + kTilePad + side};
        cnv.fill_style(mTheme.Text().opacity(0.06f));
        cnv.fill_round_rect(frame, 6.0f);

        if (mPicture)
        {
            auto state = cnv.new_state();
            cnv.begin_path();
            cnv.add_round_rect(frame, 6.0f);
            cnv.clip();
            DrawImageCovering(cnv, *mPicture, frame);
        }

        cnv.stroke_style(mTheme.Border());
        cnv.line_width(1.0f);
        cnv.stroke_round_rect(frame.inset(0.5f, 0.5f), 6.0f);

        // The name, ellipsised, with the tick after it on the current theme.
        const auto font = Font(Weight::SemiBold).size(12.0f);
        const el::color ink = selected || mHover ? mTheme.Ink() : mTheme.TextSecondary();
        const float left = b.left + kTilePad + 2.0f;
        const float tickRoom = selected ? 16.0f : 0.0f;
        const el::rect line{left, frame.bottom + kTilePad, b.right - kTilePad - 2.0f,
                            frame.bottom + kTilePad + kNameHeight};
        const std::string name = Ellipsize(cnv, std::string(mInfo.label), font, line.width() - tickRoom);
        FillText(cnv, name, font, ink, line, HAlign::Left);

        if (selected)
        {
            const float x = left + MeasureTextWidth(name, font) + 9.0f;
            const float y = line.top + line.height() * 0.5f;
            cnv.stroke_style(mTheme.Accent());
            cnv.line_width(1.6f);
            cnv.begin_path();
            cnv.move_to(x - 4.0f, y);
            cnv.line_to(x - 1.0f, y + 3.0f);
            cnv.line_to(x + 4.0f, y - 3.0f);
            cnv.stroke();
        }
    }

  private:
    [[nodiscard]] bool Selected() const
    {
        return theme::FindTheme(mCurrent).id == mInfo.id;
    }

    const Theme& mTheme;
    const theme::ThemeInfo& mInfo;
    std::shared_ptr<artist::image> mPicture;
    const std::string& mCurrent; // the client state's theme: the tick follows a change at once
    float mWidth;
};
} // namespace

std::shared_ptr<IconButton> Shell::BuildThemeButton()
{
    // The web UI's palette and "Theme" (#footer-theme-btn): the flyout says which theme is on.
    auto button = std::make_shared<IconButton>(mTheme, mIcons, "ui-theme",
                                               mWide ? IconButton::Look::Footer : IconButton::Look::Boxed);
    button->Size(mWide ? 76.0f : 84.0f, 28).IconSize(15).Label("Theme", 11.0f).AccessName("Theme and zoom");
    button->onClick = [this, b = button.get()] { ToggleThemeFlyout(b->Bounds()); };
    return button;
}

void Shell::ToggleThemeFlyout(el::rect anchor)
{
    if (mThemeFlyout)
    {
        CloseThemeFlyout();
        return;
    }

    OpenThemeFlyout(anchor);
}

void Shell::CloseThemeFlyout()
{
    CloseDropdown(); // its zoom select

    if (!mThemeFlyout)
    {
        return;
    }

    // As CloseOverlay: taken out once the event that closed it is done.
    auto flyout = std::move(mThemeFlyout);
    mThemeFlyout.reset();
    RemoveLayer(std::move(flyout));
}

void Shell::OpenThemeFlyout(el::rect anchor)
{
    const auto view = ViewSize();
    const float width = std::min(kFlyoutWidth, view.x - 16.0f);
    const int count = static_cast<int>(std::size(theme::kThemes));
    // The header, and under the tiles the App UI choice where there is one.
    float appUiHeight = 0.0f;
    Ptr appUi = BuildAppUiChoice(appUiHeight);
    const float headerHeight = kHeaderHeight + 8.0f + 1.0f + kGap + appUiHeight;

    // Above the button when there is more room there (the footer is at the bottom), and kept in
    // the window: a short window scrolls the tiles.
    const float spaceAbove = anchor.top - 6.0f - 8.0f;
    const float spaceBelow = view.y - anchor.bottom - 6.0f - 8.0f;
    const bool above = spaceAbove >= spaceBelow;
    const float room = std::max(160.0f, above ? spaceAbove : spaceBelow);

    // Three across at the flyout's full width, fewer on a narrow window; the tiles leave the
    // scroll bar its own room when they scroll.
    struct Grid
    {
        int columns;
        float tileWidth, height;
    };

    const auto gridFor = [count](float inner) {
        Grid g{};
        g.columns = std::max(1, static_cast<int>(std::floor((inner + kGap) / (kTileMin + kGap))));
        g.tileWidth = std::floor((inner - kGap * static_cast<float>(g.columns - 1)) / static_cast<float>(g.columns));
        const int rows = (count + g.columns - 1) / g.columns;
        g.height = ThemeTile::Height(g.tileWidth) * static_cast<float>(rows) + kGap * static_cast<float>(rows - 1);
        return g;
    };

    Grid grid = gridFor(width - 2.0f * kFlyoutPad);
    const bool scrolls = 2.0f * kFlyoutPad + headerHeight + grid.height > room;

    if (scrolls)
    {
        grid = gridFor(width - 2.0f * kFlyoutPad - el::get_theme().scrollbar_width - 4.0f);
    }

    const float height = std::min(2.0f * kFlyoutPad + headerHeight + grid.height, room);

    // The header: the title, and the zoom's select at the right.
    auto title = std::make_shared<Text>(mTheme, "THEME", 11.0f, Weight::SemiBold);
    title->Colour(mTheme.TextSecondary()).Fixed(true);
    std::vector<Ptr> header{Middle(title), Stretch()};

    if (mSession.getZoom && mSession.setZoom)
    {
        auto label = std::make_shared<Text>(mTheme, "Zoom", 11.0f);
        label->Colour(mTheme.TextSecondary()).Fixed(true);
        header.insert(header.end(), {Middle(label), HGap(8), BuildZoomPicker()});
    }

    auto rule = FixedHeight(1, Fill(mTheme.Text().opacity(0.08f), std::make_shared<el::element>()));

    // The tiles, a row at a time.
    std::vector<std::string> pictures;

    for (const auto& info : theme::kThemes)
    {
        pictures.push_back("images/themes/thumbnails/" + std::string(info.id) + ".jpg");
    }

    mImages.Prefetch(pictures);
    const float tileHeight = ThemeTile::Height(grid.tileWidth);
    std::vector<Ptr> lines;
    std::vector<Ptr> row;
    std::size_t index = 0;

    const auto endRow = [&] {
        row.push_back(Stretch());

        if (!lines.empty())
        {
            lines.push_back(VGap(kGap));
        }

        lines.push_back(FixedHeight(tileHeight, Row(row)));
        row.clear();
    };

    for (const auto& info : theme::kThemes)
    {
        auto tile =
            std::make_shared<ThemeTile>(mTheme, info, mImages.Find(pictures[index++]), mState.theme, grid.tileWidth);
        const std::string id(info.id);
        tile->onClick = [this, id] {
            // Applied at once; the flyout stays open, and its tick follows the state.
            if (theme::FindTheme(mState.theme).id != id)
            {
                mSession.Commands().SetTheme(id);
            }
        };

        if (!row.empty())
        {
            row.push_back(HGap(kGap));
        }

        row.push_back(tile);

        if (static_cast<int>(row.size()) == grid.columns * 2 - 1)
        {
            endRow();
        }
    }

    if (!row.empty())
    {
        endRow();
    }

    lines.push_back(std::make_shared<el::element>());
    Ptr tiles = Col(lines);

    if (scrolls)
    {
        tiles = VScroll(tiles);
    }

    std::vector<Ptr> parts{FixedHeight(kHeaderHeight, Row(header)), VGap(8), rule, VGap(kGap), tiles};

    if (appUi)
    {
        parts.push_back(appUi);
    }

    auto panel = Pad(kFlyoutPad, kFlyoutPad, kFlyoutPad, kFlyoutPad, Col(parts));
    // A layer of its own, over the overlay: at compact density the button is in the Options sheet,
    // which stays open under it, as the web UI's footer sheet does. A tap outside closes only the
    // flyout; the zoom's select opens over it.
    CloseThemeFlyout();
    mThemeFlyoutAnchor = anchor;
    mThemeFlyout = KeyLayer(PlacePanel(panel, anchor, width, height, above, [this] { CloseThemeFlyout(); }));
    mSession.View().add(mThemeFlyout);
}

Ptr Shell::BuildAppUiChoice(float& height)
{
    height = 0.0f;

    if (!mState.environment.nativeEditor || mState.environment.nativeEditorOnly)
    {
        return nullptr;
    }

    // This UI is up, so it is the choice unless a plugin has been told to open the web UI next.
    const bool native = FlagSetting(mState.appSettings, guitarfx::kNativeEditorSettingKey, true);
    auto label = std::make_shared<Text>(mTheme, "APP UI", 11.0f, Weight::SemiBold);
    label->Colour(mTheme.TextSecondary()).Fixed(true);
    auto choice = std::make_shared<Segmented>(mTheme, std::vector<std::string>{"Standard", "Native (Nano)"});
    choice->Joined().Badge(1, "BETA");
    choice->AccessName("App UI");
    choice->Select(native ? 1 : 0);
    // The standalone swaps its editor at once; a plugin's next window follows, so the choice is
    // marked here until then.
    choice->onSelect = [this, c = choice.get()](int index) {
        c->Select(index);
        mSession.View().refresh();
        mSession.Commands().SetSetting(guitarfx::kNativeEditorSettingKey, index == 1);
    };

    auto rule = FixedHeight(1, Fill(mTheme.Text().opacity(0.08f), std::make_shared<el::element>()));
    std::vector<Ptr> lines{VGap(kGap), rule, VGap(kGap),
                           FixedHeight(kAppUiRowHeight, Row({Middle(label), Stretch(), choice}))};
    height = kGap + 1.0f + kGap + kAppUiRowHeight;

    if (!mState.environment.standalone)
    {
        auto hint = std::make_shared<Text>(mTheme, "A change takes effect when the plugin window next opens.", 11.0f);
        hint->Colour(mTheme.TextSecondary());
        lines.insert(lines.end(), {VGap(6), FixedHeight(kAppUiHintHeight, Row({Middle(hint), Stretch()}))});
        height += 6.0f + kAppUiHintHeight;
    }

    return Col(lines);
}

void Shell::ShowNativeUiBetaNoticeOnce()
{
    // Once the settings are in and their theme is on (the text takes its colour when it is
    // made), and only where this UI is the one chosen over the web UI.
    if (mBetaNoticeChecked || !mState.haveState || mState.appSettings.empty() || mAppliedTheme != mState.theme)
    {
        return;
    }

    mBetaNoticeChecked = true;

    if (OfferStandardUiForScreenReader())
    {
        return;
    }

    if (!mState.environment.nativeEditor || mState.environment.nativeEditorOnly ||
        FlagSetting(mState.appSettings, kBetaNoticeSetting, false))
    {
        return;
    }

    mSession.Commands().SetSetting(kBetaNoticeSetting, true);

    // As the other dialogs (Dialogs.cpp DialogWidth): no wider than the window allows.
    const float width = std::min(480.0f, std::max(240.0f, ViewSize().x - 48.0f));
    const auto paragraph = [this, width](const char* text) {
        auto p = std::make_shared<Paragraph>(mTheme, text, width - 40.0f, metrics::kTextSize);
        p->Colour(mTheme.ModalTextSecondary());
        return p;
    };
    auto body = Col({paragraph("The native UI (Nano) is a beta feature, still in development: not everything is "
                               "there yet, and you may find bugs. Please report any you find, so they can be fixed."),
                     VGap(10),
                     paragraph("To go back to the standard UI, choose Standard under App UI in the Theme flyout, "
                               "in the footer."),
                     Stretch()});
    ShowDialog({.title = "Native UI (Nano) is in beta",
                .body = body,
                .actions = {DialogButton(
                                "Report a bug",
                                [this] {
                                    CloseOverlay();
                                    mSession.Commands().OpenUrl(kBugReports);
                                },
                                false),
                            DialogButton(
                                "OK", [this] { CloseOverlay(); }, true)},
                .width = width,
                .height = kDialogHeaderHeight + kDialogFooterHeight + 150.0f});
}

bool Shell::OfferStandardUiForScreenReader()
{
    // Only where the standard UI can be had: Soundshed Guitar hosting this one.
    if (!mState.environment.nativeEditor || mState.environment.nativeEditorOnly ||
        !AccessBridge::UnservedScreenReader())
    {
        return false;
    }

    const bool standalone = mState.environment.standalone;
    OpenConfirm("A screen reader is running",
                std::string("The native UI (Nano) can't be read by a screen reader on this system yet. The standard "
                            "UI can. Use the standard UI?") +
                    (standalone ? "" : " It opens when the plugin window next opens."),
                "Use the standard UI",
                [this] { mSession.Commands().SetSetting(guitarfx::kNativeEditorSettingKey, false); });
    return true;
}

Ptr Shell::BuildZoomPicker()
{
    auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    button->Size(80, 28).Caret(true).AccessName("UI zoom level");
    // Bound: a zoom chosen here, or by the host, shows without rebuilding the flyout.
    mLive.Bind(
        button, [this] { return ZoomPercent(CurrentZoom()); },
        [](IconButton& b, int percent) { b.Label(std::to_string(percent) + "%", 11.0f); });
    button->onClick = [this, b = button.get()] {
        const int current = ZoomPercent(CurrentZoom());
        // The stop nearest the zoom is the one marked, as the web UI's select shows it.
        float nearest = kZoomStops[0];

        for (const float stop : kZoomStops)
        {
            if (std::abs(stop * 100.0f - static_cast<float>(current)) < std::abs(nearest * 100.0f - current))
            {
                nearest = stop;
            }
        }

        std::vector<MenuItem> items;

        for (const float stop : kZoomStops)
        {
            items.push_back({std::to_string(ZoomPercent(stop)) + "%", SameZoom(stop, nearest), false,
                             [this, stop] { SetZoom(stop); }});
        }

        OpenDropdown(b->Bounds(), std::move(items), 96.0f);
    };
    return button;
}

float Shell::CurrentZoom() const
{
    return mSession.getZoom ? mSession.getZoom() : 1.0f;
}

void Shell::SetZoom(float zoom)
{
    if (!mSession.setZoom)
    {
        return;
    }

    zoom = std::clamp(zoom, kZoomMin, kZoomMax);
    mZoomKnown = true;
    mZoomStored = zoom;
    ApplyZoom(zoom);
    mSession.Commands().PatchNativeUiSettings({{"zoom", zoom}});
}

void Shell::StepZoom(int direction)
{
    if (direction == 0)
    {
        SetZoom(1.0f);
        return;
    }

    const float current = CurrentZoom();
    float next = current;

    if (direction > 0)
    {
        const auto* up = std::find_if(std::begin(kZoomStops), std::end(kZoomStops),
                                      [current](float stop) { return stop > current + 0.001f; });
        next = up != std::end(kZoomStops) ? *up : kZoomMax;
    }
    else
    {
        for (const float stop : kZoomStops)
        {
            if (stop < current - 0.001f)
            {
                next = stop;
            }
        }

        next = next == current ? kZoomMin : next;
    }

    SetZoom(next);
}

void Shell::ApplyZoom(float zoom)
{
    const auto before = ViewSize();
    mSession.setZoom(zoom);
    const auto after = ViewSize();

    if (!mThemeFlyout && !mOptionsOpen)
    {
        return;
    }

    // What is open stays open, beside its button where the new zoom puts it, as the web UI
    // places its flyout again: the footer's buttons keep their distance from the window's
    // bottom and nearest side edge, in layout pixels.
    const auto remap = [&](el::rect r) {
        const float dx = r.left + r.width() * 0.5f > before.x * 0.5f ? after.x - before.x : 0.0f;
        const float dy = r.top + r.height() * 0.5f > before.y * 0.5f ? after.y - before.y : 0.0f;
        return el::rect{r.left + dx, r.top + dy, r.right + dx, r.bottom + dy};
    };
    const bool flyout = mThemeFlyout != nullptr;

    if (mOptionsOpen)
    {
        OpenOptions(remap(mOptionsAnchor));
    }

    if (flyout)
    {
        OpenThemeFlyout(remap(mThemeFlyoutAnchor));
    }
}

void Shell::SyncZoom()
{
    if (!mState.haveState || !mSession.getZoom || !mSession.setZoom)
    {
        return;
    }

    const float current = mSession.getZoom();

    // A zoom stored that this view has not seen yet (the first state, a project the host
    // reloaded, another editor's choice) is the one shown.
    if (const auto stored = StoredZoom(mState.uiSettings); stored && (!mZoomKnown || !SameZoom(*stored, mZoomStored)))
    {
        mZoomKnown = true;
        mZoomStored = *stored;

        if (!SameZoom(current, *stored))
        {
            ApplyZoom(*stored);
        }

        return;
    }

    if (!mZoomKnown)
    {
        // Nothing stored yet: what the host opened at stands, and is stored once it changes.
        mZoomKnown = true;
        mZoomStored = current;
        return;
    }

    // Changed by the host (QPlug's zoom keys): stored, as a choice in the flyout is.
    if (!SameZoom(current, mZoomStored))
    {
        mZoomStored = current;
        mSession.Commands().PatchNativeUiSettings({{"zoom", current}});
    }
}
} // namespace nanoq::ui
