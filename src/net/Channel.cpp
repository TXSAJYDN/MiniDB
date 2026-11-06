#include "Channel.h"
#include "EventLoop.h"
#include <sys/epoll.h>

namespace minidb {

const int Channel::kNoneEvent = 0;
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI | EPOLLRDHUP;
const int Channel::kWriteEvent = EPOLLOUT;

Channel::Channel(EventLoop* loop, int fd)
    : loop_(loop), fd_(fd), events_(0), revents_(0),
      dispatch_state_(std::make_shared<DispatchState>()), tied_(false),
      added_to_loop_(false) {}

Channel::~Channel() {
    dispatch_state_->alive = false;
    if (added_to_loop_ && loop_ != nullptr) {
        loop_->assert_in_loop_thread();
        loop_->remove_channel(this);
        added_to_loop_ = false;
    }
}

void Channel::update() {
    if (loop_ == nullptr) {
        return;
    }
    loop_->update_channel(this);
    added_to_loop_ = !is_none_event();
}

void Channel::remove() {
    if (loop_ != nullptr && added_to_loop_) {
        loop_->remove_channel(this);
    }
    added_to_loop_ = false;
}

void Channel::handle_event() {
    // Snapshot everything needed from this object before invoking user code.
    // A callback may destroy the owning connection (and therefore this
    // Channel), so no member of *this may be accessed after the first callback.
    const std::shared_ptr<DispatchState> state = dispatch_state_;
    const int revents = revents_;
    const EventCallback error_callback = error_callback_;
    const EventCallback read_callback = read_callback_;
    const EventCallback write_callback = write_callback_;
    const EventCallback close_callback = close_callback_;

    std::shared_ptr<void> guard;
    if (tied_) {
        guard = tie_.lock();
        if (!guard) {
            return;
        }
    }

    // Event ordering is part of the connection contract:
    //   error -> read -> write -> peer close.
    // A readable half-close can carry the final request, so data is consumed
    // before the write side is drained and before the connection is retired.
    if ((revents & EPOLLERR) && error_callback) {
        error_callback();
        if (!state->alive) {
            return;
        }
    }

    // EPOLLHUP can arrive without EPOLLIN even while the receive queue
    // still contains the peer's final payload.  Drain it before close.
    const bool readable =
        (revents & (EPOLLIN | EPOLLPRI | EPOLLRDHUP | EPOLLHUP)) != 0;
    if (readable && read_callback) {
        read_callback();
        if (!state->alive) {
            return;
        }
    }

    if ((revents & EPOLLOUT) && write_callback) {
        write_callback();
        if (!state->alive) {
            return;
        }
    }

    if ((revents & (EPOLLHUP | EPOLLRDHUP)) && close_callback) {
        close_callback();
    }
}

} // namespace minidb
