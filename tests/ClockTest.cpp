#include "base/Clock.h"
#include <gtest/gtest.h>
#include <memory>

namespace minidb {

TEST(ClockTest, Now) {
    auto t = Clock::now();
    EXPECT_GT(Clock::to_ms(t), 0);
}

TEST(ClockTest, ToMs) {
    auto tp = Clock::from_ms(1500);
    EXPECT_EQ(Clock::to_ms(tp), 1500);
}

TEST(ClockTest, FromMs) {
    auto tp = Clock::from_ms(2000);
    EXPECT_EQ(Clock::to_ms(tp), 2000);
}

TEST(ClockTest, DurationMs) {
    auto d = std::chrono::seconds(5);
    EXPECT_EQ(Clock::duration_ms(d), 5000);
}

TEST(ClockTest, Monotonicity) {
    // Use mock clock for deterministic monotonicity test
    class TestClock : public ClockInterface {
      public:
        std::chrono::steady_clock::time_point now() const override {
            return std::chrono::steady_clock::time_point(
                std::chrono::milliseconds(counter_++));
        }

      private:
        mutable int64_t counter_ = 1000;
    };

    auto test_clock = std::make_shared<TestClock>();
    Clock::set_clock(test_clock);

    auto t1 = Clock::now();
    auto t2 = Clock::now();
    auto t3 = Clock::now();

    EXPECT_LT(Clock::to_ms(t1), Clock::to_ms(t2));
    EXPECT_LT(Clock::to_ms(t2), Clock::to_ms(t3));

    Clock::reset_clock();
}

TEST(ClockTest, MockClock) {
    class MockClock : public ClockInterface {
      public:
        std::chrono::steady_clock::time_point now() const override {
            return std::chrono::steady_clock::time_point(
                std::chrono::milliseconds(mock_time_));
        }
        void set_time(int64_t ms) {
            mock_time_ = ms;
        }

      private:
        int64_t mock_time_ = 0;
    };

    auto mock = std::make_shared<MockClock>();
    mock->set_time(1000);
    Clock::set_clock(mock);

    EXPECT_EQ(Clock::to_ms(Clock::now()), 1000);

    mock->set_time(2000);
    EXPECT_EQ(Clock::to_ms(Clock::now()), 2000);

    Clock::reset_clock();
}

TEST(ClockTest, ResetClock) {
    class TestClock : public ClockInterface {
      public:
        std::chrono::steady_clock::time_point now() const override {
            return std::chrono::steady_clock::time_point(
                std::chrono::milliseconds(12345));
        }
    };

    auto test_clock = std::make_shared<TestClock>();
    Clock::set_clock(test_clock);
    EXPECT_EQ(Clock::to_ms(Clock::now()), 12345);

    Clock::reset_clock();
    auto real_time = Clock::now();
    EXPECT_GT(Clock::to_ms(real_time), 0);
}

} // namespace minidb
