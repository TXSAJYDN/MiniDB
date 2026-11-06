#include "HttpParser.h"

#include "../base/Buffer.h"
#include "ContentLengthParser.h"
#include <algorithm>
#include <cstdint>
#include <string>

namespace minidb {
namespace {

bool is_ctl(unsigned char c) {
    return (c < 0x20 && c != '\t') || c == 0x7f;
}

bool valid_origin_target(const char* begin, const char* end) {
    if (begin == end || *begin != '/') {
        return false;
    }
    for (const char* p = begin; p != end; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (is_ctl(c) || c == ' ' || c == '#') {
            return false;
        }
    }
    return true;
}

bool is_http_version_shape(const char* begin, const char* end) {
    // HTTP-version = HTTP-name / DIGIT . DIGIT
    // Keep the syntactic check strict: malformed versions are 400, while a
    // well-formed version other than HTTP/1.1 is 505.
    static constexpr char kPrefix[] = "HTTP/";
    if (end - begin < 8 || !std::equal(begin, begin + 5, kPrefix) ||
        begin[5] < '0' || begin[5] > '9' || begin[6] != '.' || begin[7] < '0' ||
        begin[7] > '9') {
        return false;
    }
    return std::all_of(begin + 8, end,
                       [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace

bool HttpParser::parse_request(Buffer* buf, HttpRequest* request) {
    if (buf == nullptr || request == nullptr) {
        return fail(ParseError::kBadRequest);
    }
    if (state_ == kParseError) {
        return false;
    }

    for (;;) {
        if (state_ == kExpectRequestLine) {
            const char* begin = buf->peek();
            const char* crlf = buf->find_crlf();
            if (crlf == nullptr) {
                return check_incomplete_request_line(begin, buf->begin_write());
            }
            const size_t line_len = static_cast<size_t>(crlf - begin) + 2;
            if (line_len > kMaxRequestLineSize) {
                return fail(ParseError::kUriTooLong);
            }
            if (!parse_request_line(begin, crlf)) {
                return false;
            }
            buf->retrieve(line_len);
            state_ = kExpectHeaders;
            continue;
        }

        if (state_ == kExpectHeaders) {
            const char* begin = buf->peek();
            const char* crlf = buf->find_crlf();
            if (crlf == nullptr) {
                if (header_bytes_consumed_ + buf->readable_bytes() >
                    kMaxHeaderSize) {
                    return fail(ParseError::kHeaderTooLarge);
                }
                return true;
            }

            const size_t line_len = static_cast<size_t>(crlf - begin) + 2;
            if (header_bytes_consumed_ + line_len > kMaxHeaderSize) {
                return fail(ParseError::kHeaderTooLarge);
            }
            if (crlf == begin) {
                buf->retrieve(line_len);
                header_bytes_consumed_ += line_len;
                if (!finalize_headers()) {
                    return false;
                }
                if (state_ == kGotAll) {
                    *request = request_;
                    return true;
                }
                continue;
            }

            if (*begin == ' ' || *begin == '\t') {
                return fail(ParseError::kBadRequest);
            }
            if (header_field_count_ >= kMaxHeaderFieldCount) {
                return fail(ParseError::kHeaderTooLarge);
            }
            const char* colon = std::find(begin, crlf, ':');
            if (colon == crlf || !request_.add_header(begin, colon, crlf)) {
                return fail(ParseError::kBadRequest);
            }

            buf->retrieve(line_len);
            header_bytes_consumed_ += line_len;
            ++header_field_count_;
            continue;
        }

        if (state_ == kExpectBody) {
            if (buf->readable_bytes() < content_length_) {
                return true;
            }
            request_.set_body(std::string(buf->peek(), content_length_));
            buf->retrieve(content_length_);
            state_ = kGotAll;
            *request = request_;
            return true;
        }

        if (state_ == kGotAll) {
            *request = request_;
            return true;
        }
        return false;
    }
}

bool HttpParser::check_incomplete_request_line(const char* begin,
                                               const char* end) {
    const size_t line_bytes = static_cast<size_t>(end - begin);
    if (line_bytes > kMaxRequestLineSize) {
        return fail(ParseError::kUriTooLong);
    }

    const char* first_space = std::find(begin, end, ' ');
    if (first_space == end) {
        return true;
    }
    if (first_space == begin) {
        return fail(ParseError::kBadRequest);
    }

    const char* target_begin = first_space + 1;
    const char* second_space = std::find(target_begin, end, ' ');
    if (second_space == end) {
        if (static_cast<size_t>(end - target_begin) > kMaxRequestTargetSize) {
            return fail(ParseError::kUriTooLong);
        }
        return true;
    }
    if (static_cast<size_t>(second_space - target_begin) >
        kMaxRequestTargetSize) {
        return fail(ParseError::kUriTooLong);
    }
    if (line_bytes > kMaxRequestLineSize) {
        return fail(ParseError::kUriTooLong);
    }
    return true;
}

bool HttpParser::parse_request_line(const char* begin, const char* end) {
    const char* first_space = std::find(begin, end, ' ');
    if (first_space == end || first_space == begin) {
        return fail(ParseError::kBadRequest);
    }
    if (!request_.set_method(begin, first_space)) {
        return fail(ParseError::kBadRequest);
    }

    const char* target_begin = first_space + 1;
    const char* second_space = std::find(target_begin, end, ' ');
    if (second_space == end || second_space == target_begin ||
        std::find(second_space + 1, end, ' ') != end) {
        return fail(ParseError::kBadRequest);
    }

    const size_t target_len = static_cast<size_t>(second_space - target_begin);
    if (target_len > kMaxRequestTargetSize) {
        return fail(ParseError::kUriTooLong);
    }
    if (!valid_origin_target(target_begin, second_space)) {
        return fail(ParseError::kBadRequest);
    }

    const char* question = std::find(target_begin, second_space, '?');
    if (question == target_begin) {
        return fail(ParseError::kBadRequest);
    }
    request_.set_path(target_begin, question);
    if (question != second_space) {
        request_.set_query(question + 1, second_space);
    }

    const char* version_begin = second_space + 1;
    const size_t version_len = static_cast<size_t>(end - version_begin);
    if (version_len == 8 && std::equal(version_begin, end, "HTTP/1.1")) {
        request_.set_version(HttpRequest::kHttp11);
        return true;
    }
    if (is_http_version_shape(version_begin, end)) {
        return fail(ParseError::kVersionNotSupported);
    }
    return fail(ParseError::kBadRequest);
}

bool HttpParser::finalize_headers() {
    // HTTP/1.1 requires exactly one valid Host field.  Duplicate Host is
    // rejected while adding the field; the map therefore has at most one.
    if (!request_.has_header("Host") ||
        !HttpRequest::validate_host(request_.get_header("Host"))) {
        return fail(ParseError::kBadRequest);
    }

    const bool has_content_length = request_.has_header("Content-Length");
    const bool has_transfer_encoding = request_.has_header("Transfer-Encoding");
    if (has_content_length && has_transfer_encoding) {
        return fail(ParseError::kBadRequest);
    }

    content_length_ = 0;
    if (has_content_length) {
        size_t parsed = 0;
        const ContentLengthParser::Status status =
            ContentLengthParser::parse_checked(
                request_.get_header("Content-Length"), &parsed);
        if (status == ContentLengthParser::Status::kInvalid) {
            return fail(ParseError::kBadRequest);
        }
        if (status == ContentLengthParser::Status::kTooLarge) {
            return fail(ParseError::kPayloadTooLarge);
        }
        content_length_ = parsed;
    }

    if ((request_.method() == HttpRequest::kGet ||
         request_.method() == HttpRequest::kHead) &&
        content_length_ != 0) {
        return fail(ParseError::kBadRequest);
    }

    // Transfer-Encoding, Expect and Upgrade are rejected at the application
    // layer with 501/417/400.  Do not wait for a body that will never be read:
    // doing so makes an error response hostage to attacker-supplied bytes.
    const bool has_expect = request_.has_header("Expect");
    const bool has_upgrade = request_.has_header("Upgrade");
    if (has_transfer_encoding || has_expect || has_upgrade ||
        content_length_ == 0) {
        state_ = kGotAll;
        return true;
    }

    state_ = kExpectBody;
    return true;
}

} // namespace minidb
