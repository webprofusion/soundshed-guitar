// The Linux transport: libcurl, for a product that asks for it (GUITARFX_UICLIENT_CURL), which
// follows redirects, speaks TLS with the system's certificates and takes its proxy from the
// environment (https_proxy, no_proxy). It runs curl's multi interface rather than the blocking
// call, because that can be woken: cancelling returns at once, where curl_easy_perform would
// wait for its next progress callback.

#include "net/Http.h"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace guitarfx::net
{
namespace
{
/// How long one wait for the network lasts when nothing happens. curl's own low-speed check only
/// runs when it is called, so this is also how late a stalled request can notice.
constexpr int kPollMs = 250;

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Describe(CURLcode code)
{
    switch (code)
    {
    case CURLE_OPERATION_TIMEDOUT:
        return "The request timed out";
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        return "Could not find the server: check the network";
    case CURLE_COULDNT_CONNECT:
        return "Could not connect: check the network";
    case CURLE_PEER_FAILED_VERIFICATION:
        // Most likely on a Pi: it has no battery-backed clock, so the date is wrong until it
        // reaches a time server.
        return "Could not verify the server: check the date and time";
    default:
        return "Network error " + std::to_string(static_cast<int>(code)) + ": " + curl_easy_strerror(code);
    }
}

/// One header line. curl hands them over with their line ending, and hands over every response
/// of a redirect chain: a status line starts a new one, so only the last is kept (as WinHTTP
/// reports only the last).
std::size_t OnHeader(char* data, std::size_t size, std::size_t count, void* user)
{
    auto& response = *static_cast<HttpResponse*>(user);
    const std::size_t length = size * count;
    std::string line(data, length);

    if (line.rfind("HTTP/", 0) == 0)
    {
        response.headers.clear();
    }
    else if (const auto colon = line.find(':'); colon != std::string::npos)
    {
        auto value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);
        response.headers[Lower(line.substr(0, colon))] = value;
    }

    return length;
}

/// Bytes of the body. A redirect's own body is not delivered (curl skips it when following).
std::size_t OnBody(char* data, std::size_t size, std::size_t count, void* user)
{
    auto& response = *static_cast<HttpResponse*>(user);
    response.body.append(data, size * count);
    return size * count;
}

/// What a cancel from another thread reaches. CancelToken may call an abort it copied just after
/// the request finished, so the handle is forgotten, under the lock, before it is freed.
struct Wake
{
    std::mutex mutex;
    CURLM* multi = nullptr;

    void Poke()
    {
        std::lock_guard<std::mutex> lock(mutex);

        if (multi != nullptr)
        {
            curl_multi_wakeup(multi);
        }
    }

    void Forget()
    {
        std::lock_guard<std::mutex> lock(mutex);
        multi = nullptr;
    }
};

class CurlTransport final : public HttpTransport
{
  public:
    HttpResponse Perform(const HttpRequest& request, CancelToken& cancel) override
    {
        HttpResponse response;

        // Before any thread makes a request; a static is initialised once, whoever comes first.
        // Never cleaned up: libcurl outlives any one transport, and may be another library's too.
        static const bool started = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
        CURL* easy = started ? curl_easy_init() : nullptr;
        CURLM* multi = started ? curl_multi_init() : nullptr;
        curl_slist* headers = nullptr;
        auto wake = std::make_shared<Wake>();

        if (easy == nullptr || multi == nullptr)
        {
            response.error = "Networking could not start";
        }
        else
        {
            headers = Configure(easy, request, response);
            curl_multi_add_handle(multi, easy);
            wake->multi = multi;

            if (cancel.SetAbort([wake] { wake->Poke(); }))
            {
                Run(multi, easy, cancel, response, request);
                cancel.ClearAbort();
            }
            else
            {
                response.cancelled = true;
                response.error = "Cancelled";
            }

            wake->Forget();
            curl_multi_remove_handle(multi, easy);
        }

        curl_slist_free_all(headers);

        if (easy != nullptr)
        {
            curl_easy_cleanup(easy);
        }

        if (multi != nullptr)
        {
            curl_multi_cleanup(multi);
        }

        return response;
    }

  private:
    /// Sets the request up; the list it returns stays in use until the request is done.
    static curl_slist* Configure(CURL* easy, const HttpRequest& request, HttpResponse& response)
    {
        const bool secure = request.url.rfind("https://", 0) == 0;
        curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(easy, CURLOPT_USERAGENT, "Soundshed");
        curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
        // As WinHTTP's default policy: a redirect never takes a secure request back to plain http.
        curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, secure ? "https" : "http,https");
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(easy, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
        // The workers are threads: no alarm() around name lookups.
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        // Otherwise freeing the handle waits for a name lookup in progress, which is how a
        // cancelled request with the network down would hold up closing the UI for many seconds.
        curl_easy_setopt(easy, CURLOPT_QUICK_EXIT, 1L);

        // As the header says: this long to connect, and again for the answer. A long download
        // is fine for as long as bytes keep coming, so there is no limit on the whole transfer,
        // only on a stall. curl measures a stall in whole seconds.
        const int timeout = std::max(1000, request.timeoutMs);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout));
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, static_cast<long>((timeout + 999) / 1000));

        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, OnHeader);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, &response);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, OnBody);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &response);

        const std::string method = request.method.empty() ? std::string("GET") : request.method;
        bool hasContentType = false;
        curl_slist* headers = nullptr;

        for (const auto& [name, value] : request.headers)
        {
            hasContentType = hasContentType || Lower(name) == "content-type";
            // curl reads "Name:" as "send no such header", and "Name;" as an empty one.
            const auto line = value.empty() ? name + ";" : name + ": " + value;
            headers = curl_slist_append(headers, line.c_str());
        }

        if (method == "HEAD")
        {
            curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
        }
        else if (method != "GET")
        {
            curl_easy_setopt(easy, CURLOPT_POST, 1L);
            curl_easy_setopt(easy, CURLOPT_POSTFIELDS, request.body.data());
            curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body.size()));

            // Plain POST stays curl's own: a custom method would be kept across a 302, which
            // should turn it into a GET, as it does with WinHTTP.
            if (method != "POST")
            {
                curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method.c_str());
            }

            // Two things curl adds for a body that the other transports don't: a form's content
            // type, and a wait for "100 Continue".
            if (!hasContentType)
            {
                headers = curl_slist_append(headers, "Content-Type:");
            }

            headers = curl_slist_append(headers, "Expect:");
        }

        curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);
        return headers;
    }

    static void Run(CURLM* multi, CURL* easy, CancelToken& cancel, HttpResponse& response, const HttpRequest& request)
    {
        int running = 1;
        CURLMcode problem = CURLM_OK;

        while (running > 0 && problem == CURLM_OK && !cancel.IsCancelled())
        {
            problem = curl_multi_perform(multi, &running);

            if (running > 0 && problem == CURLM_OK)
            {
                problem = curl_multi_poll(multi, nullptr, 0, kPollMs, nullptr);
            }
        }

        CURLcode result = problem == CURLM_OK ? CURLE_OK : CURLE_FAILED_INIT;
        int queued = 0;

        while (const CURLMsg* message = curl_multi_info_read(multi, &queued))
        {
            if (message->msg == CURLMSG_DONE)
            {
                result = message->data.result;
            }
        }

        if (cancel.IsCancelled() || running > 0 || result != CURLE_OK)
        {
            response.body.clear();
            response.headers.clear();
            response.cancelled = cancel.IsCancelled();
            response.error = response.cancelled              ? std::string("Cancelled")
                             : result == CURLE_URL_MALFORMAT ? "Not a URL: " + request.url
                                                             : Describe(result);
            return;
        }

        long status = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
        response.status = static_cast<int>(status);
    }
};
} // namespace

std::shared_ptr<HttpTransport> MakePlatformTransport()
{
    return std::make_shared<CurlTransport>();
}
} // namespace guitarfx::net
