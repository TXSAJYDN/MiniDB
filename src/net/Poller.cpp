#include "Poller.h"
#include "Channel.h"
#include <sys/epoll.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include "../base/Log.h"

namespace minidb {

namespace {

class PosixEpollOperations final : public EpollOperations {
  public:
    int create() override {
        return ::epoll_create1(EPOLL_CLOEXEC);
    }
    int ctl(int epoll_fd, int operation, int fd,
            struct epoll_event* event) override {
        return ::epoll_ctl(epoll_fd, operation, fd, event);
    }
    int wait(int epoll_fd, struct epoll_event* events, int max_events,
             int timeout_ms) override {
        return ::epoll_wait(epoll_fd, events, max_events, timeout_ms);
    }
    int close(int fd) override {
        return ::close(fd);
    }
};

} // namespace

std::shared_ptr<EpollOperations> make_default_epoll_operations() {
    return std::make_shared<PosixEpollOperations>();
}

Poller::Poller(EventLoop* loop)
    : Poller(loop, make_default_epoll_operations()) {}

Poller::Poller(EventLoop*, std::shared_ptr<EpollOperations> operations)
    : operations_(std::move(operations)), epollfd_(-1), events_(16),
      next_token_(1) {
    if (!operations_) {
        operations_ = make_default_epoll_operations();
    }
    epollfd_ = operations_->create();
    if (epollfd_ < 0) {
        throw std::runtime_error("epoll_create1() failed");
    }
}

Poller::~Poller() {
    if (epollfd_ >= 0) {
        operations_->close(epollfd_);
    }
}

void Poller::poll(int timeout_ms, ChannelList* active_channels) {
    int num_events = operations_->wait(
        epollfd_, events_.data(), static_cast<int>(events_.size()), timeout_ms);
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
        const auto found = channels_by_token_.find(events_[i].data.u64);
        if (found == channels_by_token_.end()) {
            // The registration was retired after epoll_wait copied this
            // event.  The token is never reused, so the stale event is safe
            // to ignore even if its fd has already been reused.
            continue;
        }
        Channel* channel = found->second;
        channel->set_revents(events_[i].events);
        active_channels->push_back(ActiveChannel{channel, events_[i].data.u64});
    }
}

void Poller::update_channel(Channel* channel) {
    const int fd = channel->fd();
    auto it = channels_.find(fd);

    if (it == channels_.end()) {
        if (channel->is_none_event()) {
            return;
        }
        const std::uint64_t token = allocate_token();
        if (!update(EPOLL_CTL_ADD, fd, channel->events(), token)) {
            return;
        }
        channels_[fd] = Registration{channel, token};
        channels_by_token_[token] = channel;
        return;
    }

    assert(it->second.channel == channel);
    if (channel->is_none_event()) {
        // Remove the local registration unconditionally.  If DEL failed for
        // a reason other than ENOENT, the kernel may still deliver queued
        // events for this token, but fill_active_channels() will discard
        // them instead of dereferencing a soon-to-be-stale Channel pointer.
        update(EPOLL_CTL_DEL, fd, 0, it->second.token);
        channels_by_token_.erase(it->second.token);
        channels_.erase(it);
        return;
    }

    if (!update(EPOLL_CTL_MOD, fd, channel->events(), it->second.token) &&
        errno == ENOENT) {
        // The kernel may lose an fd after an external close/reuse.  Re-add
        // under a fresh token so an event queued for the old registration
        // cannot be mistaken for the new one.
        const std::uint64_t old_token = it->second.token;
        const std::uint64_t new_token = allocate_token();
        channels_by_token_.erase(old_token);
        if (update(EPOLL_CTL_ADD, fd, channel->events(), new_token)) {
            it->second = Registration{channel, new_token};
            channels_by_token_[new_token] = channel;
        } else {
            channels_.erase(it);
        }
    }
}

bool Poller::is_current(Channel* channel, std::uint64_t token) const {
    const auto found = channels_by_token_.find(token);
    return found != channels_by_token_.end() && found->second == channel;
}

void Poller::remove_channel(Channel* channel) {
    const int fd = channel->fd();
    auto it = channels_.find(fd);
    if (it == channels_.end()) {
        return;
    }
    assert(it->second.channel == channel);
    update(EPOLL_CTL_DEL, fd, 0, it->second.token);
    channels_by_token_.erase(it->second.token);
    channels_.erase(it);
}

bool Poller::update(int operation, int fd, int events, std::uint64_t token) {
    struct epoll_event event;
    std::memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.u64 = token;

    int result = -1;
    do {
        result = operations_->ctl(epollfd_, operation, fd, &event);
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

std::uint64_t Poller::allocate_token() {
    if (next_token_ == 0) {
        throw std::overflow_error("epoll registration token space exhausted");
    }
    const std::uint64_t token = next_token_++;
    if (next_token_ == 0) {
        // Token 0 is never issued.  Reaching this branch requires 2^64
        // registrations over the process lifetime; fail closed rather than
        // risk token aliasing if the counter wraps.
        next_token_ = std::numeric_limits<std::uint64_t>::max();
    }
    return token;
}

} // namespace minidb
