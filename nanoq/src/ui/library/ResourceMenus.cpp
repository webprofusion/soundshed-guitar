// What a resource library entry offers beyond being chosen: its details, editing them and deleting
// it, from the chooser's rows and a node's slot, and the prompt they ask in.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "uiclient/NodeLabels.h"
#include "uiclient/ResourceIndex.h"
#include "util/PathEncoding.h"

#include <algorithm>

namespace nanoq::ui
{
using namespace guitarfx::uiclient;

void Shell::OpenInlinePrompt(el::rect anchor, const std::string& title, const std::string& initial,
                             std::function<void(const std::string&)> onSubmit)
{
    // A prompt on the drop-down layer, so it stacks over a dialog (the resource chooser) rather
    // than taking its place as OpenPrompt does.
    auto heading = std::make_shared<Text>(mTheme, title, 12.5f, Weight::SemiBold);
    auto [boxElement, box] = el::input_box("", Font(Weight::Regular), 1.0f);
    box->set_text(initial);
    auto submit = [this, box, onSubmit] {
        auto text = box->get_utf8();
        const auto first = text.find_first_not_of(" \t");
        const auto last = text.find_last_not_of(" \t");
        text = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        CloseDropdown();
        onSubmit(text);
    };
    box->on_enter = [submit](std::string_view) {
        submit();
        return true;
    };
    box->on_escape = [this] { CloseDropdown(); };
    auto ok = std::make_shared<Button>(mTheme, "OK", submit, Button::Style::Primary);
    ok->MinWidth(64);
    auto panel = PadXY(
        10, 8, Col({FixedHeight(22, heading), FixedHeight(36, Row({el::share(std::move(boxElement)), HGap(6), ok}))}));
    CloseDropdown();
    mDropdown = KeyLayer(
        PlacePanel(panel, anchor, 320.0f, 84.0f, anchor.top > ViewSize().y * 0.6f, [this] { CloseDropdown(); }));
    mSession.View().add(mDropdown);
}

void Shell::OpenLibraryResourceMenu(const std::string& resourceType, const std::string& resourceId, el::rect anchor)
{
    // The web UI's row actions (resourceBrowser.ts): details, edit, copy the path, delete.
    const auto* r = mState.FindLibraryResource(resourceType, resourceId);

    if (r == nullptr)
    {
        return;
    }

    const std::string name = r->name.empty() ? r->id : r->name;
    const std::string category = r->category;
    const std::string path = r->filePath;
    const auto tags = ResourceTags(*r);
    mSession.Commands().QueryResourceUsage(resourceType, resourceId);

    std::vector<MenuItem> items;
    std::string detail = "Id " + r->id;

    if (const auto creator = ResourceCreator(*r); !creator.empty())
    {
        detail += "  \xC2\xB7  by " + creator;
    }

    if (!tags.empty())
    {
        detail += "  \xC2\xB7 ";

        for (const auto& t : tags)
        {
            detail += " #" + t;
        }
    }

    items.push_back({name, false, true, {}});
    items.push_back({detail, false, true, {}});
    items.push_back(
        {"Rename...", false, false, [this, resourceType, resourceId, name, category, tags, anchor] {
             mSession.View().post(
                 [this, alive = std::weak_ptr<bool>(mAlive), resourceType, resourceId, name, category, tags, anchor] {
                     if (alive.lock())
                     {
                         OpenInlinePrompt(anchor, "Name", name,
                                          [this, resourceType, resourceId, category, tags](const std::string& entered) {
                                              if (!entered.empty())
                                              {
                                                  mSession.Commands().UpdateLibraryResource(resourceType, resourceId,
                                                                                            entered, category, tags);
                                              }
                                          });
                     }
                 });
         }});
    items.push_back(
        {"Change category...", false, false, [this, resourceType, resourceId, name, category, tags, anchor] {
             mSession.View().post(
                 [this, alive = std::weak_ptr<bool>(mAlive), resourceType, resourceId, name, category, tags, anchor] {
                     if (alive.lock())
                     {
                         OpenInlinePrompt(anchor, "Category", category,
                                          [this, resourceType, resourceId, name, tags](const std::string& entered) {
                                              mSession.Commands().UpdateLibraryResource(resourceType, resourceId, name,
                                                                                        entered, tags);
                                          });
                     }
                 });
         }});

    if (!path.empty())
    {
        items.push_back({"Copy the file's path", false, false, [path] { el::set_clipboard({{"text/plain", path}}); }});
    }

    items.push_back({"Delete...", false, false, [this, resourceType, resourceId, anchor] {
                         // Asked once more, on the same layer, once this menu is gone.
                         mSession.View().post(
                             [this, alive = std::weak_ptr<bool>(mAlive), resourceType, resourceId, anchor] {
                                 if (alive.lock())
                                 {
                                     ConfirmDeleteLibraryResource(resourceType, resourceId, anchor);
                                 }
                             });
                     }});
    OpenDropdown(anchor, std::move(items), 300.0f);
}

void Shell::ConfirmDeleteLibraryResource(const std::string& resourceType, const std::string& resourceId,
                                         el::rect anchor)
{
    const auto* r = mState.FindLibraryResource(resourceType, resourceId);

    if (r == nullptr)
    {
        return;
    }

    // The engine refuses one in use; the usage asked for when the row's menu opened says so here.
    mSession.Commands().QueryResourceUsage(resourceType, resourceId);
    const std::string name = r->name.empty() ? r->id : r->name;
    const auto usage = mState.resourceUsage.find(resourceType + ":" + resourceId);
    std::vector<MenuItem> confirm{{"Delete " + name + " from the library?", false, true, {}}};

    if (usage != mState.resourceUsage.end() && usage->second.inUse)
    {
        confirm.push_back({usage->second.detail + ": it cannot be deleted while in use.", false, true, {}});
    }
    else
    {
        confirm.push_back({"Delete", false, false, [this, resourceType, resourceId] {
                               mSession.Commands().DeleteLibraryResource(resourceType, resourceId);
                           }});
    }

    confirm.push_back({"Cancel", false, false, [] {}});
    OpenDropdown(anchor, std::move(confirm), 300.0f);
}

void Shell::EditLibraryResource(const std::string& resourceType, const std::string& resourceId, el::rect anchor)
{
    // The web UI's edit (resourceBrowser.ts: name, category, tags), as a drop-down of fields under
    // the row: the browser stays open behind it.
    const auto* r = mState.FindLibraryResource(resourceType, resourceId);

    if (r == nullptr)
    {
        return;
    }

    struct Fields
    {
        std::shared_ptr<el::basic_input_box> name, category, tags;
    };

    auto fields = std::make_shared<Fields>();
    std::string tagText;

    for (const auto& t : ResourceTags(*r))
    {
        tagText += (tagText.empty() ? "" : ", ") + t;
    }

    const auto field = [this](const char* label, const std::string& value,
                              std::shared_ptr<el::basic_input_box>& box) -> Ptr {
        auto [element, input] = el::input_box("", Font(Weight::Regular), 1.0f);
        input->set_text(value);
        box = input;
        auto title = std::make_shared<Text>(mTheme, label, 11.0f, Weight::SemiBold);
        title->Colour(mTheme.TextSecondary());
        auto boxed = Over(PadXY(8, 0, Middle(el::share(std::move(element)))),
                          el::share(Surface(mTheme.Input(), mTheme.Border(), 6.0f)));
        return Col({FixedHeight(18, title), FixedHeight(32, boxed), VGap(8)});
    };

    auto save = std::make_shared<Button>(
        mTheme, "Save",
        [this, fields, resourceType, resourceId] {
            std::vector<std::string> tags;
            std::string text = fields->tags->get_utf8();
            std::replace(text.begin(), text.end(), ';', ',');
            std::size_t at = 0;

            while (at <= text.size())
            {
                const auto end = std::min(text.find(',', at), text.size());
                auto tag = text.substr(at, end - at);
                tag.erase(0, tag.find_first_not_of(" \t"));
                tag.erase(tag.find_last_not_of(" \t") + 1);

                if (!tag.empty())
                {
                    tags.push_back(tag);
                }

                at = end + 1;
            }

            const auto name = fields->name->get_utf8();

            if (!name.empty())
            {
                mSession.Commands().UpdateLibraryResource(resourceType, resourceId, name, fields->category->get_utf8(),
                                                          tags);
            }

            CloseDropdown();
        },
        Button::Style::Primary);
    save->MinWidth(80);
    auto cancel = std::make_shared<Button>(mTheme, "Cancel", [this] { CloseDropdown(); }, Button::Style::Plain);
    cancel->MinWidth(80);

    auto panel = Pad(
        12, 10, 12, 10,
        Col({field("NAME", r->name.empty() ? r->id : r->name, fields->name),
             field("CATEGORY", r->category, fields->category), field("TAGS (COMMA SEPARATED)", tagText, fields->tags),
             FixedHeight(32, Row({Stretch(), cancel, HGap(8), save}))}));
    CloseDropdown();
    mDropdown = KeyLayer(PlacePanel(panel, el::rect{anchor.right - 360.0f, anchor.top, anchor.right, anchor.bottom},
                                    360.0f, 220.0f, false, [this] { CloseDropdown(); }));
    mSession.View().add(mDropdown);
}
} // namespace nanoq::ui
