#include "Channel.h"
#include "EventLoop.h"
#include <sys/epoll.h>

namespace minidb {

const int Channel::kNoneEvent = 0;
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI | EPOLLRDHUP;
const int Channel::kWriteEvent = EPOLLOUT;

Channel::Channel(EventLoop* loop, int fd)
    : loop_(loop), fd_(fd), events_(0), revents_(0), added_to_loop_(false) {}

Channel::~Channel() = default;

void Channel::update() {
    loop_->update_channel(this);
}

void Channel::remove() {
    loop_->remove_channel(this);
}

void Channel::handle_event() {
    // Event ordering is part of the connection contract:
    //   error -> read -> write -> peer close.
    // A readable half-close can carry the final request, so data is consumed
    // before the write side is drained and before the connection is retired.
    if ((revents_ & EPOLLERR) && error_callback_) {
        error_callback_();
    }

    const bool readable = (revents_ & (EPOLLIN | EPOLLPRI | EPOLLRDHUP)) != 0;
    if (readable && read_callback_) {
        read_callback_();
    }

    if ((revents_ & EPOLLOUT) && write_callback_) {
        write_callback_();
    }

    if ((revents_ & (EPOLLHUP | EPOLLRDHUP)) && close_callback_) {
        close_callback_();
    }
}

} // namespace minidb
