// The accessibility bridge (AccessBridge.h): a UI Automation provider for the view's window.
//
// Threads: UI Automation calls the providers on threads of its own, so they read only a snapshot
// of the controls (Shared, under its mutex), which the UI thread rebuilds each tick while a client
// listens. Nothing a client asks touches an element from those threads: an Invoke, a Toggle, a
// range value or a SetFocus is queued, and the UI thread finds the control again by its focus key
// (role and name, and which of that name) and acts on it at its next tick.

#include "ui/input/AccessBridge.h"

#if defined(_WIN32)

    #include "ui/input/AccessUiaProviders.h"
    #include "ui/input/KeyRouter.h"

namespace nanoq::ui
{
namespace
{
using namespace uia;
namespace el = cycfi::elements;

/// The bridge itself: the window's WM_GETOBJECT, and the snapshot the UI thread keeps.
class WindowsBridge final : public AccessBridge
{
  public:
    WindowsBridge(el::view& view, HWND hwnd) : mView(view), mShared(std::make_shared<Shared>())
    {
        mShared->hwnd = hwnd;
        mRoot = new RootProvider(mShared);
        SetWindowSubclass(hwnd, &WindowsBridge::Subclass, kSubclassId, reinterpret_cast<DWORD_PTR>(this));
    }

    ~WindowsBridge() override
    {
        HWND hwnd = nullptr;
        {
            std::lock_guard lock(mShared->mutex);
            mShared->alive = false;
            mShared->nodes.clear();
            hwnd = mShared->hwnd;
        }

        if (IsWindow(hwnd))
        {
            RemoveWindowSubclass(hwnd, &WindowsBridge::Subclass, kSubclassId);
            UiaReturnRawElementProvider(hwnd, 0, 0, nullptr);
        }

        UiaDisconnectProvider(mRoot);
        mRoot->Release();
    }

    void Tick() override;

  private:
    static constexpr UINT_PTR kSubclassId = 0x4E51A11E;

    static LRESULT CALLBACK Subclass(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data)
    {
        auto* self = reinterpret_cast<WindowsBridge*>(data);

        if (message == WM_GETOBJECT && static_cast<long>(lparam) == static_cast<long>(UiaRootObjectId))
        {
            self->mAsked = true;
            return UiaReturnRawElementProvider(hwnd, wparam, lparam, self->mRoot);
        }

        if (message == WM_NCDESTROY)
        {
            RemoveWindowSubclass(hwnd, &WindowsBridge::Subclass, kSubclassId);
            UiaReturnRawElementProvider(hwnd, 0, 0, nullptr);
            std::lock_guard lock(self->mShared->mutex);
            self->mShared->hwnd = nullptr;
            self->mShared->alive = false;
        }

        return DefSubclassProc(hwnd, message, wparam, lparam);
    }

    void Perform(const Action& action);
    std::vector<Node> Snapshot();
    void RaiseChanges(const std::vector<Node>& before, const std::vector<Node>& after);

    el::view& mView;
    std::shared_ptr<Shared> mShared;
    RootProvider* mRoot;
    bool mAsked = false;
    unsigned mTicks = 0;
    unsigned mFocusSerial = 0;
};

/// Runs `f` with each control of the layer in front, in tab order, and its context's bounds. While
/// it runs every control counts as wanting focus, as it does while the keyboard is in use, so the
/// walk finds the controls a pointer user sees too.
void ForEachControl(el::view& view, const std::function<void(const FocusTarget&)>& f)
{
    auto* router = KeyRouter::Focused(view);

    if (router == nullptr)
    {
        return;
    }

    const bool visible = Focusable::FocusVisible();
    Focusable::SetFocusVisible(true);
    view.in_context_do(*router, [&](const el::context& ctx) {
        el::context sctx{ctx, &router->subject(), ctx.bounds};
        router->prepare_subject(sctx);

        for (const auto& t : CollectFocusTargets(sctx, router->subject()))
        {
            f(t);
        }
    });
    Focusable::SetFocusVisible(visible);
}

std::vector<Node> WindowsBridge::Snapshot()
{
    std::vector<Node> nodes;
    HWND hwnd = mShared->hwnd;

    if (hwnd == nullptr)
    {
        return nodes;
    }

    // Layout pixels to the screen: the editor's zoom, then the window's DPI.
    const float scale = mView.scale() * static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
    RECT client{};
    GetClientRect(hwnd, &client);
    POINT origin{0, 0};
    ClientToScreen(hwnd, &origin);
    auto* router = KeyRouter::Focused(mView);
    const auto* leaf = router != nullptr ? FocusedLeaf(router->subject()) : nullptr;

    ForEachControl(mView, [&](const FocusTarget& t) {
        Node n;
        n.key = t.key;
        n.ordinal = t.ordinal;
        n.id = t.key + "#" + std::to_string(t.ordinal);
        n.info = DescribeTarget(*t.element);
        n.focused = t.element == leaf && (t.focusable == nullptr || t.focusable->HasFocus());
        const double left = t.bounds.left * scale, top = t.bounds.top * scale;
        n.screen = {origin.x + left, origin.y + top, t.bounds.width() * scale, t.bounds.height() * scale};
        n.offscreen = left + n.screen.width <= 0.0 || top + n.screen.height <= 0.0 || left >= client.right ||
                      top >= client.bottom;
        nodes.push_back(std::move(n));
    });

    return nodes;
}

void WindowsBridge::Perform(const Action& action)
{
    auto* router = KeyRouter::Focused(mView);

    if (router == nullptr)
    {
        return;
    }

    if (action.kind == Action::Kind::Focus)
    {
        // A screen reader moving focus is the keyboard in use.
        Focusable::SetFocusVisible(true);
        router->FocusByKey(mView, action.key, action.ordinal);
        return;
    }

    // Found in the walk, acted on after it: what it does may rebuild the region it sits in.
    std::shared_ptr<el::element> target;
    el::rect bounds;
    ForEachControl(mView, [&](const FocusTarget& t) {
        if (!target && t.key == action.key && t.ordinal == action.ordinal)
        {
            target = t.element->shared_from_this();
            bounds = t.bounds;
        }
    });

    if (!target)
    {
        return;
    }

    if (action.kind == Action::Kind::Text)
    {
        if (auto* box = dynamic_cast<el::basic_text_box*>(target.get()))
        {
            box->set_text(action.text);
            mView.refresh();
        }

        return;
    }

    if (auto* f = dynamic_cast<Focusable*>(target.get()))
    {
        if (action.kind == Action::Kind::Position)
        {
            f->SetPosition(action.value);
        }
        else
        {
            f->Activate(bounds);
        }

        mView.refresh();
    }
}

void WindowsBridge::RaiseChanges(const std::vector<Node>& before, const std::vector<Node>& after)
{
    bool structure = before.size() != after.size();

    for (std::size_t i = 0; !structure && i < after.size(); ++i)
    {
        structure = before[i].id != after[i].id;
    }

    if (structure)
    {
        UiaRaiseStructureChangedEvent(mRoot, StructureChangeType_ChildrenInvalidated, nullptr, 0);
    }

    const Node* was = nullptr;
    const Node* now = nullptr;

    for (const auto& n : before)
    {
        was = n.focused ? &n : was;
    }

    for (const auto& n : after)
    {
        now = n.focused ? &n : now;
    }

    if (now == nullptr || GetFocus() != mShared->hwnd)
    {
        return;
    }

    auto* provider = new NodeProvider(mShared, mRoot, now->id);

    if (was == nullptr || was->id != now->id)
    {
        UiaRaiseAutomationEvent(provider, UIA_AutomationFocusChangedEventId);
    }
    else
    {
        // The focused control changed under the keys: its value, its state, its name.
        VARIANT oldV{}, newV{};

        if (was->info.value != now->info.value)
        {
            SetString(&oldV, was->info.value);
            SetString(&newV, now->info.value);
            UiaRaiseAutomationPropertyChangedEvent(provider, UIA_ValueValuePropertyId, oldV, newV);
            VariantClear(&oldV);
            VariantClear(&newV);
        }

        if (was->info.toggled != now->info.toggled && now->info.toggled >= 0)
        {
            SetInt(&oldV, was->info.toggled > 0 ? ToggleState_On : ToggleState_Off);
            SetInt(&newV, now->info.toggled > 0 ? ToggleState_On : ToggleState_Off);
            UiaRaiseAutomationPropertyChangedEvent(provider, UIA_ToggleToggleStatePropertyId, oldV, newV);
        }

        if (was->info.selected != now->info.selected)
        {
            SetBool(&oldV, was->info.selected);
            SetBool(&newV, now->info.selected);
            UiaRaiseAutomationPropertyChangedEvent(provider, UIA_SelectionItemIsSelectedPropertyId, oldV, newV);
        }

        if (was->info.name != now->info.name)
        {
            SetString(&oldV, was->info.name);
            SetString(&newV, now->info.name);
            UiaRaiseAutomationPropertyChangedEvent(provider, UIA_NamePropertyId, oldV, newV);
            VariantClear(&oldV);
            VariantClear(&newV);
        }
    }

    provider->Release();
}

void WindowsBridge::Tick()
{
    std::vector<Action> actions;
    {
        std::lock_guard lock(mShared->mutex);
        actions.swap(mShared->actions);
    }

    for (const auto& action : actions)
    {
        Perform(action);
    }

    // Nothing to keep while no client has asked (or none is listening).
    if (!mAsked || !UiaClientsAreListening())
    {
        return;
    }

    // Focus moving is told at once; otherwise the list is looked at ten times a second.
    const unsigned serial = Focusable::FocusSerial();

    if (serial == mFocusSerial && actions.empty() && ++mTicks % 6 != 0)
    {
        return;
    }

    mFocusSerial = serial;
    auto nodes = Snapshot();
    std::vector<Node> before;
    {
        std::lock_guard lock(mShared->mutex);
        before = std::move(mShared->nodes);
        mShared->nodes = nodes;
    }

    RaiseChanges(before, nodes);
}
} // namespace

std::unique_ptr<AccessBridge> AccessBridge::Create(el::view& view)
{
    auto hwnd = static_cast<HWND>(view.host());

    if (hwnd == nullptr || !IsWindow(hwnd))
    {
        return nullptr;
    }

    return std::make_unique<WindowsBridge>(view, hwnd);
}
} // namespace nanoq::ui

#else

namespace nanoq::ui
{
std::unique_ptr<AccessBridge> AccessBridge::Create(cycfi::elements::view&)
{
    return nullptr;
}
} // namespace nanoq::ui

#endif
