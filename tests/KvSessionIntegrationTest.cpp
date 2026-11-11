#include <gtest/gtest.h>

#include "../src/net/EventLoop.h"
#include "../src/net/InetAddress.h"
#include "../src/net/TcpServer.h"
#include "../src/protocol/KvProtocol.h"
#include "../src/protocol/KvServer.h"
#include "../src/protocol/KvSession.h"
#include "../src/storage/AgentRecordCodec.h"
#include "../src/storage/KvStore.h"
#include "RdbTestUtil.h"

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
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
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

class KvPersistenceIntegrationTest : public ::testing::Test {
  protected:
    void StartServer(std::shared_ptr<ClockInterface> clock = nullptr) {
        temp_dir_ = minidb_test::make_unique_temp_dir("minidb_kv_txn_");
        rdb_path_ = temp_dir_ + "/db.rdb";
        std::promise<uint16_t> ready;
        auto port = ready.get_future();
        server_thread_ =
            std::thread([this, ready = std::move(ready),
                         clock = std::move(clock)]() mutable {
                bool published = false;
                try {
                    EventLoop loop;
                    loop_pointer_.store(&loop);
                    KvServer server(&loop, InetAddress(0, true), rdb_path_,
                                    DurabilityMode::kSync);
                    if (clock) {
                        server.set_kv_store(std::make_shared<KvStore>(clock));
                    }
                    if (!server.load_rdb()) {
                        throw std::runtime_error("failed to open persistence");
                    }
                    server.start();
                    server_pointer_.store(&server);
                    persistence_manager_.store(server.persistence_manager());
                    ready.set_value(server.listen_address().port());
                    published = true;
                    loop.loop();
                    persistence_manager_.store(nullptr);
                    server_pointer_.store(nullptr);
                    loop_pointer_.store(nullptr);
                } catch (...) {
                    persistence_manager_.store(nullptr);
                    server_pointer_.store(nullptr);
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
        KvServer* server = server_pointer_.load();
        if (loop != nullptr && server != nullptr) {
            loop->queue_in_loop([server]() { server->begin_shutdown(); });
        } else if (loop != nullptr) {
            loop->quit();
        }
        if (server_thread_.joinable()) {
            server_thread_.join();
        }
        if (!temp_dir_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(temp_dir_, ec);
        }
    }

    KvProtocol::Command Request(KvClient* client, KvProtocol::CommandType type,
                                const std::vector<std::string>& args = {}) {
        client->SendFrame(Frame(type, args));
        return client->ReadFrame();
    }

    std::uint64_t Begin(KvClient* client) {
        const auto response = Request(client, KvProtocol::kBegin);
        EXPECT_EQ(static_cast<int>(response.type),
                  static_cast<int>(KvProtocol::kOk));
        EXPECT_EQ(response.args.size(), 1u);
        std::uint64_t txn_id = 0;
        if (response.args.size() == 1) {
            const std::string& text = response.args[0];
            const auto parsed =
                std::from_chars(text.data(), text.data() + text.size(), txn_id);
            EXPECT_EQ(parsed.ec, std::errc());
            EXPECT_EQ(parsed.ptr, text.data() + text.size());
        }
        return txn_id;
    }

    bool WaitForTransactions(
        const std::function<bool(const TransactionStats&)>& predicate,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            PersistenceManager* manager = persistence_manager_.load();
            if (manager != nullptr &&
                predicate(manager->snapshot().transactions)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    bool WaitForAgent(
        const std::function<bool(const AgentStateStore::Stats&)>& predicate,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            PersistenceManager* manager = persistence_manager_.load();
            if (manager != nullptr && predicate(manager->snapshot().agent)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    std::thread server_thread_;
    std::atomic<EventLoop*> loop_pointer_{nullptr};
    std::atomic<KvServer*> server_pointer_{nullptr};
    std::atomic<PersistenceManager*> persistence_manager_{nullptr};
    std::string temp_dir_;
    std::string rdb_path_;
    uint16_t port_ = 0;
};

TEST_F(KvPersistenceIntegrationTest,
       TransactionsReadTheirWritesAndKeepTheirBeginSnapshot) {
    StartServer();
    KvClient setup;
    setup.ConnectTo(port_);
    ExpectType(Request(&setup, KvProtocol::kSet, {"key", "before"}),
               KvProtocol::kOk);

    KvClient writer;
    writer.ConnectTo(port_);
    const std::uint64_t writer_id = Begin(&writer);
    ASSERT_GT(writer_id, 0u);
    ExpectType(Request(&writer, KvProtocol::kSet, {"key", "inside"}),
               KvProtocol::kOk);
    const auto own_read = Request(&writer, KvProtocol::kGet, {"key"});
    ExpectType(own_read, KvProtocol::kValue);
    ASSERT_EQ(own_read.args.size(), 1u);
    EXPECT_EQ(own_read.args[0], "inside");

    KvClient reader;
    reader.ConnectTo(port_);
    const std::uint64_t reader_id = Begin(&reader);
    ASSERT_GT(reader_id, 0u);
    const auto before_commit = Request(&reader, KvProtocol::kGet, {"key"});
    ExpectType(before_commit, KvProtocol::kValue);
    ASSERT_EQ(before_commit.args.size(), 1u);
    EXPECT_EQ(before_commit.args[0], "before");

    const auto committed = Request(&writer, KvProtocol::kCommit);
    ExpectType(committed, KvProtocol::kOk);
    ASSERT_EQ(committed.args.size(), 1u);
    std::uint64_t commit_lsn = 0;
    const auto parsed_lsn = std::from_chars(
        committed.args[0].data(),
        committed.args[0].data() + committed.args[0].size(), commit_lsn);
    ASSERT_EQ(parsed_lsn.ec, std::errc());
    EXPECT_EQ(parsed_lsn.ptr,
              committed.args[0].data() + committed.args[0].size());
    EXPECT_EQ(commit_lsn, 2u);

    const auto repeated = Request(&reader, KvProtocol::kGet, {"key"});
    ExpectType(repeated, KvProtocol::kValue);
    ASSERT_EQ(repeated.args.size(), 1u);
    EXPECT_EQ(repeated.args[0], "before");

    const auto read_only_commit = Request(&reader, KvProtocol::kCommit);
    ExpectType(read_only_commit, KvProtocol::kOk);
    ASSERT_EQ(read_only_commit.args.size(), 1u);
    EXPECT_EQ(read_only_commit.args[0], "0");

    const auto visible = Request(&setup, KvProtocol::kGet, {"key"});
    ExpectType(visible, KvProtocol::kValue);
    ASSERT_EQ(visible.args.size(), 1u);
    EXPECT_EQ(visible.args[0], "inside");
}

TEST_F(KvPersistenceIntegrationTest,
       ConflictingWriterGetsConflictAndRollbackDiscardsWrites) {
    StartServer();
    KvClient first;
    KvClient second;
    first.ConnectTo(port_);
    second.ConnectTo(port_);

    const std::uint64_t first_id = Begin(&first);
    const std::uint64_t second_id = Begin(&second);
    ASSERT_GT(first_id, 0u);
    ASSERT_GT(second_id, 0u);
    EXPECT_NE(first_id, second_id);
    ExpectType(Request(&first, KvProtocol::kSet, {"race", "first"}),
               KvProtocol::kOk);
    ExpectType(Request(&second, KvProtocol::kSet, {"race", "second"}),
               KvProtocol::kOk);
    ExpectType(Request(&first, KvProtocol::kCommit), KvProtocol::kOk);
    ExpectType(Request(&second, KvProtocol::kCommit), KvProtocol::kConflict);

    const auto winner = Request(&first, KvProtocol::kGet, {"race"});
    ExpectType(winner, KvProtocol::kValue);
    ASSERT_EQ(winner.args.size(), 1u);
    EXPECT_EQ(winner.args[0], "first");

    const std::uint64_t rollback_id = Begin(&first);
    ASSERT_GT(rollback_id, 0u);
    ExpectType(Request(&first, KvProtocol::kSet, {"rollback", "value"}),
               KvProtocol::kOk);
    ExpectType(Request(&first, KvProtocol::kRollback), KvProtocol::kOk);
    ExpectType(Request(&first, KvProtocol::kGet, {"rollback"}),
               KvProtocol::kNotFound);

    EXPECT_TRUE(WaitForTransactions([](const TransactionStats& stats) {
        return stats.started == 3u && stats.committed == 1u &&
               stats.conflicted == 1u && stats.rolled_back == 1u &&
               stats.active == 0u;
    }));
}

TEST_F(KvPersistenceIntegrationTest,
       TransactionStateErrorsKeepTheConnectionAndActiveTransactionUsable) {
    StartServer();
    KvClient client;
    client.ConnectTo(port_);

    ExpectType(Request(&client, KvProtocol::kCommit), KvProtocol::kError);
    ExpectType(Request(&client, KvProtocol::kRollback), KvProtocol::kError);
    ExpectType(Request(&client, KvProtocol::kBegin, {"unexpected"}),
               KvProtocol::kError);

    const std::uint64_t txn_id = Begin(&client);
    ASSERT_GT(txn_id, 0u);
    ExpectType(Request(&client, KvProtocol::kBegin), KvProtocol::kError);
    ExpectType(Request(&client, KvProtocol::kSet, {"invalid-args"}),
               KvProtocol::kError);
    ExpectType(Request(&client, KvProtocol::kGet), KvProtocol::kError);
    ExpectType(Request(&client, KvProtocol::kGet, {"missing"}),
               KvProtocol::kNotFound);
    ExpectType(Request(&client, KvProtocol::kSet, {"kept", "value"}),
               KvProtocol::kOk);
    ExpectType(Request(&client, KvProtocol::kCommit, {"unexpected"}),
               KvProtocol::kError);
    ExpectType(Request(&client, KvProtocol::kCommit), KvProtocol::kOk);

    const auto value = Request(&client, KvProtocol::kGet, {"kept"});
    ExpectType(value, KvProtocol::kValue);
    ASSERT_EQ(value.args.size(), 1u);
    EXPECT_EQ(value.args[0], "value");
    ExpectType(Request(&client, KvProtocol::kPing), KvProtocol::kOk);
}

TEST_F(KvPersistenceIntegrationTest,
       DisconnectRollsBackAndAcceptedCommitStillCompletes) {
    StartServer();
    {
        KvClient abandoned;
        abandoned.ConnectTo(port_);
        const std::uint64_t txn_id = Begin(&abandoned);
        ASSERT_GT(txn_id, 0u);
        ExpectType(Request(&abandoned, KvProtocol::kSet, {"abandoned", "no"}),
                   KvProtocol::kOk);
    }
    EXPECT_TRUE(WaitForTransactions([](const TransactionStats& stats) {
        return stats.rolled_back == 1u && stats.active == 0u;
    }));

    KvClient observer;
    observer.ConnectTo(port_);
    ExpectType(Request(&observer, KvProtocol::kGet, {"abandoned"}),
               KvProtocol::kNotFound);

    {
        KvClient committing;
        committing.ConnectTo(port_);
        const std::uint64_t txn_id = Begin(&committing);
        ASSERT_GT(txn_id, 0u);
        ExpectType(Request(&committing, KvProtocol::kSet, {"accepted", "yes"}),
                   KvProtocol::kOk);
        committing.SendFrame(Frame(KvProtocol::kCommit, {}));
    }
    EXPECT_TRUE(WaitForTransactions(
        [](const TransactionStats& stats) { return stats.committed == 1u; }));
    const auto committed = WaitForTransactions(
        [](const TransactionStats& stats) { return stats.committed == 1u; });
    ASSERT_TRUE(committed);
    std::optional<std::string> visible;
    for (int attempt = 0; attempt < 100 && !visible.has_value(); ++attempt) {
        const auto value = Request(&observer, KvProtocol::kGet, {"accepted"});
        ExpectType(value, KvProtocol::kValue);
        if (value.args.size() == 1) {
            visible = value.args[0];
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    EXPECT_EQ(visible, std::optional<std::string>("yes"));
}

TEST_F(KvPersistenceIntegrationTest,
       AgentCreateGetAndIdempotentReplayUseMdOrEnvelopes) {
    StartServer();
    KvClient client;
    client.ConnectTo(port_);

    const std::vector<std::string> create_args = {
        "SESSION_CREATE", "s1", "60000", "working", "create-s1"};
    const auto created = Request(&client, KvProtocol::kAgent, create_args);
    ExpectType(created, KvProtocol::kValue);
    ASSERT_EQ(created.args.size(), 1u);

    AgentRecordCodec::Response create_response;
    std::string error;
    ASSERT_TRUE(AgentRecordCodec::decode_response(created.args[0],
                                                  &create_response, &error))
        << error;
    EXPECT_EQ(
        create_response.status,
        static_cast<std::uint8_t>(AgentRecordCodec::ResponseStatus::kCreated));
    EXPECT_EQ(create_response.revision, 1u);
    EXPECT_EQ(create_response.fence, 0u);
    EXPECT_NE(create_response.lsn, 0u);

    PersistenceManager* manager = persistence_manager_.load();
    ASSERT_NE(manager, nullptr);
    const std::uint64_t create_lsn = manager->snapshot().last_lsn;
    EXPECT_EQ(create_response.lsn, create_lsn);

    const auto fetched =
        Request(&client, KvProtocol::kAgent, {"SESSION_GET", "s1"});
    ExpectType(fetched, KvProtocol::kValue);
    ASSERT_EQ(fetched.args.size(), 1u);
    AgentRecordCodec::Response get_response;
    ASSERT_TRUE(AgentRecordCodec::decode_response(fetched.args[0],
                                                  &get_response, &error))
        << error;
    EXPECT_EQ(get_response.status,
              static_cast<std::uint8_t>(AgentRecordCodec::ResponseStatus::kOk));
    EXPECT_EQ(get_response.revision, 1u);
    EXPECT_EQ(get_response.lsn, 0u);
    EXPECT_FALSE(get_response.payload.empty());

    std::string session_key;
    ASSERT_TRUE(AgentRecordCodec::session_key("s1", &session_key, &error))
        << error;
    AgentRecordCodec::Record record;
    ASSERT_TRUE(AgentRecordCodec::decode_typed_record(
        session_key, get_response.payload, &record, nullptr, nullptr, nullptr,
        &error))
        << error;
    EXPECT_EQ(record.type, AgentRecordCodec::RecordType::kSession);
    EXPECT_EQ(record.status, static_cast<std::uint8_t>(
                                 AgentRecordCodec::SessionStatus::kActive));
    EXPECT_EQ(record.revision, 1u);

    const auto replayed = Request(&client, KvProtocol::kAgent, create_args);
    ExpectType(replayed, KvProtocol::kValue);
    ASSERT_EQ(replayed.args.size(), 1u);
    EXPECT_EQ(replayed.args[0], created.args[0]);
    EXPECT_EQ(manager->snapshot().last_lsn, create_lsn);
    EXPECT_TRUE(WaitForAgent([](const AgentStateStore::Stats& stats) {
        return stats.idempotency_replays == 1u;
    }));

    const auto duplicate = Request(
        &client, KvProtocol::kAgent,
        {"SESSION_CREATE", "s1", "60000", "working", "create-s1-other"});
    ExpectType(duplicate, KvProtocol::kValue);
    ASSERT_EQ(duplicate.args.size(), 1u);
    AgentRecordCodec::Response duplicate_response;
    ASSERT_TRUE(AgentRecordCodec::decode_response(duplicate.args[0],
                                                  &duplicate_response, &error))
        << error;
    EXPECT_EQ(duplicate_response.status,
              static_cast<std::uint8_t>(
                  AgentRecordCodec::ResponseStatus::kAlreadyExists));
    EXPECT_EQ(manager->snapshot().last_lsn, create_lsn);

    ExpectType(Request(&client, KvProtocol::kPing), KvProtocol::kOk);
}

TEST_F(KvPersistenceIntegrationTest,
       AgentDisconnectCompletionIsSafeAndReplayStillSucceeds) {
    StartServer();
    const std::vector<std::string> create_args = {
        "SESSION_CREATE", "crash-session", "60000", "working",
        "create-after-disconnect"};
    {
        KvClient disconnected;
        disconnected.ConnectTo(port_);
        disconnected.SendFrame(Frame(KvProtocol::kAgent, create_args));
    }

    ASSERT_TRUE(WaitForAgent([](const AgentStateStore::Stats& stats) {
        return stats.sessions_created == 1u;
    }));

    KvClient replay;
    replay.ConnectTo(port_);
    const auto response = Request(&replay, KvProtocol::kAgent, create_args);
    ExpectType(response, KvProtocol::kValue);
    ASSERT_EQ(response.args.size(), 1u);
    AgentRecordCodec::Response decoded;
    std::string error;
    ASSERT_TRUE(
        AgentRecordCodec::decode_response(response.args[0], &decoded, &error))
        << error;
    EXPECT_EQ(decoded.status, static_cast<std::uint8_t>(
                                  AgentRecordCodec::ResponseStatus::kCreated));
    EXPECT_NE(decoded.lsn, 0u);
    ExpectType(Request(&replay, KvProtocol::kPing), KvProtocol::kOk);
}

TEST_F(KvPersistenceIntegrationTest,
       AgentValidationErrorsAndTransactionIsolationKeepConnectionUsable) {
    StartServer();
    KvClient client;
    client.ConnectTo(port_);

    ExpectType(Request(&client, KvProtocol::kAgent), KvProtocol::kError);
    ExpectType(Request(&client, KvProtocol::kAgent, {"UNKNOWN"}),
               KvProtocol::kError);
    ExpectType(
        Request(&client, KvProtocol::kAgent,
                {"SESSION_CREATE", "s1", "not-a-number", "state", "idem"}),
        KvProtocol::kError);
    ExpectType(Request(&client, KvProtocol::kAgent, {"SESSION_GET"}),
               KvProtocol::kError);
    ExpectType(Request(&client, KvProtocol::kPing), KvProtocol::kOk);

    ASSERT_GT(Begin(&client), 0u);
    ExpectType(Request(&client, KvProtocol::kAgent, {"SESSION_GET", "s1"}),
               KvProtocol::kError);
    ExpectType(Request(&client, KvProtocol::kSet, {"inside", "ok"}),
               KvProtocol::kOk);
    ExpectType(Request(&client, KvProtocol::kCommit), KvProtocol::kOk);
    ExpectType(Request(&client, KvProtocol::kPing), KvProtocol::kOk);
}

TEST_F(KvPersistenceIntegrationTest, AgentGcTimerDeletesTerminalGroups) {
    auto clock = std::make_shared<minidb_test::FrozenClock>(
        1000, 1700000000000);
    StartServer(clock);
    KvClient client;
    client.ConnectTo(port_);

    ExpectType(Request(&client, KvProtocol::kAgent,
                       {"SESSION_CREATE", "timer-session", "60000", "state",
                        "timer-create"}),
               KvProtocol::kValue);
    ExpectType(Request(&client, KvProtocol::kAgent,
                       {"TOOL_CREATE", "timer-session", "timer-tool", "tool",
                        "request", "timer-tool-create"}),
               KvProtocol::kValue);
    ExpectType(Request(&client, KvProtocol::kAgent,
                       {"SESSION_CLOSE", "timer-session", "2", "COMPLETED",
                        "timer-close"}),
               KvProtocol::kValue);

    ASSERT_TRUE(WaitForAgent([](const AgentStateStore::Stats& stats) {
        return stats.gc_runs >= 1u;
    }));
    const AgentStateStore::Stats stats =
        persistence_manager_.load()->snapshot().agent;
    EXPECT_EQ(stats.sessions_stored, 0u);
    EXPECT_EQ(stats.tools_stored, 0u);
    EXPECT_EQ(stats.gc_candidates, 1u);
    EXPECT_EQ(stats.gc_deleted, 2u);
    EXPECT_EQ(stats.gc_failures, 0u);
    ExpectType(Request(&client, KvProtocol::kPing), KvProtocol::kOk);
}

TEST_F(KvPersistenceIntegrationTest,
       ReservedAgentKeysAreRejectedOutsideAndInsideTransactions) {
    StartServer();
    KvClient client;
    client.ConnectTo(port_);

    const std::string reserved =
        std::string(AgentRecordCodec::kReservedPrefix) + "m";
    const std::vector<
        std::pair<KvProtocol::CommandType, std::vector<std::string>>>
        reserved_commands = {
            {KvProtocol::kSet, {reserved, "value"}},
            {KvProtocol::kGet, {reserved}},
            {KvProtocol::kDel, {reserved}},
            {KvProtocol::kExpire, {reserved, "1000"}},
            {KvProtocol::kTtl, {reserved}},
        };

    for (const auto& [command, args] : reserved_commands) {
        const auto response = Request(&client, command, args);
        ExpectType(response, KvProtocol::kError);
        ASSERT_EQ(response.args.size(), 1u);
        EXPECT_NE(response.args[0].find("reserved"), std::string::npos);
    }
    ExpectType(Request(&client, KvProtocol::kPing), KvProtocol::kOk);

    ASSERT_GT(Begin(&client), 0u);
    for (const auto& [command, args] : reserved_commands) {
        const auto response = Request(&client, command, args);
        ExpectType(response, KvProtocol::kError);
        ASSERT_EQ(response.args.size(), 1u);
        EXPECT_NE(response.args[0].find("reserved"), std::string::npos);
    }
    ExpectType(Request(&client, KvProtocol::kSet, {"ordinary", "kept"}),
               KvProtocol::kOk);
    ExpectType(Request(&client, KvProtocol::kCommit), KvProtocol::kOk);

    const auto value = Request(&client, KvProtocol::kGet, {"ordinary"});
    ExpectType(value, KvProtocol::kValue);
    ASSERT_EQ(value.args.size(), 1u);
    EXPECT_EQ(value.args[0], "kept");
}

TEST_F(KvSessionIntegrationTest, TransactionCommandsReportUnavailable) {
    StartServer();
    KvClient client;
    client.ConnectTo(port_);

    for (const auto command :
         {KvProtocol::kBegin, KvProtocol::kCommit, KvProtocol::kRollback}) {
        const auto response = Request(&client, command);
        ExpectType(response, KvProtocol::kError);
        ASSERT_EQ(response.args.size(), 1u);
        EXPECT_NE(response.args[0].find("unavailable"), std::string::npos);
    }
    ExpectType(Request(&client, KvProtocol::kPing), KvProtocol::kOk);
}

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
