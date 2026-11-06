#include <gtest/gtest.h>

#include "../src/net/EventLoop.h"
#include "../src/net/InetAddress.h"
#include "../src/net/TcpServer.h"
#include "../src/protocol/KvProtocol.h"
#include "../src/protocol/KvSession.h"
#include "../src/storage/KvStore.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
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

    int RemainingMs() const {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                end_ - SteadyClock::now());
        return remaining.count() > 0 ? static_cast<int>(remaining.count()) : 0;
    }

  private:
    SteadyClock::time_point end_;
};

class KvClient {
  public:
    KvClient() = default;
    ~KvClient() {
        Close();
    }
    KvClient(const KvClient&) = delete;
    KvClient& operator=(const KvClient&) = delete;

    void ConnectTo(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
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
            throw std::runtime_error(std::string("connect failed: ") +
                                     std::strerror(errno));
        }

        pollfd entry{fd_, POLLOUT, 0};
        Deadline deadline(std::chrono::seconds(5));
        for (;;) {
            const int ready = ::poll(&entry, 1, deadline.RemainingMs());
            if (ready > 0) {
                break;
            }
            if (ready == 0) {
                throw std::runtime_error("connect timed out");
            }
            if (errno != EINTR) {
                throw std::runtime_error(std::string("poll failed: ") +
                                         std::strerror(errno));
            }
        }

        int error = 0;
        socklen_t length = sizeof(error);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &length) < 0 ||
            error != 0) {
            if (error == 0) {
                error = errno;
            }
            throw std::runtime_error(std::string("connect error: ") +
                                     std::strerror(error));
        }

        const int one = 1;
        (void)::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    void SendFrame(const std::vector<uint8_t>& frame) {
        SendBytes(frame.data(), frame.size());
    }

    void SendBytes(const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
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
                pollfd entry{fd_, POLLOUT, 0};
                const int ready = ::poll(&entry, 1, deadline.RemainingMs());
                if (ready > 0) {
                    continue;
                }
                throw std::runtime_error("send timed out");
            }
            throw std::runtime_error(std::string("send failed: ") +
                                     std::strerror(errno));
        }
    }

    KvProtocol::Command ReadFrame() {
        Deadline deadline(std::chrono::seconds(10));
        for (;;) {
            KvProtocol::Command command;
            const int parsed = KvProtocol::parse_command(
                reinterpret_cast<const uint8_t*>(receive_buffer_.data()),
                receive_buffer_.size(), &command);
            if (parsed < 0) {
                throw std::runtime_error("received malformed KV frame");
            }
            if (parsed > 0) {
                receive_buffer_.erase(0, static_cast<size_t>(parsed));
                return command;
            }

            pollfd entry{fd_, POLLIN, 0};
            const int ready = ::poll(&entry, 1, deadline.RemainingMs());
            if (ready == 0) {
                throw std::runtime_error("receive timed out");
            }
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::runtime_error(std::string("poll failed: ") +
                                         std::strerror(errno));
            }

            char chunk[64 * 1024];
            const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (n > 0) {
                receive_buffer_.append(chunk, static_cast<size_t>(n));
                continue;
            }
            if (n == 0) {
                throw std::runtime_error("unexpected EOF");
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

    bool WaitForEof(std::chrono::milliseconds timeout) {
        Deadline deadline(timeout);
        for (;;) {
            pollfd entry{fd_, POLLIN, 0};
            const int ready = ::poll(&entry, 1, deadline.RemainingMs());
            if (ready == 0) {
                return false;
            }
            if (ready < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return false;
            }
            char chunk[1024];
            const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (n == 0) {
                return true;
            }
            if (n < 0 &&
                (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                continue;
            }
            return false;
        }
    }

    void CloseWithReset() {
        if (fd_ >= 0) {
            linger reset{1, 0};
            (void)::setsockopt(fd_, SOL_SOCKET, SO_LINGER, &reset,
                               sizeof(reset));
            ::close(fd_);
            fd_ = -1;
        }
    }

    void Close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

  private:
    int fd_ = -1;
    std::string receive_buffer_;
};

std::vector<uint8_t> Frame(KvProtocol::CommandType type,
                           const std::vector<std::string>& args) {
    return KvProtocol::encode_command(type, args);
}

void ExpectType(const KvProtocol::Command& command,
                KvProtocol::CommandType type) {
    EXPECT_EQ(static_cast<int>(command.type), static_cast<int>(type));
}

} // namespace

class KvSessionIntegrationTest : public ::testing::Test {
  protected:
    void StartServer() {
        std::promise<uint16_t> ready;
        auto port = ready.get_future();
        server_thread_ = std::thread([this,
                                      ready = std::move(ready)]() mutable {
            bool published = false;
            try {
                EventLoop loop;
                loop_pointer_.store(&loop);
                auto store = std::make_shared<KvStore>();
                std::map<std::string, std::shared_ptr<KvSession>> sessions;
                TcpServer server(&loop, InetAddress(0, true), "KvTest");

                server.set_connection_callback(
                    [this, store, &sessions](const TcpConnectionPtr& conn) {
                        if (!conn) {
                            return;
                        }
                        if (conn->connected()) {
                            auto session =
                                std::make_shared<KvSession>(conn, store);
                            session->set_save_callback(
                                [this]() { return save_result_.load(); });
                            session->start();
                            sessions[conn->name()] = std::move(session);
                        } else {
                            sessions.erase(conn->name());
                        }
                    });
                server.set_message_callback(
                    [&sessions](const TcpConnectionPtr& conn, Buffer* buffer) {
                        auto it = sessions.find(conn->name());
                        if (it != sessions.end()) {
                            auto session = it->second;
                            session->on_message(buffer);
                        }
                    });

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
        port_ = port.get();
        ASSERT_NE(port_, 0);
    }

    void TearDown() override {
        EventLoop* loop = loop_pointer_.load();
        if (loop != nullptr) {
            loop->quit();
        }
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
    }

    KvProtocol::Command Request(KvClient* client, KvProtocol::CommandType type,
                                const std::vector<std::string>& args = {}) {
        client->SendFrame(Frame(type, args));
        return client->ReadFrame();
    }

    std::thread server_thread_;
    std::atomic<EventLoop*> loop_pointer_{nullptr};
    std::atomic<RdbSaveResult> save_result_{RdbSaveResult::Ok};
    uint16_t port_ = 0;
};

TEST_F(KvSessionIntegrationTest, LargeKeyAndValueSurviveFragmentedWireInput) {
    StartServer();
    KvClient client;
    client.ConnectTo(port_);

    std::string key(1024, 'k');
    for (size_t i = 0; i < key.size(); i += 97) {
        key[i] = static_cast<char>('A' + (i % 26));
    }
    std::string value(1024u * 1024u, '\0');
    for (size_t i = 0; i < value.size(); ++i) {
        value[i] = static_cast<char>((i * 37u + 11u) & 0xffu);
    }

    const auto set_frame = Frame(KvProtocol::kSet, {key, value});
    size_t offset = 0;
    for (size_t chunk : {size_t{1}, size_t{2}, size_t{3}, size_t{5}, size_t{8},
                         size_t{13}, size_t{21}, size_t{34}}) {
        const size_t count = std::min(chunk, set_frame.size() - offset);
        client.SendBytes(set_frame.data() + offset, count);
        offset += count;
    }
    while (offset < set_frame.size()) {
        const size_t count = std::min<size_t>(8192, set_frame.size() - offset);
        client.SendBytes(set_frame.data() + offset, count);
        offset += count;
    }

    const auto set_response = client.ReadFrame();
    ExpectType(set_response, KvProtocol::kOk);

    const auto get_response = Request(&client, KvProtocol::kGet, {key});
    ExpectType(get_response, KvProtocol::kValue);
    ASSERT_EQ(get_response.args.size(), 1u);
    EXPECT_EQ(get_response.args[0], value);
}

TEST_F(KvSessionIntegrationTest, BinaryValuesAndPipelineOfOneHundredCommands) {
    StartServer();
    KvClient client;
    client.ConnectTo(port_);

    const std::string key("bin\0key", 7);
    const std::string value("v\0\xff\x01binary", 10);
    ExpectType(Request(&client, KvProtocol::kSet, {key, value}),
               KvProtocol::kOk);
    const auto get = Request(&client, KvProtocol::kGet, {key});
    ASSERT_EQ(get.args.size(), 1u);
    EXPECT_EQ(get.args[0], value);

    std::vector<uint8_t> pipeline;
    for (int i = 0; i < 100; ++i) {
        const auto frame = Frame(KvProtocol::kPing, {});
        pipeline.insert(pipeline.end(), frame.begin(), frame.end());
    }
    client.SendFrame(pipeline);
    for (int i = 0; i < 100; ++i) {
        const auto response = client.ReadFrame();
        ExpectType(response, KvProtocol::kOk);
        ASSERT_EQ(response.args.size(), 1u);
        EXPECT_EQ(response.args[0], "PONG") << "response index " << i;
    }
}

TEST_F(KvSessionIntegrationTest,
       KeyLengthAndArgumentErrorsKeepConnectionUsable) {
    StartServer();
    KvClient client;
    client.ConnectTo(port_);

    const auto empty_key = Request(&client, KvProtocol::kSet, {"", "v"});
    ExpectType(empty_key, KvProtocol::kError);
    const auto oversized_key =
        Request(&client, KvProtocol::kSet, {std::string(1025, 'k'), "v"});
    ExpectType(oversized_key, KvProtocol::kError);

    const auto wrong_args = Request(&client, KvProtocol::kGet, {});
    ExpectType(wrong_args, KvProtocol::kError);

    const auto ping = Request(&client, KvProtocol::kPing);
    ExpectType(ping, KvProtocol::kOk);
    ASSERT_EQ(ping.args.size(), 1u);
    EXPECT_EQ(ping.args[0], "PONG");
}

TEST_F(KvSessionIntegrationTest, ExpireRejectsNonCanonicalAndOverflowingTtl) {
    StartServer();
    KvClient client;
    client.ConnectTo(port_);

    ExpectType(Request(&client, KvProtocol::kSet, {"ttl-key", "value"}),
               KvProtocol::kOk);
    for (const std::string& invalid :
         {std::string("+1"), std::string(" 1"), std::string("1 "),
          std::string("1x"), std::string(""),
          std::string("999999999999999999999999999999")}) {
        const auto response =
            Request(&client, KvProtocol::kExpire, {"ttl-key", invalid});
        ExpectType(response, KvProtocol::kError);
        ASSERT_EQ(response.args.size(), 1u);
        EXPECT_NE(response.args[0].find("Invalid TTL"), std::string::npos)
            << "ttl='" << invalid << "'";
    }

    ExpectType(Request(&client, KvProtocol::kExpire, {"ttl-key", "0"}),
               KvProtocol::kOk);
    const auto missing = Request(&client, KvProtocol::kGet, {"ttl-key"});
    ExpectType(missing, KvProtocol::kNotFound);
}

TEST_F(KvSessionIntegrationTest, SaveReportsEveryTerminalResult) {
    StartServer();
    KvClient client;
    client.ConnectTo(port_);

    save_result_.store(RdbSaveResult::Ok);
    ExpectType(Request(&client, KvProtocol::kSave), KvProtocol::kOk);

    save_result_.store(RdbSaveResult::Failed);
    auto failed = Request(&client, KvProtocol::kSave);
    ExpectType(failed, KvProtocol::kError);
    ASSERT_EQ(failed.args.size(), 1u);
    EXPECT_NE(failed.args[0].find("Failed to save RDB"), std::string::npos);

    save_result_.store(RdbSaveResult::DurabilityUnconfirmed);
    auto unconfirmed = Request(&client, KvProtocol::kSave);
    ExpectType(unconfirmed, KvProtocol::kError);
    ASSERT_EQ(unconfirmed.args.size(), 1u);
    EXPECT_NE(unconfirmed.args[0].find("durability unconfirmed"),
              std::string::npos);
}

TEST_F(KvSessionIntegrationTest,
       MalformedAndUnknownFramesReturnErrorThenClose) {
    StartServer();

    {
        KvClient client;
        client.ConnectTo(port_);
        std::vector<uint8_t> malformed{KvProtocol::kPing, 0, 0};
        std::vector<uint8_t> frame{0, 0, 0, 3};
        frame.insert(frame.end(), malformed.begin(), malformed.end());
        client.SendFrame(frame);
        const auto response = client.ReadFrame();
        ExpectType(response, KvProtocol::kError);
        EXPECT_TRUE(client.WaitForEof(std::chrono::seconds(5)));
    }

    {
        KvClient client;
        client.ConnectTo(port_);
        std::vector<uint8_t> unknown{0xfe, 0};
        std::vector<uint8_t> frame{0, 0, 0, 2};
        frame.insert(frame.end(), unknown.begin(), unknown.end());
        client.SendFrame(frame);
        const auto response = client.ReadFrame();
        ExpectType(response, KvProtocol::kError);
        ASSERT_EQ(response.args.size(), 1u);
        EXPECT_NE(response.args[0].find("Unknown command"), std::string::npos);
        EXPECT_TRUE(client.WaitForEof(std::chrono::seconds(5)));
    }
}

TEST_F(KvSessionIntegrationTest, PeerResetDoesNotStopServer) {
    StartServer();

    KvClient reset_client;
    reset_client.ConnectTo(port_);
    reset_client.SendFrame(Frame(KvProtocol::kPing, {}));
    reset_client.CloseWithReset();

    KvClient client;
    client.ConnectTo(port_);
    const auto response = Request(&client, KvProtocol::kPing);
    ExpectType(response, KvProtocol::kOk);
    ASSERT_EQ(response.args.size(), 1u);
    EXPECT_EQ(response.args[0], "PONG");
}

} // namespace minidb
