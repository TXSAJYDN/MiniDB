#include "HttpRequest.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <limits>
#include <string_view>

namespace minidb {
namespace {

bool is_token_char(unsigned char c) {
    if (std::isalnum(c) != 0) {
        return true;
    }
    switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case 0x60:
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

std::string to_lower_ascii(const std::string& value) {
    std::string result = value;
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

bool is_ows(unsigned char c) {
    return c == ' ' || c == '\t';
}

bool valid_field_value(const std::string& value) {
    for (unsigned char c : value) {
        if (c == '\t') {
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

bool all_decimal_digits(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    return std::all_of(value.begin(), value.end(),
                       [](unsigned char c) { return c >= '0' && c <= '9'; });
}

bool decimal_values_equal(const std::string& lhs, const std::string& rhs) {
    if (!all_decimal_digits(lhs) || !all_decimal_digits(rhs)) {
        return false;
    }
    const std::string_view left(lhs);
    const std::string_view right(rhs);
    const size_t left_begin = left.find_first_not_of('0');
    const size_t right_begin = right.find_first_not_of('0');
    const std::string_view left_normalized =
        left_begin == std::string_view::npos ? std::string_view("0")
                                             : left.substr(left_begin);
    const std::string_view right_normalized =
        right_begin == std::string_view::npos ? std::string_view("0")
                                              : right.substr(right_begin);
    return left_normalized == right_normalized;
}

bool valid_decimal_port(std::string_view port) {
    if (port.empty() || !all_decimal_digits(port)) {
        return false;
    }
    uint32_t value = 0;
    for (unsigned char c : port) {
        const uint32_t digit = static_cast<uint32_t>(c - '0');
        if (value > (65535u - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }
    return value <= 65535u;
}

bool valid_ipv4_literal(const std::string& host) {
    uint8_t bytes[4]{};
    return ::inet_pton(AF_INET, host.c_str(), bytes) == 1;
}

bool valid_reg_name(const std::string& host) {
    if (host.empty() || host.front() == '.' || host.back() == '.') {
        return false;
    }
    size_t label_begin = 0;
    for (size_t i = 0; i <= host.size(); ++i) {
        if (i == host.size() || host[i] == '.') {
            if (i == label_begin) {
                return false;
            }
            if (host[label_begin] == '-' || host[i - 1] == '-') {
                return false;
            }
            label_begin = i + 1;
            continue;
        }
        const unsigned char c = static_cast<unsigned char>(host[i]);
        if (!(std::isalnum(c) != 0 || c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

} // namespace

bool HttpRequest::set_method(const char* start, const char* end) {
    if (start == nullptr || end == nullptr || start == end) {
        method_token_valid_ = false;
        method_ = kInvalid;
        method_raw_.clear();
        return false;
    }
    for (const char* p = start; p != end; ++p) {
        if (!is_token_char(static_cast<unsigned char>(*p))) {
            method_token_valid_ = false;
            method_ = kInvalid;
            method_raw_.clear();
            return false;
        }
    }

    method_raw_.assign(start, end);
    method_token_valid_ = true;
    if (method_raw_ == "GET") {
        method_ = kGet;
    } else if (method_raw_ == "POST") {
        method_ = kPost;
    } else if (method_raw_ == "HEAD") {
        method_ = kHead;
    } else if (method_raw_ == "PUT") {
        method_ = kPut;
    } else if (method_raw_ == "DELETE") {
        method_ = kDelete;
    } else {
        method_ = kInvalid;
    }
    return true;
}

bool HttpRequest::add_header(const char* start, const char* colon,
                             const char* end) {
    if (start == nullptr || colon == nullptr || end == nullptr ||
        start >= colon || colon >= end) {
        header_parse_error_ = true;
        return false;
    }

    for (const char* p = start; p != colon; ++p) {
        if (!is_token_char(static_cast<unsigned char>(*p))) {
            header_parse_error_ = true;
            return false;
        }
    }

    const char* value_begin = colon + 1;
    while (value_begin < end &&
           is_ows(static_cast<unsigned char>(*value_begin))) {
        ++value_begin;
    }
    const char* value_end = end;
    while (value_end > value_begin &&
           is_ows(static_cast<unsigned char>(*(value_end - 1)))) {
        --value_end;
    }

    const std::string field_lower = to_lower_ascii(std::string(start, colon));
    const std::string value(value_begin, value_end);
    if (!valid_field_value(value)) {
        header_parse_error_ = true;
        return false;
    }

    auto existing = headers_.find(field_lower);
    if (existing != headers_.end()) {
        if (field_lower == "content-length") {
            if (!decimal_values_equal(existing->second, value)) {
                header_parse_error_ = true;
                return false;
            }
            return true;
        }
        if (field_lower == "host" || field_lower == "transfer-encoding") {
            header_parse_error_ = true;
            return false;
        }
        if (field_lower == "connection") {
            if (!existing->second.empty() && !value.empty()) {
                existing->second += ", ";
            }
            existing->second += value;
            return true;
        }
    }

    headers_[field_lower] = value;
    return true;
}

bool HttpRequest::has_header(const std::string& field) const {
    return headers_.find(to_lower_ascii(field)) != headers_.end();
}

std::string HttpRequest::get_header(const std::string& field) const {
    const auto it = headers_.find(to_lower_ascii(field));
    return it != headers_.end() ? it->second : std::string();
}

bool HttpRequest::validate_host(const std::string& value) {
    if (value.empty()) {
        return false;
    }

    std::string host;
    std::string_view port;
    if (value.front() == '[') {
        const size_t close = value.find(']');
        if (close == std::string::npos || close == 1) {
            return false;
        }
        host = value.substr(1, close - 1);
        if (close + 1 < value.size()) {
            if (value[close + 1] != ':') {
                return false;
            }
            port = std::string_view(value).substr(close + 2);
            if (!valid_decimal_port(port)) {
                return false;
            }
        }

        in6_addr literal{};
        return ::inet_pton(AF_INET6, host.c_str(), &literal) == 1;
    }

    if (value.find_first_of("[]") != std::string::npos) {
        return false;
    }

    const size_t colon = value.find(':');
    if (colon != std::string::npos) {
        if (value.find(':', colon + 1) != std::string::npos) {
            return false;
        }
        host = value.substr(0, colon);
        port = std::string_view(value).substr(colon + 1);
        if (!valid_decimal_port(port)) {
            return false;
        }
    } else {
        host = value;
    }

    if (host.empty()) {
        return false;
    }

    const bool ipv4_shape =
        all_decimal_digits(host) ||
        std::all_of(host.begin(), host.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || c == '.';
        });
    if (ipv4_shape) {
        return valid_ipv4_literal(host);
    }
    return valid_reg_name(host);
}

} // namespace minidb
