#include "ui/debug/DevDriver.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace nanoq::ui
{
namespace el = cycfi::elements;

namespace
{
/// "ctrl+shift+z", "space", "escape", "delete", "f2", "a": a key and its modifiers.
std::optional<el::key_info> ParseKey(std::string spec)
{
    int mods = 0;

    for (std::size_t plus; (plus = spec.find('+')) != std::string::npos && plus + 1 < spec.size();)
    {
        const auto mod = spec.substr(0, plus);
        spec = spec.substr(plus + 1);

        if (mod == "ctrl")
        {
            mods |= el::mod_control | el::mod_action;
        }
        else if (mod == "shift")
        {
            mods |= el::mod_shift;
        }
        else if (mod == "alt")
        {
            mods |= el::mod_alt;
        }
        else if (mod == "cmd")
        {
            mods |= el::mod_command | el::mod_action;
        }
    }

    el::key_code code = el::key_code::unknown;

    if (spec == "space")
    {
        code = el::key_code::space;
    }
    else if (spec == "escape")
    {
        code = el::key_code::escape;
    }
    else if (spec == "enter")
    {
        code = el::key_code::enter;
    }
    else if (spec == "delete")
    {
        code = el::key_code::_delete;
    }
    else if (spec == "backspace")
    {
        code = el::key_code::backspace;
    }
    else if (spec == "left")
    {
        code = el::key_code::left;
    }
    else if (spec == "right")
    {
        code = el::key_code::right;
    }
    else if (spec == "up")
    {
        code = el::key_code::up;
    }
    else if (spec == "down")
    {
        code = el::key_code::down;
    }
    else if (spec == "tab")
    {
        code = el::key_code::tab;
    }
    else if (spec == "home")
    {
        code = el::key_code::home;
    }
    else if (spec == "end")
    {
        code = el::key_code::end;
    }
    else if (spec == "pageup")
    {
        code = el::key_code::page_up;
    }
    else if (spec == "pagedown")
    {
        code = el::key_code::page_down;
    }
    else if (spec == "menu")
    {
        code = el::key_code::menu;
    }
    else if (spec.size() == 1 && spec[0] >= '0' && spec[0] <= '9')
    {
        code = static_cast<el::key_code>(static_cast<int>(el::key_code::_0) + (spec[0] - '0'));
    }
    else if (spec.size() == 2 && spec[0] == 'f' && spec[1] >= '1' && spec[1] <= '9')
    {
        code = static_cast<el::key_code>(static_cast<int>(el::key_code::f1) + (spec[1] - '1'));
    }
    else if (spec.size() == 1 && spec[0] >= 'a' && spec[0] <= 'z')
    {
        code = static_cast<el::key_code>(static_cast<int>(el::key_code::a) + (spec[0] - 'a'));
    }

    if (code == el::key_code::unknown)
    {
        return std::nullopt;
    }

    return el::key_info{code, el::key_action::press, mods};
}
} // namespace

std::optional<DevDriver> DevDriver::FromEnvironment()
{
    const char* path = std::getenv("SOUNDSHED_NANOQ_DRIVE");

    if (path == nullptr || *path == '\0')
    {
        return std::nullopt;
    }

    return DevDriver(std::filesystem::path(path));
}

void DevDriver::Poll(el::view& view)
{
    std::error_code ec;

    if (!std::filesystem::exists(mFile, ec))
    {
        return;
    }

    std::vector<std::string> lines;
    {
        std::ifstream in(mFile);

        for (std::string line; std::getline(in, line);)
        {
            lines.push_back(line);
        }
    }
    std::filesystem::remove(mFile, ec);

    auto button = [](bool down, int clicks, el::mouse_button::what which, float x, float y) {
        return el::mouse_button{down, clicks, which, 0, el::point{x, y}};
    };

    for (const auto& line : lines)
    {
        if (line.rfind("setting ", 0) == 0)
        {
            const auto rest = line.substr(8);
            const auto space = rest.find(' ');

            if (space != std::string::npos && onSetting)
            {
                onSetting(rest.substr(0, space), rest.substr(space + 1));
            }

            continue;
        }

        if (line.rfind("key ", 0) == 0)
        {
            if (auto k = ParseKey(line.substr(4)))
            {
                view.key(*k);
                k->action = el::key_action::release;
                view.key(*k);
            }

            continue;
        }

        if (line.rfind("drop ", 0) == 0)
        {
            // drop x y <path>: a file dropped from the desktop, as Elements' host delivers it.
            std::istringstream words(line.substr(5));
            float x = 0.0f, y = 0.0f;
            std::string path;

            if (words >> x >> y && std::getline(words >> std::ws, path) && !path.empty())
            {
                el::drop_info info;
                info.data["text/uri-list"] = std::string("file://") + path;
                info.where = el::point{x, y};
                view.track_drop(info, el::cursor_tracking::entering);
                view.drop(info);
            }

            continue;
        }

        if (line.rfind("type ", 0) == 0)
        {
            // Text, as typed into whatever has focus.
            for (const char ch : line.substr(5))
            {
                view.text(el::text_info{static_cast<std::uint32_t>(static_cast<unsigned char>(ch)), 0});
            }

            continue;
        }

        if (line.rfind("shot ", 0) == 0)
        {
            // Drawn offscreen, so it works while the window is hidden or the session is locked
            // (a GPU window then captures black).
            const auto size = view.size();
            cycfi::artist::image img{size.x, size.y};
            {
                cycfi::artist::offscreen_image offscreen{img};
                cycfi::artist::canvas cnv{offscreen.context()};
                view.draw(cnv);
            }
            img.save_png(line.substr(5));
            continue;
        }

        std::istringstream words(line);
        std::string verb;
        float x = 0.0f, y = 0.0f;

        if (!(words >> verb >> x >> y))
        {
            continue;
        }

        if (verb == "move")
        {
            view.cursor(el::point{x, y}, el::cursor_tracking::hovering);
        }
        else if (verb == "click" || verb == "right" || verb == "double")
        {
            const auto which = verb == "right" ? el::mouse_button::right : el::mouse_button::left;
            view.cursor(el::point{x, y}, el::cursor_tracking::hovering);
            view.click(button(true, 1, which, x, y));
            view.click(button(false, 1, which, x, y));

            if (verb == "double")
            {
                view.click(button(true, 2, which, x, y));
                view.click(button(false, 2, which, x, y));
            }
        }
        else if (verb == "drag")
        {
            float x2 = 0.0f, y2 = 0.0f;

            if (!(words >> x2 >> y2))
            {
                continue;
            }

            view.click(button(true, 1, el::mouse_button::left, x, y));
            constexpr int kSteps = 12;

            for (int i = 1; i <= kSteps; ++i)
            {
                const float t = static_cast<float>(i) / kSteps;
                view.drag(button(true, 1, el::mouse_button::left, x + (x2 - x) * t, y + (y2 - y) * t));
            }

            view.click(button(false, 1, el::mouse_button::left, x2, y2));
        }
        else if (verb == "scroll")
        {
            float dy = 0.0f;

            if (words >> dy)
            {
                view.scroll(el::point{0.0f, dy}, el::point{x, y});
            }
        }
    }
}
} // namespace nanoq::ui
