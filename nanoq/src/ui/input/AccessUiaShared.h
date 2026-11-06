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

/// One control as the last snapshot saw it.
struct Node
{
    std::string id; // FocusKey "#" ordinal: stable across rebuilds while the control is the same
    std::string key;
    int ordinal = 0;
    AccessInfo info;
    UiaRect screen{}; // physical pixels, on the screen
    bool focused = false;
    bool offscreen = false;
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
        for (const auto& n : nodes)
        {
            if (n.id == id)
            {
                return &n;
            }
        }

        return nullptr;
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
