#include <gtest/gtest.h>

#include "../src/net/Channel.h"
#include "../src/net/EventLoop.h"
#include "../src/net/Poller.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace minidb {
namespace {

class FakeEpollOperations final : public EpollOperations {
  public:
    struct CtlCall {
        int operation;
        int fd;
        std::uint32_t events;
        std::uint64_t token;
    };

    struct QueuedEvent {
        std::uint64_t token;
        std::uint32_t events;
    };

    struct Failure {
        int operation;
        int error;
    };

    int create() override {
        return kEpollFd;
    }

    int ctl(int epoll_fd, int operation, int fd,
            struct epoll_event* event) override {
        EXPECT_EQ(epoll_fd, kEpollFd);
        ctl_calls.push_back(
            CtlCall{operation, fd, event->events, event->data.u64});
        if (failure && failure->operation == operation) {
            errno = failure->error;
            failure.reset();
            return -1;
        }
        return 0;
    }

    int wait(int epoll_fd, struct epoll_event* events, int max_events,
             int /*timeout_ms*/) override {
        EXPECT_EQ(epoll_fd, kEpollFd);
        const int count =
            std::min<int>(max_events, static_cast<int>(queued_events.size()));
        for (int index = 0; index < count; ++index) {
            const QueuedEvent queued = queued_events.front();
            queued_events.pop_front();
            events[index] = {};
            events[index].events = queued.events;
            events[index].data.u64 = queued.token;
        }
        if (quit_after_wait) {
            quit_after_wait();
        }
        return count;
    }

    int close(int fd) override {
        EXPECT_EQ(fd, kEpollFd);
        ++close_calls;
        return 0;
    }

    void fail_next(int operation, int error) {
        failure = Failure{operation, error};
    }

    static constexpr int kEpollFd = 91;
    std::vector<CtlCall> ctl_calls;
    std::deque<QueuedEvent> queued_events;
    std::function<void()> quit_after_wait;
    std::optional<Failure> failure;
    int close_calls = 0;
};

TEST(PollerTest, DelFailureStillRetiresLocalRegistration) {
    auto operations = std::make_shared<FakeEpollOperations>();
    EventLoop loop(operations);
    constexpr int kChannelFd = 55;
    Channel channel(&loop, kChannelFd);
    int read_calls = 0;
    channel.set_read_callback([&]() { ++read_calls; });

    operations->ctl_calls.clear();
    channel.enable_reading();
    ASSERT_EQ(operations->ctl_calls.size(), 1u);
    ASSERT_EQ(operations->ctl_calls.front().operation, EPOLL_CTL_ADD);
    const std::uint64_t stale_token = operations->ctl_calls.front().token;
    ASSERT_NE(stale_token, 0u);

    operations->fail_next(EPOLL_CTL_DEL, EIO);
    channel.disable_all();
    ASSERT_EQ(operations->ctl_calls.back().operation, EPOLL_CTL_DEL);

    // A failed DEL cannot leave a stale pointer in the local map.  A later
    // enable must therefore create a fresh ADD registration, not a MOD.
    operations->ctl_calls.clear();
    channel.enable_reading();
    ASSERT_EQ(operations->ctl_calls.size(), 1u);
    EXPECT_EQ(operations->ctl_calls.front().operation, EPOLL_CTL_ADD);
    EXPECT_NE(operations->ctl_calls.front().token, stale_token);

    operations->queued_events.push_back(
        FakeEpollOperations::QueuedEvent{stale_token, EPOLLIN});
    operations->quit_after_wait = [&loop]() { loop.quit(); };
    loop.loop();
    EXPECT_EQ(read_calls, 0);

    channel.disable_all();
}

TEST(PollerTest, StaleTokenSurvivesFdReuseWithoutDereferencingOldChannel) {
    auto operations = std::make_shared<FakeEpollOperations>();
    EventLoop loop(operations);
    constexpr int kReusedFd = 56;

    operations->ctl_calls.clear();
    int old_calls = 0;
    int new_calls = 0;
    {
        Channel old_channel(&loop, kReusedFd);
        old_channel.set_read_callback([&]() { ++old_calls; });
        old_channel.enable_reading();
        ASSERT_EQ(operations->ctl_calls.size(), 1u);
        const std::uint64_t old_token = operations->ctl_calls.front().token;
        operations->fail_next(EPOLL_CTL_DEL, EBADF);
        old_channel.disable_all();

        Channel new_channel(&loop, kReusedFd);
        new_channel.set_read_callback([&]() { ++new_calls; });
        new_channel.enable_reading();
        ASSERT_EQ(operations->ctl_calls.back().operation, EPOLL_CTL_ADD);
        const std::uint64_t new_token = operations->ctl_calls.back().token;
        EXPECT_NE(old_token, new_token);

        operations->queued_events.push_back(
            FakeEpollOperations::QueuedEvent{old_token, EPOLLIN});
        operations->quit_after_wait = [&loop]() { loop.quit(); };
        loop.loop();
        EXPECT_EQ(old_calls, 0);
        EXPECT_EQ(new_calls, 0);

        operations->queued_events.push_back(
            FakeEpollOperations::QueuedEvent{new_token, EPOLLIN});
        loop.loop();
        EXPECT_EQ(old_calls, 0);
        EXPECT_EQ(new_calls, 1);

        new_channel.disable_all();
    }
}

TEST(PollerTest, ModEnoentReAddUsesFreshTokenAndDiscardsOldEvent) {
    auto operations = std::make_shared<FakeEpollOperations>();
    EventLoop loop(operations);
    constexpr int kChannelFd = 57;
    Channel channel(&loop, kChannelFd);
    int read_calls = 0;
    channel.set_read_callback([&]() { ++read_calls; });

    operations->ctl_calls.clear();
    channel.enable_reading();
    ASSERT_EQ(operations->ctl_calls.size(), 1u);
    const std::uint64_t old_token = operations->ctl_calls.front().token;

    operations->fail_next(EPOLL_CTL_MOD, ENOENT);
    channel.enable_writing();
    ASSERT_EQ(operations->ctl_calls.size(), 3u);
    EXPECT_EQ(operations->ctl_calls[1].operation, EPOLL_CTL_MOD);
    EXPECT_EQ(operations->ctl_calls[2].operation, EPOLL_CTL_ADD);
    const std::uint64_t new_token = operations->ctl_calls[2].token;
    EXPECT_NE(new_token, old_token);

    operations->queued_events.push_back(
        FakeEpollOperations::QueuedEvent{old_token, EPOLLIN});
    operations->quit_after_wait = [&loop]() { loop.quit(); };
    loop.loop();
    EXPECT_EQ(read_calls, 0);

    operations->queued_events.push_back(
        FakeEpollOperations::QueuedEvent{new_token, EPOLLIN});
    loop.loop();
    EXPECT_EQ(read_calls, 1);

    channel.disable_all();
}

TEST(PollerTest, AddFailureDoesNotPublishTokenAndCanBeRetried) {
    auto operations = std::make_shared<FakeEpollOperations>();
    EventLoop loop(operations);
    constexpr int kChannelFd = 58;
    Channel channel(&loop, kChannelFd);
    int read_calls = 0;
    channel.set_read_callback([&]() { ++read_calls; });

    operations->ctl_calls.clear();
    operations->fail_next(EPOLL_CTL_ADD, EACCES);
    channel.enable_reading();
    ASSERT_EQ(operations->ctl_calls.size(), 1u);
    const std::uint64_t failed_token = operations->ctl_calls.front().token;

    operations->queued_events.push_back(
        FakeEpollOperations::QueuedEvent{failed_token, EPOLLIN});
    operations->quit_after_wait = [&loop]() { loop.quit(); };
    loop.loop();
    EXPECT_EQ(read_calls, 0);

    channel.enable_reading();
    ASSERT_EQ(operations->ctl_calls.size(), 2u);
    EXPECT_EQ(operations->ctl_calls.back().operation, EPOLL_CTL_ADD);
    const std::uint64_t successful_token = operations->ctl_calls.back().token;
    EXPECT_NE(successful_token, failed_token);

    operations->queued_events.push_back(
        FakeEpollOperations::QueuedEvent{successful_token, EPOLLIN});
    loop.loop();
    EXPECT_EQ(read_calls, 1);

    channel.disable_all();
}

TEST(PollerTest, BatchSkipsChannelDestroyedByEarlierCallback) {
    auto operations = std::make_shared<FakeEpollOperations>();
    EventLoop loop(operations);
    operations->ctl_calls.clear();

    auto first = std::make_unique<Channel>(&loop, 59);
    auto victim = std::make_unique<Channel>(&loop, 60);
    int first_calls = 0;
    int victim_calls = 0;
    first->set_read_callback([&]() {
        ++first_calls;
        victim.reset();
    });
    victim->set_read_callback([&]() { ++victim_calls; });
    first->enable_reading();
    victim->enable_reading();
    ASSERT_EQ(operations->ctl_calls.size(), 2u);
    const std::uint64_t first_token = operations->ctl_calls[0].token;
    const std::uint64_t victim_token = operations->ctl_calls[1].token;

    operations->queued_events.push_back(
        FakeEpollOperations::QueuedEvent{first_token, EPOLLIN});
    operations->queued_events.push_back(
        FakeEpollOperations::QueuedEvent{victim_token, EPOLLIN});
    operations->quit_after_wait = [&loop]() { loop.quit(); };
    loop.loop();

    EXPECT_EQ(first_calls, 1);
    EXPECT_EQ(victim_calls, 0);
    first->disable_all();
}

TEST(PollerTest, DestructorClosesInjectedEpollDescriptor) {
    auto operations = std::make_shared<FakeEpollOperations>();
    { EventLoop loop(operations); }
    EXPECT_EQ(operations->close_calls, 1);
}

} // namespace
} // namespace minidb
