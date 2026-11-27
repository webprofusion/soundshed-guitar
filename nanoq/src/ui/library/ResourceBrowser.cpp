// The model and IR chooser, as the web UI's resource browser (ui-components/modals/
// resource-browser-modal.html, ts/resourceBrowser/): a dialog with three sources for a node's
// slot - the Resource Library, a Folder on disk, and Tone 3000 through Soundshed's relay. A click
// loads the choice into the node at once, so it can be heard; OK keeps it, Cancel puts back what
// the slot held.
//
// - Library: the library's entries of the slot's type, by category, with a search and a
//   category filter; File... asks the engine for a file dialog.
// - Folder: the folders the user added (the web UI's setting, so both products share them),
//   listed by the engine (listResourceFolder). A file is heard where it is; OK adds it to the
//   library in place (saveLocalLibraryResource), or picks the library entry it already is.
//   "Look up details" finds which Tone3000 tone listed files come from, ten a time, and files of
//   one tone are drawn as one group (uiclient/FolderToneLookup.h).
// - Tone 3000: a search by gear, sort and architecture; a tone opens to its models; Select
//   downloads one, stores it in the library and loads it (ToneService::ImportTone3000).

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "ui/library/ResourceBrowserParts.h"

#include "dsp/EffectGuids.h"
#include "uiclient/FeatureFlags.h"
#include "uiclient/FolderToneLookup.h"
#include "uiclient/NodeLabels.h"
#include "uiclient/ResourceIndex.h"
#include "uiclient/ResourceThumbnails.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <map>
#include <optional>
#include <cctype>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;
namespace t3 = guitarfx::uiclient::tone3000;

using namespace resource_browser;

/// What a slot's browser had when it closed, put back when it opens again: the tab, the
/// library's filters, and the Tone3000 search with its results and open tone (so reopening asks
/// Tone3000 nothing). The library category is not: each open starts on the slot's own.
struct ResourceBrowserMemory
{
    int tab = 0; // the Tab below
    std::string query, arch, creator;
    std::vector<std::string> tags;
    bool favouritesOnly = false;
    std::map<std::string, bool> openPacks;
    std::string t3Query, t3Gear, t3Sort = "downloads-all-time", t3Arch;
    int t3Page = 1, t3Pages = 1;
    std::vector<t3::Tone> t3Tones;
    std::string t3Open;
    std::vector<t3::Model> t3Models;

    // What the slot's previous and next walk: the list its choice was last picked from.
    int stepFrom = 0;                                     // the Tab it was picked on
    std::vector<ResourceFolderListing::File> folderFiles; // the Folder list then shown
    std::map<std::string, std::vector<t3::Model>> t3ModelsByTone;
    std::optional<std::string> category; // the library category the browser last showed ("" all)
    bool stepping = false;               // a Tone3000 step fetching or importing
};

void Shell::OpenResourceBrowser(const std::string& nodeId, const std::string& resourceType, int resourceIndex,
                                std::function<void(const std::string& resourceId)> onPick, const std::string& title,
                                const std::string& category)
{
    // With onPick the chooser only picks a library entry for the caller (the Simple Cabinet's
    // Match an IR): nothing is loaded into the node, and only the library is offered, filtered
    // to `category` (the web UI's libraryCategoryHint).
    const auto* node = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;

    if (node == nullptr)
    {
        return;
    }

    enum class Tab
    {
        Library,
        Folder,
        Tone3000
    };

    struct Browser
    {
        std::string nodeId, resourceType;
        int index = -1;
        SlotRole role;
        guitarfx::ResourceRef original;
        bool hadOriginal = false;
        bool previewed = false;
        std::string selectedId, selectedPath; // the choice being heard
        Tab tab = Tab::Library;

        std::string query, category; // Library
        std::string arch, creator;   // "" for all
        std::vector<std::string> tags;
        bool favouritesOnly = false;
        std::map<std::string, bool> openPacks; // a pack the user opened or closed
        std::string filter;                    // Folder
        std::string rootId;
        // The files the Folder list shows after its filter: what "Look up details" looks up.
        std::vector<ResourceFolderListing::File> listedFiles;
        std::shared_ptr<IconButton> lookupButton;
        std::function<void()> updateLookupButton;
        std::string t3Query, t3Gear, t3Sort = "downloads-all-time", t3Arch;
        int t3Page = 1, t3Pages = 1;
        bool t3Loading = false;
        std::string t3Error;
        std::vector<t3::Tone> t3Tones;
        std::string t3Open;    // the tone showing its models
        std::string t3Hearing; // a Tone3000 model being auditioned (previewRemoteResource), not imported
        std::optional<t3::Tone> t3HearingTone;
        std::optional<t3::Model> t3HearingModel;
        std::vector<t3::Model> t3Models;
        bool t3ModelsLoading = false;

        SlotPtr tabs = MakeSlot(), controls = MakeSlot(), list = MakeSlot(), footer = MakeSlot();
        SlotPtr pager = MakeSlot();             // Tone 3000's Prev / Page / Next, under its list
        std::weak_ptr<el::element> selectedRow; // the Library row of the choice, brought into view on open
        std::function<void()> fillTabs, fillControls, fillList, fillFooter;
        std::function<void(int)> searchTone3000;
        std::shared_ptr<el::basic_input_box> searchBox, filterBox, t3Box;
        Ptr searchElement, filterElement, t3Element;
        std::vector<Subscription> subscriptions;
        int toneListener = 0;
        // The Tone3000 pictures the Library and Folder rows show (uiclient/ResourceThumbnails.h).
        std::unique_ptr<ResourceThumbnailFetcher> thumbnails;
        std::function<std::shared_ptr<artist::image>(const std::string&)> thumbnailFor;
        std::weak_ptr<Browser> self;
        std::function<void(const std::string&)> onPick; // pick only: what to do with the choice
        bool controlsTwoRows = false;                   // the Library's search over its filters, on a narrow window
        int pickedFrom = -1;                            // the Tab the choice being heard came from; -1 for none kept
    };

    auto state = std::make_shared<Browser>();
    auto* b = state.get();
    b->self = state;
    b->nodeId = nodeId;
    b->resourceType = resourceType;
    b->index = resourceIndex;
    b->role = RoleFor(mState, *node, resourceType, resourceIndex);
    b->onPick = std::move(onPick);
    b->thumbnails = std::make_unique<ResourceThumbnailFetcher>(mSession.Client(), mSession.Tones());
    // A library entry's tone picture: the cached file, else the download while it arrives.
    b->thumbnailFor = [this, b](const std::string& resourceId) -> std::shared_ptr<artist::image> {
        const auto* picture = b->thumbnails ? b->thumbnails->Find(b->resourceType, resourceId) : nullptr;

        if (picture == nullptr)
        {
            return nullptr;
        }

        return !picture->path.empty() ? CachedThumbnail(picture->path) : RemoteImage(picture->fetchUrl);
    };
    const auto slot = static_cast<std::size_t>(std::max(0, resourceIndex));

    if (!b->onPick && slot < node->resources.size())
    {
        b->original = node->resources[slot];
        b->hadOriginal = b->original.IsLibraryRef() || b->original.IsFilePath();
        b->selectedId = b->original.resourceId;
        b->selectedPath = guitarfx::util::PathToUtf8(b->original.filePath);
    }

    // What this slot's browser had last time; the first time, Tone3000 searches the slot's gear
    // (A2 captures for a model).
    const bool tone3000On = IsFeatureEnabled(mState, Feature::Tone3000);
    auto& memorySlot = mResourceBrowserMemory[resourceType + ":" + b->role.context + (b->onPick ? ":pick" : "")];

    if (!memorySlot)
    {
        memorySlot = std::make_shared<ResourceBrowserMemory>();
        memorySlot->t3Gear = b->role.gear;
        memorySlot->t3Arch = resourceType == "nam" ? "2" : "";
    }

    const auto memory = memorySlot;
    b->tab = static_cast<Tab>(std::clamp(memory->tab, 0, 2));
    b->tab = b->onPick || (b->tab == Tab::Tone3000 && !tone3000On) ? Tab::Library : b->tab;
    b->query = memory->query;
    b->arch = memory->arch;
    b->creator = memory->creator;
    b->tags = memory->tags;
    b->favouritesOnly = memory->favouritesOnly;
    b->openPacks = memory->openPacks;
    b->t3Query = memory->t3Query;
    b->t3Gear = memory->t3Gear;
    b->t3Sort = memory->t3Sort;
    b->t3Arch = memory->t3Arch;
    b->t3Page = memory->t3Page;
    b->t3Pages = memory->t3Pages;
    b->t3Tones = memory->t3Tones;
    b->t3Open = memory->t3Open;
    b->t3Models = memory->t3Models;

    // The library opens on the slot's category (the reverb IR on reverbs, Neural FX on pedals),
    // or on everything when the library has none; on the chosen entry's own when it is elsewhere.
    {
        const auto available = LibraryCategories(mState, resourceType);
        const auto hint = !category.empty()      ? category
                          : resourceType == "ir" ? std::string(b->role.gear == "space" ? "reverb" : "cab")
                                                 : b->role.gear;
        b->category = ResolveLibraryCategoryFromHint(hint, available).value_or(std::string());
        const auto* chosen = b->selectedId.empty() ? nullptr : mState.FindLibraryResource(resourceType, b->selectedId);
        const auto chosenCategory = chosen == nullptr          ? std::string()
                                    : chosen->category.empty() ? std::string("Uncategorized")
                                                               : chosen->category;

        if (chosen != nullptr && !b->category.empty() && b->category != chosenCategory)
        {
            const bool held = std::find(available.begin(), available.end(), chosenCategory) != available.end();
            b->category = held ? chosenCategory : std::string();
        }
    }

    // A model imported from Tone3000 opens on the tone it came from, its models listed.
    std::optional<t3::Tone> sourceTone;

    if (!b->onPick && tone3000On && !b->selectedId.empty())
    {
        if (const auto* chosen = mState.FindLibraryResource(resourceType, b->selectedId))
        {
            sourceTone = t3::SourceToneOf(*chosen);
            const auto arch = MetaText(chosen->metadata, "architectureVersion");

            if (sourceTone)
            {
                b->tab = Tab::Tone3000;
                b->t3Query.clear();
                b->t3Arch = resourceType == "nam" && (arch == "1" || arch == "2") ? arch : std::string();
                b->t3Page = b->t3Pages = 1;

                // Already open from last time: nothing to ask.
                if (b->t3Open == sourceTone->id && !b->t3Models.empty() && b->t3Tones.size() == 1 &&
                    b->t3Tones.front().id == sourceTone->id)
                {
                    sourceTone.reset();
                }
                else
                {
                    b->t3Tones = {*sourceTone};
                    b->t3Open = sourceTone->id;
                    b->t3Models.clear();
                    b->t3ModelsLoading = true;
                }
            }
        }
    }

    // Everything below runs on the UI thread; a refill asked for from inside a list waits for
    // the click that asked to finish (the row it replaces must outlive its own handler).
    const auto later = [this](std::weak_ptr<Browser> weak, auto member) {
        mSession.View().post([alive = std::weak_ptr<bool>(mAlive), weak, member] {
            if (const auto st = weak.lock(); st && alive.lock() && (st.get()->*member))
            {
                (st.get()->*member)();
            }
        });
    };
    const auto refillList = [later, weak = std::weak_ptr<Browser>(state)] { later(weak, &Browser::fillList); };

    auto& commands = mSession.Commands();
    const auto preview = [this, b](const std::string& id, const std::string& path) {
        // A choice from the library or a folder ends a Tone3000 audition, the slot taking it.
        if (!b->t3Hearing.empty())
        {
            mSession.Commands().CancelPreviewResource(b->nodeId, b->index, false);
            b->t3Hearing.clear();
            b->t3HearingTone.reset();
            b->t3HearingModel.reset();
        }

        b->previewed = true;
        b->selectedId = id;
        b->selectedPath = path;
        b->pickedFrom = static_cast<int>(b->tab);

        if (b->onPick)
        {
            b->fillFooter();
            return;
        }

        if (!id.empty())
        {
            mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, id, b->index);
        }
        else
        {
            mSession.Commands().SetNodeResourceFile(b->nodeId, b->resourceType, path, b->index);
        }

        b->fillFooter();
    };

    // ── Library ─────────────────────────────────────────────────────────────────
    // The web UI's Library tab (resourceBrowser.ts), from uiclient::ResourceIndex: each file
    // once, the filters, favourites first, and packs grouped when that is on.
    const auto libraryFilter = [b] {
        ResourceFilter filter;
        filter.query = b->query;
        filter.category = b->category.empty() ? std::string("all") : b->category;
        filter.architecture = b->arch.empty() ? std::string("all") : b->arch;
        filter.creator = b->creator.empty() ? std::string("all") : b->creator;
        filter.tags = b->tags;
        filter.favouritesOnly = b->favouritesOnly;
        return filter;
    };
    const auto libraryList = [this, b, preview, refillList, libraryFilter]() -> Ptr {
        const auto entries = FilterLibrary(mState, b->resourceType, libraryFilter(), b->selectedId);

        // Which tone pictures these show: asked once a session for each (the answer redraws).
        std::vector<std::string> ids;

        for (const auto* r : entries)
        {
            ids.push_back(r->id);
        }

        if (b->thumbnails)
        {
            b->thumbnails->Request(b->resourceType, ids);
        }

        const auto rowFor = [this, b, preview, refillList](const LibraryResource* r, const std::string& title) {
            // The category is left out while the list is filtered to one: every row would say it.
            auto row = std::make_shared<BrowserRow>(mTheme, mIcons, title,
                                                    ResourceMetaLine(*r, b->resourceType, b->category.empty()));
            row->selected = r->id == b->selectedId || (!b->selectedPath.empty() && r->filePath == b->selectedPath);

            if (row->selected)
            {
                b->selectedRow = row;
            }

            row->picture = [b, id = r->id] { return b->thumbnailFor ? b->thumbnailFor(id) : nullptr; };
            row->pictureSlot = true;
            row->star = IsResourceFavorite(mState, r->id) ? 1 : 0;
            row->onStar = [this, refillList, id = r->id] {
                mSession.Commands().SetResourceFavorite(id, !IsResourceFavorite(mState, id));
                refillList();
            };
            row->onMenu = [this, b, id = r->id, rowPtr = row.get()] {
                OpenLibraryResourceMenu(b->resourceType, id, rowPtr->bounds);
            };
            row->onClick = [preview, refillList, id = r->id] {
                preview(id, {});
                refillList();
            };
            row->onDouble = [this, b, preview, id = r->id] {
                preview(id, {});
                auto pick = b->onPick;
                CloseOverlay();

                if (pick)
                {
                    pick(id);
                }
            };

            // Edit (not a built-in one's), details, delete, and select (it plays in the chain).
            const auto provider = MetaText(r->metadata, "provider");
            const bool builtIn = provider == "built-in" || provider == "builtin" || provider == "factory";
            const auto type = b->resourceType;

            if (!builtIn)
            {
                row->actions.push_back(
                    {"ui-edit", [this, type, id = r->id](el::rect at) { EditLibraryResource(type, id, at); }});
            }

            row->actions.push_back(
                {"ui-info", [this, type, id = r->id](el::rect at) { OpenLibraryResourceMenu(type, id, at); }});
            row->actions.push_back(
                {"ui-trash", [this, type, id = r->id](el::rect at) { ConfirmDeleteLibraryResource(type, id, at); }});
            row->actions.push_back({"ui-play", [preview, refillList, id = r->id](el::rect) {
                                        preview(id, {});
                                        refillList();
                                    }});
            return row;
        };

        std::vector<Ptr> rows;

        if (IsPackGroupingEnabled(mState))
        {
            for (const auto& entry : GroupLibraryByPack(mState, entries))
            {
                if (!entry.pack)
                {
                    const auto* r = entry.items.front();
                    rows.push_back(rowFor(r, r->name.empty() ? r->id : r->name));
                    continue;
                }

                // A pack is open while the user leaves it open, and by default when it holds the
                // choice or the list is searched or narrowed to favourites.
                const auto key = entry.pack->key;
                const bool holdsSelected =
                    std::any_of(entry.items.begin(), entry.items.end(),
                                [b](const LibraryResource* r) { return r->id == b->selectedId; });
                const auto toggled = b->openPacks.find(key);
                const bool open = toggled != b->openPacks.end()
                                      ? toggled->second
                                      : (holdsSelected || !b->query.empty() || b->favouritesOnly);
                // Drawn as a Tone3000 result's tone is: picture, title over "by … · N models",
                // and the chevron on the right, so its picture lines up with every row's.
                std::vector<std::string> members, creators;

                for (const auto* r : entry.items)
                {
                    members.push_back(r->id);
                    creators.push_back(CreatorOf(r->metadata));
                }

                auto head = std::make_shared<BrowserRow>(mTheme, mIcons, entry.pack->title,
                                                         GroupMetaLine(creators, entry.items.size(), b->resourceType));
                head->note = open ? "\xE2\x96\xB4" : "\xE2\x96\xBE";

                // The first of its models with a tone picture lends it to the group's caption.
                head->picture = [b, members]() -> std::shared_ptr<artist::image> {
                    for (const auto& id : members)
                    {
                        if (auto image = b->thumbnailFor ? b->thumbnailFor(id) : nullptr)
                        {
                            return image;
                        }
                    }

                    return nullptr;
                };
                head->pictureSlot = true;
                head->onClick = [b, refillList, key, open] {
                    b->openPacks[key] = !open;
                    refillList();
                };
                rows.push_back(head);

                if (open)
                {
                    for (const auto* r : entry.items)
                    {
                        auto row = rowFor(r, NameWithinPack(r->name.empty() ? r->id : r->name, *entry.pack));
                        row->groupMember = true;
                        rows.push_back(row);
                    }
                }
            }
        }
        else
        {
            for (const auto* r : entries)
            {
                rows.push_back(rowFor(r, r->name.empty() ? r->id : r->name));
            }
        }

        if (rows.empty())
        {
            const bool narrowed = !b->query.empty() || !b->category.empty() || !b->arch.empty() ||
                                  !b->creator.empty() || !b->tags.empty() || b->favouritesOnly;
            auto none =
                std::make_shared<Text>(mTheme,
                                       !narrowed ? "The library has none yet."
                                                 : std::string("No ") + (b->resourceType == "ir" ? "IRs" : "models") +
                                                       " match the current filters.",
                                       12.0f);
            none->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
            rows.push_back(FixedHeight(60, none));
        }

        return Col(rows);
    };

    const auto libraryControls = [this, b, &commands, refillList, preview, libraryFilter]() -> Ptr {
        if (!b->searchBox)
        {
            auto [element, box] = el::input_box(b->resourceType == "ir" ? "Search IRs..." : "Search models...",
                                                Font(Weight::Regular), 1.0f);
            b->searchBox = box;
            b->searchElement = el::share(std::move(element));
            b->searchBox->set_text(b->query); // what this slot's browser was searching last
            b->searchBox->on_text = [b](std::string_view text) {
                b->query = std::string(text);

                if (b->fillList)
                {
                    b->fillList(); // the list is its own slot: the box keeps its focus
                }
            };
        }

        // A drop-down button whose choice refills the controls and the list.
        const auto chooser = [this, b, refillList](const std::string& label, float width,
                                                   std::vector<std::pair<std::string, std::string>> choices,
                                                   std::string* target) {
            auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            button->Size(width, 30).Label(label, 11.0f).Caret(true);
            button->onClick = [this, b, refillList, choices, target, c = button.get()] {
                std::vector<MenuItem> items;

                for (const auto& [text, value] : choices)
                {
                    items.push_back({text, *target == value, false, [b, refillList, target, value] {
                                         *target = value;
                                         b->fillControls();
                                         refillList();
                                     }});
                }

                OpenDropdown(c->Bounds(), std::move(items), 220.0f);
            };
            return button;
        };

        std::vector<std::pair<std::string, std::string>> categories{{"All categories", ""}};

        for (const auto& name : LibraryCategories(mState, b->resourceType))
        {
            categories.emplace_back(name, name);
        }

        // The filters, and what they take across with their gaps.
        std::vector<Ptr> row{
            chooser(b->category.empty() ? "All categories" : b->category, 140, categories, &b->category)};
        float filtersWidth = 140.0f;

        if (b->resourceType == "nam")
        {
            row.push_back(HGap(6));
            row.push_back(chooser(b->arch.empty() ? "A1 / A2" : b->arch, 84,
                                  {{"A1 and A2", ""}, {"A1", "A1"}, {"A2", "A2"}}, &b->arch));
            filtersWidth += 90.0f;
        }

        // Creators and tags: the facets of what the library holds (getResourceLibraryFacets).
        std::vector<const LibraryResource*> all;

        if (const auto it = mState.resources.find(b->resourceType); it != mState.resources.end())
        {
            for (const auto* r : DeduplicateResources(it->second).deduped)
            {
                all.push_back(r);
            }
        }

        const auto facets = LibraryFacets(all);

        if (!facets.creators.empty())
        {
            std::vector<std::pair<std::string, std::string>> creators{{"All creators", ""}};

            for (const auto& c : facets.creators)
            {
                creators.emplace_back(c, c);
            }

            row.push_back(HGap(6));
            row.push_back(chooser(b->creator.empty() ? "All creators" : b->creator, 120, creators, &b->creator));
            filtersWidth += 126.0f;
        }

        if (!facets.tags.empty())
        {
            auto tags = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            tags->Size(76, 30)
                .Label(b->tags.empty() ? std::string("Tags") : "Tags (" + std::to_string(b->tags.size()) + ")", 11.0f)
                .Caret(true);
            tags->onClick = [this, b, refillList, list = facets.tags, c = tags.get()] {
                std::vector<MenuItem> items{{"All tags", b->tags.empty(), false, [b, refillList] {
                                                 b->tags.clear();
                                                 b->fillControls();
                                                 refillList();
                                             }}};

                for (const auto& tag : list)
                {
                    const bool on = std::find(b->tags.begin(), b->tags.end(), tag) != b->tags.end();
                    items.push_back({tag, on, false, [b, refillList, tag, on] {
                                         if (on)
                                         {
                                             b->tags.erase(std::remove(b->tags.begin(), b->tags.end(), tag),
                                                           b->tags.end());
                                         }
                                         else
                                         {
                                             b->tags.push_back(tag);
                                         }

                                         b->fillControls();
                                         refillList();
                                     }});
                }

                OpenDropdown(c->Bounds(), std::move(items), 200.0f);
            };
            row.push_back(HGap(6));
            row.push_back(tags);
            filtersWidth += 82.0f;
        }

        // All | Favourites (.resource-browser-favorites-filter).
        auto favourites = std::make_shared<Segmented>(mTheme, std::vector<std::string>{"All", "Favourites"});
        favourites->Select(b->favouritesOnly ? 1 : 0);
        favourites->onSelect = [b, refillList](int index) {
            b->favouritesOnly = index == 1;
            b->fillControls();
            refillList();
        };
        const bool grouped = IsPackGroupingEnabled(mState);
        auto group = std::make_shared<IconButton>(mTheme, mIcons, "ui-box", IconButton::Look::Boxed);
        group->Size(32, 32).IconSize(15).Lit(grouped);
        group->onClick = [this, refillList, grouped] {
            mSession.Commands().SetSetting("resources.library.groupPacks", !grouped);
            refillList();
        };

        // Previous and next through the list as it is drawn, each heard in the chain.
        const auto step = [this, b, preview, refillList, libraryFilter](int delta) {
            const auto order =
                NavigationOrder(mState, FilterLibrary(mState, b->resourceType, libraryFilter(), b->selectedId));

            if (order.empty())
            {
                return;
            }

            const auto at = std::find_if(order.begin(), order.end(),
                                         [b](const LibraryResource* r) { return r->id == b->selectedId; });
            const int count = static_cast<int>(order.size());
            const int index = at == order.end()
                                  ? (delta > 0 ? 0 : count - 1)
                                  : std::clamp(static_cast<int>(at - order.begin()) + delta, 0, count - 1);
            preview(order[static_cast<std::size_t>(index)]->id, {});
            refillList();
        };
        auto previous = std::make_shared<IconButton>(mTheme, mIcons, "ui-prev", IconButton::Look::Boxed);
        previous->Size(32, 32).IconSize(14);
        previous->onClick = [step] { step(-1); };
        auto next = std::make_shared<IconButton>(mTheme, mIcons, "ui-next", IconButton::Look::Boxed);
        next->Size(32, 32).IconSize(14);
        next->onClick = [step] { step(1); };
        auto file = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        file->Size(64, 30).Label("File...", 11.0f);
        file->onClick = [this, b, &commands] {
            commands.BrowseNodeResource(b->nodeId, b->resourceType, b->index);
            b->previewed = true; // a file chosen there is the new choice; Cancel puts back the old
            b->fillFooter();     // which OK keeps
        };
        std::vector<Ptr> tools{favourites, HGap(6), Middle(group), HGap(6), Middle(previous), HGap(4), Middle(next)};
        // All | Favourites is about 130 wide; then the three square buttons and their gaps.
        float toolsWidth = 130.0f + 6.0f + 32.0f + 6.0f + 32.0f + 4.0f + 32.0f;

        if (!b->onPick)
        {
            tools.insert(tools.end(), {HGap(6), file});
            toolsWidth += 70.0f;
        }

        // The search keeps at least 220 px. Where that and everything else do not fit across the
        // dialog (800 wide squeezed it to "Searc"), the search and the tools take the first row
        // and the filters a second.
        const float inner = std::min(944.0f, ViewSize().x - 16.0f) - 32.0f;
        b->controlsTwoRows = 220.0f + 8.0f + filtersWidth + 6.0f + toolsWidth > inner;

        if (b->controlsTwoRows)
        {
            std::vector<Ptr> first{WidthRange(160.0f, el::full_extent, b->searchElement), HGap(8)};
            first.insert(first.end(), tools.begin(), tools.end());
            row.push_back(Stretch());
            return Col({FixedHeight(34, Row(first)), VGap(6), FixedHeight(30, Row(row))});
        }

        std::vector<Ptr> one{WidthRange(220.0f, el::full_extent, b->searchElement), HGap(8)};
        one.insert(one.end(), row.begin(), row.end());
        one.push_back(HGap(6));
        one.insert(one.end(), tools.begin(), tools.end());
        return Row(one);
    };

    // ── Folder ──────────────────────────────────────────────────────────────────
    const auto roots = [this]() {
        std::vector<std::tuple<std::string, std::string, std::string>> list; // id, label, path

        if (const auto it = mState.appSettings.find(kRootsSetting); it != mState.appSettings.end() && it->is_array())
        {
            for (const auto& r : *it)
            {
                if (r.is_object())
                {
                    list.emplace_back(r.value("id", ""), r.value("label", ""), r.value("path", ""));
                }
            }
        }

        return list;
    };
    const auto rememberLocation = [this, b](const std::string& path) {
        auto locations = mState.appSettings.value(kLastLocationsSetting, nlohmann::json::object());

        if (!locations.is_object())
        {
            locations = nlohmann::json::object();
        }

        locations[b->role.context] = {{"rootId", b->rootId}, {"path", path}};
        mSession.Commands().SetSetting(kLastLocationsSetting, locations);
    };
    const auto openFolder = [this, b, rememberLocation](const std::string& path) {
        mSession.Commands().ListResourceFolder(path);
        rememberLocation(path);
    };

    const auto folderList = [this, b, preview, refillList, openFolder]() -> Ptr {
        const auto& listing = mState.resourceFolder;
        std::vector<Ptr> rows;

        if (b->rootId.empty())
        {
            auto hint = std::make_shared<Text>(mTheme, "Add a folder of models or IRs to browse it here.", 12.0f);
            hint->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
            return FixedHeight(60, hint);
        }

        if (!listing.error.empty())
        {
            auto error = std::make_shared<Text>(mTheme, listing.error, 12.0f);
            error->Colour(mTheme.Error()).Alignment(Text::Align::Centre);
            return FixedHeight(60, error);
        }

        const auto filter = Lower(b->filter);
        std::vector<std::string> inLibrary; // the files' library ids, for their tone pictures

        for (const auto& dir : listing.dirs)
        {
            if (!filter.empty() && Lower(dir.name).find(filter) == std::string::npos)
            {
                continue;
            }

            auto row = std::make_shared<BrowserRow>(mTheme, mIcons, dir.name, "");
            row->icon = "ui-folder";
            row->onClick = [openFolder, path = dir.path] { openFolder(path); };
            rows.push_back(row);
        }

        // Files known to come from the same tone (a lookup's, or their library entries') are one
        // group, as the web UI's folderGroups.ts draws them.
        auto& lookup = mSession.FolderLookup();
        std::vector<const ResourceFolderListing::File*> shown;
        b->listedFiles.clear();

        for (const auto& file : listing.files)
        {
            if (file.resourceType != b->resourceType ||
                (!filter.empty() && Lower(file.name).find(filter) == std::string::npos))
            {
                continue;
            }

            shown.push_back(&file);
            b->listedFiles.push_back(file);
        }

        const auto https = [](const std::string& url) { return url.rfind("https://", 0) == 0 ? url : std::string(); };
        const auto libraryIdOf = [b](const ResourceFolderListing::File& file) {
            return file.inLibrary && file.resourceType == b->resourceType ? file.libraryId : std::string();
        };

        const auto fileRow = [&](const ResourceFolderListing::File& file, bool member) {
            auto row = std::make_shared<BrowserRow>(mTheme, mIcons, file.name, Describe(file.metadata, ""));
            row->selected = !b->selectedPath.empty() && b->selectedPath == file.path;
            row->groupMember = member;

            if (file.inLibrary)
            {
                row->note = "In library";
                row->noteColour = mTheme.Success();
            }

            // A file in the library shows its entry's tone picture, as the Library tab does; else
            // the picture of the tone a lookup matched it to, or its own metadata names (https only),
            // as folderRows.ts does.
            const auto* details = lookup.DetailsFor(file);
            auto remotePicture = details != nullptr ? https(MetaText(*details, "imageUrl")) : std::string();
            remotePicture = remotePicture.empty() ? https(MetaText(file.metadata, "imageUrl")) : remotePicture;
            const auto libraryId = libraryIdOf(file);
            row->pictureSlot = true;

            if (!libraryId.empty())
            {
                inLibrary.push_back(libraryId);
            }

            if (!libraryId.empty() || !remotePicture.empty())
            {
                row->picture = [this, b, libraryId, remotePicture]() -> std::shared_ptr<artist::image> {
                    auto image = !libraryId.empty() && b->thumbnailFor ? b->thumbnailFor(libraryId) : nullptr;
                    return image || remotePicture.empty() ? image : RemoteImage(remotePicture);
                };
            }

            row->onClick = [preview, refillList, path = file.path] {
                preview({}, path);
                refillList();
            };
            row->onDouble = [this, preview, path = file.path] {
                preview({}, path);
                CloseOverlay();
            };
            return row;
        };

        const auto grouped =
            GroupFolderFiles(shown, [&lookup](const ResourceFolderListing::File& f) { return lookup.DetailsFor(f); });

        for (const auto& item : grouped.items)
        {
            if (item.file != nullptr)
            {
                rows.push_back(fileRow(*item.file, false));
                continue;
            }

            const auto& group = grouped.groups[item.group];
            const bool holdsSelected =
                !b->selectedPath.empty() &&
                std::any_of(group.files.begin(), group.files.end(),
                            [b](const ResourceFolderListing::File* f) { return f->path == b->selectedPath; });
            const auto toggled = b->openPacks.find(group.key);
            const bool open = toggled != b->openPacks.end() ? toggled->second : (holdsSelected || !b->filter.empty());

            // The tone's title, else what its models' library names share, else its gear, else the
            // first file's name; "gear · by creator · N models"; a member's picture, else the tone's.
            std::vector<std::string> names, memberIds;

            for (const auto* f : group.files)
            {
                const auto id = libraryIdOf(*f);
                const auto* entry = id.empty() ? nullptr : mState.FindLibraryResource(f->resourceType, id);
                names.push_back(entry != nullptr ? entry->name : std::string());

                if (!id.empty())
                {
                    memberIds.push_back(id);
                }
            }

            auto title = group.title.empty() ? SharedTonePrefix(names) : group.title;
            const auto gearMake = MetaText(group.metadata, "gearMake");
            const auto gearModel = MetaText(group.metadata, "gearModel");
            const auto gear = !gearModel.empty() && Lower(gearModel).rfind(Lower(gearMake), 0) == 0
                                  ? gearModel
                                  : gearMake + (gearMake.empty() || gearModel.empty() ? "" : " ") + gearModel;
            title = title.empty() ? gear : title;
            title = title.empty() ? group.files.front()->name : title;

            auto category = MetaText(group.metadata, "tone3000Category");
            category = category.empty() ? MetaText(group.metadata, "gear") : category;
            const auto line = GroupMetaLine({FolderGroupCreator(group.metadata)}, group.files.size(), b->resourceType);
            auto head = std::make_shared<BrowserRow>(mTheme, mIcons, title,
                                                     category.empty() ? line : category + " \xC2\xB7 " + line);
            head->note = open ? "\xE2\x96\xB4" : "\xE2\x96\xBE";
            head->pictureSlot = true;
            head->picture = [this, b, memberIds,
                             tonePicture =
                                 https(MetaText(group.metadata, "imageUrl"))]() -> std::shared_ptr<artist::image> {
                for (const auto& id : memberIds)
                {
                    if (auto image = b->thumbnailFor ? b->thumbnailFor(id) : nullptr)
                    {
                        return image;
                    }
                }

                return tonePicture.empty() ? nullptr : RemoteImage(tonePicture);
            };
            head->onClick = [b, refillList, key = group.key, open] {
                b->openPacks[key] = !open;
                refillList();
            };
            rows.push_back(head);

            if (open)
            {
                for (const auto* f : group.files)
                {
                    rows.push_back(fileRow(*f, true));
                }
            }
        }

        if (b->thumbnails)
        {
            b->thumbnails->Request(b->resourceType, inLibrary);
        }

        if (b->updateLookupButton)
        {
            b->updateLookupButton();
        }

        if (rows.empty())
        {
            auto none =
                std::make_shared<Text>(mTheme, listing.path.empty() ? "Loading..." : "No models or IRs here.", 12.0f);
            none->Colour(mTheme.TextMuted()).Alignment(Text::Align::Centre);
            rows.push_back(FixedHeight(60, none));
        }

        return Col(rows);
    };

    const auto folderControls = [this, b, roots, openFolder, refillList]() -> Ptr {
        const auto list = roots();
        std::string label = "Choose a folder";

        for (const auto& [id, name, path] : list)
        {
            if (id == b->rootId)
            {
                label = name.empty() ? path : name;
            }
        }

        auto rootButton = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        rootButton->Size(140, 30).Label(label, 11.0f).Caret(true).Disabled(list.empty());
        rootButton->onClick = [this, b, list, openFolder, r = rootButton.get()] {
            std::vector<MenuItem> items;

            for (const auto& [id, name, path] : list)
            {
                items.push_back(
                    {name.empty() ? path : name, id == b->rootId, false, [this, b, id = id, path = path, openFolder] {
                         b->rootId = id;
                         mSession.Commands().SetSetting(kActiveRootSetting, id);
                         b->fillControls();
                         openFolder(path);
                     }});
            }

            OpenDropdown(r->Bounds(), std::move(items), 220.0f);
        };
        auto remove = std::make_shared<IconButton>(mTheme, mIcons, "ui-trash", IconButton::Look::Boxed);
        remove->Size(30, 30).IconSize(14).Disabled(b->rootId.empty());
        remove->onClick = [this, b] {
            auto rootsJson = mState.appSettings.value(kRootsSetting, nlohmann::json::array());
            nlohmann::json kept = nlohmann::json::array();

            for (const auto& r : rootsJson)
            {
                if (r.is_object() && r.value("id", "") != b->rootId)
                {
                    kept.push_back(r);
                }
            }

            mSession.Commands().SetSetting(kRootsSetting, kept);
            b->rootId.clear();
            // Refilled once this click is over: the button is in what the refill replaces.
            mSession.View().post([weak = b->self] {
                if (const auto st = weak.lock(); st && st->fillControls)
                {
                    st->fillControls();
                    st->fillList();
                }
            });
        };
        auto add = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        add->Size(100, 30).Label("Add Folder...", 11.0f);
        add->onClick = [this] { mSession.Commands().BrowseResourceFolder(); };

        if (!b->filterBox)
        {
            auto [element, box] = el::input_box("Filter files...", Font(Weight::Regular), 1.0f);
            b->filterBox = box;
            b->filterElement = el::share(std::move(element));
            b->filterBox->on_text = [b](std::string_view text) {
                b->filter = std::string(text);

                if (b->fillList)
                {
                    b->fillList();
                }
            };
        }

        // Where the listing is, with the way up.
        const auto& listing = mState.resourceFolder;
        auto up = std::make_shared<IconButton>(mTheme, mIcons, "ui-up", IconButton::Look::Boxed);
        up->Size(30, 30).IconSize(14).Disabled(listing.parent.empty() || b->rootId.empty());
        up->onClick = [openFolder, parent = listing.parent] { openFolder(parent); };
        auto where =
            std::make_shared<Text>(mTheme, b->rootId.empty() ? std::string() : listing.path, 11.0f, Weight::Medium);
        where->Colour(mTheme.TextSecondary());
        std::size_t files = 0;

        for (const auto& f : listing.files)
        {
            files += f.resourceType == b->resourceType ? 1 : 0;
        }

        auto counts =
            std::make_shared<Text>(mTheme,
                                   b->rootId.empty() ? std::string()
                                                     : std::to_string(listing.dirs.size()) +
                                                           (listing.dirs.size() == 1 ? " folder  " : " folders  ") +
                                                           std::to_string(files) + (files == 1 ? " file" : " files"),
                                   10.5f);
        counts->Colour(mTheme.TextMuted()).Fixed(true);
        std::vector<Ptr> second{up, HGap(10), where, HGap(10), counts};

        // "Look up details" goes to Tone3000 (through Soundshed's relay), so only with that on.
        b->lookupButton.reset();

        if (IsFeatureEnabled(mState, Feature::Tone3000) && !b->rootId.empty())
        {
            auto lookUp = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            lookUp->Size(150, 30).Label("Look up details", 11.0f);
            lookUp->onClick = [this, b, refillList, alive = std::weak_ptr<bool>(mAlive)] {
                auto& folderLookup = mSession.FolderLookup();

                if (folderLookup.Running())
                {
                    return;
                }

                // Each file done regroups the list (a match joins its tone's group); the label counts.
                const auto changed = [weak = b->self, refillList, alive] {
                    if (const auto st = weak.lock(); st && alive.lock())
                    {
                        refillList();

                        if (st->updateLookupButton)
                        {
                            st->updateLookupButton();
                        }
                    }
                };
                folderLookup.Start(
                    b->listedFiles, changed, [this, changed, alive](const FolderToneLookup::Summary& summary) {
                        if (!alive.lock())
                        {
                            return;
                        }

                        changed();

                        if (!summary.error.empty())
                        {
                            ShowToast("Lookup failed", summary.error, true);
                            return;
                        }

                        if (summary.looked == 0)
                        {
                            return;
                        }

                        const auto more =
                            summary.remaining > 0
                                ? " Look up again for the next " +
                                      std::to_string(std::min(summary.remaining, FolderToneLookup::kBatch)) + "."
                                : std::string();
                        ShowToast(summary.found > 0 ? "Details found" : "No details found",
                                  std::to_string(summary.found) + " of " + std::to_string(summary.looked) +
                                      (summary.looked == 1 ? " file" : " files") + " matched a published tone." + more,
                                  false);
                    });
            };
            b->lookupButton = lookUp;
            second.push_back(HGap(10));
            second.push_back(lookUp);
        }

        return Col({FixedHeight(30, Row({rootButton, HGap(6), remove, HGap(6), add, HGap(8), b->filterElement})),
                    VGap(8), FixedHeight(30, Row(second))});
    };
    // The button's label and state: what it would do, or how far a lookup has got.
    b->updateLookupButton = [this, b] {
        if (!b->lookupButton)
        {
            return;
        }

        const auto& folderLookup = mSession.FolderLookup();
        const auto pending = std::count_if(
            b->listedFiles.begin(), b->listedFiles.end(),
            [&folderLookup](const ResourceFolderListing::File& f) { return folderLookup.NeedsLookup(f); });

        if (folderLookup.Running())
        {
            const auto total = folderLookup.Total();
            const auto at = std::min(folderLookup.Done() + 1, total);
            b->lookupButton->Label("Looking up " + std::to_string(at) + " of " + std::to_string(total) + "...", 11.0f)
                .Disabled(true);
        }
        else
        {
            b->lookupButton->Label("Look up details", 11.0f).Disabled(pending == 0);
        }

        mSession.View().refresh();
    };

    // ── Tone 3000 ───────────────────────────────────────────────────────────────
    const auto search = [this, b, refillList](int page) {
        b->t3Loading = true;
        b->t3Error.clear();
        b->t3Page = std::max(1, page);
        b->t3Open.clear();
        b->fillList();
        mSession.Tones().SearchTone3000(b->t3Query, b->t3Gear, b->t3Sort,
                                        b->resourceType == "nam" ? b->t3Arch : std::string(), b->t3Page,
                                        [b, refillList](const t3::Page<t3::Tone>& result, const std::string& error) {
                                            b->t3Loading = false;
                                            b->t3Error = error;
                                            b->t3Tones = result.items;
                                            b->t3Pages = result.totalPages;

                                            if (b->fillList)
                                            {
                                                b->fillList();
                                            }

                                            if (b->fillFooter)
                                            {
                                                b->fillFooter();
                                            }
                                        });
    };

    b->searchTone3000 = search;

    const auto tone3000List = [this, b, preview, refillList]() -> Ptr {
        std::vector<Ptr> rows;
        const auto message = [this](const std::string& text, el::color colour) {
            auto t = std::make_shared<Text>(mTheme, text, 12.0f);
            t->Colour(colour).Alignment(Text::Align::Centre);
            return FixedHeight(60, t);
        };

        if (b->t3Loading)
        {
            return message("Searching Tone3000...", mTheme.TextMuted());
        }

        if (!b->t3Error.empty())
        {
            return message(b->t3Error, mTheme.Error());
        }

        if (b->t3Tones.empty())
        {
            return message("Search Tone3000 for models to add to your library.", mTheme.TextMuted());
        }

        for (const auto& tone : b->t3Tones)
        {
            const int count = b->resourceType == "ir" ? tone.irsCount : tone.modelsCount;
            std::string detail = (tone.creatorName.empty() ? std::string() : "by " + tone.creatorName + " \xC2\xB7 ") +
                                 std::to_string(count) + (b->resourceType == "ir" ? " IRs" : " models") +
                                 (tone.platform.empty() ? "" : " \xC2\xB7 " + tone.platform);
            auto row = std::make_shared<BrowserRow>(mTheme, mIcons, tone.title, detail);
            row->picture = [this, url = tone.imageUrl] { return RemoteImage(url); };
            row->pictureSlot = true; // .resource-browser-tone-image, or its placeholder
            row->note = b->t3Open == tone.id ? "\xE2\x96\xB4" : "\xE2\x96\xBE";
            row->onClick = [this, b, tone, refillList] {
                if (b->t3Open == tone.id)
                {
                    b->t3Open.clear();
                    refillList();
                    return;
                }

                b->t3Open = tone.id;
                b->t3Models.clear();
                b->t3ModelsLoading = true;
                refillList();
                mSession.Tones().Tone3000Models(
                    tone.id, [b, id = tone.id](const std::vector<t3::Model>& models, const std::string&) {
                        if (b->t3Open != id)
                        {
                            return;
                        }

                        b->t3Models = models;
                        b->t3ModelsLoading = false;

                        if (b->fillList)
                        {
                            b->fillList();
                        }
                    });
            };
            rows.push_back(row);

            // The web UI's Import All: every model of the open tone into the library.
            if (b->t3Open == tone.id && !b->t3Models.empty())
            {
                const auto missing =
                    std::count_if(b->t3Models.begin(), b->t3Models.end(), [this, b](const t3::Model& m) {
                        return t3::FindImported(mState, b->resourceType, m) == nullptr;
                    });

                if (missing > 0)
                {
                    auto all = std::make_shared<BrowserRow>(
                        mTheme, mIcons, "Import all " + std::to_string(missing) + " into the library", "");
                    all->indent = true;
                    all->icon = "ui-download";
                    all->onClick = [this, b, tone, refillList] {
                        for (const auto& m : b->t3Models)
                        {
                            if (t3::FindImported(mState, b->resourceType, m) == nullptr &&
                                !mSession.Tones().IsImporting(m.id))
                            {
                                mSession.Tones().ImportTone3000(
                                    tone, m, b->resourceType, b->resourceType == "ir" ? b->role.gear : std::string(),
                                    [this, refillList](const std::string&, const std::string& error) {
                                        if (!error.empty())
                                        {
                                            ShowToast("Could not import a model", error, true);
                                        }

                                        refillList();
                                    });
                            }
                        }

                        refillList();
                    };
                    rows.push_back(all);
                }
            }

            if (b->t3Open != tone.id)
            {
                continue;
            }

            if (b->t3ModelsLoading)
            {
                auto loading = std::make_shared<BrowserRow>(mTheme, mIcons, "Loading models...", "");
                loading->indent = true;
                rows.push_back(loading);
                continue;
            }

            for (const auto& model : b->t3Models)
            {
                auto modelRow = std::make_shared<BrowserRow>(
                    mTheme, mIcons, model.name, model.architecture.empty() ? std::string() : "A" + model.architecture);
                modelRow->indent = true;
                const auto* imported = t3::FindImported(mState, b->resourceType, model);
                modelRow->selected = imported != nullptr && imported->id == b->selectedId;

                if (mSession.Tones().IsImporting(model.id))
                {
                    modelRow->note = "Importing...";
                }
                else if (imported != nullptr)
                {
                    modelRow->note = "In library";
                    modelRow->noteColour = mTheme.Success();
                    modelRow->button = "Select";
                    modelRow->onButton = [preview, refillList, id = imported->id] {
                        preview(id, {});
                        refillList();
                    };
                }
                else
                {
                    // A click plays it on the slot without importing it (the web UI's preview);
                    // Select (or OK while it plays) imports it and keeps it.
                    modelRow->selected = b->t3Hearing == model.id;

                    if (b->t3Hearing == model.id)
                    {
                        modelRow->note = "Playing";
                    }

                    modelRow->onClick = [this, b, tone, model, refillList] {
                        b->t3Hearing = model.id;
                        b->t3HearingTone = tone;
                        b->t3HearingModel = model;
                        b->previewed = true;
                        b->pickedFrom = static_cast<int>(Tab::Tone3000);
                        b->selectedId.clear();
                        b->selectedPath.clear();
                        refillList();

                        if (b->fillFooter)
                        {
                            b->fillFooter();
                        }

                        mSession.Tones().DownloadTone3000(
                            model, b->resourceType,
                            [this, b, id = model.id](const std::string& base64, const std::string& error) {
                                if (b->t3Hearing != id)
                                {
                                    return;
                                }

                                if (!error.empty())
                                {
                                    ShowToast("Could not play the model", error, true);
                                    return;
                                }

                                mSession.Commands().PreviewRemoteResource(b->resourceType, "tone3000-preview:" + id,
                                                                          b->nodeId, b->index, false, base64);
                            });
                    };
                    modelRow->button = "Select";
                    modelRow->onButton = [this, b, tone, model, preview, refillList] {
                        mSession.Tones().ImportTone3000(
                            tone, model, b->resourceType, b->resourceType == "ir" ? b->role.gear : std::string(),
                            [this, preview, refillList](const std::string& id, const std::string& error) {
                                if (!error.empty())
                                {
                                    ShowToast("Could not import the model", error, true);
                                    refillList();
                                    return;
                                }

                                preview(id, {});
                                refillList();
                            });
                        refillList();
                    };
                }

                rows.push_back(modelRow);
            }
        }

        return Col(rows);
    };

    const auto tone3000Controls = [this, b, search]() -> Ptr {
        if (!b->t3Box)
        {
            auto [element, box] =
                el::input_box(b->resourceType == "ir" ? "Search cabinets and rooms..." : "Search amps and pedals...",
                              Font(Weight::Regular), 1.0f);
            b->t3Box = box;
            b->t3Element = el::share(std::move(element));
            b->t3Box->set_text(b->t3Query);
            b->t3Box->on_text = [b](std::string_view text) { b->t3Query = std::string(text); };
            b->t3Box->on_enter = [b, search](std::string_view text) {
                b->t3Query = std::string(text);
                search(1);
                return true;
            };
        }

        auto go = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        go->Size(70, 30).Label("Search", 11.0f);
        go->onClick = [search] { search(1); };

        const auto choice = [this, b, search](const std::string& label,
                                              std::vector<std::pair<std::string, std::string>> options,
                                              std::string Browser::* field, float width) {
            auto button = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
            button->Size(width, 30).Label(label, 11.0f).Caret(true);
            button->onClick = [this, b, options, field, search, width, me = button.get()] {
                std::vector<MenuItem> items;

                for (const auto& [value, text] : options)
                {
                    items.push_back({text, b->*field == value, false, [b, field, value = value, search] {
                                         b->*field = value;
                                         b->fillControls();
                                         search(1);
                                     }});
                }

                OpenDropdown(me->Bounds(), std::move(items), std::max(150.0f, width));
            };
            return button;
        };
        const auto labelOf = [](const std::vector<std::pair<std::string, std::string>>& options,
                                const std::string& value) {
            for (const auto& [v, text] : options)
            {
                if (v == value)
                {
                    return text;
                }
            }

            return options.empty() ? std::string() : options.front().second;
        };
        const std::vector<std::pair<std::string, std::string>> sorts{{"downloads-all-time", "Popular"},
                                                                     {"newest", "Recent"},
                                                                     {"trending", "Trending"},
                                                                     {"best-match", "Best match"}};
        const std::vector<std::pair<std::string, std::string>> architectures{
            {"", "Any Architecture"}, {"1", "A1"}, {"2", "A2"}};

        std::vector<Ptr> row{b->t3Element,
                             HGap(8),
                             go,
                             HGap(8),
                             choice(labelOf(b->role.gears, b->t3Gear), b->role.gears, &Browser::t3Gear, 110),
                             HGap(6),
                             choice(labelOf(sorts, b->t3Sort), sorts, &Browser::t3Sort, 96)};

        if (b->resourceType == "nam")
        {
            row.push_back(HGap(6));
            row.push_back(choice(labelOf(architectures, b->t3Arch), architectures, &Browser::t3Arch, 132));
        }

        return Row(row);
    };

    // ── The dialog ──────────────────────────────────────────────────────────────
    b->fillTabs = [this, b] {
        auto tab = [this, b](const char* label, Tab which) {
            auto t = std::make_shared<TextTab>(mTheme, label, b->tab == which, 13.0f, 36.0f, 38.0f);
            t->onClick = [this, b, which] {
                if (b->tab == which)
                {
                    return;
                }

                b->tab = which;

                // Tone 3000 opens on its popular tones for the slot's gear, as the web UI's does.
                if (which == Tab::Tone3000 && b->t3Tones.empty() && !b->t3Loading && b->searchTone3000)
                {
                    b->searchTone3000(1);
                }

                mSession.View().post([weak = b->self] {
                    if (const auto st = weak.lock())
                    {
                        st->fillTabs();
                        st->fillControls();
                        st->fillList();
                        st->fillFooter();
                    }
                });
            };
            return t;
        };
        // Tone 3000 can be switched off in Settings > Features, as in the web UI.
        std::vector<Ptr> tabs{HGap(4), tab("Resource Library", Tab::Library)};

        if (!b->onPick)
        {
            tabs.push_back(tab("Folder", Tab::Folder));
        }

        if (!b->onPick && guitarfx::uiclient::IsFeatureEnabled(mState, guitarfx::uiclient::Feature::Tone3000))
        {
            tabs.push_back(tab("Tone 3000", Tab::Tone3000));
        }

        tabs.push_back(Stretch());
        Fill(*b->tabs, Row(tabs));
    };
    b->fillControls = [this, b, libraryControls, folderControls, tone3000Controls] {
        auto content = b->tab == Tab::Library  ? libraryControls()
                       : b->tab == Tab::Folder ? folderControls()
                                               : tone3000Controls();
        const float library = b->controlsTwoRows ? 34.0f + 6.0f + 30.0f : 34.0f;
        const float height = b->tab == Tab::Folder ? 68.0f : b->tab == Tab::Library ? library : 30.0f;
        Fill(*b->controls, FixedHeight(height, content));
        mSession.View().layout();
        mSession.View().refresh();
    };
    b->fillList = [this, b, libraryList, folderList, tone3000List] {
        Fill(*b->list, VScroll(Pad(0, 0, 10, 0,
                                   b->tab == Tab::Library  ? libraryList()
                                   : b->tab == Tab::Folder ? folderList()
                                                           : tone3000List())));
        mSession.View().layout();
        mSession.View().refresh();
    };
    b->fillFooter = [this, b, search] {
        std::vector<Ptr> row;

        // Tone 3000's pages go under its list (.resource-browser-pagination), the hint staying.
        if (b->tab == Tab::Tone3000 && !b->t3Tones.empty())
        {
            auto prev = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
            prev->Size(60, 30).Label("Prev", 11.0f).Disabled(b->t3Page <= 1);
            prev->onClick = [b, search] { search(b->t3Page - 1); };
            auto next = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Ghost);
            next->Size(60, 30).Label("Next", 11.0f).Disabled(b->t3Page >= b->t3Pages);
            next->onClick = [b, search] { search(b->t3Page + 1); };
            auto page = std::make_shared<Text>(
                mTheme, "Page " + std::to_string(b->t3Page) + " of " + std::to_string(b->t3Pages), 11.0f);
            page->Colour(mTheme.TextSecondary()).Fixed(true);
            Fill(*b->pager,
                 FixedHeight(40, Pad(0, 8, 0, 0, Row({Stretch(), prev, HGap(6), page, HGap(6), next, Stretch()}))));
        }
        else
        {
            Fill(*b->pager, FixedHeight(0, std::make_shared<el::element>()));
        }

        // What a click and a double click do (.resource-browser-footer-hint).
        auto hint = std::make_shared<Text>(mTheme,
                                           b->onPick ? "Double click an item to choose it."
                                                     : "Select an item to preview it in the current signal chain, "
                                                       "double click to select and return to the effect.",
                                           11.5f);
        hint->Colour(mTheme.TextSecondary());
        row = {Middle(hint), HGap(12)};

        // Cancel is the secondary button, OK the primary (.btn-secondary, .btn-primary).
        auto cancel = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Boxed);
        cancel->Size(64, 30).Label("Cancel", 11.5f);
        cancel->onClick = [this, b] {
            // A Tone3000 audition ends and the engine puts back what the slot held.
            if (!b->t3Hearing.empty())
            {
                mSession.Commands().CancelPreviewResource(b->nodeId, b->index, true);
                b->t3Hearing.clear();
                b->previewed = false;
            }

            // Put back what the slot held before anything was heard.
            if (b->previewed && !b->onPick)
            {
                if (b->hadOriginal && b->original.IsLibraryRef())
                {
                    mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, b->original.resourceId, b->index);
                }
                else if (b->hadOriginal && b->original.IsFilePath())
                {
                    mSession.Commands().SetNodeResourceFile(b->nodeId, b->resourceType,
                                                            guitarfx::util::PathToUtf8(b->original.filePath), b->index);
                }
                else
                {
                    mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, {}, b->index);
                }
            }

            b->previewed = false;
            b->pickedFrom = -1;
            CloseOverlay();
        };
        auto ok = std::make_shared<IconButton>(mTheme, mIcons, "", IconButton::Look::Primary);
        ok->Size(44, 30).Label("OK", 11.5f).Disabled(!b->previewed);
        ok->onClick = [this, b] {
            if (b->onPick)
            {
                auto pick = b->onPick;
                const auto id = b->selectedId;
                CloseOverlay();

                if (!id.empty())
                {
                    pick(id);
                }

                return;
            }

            // A Tone3000 model being auditioned is imported and kept.
            if (!b->t3Hearing.empty() && b->t3HearingTone && b->t3HearingModel)
            {
                const auto nodeId = b->nodeId, resourceType = b->resourceType;
                const int index = b->index;
                mSession.Commands().CancelPreviewResource(nodeId, index, false);
                mSession.Tones().ImportTone3000(
                    *b->t3HearingTone, *b->t3HearingModel, resourceType,
                    resourceType == "ir" ? b->role.gear : std::string(),
                    [this, nodeId, resourceType, index](const std::string& id, const std::string& error) {
                        if (!error.empty())
                        {
                            ShowToast("Could not import the model", error, true);
                        }
                        else
                        {
                            mSession.Commands().SetNodeResource(nodeId, resourceType, id, index);
                        }
                    });
                b->t3Hearing.clear();
                b->previewed = false;
                CloseOverlay();
                return;
            }

            // A file heard from a folder joins the library (in place), or picks the entry it is.
            if (b->previewed && b->selectedId.empty() && !b->selectedPath.empty())
            {
                std::string libraryId;
                std::string name = b->selectedPath;

                for (const auto& f : mState.resourceFolder.files)
                {
                    if (f.path == b->selectedPath)
                    {
                        libraryId = f.inLibrary ? f.libraryId : std::string();
                        name = f.name;
                    }
                }

                if (!libraryId.empty())
                {
                    mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, libraryId, b->index);
                }
                else
                {
                    const auto dot = name.rfind('.');
                    mSession.Commands().SaveLocalLibraryResource(b->resourceType, b->selectedPath,
                                                                 dot == std::string::npos ? name : name.substr(0, dot),
                                                                 {{"sourceFolder", mState.resourceFolder.path},
                                                                  {"sourceFileName", name},
                                                                  {"origin", "folder-browser"}},
                                                                 b->nodeId, b->index);
                }
            }

            b->previewed = false;
            CloseOverlay();
        };
        row.push_back(cancel);
        row.push_back(HGap(8));
        row.push_back(ok);
        Fill(*b->footer, Row(row));
        mSession.View().layout();
        mSession.View().refresh();
    };

    // The folder to show first: where this kind of slot was browsed last, else the active root.
    {
        const auto list = roots();
        const auto active = mState.appSettings.value(kActiveRootSetting, std::string());
        std::string path;

        if (const auto locations = mState.appSettings.find(kLastLocationsSetting);
            locations != mState.appSettings.end() && locations->is_object() && locations->contains(b->role.context))
        {
            const auto& last = (*locations)[b->role.context];
            b->rootId = last.value("rootId", std::string());
            path = last.value("path", std::string());
        }

        if (b->rootId.empty())
        {
            b->rootId = active;
        }

        bool known = false;

        for (const auto& [id, name, rootPath] : list)
        {
            if (id == b->rootId)
            {
                known = true;

                if (path.empty())
                {
                    path = rootPath;
                }
            }
        }

        if (!known && !list.empty())
        {
            b->rootId = std::get<0>(list.front());
            path = std::get<2>(list.front());
        }
        else if (!known)
        {
            b->rootId.clear();
        }

        if (!path.empty())
        {
            mSession.Commands().ListResourceFolder(path);
        }
    }

    // The engine's answers: a folder listed or picked, the library grown (an import done).
    b->subscriptions.push_back(mSession.Client().Subscribe(Topic::ResourceFolders, [this, b, openFolder] {
        if (!mState.pickedResourceFolder.empty())
        {
            // A folder just added: kept as a root, shared with the web UI, and opened.
            const auto path = mState.pickedResourceFolder;
            mSession.Client().MutableState().pickedResourceFolder.clear();
            auto rootsJson = mState.appSettings.value(kRootsSetting, nlohmann::json::array());

            if (!rootsJson.is_array())
            {
                rootsJson = nlohmann::json::array();
            }

            std::string id;

            for (const auto& r : rootsJson)
            {
                if (r.is_object() && r.value("path", "") == path)
                {
                    id = r.value("id", "");
                }
            }

            if (id.empty())
            {
                id = "root-" + std::to_string(std::hash<std::string>{}(path) % 1000000007ull);
                const auto slash = path.find_last_of("/\\");
                rootsJson.push_back({{"id", id},
                                     {"label", slash == std::string::npos ? path : path.substr(slash + 1)},
                                     {"path", path}});
                mSession.Commands().SetSetting(kRootsSetting, rootsJson);
            }

            b->rootId = id;
            mSession.Commands().SetSetting(kActiveRootSetting, id);
            b->tab = Tab::Folder;
            b->fillTabs();
            openFolder(path);
        }

        if (b->tab == Tab::Folder)
        {
            b->fillControls();
            b->fillList();
        }
    }));
    b->subscriptions.push_back(mSession.Client().Subscribe(Topic::Resources, [b] {
        if (b->tab != Tab::Folder && b->fillList)
        {
            b->fillList();
        }
    }));
    // The engine saying which tone pictures the Library and Folder rows show: what it lacks is
    // fetched (and handed back to it to keep), and the rows redraw.
    b->subscriptions.push_back(mSession.Client().Subscribe(Topic::Thumbnails, [this, b] {
        if (b->thumbnails)
        {
            b->thumbnails->Update();
        }

        mSession.View().refresh();
    }));
    // A tone's picture arriving (a Tone3000 result's, or a library row's thumbnail), or an import
    // moving on.
    b->toneListener = mSession.Tones().AddListener([this, b] {
        if (b->thumbnails)
        {
            b->thumbnails->Update();
        }

        mSession.View().refresh();
    });

    b->fillTabs();
    b->fillControls();
    b->fillList();
    b->fillFooter();

    // The chosen Library row brought into view once the dialog has its size.
    const auto showSelected = [this](std::weak_ptr<Browser> weak) {
        mSession.View().post([this, alive = std::weak_ptr<bool>(mAlive), weak] {
            const auto st = weak.lock();
            const auto row = st && alive.lock() && st->tab == Tab::Library ? st->selectedRow.lock() : nullptr;

            if (row)
            {
                mSession.View().layout();
                mSession.View().in_context_do(
                    *row, [](const el::context& ctx) { el::scrollable::find(ctx).scroll_into_view(ctx.bounds); });
                mSession.View().refresh();
            }
        });
    };

    if (sourceTone)
    {
        // Its models, as a click on the tone asks; a pack that cannot be listed leaves the
        // Library showing the model (the web UI's fallBackToLibrary).
        const auto fallBack = [this, showSelected, libraryFilter,
                               weak = std::weak_ptr<Browser>(state)](const std::string& reason) {
            const auto st = weak.lock();

            if (!st || !st->fillTabs)
            {
                return;
            }

            ShowToast("Tone3000 pack unavailable", reason + " Showing the model in your library instead.", false);
            st->tab = Tab::Library;
            const auto shown = FilterLibrary(mState, st->resourceType, libraryFilter(), st->selectedId);

            if (std::none_of(shown.begin(), shown.end(),
                             [&st](const LibraryResource* r) { return r->id == st->selectedId; }))
            {
                st->query.clear();
                st->favouritesOnly = false;

                if (st->searchBox)
                {
                    st->searchBox->set_text("");
                }
            }

            st->fillTabs();
            st->fillControls();
            st->fillList();
            st->fillFooter();
            showSelected(weak);
        };

        if (mSession.Tones().Tone3000NeedsKey())
        {
            b->t3ModelsLoading = false;
            b->t3Open.clear();
            mSession.View().post([fallBack] { fallBack("Add a Tone3000 API key in Settings to browse."); });
        }
        else
        {
            mSession.Tones().Tone3000Models(
                sourceTone->id, [fallBack, weak = std::weak_ptr<Browser>(state),
                                 id = sourceTone->id](const std::vector<t3::Model>& models, const std::string& error) {
                    const auto st = weak.lock();

                    if (!st || st->t3Open != id)
                    {
                        return;
                    }

                    st->t3Models = models;
                    st->t3ModelsLoading = false;

                    if (!error.empty() || models.empty())
                    {
                        st->t3Open.clear();
                        fallBack(!error.empty() ? "Its models could not be loaded (" + error + ")."
                                                : std::string("It has no models to show."));
                        return;
                    }

                    if (st->fillList)
                    {
                        st->fillList();
                        st->fillFooter();
                    }
                });
        }
    }
    else if (b->tab == Tab::Tone3000 && b->t3Tones.empty())
    {
        // Back on Tone 3000 with nothing kept (the last search was still on its way).
        b->searchTone3000(b->t3Page);
    }

    auto cancel = [this, b] {
        // The x is Cancel, as in the web UI.
        if (b->previewed && !b->onPick)
        {
            if (b->hadOriginal && b->original.IsLibraryRef())
            {
                mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, b->original.resourceId, b->index);
            }
            else if (b->hadOriginal && b->original.IsFilePath())
            {
                mSession.Commands().SetNodeResourceFile(b->nodeId, b->resourceType,
                                                        guitarfx::util::PathToUtf8(b->original.filePath), b->index);
            }
            else
            {
                mSession.Commands().SetNodeResource(b->nodeId, b->resourceType, {}, b->index);
            }
        }

        b->pickedFrom = -1;
        CloseOverlay();
    };

    // The list is what this opens for: its search box waits for a tap.
    if (mKeyboard)
    {
        mKeyboard->OpenQuietly();
    }

    const auto view = ViewSize();
    // A short window (800x480) keeps the margins round the dialog and between its parts small, so
    // the list shows four rows under the search and filters.
    const bool short_ = view.y < 560.0f;
    ShowDialog({.title = title.empty() ? b->role.title : title,
                .body = Col({FixedHeight(38, b->tabs),
                             FixedHeight(1, Fill(mTheme.ModalRule(), std::make_shared<el::element>())),
                             VGap(short_ ? 8 : 12), b->controls, VGap(short_ ? 6 : 10), b->list, b->pager}),
                .footerLeft = b->footer,
                .width = std::min(944.0f, view.x - 16.0f),
                .height = std::max(320.0f, view.y - (short_ ? 16.0f : 40.0f)),
                .padX = 16.0f,
                .padY = 10.0f,
                .onClose = cancel,
                .stretch = true});
    showSelected(state);
    mOnOverlayClosed = [this, state, memory] {
        // Kept for the slot's next open; a list still on its way is asked for again then.
        memory->tab = static_cast<int>(state->tab);
        memory->category = state->category;
        memory->query = state->query;
        memory->arch = state->arch;
        memory->creator = state->creator;
        memory->tags = state->tags;
        memory->favouritesOnly = state->favouritesOnly;
        memory->openPacks = state->openPacks;
        memory->t3Query = state->t3Query;
        memory->t3Gear = state->t3Gear;
        memory->t3Sort = state->t3Sort;
        memory->t3Arch = state->t3Arch;
        memory->t3Page = state->t3Page;
        memory->t3Pages = state->t3Pages;
        memory->t3Tones = state->t3Loading ? std::vector<t3::Tone>() : state->t3Tones;
        memory->t3Open = state->t3ModelsLoading ? std::string() : state->t3Open;
        memory->t3Models = state->t3ModelsLoading ? std::vector<t3::Model>() : state->t3Models;

        if (!memory->t3Open.empty() && !memory->t3Models.empty())
        {
            memory->t3ModelsByTone[memory->t3Open] = memory->t3Models;
        }

        // A choice kept: the slot's arrows walk the list it came from.
        if (state->pickedFrom >= 0)
        {
            memory->stepFrom = state->pickedFrom;
            memory->folderFiles = state->pickedFrom == static_cast<int>(Tab::Folder)
                                      ? state->listedFiles
                                      : std::vector<ResourceFolderListing::File>();
        }

        mSession.Tones().RemoveListener(state->toneListener);
        state->subscriptions.clear();
        state->thumbnailFor = nullptr;
        state->thumbnails.reset();
        state->fillTabs = state->fillControls = state->fillList = state->fillFooter = nullptr;
        state->searchTone3000 = nullptr;
    };
    (void)commands;
}

namespace
{
/// Where a slot's previous or next lands, worked out from the list it walks.
struct SlotStep
{
    bool possible = false;
    std::string resourceId, filePath; // a library entry or a folder file
    std::optional<t3::Tone> tone;     // a Tone3000 model, imported first
    std::optional<t3::Model> model;
    std::string fetchToneId; // a tone whose models are not known yet
};

/// The slot's library list for its arrows: its category (else everything), its browser's
/// other filters, in the order the browser draws it.
std::vector<const LibraryResource*> SlotLibraryList(const ClientState& state, const std::string& resourceType,
                                                    const SlotRole& role, const ResourceBrowserMemory& memory,
                                                    const std::string& currentId)
{
    ResourceFilter filter;
    filter.query = memory.query;
    filter.architecture = memory.arch.empty() ? std::string("all") : memory.arch;
    filter.creator = memory.creator.empty() ? std::string("all") : memory.creator;
    filter.tags = memory.tags;
    filter.favouritesOnly = memory.favouritesOnly;

    // What the browser last showed; before it has been opened, the slot's own category, or the
    // loaded entry's when it is elsewhere (as the browser opens).
    if (memory.category)
    {
        filter.category = memory.category->empty() ? std::string("all") : *memory.category;
    }
    else
    {
        const auto available = LibraryCategories(state, resourceType);
        const auto hint = resourceType == "ir" ? std::string(role.gear == "space" ? "reverb" : "cab") : role.gear;
        filter.category = ResolveLibraryCategoryFromHint(hint, available).value_or("all");
        const auto* current = currentId.empty() ? nullptr : state.FindLibraryResource(resourceType, currentId);
        const auto own = current == nullptr          ? std::string()
                         : current->category.empty() ? std::string("Uncategorized")
                                                     : current->category;

        if (current != nullptr && filter.category != "all" && filter.category != own)
        {
            filter.category =
                std::find(available.begin(), available.end(), own) != available.end() ? own : std::string("all");
        }
    }

    auto list = NavigationOrder(state, FilterLibrary(state, resourceType, filter, currentId));

    // A category with nothing in it would leave the arrows nothing to do.
    if (list.empty() && filter.category != "all")
    {
        filter.category = "all";
        list = NavigationOrder(state, FilterLibrary(state, resourceType, filter, currentId));
    }

    std::erase_if(list, [](const LibraryResource* r) { return r->fileMissing; });
    return list;
}

SlotStep NextInSlotList(const ClientState& state, const std::string& resourceType, const SlotRole& role,
                        const ResourceBrowserMemory& memory, const guitarfx::ResourceRef* current, int delta)
{
    SlotStep step;
    const auto currentId = current != nullptr ? current->resourceId : std::string();
    const auto currentPath = current != nullptr ? guitarfx::util::PathToUtf8(current->filePath) : std::string();
    const int direction = delta >= 0 ? 1 : -1;

    // Tone3000: the results' models in order, tone after tone, and no further at either end.
    if (memory.stepFrom == 2 && !memory.t3Tones.empty())
    {
        std::string modelId, toneId;

        if (const auto* resource = currentId.empty() ? nullptr : state.FindLibraryResource(resourceType, currentId))
        {
            modelId = MetaText(resource->metadata, "modelId");
            toneId = MetaText(resource->metadata, "toneId");
        }

        if (modelId.empty() && currentId.rfind("tone3000:", 0) == 0)
        {
            modelId = currentId.substr(9, currentId.find(':', 9) - 9);
        }

        const auto modelsOf = [&memory](const std::string& id) -> const std::vector<t3::Model>* {
            const auto it = memory.t3ModelsByTone.find(id);
            return it != memory.t3ModelsByTone.end() ? &it->second : nullptr;
        };
        int toneAt = -1, modelAt = -1;

        for (int t = 0; t < static_cast<int>(memory.t3Tones.size()) && modelAt < 0; ++t)
        {
            const auto& tone = memory.t3Tones[static_cast<std::size_t>(t)];
            const auto* models = modelsOf(tone.id);

            for (int m = 0; models != nullptr && m < static_cast<int>(models->size()); ++m)
            {
                if (!modelId.empty() && (*models)[static_cast<std::size_t>(m)].id == modelId)
                {
                    toneAt = t;
                    modelAt = m;
                }
            }

            toneAt = modelAt < 0 && !toneId.empty() && tone.id == toneId ? t : toneAt;
        }

        // Within the tone, else into the next tone along (its models fetched first).
        if (toneAt >= 0 && modelAt >= 0)
        {
            const auto& models = *modelsOf(memory.t3Tones[static_cast<std::size_t>(toneAt)].id);
            const int next = modelAt + direction;

            if (next >= 0 && next < static_cast<int>(models.size()))
            {
                step.possible = true;
                step.tone = memory.t3Tones[static_cast<std::size_t>(toneAt)];
                step.model = models[static_cast<std::size_t>(next)];
                return step;
            }
        }

        const int toneCount = static_cast<int>(memory.t3Tones.size());

        for (int t = toneAt < 0 ? (direction > 0 ? 0 : toneCount - 1) : toneAt + direction; t >= 0 && t < toneCount;
             t += direction)
        {
            const auto& tone = memory.t3Tones[static_cast<std::size_t>(t)];
            const auto* models = modelsOf(tone.id);

            if (models == nullptr)
            {
                step.possible = true;
                step.fetchToneId = tone.id;
                return step;
            }

            if (!models->empty())
            {
                step.possible = true;
                step.tone = tone;
                step.model = direction > 0 ? models->front() : models->back();
                return step;
            }
        }

        return step;
    }

    // The folder: its files of this type as the Folder tab listed them, round and round.
    if (memory.stepFrom == 1 && !memory.folderFiles.empty())
    {
        const auto& files = memory.folderFiles;
        const auto it = std::find_if(files.begin(), files.end(), [&](const ResourceFolderListing::File& f) {
            return (!currentPath.empty() && f.path == currentPath) ||
                   (!currentId.empty() && f.inLibrary && f.libraryId == currentId);
        });
        const int count = static_cast<int>(files.size());
        const int at = it == files.end() ? -1 : static_cast<int>(it - files.begin());
        const int next = at < 0 ? (direction > 0 ? 0 : count - 1) : ((at + direction) % count + count) % count;

        if (at >= 0 && next == at)
        {
            return step;
        }

        const auto& file = files[static_cast<std::size_t>(next)];
        step.possible = true;
        step.resourceId = file.inLibrary ? file.libraryId : std::string();
        step.filePath = file.inLibrary ? std::string() : file.path;
        return step;
    }

    // The library as the slot's browser filters it, round and round.
    const auto list = SlotLibraryList(state, resourceType, role, memory, currentId);

    if (list.empty())
    {
        return step;
    }

    const auto it = std::find_if(list.begin(), list.end(), [&](const LibraryResource* r) {
        return (!currentId.empty() && r->id == currentId) || (!currentPath.empty() && r->filePath == currentPath);
    });
    const int count = static_cast<int>(list.size());
    const int at = it == list.end() ? -1 : static_cast<int>(it - list.begin());
    const int next = at < 0 ? (direction > 0 ? 0 : count - 1) : ((at + direction) % count + count) % count;

    if (at >= 0 && next == at)
    {
        return step;
    }

    step.possible = true;
    step.resourceId = list[static_cast<std::size_t>(next)]->id;
    return step;
}

const guitarfx::ResourceRef* SlotResourceRef(const guitarfx::GraphNode& node, int resourceIndex)
{
    const auto index = static_cast<std::size_t>(std::max(0, resourceIndex));
    const auto* ref = index < node.resources.size() ? &node.resources[index] : nullptr;
    return ref != nullptr && (ref->IsLibraryRef() || ref->IsFilePath()) ? ref : nullptr;
}
} // namespace

bool Shell::CanStepSlotResource(const std::string& nodeId, const std::string& resourceType, int resourceIndex,
                                int delta)
{
    const auto* node = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;

    if (node == nullptr)
    {
        return false;
    }

    const auto role = RoleFor(mState, *node, resourceType, resourceIndex);
    const auto it = mResourceBrowserMemory.find(resourceType + ":" + role.context);
    const ResourceBrowserMemory none;
    const auto& memory = it != mResourceBrowserMemory.end() && it->second ? *it->second : none;
    return NextInSlotList(mState, resourceType, role, memory, SlotResourceRef(*node, resourceIndex), delta).possible;
}

void Shell::StepSlotResource(const std::string& nodeId, const std::string& resourceType, int resourceIndex, int delta)
{
    const auto* node = mState.activePreset ? mState.activePreset->graph.FindNode(nodeId) : nullptr;

    if (node == nullptr)
    {
        return;
    }

    const auto role = RoleFor(mState, *node, resourceType, resourceIndex);
    auto& slot = mResourceBrowserMemory[resourceType + ":" + role.context];

    if (!slot)
    {
        slot = std::make_shared<ResourceBrowserMemory>();
    }

    const auto memory = slot;

    if (memory->stepping)
    {
        return; // a Tone3000 step still fetching or importing: this click would repeat it
    }

    const auto step = NextInSlotList(mState, resourceType, role, *memory, SlotResourceRef(*node, resourceIndex), delta);
    auto& commands = mSession.Commands();

    if (!step.possible)
    {
        return;
    }

    if (!step.resourceId.empty())
    {
        commands.SetNodeResource(nodeId, resourceType, step.resourceId, resourceIndex);
        return;
    }

    if (!step.filePath.empty())
    {
        commands.SetNodeResourceFile(nodeId, resourceType, step.filePath, resourceIndex);
        return;
    }

    const auto alive = std::weak_ptr<bool>(mAlive);

    // The next tone's models, then the step again from there.
    if (!step.fetchToneId.empty())
    {
        memory->stepping = true;
        mSession.Tones().Tone3000Models(
            step.fetchToneId,
            [this, alive, memory, nodeId, resourceType, resourceIndex, delta,
             toneId = step.fetchToneId](const std::vector<t3::Model>& models, const std::string& error) {
                memory->stepping = false;

                if (!alive.lock())
                {
                    return;
                }

                if (!error.empty())
                {
                    ShowToast("Could not list the tone's models", error, true);
                    return;
                }

                memory->t3ModelsByTone[toneId] = models;
                StepSlotResource(nodeId, resourceType, resourceIndex, delta);
            });
        return;
    }

    if (!step.tone || !step.model)
    {
        return;
    }

    if (const auto* imported = t3::FindImported(mState, resourceType, *step.model))
    {
        commands.SetNodeResource(nodeId, resourceType, imported->id, resourceIndex);
        return;
    }

    memory->stepping = true;
    mSession.Tones().ImportTone3000(
        *step.tone, *step.model, resourceType, resourceType == "ir" ? role.gear : std::string(),
        [this, alive, memory, nodeId, resourceType, resourceIndex](const std::string& id, const std::string& error) {
            memory->stepping = false;

            if (!alive.lock())
            {
                return;
            }

            if (!error.empty())
            {
                ShowToast("Could not import the model", error, true);
                return;
            }

            mSession.Commands().SetNodeResource(nodeId, resourceType, id, resourceIndex);
        });
}
} // namespace nanoq::ui
