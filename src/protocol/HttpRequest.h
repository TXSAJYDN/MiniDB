#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace minidb {

class HttpRequest {
  public:
    enum Method : std::uint8_t { kInvalid, kGet, kPost, kHead, kPut, kDelete };
    enum Version : std::uint8_t { kUnknown, kHttp10, kHttp11 };

    HttpRequest()
        : method_(kInvalid), version_(kUnknown), method_token_valid_(false),
          header_parse_error_(false) {}

    void set_version(Version v) {
        version_ = v;
    }
    Version version() const {
        return version_;
    }

    // Returns false only when the method is not a valid HTTP token.  A valid
    // but unrecognised extension method is retained in method_raw_ and maps to
    // kInvalid so the application layer can answer 501/405 without losing the
    // original spelling.
    bool set_method(const char* start, const char* end);
    Method method() const {
        return method_;
    }
    bool has_valid_method() const {
        return method_token_valid_;
    }
    const std::string& method_raw() const {
        return method_raw_;
    }
    const char* method_string() const {
        return method_raw_.c_str();
    }

    void set_path(const char* start, const char* end) {
        path_.assign(start, end);
    }
    const std::string& path() const {
        return path_;
    }
    void set_query(const char* start, const char* end) {
        query_.assign(start, end);
    }
    const std::string& query() const {
        return query_;
    }

    // Validates field-name syntax, rejects control characters and obs-fold,
    // and enforces single Host/Transfer-Encoding plus duplicate
    // Content-Length rules.  Duplicate Content-Length values are compared as
    // decimal numbers so "5" and "05" are equivalent, while different values
    // are rejected.
    bool add_header(const char* start, const char* colon, const char* end);
    bool has_header(const std::string& field) const;
    std::string get_header(const std::string& field) const;
    const std::map<std::string, std::string>& headers() const {
        return headers_;
    }
    bool has_header_error() const {
        return header_parse_error_;
    }

    // Shared Host/authority validation used by both the parser and the
    // session.  It accepts host names, IPv4 literals, bracketed IPv6
    // literals, and an optional decimal port in the range 0..65535.
    static bool validate_host(const std::string& value);

    void set_body(const std::string& body) {
        body_ = body;
    }
    const std::string& body() const {
        return body_;
    }

    void swap(HttpRequest& that) noexcept {
        using std::swap;
        swap(method_, that.method_);
        swap(version_, that.version_);
        method_raw_.swap(that.method_raw_);
        path_.swap(that.path_);
        query_.swap(that.query_);
        headers_.swap(that.headers_);
        body_.swap(that.body_);
        swap(method_token_valid_, that.method_token_valid_);
        swap(header_parse_error_, that.header_parse_error_);
    }

  private:
    Method method_;
    Version version_;
    std::string method_raw_;
    std::string path_;
    std::string query_;
    std::map<std::string, std::string> headers_;
    std::string body_;
    bool method_token_valid_;
    bool header_parse_error_;
};

} // namespace minidb
