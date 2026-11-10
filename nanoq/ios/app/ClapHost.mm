#include "ClapHost.h"

#import <Foundation/Foundation.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

// Nano Q's plugin, linked into the app (nanoq/src/clap_entry.cpp).
extern "C" const clap_plugin_entry_t clap_entry;

namespace nanoq::ios
{
namespace
{
thread_local bool tAudioThread = false;

ClapHost& Self(const clap_host_t* host)
{
    return *static_cast<ClapHost*>(host->host_data);
}

// ── Host extensions ────────────────────────────────────────────────────────────
// What QPlug and Nano Q ask for. A standalone app has no automation, no project to mark dirty
// and no window to resize, so most of it is acknowledging.

void ParamsRescan(const clap_host_t*, clap_param_rescan_flags) {}
void ParamsClear(const clap_host_t*, clap_id, clap_param_clear_flags) {}
void ParamsRequestFlush(const clap_host_t*) {}
const clap_host_params_t kParams = {ParamsRescan, ParamsClear, ParamsRequestFlush};

void GuiResizeHintsChanged(const clap_host_t*) {}
// The view fills the screen; the app decides its size, not the plugin.
bool GuiRequestResize(const clap_host_t*, uint32_t, uint32_t)
{
    return false;
}
bool GuiRequestShow(const clap_host_t*)
{
    return true;
}
bool GuiRequestHide(const clap_host_t*)
{
    return false;
}
void GuiClosed(const clap_host_t*, bool) {}
const clap_host_gui_t kGui = {GuiResizeHintsChanged, GuiRequestResize, GuiRequestShow, GuiRequestHide, GuiClosed};

void StateMarkDirty(const clap_host_t*) {}
const clap_host_state_t kState = {StateMarkDirty};

void LatencyChanged(const clap_host_t*) {}
const clap_host_latency_t kLatency = {LatencyChanged};

void Log(const clap_host_t*, clap_log_severity severity, const char* msg)
{
    NSLog(@"Nano Q [%d] %s", int(severity), msg != nullptr ? msg : "");
}
const clap_host_log_t kLog = {Log};

bool IsMainThread(const clap_host_t*)
{
    return [NSThread isMainThread];
}
bool IsAudioThread(const clap_host_t*)
{
    return tAudioThread;
}
const clap_host_thread_check_t kThreadCheck = {IsMainThread, IsAudioThread};

// ── Events ─────────────────────────────────────────────────────────────────────
// Nothing arrives from a host here (no automation, no MIDI yet), and what the plugin sends
// back, the GUI's parameter gestures, has nowhere to go.

uint32_t NoEventsSize(const clap_input_events_t*)
{
    return 0;
}
const clap_event_header_t* NoEventsGet(const clap_input_events_t*, uint32_t)
{
    return nullptr;
}
const clap_input_events_t kNoEvents = {nullptr, NoEventsSize, NoEventsGet};

bool DropEvent(const clap_output_events_t*, const clap_event_header_t*)
{
    return true;
}
const clap_output_events_t kDropEvents = {nullptr, DropEvent};

constexpr uint32_t kChannels = 2;

// Where the plugin reads its input when the session gives none (no microphone permission).
constexpr uint32_t kSilenceFrames = 4096;
float gSilence[kSilenceFrames] = {};
} // namespace

ClapHost::ClapHost()
{
    mHost.clap_version = CLAP_VERSION;
    mHost.host_data = this;
    mHost.name = "Soundshed Nano Q (iOS)";
    mHost.vendor = "Soundshed";
    mHost.url = "https://github.com/justsomeone-e/soundshed-guitar";
    mHost.version = NANOQ_VERSION;
    mHost.get_extension = GetExtension;
    mHost.request_restart = RequestRestart;
    mHost.request_process = RequestProcess;
    mHost.request_callback = RequestCallback;
}

ClapHost::~ClapHost()
{
    Unload();
}

bool ClapHost::Load()
{
    if (mPlugin != nullptr)
        return true;

    mEntry = &clap_entry;
    NSString* executable = NSBundle.mainBundle.executablePath;
    if (!mEntry->init(executable != nil ? executable.fileSystemRepresentation : ""))
    {
        mEntry = nullptr;
        return false;
    }

    const auto* factory = static_cast<const clap_plugin_factory_t*>(mEntry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (factory == nullptr || factory->get_plugin_count(factory) == 0)
        return false;
    const auto* descriptor = factory->get_plugin_descriptor(factory, 0);
    if (descriptor == nullptr)
        return false;

    mPlugin = factory->create_plugin(factory, &mHost, descriptor->id);
    if (mPlugin == nullptr)
        return false;
    if (!mPlugin->init(mPlugin))
    {
        mPlugin->destroy(mPlugin);
        mPlugin = nullptr;
        return false;
    }
    mGui = static_cast<const clap_plugin_gui_t*>(mPlugin->get_extension(mPlugin, CLAP_EXT_GUI));
    mState = static_cast<const clap_plugin_state_t*>(mPlugin->get_extension(mPlugin, CLAP_EXT_STATE));
    return true;
}

bool ClapHost::SaveState(const char* path)
{
    if (mPlugin == nullptr || mState == nullptr)
        return false;
    // Written beside and renamed over, so a kill mid-write leaves the last good state.
    const std::string temp = std::string(path) + ".tmp";
    FILE* file = std::fopen(temp.c_str(), "wb");
    if (file == nullptr)
        return false;
    clap_ostream_t stream{};
    stream.ctx = file;
    stream.write = [](const clap_ostream_t* s, const void* data, uint64_t size) -> int64_t {
        return int64_t(std::fwrite(data, 1, size_t(size), static_cast<FILE*>(s->ctx)));
    };
    const bool ok = mState->save(mPlugin, &stream);
    const bool closed = std::fclose(file) == 0;
    if (!ok || !closed || std::rename(temp.c_str(), path) != 0)
    {
        std::remove(temp.c_str());
        return false;
    }
    return true;
}

bool ClapHost::LoadState(const char* path)
{
    if (mPlugin == nullptr || mState == nullptr)
        return false;
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
        return false;
    clap_istream_t stream{};
    stream.ctx = file;
    stream.read = [](const clap_istream_t* s, void* data, uint64_t size) -> int64_t {
        auto* f = static_cast<FILE*>(s->ctx);
        const auto n = std::fread(data, 1, size_t(size), f);
        return n == 0 && std::ferror(f) ? -1 : int64_t(n);
    };
    const bool ok = mState->load(mPlugin, &stream);
    std::fclose(file);
    return ok;
}

void ClapHost::Unload()
{
    *mAlive = false;
    DetachView();
    Deactivate();
    if (mPlugin != nullptr)
    {
        mPlugin->destroy(mPlugin);
        mPlugin = nullptr;
    }
    if (mEntry != nullptr)
    {
        mEntry->deinit();
        mEntry = nullptr;
    }
}

bool ClapHost::Activate(double sampleRate, uint32_t maxFrames)
{
    if (mPlugin == nullptr)
        return false;
    if (mActive)
        Deactivate();
    mProcessing = false;
    mActive = mPlugin->activate(mPlugin, sampleRate, 1, maxFrames);
    return mActive;
}

void ClapHost::Deactivate()
{
    if (mPlugin == nullptr || !mActive)
        return;
    // The audio unit has stopped, so no render callback can be running: the audio thread's
    // last call is safely over, and stop_processing can come from here.
    if (mProcessing)
        mPlugin->stop_processing(mPlugin);
    mProcessing = false;
    mPlugin->deactivate(mPlugin);
    mActive = false;
}

void ClapHost::Process(const float* const* in, float* const* out, uint32_t frames)
{
    tAudioThread = true;
    if (mPlugin == nullptr || !mActive)
    {
        for (uint32_t ch = 0; ch < kChannels; ++ch)
            std::memset(out[ch], 0, frames * sizeof(float));
        return;
    }
    if (!mProcessing)
        mProcessing = mPlugin->start_processing(mPlugin);

    const float* silence[kChannels] = {gSilence, gSilence};
    if (in == nullptr && frames > kSilenceFrames)
    {
        for (uint32_t ch = 0; ch < kChannels; ++ch)
            std::memset(out[ch], 0, frames * sizeof(float));
        return;
    }

    clap_audio_buffer_t input{};
    input.data32 = const_cast<float**>(in != nullptr ? in : silence);
    input.channel_count = kChannels;

    clap_audio_buffer_t output{};
    output.data32 = const_cast<float**>(out);
    output.channel_count = kChannels;

    clap_process_t process{};
    process.steady_time = int64_t(mSteadyTime);
    process.frames_count = frames;
    process.transport = nullptr;
    process.audio_inputs = &input;
    process.audio_outputs = &output;
    process.audio_inputs_count = 1;
    process.audio_outputs_count = 1;
    process.in_events = &kNoEvents;
    process.out_events = &kDropEvents;

    mPlugin->process(mPlugin, &process);
    mSteadyTime += frames;
}

bool ClapHost::AttachView(void* parent, uint32_t width, uint32_t height)
{
    if (mPlugin == nullptr || mGui == nullptr || parent == nullptr)
        return false;
    if (!mGuiCreated)
    {
        if (!mGui->is_api_supported(mPlugin, CLAP_WINDOW_API_COCOA, false))
            return false;
        if (!mGui->create(mPlugin, CLAP_WINDOW_API_COCOA, false))
            return false;
        mGuiCreated = true;
    }

    ResizeView(width, height);
    clap_window_t window{};
    window.api = CLAP_WINDOW_API_COCOA;
    window.cocoa = parent;
    if (!mGui->set_parent(mPlugin, &window))
        return false;
    mGui->show(mPlugin);
    return true;
}

void ClapHost::ResizeView(uint32_t width, uint32_t height)
{
    if (mPlugin == nullptr || mGui == nullptr || !mGuiCreated)
        return;
    // The plugin keeps a size inside its limits; the view it made fills the parent whatever
    // it answers, so a refusal only means its layout is clamped.
    uint32_t w = width, h = height;
    if (mGui->can_resize(mPlugin))
        mGui->adjust_size(mPlugin, &w, &h);
    mGui->set_size(mPlugin, w, h);
}

void ClapHost::DetachView()
{
    if (mPlugin != nullptr && mGui != nullptr && mGuiCreated)
    {
        mGui->hide(mPlugin);
        mGui->destroy(mPlugin);
    }
    mGuiCreated = false;
}

const void* ClapHost::GetExtension(const clap_host_t*, const char* id)
{
    if (id == nullptr)
        return nullptr;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
        return &kParams;
    if (std::strcmp(id, CLAP_EXT_GUI) == 0)
        return &kGui;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0)
        return &kState;
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
        return &kLatency;
    if (std::strcmp(id, CLAP_EXT_LOG) == 0)
        return &kLog;
    if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0)
        return &kThreadCheck;
    return nullptr;
}

// A restart (new latency, new ports) needs the audio stopped and the plugin reactivated; Nano
// Q's ports never change, and its latency is reported to nobody here.
void ClapHost::RequestRestart(const clap_host_t*) {}

// The audio unit runs whenever the app does.
void ClapHost::RequestProcess(const clap_host_t*) {}

void ClapHost::RequestCallback(const clap_host_t* host)
{
    auto& self = Self(host);
    if (self.mCallbackQueued.exchange(true))
        return;
    ClapHost* target = &self;
    auto alive = self.mAlive;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!*alive)
            return;
        target->mCallbackQueued = false;
        if (target->mPlugin != nullptr)
            target->mPlugin->on_main_thread(target->mPlugin);
    });
}
} // namespace nanoq::ios
