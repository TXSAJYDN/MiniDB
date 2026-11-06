#include <gtest/gtest.h>

#include "../src/base/Buffer.h"
#include "../src/net/EventLoop.h"
#include "../src/net/InetAddress.h"
#include "../src/net/TcpConnection.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace minidb {
namespace {

int open_fd_count() {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
        return -1;
    }
    int count = 0;
    while (dirent* entry = ::readdir(dir)) {
        if (entry->d_name[0] != '.' && entry->d_name[0] != '\0') {
            ++count;
        }
    }
    ::closedir(dir);
    return count;
}

struct SocketPair {
    int first = -1;
    int second = -1;

    SocketPair() {
        int fds[2] = {-1, -1};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                         fds) != 0) {
            return;
        }
        first = fds[0];
        second = fds[1];
    }

    ~SocketPair() {
        if (second >= 0) {
            ::close(second);
        }
    }

    bool valid() const {
        return first >= 0 && second >= 0;
    }
};

std::shared_ptr<TcpConnection> make_connection(EventLoop* loop, int fd) {
    return std::make_shared<TcpConnection>(loop, "test-connection", fd,
                                           InetAddress(0, true),
                                           InetAddress(0, true));
}

class TcpConnectionTest : public ::testing::Test {
  protected:
    void RunFor(std::chrono::milliseconds duration) {
        bool timed_out = false;
        const auto timer = loop_.run_after(duration.count(), [&]() {
            timed_out = true;
            loop_.quit();
        });
        loop_.loop();
        loop_.cancel_timer(timer);
        ASSERT_TRUE(timed_out);
    }
    EventLoop loop_;
};

TEST_F(TcpConnectionTest, FinDeliversResidualPayloadBeforeClosingOnce) {
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    auto connection = make_connection(&loop_, pair.first);
    std::string received;
    int close_count = 0;
    bool connected_before = false;

    connection->set_message_callback(
        [&](const TcpConnectionPtr&, Buffer* input) {
            received.append(input->retrieve_all_as_string());
        });
    connection->set_close_callback([&](const TcpConnectionPtr&) {
        ++close_count;
        loop_.quit();
    });
    connection->connect_established();
    connected_before = connection->connected();
    ASSERT_TRUE(connected_before);

    const std::string payload = "last-request-body";
    ASSERT_EQ(::write(pair.second, payload.data(), payload.size()),
              static_cast<ssize_t>(payload.size()));
    ASSERT_EQ(::shutdown(pair.second, SHUT_WR), 0);

    bool timed_out = false;
    const auto timer = loop_.run_after(1000, [&]() {
        timed_out = true;
        loop_.quit();
    });
    loop_.loop();
    loop_.cancel_timer(timer);

    EXPECT_FALSE(timed_out);
    EXPECT_EQ(received, payload);
    EXPECT_EQ(close_count, 1);
    EXPECT_FALSE(connection->connected());
    char byte = 0;
    EXPECT_EQ(::read(pair.second, &byte, sizeof(byte)), 0);
}

TEST_F(TcpConnectionTest, StopReadingBlocksDeliveryUntilStartReading) {
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    auto connection = make_connection(&loop_, pair.first);
    std::string received;
    connection->set_message_callback(
        [&](const TcpConnectionPtr&, Buffer* input) {
            received.append(input->retrieve_all_as_string());
            loop_.quit();
        });
    connection->connect_established();
    connection->stop_reading();

    const std::string payload = "deferred";
    ASSERT_EQ(::write(pair.second, payload.data(), payload.size()),
              static_cast<ssize_t>(payload.size()));
    RunFor(std::chrono::milliseconds(40));
    EXPECT_TRUE(received.empty());

    connection->start_reading();
    bool timed_out = false;
    const auto timer = loop_.run_after(1000, [&]() {
        timed_out = true;
        loop_.quit();
    });
    loop_.loop();
    loop_.cancel_timer(timer);
    EXPECT_FALSE(timed_out);
    EXPECT_EQ(received, payload);
}

TEST_F(TcpConnectionTest, ShutdownIsIdempotentAndClosesOnce) {
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    auto connection = make_connection(&loop_, pair.first);
    int close_count = 0;
    connection->set_close_callback(
        [&](const TcpConnectionPtr&) { ++close_count; });
    connection->connect_established();

    connection->shutdown();
    connection->shutdown();
    connection->connect_destroyed();
    connection->connect_destroyed();

    EXPECT_EQ(close_count, 1);
    EXPECT_FALSE(connection->connected());
}

TEST_F(TcpConnectionTest, SingleSendAboveBudgetIsRejectedWithoutAllocation) {
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    auto connection = make_connection(&loop_, pair.first);
    connection->connect_established();

    const std::string oversized(TcpConnection::kMaxOutputBufferSize + 1, 'x');
    EXPECT_FALSE(connection->send(oversized));
    EXPECT_TRUE(connection->connected());
    EXPECT_TRUE(connection->send("still-usable"));
}

TEST_F(TcpConnectionTest, CumulativeBufferedOutputRespectsBudget) {
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    int small_buffer = 4096;
    ASSERT_EQ(::setsockopt(pair.first, SOL_SOCKET, SO_SNDBUF, &small_buffer,
                           sizeof(small_buffer)),
              0);
    ASSERT_EQ(::setsockopt(pair.second, SOL_SOCKET, SO_RCVBUF, &small_buffer,
                           sizeof(small_buffer)),
              0);

    auto connection = make_connection(&loop_, pair.first);
    connection->connect_established();
    const std::string chunk(64 * 1024, 'x');
    bool accepted = true;
    for (int i = 0; i < 64 && accepted; ++i) {
        accepted = connection->send(chunk);
    }
    EXPECT_FALSE(accepted);
    EXPECT_FALSE(connection->connected());
}

TEST_F(TcpConnectionTest, EarlyDestructionReleasesDescriptor) {
    const int before = open_fd_count();
    ASSERT_GE(before, 0);

    for (int i = 0; i < 32; ++i) {
        SocketPair pair;
        ASSERT_TRUE(pair.valid());
        auto connection = make_connection(&loop_, pair.first);
        connection->connect_established();
        connection.reset();
        ::close(pair.second);
        pair.second = -1;
    }

    EXPECT_EQ(open_fd_count(), before);
}

} // namespace
} // namespace minidb
