// What a resource library entry offers beyond being chosen: its details, editing them and deleting
// it, from the chooser's rows and a node's slot, and the prompt they ask in.

#include "ui/dialogs/DialogChrome.h"
#include "ui/shell/Shell.h"

#include "uiclient/NodeLabels.h"
#include "uiclient/ResourceIndex.h"
#include "uiclient/Tone3000.h"
#include "ui/library/ResourceBrowserParts.h"
#include "util/PathEncoding.h"

#include <algorithm>
#include <cctype>
#include <map>

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

namespace
{
/// The details table's labels for metadata keys (libraryDetails.ts' METADATA_LABELS); any other
/// key is spaced out from its camel case.
std::string MetadataLabel(const std::string& key)
{
    static const std::map<std::string, std::string> kLabels{{"provider", "Provider"},
                                                            {"authorUsername", "Author"},
                                                            {"modeledBy", "Modeled By"},
                                                            {"sourceUrl", "Source"},
                                                            {"architectureVersion", "Architecture"},
                                                            {"architecture_version", "Architecture"},
                                                            {"architecture", "Architecture"},
                                                            {"namFileVersion", "NAM File Version"},
                                                            {"sampleRate", "Sample Rate (Hz)"},
                                                            {"namName", "Model Name"},
                                                            {"gearMake", "Gear Make"},
                                                            {"gearModel", "Gear Model"},
                                                            {"gear_type", "Gear Type"},
                                                            {"toneType", "Tone Type"},
                                                            {"inputLevelDbu", "Input Level (dBu)"},
                                                            {"outputLevelDbu", "Output Level (dBu)"},
                                                            {"modelDate", "Model Date"},
                                                            {"trainingFinalLoss", "Training Final Loss"},
                                                            {"archive", "Pack Archive"},
                                                            {"factoryArchiveKey", "Pack"},
                                                            {"factoryArchiveHash", "Pack Hash"},
                                                            {"originalId", "Source ID"},
                                                            {"sourceFileName", "Source File"},
                                                            {"contentHash", "Content Hash"},
                                                            {"tone3000MatchedBy", "Tone3000 Match"}};

    if (const auto it = kLabels.find(key); it != kLabels.end())
    {
        return it->second;
    }

    std::string label;

    for (const char c : key)
    {
        if (c == '_')
        {
            label += ' ';
        }
        else
        {
            if (std::isupper(static_cast<unsigned char>(c)) != 0 && !label.empty() && label.back() != ' ')
            {
                label += ' ';
            }

            label += c;
        }
    }

    return label;
}

/// A metadata value as the table shows it; nothing for an empty one or a list or object.
std::string MetadataValue(const nlohmann::json& value)
{
    if (value.is_string())
    {
        return value.get<std::string>();
    }

    if (value.is_boolean())
    {
        return value.get<bool>() ? "true" : "false";
    }

    return value.is_number() ? value.dump() : std::string();
}
} // namespace

Ptr Shell::LibraryDetailsPanel(const std::string& resourceType, const std::string& resourceId, float width,
                               std::function<void()> onOpenTone)
{
    const auto* r = mState.FindLibraryResource(resourceType, resourceId);

    if (r == nullptr)
    {
        return std::make_shared<el::element>();
    }

    const auto& metadata = r->metadata.is_object() ? r->metadata : nlohmann::json::object();
    const auto text = [&metadata](const char* key) {
        const auto it = metadata.find(key);
        auto value = it != metadata.end() ? MetadataValue(*it) : std::string();
        const auto first = value.find_first_not_of(" \t");
        return first == std::string::npos ? std::string()
                                          : value.substr(first, value.find_last_not_of(" \t") - first + 1);
    };
    const float inner = std::max(200.0f, width - 24.0f);
    const float labelWidth = std::min(160.0f, inner * 0.3f);
    std::vector<Ptr> column;

    // The Tone3000 card (tone3000Card.ts): picture, title, creator, the ids, and its page.
    const auto toneId = text("toneId");

    if (!toneId.empty())
    {
        auto title = text("toneTitle");
        title = title.empty() ? text("groupName") : title;
        auto creator = text("creatorName");
        creator = creator.empty() ? text("authorUsername") : creator;
        auto card = std::make_shared<resource_browser::BrowserRow>(
            mTheme, mIcons, title.empty() ? "Tone3000 tone" : title, creator.empty() ? std::string() : "by " + creator);
        const auto image = text("imageUrl");
        card->pictureSlot = true;

        if (image.rfind("https://", 0) == 0)
        {
            card->picture = [this, image] { return RemoteImage(image); };
        }

        std::string facts;
        auto modelName = text("modelName");
        modelName = modelName.empty() ? text("entryName") : modelName;

        if (const auto modelId = text("modelId"); !modelName.empty() || !modelId.empty())
        {
            facts = "Model: " + (modelName.empty() ? std::string("unnamed") : modelName) +
                    (modelId.empty() ? "" : " #" + modelId) + "   ";
        }

        facts += "Tone: #" + toneId;

        if (const auto licence = text("licence"); !licence.empty())
        {
            facts += "   Licence: " + licence;
        }

        if (text("tone3000MatchedBy") == "contentHash")
        {
            facts += "   Found by content hash";
        }

        auto factsText = std::make_shared<Paragraph>(mTheme, facts, inner - 60.0f, 11.0f);
        factsText->Colour(mTheme.TextSecondary());
        auto source = text("sourceUrl");
        const auto pageUrl =
            source.rfind("https://www.tone3000.com/", 0) == 0 || source.rfind("https://tone3000.com/", 0) == 0
                ? source
                : "https://www.tone3000.com/tones/" + toneId;
        auto page = std::make_shared<Button>(
            mTheme, "Open on tone3000.com \xE2\x86\x97", [this, pageUrl] { mSession.Commands().OpenUrl(pageUrl); },
            Button::Style::Flat);
        page->TextSize(11.0f);
        column.push_back(FixedHeight(48, card));
        column.push_back(Pad(60, 2, 0, 0, factsText));
        column.push_back(Pad(56, 2, 0, 6, Row({page, Stretch()})));
    }

    // Back to the tone on the Tone 3000 tab, with the rest of its models.
    if (onOpenTone)
    {
        if (const auto tone = guitarfx::uiclient::tone3000::SourceToneOf(*r))
        {
            auto open = std::make_shared<Button>(mTheme, "Open \"" + tone->title + "\" in Tone3000", onOpenTone);
            open->TextSize(11.0f);
            column.push_back(Pad(0, 0, 0, 8, Row({open, Stretch()})));
        }
    }

    if (auto description = r->description; description.find_first_not_of(" \t\r\n") != std::string::npos)
    {
        auto paragraph = std::make_shared<Paragraph>(mTheme, description, inner, 11.0f);
        paragraph->Colour(mTheme.TextSecondary());
        column.push_back(Pad(0, 0, 0, 8, paragraph));
    }

    // The table: a muted label over a third, the value wrapping beside it.
    const auto row = [this, &column, labelWidth, inner](const std::string& label, Ptr value) {
        auto name = std::make_shared<Text>(mTheme, label, 11.0f);
        name->Colour(mTheme.TextMuted());
        column.push_back(
            Pad(0, 2, 0, 2,
                Row({FixedWidth(labelWidth, el::share(el::align_top(el::hold_any(FixedHeight(16, name))))), value})));
        (void)inner;
    };
    const auto valueText = [this, inner, labelWidth](const std::string& value, float less = 0.0f) {
        auto paragraph = std::make_shared<Paragraph>(mTheme, value, inner - labelWidth - less, 11.0f);
        paragraph->Colour(mTheme.Ink());
        return paragraph;
    };

    row("ID", valueText(r->id));

    if (const auto path = r->filePath; !path.empty())
    {
        auto copy = std::make_shared<Button>(
            mTheme, "Copy",
            [this, path] {
                el::set_clipboard({{"text/plain", path}});
                ShowToast("Path copied", path, false);
            },
            Button::Style::Flat);
        copy->TextSize(10.5f).FaceHeight(18.0f);
        row("File Path", Row({el::share(el::align_top(el::hold_any(Ptr(copy)))), HGap(6), valueText(path, 60.0f)}));
    }

    if (const auto tags = ResourceTags(*r); !tags.empty())
    {
        std::string joined;

        for (const auto& t : tags)
        {
            joined += (joined.empty() ? "" : ", ") + t;
        }

        row("Tags", valueText(joined));
    }

    for (const auto& [key, value] : metadata.items())
    {
        auto shown = MetadataValue(value);

        if (shown.empty() || (!toneId.empty() && key == "imageUrl"))
        {
            continue;
        }

        if (key == "contentHash")
        {
            const auto colon = shown.find(':');
            shown = shown.substr(0, colon == std::string::npos ? 12 : colon + 13) + "\xE2\x80\xA6";
        }
        else if (key == "tone3000MatchedBy" && shown == "contentHash")
        {
            shown = "Found by content hash";
        }

        row(MetadataLabel(key), valueText(shown));
    }

    // .resource-browser-item-details-panel: the row's own face, under a rule.
    auto body = Pad(12, 8, 12, 10, Col(column));
    return Pad(0, 0, 0, 4, Over(body, el::share(Surface(mTheme.Card(), mTheme.Border(), 6.0f))));
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

    if (usage == mState.resourceUsage.end())
    {
        // Not offered until the engine says nothing uses it.
        confirm.push_back({"Checking whether it is in use...", false, true, {}});
    }
    else if (usage->second.inUse)
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
    // The web UI's Edit resource popover (resourceBrowser.ts): name, category and tags, over the
    // row; the browser stays open behind it. An IR's category offers its slots, cab and reverb.
    const auto* r = mState.FindLibraryResource(resourceType, resourceId);

    if (r == nullptr)
    {
        return;
    }

    const auto provider = Lower(
        MetadataValue(r->metadata.is_object() ? r->metadata.value("provider", nlohmann::json()) : nlohmann::json()));

    if (provider == "built-in" || provider == "builtin" || provider == "factory")
    {
        ShowToast("Edit unavailable", "Built-in resources cannot be edited.", false);
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

    const auto trimmed = [](std::string text) {
        const auto first = text.find_first_not_of(" \t");
        return first == std::string::npos ? std::string()
                                          : text.substr(first, text.find_last_not_of(" \t") - first + 1);
    };
    const auto field = [this](const char* label, const char* placeholder, const std::string& value,
                              std::shared_ptr<el::basic_input_box>& box) -> Ptr {
        auto [element, input] = el::input_box(placeholder, Font(Weight::Regular), 1.0f);
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
        [this, fields, resourceType, resourceId, trimmed] {
            std::vector<std::string> tags;
            std::string text = fields->tags->get_utf8();
            std::replace(text.begin(), text.end(), ';', ',');
            std::size_t at = 0;

            while (at <= text.size())
            {
                const auto end = std::min(text.find(',', at), text.size());
                const auto tag = trimmed(text.substr(at, end - at));

                if (!tag.empty() && std::find(tags.begin(), tags.end(), tag) == tags.end())
                {
                    tags.push_back(tag);
                }

                at = end + 1;
            }

            auto name = trimmed(fields->name->get_utf8());
            auto category = trimmed(fields->category->get_utf8());
            name = name.empty() ? resourceId : name;
            mSession.Commands().UpdateLibraryResource(resourceType, resourceId, name,
                                                      category.empty() ? std::string("Uncategorized") : category, tags);
            CloseDropdown();
            ShowToast("Resource updated", name, false);
        },
        Button::Style::Primary);
    save->MinWidth(80);
    auto cancel = std::make_shared<Button>(mTheme, "Cancel", [this] { CloseDropdown(); }, Button::Style::Plain);
    cancel->MinWidth(80);
    auto heading = std::make_shared<Text>(mTheme, "Edit resource", 13.0f, Weight::SemiBold);
    const auto name = trimmed(r->name);
    const auto category = trimmed(r->category);
    std::vector<Ptr> column{
        FixedHeight(26, heading), VGap(4), field("Name", "Resource name", name.empty() ? r->id : name, fields->name),
        field("Category", "Category", category.empty() ? std::string("Local") : category, fields->category)};
    float height = 26.0f + 4.0f + 2.0f * 58.0f;

    if (resourceType == "ir")
    {
        // The IR slots' categories, as the web field's suggestions.
        std::vector<Ptr> chips;

        for (const char* suggestion : {"cab", "reverb"})
        {
            auto chip = std::make_shared<Button>(
                mTheme, suggestion, [fields, suggestion] { fields->category->set_text(suggestion); },
                Button::Style::Flat);
            chip->TextSize(11.0f);
            chips.push_back(chip);
            chips.push_back(HGap(4));
        }

        chips.push_back(Stretch());
        column.push_back(Pad(0, -6, 0, 8, FixedHeight(26, Row(chips))));
        height += 28.0f;
    }

    column.push_back(field("Tags", "Comma-separated tags", tagText, fields->tags));
    column.push_back(FixedHeight(32, Row({Stretch(), cancel, HGap(8), save})));
    height += 58.0f + 32.0f + 20.0f;
    auto panel = Pad(12, 10, 12, 10, Col(column));
    CloseDropdown();
    mDropdown = KeyLayer(PlacePanel(panel, el::rect{anchor.right - 360.0f, anchor.top, anchor.right, anchor.bottom},
                                    360.0f, height, false, [this] { CloseDropdown(); }));
    mSession.View().add(mDropdown);
}
} // namespace nanoq::ui
