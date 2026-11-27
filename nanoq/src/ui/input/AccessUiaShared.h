#pragma once

// The UI Automation bridge's shared state (AccessBridge.cpp, Windows only): the snapshot of the
// controls the providers read, the actions they queue for the UI thread, and the conversions
// between UTF-8 and the strings and VARIANTs UI Automation takes.

#include "ui/input/Focus.h"

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include <commctrl.h>
#include <ole2.h>
#include <uiautomation.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace nanoq::ui::uia
{
namespace el = cycfi::elements;

// Newer than some SDKs' headers.
#ifndef UIA_IsDialogPropertyId
    #define UIA_IsDialogPropertyId 30174
#endif
#ifndef UIA_HeadingLevelPropertyId
    #define UIA_HeadingLevelPropertyId 30173
#endif
#ifndef UIA_FullDescriptionPropertyId
    #define UIA_FullDescriptionPropertyId 30159
#endif
#ifndef UIA_PositionInSetPropertyId
    #define UIA_PositionInSetPropertyId 30152
#endif
#ifndef UIA_SizeOfSetPropertyId
    #define UIA_SizeOfSetPropertyId 30153
#endif
constexpr long kHeadingLevelNone = 80050; // HeadingLevel_None; HeadingLevel1 is one more
// The UIA_Notification event's NotificationKind and NotificationProcessing.
constexpr int kNotificationKindOther = 4;
constexpr int kProcessingImportantMostRecent = 2;
constexpr int kProcessingMostRecent = 3;

/// One control, or a container of controls (a dialog, a menu, a list, a row of tabs), as the
/// last snapshot saw it. Nodes are in tree order: a container comes before what it holds.
struct Node
{
    std::string id; // FocusKey "#" ordinal: stable across rebuilds while the control is the same
    std::string key;
    int ordinal = 0;
    AccessInfo info;
    UiaRect screen{}; // physical pixels, on the screen
    bool focused = false;
    bool offscreen = false;
    /// The container's id; empty for one at the top, under the window.
    std::string parent;
    bool container = false;
    /// An item of a list, a tab of a row, a row of a menu: where it is in its set, from 1.
    int positionInSet = 0;
    int sizeOfSet = 0;
};

struct Action
{
    enum class Kind
    {
        Invoke,
        Position,
        Focus,
        Text,
    } kind;
    std::string key;
    int ordinal = 0;
    double value = 0.0;
    std::string text;
};

/// What the providers read and the UI thread writes.
struct Shared
{
    std::mutex mutex;
    HWND hwnd = nullptr;
    std::vector<Node> nodes;
    std::vector<Action> actions;
    bool alive = true;

    const Node* Find(const std::string& id) const
    {
        const auto i = IndexOf(id);
        return i >= 0 ? &nodes[static_cast<std::size_t>(i)] : nullptr;
    }

    [[nodiscard]] int IndexOf(const std::string& id) const
    {
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            if (nodes[i].id == id)
            {
                return static_cast<int>(i);
            }
        }

        return -1;
    }

    /// The first or last node whose parent is `parent` ("" for the window's own), or "".
    [[nodiscard]] std::string Child(const std::string& parent, bool last) const
    {
        std::string found;

        for (const auto& n : nodes)
        {
            if (n.parent == parent)
            {
                found = n.id;

                if (!last)
                {
                    break;
                }
            }
        }

        return found;
    }

    /// The next or previous node with the same parent as `id`, or "".
    [[nodiscard]] std::string Sibling(const std::string& id, bool next) const
    {
        const auto i = IndexOf(id);

        if (i < 0)
        {
            return {};
        }

        const auto& parent = nodes[static_cast<std::size_t>(i)].parent;
        const int step = next ? 1 : -1;

        for (int j = i + step; j >= 0 && j < static_cast<int>(nodes.size()); j += step)
        {
            if (nodes[static_cast<std::size_t>(j)].parent == parent)
            {
                return nodes[static_cast<std::size_t>(j)].id;
            }
        }

        return {};
    }

    void Queue(Action action)
    {
        std::lock_guard lock(mutex);
        actions.push_back(std::move(action));
    }
};

inline std::wstring Wide(const std::string& utf8)
{
    if (utf8.empty())
    {
        return {};
    }

    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

inline std::string Narrow(BSTR text)
{
    if (text == nullptr)
    {
        return {};
    }

    const int len = static_cast<int>(SysStringLen(text));
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, len, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, len, out.data(), n, nullptr, nullptr);
    return out;
}

inline void SetString(VARIANT* v, const std::string& text)
{
    v->vt = VT_BSTR;
    v->bstrVal = SysAllocString(Wide(text).c_str());
}

inline void SetBool(VARIANT* v, bool b)
{
    v->vt = VT_BOOL;
    v->boolVal = b ? VARIANT_TRUE : VARIANT_FALSE;
}

inline void SetInt(VARIANT* v, int i)
{
    v->vt = VT_I4;
    v->lVal = i;
}

inline long ControlType(AccessRole role)
{
    switch (role)
    {
    case AccessRole::Button:
    case AccessRole::Toggle:
        return UIA_ButtonControlTypeId;
    case AccessRole::CheckBox:
        return UIA_CheckBoxControlTypeId;
    case AccessRole::Slider:
        return UIA_SliderControlTypeId;
    case AccessRole::Tab:
        return UIA_TabItemControlTypeId;
    case AccessRole::ListItem:
        return UIA_ListItemControlTypeId;
    case AccessRole::MenuItem:
        return UIA_MenuItemControlTypeId;
    case AccessRole::ComboBox:
        return UIA_ComboBoxControlTypeId;
    case AccessRole::Edit:
        return UIA_EditControlTypeId;
    case AccessRole::Heading:
    case AccessRole::Label:
        return UIA_TextControlTypeId;
    case AccessRole::Meter:
        return UIA_ProgressBarControlTypeId;
    case AccessRole::Dialog:
        return UIA_WindowControlTypeId;
    case AccessRole::Menu:
        return UIA_MenuControlTypeId;
    case AccessRole::List:
        return UIA_ListControlTypeId;
    case AccessRole::TabList:
        return UIA_TabControlTypeId;
    }

    return UIA_CustomControlTypeId;
}

/// A stable number for a node, for its runtime id.
inline int IdHash(const std::string& id)
{
    std::uint32_t h = 2166136261u;

    for (const unsigned char c : id)
    {
        h = (h ^ c) * 16777619u;
    }

    return static_cast<int>(h & 0x7fffffff);
}
} // namespace nanoq::ui::uia
