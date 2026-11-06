#include "TcpConnection.h"
#include "EventLoop.h"
#include "Channel.h"
#include "Socket.h"
#include "../base/Log.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace minidb {
namespace {

ssize_t send_no_signal(int fd, const void* data, size_t len) {
    ssize_t result = -1;
    do {
        result = ::send(fd, data, len, MSG_NOSIGNAL);
    } while (result < 0 && errno == EINTR);
    return result;
}

bool would_block(int error) {
    return error == EAGAIN || error == EWOULDBLOCK;
}

bool fatal_socket_error(int error) {
    return error == EPIPE || error == ECONNRESET || error == ENOTCONN ||
           error == ECONNABORTED || error == ETIMEDOUT;
}

} // namespace

TcpConnection::TcpConnection(EventLoop* loop, const std::string& name,
                             int sockfd, const InetAddress& local_addr,
                             const InetAddress& peer_addr)
    : loop_(loop), name_(name), state_(kConnecting),
      socket_(std::make_unique<Socket>(sockfd)),
      channel_(std::make_unique<Channel>(loop, sockfd)),
      local_addr_(local_addr), peer_addr_(peer_addr), reading_(true),
      channel_removed_(false), peer_eof_(false), close_after_write_(false),
      shutdown_write_done_(false) {
    channel_->set_read_callback([this]() { handle_read(); });
    channel_->set_write_callback([this]() { handle_write(); });
    channel_->set_close_callback([this]() { handle_peer_close(); });
    channel_->set_error_callback([this]() { handle_error(); });
}

TcpConnection::~TcpConnection() {
    if (!channel_removed_ && channel_) {
        channel_->disable_all();
    }
}

void TcpConnection::connect_established() {
    loop_->assert_in_loop_thread();
    if (state_ != kConnecting) {
        return;
    }
    set_state(kConnected);
    reading_ = true;
    channel_->enable_reading();
    if (connection_callback_) {
        connection_callback_(shared_from_this());
    }
}

void TcpConnection::connect_destroyed() {
    loop_->assert_in_loop_thread();
    if (state_ != kDisconnected) {
        set_state(kDisconnected);
        reading_ = false;
        channel_->disable_all();
        if (connection_callback_) {
            connection_callback_(shared_from_this());
        }
    }
    if (!channel_removed_) {
        channel_->remove();
        channel_removed_ = true;
    }
}

void TcpConnection::stop_reading() {
    loop_->assert_in_loop_thread();
    if (reading_ && state_ != kDisconnected) {
        reading_ = false;
        channel_->disable_reading();
    }
}

void TcpConnection::start_reading() {
    loop_->assert_in_loop_thread();
    if (reading_ || peer_eof_ || close_after_write_ ||
        (state_ != kConnected && state_ != kDisconnecting)) {
        return;
    }
    reading_ = true;
    channel_->enable_reading();

    // The session may have backpressured while the connection input buffer
    // still held a pipelined tail.  Level-triggered epoll has no new edge for
    // bytes that are already in user space, so explicitly re-drive delivery.
    if (input_buffer_.readable_bytes() > 0 && state_ == kConnected) {
        auto self = shared_from_this();
        loop_->queue_in_loop([self = std::move(self)]() {
            if (self->reading_ && self->state_ == kConnected) {
                self->handle_read();
            }
        });
    }
}

bool TcpConnection::send(const void* data, size_t len) {
    if (len == 0) {
        return true;
    }
    if (data == nullptr || len > kMaxOutputBufferSize) {
        return false;
    }
    if (loop_->is_in_loop_thread()) {
        return send_in_loop(static_cast<const char*>(data), len);
    }

    std::string copy(static_cast<const char*>(data), len);
    auto self = shared_from_this();
    loop_->queue_in_loop(
        [self = std::move(self), copy = std::move(copy)]() mutable {
            self->send_in_loop(copy.data(), copy.size());
        });
    return true;
}

bool TcpConnection::send_in_loop(const char* data, size_t len) {
    loop_->assert_in_loop_thread();
    if (len == 0) {
        return true;
    }
    if (data == nullptr || (state_ != kConnected && state_ != kDisconnecting) ||
        channel_removed_ || close_after_write_) {
        return false;
    }
    if (len > kMaxOutputBufferSize) {
        force_close("single send exceeds output budget");
        return false;
    }

    size_t written = 0;
    if (!channel_->is_writing() && output_buffer_.readable_bytes() == 0) {
        const ssize_t n = send_no_signal(channel_->fd(), data, len);
        if (n > 0) {
            written = static_cast<size_t>(n);
        } else if (n < 0) {
            const int error = errno;
            if (!would_block(error)) {
                force_close(std::strerror(error));
                return false;
            }
        } else {
            force_close("send returned zero");
            return false;
        }

        if (written == len) {
            if (write_complete_callback_) {
                write_complete_callback_(shared_from_this());
            }
            if (state_ != kDisconnected && close_after_write_ &&
                output_buffer_.readable_bytes() == 0) {
                finish_after_write();
            }
            return state_ != kDisconnected;
        }
    }

    const size_t remaining = len - written;
    if (remaining > kMaxOutputBufferSize ||
        output_buffer_.readable_bytes() > kMaxOutputBufferSize - remaining) {
        force_close("output buffer limit exceeded");
        return false;
    }
    output_buffer_.append(data + written, remaining);
    if (!channel_->is_writing() && state_ != kDisconnected) {
        channel_->enable_writing();
    }
    return state_ != kDisconnected;
}

void TcpConnection::shutdown() {
    if (!loop_->is_in_loop_thread()) {
        auto self = shared_from_this();
        loop_->queue_in_loop([self = std::move(self)]() { self->shutdown(); });
        return;
    }
    loop_->assert_in_loop_thread();
    if (state_ == kDisconnected || close_after_write_) {
        return;
    }
    if (state_ != kConnected && state_ != kDisconnecting) {
        return;
    }

    close_after_write_ = true;
    reading_ = false;
    channel_->disable_reading();
    if (output_buffer_.readable_bytes() == 0 && !channel_->is_writing()) {
        finish_after_write();
    }
}

void TcpConnection::note_peer_eof() {
    if (peer_eof_) {
        return;
    }
    peer_eof_ = true;
    reading_ = false;
    if (state_ != kDisconnected) {
        channel_->disable_reading();
    }
    if (output_buffer_.readable_bytes() == 0 && !channel_->is_writing()) {
        handle_close();
    }
}

void TcpConnection::handle_read() {
    loop_->assert_in_loop_thread();
    auto guard = shared_from_this();
    while (state_ != kDisconnected && reading_ && !peer_eof_) {
        if (input_buffer_.readable_bytes() > 0) {
            if (!message_callback_) {
                force_close("readable input has no message callback");
                return;
            }
            const size_t before = input_buffer_.readable_bytes();
            message_callback_(guard, &input_buffer_);
            if (state_ == kDisconnected || !reading_ || peer_eof_) {
                return;
            }
            if (input_buffer_.readable_bytes() >= before) {
                force_close("input callback made no progress");
                return;
            }
            continue;
        }

        const size_t room =
            kMaxInputBufferSize -
            std::min(input_buffer_.readable_bytes(), kMaxInputBufferSize);
        if (room == 0) {
            force_close("input buffer limit exceeded");
            return;
        }
        const size_t max_read = std::min(room, kMaxReadChunkSize);
        int saved_errno = 0;
        const ssize_t n =
            input_buffer_.read_fd(channel_->fd(), &saved_errno, max_read);
        if (n > 0) {
            continue;
        }
        if (n == 0) {
            note_peer_eof();
            return;
        }
        if (would_block(saved_errno)) {
            return;
        }
        if (saved_errno == EINTR) {
            continue;
        }
        errno = saved_errno;
        if (fatal_socket_error(saved_errno)) {
            force_close(std::strerror(saved_errno));
            return;
        }
        handle_error();
        return;
    }
}

void TcpConnection::handle_write() {
    loop_->assert_in_loop_thread();
    auto guard = shared_from_this();
    if (state_ == kDisconnected || state_ == kConnecting) {
        return;
    }

    while (channel_->is_writing() && output_buffer_.readable_bytes() > 0) {
        const ssize_t n = send_no_signal(channel_->fd(), output_buffer_.peek(),
                                         output_buffer_.readable_bytes());
        if (n > 0) {
            output_buffer_.retrieve(static_cast<size_t>(n));
            if (output_buffer_.readable_bytes() == 0) {
                channel_->disable_writing();
                if (write_complete_callback_) {
                    write_complete_callback_(guard);
                }
                if (state_ == kDisconnected) {
                    return;
                }
                if (output_buffer_.readable_bytes() > 0) {
                    // A write-complete callback may have enqueued a follow-up
                    // response.  Keep draining it in this event.
                    if (!channel_->is_writing()) {
                        channel_->enable_writing();
                    }
                    continue;
                }
                if (close_after_write_ || peer_eof_) {
                    finish_after_write();
                }
                return;
            }
            continue;
        }
        if (n < 0 && would_block(errno)) {
            return;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        force_close(n == 0 ? "send returned zero" : std::strerror(errno));
        return;
    }

    if (state_ != kDisconnected && output_buffer_.readable_bytes() == 0 &&
        (close_after_write_ || peer_eof_)) {
        finish_after_write();
    }
}

void TcpConnection::handle_peer_close() {
    loop_->assert_in_loop_thread();
    if (state_ == kDisconnected) {
        return;
    }
    peer_eof_ = true;
    reading_ = false;
    channel_->disable_reading();
    if (output_buffer_.readable_bytes() == 0 && !channel_->is_writing()) {
        handle_close();
    } else if (!channel_->is_writing() && state_ != kDisconnected) {
        channel_->enable_writing();
    }
}

void TcpConnection::finish_after_write() {
    if (state_ == kDisconnected) {
        return;
    }
    if (output_buffer_.readable_bytes() != 0 || channel_->is_writing()) {
        return;
    }
    if (!shutdown_write_done_) {
        shutdown_write_done_ = true;
        // A failed shutdown still means the local descriptor must be retired.
        socket_->shutdown_write();
    }
    handle_close();
}

void TcpConnection::handle_close() {
    loop_->assert_in_loop_thread();
    if (state_ == kDisconnected) {
        return;
    }

    auto guard = shared_from_this();
    set_state(kDisconnected);
    reading_ = false;
    channel_->disable_all();
    if (!channel_removed_) {
        channel_->remove();
        channel_removed_ = true;
    }

    if (connection_callback_) {
        connection_callback_(guard);
    }
    if (state_ == kDisconnected && close_callback_) {
        close_callback_(guard);
    }
}

void TcpConnection::handle_error() {
    loop_->assert_in_loop_thread();
    int error = 0;
    socklen_t error_len = sizeof(error);
    if (::getsockopt(channel_->fd(), SOL_SOCKET, SO_ERROR, &error, &error_len) <
        0) {
        error = errno;
    }
    if (error != 0) {
        force_close(std::strerror(error));
    }
    // SO_ERROR == 0 means the error bit was a spurious readiness hint.  The
    // normal read/write paths remain authoritative.
}

void TcpConnection::force_close(const char* reason) {
    if (reason != nullptr && *reason != '\0') {
        Log::Warning("Closing " + name_ + ": " + reason);
    }
    handle_close();
}

} // namespace minidb