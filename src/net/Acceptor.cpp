#include "Acceptor.h"
#include "../base/Log.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace minidb {

Acceptor::Acceptor(EventLoop* loop, const InetAddress& listen_addr)
    : loop_(loop), accept_socket_(Socket::create_tcp_socket()),
      accept_channel_(loop, accept_socket_.fd()), listening_(false),
      accept_paused_(false),
      idle_fd_(::open("/dev/null", O_RDONLY | O_CLOEXEC)), retry_timer_id_(0) {
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
        ::close(idle_fd_);
    }
}

void Acceptor::listen() {
    loop_->assert_in_loop_thread();
    listening_ = true;
    accept_socket_.listen();
    accept_channel_.enable_reading();
}

void Acceptor::pause_accepting() {
    if (accept_paused_) {
        return;
    }
    accept_paused_ = true;
    accept_channel_.disable_reading();
    retry_timer_id_ = loop_->run_after(kAcceptRetryDelayMs, [this]() {
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
        const int connfd = accept_socket_.accept(&peer_addr);
        if (connfd >= 0) {
            if (new_connection_callback_) {
                new_connection_callback_(connfd, peer_addr);
            } else {
                ::close(connfd);
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
                ::close(idle_fd_);
                idle_fd_ = -1;
                const int dummy_fd = accept_socket_.accept(&peer_addr);
                if (dummy_fd >= 0) {
                    ::close(dummy_fd);
                }
                idle_fd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
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
