// Walking the element tree for keyboard focus (Focus.h): which controls Tab visits and in what
// order, where each is, and moving focus to one.
//
// Elements' own Tab moves through a composite's children whether they are shown or not, and runs
// off the end of a dialog into the page under it, so KeyRouter does its own: it collects the
// controls under it, in layout order, with their bounds (the walk builds each child's context as
// Elements does when it draws, a scroller's offset included), and focuses the next one by setting
// each composite's focus on the way down.

#include "ui/input/Focus.h"

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#endif

#include <algorithm>
#include <map>

namespace nanoq::ui
{
namespace
{
const char* RoleName(AccessRole role)
{
    switch (role)
    {
    case AccessRole::Button:
        return "button";
    case AccessRole::Toggle:
        return "switch";
    case AccessRole::CheckBox:
        return "checkbox";
    case AccessRole::Slider:
        return "slider";
    case AccessRole::Tab:
        return "tab";
    case AccessRole::ListItem:
        return "item";
    case AccessRole::MenuItem:
        return "menuitem";
    case AccessRole::ComboBox:
        return "combobox";
    case AccessRole::Edit:
        return "edit";
    case AccessRole::Heading:
        return "heading";
    case AccessRole::Label:
        return "text";
    case AccessRole::Meter:
        return "meter";
    case AccessRole::Dialog:
        return "dialog";
    case AccessRole::Menu:
        return "menu";
    case AccessRole::List:
        return "list";
    case AccessRole::TabList:
        return "tablist";
    }

    return "control";
}

using Path = std::vector<std::pair<el::composite_base*, std::size_t>>;

struct WalkOptions
{
    bool forAccess = false;
    const std::string* label = nullptr; // the nearest AccessLabel's
};

void Walk(const el::context& ctx, el::element& e, Path& path, std::vector<FocusTarget>& out, WalkOptions options)
{
    const auto label = [&options] { return options.label != nullptr ? *options.label : std::string(); };

    // A control is a leaf of the walk, whatever it is built from.
    if (auto* f = dynamic_cast<Focusable*>(&e))
    {
        // A screen reader hears a disabled control too, as the web's disabled button: Tab skips it.
        if (ctx.bounds.width() > 0.0f && ctx.bounds.height() > 0.0f &&
            (e.wants_focus() || (options.forAccess && !f->Describe().enabled)))
        {
            out.push_back({&e, f, ctx.bounds, path, {}, 0, label(), false, !e.wants_focus()});
        }

        return;
    }

    if (options.forAccess)
    {
        if (const auto* s = dynamic_cast<const AccessStatic*>(&e))
        {
            if (ctx.bounds.width() > 0.0f && ctx.bounds.height() > 0.0f && s->DescribeStatic())
            {
                out.push_back({&e, nullptr, ctx.bounds, path, {}, 0, {}, true, true});
            }

            return;
        }
    }

    if (auto* c = dynamic_cast<el::composite_base*>(&e))
    {
        for (std::size_t i = 0; i < c->size(); ++i)
        {
            auto& child = c->at(i);
            el::context cctx{ctx, &child, c->bounds_of(ctx, i)};
            path.emplace_back(c, i);
            Walk(cctx, child, path, out, options);
            path.pop_back();
        }

        return;
    }

    if (auto* p = dynamic_cast<el::proxy_base*>(&e))
    {
        if (const auto* named = dynamic_cast<const AccessLabel*>(p))
        {
            options.label = &named->Label();
        }

        el::context sctx{ctx, &p->subject(), ctx.bounds};
        p->prepare_subject(sctx);
        Walk(sctx, p->subject(), path, out, options);
        p->restore_subject(sctx);
        return;
    }

    // A slot (Layout.h): what it shows, where it is.
    if (auto* i = dynamic_cast<el::indirect_base*>(&e))
    {
        el::context ictx{ctx, &i->get(), ctx.bounds};
        Walk(ictx, i->get(), path, out, options);
        return;
    }

    // An Elements text field.
    if (dynamic_cast<el::basic_text_box*>(&e) != nullptr && e.wants_focus() && ctx.bounds.width() > 0.0f)
    {
        out.push_back({&e, nullptr, ctx.bounds, path, {}, 0, label(), false});
    }
}

/// How many of the walk's steps two paths share.
std::size_t CommonPrefix(const FocusTarget& a, const FocusTarget& b)
{
    std::size_t n = 0;

    while (n < a.path.size() && n < b.path.size() && a.path[n] == b.path[n])
    {
        ++n;
    }

    return n;
}

/// Under the same composite as the set's: its first `depth` steps are `first`'s, and the next
/// step is in the same composite.
bool UnderSet(const FocusTarget& t, const FocusTarget& first, std::size_t depth)
{
    return t.path.size() > depth && first.path.size() > depth && CommonPrefix(t, first) >= depth &&
           t.path[depth].first == first.path[depth].first;
}
} // namespace

AccessInfo DescribeTarget(const el::element& element)
{
    if (const auto* f = dynamic_cast<const Focusable*>(&element))
    {
        auto info = f->Describe();

        // The tooltip as help text, where it says more than the name.
        if (auto tip = f->TooltipText(); info.help.empty() && tip != info.name)
        {
            info.help = std::move(tip);
        }

        if (info.shortcut.empty())
        {
            info.shortcut = f->Shortcut();
        }

        return info;
    }

    if (const auto* s = dynamic_cast<const AccessStatic*>(&element))
    {
        return s->DescribeStatic().value_or(AccessInfo{AccessRole::Label});
    }

    AccessInfo info;
    info.role = AccessRole::Edit;

    if (const auto* box = dynamic_cast<const el::basic_text_box*>(&element))
    {
        info.value = box->get_utf8();
        info.enabled = box->editable();
    }

    return info;
}

AccessInfo DescribeTarget(const FocusTarget& target)
{
    auto info = DescribeTarget(*target.element);

    if (info.name.empty())
    {
        info.name = target.label;
    }

    return info;
}

std::vector<FocusSet> FindFocusSets(const std::vector<FocusTarget>& targets, const std::vector<AccessInfo>& infos,
                                    bool inMenu)
{
    std::vector<FocusSet> sets;
    std::vector<bool> taken(targets.size(), false);

    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        const auto role = infos[i].role;

        if (taken[i] ||
            (role != AccessRole::ListItem && role != AccessRole::Tab && (role != AccessRole::MenuItem || inMenu)))
        {
            continue;
        }

        // The next of its role decides the composite they share.
        std::size_t k = i + 1;

        while (k < targets.size() && infos[k].role != role)
        {
            ++k;
        }

        if (k >= targets.size())
        {
            continue;
        }

        const auto depth = CommonPrefix(targets[i], targets[k]);

        if (!UnderSet(targets[k], targets[i], depth))
        {
            continue;
        }

        FocusSet set;
        set.role = role == AccessRole::Tab        ? AccessRole::TabList
                   : role == AccessRole::MenuItem ? AccessRole::Menu
                                                  : AccessRole::List;
        set.begin = i;

        while (set.begin > 0 && UnderSet(targets[set.begin - 1], targets[i], depth))
        {
            --set.begin;
        }

        set.end = i;

        while (set.end < targets.size() && UnderSet(targets[set.end], targets[i], depth))
        {
            if (infos[set.end].role == role && !targets[set.end].isStatic)
            {
                set.items.push_back(set.end);
            }

            ++set.end;
        }

        for (auto j = set.begin; j < set.end; ++j)
        {
            taken[j] = true;
        }

        if (set.items.size() >= 2)
        {
            sets.push_back(std::move(set));
        }
    }

    return sets;
}

std::vector<FocusSet> FindFocusSets(const std::vector<FocusTarget>& targets, bool inMenu)
{
    std::vector<AccessInfo> infos;

    for (const auto& t : targets)
    {
        infos.push_back(DescribeTarget(t));
    }

    return FindFocusSets(targets, infos, inMenu);
}

int FocusPeerDepth(const std::vector<FocusTarget>& targets, std::size_t at, AccessRole role,
                   const std::vector<AccessInfo>& infos)
{
    int depth = -1;

    for (std::size_t i = 0; i < targets.size(); ++i)
    {
        if (i != at && infos[i].role == role)
        {
            depth = std::max(depth, static_cast<int>(CommonPrefix(targets[at], targets[i])));
        }
    }

    return depth;
}

bool SamePrefix(const FocusTarget& a, const FocusTarget& b, int depth)
{
    return depth >= 0 && static_cast<int>(CommonPrefix(a, b)) >= depth;
}

std::string FocusKey(const el::element& element)
{
    const auto info = DescribeTarget(element);
    return std::string(RoleName(info.role)) + ":" + info.name;
}

std::vector<FocusTarget> CollectFocusTargets(const el::context& ctx, el::element& root, bool forAccess)
{
    std::vector<FocusTarget> out;
    Path path;
    Walk(ctx, root, path, out, {forAccess, nullptr});

    // Two knobs both called Level are told apart by their order.
    std::map<std::string, int> seen, seenExtra;

    for (auto& t : out)
    {
        const auto info = DescribeTarget(t);
        t.key = std::string(RoleName(info.role)) + ":" + info.name;
        t.ordinal = t.accessOnly ? 1000 + seenExtra[t.key]++ : seen[t.key]++;
    }

    return out;
}

el::element* FocusedLeaf(el::element& root)
{
    el::element* e = &root;

    for (int depth = 0; depth < 128; ++depth)
    {
        if (dynamic_cast<Focusable*>(e) != nullptr)
        {
            return e;
        }

        auto* next = e->focus();

        if (next == nullptr || next == e)
        {
            break;
        }

        e = next;
    }

    // The end of the chain: a control that wants focus (a text box), or a composite whose focus
    // went with a rebuild.
    if (e != &root && dynamic_cast<el::composite_base*>(e) == nullptr && dynamic_cast<el::proxy_base*>(e) == nullptr &&
        dynamic_cast<el::indirect_base*>(e) == nullptr && e->wants_focus())
    {
        return e;
    }

    return nullptr;
}

void WithFocusedLeaf(const el::context& ctx, el::element& root,
                     const std::function<void(const el::context&, el::element&)>& f)
{
    if (dynamic_cast<Focusable*>(&root) != nullptr)
    {
        f(ctx, root);
        return;
    }

    if (auto* c = dynamic_cast<el::composite_base*>(&root))
    {
        const int index = c->focus_index();

        if (index < 0 || index >= static_cast<int>(c->size()))
        {
            return;
        }

        auto& child = c->at(static_cast<std::size_t>(index));
        el::context cctx{ctx, &child, c->bounds_of(ctx, static_cast<std::size_t>(index))};
        WithFocusedLeaf(cctx, child, f);
        return;
    }

    if (auto* p = dynamic_cast<el::proxy_base*>(&root))
    {
        el::context sctx{ctx, &p->subject(), ctx.bounds};
        p->prepare_subject(sctx);
        WithFocusedLeaf(sctx, p->subject(), f);
        p->restore_subject(sctx);
        return;
    }

    if (auto* i = dynamic_cast<el::indirect_base*>(&root))
    {
        el::context ictx{ctx, &i->get(), ctx.bounds};
        WithFocusedLeaf(ictx, i->get(), f);
        return;
    }

    if (root.wants_focus())
    {
        f(ctx, root);
    }
}

void FocusTargetUnder(el::view& view, el::element& root, const FocusTarget& target)
{
    // End what had focus (a value being typed into a knob is committed by its end_focus), then
    // point each composite on the way down at the next step, and start the control's focus.
    root.end_focus();

    for (const auto& [composite, index] : target.path)
    {
        composite->focus(index);
    }

    target.element->begin_focus(el::element::focus_request::from_top);

    // In a scroller: brought into view, as Elements does for its own focus moves.
    view.in_context_do(*target.element,
                       [](const el::context& ctx) { el::scrollable::find(ctx).scroll_into_view(ctx.bounds); });
    view.refresh();
}

bool TypingUnder(el::element& root)
{
    auto* leaf = FocusedLeaf(root);

    if (leaf == nullptr)
    {
        return false;
    }

    if (const auto* f = dynamic_cast<const Focusable*>(leaf))
    {
        return f->Editing();
    }

    const auto* box = dynamic_cast<const el::basic_text_box*>(leaf);
    return box != nullptr && box->select_start() != -1;
}

namespace
{
int gKeyModifiers = 0;
} // namespace

void NoteKeyModifiers(int modifiers)
{
    gKeyModifiers = modifiers;
}

bool ShiftHeld()
{
#ifdef _WIN32
    return (GetKeyState(VK_SHIFT) & 0x8000) != 0;
#else
    return (gKeyModifiers & el::mod_shift) != 0;
#endif
}

bool FocusInView(el::view& view, el::element& element)
{
    auto& layers = view.content();

    for (auto i = static_cast<int>(layers.size()) - 1; i >= 0; --i)
    {
        auto& layer = layers.at(static_cast<std::size_t>(i));
        bool found = false;
        view.in_context_do(layer, [&](const el::context& ctx) {
            for (const auto& t : CollectFocusTargets(ctx, layer))
            {
                if (t.element == &element)
                {
                    if (layers.focus_index() != i)
                    {
                        layers.end_focus();
                        layers.focus(static_cast<std::size_t>(i));
                    }

                    FocusTargetUnder(view, layer, t);
                    found = true;
                    return;
                }
            }
        });

        if (found)
        {
            return true;
        }
    }

    return false;
}

namespace
{
std::vector<el::rect>& PendingRepaints()
{
    static std::vector<el::rect> pending;
    return pending;
}
} // namespace

void RepaintSoon(const std::optional<el::rect>& area)
{
    if (area)
    {
        PendingRepaints().push_back(*area);
    }
}

void FlushRepaints(el::view& view)
{
    auto& pending = PendingRepaints();

    for (const auto& area : pending)
    {
        view.refresh(area);
    }

    pending.clear();
}
} // namespace nanoq::ui
