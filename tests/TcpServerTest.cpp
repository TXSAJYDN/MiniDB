#include <gtest/gtest.h>

#include "../src/net/EventLoop.h"
#include "../src/net/InetAddress.h"
#include "../src/net/TcpServer.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace minidb {
namespace {

class TcpServerLifecycleTest : public ::testing::Test {
  protected:
    static int ConnectTo(uint16_t port) {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            throw std::runtime_error("socket failed");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address),
                      sizeof(address)) < 0) {
            const int saved = errno;
            ::close(fd);
            errno = saved;
            return -1;
        }
        return fd;
    }
};

TEST_F(TcpServerLifecycleTest, StopClosesListenerAndLiveConnections) {
    EventLoop loop;
    TcpServer server(&loop, InetAddress(0, true), "Lifecycle");
    bool accepted = false;
    bool stopped = false;
    server.set_connection_callback([&](const TcpConnectionPtr& conn) {
        if (!conn || !conn->connected()) {
            return;
        }
        accepted = true;
        loop.run_after(20, [&]() {
            server.stop();
            server.stop();
            stopped = true;
        });
    });
    server.start();
    const uint16_t port = server.listen_address().port();
    ASSERT_NE(port, 0);

    const int client_fd = ConnectTo(port);
    ASSERT_GE(client_fd, 0) << std::strerror(errno);

    loop.run_after(500, [&]() { loop.quit(); });
    loop.loop();

    EXPECT_TRUE(accepted);
    EXPECT_TRUE(stopped);
    EXPECT_EQ(server.connection_count(), 0u);
    EXPECT_THROW(server.start(), std::logic_error);

    timeval timeout{};
    timeout.tv_sec = 2;
    ASSERT_EQ(::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                           sizeof(timeout)),
              0);
    char byte = 0;
    EXPECT_EQ(::recv(client_fd, &byte, sizeof(byte), 0), 0);
    ::close(client_fd);

    const int retry_fd = ConnectTo(port);
    EXPECT_LT(retry_fd, 0) << "listener still accepted connections after stop";
    if (retry_fd >= 0) {
        ::close(retry_fd);
    }
}

} // namespace
} // namespace minidb
