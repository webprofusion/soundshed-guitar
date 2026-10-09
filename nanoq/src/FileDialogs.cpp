#include "FileDialogs.h"

#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
 #include <shobjidl.h>
 #include <windows.h>
#endif

namespace nanoq
{
#if defined(_WIN32)
namespace
{
std::wstring Widen(const std::string& utf8)
{
    if (utf8.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

struct Filter
{
    std::wstring name;
    std::wstring spec;
};

std::vector<Filter> FiltersFor(guitarfx::BrowseFileType type)
{
    using T = guitarfx::BrowseFileType;
    switch (type)
    {
    case T::NAMModel:
        return {{L"Neural amp models (*.nam)", L"*.nam"}};
    case T::IRFile:
        return {{L"Impulse responses (*.wav)", L"*.wav"}};
    case T::PresetFile:
        return {{L"Presets (*.json;*.soundshed)", L"*.json;*.soundshed"}};
    case T::ImageFile:
        return {{L"Images", L"*.png;*.jpg;*.jpeg;*.webp"}};
    case T::AudioFile:
        return {{L"Audio", L"*.wav;*.mp3;*.flac;*.ogg;*.aif;*.aiff"}};
    case T::ArchiveFile:
        return {{L"Archives (*.zip)", L"*.zip"}};
    case T::PluginFile:
        return {{L"Plugins", L"*.vst3;*.dll"}};
    default:
        return {};
    }
}

// Runs one IFileDialog; `save` picks the save flavour, `folder` the folder picker.
guitarfx::BrowseFileResult RunDialog(bool save, guitarfx::BrowseFileType type, const std::string& title,
                                     const std::string& defaultName)
{
    guitarfx::BrowseFileResult result;
    const bool coInitialised = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));

    IFileDialog* dialog = nullptr;
    const HRESULT created = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                             CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(created) && dialog != nullptr)
    {
        DWORD options = 0;
        dialog->GetOptions(&options);
        options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
        if (type == guitarfx::BrowseFileType::Folder)
            options |= FOS_PICKFOLDERS;
        dialog->SetOptions(options);

        const auto wideTitle = Widen(title);
        if (!wideTitle.empty())
            dialog->SetTitle(wideTitle.c_str());

        const auto filters = FiltersFor(type);
        if (!filters.empty())
        {
            std::vector<COMDLG_FILTERSPEC> specs;
            specs.reserve(filters.size());
            for (const auto& f : filters)
                specs.push_back({f.name.c_str(), f.spec.c_str()});
            dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
        }

        if (save && !defaultName.empty())
        {
            const auto name = Widen(defaultName);
            dialog->SetFileName(name.c_str());
        }

        if (SUCCEEDED(dialog->Show(nullptr)))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr)
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr)
                {
                    result.success = true;
                    result.path = std::filesystem::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }

    if (coInitialised)
        CoUninitialize();
    return result;
}
} // namespace

void ShowOpenDialog(guitarfx::BrowseFileType type, const std::string& title,
                    std::function<void(const guitarfx::BrowseFileResult&)> done)
{
    std::thread([=, done = std::move(done)] { done(RunDialog(false, type, title, {})); }).detach();
}

void ShowSaveDialog(guitarfx::BrowseFileType type, const std::string& title, const std::string& defaultName,
                    std::function<void(const guitarfx::BrowseFileResult&)> done)
{
    std::thread([=, done = std::move(done)] { done(RunDialog(true, type, title, defaultName)); }).detach();
}
#else
void ShowOpenDialog(guitarfx::BrowseFileType, const std::string&,
                    std::function<void(const guitarfx::BrowseFileResult&)> done)
{
    done({});
}

void ShowSaveDialog(guitarfx::BrowseFileType, const std::string&, const std::string&,
                    std::function<void(const guitarfx::BrowseFileResult&)> done)
{
    done({});
}
#endif
} // namespace nanoq
