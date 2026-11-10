#include "SingleWriterLock.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <utility>

namespace minidb {

SingleWriterLock::SingleWriterLock(std::string path) : path_(std::move(path)) {}

SingleWriterLock::~SingleWriterLock() {
    release();
}

SingleWriterLockStatus SingleWriterLock::acquire(std::string* error) {
    if (held()) {
        return SingleWriterLockStatus::kAcquired;
    }
    const int fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        if (error != nullptr) {
            *error = "failed to open writer lock: " +
                     std::string(std::strerror(errno));
        }
        return SingleWriterLockStatus::kError;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) < 0) {
        const int saved_errno = errno;
        ::close(fd);
        if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN) {
            if (error != nullptr) {
                *error = "database is already owned by another process";
            }
            return SingleWriterLockStatus::kBusy;
        }
        if (error != nullptr) {
            *error = "failed to acquire writer lock: " +
                     std::string(std::strerror(saved_errno));
        }
        return SingleWriterLockStatus::kError;
    }
    fd_ = fd;
    return SingleWriterLockStatus::kAcquired;
}

void SingleWriterLock::release() noexcept {
    if (fd_ >= 0) {
        (void)::flock(fd_, LOCK_UN);
        (void)::close(fd_);
        fd_ = -1;
    }
}

} // namespace minidb
