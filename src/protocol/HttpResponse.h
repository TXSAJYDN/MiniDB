#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace minidb {
class Buffer;

class HttpResponse {
  public:
    enum HttpStatusCode : std::uint16_t {
        kUnknown = 0,
        k200Ok = 200,
        k400BadRequest = 400,
        k404NotFound = 404,
        k409Conflict = 409,
        k405MethodNotAllowed = 405,
        k408RequestTimeout = 408,
        k413RequestEntityTooLarge = 413,
        k414RequestUriTooLong = 414,
        k417ExpectationFailed = 417,
        k431RequestHeaderFieldsTooLarge = 431,
        k500InternalServerError = 500,
        k501NotImplemented = 501,
        k503ServiceUnavailable = 503,
        k505HttpVersionNotSupported = 505,
    };

    explicit HttpResponse(bool close)
        : status_code_(kUnknown), close_connection_(close) {}

    void set_status_code(HttpStatusCode code) {
        status_code_ = code;
    }
    int status_code() const {
        return static_cast<int>(status_code_);
    }
    void set_status_message(const std::string& message) {
        status_message_ = message;
    }
    void set_close_connection(bool on) {
        close_connection_ = on;
    }
    bool close_connection() const {
        return close_connection_;
    }
    void set_content_type(const std::string& content_type) {
        add_header("Content-Type", content_type);
    }
    void set_body(const std::string& body) {
        body_ = body;
    }
    void add_header(const std::string& key, const std::string& value) {
        headers_[key] = value;
    }
    void append_to_buffer(Buffer* output) const;

  private:
    std::map<std::string, std::string> headers_;
    HttpStatusCode status_code_;
    std::string status_message_;
    bool close_connection_;
    std::string body_;
};

} // namespace minidb
