// Blocking TCP echo server used to study Linux socket syscalls.
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr size_t kBufferSize = 4096;

bool write_all(int fd, const char* data, size_t length) {
    size_t written = 0;
    while (written < length) {
        const ssize_t result =
            ::send(fd, data + written, length - written, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "write failed: " << std::strerror(errno) << '\n';
            return false;
        }
        if (result == 0) {
            std::cerr << "write returned zero\n";
            return false;
        }
        const size_t chunk = static_cast<size_t>(result);
        written += chunk;
        if (chunk < length - (written - chunk)) {
            std::cout << "partial write: " << chunk << " bytes\n";
        }
    }
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);

    int port = argc > 1 ? std::atoi(argv[1]) : 0;
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    int option = 1;
    if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &option,
                     sizeof(option)) < 0) {
        std::cerr << "setsockopt(SO_REUSEADDR) failed: " << std::strerror(errno)
                  << '\n';
        ::close(listen_fd);
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) < 0) {
        std::cerr << "bind failed: " << std::strerror(errno) << '\n';
        ::close(listen_fd);
        return 1;
    }

    socklen_t address_length = sizeof(address);
    if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&address),
                      &address_length) < 0) {
        std::cerr << "getsockname failed: " << std::strerror(errno) << '\n';
        ::close(listen_fd);
        return 1;
    }
    port = ntohs(address.sin_port);
    if (::listen(listen_fd, SOMAXCONN) < 0) {
        std::cerr << "listen failed: " << std::strerror(errno) << '\n';
        ::close(listen_fd);
        return 1;
    }

    std::cout << "Blocking Echo Server listening on port " << port << '\n';
    std::cout.flush();

    while (true) {
        sockaddr_in peer{};
        socklen_t peer_length = sizeof(peer);
        const int connection_fd = ::accept(
            listen_fd, reinterpret_cast<sockaddr*>(&peer), &peer_length);
        if (connection_fd < 0) {
            if (errno == EINTR) {
                std::cout << "accept interrupted, retrying\n";
                continue;
            }
            std::cerr << "accept failed: " << std::strerror(errno) << '\n';
            break;
        }

        char peer_ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
        std::cout << "accepted fd=" << connection_fd << " peer=" << peer_ip
                  << ':' << ntohs(peer.sin_port) << '\n';

        char buffer[kBufferSize];
        while (true) {
            const ssize_t received =
                ::read(connection_fd, buffer, sizeof(buffer));
            if (received < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::cerr << "read failed on fd=" << connection_fd << ": "
                          << std::strerror(errno) << '\n';
                break;
            }
            if (received == 0) {
                std::cout << "EOF on fd=" << connection_fd << '\n';
                break;
            }
            if (!write_all(connection_fd, buffer,
                           static_cast<size_t>(received))) {
                break;
            }
        }

        ::shutdown(connection_fd, SHUT_RDWR);
        ::close(connection_fd);
        std::cout << "closed fd=" << connection_fd << '\n';
    }

    ::close(listen_fd);
    return 1;
}
