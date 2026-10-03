#include <drogon/HttpAppFramework.h>
#include <drogon/HttpClient.h>
#include <drogon/drogon_test.h>
#include <trantor/net/EventLoopThread.h>
#include <trantor/net/TcpServer.h>
#include <atomic>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace drogon;

namespace
{
// A local server on an OS-assigned port; no public DNS or external service.
class LocalHttpServer
{
  public:
    LocalHttpServer()
    {
        std::promise<void> ready;
        app().getLoop()->queueInLoop([this, &ready]() {
            server_ = std::make_unique<trantor::TcpServer>(
                app().getLoop(),
                trantor::InetAddress("127.0.0.1", 0),
                "before-connect-test");
            port_ = server_->address().toPort();
            server_->setRecvMessageCallback(
                [](const trantor::TcpConnectionPtr &connection,
                   trantor::MsgBuffer *buffer) {
                    const std::string request(buffer->peek(),
                                              buffer->readableBytes());
                    if (request.find("\r\n\r\n") == std::string::npos)
                        return;
                    buffer->retrieveAll();
                    const bool close = request.find("GET /close ") == 0;
                    std::string body = "ok";
                    if (request.find("GET /host ") == 0)
                    {
                        const auto host = request.find("\r\nhost: ");
                        if (host != std::string::npos)
                        {
                            const auto begin = host + 8;
                            body = request.substr(begin,
                                                  request.find("\r\n", begin) -
                                                      begin);
                        }
                    }
                    const std::string response =
                        "HTTP/1.1 200 OK\r\nContent-Length: " +
                        std::to_string(body.size()) + "\r\nConnection: " +
                        std::string(close ? "close" : "keep-alive") +
                        "\r\n\r\n" + body;
                    connection->send(response);
                    if (close)
                        connection->shutdown();
                });
            server_->start();
            ready.set_value();
        });
        ready.get_future().get();
    }

    ~LocalHttpServer()
    {
        std::promise<void> stopped;
        app().getLoop()->queueInLoop([this, &stopped]() {
            server_->stop();
            server_.reset();
            stopped.set_value();
        });
        stopped.get_future().get();
    }

    uint16_t port() const
    {
        return port_;
    }

  private:
    std::unique_ptr<trantor::TcpServer> server_;
    uint16_t port_{0};
};

HttpRequestPtr requestFor(const std::string &path = "/")
{
    auto request = HttpRequest::newHttpRequest();
    request->setPath(path);
    return request;
}
}  // namespace

DROGON_TEST(HttpClientBeforeConnectDefaultsAndReuse)
{
    LocalHttpServer server;
    auto unfiltered = HttpClient::newHttpClient("127.0.0.1", server.port());
    auto response = unfiltered->sendRequest(requestFor(), 2.0);
    REQUIRE(response.first == ReqResult::Ok);
    CHECK(response.second->body() == "ok");

    auto cleared = HttpClient::newHttpClient("127.0.0.1", server.port());
    cleared->setBeforeConnectCallback(
        [](const HttpClient::ConnectionInfo &) { return false; });
    cleared->setBeforeConnectCallback({});
    CHECK(cleared->sendRequest(requestFor(), 2.0).first == ReqResult::Ok);

    auto client = HttpClient::newHttpClient("127.0.0.1", server.port());
    std::atomic<size_t> hookCalls{0};
    client->setBeforeConnectCallback(
        [TEST_CTX, &hookCalls, port = server.port()](
            const HttpClient::ConnectionInfo &info) {
            CHECK(app().getLoop()->isInLoopThread());
            CHECK(info.host == "127.0.0.1");
            CHECK(info.address.toIp() == "127.0.0.1");
            CHECK(info.address.toPort() == port);
            CHECK(!info.secure);
            ++hookCalls;
            return true;
        });
    CHECK(client->sendRequest(requestFor(), 2.0).first == ReqResult::Ok);
    CHECK(client->sendRequest(requestFor(), 2.0).first == ReqResult::Ok);
    CHECK(hookCalls == 1);  // Existing keep-alive connection is reused.
    CHECK(client->sendRequest(requestFor("/close"), 2.0).first ==
          ReqResult::Ok);
    CHECK(client->sendRequest(requestFor(), 2.0).first == ReqResult::Ok);
    CHECK(hookCalls == 2);  // A new connection is checked again.
}

DROGON_TEST(HttpClientBeforeConnectRejectsBeforeSocketCreation)
{
    struct TestCase
    {
        HttpClientPtr client;
        std::string host;
        uint16_t port;
        bool secure;
    };

    const std::vector<TestCase> cases{
        {HttpClient::newHttpClient("127.0.0.1", 9876),
         "127.0.0.1",
         9876,
         false},
        {HttpClient::newHttpClient("::1", 9876), "::1", 9876, false},
        {HttpClient::newHttpClient("::ffff:127.0.0.1", 9876),
         "::ffff:127.0.0.1",
         9876,
         false},
        {HttpClient::newHttpClient("127.0.0.1", 9876, true),
         "127.0.0.1",
         9876,
         true},
        {HttpClient::newHttpClient("http://localhost:9876"),
         "localhost",
         9876,
         false},
        {HttpClient::newHttpClient("https://localhost:9876"),
         "localhost",
         9876,
         true},
        {HttpClient::newHttpClient("http://LOCALHOST"), "localhost", 80, false},
        {HttpClient::newHttpClient("https://localhost"),
         "localhost",
         443,
         true},
        {HttpClient::newHttpClient("::1", 9876, true), "::1", 9876, true}};
    for (const auto &test : cases)
    {
        const auto &client = test.client;
        std::atomic<size_t> hookCalls{0};
        std::atomic<size_t> socketCalls{0};
        client->setSockOptCallback([&socketCalls](int) { ++socketCalls; });
        client->setBeforeConnectCallback(
            [TEST_CTX, &hookCalls, &test](
                const HttpClient::ConnectionInfo &info) {
                CHECK(app().getLoop()->isInLoopThread());
                CHECK(info.host == test.host);
                CHECK(info.address.toPort() == test.port);
                CHECK(info.secure == test.secure);
                if (test.host == "localhost")
                    CHECK((info.address.toIp() == "127.0.0.1" ||
                           info.address.toIp() == "::1"));
                else
                    CHECK(info.address.toIp() == test.host);
                ++hookCalls;
                return false;
            });
        const auto response = client->sendRequest(requestFor(), 2.0);
        CHECK(response.first == ReqResult::BadServerAddress);
        CHECK(response.second == nullptr);
        CHECK(hookCalls == 1);
        CHECK(socketCalls == 0);
        CHECK(client->requestsBufferSize() == 0);
        CHECK(client->outstandingRequests() == 0);
    }
}

DROGON_TEST(HttpClientBeforeConnectExceptionsReject)
{
    auto client = HttpClient::newHttpClient("http://127.0.0.1:9876");
    std::atomic<size_t> socketCalls{0};
    client->setSockOptCallback([&socketCalls](int) { ++socketCalls; });
    client->setBeforeConnectCallback(
        [](const HttpClient::ConnectionInfo &) -> bool {
            throw std::runtime_error("policy failure");
        });
    const auto response = client->sendRequest(requestFor(), 2.0);
    CHECK(response.first == ReqResult::BadServerAddress);
    CHECK(response.second == nullptr);
    CHECK(socketCalls == 0);
    CHECK(client->requestsBufferSize() == 0);
    CHECK(client->outstandingRequests() == 0);
}

DROGON_TEST(HttpClientBeforeConnectRejectedCallbackCanSendAgain)
{
    LocalHttpServer server;
    auto client = HttpClient::newHttpClient("127.0.0.1", server.port());
    std::atomic<size_t> hookCalls{0};
    client->setBeforeConnectCallback(
        [&hookCalls](const HttpClient::ConnectionInfo &) {
            return ++hookCalls > 1;
        });
    std::promise<void> completed;
    client->sendRequest(
        requestFor(),
        [TEST_CTX, client, &completed](ReqResult result,
                                       const HttpResponsePtr &response) {
            CHECK(result == ReqResult::BadServerAddress);
            CHECK(response == nullptr);
            CHECK(client->requestsBufferSize() == 0);
            CHECK(client->outstandingRequests() == 0);
            client->sendRequest(
                requestFor(),
                [TEST_CTX, &completed](ReqResult retryResult,
                                       const HttpResponsePtr &retryResponse) {
                    CHECK(retryResult == ReqResult::Ok);
                    CHECK(retryResponse != nullptr);
                    completed.set_value();
                },
                2.0);
        },
        2.0);
    completed.get_future().get();
    CHECK(hookCalls == 2);
}

DROGON_TEST(HttpClientBeforeConnectCompletesQueuedRequestsOnce)
{
    auto client = HttpClient::newHttpClient("http://localhost:9876");
    client->setBeforeConnectCallback(
        [](const HttpClient::ConnectionInfo &) { return false; });
    std::atomic<size_t> completions{0};
    std::promise<void> completed;
    app().getLoop()->queueInLoop(
        [TEST_CTX, client, &completions, &completed]() {
            for (size_t i = 0; i < 3; ++i)
            {
                client->sendRequest(
                    requestFor(),
                    [TEST_CTX, &completions](ReqResult result,
                                             const HttpResponsePtr &response) {
                        CHECK(result == ReqResult::BadServerAddress);
                        CHECK(response == nullptr);
                        ++completions;
                    },
                    i == 0 ? 0.0 : 0.1);
            }
            // Let request timers expire to detect duplicate completions too.
            app().getLoop()->runAfter(0.2, [&completed]() {
                completed.set_value();
            });
        });
    completed.get_future().get();
    CHECK(completions == 3);
    CHECK(client->requestsBufferSize() == 0);
    CHECK(client->outstandingRequests() == 0);
}

DROGON_TEST(HttpClientBeforeConnectBaseDoesNotSilentlyIgnorePolicy)
{
    auto client = HttpClient::newHttpClient("http://127.0.0.1:9876");
    CHECK_NOTHROW(client->HttpClient::setBeforeConnectCallback({}));
    CHECK_THROWS_AS(client->HttpClient::setBeforeConnectCallback(
                        [](const HttpClient::ConnectionInfo &) {
                            return false;
                        }),
                    std::logic_error);
}

DROGON_TEST(HttpClientBeforeConnectHostnameAndHostHeader)
{
    LocalHttpServer server;
    auto client = HttpClient::newHttpClient("http://LOCALHOST:" +
                                            std::to_string(server.port()));
    std::atomic<size_t> hookCalls{0};
    client->setBeforeConnectCallback(
        [TEST_CTX, &hookCalls, port = server.port()](
            const HttpClient::ConnectionInfo &info) {
            CHECK(info.host == "localhost");
            CHECK(info.address.toIp() == "127.0.0.1");
            CHECK(info.address.toPort() == port);
            CHECK(!info.secure);
            ++hookCalls;
            return info.host == "localhost";
        });
    auto response = client->sendRequest(requestFor("/host"), 2.0);
    REQUIRE(response.first == ReqResult::Ok);
    CHECK(response.second->body() ==
          "localhost:" + std::to_string(server.port()));
    CHECK(client->sendRequest(requestFor("/close"), 2.0).first ==
          ReqResult::Ok);

    // A new connection must still describe the configured host, not a
    // per-request Host override. Neither header is changed by the hook.
    auto customHostRequest = requestFor("/host");
    customHostRequest->addHeader("host", "request-host.example");
    response = client->sendRequest(customHostRequest, 2.0);
    REQUIRE(response.first == ReqResult::Ok);
    CHECK(response.second->body() == "request-host.example");
    CHECK(hookCalls == 2);
}

DROGON_TEST(HttpClientBeforeConnectCanRejectReconnect)
{
    LocalHttpServer server;
    auto client = HttpClient::newHttpClient("127.0.0.1", server.port());
    std::atomic<size_t> hookCalls{0};
    std::atomic<size_t> socketCalls{0};
    client->setSockOptCallback([&socketCalls](int) { ++socketCalls; });
    client->setBeforeConnectCallback(
        [&hookCalls](const HttpClient::ConnectionInfo &) {
            return ++hookCalls == 1;
        });
    CHECK(client->sendRequest(requestFor("/close"), 2.0).first ==
          ReqResult::Ok);
    const auto response = client->sendRequest(requestFor(), 2.0);
    CHECK(response.first == ReqResult::BadServerAddress);
    CHECK(response.second == nullptr);
    CHECK(hookCalls == 2);
    CHECK(socketCalls == 1);
    CHECK(client->outstandingRequests() == 0);
}

DROGON_TEST(HttpClientBeforeConnectUsesClientLoop)
{
    trantor::EventLoopThread thread;
    thread.run();
    auto client =
        HttpClient::newHttpClient("127.0.0.1", 9876, false, thread.getLoop());
    std::atomic<size_t> hookCalls{0};
    client->setBeforeConnectCallback(
        [TEST_CTX, &hookCalls, loop = thread.getLoop()](
            const HttpClient::ConnectionInfo &) {
            CHECK(loop->isInLoopThread());
            CHECK(!app().getLoop()->isInLoopThread());
            ++hookCalls;
            return false;
        });
    CHECK(client->sendRequest(requestFor(), 2.0).first ==
          ReqResult::BadServerAddress);
    CHECK(hookCalls == 1);
}

DROGON_TEST(HttpClientBeforeConnectSkipsInvalidAddress)
{
    auto client = HttpClient::newHttpClient("0.0.0.0", 9876);
    std::atomic<size_t> hookCalls{0};
    client->setBeforeConnectCallback(
        [&hookCalls](const HttpClient::ConnectionInfo &) {
            ++hookCalls;
            return true;
        });
    CHECK(client->sendRequest(requestFor(), 2.0).first ==
          ReqResult::BadServerAddress);
    CHECK(hookCalls == 0);
    CHECK(client->outstandingRequests() == 0);
}
