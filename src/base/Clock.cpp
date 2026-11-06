#include "base/Clock.h"
#include "base/Int128.h"

#include <limits>

namespace minidb {
namespace {

template <typename ToDuration, typename FromDuration>
ToDuration saturating_duration_cast(FromDuration from) {
    using ToRep = typename ToDuration::rep;
    using ToPeriod = typename ToDuration::period;
    using FromPeriod = typename FromDuration::period;

    const int128_t value = static_cast<int128_t>(from.count());
    const int128_t numerator =
        static_cast<int128_t>(FromPeriod::num) * ToPeriod::den;
    const int128_t denominator =
        static_cast<int128_t>(FromPeriod::den) * ToPeriod::num;
    const int128_t converted = (value * numerator) / denominator;

    if (converted > static_cast<int128_t>(std::numeric_limits<ToRep>::max())) {
        return ToDuration(std::numeric_limits<ToRep>::max());
    }
    if (converted < static_cast<int128_t>(std::numeric_limits<ToRep>::min())) {
        return ToDuration(std::numeric_limits<ToRep>::min());
    }
    return ToDuration(static_cast<ToRep>(converted));
}

} // namespace

std::mutex Clock::mutex_;
std::shared_ptr<ClockInterface> Clock::clock_ = nullptr;

std::shared_ptr<ClockInterface> Clock::snapshot_clock() {
    std::lock_guard<std::mutex> lock(mutex_);
    return clock_;
}

Clock::time_point Clock::now() {
    auto clock = snapshot_clock();
    return clock ? clock->now() : std::chrono::steady_clock::now();
}

int64_t Clock::system_now_ms() {
    auto clock = snapshot_clock();
    if (clock) {
        return clock->system_now_ms();
    }
    return static_cast<int64_t>(
        saturating_duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

int64_t Clock::to_ms(time_point tp) {
    using Milliseconds = std::chrono::milliseconds;
    return static_cast<int64_t>(
        saturating_duration_cast<Milliseconds>(tp.time_since_epoch()).count());
}

Clock::time_point Clock::from_ms(int64_t ms) {
    return time_point(saturating_duration_cast<Clock::duration>(
        std::chrono::milliseconds(ms)));
}

int64_t Clock::duration_ms(duration d) {
    using Milliseconds = std::chrono::milliseconds;
    return static_cast<int64_t>(
        saturating_duration_cast<Milliseconds>(d).count());
}

Clock::duration Clock::ms_to_duration(int64_t ms) {
    return saturating_duration_cast<Clock::duration>(
        std::chrono::milliseconds(ms));
}

void Clock::set_clock(std::shared_ptr<ClockInterface> clock) {
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = std::move(clock);
}

void Clock::reset_clock() {
    std::lock_guard<std::mutex> lock(mutex_);
    clock_ = nullptr;
}

} // namespace minidb