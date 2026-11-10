#include "WalFileOperations.h"

#include "WalFormat.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <limits>
#include <unistd.h>

namespace fs = std::filesystem;

namespace minidb {
namespace {

class PosixWalFileOperations final : public WalFileOperations {
  public:
    int open_append(const std::string& path) override {
        return ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    }
    int open_read(const std::string& path) override {
        return ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    }
    int open_read_write(const std::string& path) override {
        return ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    }
    int open_exclusive(const std::string& path, mode_t mode) override {
        return ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                      mode);
    }
    ssize_t read(int fd, void* data, std::size_t len) override {
        return ::read(fd, data, len);
    }
    ssize_t write(int fd, const void* data, std::size_t len) override {
        return ::write(fd, data, len);
    }
    int fsync_fd(int fd) override {
        return ::fsync(fd);
    }
    int truncate_fd(int fd, off_t size) override {
        return ::ftruncate(fd, size);
    }
    int close_fd(int fd) override {
        return ::close(fd);
    }
    int rename_file(const std::string& from, const std::string& to) override {
        return ::rename(from.c_str(), to.c_str());
    }
    int unlink_file(const std::string& path) override {
        return ::unlink(path.c_str());
    }
    int open_dir(const std::string& path) override {
        return ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }
    int fsync_dir(int fd) override {
        return ::fsync(fd);
    }
};

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

std::shared_ptr<WalFileOperations> default_wal_file_operations() {
    return std::make_shared<PosixWalFileOperations>();
}

std::string wal_segment_name(std::uint64_t start_lsn) {
    std::ostringstream out;
    out << "wal-" << std::setw(20) << std::setfill('0') << start_lsn << ".log";
    return out.str();
}

bool list_wal_segments(const std::string& wal_dir,
                       std::vector<WalSegmentInfo>* segments,
                       std::string* error, const WalLimits& limits) {
    if (!validate_wal_limits(limits, error)) {
        return false;
    }
    if (segments == nullptr) {
        set_error(error, "WAL segment output is null");
        return false;
    }
    segments->clear();
    std::error_code ec;
    const fs::path dir(wal_dir);
    if (!fs::exists(dir, ec)) {
        if (ec == std::errc::no_such_file_or_directory || !ec) {
            return true;
        }
        set_error(error, "failed to inspect WAL directory: " + ec.message());
        return false;
    }
    ec.clear();
    if (!fs::is_directory(dir, ec) || ec) {
        set_error(error, "WAL path is not a directory");
        return false;
    }

    fs::directory_iterator it(dir, ec);
    const fs::directory_iterator end;
    while (it != end) {
        if (ec) {
            set_error(error,
                      "failed to enumerate WAL directory: " + ec.message());
            return false;
        }

        ec.clear();
        const bool regular = it->is_regular_file(ec);
        if (ec) {
            set_error(error, "failed to inspect WAL entry: " + ec.message());
            return false;
        }
        if (regular) {
            const std::string name = it->path().filename().string();
            constexpr const char prefix[] = "wal-";
            constexpr const char suffix[] = ".log";
            constexpr std::size_t digits = 20;
            if (name.size() == 4 + digits + 4 &&
                name.compare(0, 4, prefix) == 0 &&
                name.compare(4 + digits, 4, suffix) == 0) {
                std::uint64_t start_lsn = 0;
                bool valid_digits = true;
                for (std::size_t i = 0; i < digits; ++i) {
                    const char c = name[4 + i];
                    if (c < '0' || c > '9') {
                        valid_digits = false;
                        break;
                    }
                    if (start_lsn > (std::numeric_limits<std::uint64_t>::max() -
                                     static_cast<std::uint64_t>(c - '0')) /
                                        10) {
                        valid_digits = false;
                        break;
                    }
                    start_lsn =
                        start_lsn * 10 + static_cast<std::uint64_t>(c - '0');
                }
                if (!valid_digits || start_lsn == 0) {
                    set_error(error,
                              "WAL segment has an invalid start LSN: " + name);
                    return false;
                }
                ec.clear();
                const std::uintmax_t size_u = it->file_size(ec);
                if (ec || size_u > limits.max_segment_bytes) {
                    set_error(error,
                              "WAL segment exceeds the size limit: " + name);
                    return false;
                }
                segments->push_back({start_lsn, it->path().string(),
                                     static_cast<std::uint64_t>(size_u)});
                if (segments->size() > limits.max_segments) {
                    set_error(error,
                              "WAL segment count exceeds the configured limit");
                    return false;
                }
            }
        }

        it.increment(ec);
    }
    if (ec) {
        set_error(error, "failed to enumerate WAL directory: " + ec.message());
        return false;
    }

    std::sort(segments->begin(), segments->end());
    for (std::size_t i = 1; i < segments->size(); ++i) {
        if ((*segments)[i - 1].start_lsn == (*segments)[i].start_lsn) {
            set_error(error, "duplicate WAL segment start LSN");
            return false;
        }
    }
    return true;
}

bool fsync_directory(const std::string& path, WalFileOperations* operations,
                     std::string* error) {
    if (operations == nullptr) {
        set_error(error, "WAL file operations are null");
        return false;
    }
    const int fd = operations->open_dir(path);
    if (fd < 0) {
        set_error(error, "failed to open directory for fsync");
        return false;
    }
    const int result = operations->fsync_dir(fd);
    const int saved_errno = errno;
    operations->close_fd(fd);
    if (result < 0) {
        errno = saved_errno;
        set_error(error, "directory fsync failed");
        return false;
    }
    return true;
}

bool ensure_directory_durable(const std::string& path,
                              WalFileOperations* operations,
                              std::string* error) {
    std::error_code ec;
    const fs::path dir(path);
    if (!fs::exists(dir, ec)) {
        if (ec) {
            set_error(error,
                      "failed to inspect WAL directory: " + ec.message());
            return false;
        }
        if (!fs::create_directories(dir, ec) || ec) {
            set_error(error, "failed to create WAL directory: " + ec.message());
            return false;
        }
        fs::path parent = dir.parent_path();
        if (parent.empty()) {
            parent = ".";
        }
        if (!fsync_directory(parent.string(), operations, error)) {
            return false;
        }
    }
    ec.clear();
    if (!fs::is_directory(dir, ec) || ec) {
        set_error(error, "WAL directory is not a directory");
        return false;
    }
    return fsync_directory(path, operations, error);
}

bool write_all(WalFileOperations* operations, int fd, const void* data,
               std::size_t len, std::string* error) {
    if (operations == nullptr) {
        set_error(error, "WAL file operations are null");
        return false;
    }
    const auto* ptr = static_cast<const std::uint8_t*>(data);
    std::size_t remaining = len;
    while (remaining > 0) {
        const ssize_t written = operations->write(fd, ptr, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_error(error,
                      std::string("write failed: ") + std::strerror(errno));
            return false;
        }
        if (written == 0) {
            errno = EIO;
            set_error(error, "write returned zero bytes");
            return false;
        }
        ptr += static_cast<std::size_t>(written);
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
}

} // namespace minidb
