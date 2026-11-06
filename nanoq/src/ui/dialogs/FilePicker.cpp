// The file dialog drawn in the view, for a host with no native one (Linux: UiLink's
// TakeFileDialogRequest). A kiosk has no desktop for a GTK dialog or a portal to appear on, so
// this is part of the app: it takes the whole window, its rows and buttons are big enough for a
// finger on a 7 inch screen, and its list scrolls by dragging, as the touch screen reaches X as a
// pointer and a scroll bar is a poor target. A tap opens: there is no double tap to learn.
//
// It opens over whatever asked for the file, the model chooser above all, and that dialog is
// still there when it closes. A disk can be slow to answer, or gone, so folders are read on a
// thread of their own; a stick pulled out while it is showing drops the picker back to Home.

#include "ui/shell/Shell.h"

#include "FileBrowsing.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string_view>
#include <thread>
#include <utility>

namespace nanoq::ui
{
namespace
{
constexpr float kRowHeight = 52.0f;
constexpr float kBarHeight = 56.0f;
constexpr float kPlacesHeight = 52.0f;
constexpr float kPathHeight = 44.0f;
constexpr float kFooterHeight = 60.0f;
constexpr float kButtonHeight = 40.0f;
constexpr float kPlaceWidth = 150.0f;
constexpr float kWheelStep = kRowHeight / 2.0f;
constexpr double kMountCheckSeconds = 1.0;

std::string SizeText(std::uintmax_t bytes)
{
    char text[32];

    if (bytes >= 1024u * 1024u)
    {
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    }
    else
    {
        std::snprintf(text, sizeof(text), "%.0f KB", std::ceil(static_cast<double>(bytes) / 1024.0));
    }

    return text;
}

/// The rows: owner-drawn, not one element each, since a USB stick can hold thousands of files and
/// the list scrolls under a finger. Scrolls by a drag anywhere on it, or the wheel. With focus,
/// the arrow keys walk it and Enter takes the row.
class FileList final : public el::element, public Drawn, public Focusable
{
  public:
    FileList(const Theme& theme, Icons& icons) : mTheme(theme), mIcons(icons)
    {
    }

    std::function<void(const FileEntry&)> onPick;

    /// Shows these rows from the top; `message` stands in for an empty list.
    void Set(std::vector<FileEntry> entries, std::string message)
    {
        mEntries = std::move(entries);
        mMessage = std::move(message);
        mOffset = 0.0f;
        mPressed = -1;
        mCursor = mEntries.empty() ? -1 : 0;
    }

    el::view_limits limits(const el::basic_context&) const override
    {
        return {{100.0f, kRowHeight}, {el::full_extent, el::full_extent}};
    }

    void draw(const el::context& ctx) override
    {
        NoteDrawn(ctx);
        auto& cnv = ctx.canvas;
        const auto b = ctx.bounds;
        mViewHeight = b.height();
        mOffset = Clamped(mOffset);
        const auto nameFont = Font(Weight::Medium).size(15.0f);
        const auto sizeFont = Font(Weight::Regular).size(12.0f);

        auto state = cnv.new_state();
        cnv.add_rect(b);
        cnv.clip();

        if (mEntries.empty())
        {
            FillText(cnv, mMessage, Font(Weight::Regular).size(14.0f), mTheme.ModalTextMuted(), b, HAlign::Centre);
            return;
        }

        const int first = static_cast<int>(mOffset / kRowHeight);

        for (int i = first; i < static_cast<int>(mEntries.size()); ++i)
        {
            const float top = b.top + static_cast<float>(i) * kRowHeight - mOffset;

            if (top >= b.bottom)
            {
                break;
            }

            const auto& entry = mEntries[static_cast<std::size_t>(i)];
            const el::rect row{b.left, top, b.right, top + kRowHeight};

            if (i == mPressed || (i == mCursor && HasFocus()))
            {
                cnv.fill_style(mTheme.ModalText().opacity(0.10f));
                cnv.fill_rect(row);
            }

            cnv.fill_style(mTheme.ModalRule());
            cnv.fill_rect(el::rect{row.left + 16.0f, row.bottom - 1.0f, row.right - 16.0f, row.bottom});

            const float iconTop = top + (kRowHeight - 22.0f) / 2.0f;
            mIcons.Draw(cnv, entry.folder ? "ui-folder" : "ui-file",
                        el::rect{b.left + 16.0f, iconTop, b.left + 38.0f, iconTop + 22.0f},
                        entry.folder ? mTheme.Accent() : mTheme.ModalTextMuted());

            float right = b.right - 20.0f;

            if (!entry.folder)
            {
                FillText(cnv, SizeText(entry.bytes), sizeFont, mTheme.ModalTextMuted(),
                         el::rect{right - 80.0f, row.top, right, row.bottom}, HAlign::Right);
                right -= 92.0f;
            }

            const float left = b.left + 52.0f;
            FillText(cnv, Ellipsize(cnv, entry.name, nameFont, right - left), nameFont, mTheme.ModalText(),
                     el::rect{left, row.top, right, row.bottom}, HAlign::Left);
        }

        // Where the list is: for the eye only, a finger drags the rows themselves.
        if (const float content = ContentHeight(); content > mViewHeight)
        {
            const float length = std::max(36.0f, mViewHeight * mViewHeight / content);
            const float top = b.top + (mViewHeight - length) * (mOffset / (content - mViewHeight));
            cnv.fill_style(mTheme.ModalText().opacity(0.25f));
            cnv.fill_round_rect(el::rect{b.right - 8.0f, top, b.right - 3.0f, top + length}, 2.5f);
        }
    }

    bool click(const el::context& ctx, el::mouse_button btn) override
    {
        if (btn.state != el::mouse_button::left)
        {
            return false;
        }

        if (btn.down)
        {
            mPressY = btn.pos.y;
            mPressOffset = mOffset;
            mDragging = false;
            mPressed = RowAt(ctx.bounds, btn.pos);
            ctx.view.refresh(ctx);
            return true;
        }

        const int pressed = mPressed;
        mPressed = -1;
        ctx.view.refresh(ctx);

        if (!mDragging && pressed >= 0 && pressed == RowAt(ctx.bounds, btn.pos))
        {
            Pick(pressed);
        }

        return true;
    }

    void drag(const el::context& ctx, el::mouse_button btn) override
    {
        const float moved = mPressY - btn.pos.y;

        if (!mDragging && std::abs(moved) < kDragSlop)
        {
            return;
        }

        mDragging = true;
        mPressed = -1;
        mOffset = Clamped(mPressOffset + moved);
        ctx.view.refresh(ctx);
    }

    bool scroll(const el::context& ctx, el::point dir, el::point) override
    {
        mOffset = Clamped(mOffset - dir.y * kWheelStep);
        ctx.view.refresh(ctx);
        return true;
    }

    bool wants_control() const override
    {
        return true;
    }

    bool wants_focus() const override
    {
        return FocusWanted(true);
    }

    void begin_focus(focus_request) override
    {
        NoteFocus(true);
    }

    bool end_focus() override
    {
        NoteFocus(false);
        return true;
    }

    bool key(const el::context& ctx, el::key_info k) override
    {
        if (k.action != el::key_action::press && k.action != el::key_action::repeat)
        {
            return false;
        }

        const int page = std::max(1, static_cast<int>(mViewHeight / kRowHeight) - 1);
        int step = 0;

        switch (k.key)
        {
        case el::key_code::up:
            step = -1;
            break;
        case el::key_code::down:
            step = 1;
            break;
        case el::key_code::page_up:
            step = -page;
            break;
        case el::key_code::page_down:
            step = page;
            break;
        case el::key_code::enter:
        case el::key_code::kp_enter:
            return Pick(mCursor);
        default:
            return false;
        }

        if (mEntries.empty())
        {
            return true;
        }

        mCursor = std::clamp(mCursor + step, 0, static_cast<int>(mEntries.size()) - 1);
        const float top = static_cast<float>(mCursor) * kRowHeight;
        mOffset = Clamped(std::clamp(mOffset, top + kRowHeight - mViewHeight, top));
        ctx.view.refresh(ctx);
        return true;
    }

    [[nodiscard]] AccessInfo Describe() const override
    {
        AccessInfo info;
        info.role = AccessRole::ListItem;

        if (mCursor >= 0 && mCursor < static_cast<int>(mEntries.size()))
        {
            info.name = mEntries[static_cast<std::size_t>(mCursor)].name;
        }

        return info;
    }

    bool Activate(const el::rect&) override
    {
        return Pick(mCursor);
    }

  private:
    [[nodiscard]] float ContentHeight() const
    {
        return static_cast<float>(mEntries.size()) * kRowHeight;
    }

    [[nodiscard]] float Clamped(float offset) const
    {
        return std::clamp(offset, 0.0f, std::max(0.0f, ContentHeight() - mViewHeight));
    }

    [[nodiscard]] int RowAt(const el::rect& bounds, el::point p) const
    {
        if (!bounds.includes(p))
        {
            return -1;
        }

        const int row = static_cast<int>((p.y - bounds.top + mOffset) / kRowHeight);
        return row < static_cast<int>(mEntries.size()) ? row : -1;
    }

    bool Pick(int row)
    {
        if (row < 0 || row >= static_cast<int>(mEntries.size()) || !onPick)
        {
            return false;
        }

        // A copy: what it opens replaces the rows.
        const auto entry = mEntries[static_cast<std::size_t>(row)];
        const auto pick = onPick;
        pick(entry);
        return true;
    }

    const Theme& mTheme;
    Icons& mIcons;
    std::vector<FileEntry> mEntries;
    std::string mMessage;
    float mOffset = 0.0f; // how far down the rows the view starts
    float mViewHeight = 0.0f;
    float mPressY = 0.0f, mPressOffset = 0.0f;
    bool mDragging = false;
    int mPressed = -1; // the row under a finger
    int mCursor = -1;  // the keys' row
};
} // namespace

struct Shell::FilePickerState
{
    FileDialogRequest request;
    std::filesystem::path home;
    std::filesystem::path folder;
    std::filesystem::path disk; // the removable place `folder` is on, if any

    std::string mounts; // the text `places` came from
    std::vector<FilePlace> places;
    bool placesStale = true;
    double nextMountCheck = 0.0;

    int loading = 0; // the latest read: a slower, older one is ignored when it lands

    std::shared_ptr<FileList> list;
    std::shared_ptr<Text> where;
    std::shared_ptr<Text> status;
    std::shared_ptr<IconButton> up;
    std::shared_ptr<IconButton> confirm;
    std::shared_ptr<el::basic_input_box> nameBox;
    std::string name; // a save's file name, as the box last had it
    SlotPtr placesSlot = MakeSlot();
    bool replacing = false; // a save was told the file exists; the button now says so
};

void Shell::OpenFilePicker(FileDialogRequest request)
{
    // One at a time; nothing else can ask while it covers the window, short of the engine itself.
    if (mFilePicker)
    {
        request.done({});
        return;
    }

    auto picker = std::make_shared<FilePickerState>();
    picker->request = std::move(request);
    picker->home = HomeFolder();
    picker->mounts = ReadMounts();
    picker->places = FilePlaces(picker->home, picker->mounts);
    const auto type = picker->request.type;
    const bool save = picker->request.save;
    const bool folderOnly = type == guitarfx::BrowseFileType::Folder;

    auto title = std::make_shared<Text>(mTheme, picker->request.title, 17.0f, Weight::SemiBold);
    title->Colour(mTheme.ModalText());
    auto cancel = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
    cancel->Size(100, kButtonHeight).Label("Cancel", 14.0f);
    cancel->onClick = [this] { CloseFilePicker({}); };

    picker->up = std::make_shared<IconButton>(mTheme, mIcons, "ui-up", IconButton::Look::Boxed);
    picker->up->Size(56, kButtonHeight - 4.0f).IconSize(18);
    picker->up->onClick = [this, weak = std::weak_ptr<FilePickerState>(picker)] {
        if (const auto p = weak.lock())
        {
            FilePickerGoTo(p->folder.parent_path());
        }
    };
    picker->where = std::make_shared<Text>(mTheme, "", 13.0f, Weight::Medium);
    picker->where->Colour(mTheme.ModalTextMuted());

    picker->list = std::make_shared<FileList>(mTheme, mIcons);
    picker->list->onPick = [this, save, folderOnly,
                            weak = std::weak_ptr<FilePickerState>(picker)](const FileEntry& entry) {
        const auto p = weak.lock();

        if (!p)
        {
            return;
        }

        if (entry.folder)
        {
            FilePickerGoTo(entry.path);
        }
        else if (save)
        {
            p->nameBox->set_text(std::string_view(entry.name)); // replacing it is asked about at Save
            p->name = entry.name;
            p->replacing = false;
            mSession.View().refresh();
        }
        else if (!folderOnly)
        {
            guitarfx::BrowseFileResult result;
            result.success = true;
            result.path = entry.path;
            CloseFilePicker(result);
        }
    };

    std::vector<Ptr> column{
        FixedHeight(kBarHeight, Fill(mTheme.ModalRaised(), Pad(16, 0, 12, 0, Middle(Row({Stretch(title), cancel}))))),
        FixedHeight(kPlacesHeight, Pad(12, 8, 12, 4, picker->placesSlot)),
        FixedHeight(kPathHeight, Pad(12, 4, 12, 4, Row({picker->up, HGap(10), Stretch(Middle(picker->where))}))),
        Expand(picker->list)};

    if (save || folderOnly)
    {
        picker->confirm = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Primary);
        picker->confirm->Size(save ? 110.0f : 170.0f, kButtonHeight).Label(save ? "Save" : "Use this folder", 14.0f);
        picker->confirm->onClick = [this] { FilePickerConfirm(); };
        std::vector<Ptr> footer;

        if (save)
        {
            auto [element, box] = el::input_box("File name", Font(Weight::Regular), 1.0f);
            picker->nameBox = box;
            box->set_text(std::string_view(picker->request.defaultName));
            picker->name = picker->request.defaultName;
            box->on_text = [weak = std::weak_ptr<FilePickerState>(picker)](std::string_view text) {
                if (const auto p = weak.lock())
                {
                    p->name = std::string(text);
                    p->replacing = false;
                }
            };
            picker->status = std::make_shared<Text>(mTheme, "", 12.0f);
            picker->status->Colour(mTheme.Error());
            footer = {Stretch(Middle(el::share(std::move(element))), 2.0), HGap(10), Stretch(Middle(picker->status)),
                      HGap(10), Middle(picker->confirm)};
        }
        else
        {
            footer = {Stretch(), picker->confirm};
        }

        column.push_back(FixedHeight(kFooterHeight, Fill(mTheme.ModalRaised(), Pad(16, 0, 16, 0, Row(footer)))));
    }

    // The face takes every click, so nothing under the picker hears one.
    mFilePickerLayer = KeyLayer(Over(Col(column), el::share(Surface(mTheme.ModalBg(), mTheme.ModalBg(), 0.0f))));
    mFilePicker = picker;
    mSession.View().add(mFilePickerLayer);

    // Where it was last used, or a stick if one is in (the reason to open it on a kiosk).
    auto start = picker->home;

    if (!mLastPickedFolder.empty())
    {
        start = mLastPickedFolder;
    }
    else if (const auto disk = std::find_if(picker->places.begin(), picker->places.end(),
                                            [](const FilePlace& place) { return place.removable; });
             disk != picker->places.end())
    {
        start = disk->path;
    }

    FilePickerGoTo(start);
}

void Shell::CloseFilePicker(const guitarfx::BrowseFileResult& result)
{
    if (!mFilePicker)
    {
        return;
    }

    const auto picker = std::exchange(mFilePicker, nullptr);
    RemoveLayer(std::exchange(mFilePickerLayer, nullptr));

    if (result.success)
    {
        mLastPickedFolder = picker->folder;
    }

    picker->request.done(result);
}

void Shell::FilePickerGoTo(std::filesystem::path folder)
{
    const auto picker = mFilePicker;

    if (!picker)
    {
        return;
    }

    picker->folder = folder;
    picker->disk.clear();
    picker->placesStale = true;

    for (const auto& place : picker->places)
    {
        if (place.removable && IsInside(folder, place.path))
        {
            picker->disk = place.path;
        }
    }

    const int id = ++picker->loading;
    picker->where->Set(guitarfx::util::PathToUtf8(folder));
    picker->up->Disabled(folder == folder.parent_path());
    picker->list->Set({}, "Reading...");
    mSession.View().refresh();

    std::thread([post = mSession.Poster(), weak = std::weak_ptr<FilePickerState>(picker), this, folder, id,
                 type = picker->request.type] {
        auto listing = ListFolder(folder, type);
        post([this, weak, id, listing = std::move(listing)]() mutable {
            const auto picker = weak.lock();

            // Posted to the session, which the shell does not outlive; and the state is only
            // kept while the picker is open, so a closed one finds nothing here.
            if (!picker || picker->loading != id)
            {
                return;
            }

            std::string message;

            if (!listing.error.empty())
            {
                message = "Could not read this folder: " + listing.error;
            }
            else if (picker->request.type == guitarfx::BrowseFileType::Folder || picker->request.save)
            {
                message = "No folders here";
            }
            else
            {
                const bool noDisk = std::none_of(picker->places.begin(), picker->places.end(),
                                                 [](const FilePlace& place) { return place.removable; });
                message = noDisk ? "Nothing to open here. A USB stick shows up above when it is plugged in."
                                 : "Nothing to open here";
            }

            picker->list->Set(std::move(listing.entries), std::move(message));
            mSession.View().refresh();
        });
    }).detach();
}

void Shell::FilePickerConfirm()
{
    const auto picker = mFilePicker;

    if (!picker)
    {
        return;
    }

    guitarfx::BrowseFileResult result;
    result.success = true;
    result.path = picker->folder;

    if (picker->request.save)
    {
        const auto& name = picker->name;

        if (name.empty() || name.find('/') != std::string::npos)
        {
            picker->status->Set("Needs a file name");
            mSession.View().refresh();
            return;
        }

        result.path = picker->folder / guitarfx::util::PathFromUtf8(name);
        std::error_code ec;

        if (!picker->replacing && std::filesystem::exists(result.path, ec))
        {
            picker->replacing = true;
            picker->confirm->Label("Replace", 14.0f);
            picker->status->Set("That file is already here");
            mSession.View().refresh();
            return;
        }
    }

    CloseFilePicker(result);
}

void Shell::FillFilePickerPlaces()
{
    const auto picker = mFilePicker;
    picker->placesStale = false;
    const float room = ViewSize().x - 24.0f;
    const float gaps = static_cast<float>(picker->places.size() - 1) * 8.0f;
    const float width = std::min(kPlaceWidth, (room - gaps) / static_cast<float>(picker->places.size()));
    std::vector<Ptr> row;

    for (const auto& place : picker->places)
    {
        if (!row.empty())
        {
            row.push_back(HGap(8));
        }

        const bool here = place.removable ? picker->disk == place.path
                                          : picker->disk.empty() && IsInside(picker->folder, place.path) &&
                                                (place.path == picker->home || !IsInside(picker->folder, picker->home));
        auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        button->Size(width, kButtonHeight).Label(place.name, 13.0f).Lit(here);
        button->onClick = [this, path = place.path] { FilePickerGoTo(path); };
        row.push_back(button);
    }

    row.push_back(Stretch());
    Fill(*picker->placesSlot, Row(row));
    mSession.View().layout();
    mSession.View().refresh();
}

void Shell::TickFilePicker()
{
    if (auto request = mSession.Link().TakeFileDialogRequest())
    {
        OpenFilePicker(std::move(*request));
    }

    if (!mFilePicker)
    {
        return;
    }

    const auto picker = mFilePicker;

    if (const double now = UiSession::Now(); now >= picker->nextMountCheck)
    {
        picker->nextMountCheck = now + kMountCheckSeconds;

        if (auto mounts = ReadMounts(); mounts != picker->mounts)
        {
            picker->mounts = std::move(mounts);
            picker->places = FilePlaces(picker->home, picker->mounts);
            picker->placesStale = true;

            // The disk it was showing has been taken out.
            if (!picker->disk.empty() &&
                std::none_of(picker->places.begin(), picker->places.end(),
                             [&picker](const FilePlace& place) { return place.path == picker->disk; }))
            {
                FilePickerGoTo(picker->home);
            }
        }
    }

    // Here, between events: a button in that row may be what was just tapped.
    if (picker->placesStale)
    {
        FillFilePickerPlaces();
    }
}
} // namespace nanoq::ui
