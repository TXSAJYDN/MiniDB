// HTTP/1.1 socket and session integration tests.
#include <gtest/gtest.h>

#include "../src/base/Buffer.h"
#include "../src/net/EventLoop.h"
#include "../src/net/InetAddress.h"
#include "../src/protocol/HttpParser.h"
#include "../src/protocol/HttpServer.h"
#include "../src/storage/KvStore.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace minidb {
namespace {

using SteadyClock = std::chrono::steady_clock;

class Deadline {
  public:
    explicit Deadline(std::chrono::milliseconds timeout)
        : end_(SteadyClock::now() + timeout) {}

    int remaining_ms() const {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                end_ - SteadyClock::now());
        return remaining.count() > 0 ? static_cast<int>(remaining.count()) : 0;
    }

  private:
    SteadyClock::time_point end_;
};

std::string lower_ascii(std::string value) {
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string trim_ows(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

struct HttpReply {
    int status_code = 0;
    std::string reason;
    std::map<std::string, std::string> headers;
    size_t content_length = 0;
    std::string body;

    bool has_header(std::string_view name) const {
        return headers.find(lower_ascii(std::string(name))) != headers.end();
    }
};

class HttpClient {
  public:
    explicit HttpClient(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                       IPPROTO_TCP);
        if (fd_ < 0) {
            throw std::runtime_error(std::string("socket failed: ") +
                                     std::strerror(errno));
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address),
                      sizeof(address)) < 0 &&
            errno != EINPROGRESS) {
            const int saved = errno;
            Close();
            throw std::runtime_error(std::string("connect failed: ") +
                                     std::strerror(saved));
        }

        Deadline deadline(std::chrono::seconds(5));
        Wait(POLLOUT, deadline, "connect");
        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) < 0 ||
            error != 0) {
            if (error == 0) {
                error = errno;
            }
            Close();
            throw std::runtime_error(std::string("connect error: ") +
                                     std::strerror(error));
        }

        const int one = 1;
        (void)::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    ~HttpClient() {
        Close();
    }
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    void SendAll(std::string_view bytes) {
        SendAll(bytes.data(), bytes.size());
    }

    void SendByteByByte(std::string_view bytes) {
        for (char byte : bytes) {
            SendAll(&byte, 1);
        }
    }

    void ShutdownWrite() {
        if (fd_ >= 0 && ::shutdown(fd_, SHUT_WR) < 0 && errno != ENOTCONN &&
            errno != EPIPE && errno != ECONNRESET) {
            throw std::runtime_error(std::string("shutdown failed: ") +
                                     std::strerror(errno));
        }
    }

    void CloseWithReset() {
        if (fd_ < 0) {
            return;
        }
        linger reset{};
        reset.l_onoff = 1;
        reset.l_linger = 0;
        (void)::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &reset, sizeof(reset));
        Close();
    }

    HttpReply ReadResponse() {
        Deadline deadline(std::chrono::seconds(15));
        return ReadResponseInternal(deadline, 64u * 1024u,
                                    std::chrono::milliseconds(0));
    }

    HttpReply ReadResponseSlowly(size_t chunk_size,
                                 std::chrono::milliseconds delay) {
        Deadline deadline(std::chrono::seconds(20));
        return ReadResponseInternal(deadline, chunk_size, delay);
    }

    void ExpectEof(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        Deadline deadline(timeout);
        while (!eof_) {
            if (!receive_buffer_.empty()) {
                throw std::runtime_error(
                    "received bytes after a final HTTP response");
            }
            ReceiveMore(deadline, 64u * 1024u);
        }
        if (!receive_buffer_.empty()) {
            throw std::runtime_error(
                "received bytes after a final HTTP response");
        }
    }

  private:
    void Close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    void SendAll(const void* data, size_t size) {
        if (size == 0) {
            return;
        }
        const auto* bytes = static_cast<const char*>(data);
        size_t offset = 0;
        Deadline deadline(std::chrono::seconds(10));
        while (offset < size) {
            const ssize_t n =
                ::send(fd_, bytes + offset, size - offset, MSG_NOSIGNAL);
            if (n > 0) {
                offset += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                Wait(POLLOUT, deadline, "send");
                continue;
            }
            throw std::runtime_error(std::string("send failed: ") +
                                     std::strerror(errno));
        }
    }

    void Wait(short events, Deadline& deadline, const char* operation) const {
        for (;;) {
            pollfd entry{fd_, events, 0};
            const int ready = ::poll(&entry, 1, deadline.remaining_ms());
            if (ready > 0) {
                if ((entry.revents & POLLNVAL) != 0) {
                    throw std::runtime_error(std::string(operation) +
                                             " failed: invalid descriptor");
                }
                return;
            }
            if (ready == 0) {
                throw std::runtime_error(std::string(operation) + " timed out");
            }
            if (errno != EINTR) {
                throw std::runtime_error(
                    std::string(operation) +
                    " poll failed: " + std::strerror(errno));
            }
        }
    }

    void ReceiveMore(Deadline& deadline, size_t max_chunk) {
        if (eof_) {
            return;
        }
        for (;;) {
            Wait(POLLIN, deadline, "receive");
            char chunk[65536];
            const size_t wanted = std::min(max_chunk, sizeof(chunk));
            const ssize_t n = ::recv(fd_, chunk, wanted, 0);
            if (n > 0) {
                receive_buffer_.append(chunk, static_cast<size_t>(n));
                return;
            }
            if (n == 0) {
                eof_ = true;
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            throw std::runtime_error(std::string("recv failed: ") +
                                     std::strerror(errno));
        }
    }

    HttpReply ReadResponseInternal(Deadline& deadline, size_t chunk_size,
                                   std::chrono::milliseconds delay) {
        if (chunk_size == 0) {
            chunk_size = 1;
        }

        size_t header_end = std::string::npos;
        for (;;) {
            header_end = receive_buffer_.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                break;
            }
            if (receive_buffer_.size() > 64u * 1024u) {
                throw std::runtime_error("HTTP response header too large");
            }
            const size_t before = receive_buffer_.size();
            ReceiveMore(deadline, chunk_size);
            if (eof_ && receive_buffer_.size() == before) {
                throw std::runtime_error(
                    "peer closed before sending a complete response header");
            }
            if (delay.count() > 0) {
                std::this_thread::sleep_for(delay);
            }
        }

        const std::string block = receive_buffer_.substr(0, header_end);
        const size_t first_line_end = block.find("\r\n");
        if (first_line_end == std::string::npos) {
            throw std::runtime_error("HTTP response has no header lines");
        }

        const std::string status_line = block.substr(0, first_line_end);
        if (status_line.size() < 12 ||
            status_line.compare(0, 9, "HTTP/1.1 ") != 0 ||
            !std::isdigit(static_cast<unsigned char>(status_line[9])) ||
            !std::isdigit(static_cast<unsigned char>(status_line[10])) ||
            !std::isdigit(static_cast<unsigned char>(status_line[11])) ||
            (status_line.size() > 12 && status_line[12] != ' ')) {
            throw std::runtime_error("malformed HTTP status line: " +
                                     status_line);
        }

        HttpReply reply;
        reply.status_code = (status_line[9] - '0') * 100 +
                            (status_line[10] - '0') * 10 +
                            (status_line[11] - '0');
        if (status_line.size() > 13) {
            reply.reason = status_line.substr(13);
        }

        size_t header_begin = first_line_end + 2;
        while (header_begin < block.size()) {
            const size_t header_line_end = block.find("\r\n", header_begin);
            const size_t line_end = header_line_end == std::string::npos
                                        ? block.size()
                                        : header_line_end;
            const std::string line =
                block.substr(header_begin, line_end - header_begin);
            const size_t colon = line.find(':');
            if (colon == std::string::npos || colon == 0) {
                throw std::runtime_error("malformed HTTP response header: " +
                                         line);
            }
            const std::string name = lower_ascii(line.substr(0, colon));
            const std::string value = trim_ows(line.substr(colon + 1));
            if (reply.headers.find(name) != reply.headers.end()) {
                throw std::runtime_error("duplicate HTTP response header: " +
                                         name);
            }
            reply.headers.emplace(name, value);
            if (header_line_end == std::string::npos) {
                break;
            }
            header_begin = header_line_end + 2;
        }

        const auto content_length = reply.headers.find("content-length");
        if (content_length == reply.headers.end()) {
            throw std::runtime_error("HTTP response lacks Content-Length");
        }
        if (reply.headers.find("transfer-encoding") != reply.headers.end()) {
            throw std::runtime_error(
                "HTTP response unexpectedly uses Transfer-Encoding");
        }
        const std::string& length_text = content_length->second;
        const auto parsed = std::from_chars(
            length_text.data(), length_text.data() + length_text.size(),
            reply.content_length);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != length_text.data() + length_text.size()) {
            throw std::runtime_error("invalid response Content-Length: " +
                                     length_text);
        }
        if (reply.content_length > 2u * 1024u * 1024u) {
            throw std::runtime_error("response body exceeds test limit");
        }

        const size_t body_begin = header_end + 4;
        while (receive_buffer_.size() < body_begin + reply.content_length) {
            const size_t before = receive_buffer_.size();
            ReceiveMore(deadline, chunk_size);
            if (eof_ && receive_buffer_.size() == before) {
                throw std::runtime_error(
                    "peer closed before the complete response body");
            }
            if (delay.count() > 0) {
                std::this_thread::sleep_for(delay);
            }
        }

        reply.body = receive_buffer_.substr(body_begin, reply.content_length);
        receive_buffer_.erase(0, body_begin + reply.content_length);
        return reply;
    }

    int fd_ = -1;
    bool eof_ = false;
    std::string receive_buffer_;
};

std::string BuildRequest(
    const std::string& method, const std::string& target,
    const std::string& body = std::string(),
    const std::vector<std::pair<std::string, std::string>>& extra_headers = {},
    bool include_host = true) {
    bool has_content_length = false;
    for (const auto& header : extra_headers) {
        if (lower_ascii(header.first) == "content-length") {
            has_content_length = true;
        }
    }

    std::string request = method + " " + target + " HTTP/1.1\r\n";
    if (include_host) {
        request += "Host: localhost\r\n";
    }
    for (const auto& header : extra_headers) {
        request += header.first + ": " + header.second + "\r\n";
    }
    if (!body.empty() && !has_content_length) {
        request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    request += "\r\n";
    request += body;
    return request;
}

std::string HeaderValue(const HttpReply& reply, const std::string& name) {
    const auto it = reply.headers.find(lower_ascii(name));
    return it == reply.headers.end() ? std::string() : it->second;
}

void ExpectStatus(const HttpReply& reply, int status) {
    EXPECT_EQ(reply.status_code, status)
        << "reason: " << reply.reason << ", body: " << reply.body;
    EXPECT_FALSE(reply.reason.empty());
    EXPECT_TRUE(reply.has_header("content-length"));
    EXPECT_TRUE(reply.has_header("connection"));
    EXPECT_EQ(reply.content_length, reply.body.size());
}

void ExpectAllow(const HttpReply& reply, const std::string& allow) {
    EXPECT_EQ(HeaderValue(reply, "Allow"), allow);
}

void ExpectFinalResponse(HttpClient& client, const std::string& raw,
                         int status) {
    client.SendAll(raw);
    const HttpReply reply = client.ReadResponse();
    ExpectStatus(reply, status);
    EXPECT_EQ(lower_ascii(HeaderValue(reply, "Connection")), "close");
    // Do not wait for EOF when the expectation already failed: a keep-alive
    // session intentionally remains open and would otherwise add a timeout.
    if (reply.status_code != status ||
        lower_ascii(HeaderValue(reply, "Connection")) != "close") {
        return;
    }
    client.ExpectEof();
}

HttpReply ExpectApplicationResponse(HttpClient& client, const std::string& raw,
                                    int status) {
    client.SendAll(raw);
    HttpReply reply = client.ReadResponse();
    ExpectStatus(reply, status);
    EXPECT_EQ(lower_ascii(HeaderValue(reply, "Connection")), "keep-alive");
    return reply;
}

HttpReply Exchange(HttpClient& client, const std::string& request) {
    client.SendAll(request);
    return client.ReadResponse();
}

class HttpSessionIntegrationTest : public ::testing::Test {
  protected:
    void SetUp() override {
        char directory_template[] = "/tmp/minidb-http-integration-XXXXXX";
        char* directory = ::mkdtemp(directory_template);
        if (directory == nullptr) {
            throw std::runtime_error(std::string("mkdtemp failed: ") +
                                     std::strerror(errno));
        }
        temp_dir_ = directory;
        rdb_path_ = temp_dir_ + "/snapshot.rdb";
    }

    void TearDown() override {
        StopServer();
        if (!rdb_path_.empty()) {
            (void)::unlink(rdb_path_.c_str());
        }
        if (!temp_dir_.empty()) {
            (void)::rmdir(temp_dir_.c_str());
        }
    }

    void StartServer(
        std::chrono::milliseconds idle_timeout = std::chrono::seconds(30)) {
        ASSERT_FALSE(server_thread_.joinable());

        auto store = std::make_shared<KvStore>();
        store->set("test_key", "test_value");
        for (int i = 0; i < 100; ++i) {
            store->set("key" + std::to_string(i), "value" + std::to_string(i));
        }
        store_ = store;

        std::promise<uint16_t> ready;
        std::future<uint16_t> port = ready.get_future();
        server_thread_ = std::thread(
            [this, store, idle_timeout, ready = std::move(ready)]() mutable {
                bool published = false;
                try {
                    EventLoop loop;
                    loop_pointer_.store(&loop);
                    InetAddress listen_address(0, true);
                    HttpServer server(&loop, listen_address, rdb_path_);
                    server.set_kv_store(store);
                    server.set_idle_timeout(idle_timeout);
                    server.start();
                    ready.set_value(server.listen_address().port());
                    published = true;
                    loop.loop();
                    loop_pointer_.store(nullptr);
                } catch (...) {
                    loop_pointer_.store(nullptr);
                    if (!published) {
                        ready.set_exception(std::current_exception());
                    }
                }
            });

        ASSERT_EQ(port.wait_for(std::chrono::seconds(5)),
                  std::future_status::ready);
        try {
            port_ = port.get();
        } catch (const std::exception& ex) {
            StopServer();
            FAIL() << "server failed to start: " << ex.what();
        }
        ASSERT_NE(port_, 0u);
    }

    void StopServer() {
        EventLoop* loop = loop_pointer_.exchange(nullptr);
        if (loop != nullptr) {
            loop->quit();
        }
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    std::string temp_dir_;
    std::string rdb_path_;
    std::shared_ptr<KvStore> store_;
    std::thread server_thread_;
    std::atomic<EventLoop*> loop_pointer_{nullptr};
    uint16_t port_ = 0;
};

TEST_F(HttpSessionIntegrationTest, HealthRoutesAndMethodAllowMatrix) {
    StartServer();
    HttpClient client(port_);

    HttpReply reply = Exchange(client, BuildRequest("GET", "/health"));
    ExpectStatus(reply, 200);
    EXPECT_EQ(reply.body, "OK\n");
    EXPECT_EQ(lower_ascii(HeaderValue(reply, "Content-Type")), "text/plain");
    EXPECT_EQ(lower_ascii(HeaderValue(reply, "Connection")), "keep-alive");

    reply = Exchange(client, BuildRequest("POST", "/health"));
    ExpectStatus(reply, 405);
    ExpectAllow(reply, "GET");
    EXPECT_EQ(reply.body, "405 Method Not Allowed\n");

    reply = Exchange(client, BuildRequest("DELETE", "/health"));
    ExpectStatus(reply, 405);
    ExpectAllow(reply, "GET");

    reply = Exchange(client, BuildRequest("GET", "/echo"));
    ExpectStatus(reply, 405);
    ExpectAllow(reply, "POST");

    reply = Exchange(client, BuildRequest("GET", "/save"));
    ExpectStatus(reply, 405);
    ExpectAllow(reply, "POST");

    reply = Exchange(client, BuildRequest("TRACE", "/health"));
    ExpectStatus(reply, 501);
    EXPECT_EQ(reply.body, "501 Not Implemented\n");

    reply = Exchange(client, BuildRequest("GET", "/missing"));
    ExpectStatus(reply, 404);
    EXPECT_EQ(reply.body, "404 Not Found\n");
}

TEST_F(HttpSessionIntegrationTest, KvCrudHandlesBinaryValuesAndQueries) {
    StartServer();
    HttpClient client(port_);

    const std::string binary_value("v\0\xff\x01binary", 10);
    HttpReply reply =
        Exchange(client, BuildRequest("POST", "/kv/bin-key", binary_value));
    ExpectStatus(reply, 200);
    EXPECT_EQ(reply.body, "OK\n");

    reply = Exchange(client, BuildRequest("GET", "/kv/bin-key?ignored=yes"));
    ExpectStatus(reply, 200);
    EXPECT_EQ(reply.body, binary_value);
    EXPECT_EQ(lower_ascii(HeaderValue(reply, "Content-Type")),
              "application/octet-stream");

    reply = Exchange(client, BuildRequest("GET", "/kv/test_key"));
    ExpectStatus(reply, 200);
    EXPECT_EQ(reply.body, "test_value");

    reply = Exchange(client, BuildRequest("DELETE", "/kv/bin-key"));
    ExpectStatus(reply, 200);
    EXPECT_EQ(reply.body, "OK\n");

    reply = Exchange(client, BuildRequest("GET", "/kv/bin-key"));
    ExpectStatus(reply, 404);

    reply = Exchange(client, BuildRequest("DELETE", "/kv/bin-key"));
    ExpectStatus(reply, 404);

    reply = Exchange(client, BuildRequest("GET", "/kv"));
    ExpectStatus(reply, 404);

    HttpClient empty_key(port_);
    const HttpReply empty_key_error = ExpectApplicationResponse(
        empty_key, BuildRequest("POST", "/kv/", "value"), 400);
    EXPECT_EQ(empty_key_error.body, "KV key must not be empty\n");
    reply = Exchange(empty_key, BuildRequest("GET", "/health"));
    ExpectStatus(reply, 200);

    HttpClient oversized_key(port_);
    const HttpReply oversized_key_error = ExpectApplicationResponse(
        oversized_key,
        BuildRequest("POST", "/kv/" + std::string(1025, 'k'), "value"), 400);
    EXPECT_EQ(oversized_key_error.body, "KV key exceeds 1 KiB limit\n");
    reply = Exchange(oversized_key, BuildRequest("GET", "/health"));
    ExpectStatus(reply, 200);
}

TEST_F(HttpSessionIntegrationTest, EchoAndSaveEndpoints) {
    StartServer();
    HttpClient client(port_);

    std::string payload(70u * 1024u, '\0');
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>((i * 37u + 11u) & 0xffu);
    }

    HttpReply reply = Exchange(client, BuildRequest("POST", "/echo", payload));
    ExpectStatus(reply, 200);
    EXPECT_EQ(reply.body, payload);
    EXPECT_EQ(lower_ascii(HeaderValue(reply, "Content-Type")),
              "application/octet-stream");

    reply = Exchange(client, BuildRequest("POST", "/save"));
    ExpectStatus(reply, 200);
    EXPECT_EQ(lower_ascii(HeaderValue(reply, "Content-Type")),
              "application/json");
    EXPECT_NE(reply.body.find("\"status\":\"ok\""), std::string::npos);
    EXPECT_NE(reply.body.find("\"keys\":"), std::string::npos);

    struct stat file_info {};
    ASSERT_EQ(::stat(rdb_path_.c_str(), &file_info), 0);
    EXPECT_GT(file_info.st_size, 0);
}

TEST_F(HttpSessionIntegrationTest, FragmentedInputAndHundredKeepAliveRequests) {
    StartServer();
    HttpClient client(port_);

    // Every request is delivered one byte at a time. The matrix spans the
    // request line, Host, ordinary headers, Content-Length, and a binary-safe
    // body so each parsed field crosses the incremental boundary.
    struct FragmentedCase {
        std::string request;
        int status;
        std::string body;
    };
    const std::vector<FragmentedCase> cases = {
        {BuildRequest("GET", "/health"), 200, "OK\n"},
        {"GET /health HTTP/1.1\r\nHost: localhost\r\n"
         "X-Fragment: one-byte\r\n\r\n",
         200, "OK\n"},
        {BuildRequest("POST", "/echo", "fragmented-body"), 200,
         "fragmented-body"},
        {"POST /kv/fragmented-key HTTP/1.1\r\nHost: localhost\r\n"
         "Content-Length: 5\r\n\r\nvalue",
         200, "OK\n"},
    };
    for (const FragmentedCase& fragmented : cases) {
        client.SendByteByByte(fragmented.request);
        const HttpReply reply = client.ReadResponse();
        ExpectStatus(reply, fragmented.status);
        EXPECT_EQ(reply.body, fragmented.body);
    }

    for (int i = 0; i < 100; ++i) {
        const HttpReply reply = Exchange(
            client, BuildRequest("GET", "/kv/key" + std::to_string(i)));
        ExpectStatus(reply, 200);
        EXPECT_EQ(reply.body, "value" + std::to_string(i))
            << "response index " << i;
        EXPECT_EQ(lower_ascii(HeaderValue(reply, "Connection")), "keep-alive");
    }
}

TEST_F(HttpSessionIntegrationTest, PipelinedHundredRequestsPreserveOrder) {
    StartServer();
    HttpClient client(port_);

    std::string pipeline;
    for (int i = 0; i < 100; ++i) {
        pipeline += BuildRequest("GET", "/kv/key" + std::to_string(i));
    }
    client.SendAll(pipeline);

    for (int i = 0; i < 100; ++i) {
        const HttpReply reply = client.ReadResponse();
        ExpectStatus(reply, 200);
        EXPECT_EQ(reply.body, "value" + std::to_string(i))
            << "pipelined response index " << i;
    }
}

TEST_F(HttpSessionIntegrationTest, MissingDuplicateAndMalformedHostRejected) {
    StartServer();

    {
        HttpClient client(port_);
        ExpectFinalResponse(client, "GET /health HTTP/1.1\r\n\r\n", 400);
    }
    {
        HttpClient client(port_);
        ExpectFinalResponse(client,
                            "GET /health HTTP/1.1\r\nHost: localhost\r\n"
                            "Host: localhost\r\n\r\n",
                            400);
    }
    {
        HttpClient client(port_);
        ExpectFinalResponse(
            client, "GET /health HTTP/1.1\r\nHost: bad host\r\n\r\n", 400);
    }
    {
        HttpClient client(port_);
        ExpectFinalResponse(
            client, "GET /health HTTP/1.1\r\nHost: localhost:65536\r\n\r\n",
            400);
    }

    HttpClient valid(port_);
    const HttpReply reply =
        Exchange(valid, "GET /health HTTP/1.1\r\nHost: localhost:80\r\n\r\n");
    ExpectStatus(reply, 200);
}

TEST_F(HttpSessionIntegrationTest,
       FramingAndUpgradeHeadersProduceContractErrors) {
    StartServer();

    {
        HttpClient client(port_);
        ExpectFinalResponse(client,
                            "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
                            "Content-Length: 3\r\n"
                            "Transfer-Encoding: chunked\r\n\r\nabc",
                            400);
    }
    {
        HttpClient client(port_);
        ExpectFinalResponse(client,
                            "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
                            "Transfer-Encoding: chunked\r\n\r\n",
                            501);
    }
    {
        HttpClient client(port_);
        ExpectFinalResponse(
            client,
            "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
            "Expect: 100-continue\r\nContent-Length: 3\r\n\r\nabc",
            417);
    }
    {
        HttpClient client(port_);
        ExpectFinalResponse(client,
                            "GET /health HTTP/1.1\r\nHost: localhost\r\n"
                            "Upgrade: websocket\r\nConnection: Upgrade\r\n\r\n",
                            400);
    }
}

TEST_F(HttpSessionIntegrationTest, MalformedSyntaxAndVersionMatrix) {
    StartServer();

    {
        HttpClient client(port_);
        ExpectFinalResponse(
            client, "GET /health HTTP/1.1 extra\r\nHost: localhost\r\n\r\n",
            400);
    }
    {
        HttpClient client(port_);
        ExpectFinalResponse(client,
                            "GET /health HTTP/1.1\r\nHost: localhost\r\n"
                            "Malformed-Header\r\n\r\n",
                            400);
    }
    {
        HttpClient client(port_);
        ExpectFinalResponse(
            client, "GET /health HTTP/1.0\r\nHost: localhost\r\n\r\n", 505);
    }
    {
        HttpClient client(port_);
        ExpectFinalResponse(
            client, "GET /health HTTP/2.0\r\nHost: localhost\r\n\r\n", 505);
    }
    {
        HttpClient client(port_);
        ExpectFinalResponse(
            client, "GET /health HTTP/1.x\r\nHost: localhost\r\n\r\n", 400);
    }
    {
        HttpClient client(port_);
        HttpReply reply = ExpectApplicationResponse(
            client, BuildRequest("TRACE", "/health"), 501);
        EXPECT_EQ(reply.body, "501 Not Implemented\n");
        reply = Exchange(client, BuildRequest("GET", "/health"));
        ExpectStatus(reply, 200);
    }
}

TEST_F(HttpSessionIntegrationTest,
       HeaderTargetAndBodyLimitsMapToExactStatuses) {
    StartServer();

    {
        HttpClient client(port_);
        const std::string target(
            static_cast<size_t>(HttpParser::kMaxRequestTargetSize) + 1, 'a');
        ExpectFinalResponse(client, BuildRequest("GET", "/" + target), 414);
    }
    {
        HttpClient client(port_);
        ExpectFinalResponse(
            client,
            "GET /health HTTP/1.1\r\nHost: localhost\r\nX-Large: " +
                std::string(17u * 1024u, 'v') + "\r\n\r\n",
            431);
    }
    {
        HttpClient client(port_);
        std::string request = "GET /health HTTP/1.1\r\nHost: localhost\r\n";
        for (int i = 0; i < 64; ++i) {
            request += "X-Fill-" + std::to_string(i) + ": v\r\n";
        }
        request += "\r\n";
        ExpectFinalResponse(client, request, 431);
    }
    {
        HttpClient client(port_);
        ExpectFinalResponse(client,
                            "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
                            "Content-Length: 1048577\r\n\r\n",
                            413);
    }
}

TEST_F(HttpSessionIntegrationTest, ExactOneMiBAndOversizedBodies) {
    StartServer();
    HttpClient client(port_);

    std::string payload(1024u * 1024u, 'b');
    for (size_t i = 0; i < payload.size(); i += 4096) {
        payload[i] = static_cast<char>('A' + ((i / 4096) % 26));
    }

    HttpReply reply = Exchange(client, BuildRequest("POST", "/echo", payload));
    ExpectStatus(reply, 200);
    EXPECT_EQ(reply.body.size(), payload.size());
    EXPECT_EQ(reply.body, payload);

    reply = Exchange(client, BuildRequest("GET", "/health"));
    ExpectStatus(reply, 200);

    HttpClient oversized(port_);
    ExpectFinalResponse(oversized,
                        "POST /echo HTTP/1.1\r\nHost: localhost\r\n"
                        "Content-Length: 1048577\r\n\r\n",
                        413);
}

TEST_F(HttpSessionIntegrationTest, HeaderCaseAndConnectionCloseSemantics) {
    StartServer();

    HttpClient client(port_);
    HttpReply reply = Exchange(
        client, "POST /echo HTTP/1.1\r\nhOsT: localhost\r\n"
                "cOnNeCtIoN: keep-alive\r\ncOnTeNt-LeNgTh: 5\r\n\r\nhello");
    ExpectStatus(reply, 200);
    EXPECT_EQ(reply.body, "hello");

    reply = Exchange(client, BuildRequest("GET", "/health"));
    ExpectStatus(reply, 200);

    HttpClient close_client(port_);
    reply = Exchange(close_client,
                     "GET /health HTTP/1.1\r\nHost: localhost\r\n"
                     "Connection: keep-alive\r\nConnection: close\r\n\r\n");
    ExpectStatus(reply, 200);
    EXPECT_EQ(lower_ascii(HeaderValue(reply, "Connection")), "close");
    close_client.ExpectEof();
}

TEST_F(HttpSessionIntegrationTest, IdleTimeoutReturns408AndClosesConnection) {
    StartServer(std::chrono::milliseconds(150));

    {
        HttpClient client(port_);
        const HttpReply reply = client.ReadResponse();
        ExpectStatus(reply, 408);
        EXPECT_EQ(reply.body, "Request timed out\n");
        EXPECT_EQ(lower_ascii(HeaderValue(reply, "Connection")), "close");
        client.ExpectEof();
    }

    {
        HttpClient client(port_);
        client.SendAll("GET /hea");
        const HttpReply reply = client.ReadResponse();
        ExpectStatus(reply, 408);
        client.ExpectEof();
    }

    HttpClient later(port_);
    const HttpReply reply = Exchange(later, BuildRequest("GET", "/health"));
    ExpectStatus(reply, 200);
}

TEST_F(HttpSessionIntegrationTest, SlowReadDrainsOneMiBResponseCompletely) {
    StartServer();
    HttpClient client(port_);

    std::string payload(1024u * 1024u, 's');
    for (size_t i = 0; i < payload.size(); i += 8192) {
        payload[i] = static_cast<char>((i / 8192) & 0xff);
    }
    client.SendAll(BuildRequest("POST", "/echo", payload));
    const HttpReply reply =
        client.ReadResponseSlowly(4096, std::chrono::milliseconds(1));
    ExpectStatus(reply, 200);
    EXPECT_EQ(reply.body, payload);
}

TEST_F(HttpSessionIntegrationTest, PeerHupResetAndEpipeDoNotStopServer) {
    StartServer();

    {
        HttpClient half_closed(port_);
        half_closed.SendAll("GET /health HTTP/1.1\r\nHost: lo");
        half_closed.ShutdownWrite();
        half_closed.ExpectEof(std::chrono::seconds(5));
    }

    for (int i = 0; i < 16; ++i) {
        HttpClient resetting(port_);
        std::string requests;
        for (int j = 0; j < 100; ++j) {
            requests += BuildRequest("GET", "/kv/key" + std::to_string(j));
        }
        resetting.SendAll(requests);
        resetting.CloseWithReset();
    }

    HttpClient healthy(port_);
    const HttpReply reply = Exchange(healthy, BuildRequest("GET", "/health"));
    ExpectStatus(reply, 200);
    EXPECT_EQ(reply.body, "OK\n");
}

TEST_F(HttpSessionIntegrationTest,
       CompleteRequestThenShutdownWriteReturnsResponse) {
    StartServer();

    HttpClient client(port_);
    client.SendAll(BuildRequest("GET", "/health"));
    client.ShutdownWrite();

    const HttpReply reply = client.ReadResponse();
    ExpectStatus(reply, 200);
    EXPECT_EQ(reply.body, "OK\n");
    client.ExpectEof(std::chrono::seconds(5));
}

TEST_F(HttpSessionIntegrationTest,
       OneMiBEchoThenShutdownWriteSlowReadGetsCompleteResponse) {
    StartServer();

    HttpClient client(port_);
    std::string payload(1024u * 1024u, 'f');
    for (size_t i = 0; i < payload.size(); i += 4096) {
        payload[i] = static_cast<char>('A' + ((i / 4096) % 26));
    }

    client.SendAll(BuildRequest("POST", "/echo", payload));
    client.ShutdownWrite();

    const HttpReply reply =
        client.ReadResponseSlowly(4096, std::chrono::milliseconds(1));
    ExpectStatus(reply, 200);
    EXPECT_EQ(reply.body.size(), payload.size());
    EXPECT_EQ(reply.body, payload);
    client.ExpectEof(std::chrono::seconds(5));
}

} // namespace
} // namespace minidb
