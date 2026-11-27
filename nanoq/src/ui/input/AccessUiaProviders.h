#pragma once

// The UI Automation providers (AccessBridge.cpp, Windows only): the window as a fragment root,
// and each node (a control, a heading or text, or a container: a dialog, a menu, a list, a row
// of tabs) as a fragment with the patterns its role takes. They read the snapshot in Shared and
// queue what a client asks; they never touch an element.

#include "ui/input/AccessUiaShared.h"

namespace nanoq::ui::uia
{
class RootProvider;

[[nodiscard]] inline bool IsReadOnlyRole(AccessRole role)
{
    return role == AccessRole::Heading || role == AccessRole::Label || role == AccessRole::Meter ||
           role == AccessRole::Dialog || role == AccessRole::Menu || role == AccessRole::List ||
           role == AccessRole::TabList;
}

/// A node's value pattern, apart from its range (both have IsReadOnly): only a text field's
/// value can be set; a knob's reads its value with its unit.
class ValueAdapter final : public IValueProvider
{
  public:
    ValueAdapter(std::shared_ptr<Shared> shared, std::string id) : mShared(std::move(shared)), mId(std::move(id))
    {
    }

    IFACEMETHODIMP_(ULONG) AddRef() override
    {
        return ++mRefs;
    }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        const ULONG refs = --mRefs;

        if (refs == 0)
        {
            delete this;
        }

        return refs;
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr)
        {
            return E_INVALIDARG;
        }

        *out = nullptr;

        if (riid == __uuidof(IUnknown) || riid == __uuidof(IValueProvider))
        {
            *out = static_cast<IValueProvider*>(this);
            AddRef();
            return S_OK;
        }

        return E_NOINTERFACE;
    }

    IFACEMETHODIMP SetValue(LPCWSTR text) override
    {
        Node node;

        if (!Get(node))
        {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }

        if (node.info.role != AccessRole::Edit)
        {
            return UIA_E_INVALIDOPERATION;
        }

        if (!node.info.enabled)
        {
            return UIA_E_ELEMENTNOTENABLED;
        }

        BSTR b = SysAllocString(text);
        const auto utf8 = Narrow(b);
        SysFreeString(b);
        mShared->Queue({Action::Kind::Text, node.key, node.ordinal, 0.0, utf8});
        return S_OK;
    }

    IFACEMETHODIMP get_Value(BSTR* out) override
    {
        Node node;

        if (!Get(node))
        {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }

        *out = SysAllocString(Wide(node.info.value).c_str());
        return S_OK;
    }

    IFACEMETHODIMP get_IsReadOnly(BOOL* out) override
    {
        Node node;

        if (!Get(node))
        {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }

        *out = node.info.role == AccessRole::Edit && node.info.enabled ? FALSE : TRUE;
        return S_OK;
    }

  private:
    bool Get(Node& node) const
    {
        std::lock_guard lock(mShared->mutex);
        const auto* found = mShared->alive ? mShared->Find(mId) : nullptr;

        if (found != nullptr)
        {
            node = *found;
        }

        return found != nullptr;
    }

    std::atomic<ULONG> mRefs{1};
    std::shared_ptr<Shared> mShared;
    std::string mId;
};

/// One control. Holds its id, not the element: every call looks the id up in the snapshot.
class NodeProvider final : public IRawElementProviderSimple,
                           public IRawElementProviderFragment,
                           public IInvokeProvider,
                           public IToggleProvider,
                           public IRangeValueProvider,
                           public ISelectionItemProvider,
                           public IExpandCollapseProvider
{
  public:
    NodeProvider(std::shared_ptr<Shared> shared, IRawElementProviderFragmentRoot* root, std::string id)
        : mShared(std::move(shared)), mRoot(root), mId(std::move(id))
    {
        mRoot->AddRef();
    }

    ~NodeProvider()
    {
        mRoot->Release();
    }

    // IUnknown
    IFACEMETHODIMP_(ULONG) AddRef() override
    {
        return ++mRefs;
    }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        const ULONG refs = --mRefs;

        if (refs == 0)
        {
            delete this;
        }

        return refs;
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr)
        {
            return E_INVALIDARG;
        }

        *out = nullptr;

        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple))
        {
            *out = static_cast<IRawElementProviderSimple*>(this);
        }
        else if (riid == __uuidof(IRawElementProviderFragment))
        {
            *out = static_cast<IRawElementProviderFragment*>(this);
        }
        // The patterns GetPatternProvider hands out are asked for by their interfaces.
        else if (riid == __uuidof(IInvokeProvider))
        {
            *out = static_cast<IInvokeProvider*>(this);
        }
        else if (riid == __uuidof(IToggleProvider))
        {
            *out = static_cast<IToggleProvider*>(this);
        }
        else if (riid == __uuidof(IRangeValueProvider))
        {
            *out = static_cast<IRangeValueProvider*>(this);
        }
        else if (riid == __uuidof(ISelectionItemProvider))
        {
            *out = static_cast<ISelectionItemProvider*>(this);
        }
        else if (riid == __uuidof(IExpandCollapseProvider))
        {
            *out = static_cast<IExpandCollapseProvider*>(this);
        }
        else
        {
            return E_NOINTERFACE;
        }

        AddRef();
        return S_OK;
    }

    // IRawElementProviderSimple
    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* out) override
    {
        *out = ProviderOptions_ServerSideProvider;
        return S_OK;
    }

    IFACEMETHODIMP GetPatternProvider(PATTERNID pattern, IUnknown** out) override
    {
        *out = nullptr;
        Node node;

        if (!Get(node))
        {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }

        const auto role = node.info.role;
        const bool control = !IsReadOnlyRole(role);
        IUnknown* found = nullptr;

        if (pattern == UIA_TogglePatternId && control && node.info.toggled >= 0)
        {
            found = static_cast<IToggleProvider*>(this);
        }
        else if (pattern == UIA_InvokePatternId && control && role != AccessRole::Slider && role != AccessRole::Edit &&
                 node.info.toggled < 0 && node.info.expanded < 0)
        {
            found = static_cast<IInvokeProvider*>(this);
        }
        else if (pattern == UIA_RangeValuePatternId && node.info.position >= 0.0)
        {
            found = static_cast<IRangeValueProvider*>(this);
        }
        else if (pattern == UIA_ValuePatternId &&
                 (role == AccessRole::Slider || role == AccessRole::Edit || role == AccessRole::ComboBox ||
                  role == AccessRole::Meter))
        {
            *out = new ValueAdapter(mShared, mId);
            return S_OK;
        }
        else if (pattern == UIA_SelectionItemPatternId &&
                 (role == AccessRole::Tab || role == AccessRole::ListItem || role == AccessRole::MenuItem))
        {
            found = static_cast<ISelectionItemProvider*>(this);
        }
        else if (pattern == UIA_ExpandCollapsePatternId && control && node.info.expanded >= 0)
        {
            found = static_cast<IExpandCollapseProvider*>(this);
        }

        if (found != nullptr)
        {
            found->AddRef();
            *out = found;
        }

        return S_OK;
    }

    IFACEMETHODIMP GetPropertyValue(PROPERTYID property, VARIANT* out) override
    {
        out->vt = VT_EMPTY;
        Node node;

        if (!Get(node))
        {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }

        switch (property)
        {
        case UIA_ControlTypePropertyId:
            SetInt(out, ControlType(node.info.role));
            break;
        case UIA_NamePropertyId:
            SetString(out, node.info.name.empty() ? node.info.value : node.info.name);
            break;
        case UIA_AutomationIdPropertyId:
            SetString(out, node.id);
            break;
        case UIA_HelpTextPropertyId:

            if (!node.info.help.empty())
            {
                SetString(out, node.info.help);
            }

            break;
        case UIA_FrameworkIdPropertyId:
            SetString(out, "Elements");
            break;
        case UIA_IsKeyboardFocusablePropertyId:
            SetBool(out, !IsReadOnlyRole(node.info.role) && node.info.enabled);
            break;
        case UIA_IsControlElementPropertyId:
        case UIA_IsContentElementPropertyId:
            SetBool(out, true);
            break;
        case UIA_IsDialogPropertyId:
            SetBool(out, node.info.role == AccessRole::Dialog);
            break;
        case UIA_FullDescriptionPropertyId:

            if (!node.info.description.empty())
            {
                SetString(out, node.info.description);
            }

            break;
        case UIA_HeadingLevelPropertyId:
            SetInt(out, static_cast<int>(kHeadingLevelNone) +
                            (node.info.role == AccessRole::Heading ? std::clamp(node.info.level, 1, 9) : 0));
            break;
        case UIA_PositionInSetPropertyId:

            if (node.positionInSet > 0)
            {
                SetInt(out, node.positionInSet);
            }

            break;
        case UIA_SizeOfSetPropertyId:

            if (node.sizeOfSet > 0)
            {
                SetInt(out, node.sizeOfSet);
            }

            break;
        case UIA_AcceleratorKeyPropertyId:

            if (!node.info.shortcut.empty())
            {
                SetString(out, node.info.shortcut);
            }

            break;
        case UIA_HasKeyboardFocusPropertyId:
            SetBool(out, node.focused);
            break;
        case UIA_IsEnabledPropertyId:
            SetBool(out, node.info.enabled);
            break;
        case UIA_IsOffscreenPropertyId:
            SetBool(out, node.offscreen);
            break;
        case UIA_ItemStatusPropertyId:

            if (node.info.role == AccessRole::ListItem && !node.info.value.empty())
            {
                SetString(out, node.info.value);
            }

            break;
        default:
            break;
        }

        return S_OK;
    }

    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** out) override
    {
        *out = nullptr;
        return S_OK;
    }

    // IRawElementProviderFragment
    IFACEMETHODIMP Navigate(NavigateDirection direction, IRawElementProviderFragment** out) override;

    IFACEMETHODIMP GetRuntimeId(SAFEARRAY** out) override
    {
        const int ids[] = {UiaAppendRuntimeId, IdHash(mId)};
        *out = SafeArrayCreateVector(VT_I4, 0, 2);

        if (*out == nullptr)
        {
            return E_OUTOFMEMORY;
        }

        for (LONG i = 0; i < 2; ++i)
        {
            SafeArrayPutElement(*out, &i, const_cast<int*>(&ids[i]));
        }

        return S_OK;
    }

    IFACEMETHODIMP get_BoundingRectangle(UiaRect* out) override
    {
        Node node;

        if (!Get(node))
        {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }

        *out = node.screen;
        return S_OK;
    }

    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** out) override
    {
        *out = nullptr;
        return S_OK;
    }

    IFACEMETHODIMP SetFocus() override
    {
        return QueueFor(Action::Kind::Focus);
    }

    IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** out) override
    {
        mRoot->AddRef();
        *out = mRoot;
        return S_OK;
    }

    // IInvokeProvider
    IFACEMETHODIMP Invoke() override
    {
        return QueueFor(Action::Kind::Invoke);
    }

    // IToggleProvider
    IFACEMETHODIMP Toggle() override
    {
        return QueueFor(Action::Kind::Invoke);
    }

    IFACEMETHODIMP get_ToggleState(ToggleState* out) override
    {
        Node node;

        if (!Get(node))
        {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }

        *out = node.info.toggled > 0 ? ToggleState_On : ToggleState_Off;
        return S_OK;
    }

    // IRangeValueProvider: the position as 0 to 100; the value pattern reads it with its unit.
    IFACEMETHODIMP SetValue(double value) override
    {
        Node node;

        if (Get(node) && IsReadOnlyRole(node.info.role))
        {
            return UIA_E_INVALIDOPERATION;
        }

        return QueueFor(Action::Kind::Position, std::clamp(value, 0.0, 100.0) / 100.0);
    }

    IFACEMETHODIMP get_Value(double* out) override
    {
        Node node;

        if (!Get(node))
        {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }

        *out = std::max(0.0, node.info.position) * 100.0;
        return S_OK;
    }

    IFACEMETHODIMP get_IsReadOnly(BOOL* out) override
    {
        Node node;

        if (!Get(node))
        {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }

        // The range is set from a screen reader as from the keys; a meter's only shows.
        *out = node.info.enabled && !IsReadOnlyRole(node.info.role) ? FALSE : TRUE;
        return S_OK;
    }

    IFACEMETHODIMP get_Maximum(double* out) override
    {
        *out = 100.0;
        return S_OK;
    }

    IFACEMETHODIMP get_Minimum(double* out) override
    {
        *out = 0.0;
        return S_OK;
    }

    // A stepped knob (an enum's choices) moves a whole step at a time, as its keys do.
    IFACEMETHODIMP get_LargeChange(double* out) override
    {
        Node node;
        *out = Get(node) && node.info.steps > 1 ? std::max(10.0, 100.0 / (node.info.steps - 1)) : 10.0;
        return S_OK;
    }

    IFACEMETHODIMP get_SmallChange(double* out) override
    {
        Node node;
        *out = Get(node) && node.info.steps > 1 ? 100.0 / (node.info.steps - 1) : 1.0;
        return S_OK;
    }

    // IExpandCollapseProvider: a picker or a menu button. Opening it is pressing it; its list
    // takes the screen reader's focus, and closing that list is Esc there.
    IFACEMETHODIMP Expand() override
    {
        return QueueFor(Action::Kind::Invoke);
    }

    IFACEMETHODIMP Collapse() override
    {
        return S_OK;
    }

    IFACEMETHODIMP get_ExpandCollapseState(ExpandCollapseState* out) override
    {
        Node node;

        if (!Get(node))
        {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }

        *out = node.info.expanded > 0 ? ExpandCollapseState_Expanded : ExpandCollapseState_Collapsed;
        return S_OK;
    }

    // ISelectionItemProvider: the chosen tab, row or menu item.
    IFACEMETHODIMP Select() override
    {
        return QueueFor(Action::Kind::Invoke);
    }

    IFACEMETHODIMP AddToSelection() override
    {
        return UIA_E_INVALIDOPERATION;
    }

    IFACEMETHODIMP RemoveFromSelection() override
    {
        return UIA_E_INVALIDOPERATION;
    }

    IFACEMETHODIMP get_IsSelected(BOOL* out) override
    {
        Node node;

        if (!Get(node))
        {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }

        *out = node.info.selected ? TRUE : FALSE;
        return S_OK;
    }

    IFACEMETHODIMP get_SelectionContainer(IRawElementProviderSimple** out) override
    {
        *out = nullptr;
        Node node;

        if (Get(node) && !node.parent.empty())
        {
            *out = new NodeProvider(mShared, mRoot, node.parent);
        }

        return S_OK;
    }

  private:
    bool Get(Node& node) const
    {
        std::lock_guard lock(mShared->mutex);

        if (!mShared->alive)
        {
            return false;
        }

        const auto* found = mShared->Find(mId);

        if (found == nullptr)
        {
            return false;
        }

        node = *found;
        return true;
    }

    HRESULT QueueFor(Action::Kind kind, double value = 0.0)
    {
        Node node;

        if (!Get(node))
        {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }

        if (!node.info.enabled)
        {
            return UIA_E_ELEMENTNOTENABLED;
        }

        mShared->Queue({kind, node.key, node.ordinal, value, {}});
        return S_OK;
    }

    std::atomic<ULONG> mRefs{1};
    std::shared_ptr<Shared> mShared;
    IRawElementProviderFragmentRoot* mRoot;
    std::string mId;
};

/// The window: its children are the controls, in tab order.
class RootProvider final : public IRawElementProviderSimple,
                           public IRawElementProviderFragment,
                           public IRawElementProviderFragmentRoot
{
  public:
    explicit RootProvider(std::shared_ptr<Shared> shared) : mShared(std::move(shared))
    {
    }

    IFACEMETHODIMP_(ULONG) AddRef() override
    {
        return ++mRefs;
    }

    IFACEMETHODIMP_(ULONG) Release() override
    {
        const ULONG refs = --mRefs;

        if (refs == 0)
        {
            delete this;
        }

        return refs;
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override
    {
        if (out == nullptr)
        {
            return E_INVALIDARG;
        }

        *out = nullptr;

        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple))
        {
            *out = static_cast<IRawElementProviderSimple*>(this);
        }
        else if (riid == __uuidof(IRawElementProviderFragment))
        {
            *out = static_cast<IRawElementProviderFragment*>(this);
        }
        else if (riid == __uuidof(IRawElementProviderFragmentRoot))
        {
            *out = static_cast<IRawElementProviderFragmentRoot*>(this);
        }
        else
        {
            return E_NOINTERFACE;
        }

        AddRef();
        return S_OK;
    }

    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* out) override
    {
        *out = ProviderOptions_ServerSideProvider;
        return S_OK;
    }

    IFACEMETHODIMP GetPatternProvider(PATTERNID, IUnknown** out) override
    {
        *out = nullptr;
        return S_OK;
    }

    IFACEMETHODIMP GetPropertyValue(PROPERTYID property, VARIANT* out) override
    {
        out->vt = VT_EMPTY;

        if (property == UIA_NamePropertyId)
        {
#if defined(NANOQ_NAME)
            SetString(out, NANOQ_NAME);
#else
            SetString(out, "Soundshed");
#endif
        }
        else if (property == UIA_ControlTypePropertyId)
        {
            SetInt(out, UIA_PaneControlTypeId);
        }
        else if (property == UIA_FrameworkIdPropertyId)
        {
            SetString(out, "Elements");
        }

        return S_OK;
    }

    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** out) override
    {
        std::lock_guard lock(mShared->mutex);
        return mShared->alive ? UiaHostProviderFromHwnd(mShared->hwnd, out) : (*out = nullptr, S_OK);
    }

    IFACEMETHODIMP Navigate(NavigateDirection direction, IRawElementProviderFragment** out) override
    {
        *out = nullptr;
        std::string id;
        {
            std::lock_guard lock(mShared->mutex);

            if (!mShared->alive || mShared->nodes.empty())
            {
                return S_OK;
            }

            if (direction == NavigateDirection_FirstChild || direction == NavigateDirection_LastChild)
            {
                id = mShared->Child({}, direction == NavigateDirection_LastChild);
            }
        }

        if (!id.empty())
        {
            *out = new NodeProvider(mShared, this, id);
        }

        return S_OK;
    }

    IFACEMETHODIMP GetRuntimeId(SAFEARRAY** out) override
    {
        *out = nullptr; // the window's own
        return S_OK;
    }

    IFACEMETHODIMP get_BoundingRectangle(UiaRect* out) override
    {
        *out = {}; // the window's own
        return S_OK;
    }

    IFACEMETHODIMP GetEmbeddedFragmentRoots(SAFEARRAY** out) override
    {
        *out = nullptr;
        return S_OK;
    }

    IFACEMETHODIMP SetFocus() override
    {
        return S_OK;
    }

    IFACEMETHODIMP get_FragmentRoot(IRawElementProviderFragmentRoot** out) override
    {
        AddRef();
        *out = this;
        return S_OK;
    }

    IFACEMETHODIMP ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** out) override
    {
        *out = nullptr;
        std::string id;
        {
            std::lock_guard lock(mShared->mutex);

            for (const auto& n : mShared->nodes)
            {
                if (x >= n.screen.left && y >= n.screen.top && x < n.screen.left + n.screen.width &&
                    y < n.screen.top + n.screen.height)
                {
                    id = n.id; // the last that holds it: drawn over the others
                }
            }
        }

        if (!id.empty())
        {
            *out = new NodeProvider(mShared, this, id);
        }

        return S_OK;
    }

    IFACEMETHODIMP GetFocus(IRawElementProviderFragment** out) override
    {
        *out = nullptr;
        std::string id;
        {
            std::lock_guard lock(mShared->mutex);

            for (const auto& n : mShared->nodes)
            {
                if (n.focused)
                {
                    id = n.id;
                }
            }
        }

        if (!id.empty())
        {
            *out = new NodeProvider(mShared, this, id);
        }

        return S_OK;
    }

  private:
    std::atomic<ULONG> mRefs{1};
    std::shared_ptr<Shared> mShared;
};

inline IFACEMETHODIMP NodeProvider::Navigate(NavigateDirection direction, IRawElementProviderFragment** out)
{
    *out = nullptr;
    std::string id;
    {
        std::lock_guard lock(mShared->mutex);
        const auto* node = mShared->alive ? mShared->Find(mId) : nullptr;

        if (node == nullptr)
        {
            return UIA_E_ELEMENTNOTAVAILABLE;
        }

        switch (direction)
        {
        case NavigateDirection_Parent:

            if (node->parent.empty())
            {
                return mRoot->QueryInterface(IID_PPV_ARGS(out));
            }

            id = node->parent;
            break;
        case NavigateDirection_FirstChild:
        case NavigateDirection_LastChild:
            id = node->container ? mShared->Child(mId, direction == NavigateDirection_LastChild) : std::string();
            break;
        case NavigateDirection_NextSibling:
        case NavigateDirection_PreviousSibling:
            id = mShared->Sibling(mId, direction == NavigateDirection_NextSibling);
            break;
        default:
            break;
        }
    }

    if (!id.empty())
    {
        *out = new NodeProvider(mShared, mRoot, id);
    }

    return S_OK;
}
} // namespace nanoq::ui::uia
