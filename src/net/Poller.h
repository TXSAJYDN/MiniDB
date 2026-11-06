#pragma once
#include <vector>
#include <map>
#include <sys/epoll.h>

namespace minidb {
class Channel;
class EventLoop;

class Poller {
  public:
    using ChannelList = std::vector<Channel*>;
    explicit Poller(EventLoop* loop);
    ~Poller();
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    void poll(int timeout_ms, ChannelList* active_channels);
    void update_channel(Channel* channel);
    void remove_channel(Channel* channel);

  private:
    void fill_active_channels(int num_events,
                              ChannelList* active_channels) const;
    bool update(int operation, Channel* channel);

    using EventList = std::vector<struct epoll_event>;
    using ChannelMap = std::map<int, Channel*>;

    EventLoop* owner_loop_;
    int epollfd_;
    EventList events_;
    ChannelMap channels_;
};
} // namespace minidb
