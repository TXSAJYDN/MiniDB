#ifndef MINIDB_BASE_CLOCK_H
#define MINIDB_BASE_CLOCK_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

namespace minidb {

class ClockInterface {
  public:
    virtual ~ClockInterface() = default;

    // Monotonic time used for deadlines and elapsed measurements.
    virtual std::chrono::steady_clock::time_point now() const = 0;

    // Wall-clock Unix epoch milliseconds used only by persistence.
    // Keeping it separate from the monotonic clock avoids RDB TTL extension
    // when a snapshot is restored after a restart.
    virtual int64_t system_now_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
};

class Clock {
  public:
    using time_point = std::chrono::steady_clock::time_point;
    using duration = std::chrono::steady_clock::duration;

    static time_point now();
    static int64_t system_now_ms();

    // All conversions saturate at the representable int64/duration bounds
    // instead of overflowing.  They are total functions used by persistence
    // and timer code, where hostile or extreme inputs must not trigger UB.
    static int64_t to_ms(time_point tp);
    static time_point from_ms(int64_t ms);
    static int64_t duration_ms(duration d);
    static duration ms_to_duration(int64_t ms);

    static void set_clock(std::shared_ptr<ClockInterface> clock);
    static void reset_clock();

  private:
    static std::shared_ptr<ClockInterface> snapshot_clock();
    static std::mutex mutex_;
    static std::shared_ptr<ClockInterface> clock_;
};

} // namespace minidb

#endif // MINIDB_BASE_CLOCK_H