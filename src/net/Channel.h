#pragma once
#include <functional>

namespace minidb {
class EventLoop;
class Channel {
  public:
    using EventCallback = std::function<void()>;
    Channel(EventLoop* loop, int fd);
    ~Channel();
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    void handle_event();
    void set_read_callback(EventCallback cb) {
        read_callback_ = std::move(cb);
    }
    void set_write_callback(EventCallback cb) {
        write_callback_ = std::move(cb);
    }
    void set_close_callback(EventCallback cb) {
        close_callback_ = std::move(cb);
    }
    void set_error_callback(EventCallback cb) {
        error_callback_ = std::move(cb);
    }

    int fd() const {
        return fd_;
    }
    int events() const {
        return events_;
    }
    void set_revents(int revt) {
        revents_ = revt;
    }

    void enable_reading() {
        events_ |= kReadEvent;
        update();
    }
    void disable_reading() {
        events_ &= ~kReadEvent;
        update();
    }
    void enable_writing() {
        events_ |= kWriteEvent;
        update();
    }
    void disable_writing() {
        events_ &= ~kWriteEvent;
        update();
    }
    void disable_all() {
        events_ = kNoneEvent;
        update();
    }

    bool is_none_event() const {
        return events_ == kNoneEvent;
    }
    bool is_writing() const {
        return events_ & kWriteEvent;
    }
    bool is_reading() const {
        return events_ & kReadEvent;
    }

    EventLoop* owner_loop() {
        return loop_;
    }
    void remove();

  private:
    void update();
    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;

    EventLoop* loop_;
    const int fd_;
    int events_;
    int revents_;
    EventCallback read_callback_;
    EventCallback write_callback_;
    EventCallback close_callback_;
    EventCallback error_callback_;
    bool added_to_loop_;
};
} // namespace minidb
