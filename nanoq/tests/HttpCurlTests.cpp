// Linux's HTTP transport (core/src/net/HttpCurl.cpp) against a small server on loopback: what the
// request sends, what comes back, and the two promises the rest of the app leans on, that a
// timeout is honoured and that a cancel returns at once.

#include "net/Http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <map>
#include <string>
#include <thread>

namespace
{
namespace net = guitarfx::net;
using Clock = std::chrono::steady_clock;

int gFailures = 0;

void Check(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::cerr << "[HttpCurlTests] " << what << std::endl;
        ++gFailures;
    }
}

double SecondsSince(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::string Answer(const std::string& status, const std::string& headers, const std::string& body)
{
    return "HTTP/1.1 " + status + "\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n" + headers + "\r\n" + body;
}

/// One thread, one connection at a time: reads a request, hands it to `respond`, writes the answer.
class Server
{
  public:
    /// Text to send; "" sends nothing and holds the connection open, as a stalled server does.
    using Respond = std::function<std::string(const std::string& request)>;

    explicit Server(Respond respond) : mRespond(std::move(respond))
    {
        mListen = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(mListen, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        socklen_t length = sizeof(address);
        getsockname(mListen, reinterpret_cast<sockaddr*>(&address), &length);
        mPort = ntohs(address.sin_port);
        listen(mListen, 8);
        mThread = std::thread([this] { Loop(); });
    }

    ~Server()
    {
        mStopping = true;
        shutdown(mListen, SHUT_RDWR);
        close(mListen);
        mThread.join();
    }

    [[nodiscard]] std::string Url(const std::string& path) const
    {
        return "http://127.0.0.1:" + std::to_string(mPort) + path;
    }

  private:
    void Loop()
    {
        while (!mStopping)
        {
            const int client = accept(mListen, nullptr, nullptr);

            if (client < 0)
            {
                return;
            }

            std::string request;
            char buffer[4096];

            for (;;)
            {
                const auto got = recv(client, buffer, sizeof(buffer), 0);

                if (got <= 0)
                {
                    break;
                }

                request.append(buffer, static_cast<std::size_t>(got));
                const auto end = request.find("\r\n\r\n");

                if (end == std::string::npos)
                {
                    continue;
                }

                // Headers read: the body, if it says it has one.
                const auto at = request.find("Content-Length: ");
                const std::size_t length = at == std::string::npos ? 0 : std::stoul(request.substr(at + 16));

                if (request.size() >= end + 4 + length)
                {
                    break;
                }
            }

            const auto answer = mRespond(request);

            if (answer.empty())
            {
                // Holds it until the client gives up, or the server is taken down.
                while (recv(client, buffer, sizeof(buffer), 0) > 0)
                {
                }
            }
            else
            {
                send(client, answer.data(), answer.size(), MSG_NOSIGNAL);
            }

            close(client);
        }
    }

    Respond mRespond;
    int mListen = -1;
    int mPort = 0;
    std::atomic<bool> mStopping{false};
    std::thread mThread;
};

void TestGetAndHeaders()
{
    std::string seen;
    Server server([&seen](const std::string& request) {
        seen = request;
        return Answer("200 OK", "X-Thing: value  \r\nSet-Cookie: a\r\nSet-Cookie: b\r\n", "hello!");
    });

    net::HttpRequest request;
    request.url = server.Url("/path?q=1");
    request.headers.emplace_back("Accept", "application/json");
    net::CancelToken token;
    const auto response = net::MakePlatformTransport()->Perform(request, token);

    Check(response.status == 200, "status, got " + std::to_string(response.status) + ": " + response.error);
    Check(response.error.empty() && !response.cancelled, "no error on a success");
    Check(response.body == "hello!", "the body, got " + response.body);
    Check(response.Header("x-thing") == "value", "a header, lower-cased and trimmed: " + response.Header("x-thing"));
    Check(response.Header("set-cookie") == "b", "a repeated header keeps the last, as WinHTTP's parse does");
    Check(seen.rfind("GET /path?q=1 HTTP/1.1\r\n", 0) == 0, "the request line: " + seen);
    Check(seen.find("Accept: application/json\r\n") != std::string::npos, "the request's headers");
    Check(seen.find("User-Agent: Soundshed\r\n") != std::string::npos, "the agent, as Windows sends");
}

void TestBodies()
{
    std::string seen;
    Server server([&seen](const std::string& request) {
        seen = request;
        return Answer("201 Created", "", "{}");
    });

    const auto transport = net::MakePlatformTransport();

    for (const std::string method : {"POST", "PUT", "DELETE"})
    {
        net::HttpRequest request;
        request.method = method;
        request.url = server.Url("/thing");
        request.body = R"({"a":1})";
        request.headers.emplace_back("Content-Type", "application/json");
        net::CancelToken token;
        const auto response = transport->Perform(request, token);

        Check(response.status == 201, method + " status: " + response.error);
        Check(seen.rfind(method + " /thing HTTP/1.1\r\n", 0) == 0, method + " request line: " + seen);
        Check(seen.size() >= 7 && seen.substr(seen.size() - 7) == R"({"a":1})", method + " carries the body: " + seen);
        Check(seen.find("Content-Type: application/json\r\n") != std::string::npos, method + " keeps its type");
        Check(seen.find("Expect:") == std::string::npos, method + " does not wait for 100 Continue");
    }

    net::HttpRequest bare;
    bare.method = "POST";
    bare.url = server.Url("/thing");
    bare.body = "x";
    net::CancelToken token;
    (void)transport->Perform(bare, token);
    Check(seen.find("Content-Type:") == std::string::npos, "no form content type is made up for a body");
}

void TestRedirect()
{
    std::string secondRequest;
    Server server([&secondRequest](const std::string& request) {
        if (request.rfind("POST /old", 0) == 0)
        {
            return Answer("302 Found", "Location: /new\r\nX-Hop: first\r\n", "moved");
        }

        secondRequest = request;
        return Answer("200 OK", "X-Hop: last\r\n", "here");
    });

    net::HttpRequest request;
    request.method = "POST";
    request.url = server.Url("/old");
    request.body = "x";
    net::CancelToken token;
    const auto response = net::MakePlatformTransport()->Perform(request, token);

    Check(response.status == 200, "a redirect is followed: " + response.error);
    Check(response.body == "here", "only the last response's body: " + response.body);
    Check(response.Header("x-hop") == "last", "only the last response's headers");
    Check(response.Header("location").empty(), "the redirect's headers are gone");
    Check(secondRequest.rfind("GET /new", 0) == 0, "a POST redirected by a 302 becomes a GET: " + secondRequest);
}

void TestErrorStatusIsAnAnswer()
{
    Server server([](const std::string&) { return Answer("404 Not Found", "", "nope"); });
    net::HttpRequest request;
    request.url = server.Url("/missing");
    net::CancelToken token;
    const auto response = net::MakePlatformTransport()->Perform(request, token);
    Check(response.status == 404 && response.body == "nope", "a 404 is an answer, with its body");
    Check(response.error.empty(), "and no transport error");
}

void TestStallTimesOut()
{
    Server server([](const std::string&) { return std::string(); });
    net::HttpRequest request;
    request.url = server.Url("/stall");
    request.timeoutMs = 1000;
    net::CancelToken token;
    const auto start = Clock::now();
    const auto response = net::MakePlatformTransport()->Perform(request, token);
    const auto took = SecondsSince(start);

    Check(response.status == 0 && !response.cancelled, "a stall is not an answer");
    Check(response.error == "The request timed out", "says so: " + response.error);
    Check(took >= 0.8 && took < 3.5, "at about the time asked for, took " + std::to_string(took));
}

void TestCancelInFlightReturnsAtOnce()
{
    Server server([](const std::string&) { return std::string(); });
    net::HttpRequest request;
    request.url = server.Url("/stall");
    request.timeoutMs = 60000;
    net::CancelToken token;

    std::thread canceller([&token] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        token.Cancel();
    });

    const auto start = Clock::now();
    const auto response = net::MakePlatformTransport()->Perform(request, token);
    const auto took = SecondsSince(start);
    canceller.join();

    Check(response.cancelled && response.status == 0, "a cancel is reported as one");
    Check(response.error == "Cancelled", "and worded as on Windows: " + response.error);
    Check(took < 0.28 + 0.2, "at once, not at the next poll: took " + std::to_string(took));
}

void TestAlreadyCancelled()
{
    Server server([](const std::string&) { return Answer("200 OK", "", "x"); });
    net::HttpRequest request;
    request.url = server.Url("/");
    net::CancelToken token;
    token.Cancel();
    const auto response = net::MakePlatformTransport()->Perform(request, token);
    Check(response.cancelled && response.status == 0, "a request after a cancel never starts");
}

void TestWorkersCutShort()
{
    Server server([](const std::string&) { return std::string(); });
    std::atomic<bool> cut{false};
    std::atomic<bool> started{false};
    const auto start = Clock::now();
    {
        net::HttpWorkers workers(net::MakePlatformTransport(), 2);
        workers.Run([&workers, &cut, &started, url = server.Url("/stall")] {
            net::HttpRequest request;
            request.url = url;
            request.timeoutMs = 60000;
            started = true;
            cut = workers.Perform(request).cancelled;
        });

        while (!started)
        {
            std::this_thread::yield();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    Check(cut, "closing the UI cuts the request short");
    Check(SecondsSince(start) < 1.0, "and is not held up by it");
}

void TestNoServer()
{
    // Bound and closed at once, so nothing is listening there.
    int port = 0;
    {
        Server server([](const std::string&) { return std::string(); });
        const auto url = server.Url("");
        port = std::stoi(url.substr(url.rfind(':') + 1));
    }

    net::HttpRequest request;
    request.url = "http://127.0.0.1:" + std::to_string(port) + "/";
    net::CancelToken token;
    const auto refused = net::MakePlatformTransport()->Perform(request, token);
    Check(refused.status == 0 && refused.error == "Could not connect: check the network",
          "nothing listening: " + refused.error);

    request.url = "not a url at all";
    const auto bad = net::MakePlatformTransport()->Perform(request, token);
    Check(bad.status == 0 && !bad.error.empty(), "a bad URL says something: " + bad.error);
}
} // namespace

int main()
{
    TestGetAndHeaders();
    TestBodies();
    TestRedirect();
    TestErrorStatusIsAnAnswer();
    TestStallTimesOut();
    TestCancelInFlightReturnsAtOnce();
    TestAlreadyCancelled();
    TestWorkersCutShort();
    TestNoServer();

    if (gFailures == 0)
    {
        std::cout << "[HttpCurlTests] passed" << std::endl;
    }

    return gFailures == 0 ? 0 : 1;
}
