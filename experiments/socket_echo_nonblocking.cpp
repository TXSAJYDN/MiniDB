// Non-blocking TCP echo server used to study EAGAIN, short writes and EOF.
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr size_t kReadChunkSize = 4096;
constexpr size_t kMaxPendingBytes = 8 * 1024 * 1024;
volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int) {
    g_stop = 1;
}

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        std::cerr << "fcntl(F_GETFL) failed: " << std::strerror(errno) << '\n';
        return false;
    }
    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::cerr << "fcntl(F_SETFL) failed: " << std::strerror(errno) << '\n';
        return false;
    }
    return true;
}

struct Connection {
    int fd = -1;
    std::string peer;
    std::vector<char> pending;
    size_t pending_offset = 0;
    bool read_eof = false;

    size_t pending_bytes() const {
        return pending.size() - pending_offset;
    }
};

struct Statistics {
    uint64_t accepted = 0;
    uint64_t eagain = 0;
    uint64_t partial_writes = 0;
    uint64_t read_eof = 0;
    uint64_t reset = 0;
};

void close_connection(Connection& connection) {
    if (connection.fd >= 0) {
        std::cout << "CLOSE fd=" << connection.fd
                  << " pending=" << connection.pending_bytes() << '\n';
        ::close(connection.fd);
        connection.fd = -1;
    }
}

// Returns false when the connection must be retired.
bool flush_pending(Connection& connection, Statistics& stats) {
    while (connection.pending_bytes() > 0) {
        const ssize_t result =
            ::send(connection.fd,
                   connection.pending.data() + connection.pending_offset,
                   connection.pending_bytes(), MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                ++stats.eagain;
                if (stats.eagain == 1 || stats.eagain % 1024 == 0) {
                    std::cout << "EAGAIN fd=" << connection.fd
                              << " pending=" << connection.pending_bytes()
                              << " count=" << stats.eagain << '\n';
                }
                return true;
            }
            std::cerr << "write failed fd=" << connection.fd << ": "
                      << std::strerror(errno) << '\n';
            if (errno == ECONNRESET) {
                ++stats.reset;
            }
            return false;
        }
        if (result == 0) {
            std::cerr << "write returned zero fd=" << connection.fd << '\n';
            return false;
        }

        const size_t chunk = static_cast<size_t>(result);
        if (chunk < connection.pending_bytes()) {
            ++stats.partial_writes;
            std::cout << "SHORT_WRITE fd=" << connection.fd
                      << " wrote=" << chunk
                      << " remaining=" << connection.pending_bytes() - chunk
                      << '\n';
        }
        connection.pending_offset += chunk;
        if (connection.pending_offset == connection.pending.size()) {
            connection.pending.clear();
            connection.pending_offset = 0;
        }
    }
    return true;
}

// Returns false when the connection must be retired.
bool read_once(Connection& connection, Statistics& stats) {
    char buffer[kReadChunkSize];
    const size_t room =
        std::min(kReadChunkSize, kMaxPendingBytes - connection.pending_bytes());
    if (room == 0) {
        return true;
    }

    const ssize_t received = ::read(connection.fd, buffer, room);
    if (received < 0) {
        if (errno == EINTR) {
            return true;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        std::cerr << "read failed fd=" << connection.fd << ": "
                  << std::strerror(errno) << '\n';
        if (errno == ECONNRESET) {
            ++stats.reset;
        }
        return false;
    }
    if (received == 0) {
        connection.read_eof = true;
        ++stats.read_eof;
        std::cout << "READ_EOF fd=" << connection.fd
                  << " pending=" << connection.pending_bytes() << '\n';
        return true;
    }

    const size_t old_size = connection.pending.size();
    if (old_size == 0) {
        connection.pending.assign(buffer, buffer + received);
    } else {
        connection.pending.insert(connection.pending.end(), buffer,
                                  buffer + received);
    }
    std::cout << "READ fd=" << connection.fd << " bytes=" << received
              << " pending=" << connection.pending_bytes() << '\n';
    return true;
}

int parse_positive_int(const char* text) {
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value <= 0 ||
        value > 1024 * 1024 * 1024L) {
        return -1;
    }
    return static_cast<int>(value);
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    struct sigaction action {};
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    ::sigaction(SIGINT, &action, nullptr);
    ::sigaction(SIGTERM, &action, nullptr);

    int port = 0;
    int send_buffer = 0;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--sndbuf" && index + 1 < argc) {
            send_buffer = parse_positive_int(argv[++index]);
            if (send_buffer < 0) {
                std::cerr << "invalid --sndbuf value\n";
                return 2;
            }
        } else {
            const int parsed_port = parse_positive_int(argv[index]);
            if (parsed_port < 0 || parsed_port > 65535) {
                std::cerr << "usage: socket_echo_nonblocking [port] "
                             "[--sndbuf bytes]\n";
                return 2;
            }
            port = parsed_port;
        }
    }

    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket failed: " << std::strerror(errno) << '\n';
        return 1;
    }

    int option = 1;
    if (::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &option,
                     sizeof(option)) < 0 ||
        !set_nonblocking(listen_fd)) {
        std::cerr << "listen socket setup failed: " << std::strerror(errno)
                  << '\n';
        ::close(listen_fd);
        return 1;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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

    std::cout << "Non-blocking Echo Server listening on port " << port
              << " sndbuf=" << send_buffer << '\n';
    std::cout.flush();

    std::vector<std::unique_ptr<Connection>> connections;
    Statistics stats;

    while (g_stop == 0) {
        const size_t polled_connection_count = connections.size();
        std::vector<pollfd> poll_fds;
        poll_fds.reserve(polled_connection_count + 1);
        poll_fds.push_back({listen_fd, POLLIN, 0});
        for (const auto& connection : connections) {
            short events = 0;
            if (!connection->read_eof &&
                connection->pending_bytes() < kMaxPendingBytes) {
                events |= POLLIN;
            }
            if (connection->pending_bytes() > 0) {
                events |= POLLOUT;
            }
            poll_fds.push_back({connection->fd, events, 0});
        }

        const int ready = ::poll(poll_fds.data(), poll_fds.size(), 100);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "poll failed: " << std::strerror(errno) << '\n';
            break;
        }

        if ((poll_fds[0].revents & POLLIN) != 0) {
            while (true) {
                sockaddr_in peer{};
                socklen_t peer_length = sizeof(peer);
                const int connection_fd =
                    ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&peer),
                              &peer_length, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (connection_fd < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    std::cerr << "accept failed: " << std::strerror(errno)
                              << '\n';
                    break;
                }
                if (send_buffer > 0 &&
                    ::setsockopt(connection_fd, SOL_SOCKET, SO_SNDBUF,
                                 &send_buffer, sizeof(send_buffer)) < 0) {
                    std::cerr << "setsockopt(SO_SNDBUF) failed: "
                              << std::strerror(errno) << '\n';
                    ::close(connection_fd);
                    continue;
                }
                auto connection = std::make_unique<Connection>();
                connection->fd = connection_fd;
                char peer_ip[INET_ADDRSTRLEN] = {};
                ::inet_ntop(AF_INET, &peer.sin_addr, peer_ip, sizeof(peer_ip));
                connection->peer = std::string(peer_ip) + ':' +
                                   std::to_string(ntohs(peer.sin_port));
                std::cout << "ACCEPT fd=" << connection_fd
                          << " peer=" << connection->peer << '\n';
                connections.push_back(std::move(connection));
                ++stats.accepted;
            }
        }

        for (size_t index = 0; index < polled_connection_count; ++index) {
            Connection& connection = *connections[index];
            bool alive = true;

            // Flush first.  A connection whose pending buffer is bounded and
            // non-empty is not read again until room becomes available.
            if (connection.pending_bytes() > 0) {
                alive = flush_pending(connection, stats);
            }
            const short revents = poll_fds[index + 1].revents;
            if (alive &&
                (revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0) {
                alive = read_once(connection, stats);
                if (alive) {
                    alive = flush_pending(connection, stats);
                }
            }
            if (!alive) {
                close_connection(connection);
            } else if (connection.read_eof && connection.pending_bytes() == 0) {
                // All accepted bytes are in the kernel send queue.  SHUT_WR
                // emits FIN without discarding those bytes.
                if (::shutdown(connection.fd, SHUT_WR) < 0 &&
                    errno != ENOTCONN && errno != EPIPE) {
                    std::cerr << "shutdown failed fd=" << connection.fd << ": "
                              << std::strerror(errno) << '\n';
                }
                close_connection(connection);
            }
        }

        connections.erase(
            std::remove_if(connections.begin(), connections.end(),
                           [](const std::unique_ptr<Connection>& connection) {
                               return connection->fd < 0;
                           }),
            connections.end());
    }

    for (auto& connection : connections) {
        close_connection(*connection);
    }
    ::close(listen_fd);
    std::cout << "STATS accepted=" << stats.accepted
              << " eagain=" << stats.eagain
              << " short_writes=" << stats.partial_writes
              << " read_eof=" << stats.read_eof << " reset=" << stats.reset
              << '\n';
    return 0;
}
