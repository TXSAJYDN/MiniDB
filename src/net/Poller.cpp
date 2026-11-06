#include "Poller.h"
#include "Channel.h"
#include <sys/epoll.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <cerrno>
#include <stdexcept>
#include "../base/Log.h"

namespace minidb {

Poller::Poller(EventLoop* loop)
    : owner_loop_(loop), epollfd_(::epoll_create1(EPOLL_CLOEXEC)), events_(16) {
    if (epollfd_ < 0) {
        throw std::runtime_error("epoll_create1() failed");
    }
}

Poller::~Poller() {
    ::close(epollfd_);
}

void Poller::poll(int timeout_ms, ChannelList* active_channels) {
    int num_events = ::epoll_wait(epollfd_, &*events_.begin(),
                                  static_cast<int>(events_.size()), timeout_ms);
    int saved_errno = errno;

    if (num_events > 0) {
        fill_active_channels(num_events, active_channels);
        if (static_cast<size_t>(num_events) == events_.size()) {
            events_.resize(events_.size() * 2);
        }
    } else if (num_events == 0) {
        // Timeout
    } else {
        if (saved_errno != EINTR) {
            errno = saved_errno;
            Log::Error(std::string("epoll_wait failed: ") +
                       std::strerror(saved_errno));
        }
    }
}

void Poller::fill_active_channels(int num_events,
                                  ChannelList* active_channels) const {
    for (int i = 0; i < num_events; ++i) {
        Channel* channel = static_cast<Channel*>(events_[i].data.ptr);
        channel->set_revents(events_[i].events);
        active_channels->push_back(channel);
    }
}

void Poller::update_channel(Channel* channel) {
    const int fd = channel->fd();
    auto it = channels_.find(fd);

    if (it == channels_.end()) {
        if (channel->is_none_event()) {
            return;
        }
        if (!update(EPOLL_CTL_ADD, channel)) {
            return;
        }
        channels_[fd] = channel;
        return;
    }

    assert(it->second == channel);
    if (channel->is_none_event()) {
        if (update(EPOLL_CTL_DEL, channel) || errno == ENOENT) {
            channels_.erase(it);
        }
        return;
    }

    if (!update(EPOLL_CTL_MOD, channel) && errno == ENOENT) {
        // The kernel may lose an fd after an external close/reuse.  Re-add
        // the channel only when the exact registration can be recovered.
        if (update(EPOLL_CTL_ADD, channel)) {
            it->second = channel;
        } else {
            channels_.erase(it);
        }
    }
}

void Poller::remove_channel(Channel* channel) {
    const int fd = channel->fd();
    auto it = channels_.find(fd);
    if (it == channels_.end()) {
        return;
    }
    assert(it->second == channel);
    if (update(EPOLL_CTL_DEL, channel) || errno == ENOENT) {
        channels_.erase(it);
    }
}

bool Poller::update(int operation, Channel* channel) {
    struct epoll_event event;
    std::memset(&event, 0, sizeof(event));
    event.events = channel->events();
    event.data.ptr = channel;
    const int fd = channel->fd();

    int result = -1;
    do {
        result = ::epoll_ctl(epollfd_, operation, fd, &event);
    } while (result < 0 && errno == EINTR);

    if (result == 0) {
        return true;
    }

    const int saved_errno = errno;
    if ((operation == EPOLL_CTL_DEL && saved_errno == ENOENT) ||
        (operation == EPOLL_CTL_ADD && saved_errno == EEXIST)) {
        errno = saved_errno;
        return false;
    }
    errno = saved_errno;
    Log::Error(std::string("epoll_ctl failed for fd ") + std::to_string(fd) +
               ": " + std::strerror(saved_errno));
    return false;
}

} // namespace minidb
