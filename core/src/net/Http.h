#pragma once

/**
 * Http.h - HTTP for the native UIs (Soundshed Nano Q, Nano Q), with no UI framework in it.
 *
 * The engine itself makes no requests; the UIs do, for tone sharing and Tone3000. Each platform
 * has a transport of its own (WinHTTP on Windows, NSURLSession on macOS and iOS, libcurl on Linux
 * where the product asks for it); a product that already has an HTTP stack it prefers (JUCE's
 * URL, say) can hand in its own HttpTransport
 * instead. HttpWorkers runs requests off the UI thread and cuts every one short when it is
 * destroyed, so a UI that closes while a slow network keeps a request waiting is not held up by
 * it.
 */

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace guitarfx::net
{
struct HttpRequest
{
    std::string method = "GET"; // GET, POST, PUT, DELETE; any but GET sends `body`
    std::string url;            // absolute, http or https
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeoutMs = 15000; // to connect, and again for the answer
};

struct HttpResponse
{
    int status = 0;                             // 0: no answer (no network, a timeout, or cut short)
    std::string body;                           // bytes, as received
    std::map<std::string, std::string> headers; // names in lower case
    std::string error;                          // why there was no answer, when status is 0
    bool cancelled = false;

    [[nodiscard]] std::string Header(const std::string& lowerCaseName) const
    {
        const auto it = headers.find(lowerCaseName);
        return it != headers.end() ? it->second : std::string();
    }
};

/// Lets a request in flight be abandoned from another thread. A transport registers how to
/// abort what it is doing (close a handle, cancel a task); Cancel() calls it, then and for any
/// request that starts afterwards.
class CancelToken
{
  public:
    void Cancel();

    [[nodiscard]] bool IsCancelled() const noexcept
    {
        return mCancelled.load();
    }

    /// Registers `abort` for the request now starting; returns false (and registers nothing)
    /// when the token is already cancelled. Clear() when the request is done.
    bool SetAbort(std::function<void()> abort);
    void ClearAbort();

  private:
    std::atomic<bool> mCancelled{false};
    std::mutex mMutex;
    std::function<void()> mAbort;
};

/// One request, blocking, on the calling thread.
class HttpTransport
{
  public:
    virtual ~HttpTransport() = default;
    [[nodiscard]] virtual HttpResponse Perform(const HttpRequest& request, CancelToken& cancel) = 0;
};

/// This platform's transport: WinHTTP, NSURLSession, libcurl, or, where there is none, one that answers
/// every request with status 0 and says so.
[[nodiscard]] std::shared_ptr<HttpTransport> MakePlatformTransport();

/// A few worker threads for requests. The destructor cancels every request in flight and waits
/// for the workers, which then return at once; jobs still queued are dropped.
class HttpWorkers
{
  public:
    HttpWorkers(std::shared_ptr<HttpTransport> transport, int threads);
    ~HttpWorkers();

    HttpWorkers(const HttpWorkers&) = delete;
    HttpWorkers& operator=(const HttpWorkers&) = delete;

    /// Runs `job` on a worker; inside it, Perform() makes requests (a job may make several).
    void Run(std::function<void()> job);

    /// One request, blocking, from inside a job. Cut short (status 0, `cancelled`) once the
    /// workers are being destroyed.
    [[nodiscard]] HttpResponse Perform(const HttpRequest& request);

  private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};
} // namespace guitarfx::net
