#include "Acceptor.h"
#include "../base/Log.h"
#include <cerrno>
#include <stdexcept>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace minidb {

namespace {

class PosixAcceptorIo final : public AcceptorIo {
  public:
    int accept_fd(int listen_fd, InetAddress* peer_addr) override {
        struct sockaddr_in addr;
        socklen_t len = sizeof(addr);
        std::memset(&addr, 0, sizeof(addr));
        const int fd =
            ::accept4(listen_fd, reinterpret_cast<struct sockaddr*>(&addr),
                      &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0 && peer_addr != nullptr) {
            *peer_addr = InetAddress(addr);
        }
        return fd;
    }

    int open_idle_fd() override {
        return ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    }

    int close_fd(int fd) override {
        return ::close(fd);
    }
};

} // namespace

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listen_addr)
    : Acceptor(loop, listen_addr, nullptr, 1000) {}

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listen_addr,
                   std::shared_ptr<AcceptorIo> io, int64_t retry_delay_ms)
    : loop_(loop), accept_socket_(Socket::create_tcp_socket()),
      accept_channel_(loop, accept_socket_.fd()),
      io_(io ? std::move(io) : std::make_shared<PosixAcceptorIo>()),
      listening_(false), accept_paused_(false), idle_fd_(io_->open_idle_fd()),
      retry_timer_id_(0),
      accept_retry_delay_ms_(retry_delay_ms > 0 ? retry_delay_ms : 1000) {
    accept_socket_.set_reuse_addr(true);
    accept_socket_.set_reuse_port(true);
    accept_socket_.bind(listen_addr);
    accept_channel_.set_read_callback([this]() { handle_read(); });
    if (idle_fd_ < 0) {
        Log::Warning("Unable to reserve idle fd for EMFILE recovery");
    }
}

Acceptor::~Acceptor() {
    if (retry_timer_id_ != 0) {
        loop_->cancel_timer(retry_timer_id_);
    }
    accept_channel_.disable_all();
    accept_channel_.remove();
    if (idle_fd_ >= 0) {
        io_->close_fd(idle_fd_);
    }
}

void Acceptor::listen() {
    loop_->assert_in_loop_thread();
    if (accept_socket_.fd() < 0) {
        throw std::logic_error("Acceptor cannot listen after stop");
    }
    listening_ = true;
    accept_socket_.listen();
    accept_channel_.enable_reading();
}

void Acceptor::stop() {
    loop_->assert_in_loop_thread();
    if (retry_timer_id_ != 0) {
        loop_->cancel_timer(retry_timer_id_);
        retry_timer_id_ = 0;
    }
    listening_ = false;
    accept_paused_ = false;
    accept_channel_.disable_all();
    accept_channel_.remove();
    accept_socket_.close();
}

void Acceptor::pause_accepting() {
    if (accept_paused_) {
        return;
    }
    accept_paused_ = true;
    accept_channel_.disable_reading();
    retry_timer_id_ = loop_->run_after(accept_retry_delay_ms_, [this]() {
        retry_timer_id_ = 0;
        resume_accepting();
    });
}

void Acceptor::resume_accepting() {
    if (!listening_ || !accept_paused_) {
        return;
    }
    accept_paused_ = false;
    accept_channel_.enable_reading();
}

void Acceptor::handle_read() {
    loop_->assert_in_loop_thread();
    if (accept_paused_) {
        return;
    }

    for (int accepted = 0; accepted < kMaxAcceptsPerEvent; ++accepted) {
        InetAddress peer_addr;
        const int connfd = io_->accept_fd(accept_socket_.fd(), &peer_addr);
        if (connfd >= 0) {
            if (new_connection_callback_) {
                new_connection_callback_(connfd, peer_addr);
            } else {
                io_->close_fd(connfd);
            }
            continue;
        }

        const int saved_errno = errno;
        if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) {
            return;
        }
        if (saved_errno == EINTR) {
            continue;
        }
        if (saved_errno == EMFILE || saved_errno == ENFILE) {
            if (idle_fd_ >= 0) {
                io_->close_fd(idle_fd_);
                idle_fd_ = -1;
                const int dummy_fd =
                    io_->accept_fd(accept_socket_.fd(), &peer_addr);
                if (dummy_fd >= 0) {
                    io_->close_fd(dummy_fd);
                }
                idle_fd_ = io_->open_idle_fd();
                if (idle_fd_ < 0) {
                    Log::Warning("Unable to restore idle fd after EMFILE");
                }
            }
            Log::Error(std::string("accept resource exhaustion: ") +
                       std::strerror(saved_errno) +
                       "; pausing accepts for 1000 ms");
            pause_accepting();
            return;
        }

        Log::Error(std::string("accept failed: ") + std::strerror(saved_errno));
        return;
    }

    pause_accepting();
}

} // namespace minidb
