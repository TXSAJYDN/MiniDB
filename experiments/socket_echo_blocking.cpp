// Blocking TCP Echo Server Experiment
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

constexpr int BUFFER_SIZE = 4096;

int main(int argc, char* argv[]) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 0;  // 0 = dynamic port

    // Create socket
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        std::cerr << "socket() failed: " << strerror(errno) << std::endl;
        return 1;
    }

    // SO_REUSEADDR for quick restart
    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        std::cerr << "setsockopt(SO_REUSEADDR) failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }

    // Bind
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind() failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }

    // Get actual port if dynamic
    if (port == 0) {
        socklen_t addr_len = sizeof(addr);
        if (getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0) {
            std::cerr << "getsockname() failed: " << strerror(errno) << std::endl;
            close(listen_fd);
            return 1;
        }
        port = ntohs(addr.sin_port);
    }

    // Listen
    if (listen(listen_fd, SOMAXCONN) < 0) {
        std::cerr << "listen() failed: " << strerror(errno) << std::endl;
        close(listen_fd);
        return 1;
    }

    std::cout << "Blocking Echo Server listening on port " << port << std::endl;

    // Accept loop
    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);

        int conn_fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (conn_fd < 0) {
            if (errno == EINTR) {
                std::cout << "accept() interrupted by signal, retrying..." << std::endl;
                continue;
            }
            std::cerr << "accept() failed: " << strerror(errno) << std::endl;
            break;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        std::cout << "Accepted connection from " << client_ip << ":"
                  << ntohs(client_addr.sin_port) << std::endl;

        // Echo loop
        char buffer[BUFFER_SIZE];
        while (true) {
            ssize_t n = read(conn_fd, buffer, sizeof(buffer));

            if (n < 0) {
                if (errno == EINTR) {
                    std::cout << "read() interrupted by signal, retrying..." << std::endl;
                    continue;
                }
                std::cerr << "read() failed: " << strerror(errno) << std::endl;
                break;
            }

            if (n == 0) {
                std::cout << "Client closed connection (EOF)" << std::endl;
                break;
            }

            std::cout << "Read " << n << " bytes" << std::endl;

            // Echo back
            ssize_t written = 0;
            while (written < n) {
                ssize_t w = write(conn_fd, buffer + written, n - written);
                if (w < 0) {
                    if (errno == EINTR) {
                        std::cout << "write() interrupted by signal, retrying..." << std::endl;
                        continue;
                    }
                    if (errno == EPIPE) {
                        std::cerr << "write() failed: EPIPE (broken pipe)" << std::endl;
                    } else {
                        std::cerr << "write() failed: " << strerror(errno) << std::endl;
                    }
                    goto close_conn;
                }
                written += w;
                if (w < n - written) {
                    std::cout << "Partial write: " << w << " bytes" << std::endl;
                }
            }
        }

    close_conn:
        if (shutdown(conn_fd, SHUT_RDWR) < 0) {
            std::cerr << "shutdown() failed: " << strerror(errno) << std::endl;
        }
        close(conn_fd);
        std::cout << "Connection closed" << std::endl;
    }

    close(listen_fd);
    return 0;
}
