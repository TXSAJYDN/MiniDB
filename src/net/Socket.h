#pragma once
#include "../base/FileDescriptor.h"
#include "InetAddress.h"

namespace minidb {
class Socket {
  public:
    explicit Socket(int sockfd);
    Socket(Socket&&) = default;
    Socket& operator=(Socket&&) = default;
    ~Socket() = default;

    int fd() const {
        return sockfd_.get();
    }
    void bind(const InetAddress& addr);
    void listen();
    int accept(InetAddress* peer_addr);
    bool set_reuse_addr(bool on);
    bool set_reuse_port(bool on);
    bool set_keep_alive(bool on);
    bool set_tcp_no_delay(bool on);
    bool shutdown_write();
    void close();
    InetAddress local_address() const;
    static Socket create_tcp_socket();

  private:
    bool set_socket_option(int level, int option, bool on);

    FileDescriptor sockfd_;
};
} // namespace minidb
