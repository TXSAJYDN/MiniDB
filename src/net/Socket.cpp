#include "Socket.h"
#include "../base/Log.h"
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace minidb {

Socket::Socket(int sockfd) : sockfd_(sockfd) {}

Socket Socket::create_tcp_socket() {
    int sockfd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          IPPROTO_TCP);
    if (sockfd < 0) {
        throw std::runtime_error(std::string("socket() failed: ") +
                                 std::strerror(errno));
    }
    return Socket(sockfd);
}

void Socket::bind(const InetAddress& addr) {
    if (::bind(sockfd_.get(), addr.sockaddr(), addr.length()) < 0) {
        throw std::runtime_error(std::string("bind() failed: ") +
                                 std::strerror(errno));
    }
}

void Socket::listen() {
    if (::listen(sockfd_.get(), SOMAXCONN) < 0) {
        throw std::runtime_error(std::string("listen() failed: ") +
                                 std::strerror(errno));
    }
}

int Socket::accept(InetAddress* peer_addr) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    std::memset(&addr, 0, sizeof(addr));
    int connfd = -1;
    do {
        connfd =
            ::accept4(sockfd_.get(), reinterpret_cast<struct sockaddr*>(&addr),
                      &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
    } while (connfd < 0 && errno == EINTR);
    if (connfd >= 0 && peer_addr) {
        *peer_addr = InetAddress(addr);
    }
    return connfd;
}

bool Socket::set_socket_option(int level, int option, bool on) {
    int optval = on ? 1 : 0;
    if (::setsockopt(sockfd_.get(), level, option, &optval, sizeof(optval)) <
        0) {
        Log::Warning(std::string("setsockopt(") + std::to_string(option) +
                     ") failed for fd " + std::to_string(sockfd_.get()) + ": " +
                     std::strerror(errno));
        return false;
    }
    return true;
}

bool Socket::set_reuse_addr(bool on) {
    return set_socket_option(SOL_SOCKET, SO_REUSEADDR, on);
}

bool Socket::set_reuse_port(bool on) {
    return set_socket_option(SOL_SOCKET, SO_REUSEPORT, on);
}

bool Socket::set_keep_alive(bool on) {
    return set_socket_option(SOL_SOCKET, SO_KEEPALIVE, on);
}

bool Socket::set_tcp_no_delay(bool on) {
    return set_socket_option(IPPROTO_TCP, TCP_NODELAY, on);
}

InetAddress Socket::local_address() const {
    struct sockaddr_in addr {};
    socklen_t len = sizeof(addr);
    if (::getsockname(sockfd_.get(), reinterpret_cast<struct sockaddr*>(&addr),
                      &len) < 0) {
        throw std::runtime_error(std::string("getsockname() failed: ") +
                                 std::strerror(errno));
    }
    return InetAddress(addr);
}

void Socket::close() {
    sockfd_.close();
}

bool Socket::shutdown_write() {
    if (::shutdown(sockfd_.get(), SHUT_WR) < 0 && errno != ENOTCONN &&
        errno != EPIPE && errno != ECONNRESET) {
        Log::Warning(std::string("shutdown(SHUT_WR) failed for fd ") +
                     std::to_string(sockfd_.get()) + ": " +
                     std::strerror(errno));
        return false;
    }
    return true;
}

} // namespace minidb
