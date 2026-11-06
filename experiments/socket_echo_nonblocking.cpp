// Non-blocking TCP Echo Server Experiment
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <vector>

constexpr int BUFFER_SIZE = 4096;

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        std::cerr << "fcntl(F_GETFL) failed: " << strerror(errno) << std::endl;
        return false;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        std::cerr << "fcntl(F_SETFL) failed: " << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 0;

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket() failed: " << strerror(errno) << std::endl;
        return 1;
    }

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt(SO_REUSEADDR) failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }

    if (!set_nonblocking(listen_fd)) {
        close(listen_fd);
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind() failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }

    if (port == 0) {
        socklen_t addr_len = sizeof(addr);
        if (getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0) {
            std::cerr << "getsockname() failed: " << strerror(errno) << std::endl;
            close(listen_fd);
            return 1;
        }
        port = ntohs(addr.sin_port);
    }

    if (listen(listen_fd, SOMAXCONN) < 0) {
        std::cerr << "listen() failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }

    std::cout << "Non-blocking Echo Server listening on port " << port << std::endl;

    std::vector<int> active_fds;

    while (true) {
        // Try accept
        while (true) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);

            int conn_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

            if (conn_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    std::cout << "accept() interrupted, retrying..." << std::endl;
                    continue;
                }
                std::cerr << "accept() failed: " << strerror(errno) << std::endl;
                break;
            }

            if (!set_nonblocking(conn_fd)) {
                close(conn_fd);
                continue;
            }

            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
            std::cout << "Accepted fd=" << conn_fd << " from " << client_ip << ":"
                      << ntohs(client_addr.sin_port) << std::endl;

            active_fds.push_back(conn_fd);
        }

        // Service connections
        for (auto it = active_fds.begin(); it != active_fds.end();) {
            int fd = *it;
            char buffer[BUFFER_SIZE];

            ssize_t n = read(fd, buffer, sizeof(buffer));

            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    ++it;
                    continue;
                }
                if (errno == EINTR) {
                    continue;
                }
                std::cerr << "read() failed on fd=" << fd << ": " << strerror(errno) << std::endl;
                close(fd);
                it = active_fds.erase(it);
                continue;
            }

            if (n == 0) {
                std::cout << "EOF on fd=" << fd << std::endl;
                close(fd);
                it = active_fds.erase(it);
                continue;
            }

            std::cout << "Read " << n << " bytes from fd=" << fd << std::endl;

            // Echo back
            ssize_t written = 0;
            while (written < n) {
                ssize_t w = write(fd, buffer + written, n - written);

                if (w < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        std::cout << "write() would block on fd=" << fd << std::endl;
                        usleep(1000);
                        continue;
                    }
                    if (errno == EINTR) {
                        continue;
                    }
                    if (errno == EPIPE) {
                        std::cerr << "EPIPE on fd=" << fd << std::endl;
                    } else {
                        std::cerr << "write() failed on fd=" << fd << ": " << strerror(errno) << std::endl;
                    }
                    close(fd);
                    it = active_fds.erase(it);
                    goto next_fd;
                }

                written += w;
                if (w < n - written) {
                    std::cout << "Partial write: " << w << " bytes on fd=" << fd << std::endl;
                }
            }

            ++it;
        next_fd:
            continue;
        }

        usleep(10000); // 10ms
    }

    close(listen_fd);
    return 0;
}
