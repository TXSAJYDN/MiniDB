#ifndef MINIDB_BASE_ERROR_H
#define MINIDB_BASE_ERROR_H

#include <string>

namespace minidb {

enum class ErrorCode {
    Success = 0,
    InvalidArgument,
    IOError,
    NetworkError,
    TimeoutError,
    NotFound,
    AlreadyExists,
    PermissionDenied
};

class Error {
  public:
    Error() : code_(ErrorCode::Success) {}
    explicit Error(ErrorCode code, std::string message = "")
        : code_(code), message_(std::move(message)) {}

    ErrorCode code() const {
        return code_;
    }
    const std::string& message() const {
        return message_;
    }
    bool ok() const {
        return code_ == ErrorCode::Success;
    }

    static Error OK() {
        return Error();
    }
    static Error InvalidArgument(std::string msg) {
        return Error(ErrorCode::InvalidArgument, std::move(msg));
    }
    static Error IOError(std::string msg) {
        return Error(ErrorCode::IOError, std::move(msg));
    }

  private:
    ErrorCode code_;
    std::string message_;
};

} // namespace minidb

#endif // MINIDB_BASE_ERROR_H
