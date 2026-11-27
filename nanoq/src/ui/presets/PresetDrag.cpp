// Dragging in the preset library (PresetDrag.h): a preset onto a folder, the setlist editor or a
// setlist pad; a folder onto another folder; a setlist slot onto another slot. As the web UI's
// presets/drag.ts, folderDrag.ts and setlists.ts: what is dragged reads by the pointer, the place
// it would land is outlined, and it lands once it is let go (after the release, as the drop
// rebuilds what it was dragged from). A preset dragged out of the chooser fades it once it has
// left it, so the pads under it can take the drop (.drag-released).

#include "ui/shell/Shell.h"

#include "uiclient/PresetBrowse.h"

#include <algorithm>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

namespace
{
/// How much of a nested folder's left edge is its indent: dropped there, a folder goes beside it
/// rather than into it (folderDrag.ts's INDENT_DROP_PX), past the indent itself.
constexpr float kIndentDrop = 16.0f;

/// Whether `spot` takes `item`.
bool Takes(const ClientState& state, const LibraryDrag& item, const DropSpot& spot)
{
    using What = LibraryDrag::What;
    using Kind = DropSpot::Kind;

    switch (item.what)
    {
    case What::Preset:
        return (spot.kind == Kind::Folder && spot.id != kPresetFolderRecents) ||
               (spot.kind == Kind::SetlistPanel && !spot.id.empty()) || spot.kind == Kind::Pad;

    case What::Folder: {
        if (spot.kind != Kind::Folder || spot.id == item.id || spot.id == kPresetFolderRecents ||
            spot.id == kPresetFolderFavorites)
        {
            return false;
        }

        // Not into a folder inside itself: the engine refuses that anyway.
        const auto* self = FindPresetFolder(state.presetFolders, item.id);
        return self == nullptr || FindPresetFolder(self->children, spot.id) == nullptr;
    }

    case What::SetlistSlot:
        return spot.kind == Kind::SetlistSlot && spot.id == item.id && spot.index != item.index;
    }

    return false;
}
} // namespace

Ptr Shell::DropTarget(DropSpot spot, Ptr subject)
{
    mDropSpots.erase(std::remove_if(mDropSpots.begin(), mDropSpots.end(),
                                    [](const std::weak_ptr<DropSpot>& s) { return s.expired(); }),
                     mDropSpots.end());
    auto shared = std::make_shared<DropSpot>(std::move(spot));
    mDropSpots.push_back(shared);
    return std::make_shared<DropArea>(mTheme, std::move(shared), std::move(subject));
}

Ptr Shell::LibraryDragSource(LibraryDrag item, Ptr subject)
{
    auto source = std::make_shared<DragSource>(std::move(subject));
    source->onDrag = [this, item = std::move(item)](el::point to, bool done) { DragLibraryItem(item, to, done); };
    return source;
}

void Shell::DragLibraryItem(const LibraryDrag& item, el::point to, bool done)
{
    // Begun: only what is drawn from here on counts (PresetDrag.h), so the window is redrawn.
    if (!mLibraryDragging && !done)
    {
        mLibraryDragging = true;
        mLibraryDragLeftChooser = false;

        for (const auto& weak : mDropSpots)
        {
            if (const auto s = weak.lock())
            {
                s->drawn = false;
                s->lit = false;
            }
        }

        mLibraryGhost = std::make_shared<DragGhost>(mTheme, item.name);
        mSession.View().add(mLibraryGhost);
    }

    if (mLibraryGhost)
    {
        std::static_pointer_cast<DragGhost>(mLibraryGhost)->MoveTo(to);
    }

    // What is under the pointer, the one on top first; the chooser's face covers what is
    // under it, unless the preset has left it and it has faded.
    std::vector<std::shared_ptr<DropSpot>> under;
    bool coverDrawn = false, inCover = false;

    for (const auto& weak : mDropSpots)
    {
        const auto s = weak.lock();

        if (!s || !s->drawn)
        {
            continue;
        }

        // The chooser's face reaches past its content by the popover's own inset.
        const auto bounds = s->kind == DropSpot::Kind::Cover ? s->bounds.inset(-14.0f, -12.0f) : s->bounds;

        if (s->kind == DropSpot::Kind::Cover)
        {
            coverDrawn = true;
            inCover = inCover || bounds.includes(to);
        }

        if (bounds.includes(to) && !(mLibraryDragLeftChooser && s->inChooser))
        {
            under.push_back(s);
        }
    }

    // Not until the chooser has been drawn since the drag began: it is not known where it is yet.
    if (item.what == LibraryDrag::What::Preset && item.fromChooser && coverDrawn && !inCover && !done &&
        !mLibraryDragLeftChooser)
    {
        mLibraryDragLeftChooser = true;
        *mOverlayAlpha = 0.25f;
        under.erase(std::remove_if(under.begin(), under.end(), [](const auto& s) { return s->inChooser; }),
                    under.end());
    }

    std::stable_sort(under.begin(), under.end(), [](const auto& a, const auto& b) { return a->Over(*b); });
    std::shared_ptr<DropSpot> target;

    for (const auto& s : under)
    {
        if (Takes(mState, item, *s))
        {
            target = s;
            break;
        }

        if (s->kind == DropSpot::Kind::Cover)
        {
            break;
        }
    }

    if (const auto lit = mDropLit.lock(); lit && lit != target)
    {
        lit->lit = false;
    }

    mDropLit.reset();

    if (target && !done)
    {
        target->lit = true;
        mDropLit = target;
    }

    if (!done)
    {
        mSession.View().refresh();
        return;
    }

    // Let go: the ghost and the fade go, and it lands, once the release is over.
    mLibraryDragging = false;
    mLibraryDragLeftChooser = false;
    *mOverlayAlpha = 1.0f;

    if (mLibraryGhost)
    {
        RemoveLayer(std::move(mLibraryGhost));
        mLibraryGhost.reset();
    }

    mSession.View().refresh();

    if (!target)
    {
        return;
    }

    const auto spot = *target;
    const float offsetX = to.x - spot.bounds.left;
    mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), item, spot, offsetX] {
        if (!alive.lock())
        {
            return;
        }

        auto& commands = mSession.Commands();

        switch (item.what)
        {
        case LibraryDrag::What::Preset:

            if (spot.kind == DropSpot::Kind::Folder)
            {
                // Favourites favourites it, as the web's movePresetToFolder.
                if (spot.id == kPresetFolderFavorites)
                {
                    commands.SetPresetFavorite(item.id, !IsPresetFavorite(mState, item.id));
                }
                else
                {
                    commands.MovePresetToFolder(item.id, spot.id);
                }
            }
            else if (spot.kind == DropSpot::Kind::SetlistPanel)
            {
                commands.AddSetlistSlot(spot.id, item.id);
            }
            else if (spot.kind == DropSpot::Kind::Pad)
            {
                AssignSetlistPad(spot.index, item.id);
            }

            break;

        case LibraryDrag::What::Folder: {
            // All Presets is the top level; a nested folder's indent, its parent.
            const bool beside = spot.depth > 0 && offsetX < static_cast<float>(spot.depth) * 12.0f + kIndentDrop;
            const std::string parent = spot.id == kPresetFolderAll ? std::string() : beside ? spot.parentId : spot.id;
            commands.MovePresetFolder(item.id, parent);
            break;
        }

        case LibraryDrag::What::SetlistSlot:
            commands.MoveSetlistSlot(item.id, item.index, spot.index);
            break;
        }
    });
}
} // namespace nanoq::ui
