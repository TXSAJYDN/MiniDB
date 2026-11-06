#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>
#include <sys/epoll.h>

namespace minidb {
class Channel;
class EventLoop;

// Narrow seam around the epoll syscalls used by Poller.  Tests can inject
// deterministic failures without making the production path conditional.
class EpollOperations {
  public:
    virtual ~EpollOperations() = default;
    virtual int create() = 0;
    virtual int ctl(int epoll_fd, int operation, int fd,
                    struct epoll_event* event) = 0;
    virtual int wait(int epoll_fd, struct epoll_event* events, int max_events,
                     int timeout_ms) = 0;
    virtual int close(int fd) = 0;
};

std::shared_ptr<EpollOperations> make_default_epoll_operations();

class Poller {
  public:
    using ChannelList = std::vector<Channel*>;
    explicit Poller(EventLoop* loop);
    Poller(EventLoop* loop, std::shared_ptr<EpollOperations> operations);
    ~Poller();
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    void poll(int timeout_ms, ChannelList* active_channels);
    void update_channel(Channel* channel);
    void remove_channel(Channel* channel);

  private:
    struct Registration {
        Channel* channel;
        std::uint64_t token;
    };

    void fill_active_channels(int num_events,
                              ChannelList* active_channels) const;
    bool update(int operation, int fd, int events, std::uint64_t token);
    std::uint64_t allocate_token();

    using EventList = std::vector<struct epoll_event>;
    using ChannelMap = std::map<int, Registration>;
    using TokenMap = std::unordered_map<std::uint64_t, Channel*>;

    EventLoop* owner_loop_;
    std::shared_ptr<EpollOperations> operations_;
    int epollfd_;
    EventList events_;
    ChannelMap channels_;
    TokenMap channels_by_token_;
    std::uint64_t next_token_;
};
} // namespace minidb
