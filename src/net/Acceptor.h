#pragma once
#include "Socket.h"
#include "Channel.h"
#include "InetAddress.h"
#include "EventLoop.h"
#include <cstdint>
#include <functional>
#include <memory>

namespace minidb {

class AcceptorIo {
  public:
    virtual ~AcceptorIo() = default;
    virtual int accept_fd(int listen_fd, InetAddress* peer_addr) = 0;
    virtual int open_idle_fd() = 0;
    virtual int close_fd(int fd) = 0;
};

class Acceptor {
  public:
    using NewConnectionCallback =
        std::function<void(int sockfd, const InetAddress&)>;

    Acceptor(EventLoop* loop, const InetAddress& listen_addr);
    Acceptor(EventLoop* loop, const InetAddress& listen_addr,
             std::shared_ptr<AcceptorIo> io, int64_t retry_delay_ms = 1000);
    ~Acceptor();
    Acceptor(const Acceptor&) = delete;
    Acceptor& operator=(const Acceptor&) = delete;

    void set_new_connection_callback(const NewConnectionCallback& cb) {
        new_connection_callback_ = cb;
    }
    void listen();
    bool listening() const {
        return listening_;
    }
    InetAddress listen_address() const {
        return accept_socket_.local_address();
    }

  private:
    friend struct AcceptorTestPeer;

    void handle_read();
    void resume_accepting();
    void pause_accepting();

    EventLoop* loop_;
    Socket accept_socket_;
    Channel accept_channel_;
    std::shared_ptr<AcceptorIo> io_;
    NewConnectionCallback new_connection_callback_;
    bool listening_;
    bool accept_paused_;
    int idle_fd_;
    EventLoop::TimerId retry_timer_id_;
    int64_t accept_retry_delay_ms_;

    static constexpr int kMaxAcceptsPerEvent = 1024;
};
} // namespace minidb
