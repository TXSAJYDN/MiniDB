#pragma once

#include "HttpParser.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "../net/TcpConnection.h"
#include "../base/Buffer.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace minidb {

class HttpSession : public std::enable_shared_from_this<HttpSession> {
  public:
    using RequestCallback =
        std::function<void(const HttpRequest&, HttpResponse*)>;
    // Returns true only when the response is deferred and exactly one sink
    // invocation will follow.  A false return means *response is complete.
    using ResponseSender = std::function<void(HttpResponse)>;
    using AsyncRequestCallback =
        std::function<bool(const HttpRequest&, HttpResponse*, ResponseSender)>;

    explicit HttpSession(const TcpConnectionPtr& conn);
    ~HttpSession();

    // Must be called after the shared_ptr has been constructed. It installs
    // weak write callbacks and starts the idle timer without capturing a bare
    // this pointer.
    void start();
    void set_idle_timeout(std::chrono::milliseconds timeout) {
        idle_timeout_ = timeout;
    }
    void set_request_callback(const RequestCallback& cb) {
        request_callback_ = cb;
    }
    void set_async_request_callback(AsyncRequestCallback cb) {
        async_request_callback_ = std::move(cb);
    }
    void on_message(Buffer* buf);

    bool is_processing() const {
        return processing_;
    }
    bool is_closing() const {
        return closing_;
    }

  private:
    void drain();
    void handle_request(const HttpRequest& req);
    bool send_response(const HttpResponse& resp);
    bool send_final_response(const HttpResponse& resp);
    bool validate_request(const HttpRequest& req, HttpResponse* error_resp);
    void begin_error(HttpResponse::HttpStatusCode code, const std::string& msg);
    void finish_response(const HttpResponse& resp);
    void complete_deferred_response(HttpResponse resp);
    ResponseSender make_response_sender();
    void on_write_complete();
    void schedule_idle_timeout();
    void cancel_idle_timeout();
    bool should_close(const HttpRequest& req) const;
    void schedule_drain();

    TcpConnectionPtr conn_;
    HttpParser parser_;
    RequestCallback request_callback_;
    AsyncRequestCallback async_request_callback_;
    bool started_;
    bool processing_;
    bool closing_;
    bool closed_;
    bool drain_scheduled_;
    bool awaiting_async_response_;
    bool request_close_;
    bool request_is_head_;
    size_t requests_handled_;
    Buffer pending_buffer_;
    std::chrono::milliseconds idle_timeout_{30000};
    uint64_t idle_timer_id_;

    static constexpr size_t kMaxHeaderSize = HttpParser::kMaxHeaderSize;
    static constexpr size_t kMaxBodySize = HttpParser::kMaxBodySize;
    // One request body plus its bounded framing and a pipelined tail.
    static constexpr size_t kMaxPendingBufferSize =
        TcpConnection::kMaxInputBufferSize;
};

} // namespace minidb
