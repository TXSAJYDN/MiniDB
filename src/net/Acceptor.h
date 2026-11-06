#pragma once
#include "Socket.h"
#include "Channel.h"
#include "InetAddress.h"
#include "EventLoop.h"
#include <cstdint>
#include <functional>

namespace minidb {

class Acceptor {
  public:
    using NewConnectionCallback =
        std::function<void(int sockfd, const InetAddress&)>;

    Acceptor(EventLoop* loop, const InetAddress& listen_addr);
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
    void handle_read();
    void resume_accepting();
    void pause_accepting();

    EventLoop* loop_;
    Socket accept_socket_;
    Channel accept_channel_;
    NewConnectionCallback new_connection_callback_;
    bool listening_;
    bool accept_paused_;
    int idle_fd_;
    EventLoop::TimerId retry_timer_id_;

    static constexpr int kMaxAcceptsPerEvent = 1024;
    static constexpr int64_t kAcceptRetryDelayMs = 1000;
};
} // namespace minidb
