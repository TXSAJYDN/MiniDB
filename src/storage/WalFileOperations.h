#pragma once

#include "WalFormat.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <sys/types.h>
#include <vector>

namespace minidb {

// Narrow file-system seam for WAL, replay and manifest code.  The production
// implementation uses POSIX calls; tests inject deterministic failures,
// short writes, EINTR and crashes at individual durable-publish steps.
class WalFileOperations {
  public:
    virtual ~WalFileOperations() = default;
    virtual int open_append(const std::string& path) = 0;
    virtual int open_read(const std::string& path) = 0;
    virtual int open_read_write(const std::string& path) = 0;
    virtual int open_exclusive(const std::string& path, mode_t mode) = 0;
    virtual ssize_t read(int fd, void* data, std::size_t len) = 0;
    virtual ssize_t write(int fd, const void* data, std::size_t len) = 0;
    virtual int fsync_fd(int fd) = 0;
    virtual int truncate_fd(int fd, off_t size) = 0;
    virtual int close_fd(int fd) = 0;
    virtual int rename_file(const std::string& from, const std::string& to) = 0;
    virtual int unlink_file(const std::string& path) = 0;
    virtual int open_dir(const std::string& path) = 0;
    virtual int fsync_dir(int fd) = 0;
};

std::shared_ptr<WalFileOperations> default_wal_file_operations();

struct WalSegmentInfo {
    std::uint64_t start_lsn = 0;
    std::string path;
    std::uint64_t size = 0;

    bool operator<(const WalSegmentInfo& other) const {
        return start_lsn < other.start_lsn;
    }
};

std::string wal_segment_name(std::uint64_t start_lsn);

// Returns false for a malformed directory or file-size lookup error.  Unknown
// files are ignored so diagnostic artifacts do not become WAL input.
bool list_wal_segments(const std::string& wal_dir,
                       std::vector<WalSegmentInfo>* segments,
                       std::string* error,
                       const WalLimits& limits = default_wal_limits());

bool ensure_directory_durable(const std::string& path,
                              WalFileOperations* operations,
                              std::string* error);
bool fsync_directory(const std::string& path, WalFileOperations* operations,
                     std::string* error);

bool write_all(WalFileOperations* operations, int fd, const void* data,
               std::size_t len, std::string* error);

} // namespace minidb
