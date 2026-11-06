#include <gtest/gtest.h>

#include "../src/net/EventLoop.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>

namespace minidb {
namespace {

TEST(EventLoopTimerTest, HugeDelayDoesNotFireImmediately) {
    EventLoop loop;
    int huge_calls = 0;
    int near_calls = 0;

    loop.run_after(INT64_MAX, [&]() {
        ++huge_calls;
        loop.quit();
    });
    loop.run_after(40, [&]() {
        ++near_calls;
        loop.quit();
    });
    loop.loop();

    EXPECT_EQ(near_calls, 1);
    EXPECT_EQ(huge_calls, 0);
}

TEST(EventLoopTimerTest, CancelledTimerNeverRuns) {
    EventLoop loop;
    int cancelled_calls = 0;
    bool completed = false;

    const EventLoop::TimerId timer_id =
        loop.run_after(20, [&]() { ++cancelled_calls; });
    ASSERT_NE(timer_id, 0u);
    loop.cancel_timer(timer_id);
    loop.run_after(60, [&]() {
        completed = true;
        loop.quit();
    });
    loop.loop();

    EXPECT_TRUE(completed);
    EXPECT_EQ(cancelled_calls, 0);
}

TEST(EventLoopTimerTest, RepeatingTimerRunsAndCanBeCancelled) {
    EventLoop loop;
    int calls = 0;
    EventLoop::TimerId repeating_id = 0;

    repeating_id = loop.run_every(8, [&]() { ++calls; });
    ASSERT_NE(repeating_id, 0u);
    loop.run_after(70, [&]() {
        loop.cancel_timer(repeating_id);
        loop.quit();
    });
    loop.loop();

    EXPECT_GE(calls, 2);
}

TEST(EventLoopTimerTest, CallbackExceptionDoesNotStopTheLoop) {
    EventLoop loop;
    bool continued = false;

    loop.run_after(1, []() { throw std::runtime_error("timer failure"); });
    loop.run_after(30, [&]() {
        continued = true;
        loop.quit();
    });
    loop.loop();

    EXPECT_TRUE(continued);
}

TEST(EventLoopTimerTest, TimerCallbackMayScheduleAnotherTimer) {
    EventLoop loop;
    bool second_ran = false;

    loop.run_after(1, [&]() {
        loop.run_after(1, [&]() {
            second_ran = true;
            loop.quit();
        });
    });
    loop.run_after(100, [&]() { loop.quit(); });
    loop.loop();

    EXPECT_TRUE(second_ran);
}

TEST(EventLoopTimerTest, ZeroOrNegativeDelayIsClampedToImmediate) {
    EventLoop loop;
    std::atomic<int> calls{0};

    loop.run_after(0, [&]() { ++calls; });
    loop.run_after(-100, [&]() {
        ++calls;
        loop.quit();
    });
    loop.loop();

    EXPECT_EQ(calls.load(), 2);
}

} // namespace
} // namespace minidb
