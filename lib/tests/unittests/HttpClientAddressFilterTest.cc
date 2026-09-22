#include <drogon/HttpAppFramework.h>
#include <drogon/HttpClient.h>
#include <drogon/drogon_test.h>
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
                "address-filter-test");
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
                    const std::string response =
                        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: " +
                        std::string(close ? "close" : "keep-alive") +
                        "\r\n\r\nok";
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

DROGON_TEST(HttpClientAddressFilterDefaultsAndReuse)
{
    LocalHttpServer server;
    auto unfiltered = HttpClient::newHttpClient("127.0.0.1", server.port());
    auto response = unfiltered->sendRequest(requestFor(), 2.0);
    REQUIRE(response.first == ReqResult::Ok);
    CHECK(response.second->body() == "ok");

    auto cleared = HttpClient::newHttpClient("127.0.0.1", server.port());
    cleared->setServerAddressFilter(
        [](const trantor::InetAddress &) { return false; });
    cleared->setServerAddressFilter({});
    CHECK(cleared->sendRequest(requestFor(), 2.0).first == ReqResult::Ok);

    auto filtered = HttpClient::newHttpClient("127.0.0.1", server.port());
    std::atomic<size_t> filterCalls{0};
    filtered->setServerAddressFilter(
        [TEST_CTX, &filterCalls, port = server.port()](
            const trantor::InetAddress &address) {
            CHECK(app().getLoop()->isInLoopThread());
            CHECK(address.toIp() == "127.0.0.1");
            CHECK(address.toPort() == port);
            ++filterCalls;
            return true;
        });
    CHECK(filtered->sendRequest(requestFor(), 2.0).first == ReqResult::Ok);
    CHECK(filtered->sendRequest(requestFor(), 2.0).first == ReqResult::Ok);
    CHECK(filterCalls == 1);  // Existing keep-alive connection is reused.
    CHECK(filtered->sendRequest(requestFor("/close"), 2.0).first ==
          ReqResult::Ok);
    CHECK(filtered->sendRequest(requestFor(), 2.0).first == ReqResult::Ok);
    CHECK(filterCalls == 2);  // A new connection checks the address again.
}

DROGON_TEST(HttpClientAddressFilterRejectsBeforeSocketCreation)
{
    const std::vector<HttpClientPtr> clients{
        HttpClient::newHttpClient("127.0.0.1", 9876),
        HttpClient::newHttpClient("::1", 9876),
        HttpClient::newHttpClient("::ffff:127.0.0.1", 9876),
        HttpClient::newHttpClient("http://localhost:9876"),
        HttpClient::newHttpClient("https://localhost:9876")};
    for (const auto &client : clients)
    {
        std::atomic<size_t> filterCalls{0};
        std::atomic<size_t> socketCalls{0};
        client->setSockOptCallback([&socketCalls](int) { ++socketCalls; });
        client->setServerAddressFilter(
            [TEST_CTX, &filterCalls](const trantor::InetAddress &address) {
                CHECK(app().getLoop()->isInLoopThread());
                CHECK(address.toPort() == 9876);
                ++filterCalls;
                return false;
            });
        const auto response = client->sendRequest(requestFor(), 2.0);
        CHECK(response.first == ReqResult::BadServerAddress);
        CHECK(response.second == nullptr);
        CHECK(filterCalls == 1);
        CHECK(socketCalls == 0);
        CHECK(client->requestsBufferSize() == 0);
    }
}

DROGON_TEST(HttpClientAddressFilterExceptionsReject)
{
    auto client = HttpClient::newHttpClient("http://127.0.0.1:9876");
    std::atomic<size_t> socketCalls{0};
    client->setSockOptCallback([&socketCalls](int) { ++socketCalls; });
    client->setServerAddressFilter([](const trantor::InetAddress &) -> bool {
        throw std::runtime_error("policy failure");
    });
    const auto response = client->sendRequest(requestFor(), 2.0);
    CHECK(response.first == ReqResult::BadServerAddress);
    CHECK(response.second == nullptr);
    CHECK(socketCalls == 0);
    CHECK(client->requestsBufferSize() == 0);
}

DROGON_TEST(HttpClientAddressFilterRejectedCallbackCanSendAgain)
{
    LocalHttpServer server;
    auto client = HttpClient::newHttpClient("127.0.0.1", server.port());
    std::atomic<size_t> filterCalls{0};
    client->setServerAddressFilter(
        [&filterCalls](const trantor::InetAddress &) {
            return ++filterCalls > 1;
        });
    std::promise<void> completed;
    client->sendRequest(
        requestFor(),
        [TEST_CTX, client, &completed](ReqResult result,
                                       const HttpResponsePtr &response) {
            CHECK(result == ReqResult::BadServerAddress);
            CHECK(response == nullptr);
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
    CHECK(filterCalls == 2);
}

DROGON_TEST(HttpClientAddressFilterCompletesQueuedRequestsOnce)
{
    auto client = HttpClient::newHttpClient("http://localhost:9876");
    client->setServerAddressFilter(
        [](const trantor::InetAddress &) { return false; });
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
                    0.1);
            }
            // Let request timers expire to detect duplicate completions too.
            app().getLoop()->runAfter(0.2, [&completed]() {
                completed.set_value();
            });
        });
    completed.get_future().get();
    CHECK(completions == 3);
    CHECK(client->requestsBufferSize() == 0);
}

DROGON_TEST(HttpClientAddressFilterBaseDoesNotSilentlyIgnorePolicy)
{
    auto client = HttpClient::newHttpClient("http://127.0.0.1:9876");
    CHECK_NOTHROW(client->HttpClient::setServerAddressFilter({}));
    CHECK_THROWS_AS(client->HttpClient::setServerAddressFilter(
                        [](const trantor::InetAddress &) { return false; }),
                    std::logic_error);
}
