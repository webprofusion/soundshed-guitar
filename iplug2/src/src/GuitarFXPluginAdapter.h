#pragma once

/**
 * GuitarFXPluginAdapter — iPlug2 thin adapter.
 *
 * Implements IPluginHost and delegates all business logic to PluginController.
 * This replaces the original monolithic GuitarFXPlugin class.
 */

#include "config.h"
#include "IPlug_include_in_plug_hdr.h"

#include "IPluginHost.h"
#include "PluginController.h"

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace guitarfx
{

class GuitarFXPluginAdapter final : public iplug::Plugin,
                                     public IPluginHost
{
public:
    explicit GuitarFXPluginAdapter(const iplug::InstanceInfo& info);

    // ── iPlug2 overrides ───────────────────────────────────────────
    void ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames) override;
    void OnReset() override;
    void OnIdle() override;
    void* OpenWindow(void* pParent) override;
    void CloseWindow() override;
    void OnUIOpen() override;
    void OnUIClose() override;
    void OnWebContentLoaded() override;
    void OnParentWindowResize(int width, int height) override;
    bool SerializeState(iplug::IByteChunk& chunk) const override;
    int UnserializeState(const iplug::IByteChunk& chunk, int startPos) override;
    void OnParamChange(int paramIdx) override;
    bool OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData) override;
    void OnMessageFromWebView(const char* jsonStr) override;

#ifdef VST3_API
    Steinberg::tresult PLUGIN_API initialize(FUnknown* context) override;
#endif

    // ── IPluginHost implementation ─────────────────────────────────
    void SendMessageToUI(const std::string& jsonMessage) override;
    void BrowseFileAsync(BrowseFileType type,
                         const std::string& title,
                         std::function<void(const BrowseFileResult&)> callback) override;
    void SaveFileAsync(BrowseFileType type,
                       const std::string& title,
                       const std::string& defaultName,
                       std::function<void(const BrowseFileResult&)> callback) override;
    void RunOnMainThread(std::function<void()> fn) override;
    [[nodiscard]] std::filesystem::path GetUserDataPath() const override;
    [[nodiscard]] std::filesystem::path GetBundledAssetsPath() const override;
    [[nodiscard]] double GetSampleRate() const override;
    [[nodiscard]] int GetBlockSize() const override;
    void OpenAudioPreferences() override;
    void NotifyStateChanged() override;
    [[nodiscard]] double GetHostTempo() const override;
    [[nodiscard]] bool IsHostPlaying() const override;

    // ── Accessors ──────────────────────────────────────────────────
    [[nodiscard]] PluginController& GetController() { return mController; }

    // ── Parameter enum (must match PluginController::ParameterId) ──
    enum ParameterId
    {
        kParamInputTrim = 0,
        kParamOutputTrim,
        kParamDrive,
        kParamTone,
        kParamGateEnabled,
        kParamGateThreshold,
        kParamMix,
        kParamDoublerEnabled,
        kParamDoublerDelay,
        kParamTranspose,
        kParamIRQuality,
        kParamEQEnabled,
        kParamEQLowGain,
        kParamEQLowFreq,
        kParamEQLowMidGain,
        kParamEQLowMidFreq,
        kParamEQLowMidQ,
        kParamEQHighMidGain,
        kParamEQHighMidFreq,
        kParamEQHighMidQ,
        kParamEQHighGain,
        kParamEQHighFreq,
        kParamCount
    };

private:
    void InitializeParameters();
    void LoadWebViewContent(bool forceReload);
    [[nodiscard]] std::filesystem::path ResolveResourceRoot() const;

#ifdef _WIN32
    void ApplyWindowIcon();
    void ReleaseWindowIcon();
#endif

    // ── State ──────────────────────────────────────────────────────
    PluginController mController;

    std::filesystem::path mResourceRoot;

    // Float conversion buffers (iPlug2 uses double, core uses float)
    std::vector<float> mFloatInputLeft;
    std::vector<float> mFloatInputRight;
    std::vector<float> mFloatOutputLeft;
    std::vector<float> mFloatOutputRight;

    // WebView UI state
    bool mUIContentLoaded = false;
    bool mUIVisible = false;
    bool mUIReloadInProgress = false;
    int mUIReloadAttempts = 0;
    std::chrono::steady_clock::time_point mUIReloadDeadline{};
    void* mParentWindow = nullptr;

#ifdef _WIN32
    void* mWindowIconLarge = nullptr;
    void* mWindowIconSmall = nullptr;
    bool mWindowIconApplied = false;
#endif
};

} // namespace guitarfx

// Keep backward compatibility for references using the old name
// Note: The alias is also inside guitarfx namespace for PLUG_CLASS_NAME compatibility
namespace guitarfx { using GuitarFXPlugin = GuitarFXPluginAdapter; }
using GuitarFXPlugin = guitarfx::GuitarFXPluginAdapter;
