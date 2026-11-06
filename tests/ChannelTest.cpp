#include <gtest/gtest.h>

#include "../src/net/Channel.h"

#include <cerrno>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace minidb {
namespace {

TEST(ChannelTest, DispatchesErrorReadWriteAndCloseInContractOrder) {
    Channel channel(nullptr, 77);
    std::vector<std::string> order;
    channel.set_error_callback([&]() { order.push_back("error"); });
    channel.set_read_callback([&]() { order.push_back("read"); });
    channel.set_write_callback([&]() { order.push_back("write"); });
    channel.set_close_callback([&]() { order.push_back("close"); });

    channel.set_revents(EPOLLERR | EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLHUP);
    channel.handle_event();

    EXPECT_EQ(order,
              std::vector<std::string>({"error", "read", "write", "close"}));
}

TEST(ChannelTest, HupWithoutInStillDrainsResidualPayloadBeforeClose) {
    int sockets[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                           0, sockets),
              0);
    const std::string payload = "payload-delivered-with-hup";
    ASSERT_EQ(::write(sockets[1], payload.data(), payload.size()),
              static_cast<ssize_t>(payload.size()));
    ASSERT_EQ(::shutdown(sockets[1], SHUT_WR), 0);

    Channel channel(nullptr, sockets[0]);
    std::string received;
    std::vector<std::string> order;
    channel.set_read_callback([&]() {
        char buffer[64];
        const ssize_t n = ::read(sockets[0], buffer, sizeof(buffer));
        if (n > 0) {
            received.append(buffer, static_cast<size_t>(n));
        }
        order.push_back("read");
    });
    channel.set_close_callback([&]() { order.push_back("close"); });

    // Linux may report HUP without IN while unread bytes remain.
    channel.set_revents(EPOLLHUP);
    channel.handle_event();

    EXPECT_EQ(received, payload);
    EXPECT_EQ(order, std::vector<std::string>({"read", "close"}));

    ::close(sockets[0]);
    ::close(sockets[1]);
}

TEST(ChannelTest, RdhupDrainsBeforeSingleCloseCallback) {
    int sockets[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                           0, sockets),
              0);
    const std::string payload = "tail";
    ASSERT_EQ(::write(sockets[1], payload.data(), payload.size()),
              static_cast<ssize_t>(payload.size()));
    ASSERT_EQ(::shutdown(sockets[1], SHUT_WR), 0);

    Channel channel(nullptr, sockets[0]);
    std::string received;
    std::vector<std::string> order;
    channel.set_read_callback([&]() {
        char buffer[16];
        const ssize_t n = ::read(sockets[0], buffer, sizeof(buffer));
        if (n > 0) {
            received.append(buffer, static_cast<size_t>(n));
        }
        order.push_back("read");
    });
    channel.set_close_callback([&]() { order.push_back("close"); });

    channel.set_revents(EPOLLRDHUP | EPOLLHUP);
    channel.handle_event();

    EXPECT_EQ(received, payload);
    EXPECT_EQ(order, std::vector<std::string>({"read", "close"}));

    ::close(sockets[0]);
    ::close(sockets[1]);
}

TEST(ChannelTest, ErrorWithoutReadOrCloseBitsDoesNotInvokeThoseCallbacks) {
    Channel channel(nullptr, 78);
    int errors = 0;
    int reads = 0;
    int closes = 0;
    channel.set_error_callback([&]() { ++errors; });
    channel.set_read_callback([&]() { ++reads; });
    channel.set_close_callback([&]() { ++closes; });

    channel.set_revents(EPOLLERR);
    channel.handle_event();

    EXPECT_EQ(errors, 1);
    EXPECT_EQ(reads, 0);
    EXPECT_EQ(closes, 0);
}

TEST(ChannelTest, MissingCallbacksAreIgnored) {
    Channel channel(nullptr, 79);
    channel.set_revents(EPOLLERR | EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLHUP);
    EXPECT_NO_THROW(channel.handle_event());
}

} // namespace
} // namespace minidb
