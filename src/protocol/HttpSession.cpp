#include "HttpSession.h"

#include "../base/Log.h"
#include "../net/EventLoop.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <string>
#include <utility>

namespace minidb {
namespace {

std::string to_lower_ascii(const std::string& value) {
    std::string result = value;
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string trim_ows(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() &&
           (value[begin] == ' ' || value[begin] == '\t')) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin && (value[end - 1] == ' ' || value[end - 1] == '\t')) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool connection_has_token(const HttpRequest& req, const std::string& token) {
    const std::string value = req.get_header("Connection");
    const std::string wanted = to_lower_ascii(token);
    size_t begin = 0;
    while (begin <= value.size()) {
        const size_t comma = value.find(',', begin);
        const size_t end = comma == std::string::npos ? value.size() : comma;
        if (to_lower_ascii(trim_ows(value.substr(begin, end - begin))) ==
            wanted) {
            return true;
        }
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    return false;
}

const char* reason_phrase(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 408:
        return "Request Timeout";
    case 413:
        return "Request Entity Too Large";
    case 414:
        return "URI Too Long";
    case 417:
        return "Expectation Failed";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 505:
        return "HTTP Version Not Supported";
    default:
        return "Unknown";
    }
}

} // namespace

HttpSession::HttpSession(const TcpConnectionPtr& conn)
    : conn_(conn), started_(false), processing_(false), closing_(false),
      closed_(false), drain_scheduled_(false), awaiting_async_response_(false),
      request_close_(false), request_is_head_(false), requests_handled_(0),
      idle_timer_id_(0) {}

HttpSession::~HttpSession() {
    cancel_idle_timeout();
}

void HttpSession::start() {
    if (started_ || !conn_) {
        return;
    }
    started_ = true;

    std::weak_ptr<HttpSession> weak = weak_from_this();
    conn_->set_write_complete_callback(
        [weak](const TcpConnectionPtr& completed_conn) {
            if (weak.expired() || !completed_conn) {
                return;
            }
            EventLoop* loop = completed_conn->loop();
            // TcpConnection::send() invokes this callback synchronously when
            // the socket accepts the whole response. Always defer session
            // state changes so request handling is never recursive.
            loop->queue_in_loop([weak]() {
                if (auto self = weak.lock()) {
                    self->on_write_complete();
                }
            });
        });

    schedule_idle_timeout();
}

void HttpSession::schedule_drain() {
    if (closed_ || closing_ || processing_ || drain_scheduled_ || !conn_) {
        return;
    }

    drain_scheduled_ = true;
    std::weak_ptr<HttpSession> weak = weak_from_this();
    conn_->loop()->queue_in_loop([weak]() {
        if (auto self = weak.lock()) {
            self->drain();
        }
    });
}

void HttpSession::on_message(Buffer* buf) {
    if (buf == nullptr || buf->readable_bytes() == 0) {
        return;
    }
    if (closed_ || closing_) {
        buf->retrieve_all();
        return;
    }

    const size_t incoming = buf->readable_bytes();
    const size_t pending = pending_buffer_.readable_bytes();
    if (incoming > kMaxPendingBufferSize ||
        pending > kMaxPendingBufferSize - incoming) {
        // Consume the callback's input before entering the error path so
        // TcpConnection observes forward progress.
        buf->retrieve_all();
        pending_buffer_.retrieve_all();
        begin_error(HttpResponse::k413RequestEntityTooLarge,
                    "Pending request data exceeds limit");
        return;
    }

    pending_buffer_.append(buf->peek(), incoming);
    buf->retrieve_all();

    if (!processing_) {
        // Any newly-arrived bytes restart the incomplete-request interval.
        schedule_idle_timeout();
        schedule_drain();
    }
}

void HttpSession::drain() {
    drain_scheduled_ = false;
    if (closed_ || closing_ || processing_ || !conn_ || !conn_->connected()) {
        return;
    }

    if (pending_buffer_.readable_bytes() == 0) {
        schedule_idle_timeout();
        conn_->start_reading();
        return;
    }

    HttpRequest req;
    if (!parser_.parse_request(&pending_buffer_, &req)) {
        const int status = parser_.error_status();
        begin_error(static_cast<HttpResponse::HttpStatusCode>(status),
                    reason_phrase(status));
        return;
    }

    if (!parser_.got_all()) {
        schedule_idle_timeout();
        conn_->start_reading();
        return;
    }

    request_is_head_ = req.method() == HttpRequest::kHead;
    processing_ = true;
    cancel_idle_timeout();
    conn_->stop_reading();
    parser_.reset();

    if (req.body().size() > kMaxBodySize) {
        begin_error(HttpResponse::k413RequestEntityTooLarge,
                    "Request body too large");
        return;
    }

    HttpResponse validation_error(true);
    if (!validate_request(req, &validation_error)) {
        send_final_response(validation_error);
        return;
    }

    ++requests_handled_;
    handle_request(req);
}

bool HttpSession::validate_request(const HttpRequest& req,
                                   HttpResponse* error_resp) {
    if (!req.has_header("host") ||
        !HttpRequest::validate_host(req.get_header("Host"))) {
        error_resp->set_status_code(HttpResponse::k400BadRequest);
        error_resp->set_status_message("Bad Request");
        error_resp->set_body("invalid Host header\n");
        return false;
    }

    if (req.has_header("transfer-encoding")) {
        error_resp->set_status_code(HttpResponse::k501NotImplemented);
        error_resp->set_status_message("Not Implemented");
        error_resp->set_body("Transfer-Encoding not supported\n");
        return false;
    }

    if (req.has_header("expect")) {
        error_resp->set_status_code(HttpResponse::k417ExpectationFailed);
        error_resp->set_status_message("Expectation Failed");
        error_resp->set_body("Expect not supported\n");
        return false;
    }

    if (req.has_header("upgrade")) {
        error_resp->set_status_code(HttpResponse::k400BadRequest);
        error_resp->set_status_message("Bad Request");
        error_resp->set_body("Upgrade not supported\n");
        return false;
    }

    if (req.path().size() + (req.query().empty() ? 0 : req.query().size() + 1) >
        HttpParser::kMaxRequestTargetSize) {
        error_resp->set_status_code(HttpResponse::k414RequestUriTooLong);
        error_resp->set_status_message("URI Too Long");
        error_resp->set_body("Request target too long\n");
        return false;
    }

    return true;
}

bool HttpSession::should_close(const HttpRequest& req) const {
    const bool close_token = connection_has_token(req, "close");
    const bool keep_alive_token = connection_has_token(req, "keep-alive");
    if (close_token) {
        return true;
    }
    if (req.version() == HttpRequest::kHttp10) {
        return !keep_alive_token;
    }
    return false;
}

void HttpSession::handle_request(const HttpRequest& req) {
    const bool close = should_close(req);
    request_close_ = close;
    HttpResponse response(close);
    bool deferred = false;

    try {
        if (async_request_callback_) {
            deferred =
                async_request_callback_(req, &response, make_response_sender());
        } else if (request_callback_) {
            request_callback_(req, &response);
        } else {
            response.set_status_code(HttpResponse::k501NotImplemented);
            response.set_status_message("Not Implemented");
            response.set_body("No handler\n");
        }
    } catch (const std::exception& ex) {
        Log::Error(std::string("HTTP request callback threw: ") + ex.what());
        response.set_status_code(HttpResponse::k500InternalServerError);
        response.set_status_message("Internal Server Error");
        response.set_body("Internal Server Error\n");
        response.set_close_connection(true);
        deferred = false;
    } catch (...) {
        Log::Error("HTTP request callback threw an unknown exception");
        response.set_status_code(HttpResponse::k500InternalServerError);
        response.set_status_message("Internal Server Error");
        response.set_body("Internal Server Error\n");
        response.set_close_connection(true);
        deferred = false;
    }

    if (close) {
        response.set_close_connection(true);
    }

    if (deferred) {
        // The sink is always posted to the owner loop.  Marking the deferral
        // after the callback returns cannot lose a synchronous sink call.
        awaiting_async_response_ = true;
        return;
    }
    finish_response(response);
}

HttpSession::ResponseSender HttpSession::make_response_sender() {
    std::weak_ptr<HttpSession> weak = weak_from_this();
    return [weak](HttpResponse response) {
        if (auto self = weak.lock()) {
            EventLoop* loop = self->conn_ ? self->conn_->loop() : nullptr;
            if (loop == nullptr) {
                return;
            }
            loop->queue_in_loop([weak,
                                 response = std::move(response)]() mutable {
                if (auto session = weak.lock()) {
                    session->complete_deferred_response(std::move(response));
                }
            });
        }
    };
}

void HttpSession::complete_deferred_response(HttpResponse response) {
    if (closed_ || closing_ || !awaiting_async_response_) {
        return;
    }
    awaiting_async_response_ = false;
    finish_response(response);
}

void HttpSession::finish_response(const HttpResponse& response) {
    const bool must_close = request_close_ || response.close_connection();
    if (!send_response(response)) {
        return;
    }

    if (must_close) {
        closing_ = true;
        cancel_idle_timeout();
        if (conn_ && conn_->connected()) {
            conn_->shutdown();
        } else {
            closed_ = true;
        }
    }
}

bool HttpSession::send_response(const HttpResponse& resp) {
    if (!conn_ || closed_ || !conn_->connected()) {
        closed_ = true;
        cancel_idle_timeout();
        return false;
    }

    // HEAD keeps the Content-Length a GET would produce but omits the body
    // bytes.  Copying here covers synchronous, deferred, validation, and
    // error responses without teaching every route about HEAD.
    HttpResponse effective = resp;
    if (request_is_head_) {
        effective.set_suppress_body(true);
    }
    Buffer buf;
    effective.append_to_buffer(&buf);
    if (buf.readable_bytes() == 0) {
        return true;
    }
    if (!conn_->send(buf.peek(), buf.readable_bytes())) {
        closed_ = true;
        cancel_idle_timeout();
        return false;
    }
    return true;
}

bool HttpSession::send_final_response(const HttpResponse& resp) {
    if (closed_ || closing_ || !conn_) {
        return false;
    }

    closing_ = true;
    processing_ = true;
    drain_scheduled_ = false;
    pending_buffer_.retrieve_all();
    cancel_idle_timeout();
    conn_->stop_reading();

    if (!send_response(resp)) {
        closed_ = true;
        return false;
    }

    // shutdown() is write-through: TcpConnection drains every queued byte
    // before retiring the descriptor.
    if (conn_->connected()) {
        conn_->shutdown();
    } else {
        closed_ = true;
    }
    return true;
}

void HttpSession::begin_error(HttpResponse::HttpStatusCode code,
                              const std::string& msg) {
    if (closed_ || closing_) {
        return;
    }

    HttpResponse resp(true);
    resp.set_status_code(code);
    resp.set_status_message(reason_phrase(static_cast<int>(code)));
    resp.set_body(msg.empty() ? std::string("Request failed\n") : msg + "\n");
    send_final_response(resp);
}

void HttpSession::on_write_complete() {
    if (closed_) {
        return;
    }

    // The current request has been fully answered.  A later timeout or
    // protocol error must not inherit HEAD body suppression.
    request_is_head_ = false;

    if (closing_) {
        closed_ = true;
        cancel_idle_timeout();
        if (conn_ && conn_->connected()) {
            conn_->shutdown();
        }
        return;
    }

    if (!processing_) {
        return;
    }

    processing_ = false;
    if (!conn_ || !conn_->connected()) {
        closed_ = true;
        return;
    }

    if (pending_buffer_.readable_bytes() > 0) {
        schedule_drain();
    } else {
        schedule_idle_timeout();
        conn_->start_reading();
    }
}

void HttpSession::schedule_idle_timeout() {
    if (closed_ || closing_ || processing_ || !started_ || !conn_ ||
        idle_timeout_.count() <= 0 || !conn_->loop()->is_in_loop_thread()) {
        return;
    }
    cancel_idle_timeout();
    std::weak_ptr<HttpSession> weak = weak_from_this();
    idle_timer_id_ = conn_->loop()->run_after(idle_timeout_.count(), [weak]() {
        if (auto self = weak.lock()) {
            self->idle_timer_id_ = 0;
            if (self->closed_ || self->closing_ || self->processing_) {
                return;
            }
            self->begin_error(HttpResponse::k408RequestTimeout,
                              "Request timed out");
        }
    });
}

void HttpSession::cancel_idle_timeout() {
    if (idle_timer_id_ == 0 || !conn_) {
        return;
    }
    EventLoop* loop = conn_->loop();
    if (loop->is_in_loop_thread()) {
        loop->cancel_timer(idle_timer_id_);
    }
    idle_timer_id_ = 0;
}

} // namespace minidb
