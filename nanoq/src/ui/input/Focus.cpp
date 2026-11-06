// Walking the element tree for keyboard focus (Focus.h): which controls Tab visits and in what
// order, where each is, and moving focus to one.
//
// Elements' own Tab moves through a composite's children whether they are shown or not, and runs
// off the end of a dialog into the page under it, so KeyRouter does its own: it collects the
// controls under it, in layout order, with their bounds (the walk builds each child's context as
// Elements does when it draws, a scroller's offset included), and focuses the next one by setting
// each composite's focus on the way down.

#include "ui/input/Focus.h"

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
    }

    return "control";
}

using Path = std::vector<std::pair<el::composite_base*, std::size_t>>;

void Walk(const el::context& ctx, el::element& e, Path& path, std::vector<FocusTarget>& out)
{
    // A control is a leaf of the walk, whatever it is built from.
    if (auto* f = dynamic_cast<Focusable*>(&e))
    {
        if (e.wants_focus() && ctx.bounds.width() > 0.0f && ctx.bounds.height() > 0.0f)
        {
            out.push_back({&e, f, ctx.bounds, path, {}, 0});
        }

        return;
    }

    if (auto* c = dynamic_cast<el::composite_base*>(&e))
    {
        for (std::size_t i = 0; i < c->size(); ++i)
        {
            auto& child = c->at(i);
            el::context cctx{ctx, &child, c->bounds_of(ctx, i)};
            path.emplace_back(c, i);
            Walk(cctx, child, path, out);
            path.pop_back();
        }

        return;
    }

    if (auto* p = dynamic_cast<el::proxy_base*>(&e))
    {
        el::context sctx{ctx, &p->subject(), ctx.bounds};
        p->prepare_subject(sctx);
        Walk(sctx, p->subject(), path, out);
        p->restore_subject(sctx);
        return;
    }

    // A slot (Layout.h): what it shows, where it is.
    if (auto* i = dynamic_cast<el::indirect_base*>(&e))
    {
        el::context ictx{ctx, &i->get(), ctx.bounds};
        Walk(ictx, i->get(), path, out);
        return;
    }

    // An Elements text field.
    if (dynamic_cast<el::basic_text_box*>(&e) != nullptr && e.wants_focus() && ctx.bounds.width() > 0.0f)
    {
        out.push_back({&e, nullptr, ctx.bounds, path, {}, 0});
    }
}
} // namespace

AccessInfo DescribeTarget(const el::element& element)
{
    if (const auto* f = dynamic_cast<const Focusable*>(&element))
    {
        return f->Describe();
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

std::string FocusKey(const el::element& element)
{
    const auto info = DescribeTarget(element);
    return std::string(RoleName(info.role)) + ":" + info.name;
}

std::vector<FocusTarget> CollectFocusTargets(const el::context& ctx, el::element& root)
{
    std::vector<FocusTarget> out;
    Path path;
    Walk(ctx, root, path, out);

    // Two knobs both called Level are told apart by their order.
    std::map<std::string, int> seen;

    for (auto& t : out)
    {
        t.key = FocusKey(*t.element);
        t.ordinal = seen[t.key]++;
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
