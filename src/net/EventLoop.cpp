#include "EventLoop.h"
#include "Channel.h"
#include "Poller.h"
#include "../base/Log.h"
#include "../base/Int128.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <csignal>
#include <pthread.h>

namespace minidb {

namespace {
thread_local EventLoop* t_loop_in_this_thread = nullptr;

int create_eventfd() {
    const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error(std::string("eventfd() failed: ") +
                                 std::strerror(errno));
    }
    return fd;
}

int create_timerfd() {
    const int fd =
        ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0) {
        throw std::runtime_error(std::string("timerfd_create() failed: ") +
                                 std::strerror(errno));
    }
    return fd;
}

std::chrono::steady_clock::time_point
saturating_add_milliseconds(std::chrono::steady_clock::time_point base,
                            int64_t milliseconds) {
    if (milliseconds <= 0) {
        return base;
    }
    using Duration = std::chrono::steady_clock::duration;
    using Rep = Duration::rep;
    using Period = Duration::period;
    const int128_t delta = (static_cast<int128_t>(milliseconds) *
                            static_cast<int128_t>(Period::den)) /
                           (static_cast<int128_t>(Period::num) * 1000);
    const int128_t current =
        static_cast<int128_t>(base.time_since_epoch().count());
    const int128_t maximum =
        static_cast<int128_t>(std::chrono::steady_clock::time_point::max()
                                  .time_since_epoch()
                                  .count());
    const int128_t sum = current + delta;
    if (sum >= maximum) {
        return std::chrono::steady_clock::time_point::max();
    }
    return std::chrono::steady_clock::time_point(
        Duration(static_cast<Rep>(sum)));
}

std::chrono::steady_clock::time_point
advance_repeating_timer(std::chrono::steady_clock::time_point when,
                        std::chrono::steady_clock::time_point now,
                        int64_t interval_ms) {
    using Duration = std::chrono::steady_clock::duration;
    using Period = Duration::period;
    const int128_t interval_ticks =
        (static_cast<int128_t>(interval_ms) * Period::den) /
        (static_cast<int128_t>(Period::num) * 1000);

    const int128_t maximum =
        static_cast<int128_t>(std::chrono::steady_clock::time_point::max()
                                  .time_since_epoch()
                                  .count());
    int128_t when_ticks =
        static_cast<int128_t>(when.time_since_epoch().count());
    const int128_t now_ticks =
        static_cast<int128_t>(now.time_since_epoch().count());

    if (when_ticks >= maximum) {
        return std::chrono::steady_clock::time_point::max();
    }
    if (when_ticks > now_ticks) {
        return when;
    }

    const int128_t interval = interval_ticks > 0 ? interval_ticks : 1;
    const int128_t missed = (now_ticks - when_ticks) / interval + 1;
    when_ticks += missed * interval;
    if (when_ticks >= maximum) {
        return std::chrono::steady_clock::time_point::max();
    }
    return std::chrono::steady_clock::time_point(
        Duration(static_cast<Duration::rep>(when_ticks)));
}
} // namespace

EventLoop::EventLoop() : EventLoop(nullptr) {}

EventLoop::EventLoop(std::shared_ptr<EpollOperations> poller_operations)
    : looping_(false), quit_(false), thread_id_(std::this_thread::get_id()),
      poller_(std::make_unique<Poller>(this, std::move(poller_operations))),
      wakeup_fd_(create_eventfd()),
      wakeup_channel_(std::make_unique<Channel>(this, wakeup_fd_)),
      timer_fd_(create_timerfd()),
      timer_channel_(std::make_unique<Channel>(this, timer_fd_)),
      next_timer_id_(1), timer_armed_(false), signal_fd_(-1) {
    if (t_loop_in_this_thread == nullptr) {
        t_loop_in_this_thread = this;
    }

    wakeup_channel_->set_read_callback([this]() { handle_wakeup_read(); });
    wakeup_channel_->enable_reading();

    timer_channel_->set_read_callback([this]() { handle_timer_read(); });
    timer_channel_->enable_reading();
}

EventLoop::~EventLoop() {
    assert(!looping_);
    if (signal_channel_) {
        signal_channel_->disable_all();
        signal_channel_->remove();
    }
    timer_channel_->disable_all();
    timer_channel_->remove();
    wakeup_channel_->disable_all();
    wakeup_channel_->remove();
    if (signal_fd_ >= 0) {
        ::close(signal_fd_);
    }
    if (signal_mask_saved_) {
        ::pthread_sigmask(SIG_SETMASK, &old_signal_mask_, nullptr);
    }
    ::close(timer_fd_);
    {
        std::lock_guard<std::mutex> lock(wakeup_mutex_);
        ::close(wakeup_fd_);
    }
    if (t_loop_in_this_thread == this) {
        t_loop_in_this_thread = nullptr;
    }
}

void EventLoop::loop() {
    assert(!looping_);
    assert_in_loop_thread();
    looping_ = true;
    quit_.store(false, std::memory_order_release);

    while (!quit_.load(std::memory_order_acquire)) {
        active_channels_.clear();
        poller_->poll(10000, &active_channels_);
        for (const Poller::ActiveChannel& ready : active_channels_) {
            // A callback earlier in this batch may have retired the channel
            // (and even reused its descriptor).  The poller token is unique
            // per registration, so stale entries are skipped without
            // dereferencing a retired Channel.
            if (poller_->is_current(ready.channel, ready.token)) {
                ready.channel->handle_event();
            }
        }
        do_pending_functors();
    }

    looping_ = false;
}

void EventLoop::queue_in_loop(std::function<void()> cb) {
    if (!cb) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_functors_.push_back(std::move(cb));
    }
    wakeup();
}

void EventLoop::quit() {
    quit_.store(true, std::memory_order_release);
    if (!is_in_loop_thread()) {
        wakeup();
    }
}

EventLoop::TimerId EventLoop::run_after(int64_t delay_ms,
                                        std::function<void()> cb) {
    assert_in_loop_thread();
    if (!cb) {
        return 0;
    }
    if (delay_ms < 0) {
        delay_ms = 0;
    }
    const TimerId id = next_timer_id_++;
    Timer timer;
    timer.when =
        saturating_add_milliseconds(std::chrono::steady_clock::now(), delay_ms);
    timer.callback = std::move(cb);
    timers_.emplace(id, std::move(timer));
    arm_timer();
    return id;
}

EventLoop::TimerId EventLoop::run_every(int64_t interval_ms,
                                        std::function<void()> cb) {
    assert_in_loop_thread();
    if (!cb) {
        return 0;
    }
    if (interval_ms <= 0) {
        interval_ms = 1;
    }
    const TimerId id = next_timer_id_++;
    Timer timer;
    timer.when = saturating_add_milliseconds(std::chrono::steady_clock::now(),
                                             interval_ms);
    timer.interval = std::chrono::milliseconds(interval_ms);
    timer.callback = std::move(cb);
    timer.repeat = true;
    timers_.emplace(id, std::move(timer));
    arm_timer();
    return id;
}

void EventLoop::cancel_timer(TimerId id) {
    assert_in_loop_thread();
    if (id == 0) {
        return;
    }
    timers_.erase(id);
    arm_timer();
}

void EventLoop::quit_on_signals(std::initializer_list<int> signals,
                                SignalCallback callback) {
    assert_in_loop_thread();
    if (signal_channel_) {
        return;
    }
    signal_callback_ = std::move(callback);
    sigset_t mask;
    sigemptyset(&mask);
    for (int signal_number : signals) {
        sigaddset(&mask, signal_number);
    }
    if (::pthread_sigmask(SIG_BLOCK, &mask, &old_signal_mask_) != 0) {
        throw std::runtime_error("pthread_sigmask() failed");
    }
    signal_mask_saved_ = true;
    signal_fd_ = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd_ < 0) {
        throw std::runtime_error(std::string("signalfd() failed: ") +
                                 std::strerror(errno));
    }
    signal_channel_ = std::make_unique<Channel>(this, signal_fd_);
    signal_channel_->set_read_callback([this]() { handle_signal_read(); });
    signal_channel_->enable_reading();
}

void EventLoop::wakeup() {
    std::lock_guard<std::mutex> lock(wakeup_mutex_);
    const uint64_t one = 1;
    ssize_t n = 0;
    do {
        n = ::write(wakeup_fd_, &one, sizeof(one));
    } while (n < 0 && errno == EINTR);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        Log::Error(std::string("eventfd write failed: ") +
                   std::strerror(errno));
    }
}

void EventLoop::handle_wakeup_read() {
    uint64_t value = 0;
    ssize_t n = 0;
    do {
        n = ::read(wakeup_fd_, &value, sizeof(value));
    } while (n < 0 && errno == EINTR);
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        Log::Error(std::string("eventfd read failed: ") + std::strerror(errno));
    }
}

void EventLoop::handle_timer_read() {
    uint64_t expirations = 0;
    ssize_t n = 0;
    do {
        n = ::read(timer_fd_, &expirations, sizeof(expirations));
    } while (n < 0 && errno == EINTR);
    if (n != static_cast<ssize_t>(sizeof(expirations))) {
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            Log::Error(std::string("timerfd read failed: ") +
                       std::strerror(errno));
        }
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    std::vector<std::pair<TimerId, std::function<void()>>> due;
    for (auto it = timers_.begin(); it != timers_.end();) {
        if (it->second.when <= now) {
            due.emplace_back(it->first, it->second.callback);
            if (it->second.repeat) {
                // Schedule from the previous deadline to avoid drift.  All
                // arithmetic is done in 128 bits so a maximum int64 interval
                // can never wrap into an immediate timer storm.
                it->second.when = advance_repeating_timer(
                    it->second.when, now, it->second.interval.count());
                ++it;
            } else {
                it = timers_.erase(it);
            }
        } else {
            ++it;
        }
    }
    for (auto& item : due) {
        if (item.second) {
            try {
                item.second();
            } catch (const std::exception& ex) {
                Log::Error(std::string("timer callback threw: ") + ex.what());
            } catch (...) {
                Log::Error("timer callback threw an unknown exception");
            }
        }
    }
    arm_timer();
}

void EventLoop::handle_signal_read() {
    while (true) {
        struct signalfd_siginfo info;
        const ssize_t n = ::read(signal_fd_, &info, sizeof(info));
        if (n == static_cast<ssize_t>(sizeof(info))) {
            if (!signal_callback_) {
                quit();
                continue;
            }
            if (signal_callback_invoked_) {
                continue;
            }
            signal_callback_invoked_ = true;
            try {
                signal_callback_();
            } catch (const std::exception& ex) {
                Log::Error(std::string("signal shutdown callback threw: ") +
                           ex.what());
                quit();
            } catch (...) {
                Log::Error(
                    "signal shutdown callback threw an unknown exception");
                quit();
            }
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            Log::Error(std::string("signalfd read failed: ") +
                       std::strerror(errno));
        }
        break;
    }
}

void EventLoop::arm_timer() {
    if (timers_.empty()) {
        struct itimerspec spec {};
        ::timerfd_settime(timer_fd_, 0, &spec, nullptr);
        timer_armed_ = false;
        return;
    }
    auto earliest = timers_.end();
    for (auto it = timers_.begin(); it != timers_.end(); ++it) {
        if (earliest == timers_.end() ||
            it->second.when < earliest->second.when) {
            earliest = it;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    using Duration = std::chrono::steady_clock::duration;
    using Period = Duration::period;
    const int128_t when_ticks =
        static_cast<int128_t>(earliest->second.when.time_since_epoch().count());
    const int128_t now_ticks =
        static_cast<int128_t>(now.time_since_epoch().count());
    // Convert the signed time-point difference directly in 128 bits.  A
    // duration_cast subtraction can overflow before the saturation check when
    // a timer deadline is near steady_clock::time_point::max().
    const int128_t delta_ticks = when_ticks - now_ticks;
    int128_t delay_ns = 0;
    if (delta_ticks > 0) {
        // delta_ticks is expressed in steady_clock::duration ticks.  Convert
        // it to nanoseconds exactly once; multiplying by Period::den again
        // turns a 40 ms timer into roughly 1.3 million years.
        delay_ns = (delta_ticks * static_cast<int128_t>(1000000000) *
                    static_cast<int128_t>(Period::num)) /
                   static_cast<int128_t>(Period::den);
    }
    const int128_t seconds = delay_ns / 1000000000;
    const int128_t maximum_seconds =
        static_cast<int128_t>(std::numeric_limits<time_t>::max());
    struct itimerspec spec {};
    if (seconds > maximum_seconds) {
        spec.it_value.tv_sec = std::numeric_limits<time_t>::max();
        spec.it_value.tv_nsec = 999999999L;
    } else {
        spec.it_value.tv_sec = static_cast<time_t>(seconds);
        spec.it_value.tv_nsec = static_cast<long>(delay_ns % 1000000000);
    }
    if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) {
        spec.it_value.tv_nsec = 1;
    }
    if (::timerfd_settime(timer_fd_, 0, &spec, nullptr) < 0) {
        Log::Error(std::string("timerfd_settime failed: ") +
                   std::strerror(errno));
        timer_armed_ = false;
        return;
    }
    timer_armed_ = true;
}

void EventLoop::do_pending_functors() {
    std::vector<std::function<void()>> functors;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        functors.swap(pending_functors_);
    }
    for (auto& functor : functors) {
        if (functor) {
            functor();
        }
    }
}

void EventLoop::update_channel(Channel* channel) {
    assert(channel->owner_loop() == this);
    assert_in_loop_thread();
    poller_->update_channel(channel);
}

void EventLoop::remove_channel(Channel* channel) {
    assert(channel->owner_loop() == this);
    assert_in_loop_thread();
    poller_->remove_channel(channel);
}

void EventLoop::assert_in_loop_thread() {
    if (!is_in_loop_thread()) {
        abort_not_in_loop_thread();
    }
}

void EventLoop::abort_not_in_loop_thread() {
    Log::Error("EventLoop API called from a non-owner thread");
    std::abort();
}

bool EventLoop::is_in_loop_thread() const {
    return thread_id_ == std::this_thread::get_id();
}

EventLoop* EventLoop::get_event_loop_of_current_thread() {
    return t_loop_in_this_thread;
}

} // namespace minidb
