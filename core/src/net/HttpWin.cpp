// The Windows transport: WinHTTP, which follows redirects, speaks TLS with the system's
// certificates and uses the system's proxy settings. Cancelling closes the request's handle,
// which makes the blocking call in progress return at once.

#include "net/Http.h"

#ifndef NOMINMAX
    #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>

namespace guitarfx::net
{
namespace
{
std::wstring Widen(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string Narrow(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }

    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), length, nullptr,
                        nullptr);
    return narrow;
}

std::string Lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Describe(DWORD code)
{
    switch (code)
    {
    case ERROR_WINHTTP_TIMEOUT:
        return "The request timed out";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        return "Could not find the server: check the network";
    case ERROR_WINHTTP_CANNOT_CONNECT:
        return "Could not connect: check the network";
    case ERROR_WINHTTP_OPERATION_CANCELLED:
        return "Cancelled";
    default:
        return "Network error " + std::to_string(code);
    }
}

/// The session handle, shared by every request; closed handles below it are the requests'.
struct Session
{
    HINTERNET handle = nullptr;

    Session()
    {
        handle = WinHttpOpen(L"Sepia", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0);

        if (handle == nullptr) // before Windows 8.1 there is no automatic proxy
        {
            handle = WinHttpOpen(L"Sepia", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
        }

        if (handle != nullptr)
        {
            DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
            protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
            WinHttpSetOption(handle, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
        }
    }

    ~Session()
    {
        if (handle != nullptr)
        {
            WinHttpCloseHandle(handle);
        }
    }
};

class WinHttpTransport final : public HttpTransport
{
  public:
    HttpResponse Perform(const HttpRequest& request, CancelToken& cancel) override
    {
        HttpResponse response;

        if (mSession.handle == nullptr)
        {
            response.error = "Networking could not start";
            return response;
        }

        const auto url = Widen(request.url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);

        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts))
        {
            response.error = "Not a URL: " + request.url;
            return response;
        }

        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;

        HINTERNET connection = WinHttpConnect(mSession.handle, host.c_str(), parts.nPort, 0);

        if (connection == nullptr)
        {
            response.error = Describe(GetLastError());
            return response;
        }

        const auto method = Widen(request.method.empty() ? std::string("GET") : request.method);
        HINTERNET handle = WinHttpOpenRequest(connection, method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                              WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);

        if (handle == nullptr)
        {
            response.error = Describe(GetLastError());
            WinHttpCloseHandle(connection);
            return response;
        }

        // Cancel closes the request handle from the other thread: the call blocked on it fails
        // with ERROR_WINHTTP_OPERATION_CANCELLED. The handle is closed once, by whoever is first.
        auto closed = std::make_shared<std::atomic<bool>>(false);
        const auto closeRequest = [handle, closed] {
            if (!closed->exchange(true))
            {
                WinHttpCloseHandle(handle);
            }
        };

        if (!cancel.SetAbort(closeRequest))
        {
            closeRequest();
            WinHttpCloseHandle(connection);
            response.cancelled = true;
            response.error = "Cancelled";
            return response;
        }

        const int timeout = std::max(1000, request.timeoutMs);
        WinHttpSetTimeouts(handle, timeout, timeout, timeout, timeout);

        std::wstring headers;

        for (const auto& [name, value] : request.headers)
        {
            headers += Widen(name + ": " + value + "\r\n");
        }

        const bool hasBody = request.method != "GET" && request.method != "HEAD";
        auto* body = hasBody ? const_cast<char*>(request.body.data()) : nullptr;
        const DWORD bodyLength = hasBody ? static_cast<DWORD>(request.body.size()) : 0;

        bool ok = WinHttpSendRequest(handle, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                                     headers.empty() ? 0 : static_cast<DWORD>(-1), body, bodyLength, bodyLength, 0) &&
                  WinHttpReceiveResponse(handle, nullptr);

        if (ok)
        {
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            response.status = static_cast<int>(status);
            ReadHeaders(handle, response);

            for (;;)
            {
                DWORD available = 0;

                if (!WinHttpQueryDataAvailable(handle, &available))
                {
                    ok = false;
                    break;
                }

                if (available == 0)
                {
                    break;
                }

                const auto offset = response.body.size();
                response.body.resize(offset + available);
                DWORD read = 0;

                if (!WinHttpReadData(handle, response.body.data() + offset, available, &read))
                {
                    ok = false;
                    break;
                }

                response.body.resize(offset + read);
            }
        }

        if (!ok)
        {
            const DWORD code = GetLastError();
            response.status = 0;
            response.body.clear();
            response.cancelled = code == ERROR_WINHTTP_OPERATION_CANCELLED || cancel.IsCancelled();
            response.error = response.cancelled ? std::string("Cancelled") : Describe(code);
        }

        cancel.ClearAbort();
        closeRequest();
        WinHttpCloseHandle(connection);
        return response;
    }

  private:
    static void ReadHeaders(HINTERNET handle, HttpResponse& response)
    {
        DWORD size = 0;
        WinHttpQueryHeaders(handle, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                            WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);

        if (size == 0)
        {
            return;
        }

        std::wstring raw(size / sizeof(wchar_t), L'\0');

        if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(),
                                 &size, WINHTTP_NO_HEADER_INDEX))
        {
            return;
        }

        const auto text = Narrow(raw);
        std::size_t start = 0;

        while (start < text.size())
        {
            auto end = text.find("\r\n", start);

            if (end == std::string::npos)
            {
                end = text.size();
            }

            const auto line = text.substr(start, end - start);

            if (const auto colon = line.find(':'); colon != std::string::npos)
            {
                auto value = line.substr(colon + 1);
                value.erase(0, value.find_first_not_of(' '));
                response.headers[Lower(line.substr(0, colon))] = value;
            }

            start = end + 2;
        }
    }

    Session mSession;
};
} // namespace

std::shared_ptr<HttpTransport> MakePlatformTransport()
{
    return std::make_shared<WinHttpTransport>();
}
} // namespace guitarfx::net
