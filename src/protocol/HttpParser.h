#pragma once

#include "HttpRequest.h"

#include <cstddef>
#include <cstdint>

namespace minidb {
class Buffer;

class HttpParser {
  public:
    enum HttpRequestParseState : std::uint8_t {
        kExpectRequestLine,
        kExpectHeaders,
        kExpectBody,
        kGotAll,
        kParseError,
    };

    enum class ParseError : std::uint16_t {
        kNone = 0,
        kBadRequest = 400,
        kPayloadTooLarge = 413,
        kUriTooLong = 414,
        kHeaderTooLarge = 431,
        kNotImplemented = 501,
        kVersionNotSupported = 505,
    };

    HttpParser()
        : state_(kExpectRequestLine), content_length_(0),
          header_bytes_consumed_(0), header_field_count_(0),
          parse_error_(ParseError::kNone) {}

    // Returns false only for a protocol error.  An incomplete request returns
    // true and remains in a non-error state so the caller can read more bytes.
    bool parse_request(Buffer* buf, HttpRequest* req);
    bool got_all() const {
        return state_ == kGotAll;
    }
    bool has_error() const {
        return state_ == kParseError;
    }
    HttpRequestParseState state() const {
        return state_;
    }
    ParseError parse_error() const {
        return parse_error_;
    }
    int error_status() const {
        return static_cast<int>(parse_error_);
    }
    size_t header_bytes_consumed() const {
        return header_bytes_consumed_;
    }
    size_t header_field_count() const {
        return header_field_count_;
    }

    void reset() {
        state_ = kExpectRequestLine;
        HttpRequest dummy;
        request_.swap(dummy);
        content_length_ = 0;
        header_bytes_consumed_ = 0;
        header_field_count_ = 0;
        parse_error_ = ParseError::kNone;
    }

    const HttpRequest& request() const {
        return request_;
    }

    // P0 limits are deliberately small and are part of the wire contract.
    static constexpr size_t kMaxRequestLineSize = size_t{4} * 1024;
    static constexpr size_t kMaxRequestTargetSize = size_t{4} * 1024;
    static constexpr size_t kMaxHeaderSize = size_t{16} * 1024;
    static constexpr size_t kMaxHeaderFieldCount = 64;
    static constexpr size_t kMaxBodySize = size_t{1} * 1024 * 1024;

  private:
    bool parse_request_line(const char* begin, const char* end);
    bool check_incomplete_request_line(const char* begin, const char* end);
    bool finalize_headers();
    bool fail(ParseError error) {
        parse_error_ = error;
        state_ = kParseError;
        return false;
    }

    HttpRequestParseState state_;
    HttpRequest request_;
    size_t content_length_;
    size_t header_bytes_consumed_;
    size_t header_field_count_;
    ParseError parse_error_;
};

} // namespace minidb
