#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <signal.h>
#include <thread>
#include <vector>

namespace minidb {

class Channel;
class Poller;
class EpollOperations;

class EventLoop {
  public:
    using TimerId = uint64_t;

    EventLoop();
    explicit EventLoop(std::shared_ptr<EpollOperations> poller_operations);
    ~EventLoop();

    void loop();
    void quit();
    void queue_in_loop(std::function<void()> cb);

    // Timer APIs are owner-loop only.  TimerId 0 is always invalid.
    TimerId run_after(int64_t delay_ms, std::function<void()> cb);
    TimerId run_every(int64_t interval_ms, std::function<void()> cb);
    void cancel_timer(TimerId id);

    // Installs a signalfd-backed watcher.  The calling thread's mask is
    // updated before signalfd is created, avoiding async-signal-unsafe work
    // inside a POSIX signal handler.
    void quit_on_signals(std::initializer_list<int> signals);

    void update_channel(Channel* channel);
    void remove_channel(Channel* channel);

    void assert_in_loop_thread();
    bool is_in_loop_thread() const;
    bool is_looping() const {
        return looping_;
    }

    static EventLoop* get_event_loop_of_current_thread();

  private:
    struct Timer {
        std::chrono::steady_clock::time_point when;
        std::chrono::milliseconds interval{0};
        std::function<void()> callback;
        bool repeat = false;
    };

    void abort_not_in_loop_thread();
    void wakeup();
    void handle_wakeup_read();
    void handle_timer_read();
    void handle_signal_read();
    void do_pending_functors();
    void arm_timer();

    bool looping_;
    std::atomic<bool> quit_;
    std::thread::id thread_id_;
    std::unique_ptr<Poller> poller_;
    std::vector<Channel*> active_channels_;

    int wakeup_fd_;
    // Serializes cross-thread wakeup writes with descriptor teardown.
    mutable std::mutex wakeup_mutex_;
    std::unique_ptr<Channel> wakeup_channel_;
    mutable std::mutex pending_mutex_;
    std::vector<std::function<void()>> pending_functors_;

    int timer_fd_;
    std::unique_ptr<Channel> timer_channel_;
    std::map<TimerId, Timer> timers_;
    TimerId next_timer_id_;
    bool timer_armed_;

    int signal_fd_;
    std::unique_ptr<Channel> signal_channel_;
    sigset_t old_signal_mask_{};
    bool signal_mask_saved_{false};
};

} // namespace minidb
