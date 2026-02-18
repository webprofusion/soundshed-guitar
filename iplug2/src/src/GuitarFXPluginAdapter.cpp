/**
 * GuitarFXPluginAdapter.cpp — iPlug2 thin adapter implementation.
 *
 * All business logic (DSP, presets, message handling) is delegated to
 * PluginController from soundshed-guitar-core. This file only contains
 * iPlug2-specific glue code.
 */

#include "GuitarFXPluginAdapter.h"
#include "IPlug_include_in_plug_src.h"
#include "IPlugPaths.h"
#include "UiBridge.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <shlobj_core.h>

namespace
{
    /// RAII COM initializer for file-dialog threads.
    struct ScopedComInitializer
    {
        HRESULT hr;
        ScopedComInitializer() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
        ~ScopedComInitializer() { if (SUCCEEDED(hr)) CoUninitialize(); }
        ScopedComInitializer(const ScopedComInitializer&) = delete;
        ScopedComInitializer& operator=(const ScopedComInitializer&) = delete;
    };
} // namespace
#endif

// Constants
namespace
{
    constexpr int kNumPrograms = 1;
    constexpr bool kIsStandaloneBuild =
#ifdef APP_API
        true;
#else
        false;
#endif
} // namespace

// Forward declarations for iPlug2 resource helpers
extern HINSTANCE gHINSTANCE;

namespace
{
    std::filesystem::path ResolveDefaultResourceRoot(HMODULE moduleHandle)
    {
        std::vector<std::filesystem::path> candidates;

        WDL_String bundlePath;
        iplug::BundleResourcePath(bundlePath, moduleHandle);
        if (bundlePath.GetLength() > 0)
        {
            const auto root = std::filesystem::path{bundlePath.Get()};
            candidates.push_back(root);
            candidates.push_back(root / "resources");
        }

        WDL_String pluginPath;
        iplug::PluginPath(pluginPath, moduleHandle);
        if (pluginPath.GetLength() > 0)
        {
            const auto pluginDir = std::filesystem::path{pluginPath.Get()};
            candidates.push_back(pluginDir / "resources");
            candidates.push_back(pluginDir / "Resources");
            candidates.push_back(pluginDir / ".." / "Resources");
            candidates.push_back(pluginDir / ".." / "resources");
        }

        if (kIsStandaloneBuild || candidates.empty())
        {
            WDL_String hostPath;
            iplug::HostPath(hostPath, nullptr);
            if (hostPath.GetLength() > 0)
            {
                const auto hostDir = std::filesystem::path{hostPath.Get()};
                candidates.push_back(hostDir / "resources");
                candidates.push_back(hostDir / "Resources");
            }
        }

        for (auto& candidate : candidates)
            candidate = candidate.lexically_normal();

        return guitarfx::ui::ResolveResourceRoot(candidates);
    }
} // namespace

namespace guitarfx
{

// ════════════════════════════════════════════════════════════════════
// Construction
// ════════════════════════════════════════════════════════════════════

GuitarFXPluginAdapter::GuitarFXPluginAdapter(const iplug::InstanceInfo& info)
    : iplug::Plugin(info, iplug::MakeConfig(kParamCount, kNumPrograms)),
      mController(*this)
{
    std::cout << "[Adapter] Constructor called" << std::endl;

    mResourceRoot = ResolveDefaultResourceRoot(::gHINSTANCE);
    if (mResourceRoot.empty())
        std::cerr << "[Adapter] Resource root could not be resolved." << std::endl;
    else
        std::cout << "[Adapter] Resource root: " << mResourceRoot.generic_string() << std::endl;

    InitializeParameters();
    mController.Initialize();

    // Sync initial parameter values from iPlug2 to the controller
    for (int i = 0; i < kParamCount; ++i)
        OnParamChange(i);

#if PLUG_HAS_UI
    SetEnableDevTools(true);
    mEditorInitFunc = [this]() { OnUIOpen(); };
#endif
}

// ════════════════════════════════════════════════════════════════════
// iPlug2 overrides
// ════════════════════════════════════════════════════════════════════

void GuitarFXPluginAdapter::ProcessBlock(iplug::sample** inputs,
                                          iplug::sample** outputs,
                                          int nFrames)
{
    const auto frames = static_cast<std::size_t>(nFrames);

    // Ensure conversion buffers are big enough
    if (mFloatInputLeft.size() < frames)
    {
        mFloatInputLeft.resize(frames);
        mFloatInputRight.resize(frames);
        mFloatOutputLeft.resize(frames);
        mFloatOutputRight.resize(frames);
    }

    // Convert double → float
    if (inputs && inputs[0])
        for (std::size_t i = 0; i < frames; ++i)
            mFloatInputLeft[i] = static_cast<float>(inputs[0][i]);
    if (inputs && inputs[1])
        for (std::size_t i = 0; i < frames; ++i)
            mFloatInputRight[i] = static_cast<float>(inputs[1][i]);

    float* floatIn[2] = { mFloatInputLeft.data(), mFloatInputRight.data() };
    float* floatOut[2] = { mFloatOutputLeft.data(), mFloatOutputRight.data() };

    // Delegate to core controller (handles try-lock, silence on fail, etc.)
    const bool processed = mController.ProcessAudio(floatIn, floatOut, nFrames);

    if (!processed)
    {
        // Controller couldn't lock — output silence
        if (outputs && outputs[0])
            std::fill(outputs[0], outputs[0] + nFrames, 0.0);
        if (outputs && outputs[1])
            std::fill(outputs[1], outputs[1] + nFrames, 0.0);
        return;
    }

    // Convert float → double
    if (outputs && outputs[0])
        for (std::size_t i = 0; i < frames; ++i)
            outputs[0][i] = static_cast<double>(mFloatOutputLeft[i]);
    if (outputs && outputs[1])
        for (std::size_t i = 0; i < frames; ++i)
            outputs[1][i] = static_cast<double>(mFloatOutputRight[i]);
}

void GuitarFXPluginAdapter::OnReset()
{
    mController.Prepare(iplug::Plugin::GetSampleRate(), iplug::Plugin::GetBlockSize());

    const auto blockSize = static_cast<std::size_t>(iplug::Plugin::GetBlockSize());
    mFloatInputLeft.resize(blockSize);
    mFloatInputRight.resize(blockSize);
    mFloatOutputLeft.resize(blockSize);
    mFloatOutputRight.resize(blockSize);
}

void GuitarFXPluginAdapter::OnIdle()
{
    // Drain the main-thread deferred execution queue
    {
        std::vector<std::function<void()>> pending;
        {
            std::lock_guard<std::mutex> lock(mMainThreadQueueMutex);
            pending.swap(mMainThreadQueue);
        }
        for (auto& fn : pending)
        {
            if (fn) fn();
        }
    }

    mController.OnIdle();

    // Handle UI reload timeout (framework-specific, stays in adapter)
    if (mUIReloadInProgress && !mUIContentLoaded)
    {
        if (std::chrono::steady_clock::now() > mUIReloadDeadline)
        {
            mUIReloadInProgress = false;
            if (mUIReloadAttempts < 3)
                LoadWebViewContent(true);
        }
    }
}

void* GuitarFXPluginAdapter::OpenWindow(void* pParent)
{
    mParentWindow = pParent;
    mUIVisible = true;
    mUIContentLoaded = false;
    mUIReloadInProgress = false;
    mUIReloadAttempts = 0;
    return iplug::WebViewEditorDelegate::OpenWindow(pParent);
}

void GuitarFXPluginAdapter::CloseWindow()
{
    iplug::WebViewEditorDelegate::CloseWindow();
    OnUIClose();
}

void GuitarFXPluginAdapter::OnUIOpen()
{
    std::cout << "[Adapter] OnUIOpen" << std::endl;
    mUIVisible = true;
    LoadWebViewContent(false);
#ifdef _WIN32
    ApplyWindowIcon();
#endif
}

void GuitarFXPluginAdapter::OnUIClose()
{
    mUIContentLoaded = false;
    mUIVisible = false;
    mUIReloadInProgress = false;
    mUIReloadAttempts = 0;
#ifdef _WIN32
    ReleaseWindowIcon();
#endif
}

void GuitarFXPluginAdapter::OnWebContentLoaded()
{
    iplug::WebViewEditorDelegate::OnWebContentLoaded();
    mUIContentLoaded = true;
    mUIReloadInProgress = false;
    mController.OnWebContentLoaded();
}

void GuitarFXPluginAdapter::OnParentWindowResize(int width, int height)
{
    iplug::WebViewEditorDelegate::OnParentWindowResize(width, height);

    const bool nowVisible = width > 1 && height > 1;
    if (nowVisible && !mUIVisible)
    {
        LoadWebViewContent(true);
    }
    mUIVisible = nowVisible;
}

bool GuitarFXPluginAdapter::SerializeState(iplug::IByteChunk& chunk) const
{
    const std::string stateJson = mController.SerializeState();
    return chunk.PutStr(stateJson.c_str());
}

int GuitarFXPluginAdapter::UnserializeState(const iplug::IByteChunk& chunk, int startPos)
{
    WDL_String stateStr;
    int position = chunk.GetStr(stateStr, startPos);
    if (position < 0) return startPos;

    mController.DeserializeState(stateStr.Get());

    auto json = nlohmann::json::parse(stateStr.Get(), nullptr, false);
    if (json.is_object() && json.contains("parameters") && json["parameters"].is_array())
    {
        int idx = 0;
        for (const auto& value : json["parameters"])
        {
            if (idx >= kParamCount) break;
            if (value.is_number())
            {
                if (auto* param = GetParam(idx))
                {
                    param->Set(value.get<double>());
                    OnParamChange(idx);
                }
            }
            idx++;
        }
    }
    return position;
}

void GuitarFXPluginAdapter::OnParamChange(int paramIdx)
{
    const auto* param = GetParam(paramIdx);
    if (!param) return;
    mController.OnParamChange(paramIdx, param->Value());
}

bool GuitarFXPluginAdapter::OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData)
{
    if (msgTag == -1 && dataSize > 0 && pData != nullptr)
    {
        std::string message(reinterpret_cast<const char*>(pData), dataSize);
        mController.HandleUIMessage(message);
        return true;
    }
    return false;
}

void GuitarFXPluginAdapter::OnMessageFromWebView(const char* jsonStr)
{
    if (!jsonStr) return;

    auto json = nlohmann::json::parse(jsonStr, nullptr, false);

    // Handle double-encoded JSON strings
    if (!json.is_discarded() && json.is_string())
    {
        std::string innerStr = json.get<std::string>();
        json = nlohmann::json::parse(innerStr, nullptr, false);
    }

    // Custom messages with a "type" field go to the controller
    if (!json.is_discarded() && json.is_object() && json.contains("type"))
    {
        mController.HandleUIMessage(json.dump());
        return;
    }

    // Standard iPlug2 protocol messages (SPVFUI etc.) go to base class
    iplug::Plugin::OnMessageFromWebView(jsonStr);
}

#ifdef VST3_API
Steinberg::tresult PLUGIN_API GuitarFXPluginAdapter::initialize(FUnknown* context)
{
    return iplug::Plugin::initialize(context);
}
#endif

// ════════════════════════════════════════════════════════════════════
// IPluginHost implementation
// ════════════════════════════════════════════════════════════════════

void GuitarFXPluginAdapter::SendMessageToUI(const std::string& jsonMessage)
{
    const auto script = guitarfx::ui::BuildIPlugReceiveScript(jsonMessage);
    EvaluateJavaScript(script.c_str());
}

void GuitarFXPluginAdapter::BrowseFileAsync(
    BrowseFileType type,
    const std::string& title,
    std::function<void(const BrowseFileResult&)> callback)
{
#ifdef _WIN32
    // Run file dialog on a background thread to avoid blocking UI
    std::thread([type, title, callback]()
    {
        ScopedComInitializer com;
        BrowseFileResult result;

        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&dialog));
        if (FAILED(hr)) { callback(result); return; }

        // Set file type filters based on BrowseFileType
        std::vector<COMDLG_FILTERSPEC> filters;
        switch (type)
        {
            case BrowseFileType::NAMModel:
                filters = {{ L"NAM Models", L"*.nam" }, { L"All Files", L"*.*" }};
                break;
            case BrowseFileType::IRFile:
                filters = {{ L"WAV Files", L"*.wav" }, { L"All Files", L"*.*" }};
                break;
            case BrowseFileType::PresetFile:
                filters = {{ L"JSON Presets", L"*.json" }, { L"All Files", L"*.*" }};
                break;
            case BrowseFileType::ImageFile:
                filters = {{ L"Images", L"*.png;*.jpg;*.jpeg;*.svg" }, { L"All Files", L"*.*" }};
                break;
            case BrowseFileType::AudioFile:
                filters = {{ L"Audio Files", L"*.wav;*.mp3;*.flac;*.ogg" }, { L"All Files", L"*.*" }};
                break;
            case BrowseFileType::ArchiveFile:
                filters = {{ L"Preset Archives", L"*.soundshed.preset;*.soundshed.presets;*.zip" }, { L"All Files", L"*.*" }};
                break;
            default:
                filters = {{ L"All Files", L"*.*" }};
                break;
        }
        dialog->SetFileTypes(static_cast<UINT>(filters.size()), filters.data());

        // Set title
        std::wstring wtitle(title.begin(), title.end());
        dialog->SetTitle(wtitle.c_str());

        hr = dialog->Show(nullptr);
        if (SUCCEEDED(hr))
        {
            Microsoft::WRL::ComPtr<IShellItem> item;
            hr = dialog->GetResult(&item);
            if (SUCCEEDED(hr))
            {
                PWSTR filePath = nullptr;
                hr = item->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
                if (SUCCEEDED(hr) && filePath)
                {
                    result.path = std::filesystem::path(filePath);
                    result.success = true;
                    CoTaskMemFree(filePath);
                }
            }
        }

        callback(result);
    }).detach();
#else
    // macOS/Linux: not yet implemented for iPlug2 adapter
    BrowseFileResult result;
    callback(result);
#endif
}

void GuitarFXPluginAdapter::SaveFileAsync(
    BrowseFileType type,
    const std::string& title,
    const std::string& defaultName,
    std::function<void(const BrowseFileResult&)> callback)
{
#ifdef _WIN32
    std::thread([type, title, defaultName, callback]()
    {
        ScopedComInitializer com;
        BrowseFileResult result;

        Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;
        HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&dialog));
        if (FAILED(hr)) { callback(result); return; }

        auto normalizedDefaultName = defaultName;
        std::transform(normalizedDefaultName.begin(), normalizedDefaultName.end(), normalizedDefaultName.begin(),
            [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

        const auto hasSuffix = [&normalizedDefaultName](std::string_view suffix)
        {
            return normalizedDefaultName.size() >= suffix.size()
                && normalizedDefaultName.compare(normalizedDefaultName.size() - suffix.size(), suffix.size(), suffix) == 0;
        };

        std::vector<COMDLG_FILTERSPEC> filters;
        switch (type)
        {
            case BrowseFileType::PresetFile:
                filters = {{ L"JSON Files", L"*.json" }};
                break;
            case BrowseFileType::ArchiveFile:
                if (hasSuffix(".soundshed.preset"))
                    filters = {{ L"Preset Archive", L"*.soundshed.preset" }};
                else if (hasSuffix(".soundshed.presets"))
                    filters = {{ L"Preset Archives", L"*.soundshed.presets" }};
                else if (hasSuffix(".zip"))
                    filters = {{ L"ZIP Archives", L"*.zip" }};
                else
                    filters = {{ L"Preset Archive", L"*.soundshed.preset" }};
                break;
            default:
                filters = {{ L"All Files", L"*.*" }};
                break;
        }
        dialog->SetFileTypes(static_cast<UINT>(filters.size()), filters.data());

        std::wstring wtitle(title.begin(), title.end());
        dialog->SetTitle(wtitle.c_str());

        std::wstring wname(defaultName.begin(), defaultName.end());
        dialog->SetFileName(wname.c_str());

        hr = dialog->Show(nullptr);
        if (SUCCEEDED(hr))
        {
            Microsoft::WRL::ComPtr<IShellItem> item;
            hr = dialog->GetResult(&item);
            if (SUCCEEDED(hr))
            {
                PWSTR filePath = nullptr;
                hr = item->GetDisplayName(SIGDN_FILESYSPATH, &filePath);
                if (SUCCEEDED(hr) && filePath)
                {
                    result.path = std::filesystem::path(filePath);
                    result.success = true;
                    CoTaskMemFree(filePath);
                }
            }
        }

        callback(result);
    }).detach();
#else
    BrowseFileResult result;
    callback(result);
#endif
}

void GuitarFXPluginAdapter::RunOnMainThread(std::function<void()> fn)
{
    if (!fn) return;

    // Enqueue for execution during the next OnIdle() tick on the main thread.
    // OnIdle() is called by the iPlug2 host on the main/UI thread.
    std::lock_guard<std::mutex> lock(mMainThreadQueueMutex);
    mMainThreadQueue.push_back(std::move(fn));
}

std::filesystem::path GuitarFXPluginAdapter::GetUserDataPath() const
{
#ifdef _WIN32
    wchar_t* appDataPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)))
    {
        auto path = std::filesystem::path(appDataPath) / "Soundshed Guitar";
        CoTaskMemFree(appDataPath);
        return path;
    }
#endif
    // Fallback: use user home (match FileSystem::ResolveSettingsDirectory layout)
#ifdef __APPLE__
    auto home = std::filesystem::path(std::getenv("HOME") ? std::getenv("HOME") : ".");
    return home / "Library" / "Soundshed Guitar";
#else
    auto home = std::filesystem::path(std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : ".");
    return home / ".config" / "Soundshed Guitar";
#endif
}

std::filesystem::path GuitarFXPluginAdapter::GetBundledAssetsPath() const
{
    return mResourceRoot;
}

double GuitarFXPluginAdapter::GetSampleRate() const
{
    return iplug::Plugin::GetSampleRate();
}

int GuitarFXPluginAdapter::GetBlockSize() const
{
    return iplug::Plugin::GetBlockSize();
}

void GuitarFXPluginAdapter::OpenAudioPreferences()
{
#ifdef APP_API
    // In the iPlug2 standalone app, show the system preferences dialog.
    // The iPlug2 APP wrapper exposes no direct API for this; use the
    // host-specific mechanism or a platform dialog.
    // On Windows, open the Sound control panel.
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", L"mmsys.cpl", nullptr, nullptr, SW_SHOW);
#endif
#endif
}

void GuitarFXPluginAdapter::NotifyStateChanged()
{
    InformHostOfPresetChange();
}

double GuitarFXPluginAdapter::GetHostTempo() const
{
    if (kIsStandaloneBuild) return 120.0;
    const double tempo = GetTempo();
    return tempo > 0.0 ? tempo : 120.0;
}

bool GuitarFXPluginAdapter::IsHostPlaying() const
{
    if (kIsStandaloneBuild) return false;
    return GetTransportIsRunning();
}

bool GuitarFXPluginAdapter::IsStandalone() const
{
    return kIsStandaloneBuild;
}

// ════════════════════════════════════════════════════════════════════
// Private helpers
// ════════════════════════════════════════════════════════════════════

void GuitarFXPluginAdapter::InitializeParameters()
{
    GetParam(kParamInputTrim)->InitDouble("Input Trim", 0.0, -24.0, 12.0, 0.1, "dB");
    GetParam(kParamOutputTrim)->InitDouble("Output Trim", 0.0, -24.0, 12.0, 0.1, "dB");
    GetParam(kParamDrive)->InitDouble("Drive", 0.5, 0.0, 1.0, 0.01);
    GetParam(kParamTone)->InitDouble("Tone Tilt", 0.5, 0.0, 1.0, 0.01);
    GetParam(kParamGateEnabled)->InitBool("Noise Gate", false);
    GetParam(kParamGateThreshold)->InitDouble("Gate Threshold", -60.0, -80.0, -20.0, 0.1, "dB");
    GetParam(kParamMix)->InitDouble("Mix", 1.0, 0.0, 1.0, 0.01);
    GetParam(kParamDoublerEnabled)->InitBool("Doubler", false);
    GetParam(kParamDoublerDelay)->InitDouble("Doubler Delay", 6.0, 0.5, 50.0, 0.1, "ms");
    GetParam(kParamTranspose)->InitInt("Transpose", 0, -12, 12, "st");
    GetParam(kParamIRQuality)->InitEnum("IR Quality", 1, 4, "", iplug::IParam::kFlagsNone,
        "", "Economy", "Standard", "High", "Full");
    GetParam(kParamEQEnabled)->InitBool("EQ", false);
    GetParam(kParamEQLowGain)->InitDouble("EQ Low Gain", 0.0, -12.0, 12.0, 0.1, "dB");
    GetParam(kParamEQLowFreq)->InitDouble("EQ Low Freq", 100.0, 20.0, 500.0, 1.0, "Hz");
    GetParam(kParamEQLowMidGain)->InitDouble("EQ Low-Mid Gain", 0.0, -12.0, 12.0, 0.1, "dB");
    GetParam(kParamEQLowMidFreq)->InitDouble("EQ Low-Mid Freq", 500.0, 100.0, 2000.0, 1.0, "Hz");
    GetParam(kParamEQLowMidQ)->InitDouble("EQ Low-Mid Q", 1.0, 0.1, 10.0, 0.1);
    GetParam(kParamEQHighMidGain)->InitDouble("EQ High-Mid Gain", 0.0, -12.0, 12.0, 0.1, "dB");
    GetParam(kParamEQHighMidFreq)->InitDouble("EQ High-Mid Freq", 2000.0, 500.0, 8000.0, 1.0, "Hz");
    GetParam(kParamEQHighMidQ)->InitDouble("EQ High-Mid Q", 1.0, 0.1, 10.0, 0.1);
    GetParam(kParamEQHighGain)->InitDouble("EQ High Gain", 0.0, -12.0, 12.0, 0.1, "dB");
    GetParam(kParamEQHighFreq)->InitDouble("EQ High Freq", 8000.0, 2000.0, 16000.0, 1.0, "Hz");
}

void GuitarFXPluginAdapter::LoadWebViewContent(bool forceReload)
{
    if (mUIReloadInProgress) return;
    if (!forceReload && mUIContentLoaded) return;

    std::filesystem::path htmlPath = mResourceRoot / "ui" / "index.html";
    if (!std::filesystem::exists(htmlPath))
    {
        std::cerr << "[Adapter] index.html not found at: " << htmlPath.generic_string() << std::endl;
        return;
    }

    mUIReloadInProgress = true;
    mUIReloadAttempts++;
    mUIReloadDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    LoadFile(htmlPath.string().c_str(), nullptr);
    EnableScroll(false);
}

std::filesystem::path GuitarFXPluginAdapter::ResolveResourceRoot() const
{
    return ResolveDefaultResourceRoot(::gHINSTANCE);
}

#ifdef _WIN32
void GuitarFXPluginAdapter::ApplyWindowIcon()
{
    if (mWindowIconApplied || !mParentWindow) return;

    HWND hwnd = reinterpret_cast<HWND>(mParentWindow);
    if (!IsWindow(hwnd)) return;

    // Only set icon for standalone top-level windows
    if (!kIsStandaloneBuild)
    {
        if (GetParent(hwnd) != nullptr || GetWindow(hwnd, GW_OWNER) != nullptr)
            return;
    }

    const std::filesystem::path iconPath = mResourceRoot / "ui" / "images" / "icon.png";
    if (!std::filesystem::exists(iconPath)) return;

    // Load PNG via WIC and create HICON
    Microsoft::WRL::ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&wicFactory));
    if (FAILED(hr)) return;

    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    hr = wicFactory->CreateDecoderFromFilename(iconPath.c_str(), nullptr,
                                                GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder);
    if (FAILED(hr)) return;

    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return;

    Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
    hr = wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) return;

    hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return;

    UINT width = 0, height = 0;
    converter->GetSize(&width, &height);
    if (width == 0 || height == 0) return;

    std::vector<BYTE> pixels(width * height * 4);
    hr = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data());
    if (FAILED(hr)) return;

    // Pre-multiply alpha for icon
    for (UINT i = 0; i < width * height; ++i)
    {
        BYTE* px = &pixels[i * 4];
        const BYTE a = px[3];
        px[0] = static_cast<BYTE>((px[0] * a) / 255);
        px[1] = static_cast<BYTE>((px[1] * a) / 255);
        px[2] = static_cast<BYTE>((px[2] * a) / 255);
    }

    HBITMAP hbmp = CreateBitmap(static_cast<int>(width), static_cast<int>(height), 1, 32, pixels.data());
    if (!hbmp) return;

    HBITMAP hMask = CreateBitmap(static_cast<int>(width), static_cast<int>(height), 1, 1, nullptr);
    if (!hMask) { DeleteObject(hbmp); return; }

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmMask = hMask;
    ii.hbmColor = hbmp;
    HICON hIcon = CreateIconIndirect(&ii);
    DeleteObject(hbmp);
    DeleteObject(hMask);

    if (hIcon)
    {
        mWindowIconLarge = reinterpret_cast<void*>(
            SendMessage(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon)));
        mWindowIconSmall = reinterpret_cast<void*>(
            SendMessage(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIcon)));
        mWindowIconApplied = true;
    }
}

void GuitarFXPluginAdapter::ReleaseWindowIcon()
{
    if (!mWindowIconApplied) return;
    if (mWindowIconLarge)
    {
        DestroyIcon(reinterpret_cast<HICON>(mWindowIconLarge));
        mWindowIconLarge = nullptr;
    }
    if (mWindowIconSmall)
    {
        DestroyIcon(reinterpret_cast<HICON>(mWindowIconSmall));
        mWindowIconSmall = nullptr;
    }
    mWindowIconApplied = false;
}
#endif

} // namespace guitarfx
