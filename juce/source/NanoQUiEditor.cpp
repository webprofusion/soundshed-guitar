#include "NanoQUiEditor.h"

#include "net/Http.h"
#include "ui/EmbeddedUi.h"
#include "ui/UiLink.h"

#include <mutex>
#include <string>
#include <vector>

// ── HTTP ────────────────────────────────────────────────────────────────────

#if !JUCE_WINDOWS
/// The UI's HTTP through JUCE's URL, which the WebView build already relies on. Windows keeps
/// the UI's own WinHTTP; macOS and Linux would otherwise have none here (core/src/net, whose
/// NSURLSession transport needs Objective-C++ enabled and whose libcurl one is opt-in).
class JuceHttpTransport final : public guitarfx::net::HttpTransport
{
  public:
    guitarfx::net::HttpResponse Perform(const guitarfx::net::HttpRequest& request,
                                        guitarfx::net::CancelToken& cancel) override
    {
        guitarfx::net::HttpResponse response;
        const bool sendsBody = request.method != "GET";
        juce::URL url(juce::String::fromUTF8(request.url.c_str()));

        if (sendsBody)
        {
            url = url.withPOSTData(juce::MemoryBlock(request.body.data(), request.body.size()));
        }

        juce::String headers;

        for (const auto& [name, value] : request.headers)
        {
            headers << juce::String::fromUTF8(name.c_str()) << ": " << juce::String::fromUTF8(value.c_str()) << "\r\n";
        }

        juce::WebInputStream stream(url, sendsBody);
        stream.withExtraHeaders(headers)
            .withConnectionTimeout(request.timeoutMs)
            .withCustomRequestCommand(juce::String(request.method))
            .withNumRedirectsToFollow(5);

        if (!cancel.SetAbort([&stream] { stream.cancel(); }))
        {
            response.cancelled = true;
            return response;
        }

        const bool connected = stream.connect(nullptr);

        if (connected)
        {
            juce::MemoryOutputStream body;
            body.writeFromInputStream(stream, -1);
            response.body.assign(static_cast<const char*>(body.getData()), body.getDataSize());
            response.status = stream.getStatusCode();

            const auto& answer = stream.getResponseHeaders();

            for (int i = 0; i < answer.size(); ++i)
            {
                response.headers[answer.getAllKeys()[i].toLowerCase().toStdString()] =
                    answer.getAllValues()[i].toStdString();
            }
        }

        cancel.ClearAbort();
        response.cancelled = cancel.IsCancelled();

        if (response.status == 0)
        {
            response.error = response.cancelled ? "Cancelled" : (connected ? "No answer" : "Could not connect");
        }

        return response;
    }
};
#endif

// ── Linux: the UI's own X11 connection ──────────────────────────────────────

#if JUCE_LINUX || JUCE_BSD
/// Elements' X11 views read a display connection of their own, which JUCE's loop never looks
/// at (as a CLAP host is asked to, through posix-fd-support): JUCE watches it for them, and a
/// timer runs their animations and deferred redraws. One for every editor open.
class NanoQUiEventPump final : private juce::Timer
{
  public:
    NanoQUiEventPump() : mFd(nanoq::ui::EmbeddedUi::EventFd())
    {
        if (mFd >= 0)
        {
            juce::LinuxEventLoop::registerFdCallback(mFd, [](int) { nanoq::ui::EmbeddedUi::PumpEvents(); });
        }

        startTimerHz(60);
    }

    ~NanoQUiEventPump() override
    {
        stopTimer();

        if (mFd >= 0)
        {
            juce::LinuxEventLoop::unregisterFdCallback(mFd);
        }
    }

  private:
    void timerCallback() override
    {
        nanoq::ui::EmbeddedUi::PumpEvents();
    }

    int mFd = -1;
};
#endif

// ── The engine's end ────────────────────────────────────────────────────────

/// Nano Q's UI on the processor's WebView bridge: requests go in as the page's messages do,
/// and what the controller sends the page is kept for the UI's next pump.
class NanoQUiEditor::Link final : public nanoq::ui::UiLink
{
  public:
    explicit Link(PluginProcessorAdapter& processor) : mProcessor(processor)
    {
        // The processor calls this on the message thread, which is also the UI's.
        mProcessor.setWebMessageCallback([this](const juce::String& message) {
            std::scoped_lock lock(mMutex);

            if (mAttached && mOutbox.size() < kMaxOutbox)
            {
                mOutbox.push_back(message.toStdString());
            }
        });
    }

    ~Link() override
    {
        mProcessor.setWebMessageCallback(nullptr);
    }

    void SetViewAttached(bool attached) override
    {
        std::scoped_lock lock(mMutex);
        mAttached = attached;

        if (!attached)
        {
            mOutbox.clear();
        }
    }

    std::vector<std::string> TakeMessagesForView() override
    {
        std::vector<std::string> out;
        std::scoped_lock lock(mMutex);
        out.swap(mOutbox);
        return out;
    }

    void SendRequest(std::string json) override
    {
        // Handled after the UI event that sent it, as a WebView message is, and dropped if the
        // processor has gone by then.
        juce::MessageManager::callAsync(
            [processor = juce::WeakReference<PluginProcessorAdapter>(&mProcessor), json = std::move(json)] {
                if (auto* p = processor.get())
                {
                    p->handleWebMessage(juce::String(json));
                }
            });
    }

    std::filesystem::path AssetRoot() const override
    {
        return mProcessor.GetBundledAssetsPath();
    }

    std::optional<std::filesystem::path> ResolveLayoutImageFile(const std::string& imageId) override
    {
        return mProcessor.getController().ResolveLayoutImageFile(imageId);
    }

    std::shared_ptr<guitarfx::net::HttpTransport> MakeHttpTransport() override
    {
#if JUCE_WINDOWS
        return nullptr; // the UI's own WinHTTP
#else
        return std::make_shared<JuceHttpTransport>();
#endif
    }

  private:
    static constexpr size_t kMaxOutbox = 4096; // as Nano Q's engine

    PluginProcessorAdapter& mProcessor;
    std::mutex mMutex;
    std::vector<std::string> mOutbox;
    bool mAttached = false;
};

// ── Where the child window goes ─────────────────────────────────────────────

/// What juce::HWNDComponent watches, for a window HWNDComponent cannot own: it destroys the
/// window it is given when it lets go, and the Elements view destroys its own.
class NanoQUiEditor::Placement final : public juce::ComponentMovementWatcher
{
  public:
    explicit Placement(NanoQUiEditor& editor) : juce::ComponentMovementWatcher(&editor), mEditor(editor)
    {
    }

    using juce::ComponentMovementWatcher::componentMovedOrResized;
    using juce::ComponentMovementWatcher::componentVisibilityChanged;

    void componentMovedOrResized(bool, bool) override
    {
        mEditor.placeChildWindow();
    }

    void componentPeerChanged() override
    {
        mEditor.attachToPeer();
    }

    void componentVisibilityChanged() override
    {
        mEditor.attachToPeer();
    }

  private:
    NanoQUiEditor& mEditor;
};

// ── The editor ──────────────────────────────────────────────────────────────

NanoQUiEditor::NanoQUiEditor(PluginProcessorAdapter& processor)
    : SoundshedEditorBase(processor, SizeLimits{640, 400, 8192, 8192, 1280, 800}),
      mLink(std::make_unique<Link>(processor))
{
    setOpaque(true);
#if JUCE_LINUX || JUCE_BSD
    mEventPump = std::make_shared<juce::SharedResourcePointer<NanoQUiEventPump>>();
#endif
    applyInitialEditorSize();
    mPlacement = std::make_unique<Placement>(*this);

    if (isShowing())
    {
        attachToPeer();
    }
}

NanoQUiEditor::~NanoQUiEditor()
{
    mPlacement.reset();
    mUi.reset();
}

void NanoQUiEditor::paint(juce::Graphics& g)
{
    // Only ever seen for the moment before the child window covers it.
    g.fillAll(juce::Colour(0xff1c1d21));
}

void NanoQUiEditor::resized()
{
    // The child window follows through Placement, which hears of the move as well.
    rememberEditorSize();
}

void NanoQUiEditor::attachToPeer()
{
    auto* peer = getPeer();

    if (peer != mPeer)
    {
        mUi.reset();
        mPeer = peer;

        if (mPeer != nullptr)
        {
            mUi = std::make_unique<nanoq::ui::EmbeddedUi>(*mLink, mPeer->getNativeHandle());
        }
    }

    placeChildWindow();

    if (mUi != nullptr)
    {
        // Nothing async to wait for: the view draws on its first paint.
        notifyReadyToShow();
    }
}

void NanoQUiEditor::placeChildWindow()
{
    if (mUi == nullptr || mPeer == nullptr)
    {
        return;
    }

#if JUCE_MAC
    // A Cocoa view is placed in points, as JUCE measures.
    const auto area = mPeer->getAreaCoveredBy(*this);
#else
    const auto area =
        (mPeer->getAreaCoveredBy(*this).toFloat() * mPeer->getPlatformScaleFactor()).getSmallestIntegerContainer();
#endif
    mUi->Place(area.getX(), area.getY(), area.getWidth(), area.getHeight(), isShowing());
}
