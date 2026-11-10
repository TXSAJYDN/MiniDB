#pragma once

#include <cstdint>
#include <string>

namespace minidb {

enum class SingleWriterLockStatus : std::uint8_t {
    kAcquired,
    kBusy,
    kError,
};

// Owns an advisory exclusive lock on "<rdb_path>.lock".  The lock is acquired
// lazily by the persistence manager and held until that manager is destroyed.
class SingleWriterLock {
  public:
    explicit SingleWriterLock(std::string path);
    ~SingleWriterLock();

    SingleWriterLock(const SingleWriterLock&) = delete;
    SingleWriterLock& operator=(const SingleWriterLock&) = delete;

    SingleWriterLockStatus acquire(std::string* error = nullptr);
    void release() noexcept;

    bool held() const noexcept {
        return fd_ >= 0;
    }
    const std::string& path() const noexcept {
        return path_;
    }

  private:
    std::string path_;
    int fd_ = -1;
};

} // namespace minidb
