#pragma once

#include "../src/storage/RdbManager.h"
#include "../src/storage/WalFileOperations.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <utility>

namespace minidb_test {

// A deterministic POSIX-backed fault seam shared by WAL, manifest and RDB
// code.  Tests can fail individual operations, force EINTR/short writes, or
// stop a forked child at an exact durable-publish step and SIGKILL it.
class FaultWalFileOperations : public minidb::WalFileOperations,
                               public minidb::RdbFileOperations {
  public:
    enum class FileKind {
        kUnknown,
        kWalSegment,
        kManifest,
        kRdbSnapshot,
        kDirectory,
    };

    FaultWalFileOperations() = default;

    int open_append(const std::string& path) override {
        if (fail_open_append_) {
            errno = open_append_errno_;
            return -1;
        }
        const int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        remember(fd, FileKind::kWalSegment);
        return fd;
    }

    int open_read(const std::string& path) override {
        if (fail_open_read_) {
            errno = open_read_errno_;
            return -1;
        }
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        remember(fd, FileKind::kWalSegment);
        return fd;
    }

    int open_read_write(const std::string& path) override {
        if (fail_open_read_write_) {
            errno = open_read_write_errno_;
            return -1;
        }
        const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        remember(fd, FileKind::kWalSegment);
        return fd;
    }

    int open_exclusive(const std::string& path, mode_t mode) override {
        if (fail_open_exclusive_) {
            errno = open_exclusive_errno_;
            return -1;
        }
        const int fd =
            ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
        remember(fd, classify_exclusive(path));
        return fd;
    }

    ssize_t read(int fd, void* data, std::size_t len) override {
        if (fail_read_ || eintr_reads_remaining_ > 0) {
            if (eintr_reads_remaining_ > 0) {
                --eintr_reads_remaining_;
                errno = EINTR;
            } else {
                errno = read_errno_;
            }
            return -1;
        }
        const std::size_t bounded = std::min(len, read_chunk_bytes_);
        return ::read(fd, data, bounded);
    }

    ssize_t write(int fd, const void* data, std::size_t len) override {
        ++write_calls_;
        const FileKind kind = kind_for(fd);
        if (block_before_write_kind_ == kind) {
            notify_and_block();
        }
        if (block_partial_write_kind_ == kind) {
            const std::size_t prefix =
                std::min(len, static_cast<std::size_t>(block_write_prefix_));
            ssize_t written = -1;
            do {
                written = ::write(fd, data, prefix);
            } while (written < 0 && errno == EINTR);
            notify_and_block();
        }
        if (eintr_writes_remaining_ > 0) {
            --eintr_writes_remaining_;
            errno = EINTR;
            return -1;
        }
        if (fail_write_after_ >= 0 && write_calls_ > fail_write_after_) {
            errno = write_errno_;
            return -1;
        }
        if (fail_next_write_) {
            fail_next_write_ = false;
            errno = write_errno_;
            return -1;
        }
        if (partial_write_then_fail_ &&
            write_calls_ == partial_write_fail_call_) {
            partial_write_then_fail_ = false;
            fail_next_write_ = true;
            const std::size_t prefix = std::min(
                len, static_cast<std::size_t>(partial_write_prefix_bytes_));
            return ::write(fd, data, prefix);
        }

        const std::size_t bounded =
            short_writes_
                ? std::min(len, static_cast<std::size_t>(short_write_bytes_))
                : len;
        const ssize_t written = ::write(fd, data, bounded);
        if (written == static_cast<ssize_t>(bounded) &&
            block_after_write_kind_ == kind) {
            notify_and_block();
        }
        return written;
    }

    int fsync_fd(int fd) override {
        const FileKind kind = kind_for(fd);
        if (block_before_fsync_kind_ == kind) {
            notify_and_block();
        }
        if (fail_fsync_after_ >= 0 && fsync_calls_ >= fail_fsync_after_) {
            ++fsync_calls_;
            errno = fsync_errno_;
            return -1;
        }
        ++fsync_calls_;
        const int result = ::fsync(fd);
        if (result == 0 && block_after_fsync_kind_ == kind) {
            notify_and_block();
        }
        return result;
    }

    int truncate_fd(int fd, off_t size) override {
        if (fail_truncate_) {
            errno = truncate_errno_;
            return -1;
        }
        return ::ftruncate(fd, size);
    }

    int close_fd(int fd) override {
        forget(fd);
        if (fail_close_) {
            (void)::close(fd);
            errno = close_errno_;
            return -1;
        }
        return ::close(fd);
    }

    int rename_file(const std::string& from, const std::string& to) override {
        if (fail_rename_) {
            errno = rename_errno_;
            return -1;
        }
        const int result = ::rename(from.c_str(), to.c_str());
        if (result == 0 &&
            block_after_rename_kind_ == classify_destination(to)) {
            notify_and_block();
        }
        (void)from;
        return result;
    }

    int unlink_file(const std::string& path) override {
        if (fail_unlink_) {
            errno = unlink_errno_;
            return -1;
        }
        const int result = ::unlink(path.c_str());
        if (result == 0 && block_after_unlink_kind_ == classify_path(path)) {
            notify_and_block();
        }
        return result;
    }

    int open_dir(const std::string& path) override {
        return open_directory(path);
    }

    int open_parent_dir(const std::string& path) override {
        std::filesystem::path parent =
            std::filesystem::path(path).parent_path();
        if (parent.empty()) {
            parent = ".";
        }
        return open_directory(parent.string());
    }

    int fsync_dir(int fd) override {
        if (block_before_dir_fsync_) {
            notify_and_block();
        }
        if (fail_dir_fsync_after_ >= 0 &&
            dir_fsync_calls_ >= fail_dir_fsync_after_) {
            ++dir_fsync_calls_;
            errno = dir_fsync_errno_;
            return -1;
        }
        ++dir_fsync_calls_;
        const int result = ::fsync(fd);
        if (result == 0 && block_after_dir_fsync_) {
            notify_and_block();
        }
        return result;
    }

    void set_notify_fd(int fd) {
        notify_fd_ = fd;
    }
    void set_short_writes(int chunk_bytes) {
        short_write_bytes_ = chunk_bytes;
    }
    void set_eintr_writes(int count) {
        eintr_writes_remaining_ = count;
    }
    void set_eintr_reads(int count) {
        eintr_reads_remaining_ = count;
    }
    void set_read_chunk_bytes(std::size_t bytes) {
        read_chunk_bytes_ = bytes;
    }
    void set_fail_write_after(int successful_writes) {
        fail_write_after_ = successful_writes;
    }
    void set_partial_write_then_fail(int call_number, int prefix_bytes,
                                     int error) {
        partial_write_then_fail_ = true;
        partial_write_fail_call_ = call_number;
        partial_write_prefix_bytes_ = prefix_bytes;
        write_errno_ = error;
    }
    void set_fail_fsync(int error) {
        fail_fsync_after_ = 0;
        fsync_errno_ = error;
    }
    void set_fail_dir_fsync(int error) {
        fail_dir_fsync_after_ = 0;
        dir_fsync_errno_ = error;
    }
    void set_fail_rename(int error) {
        fail_rename_ = true;
        rename_errno_ = error;
    }
    void set_fail_unlink(int error) {
        fail_unlink_ = true;
        unlink_errno_ = error;
    }
    void clear_fail_unlink() {
        fail_unlink_ = false;
    }
    void set_fail_truncate(int error) {
        fail_truncate_ = true;
        truncate_errno_ = error;
    }
    void set_fail_close(int error) {
        fail_close_ = true;
        close_errno_ = error;
    }
    void set_fail_open_exclusive(int error) {
        fail_open_exclusive_ = true;
        open_exclusive_errno_ = error;
    }
    void set_fail_open_append(int error) {
        fail_open_append_ = true;
        open_append_errno_ = error;
    }
    void set_fail_open_read(int error) {
        fail_open_read_ = true;
        open_read_errno_ = error;
    }
    void set_fail_open_read_write(int error) {
        fail_open_read_write_ = true;
        open_read_write_errno_ = error;
    }

    void block_before_write(FileKind kind) {
        block_before_write_kind_ = kind;
    }
    void block_after_partial_write(FileKind kind, int prefix_bytes) {
        block_partial_write_kind_ = kind;
        block_write_prefix_ = prefix_bytes;
    }
    void block_after_write(FileKind kind) {
        block_after_write_kind_ = kind;
    }
    void block_before_fsync(FileKind kind) {
        block_before_fsync_kind_ = kind;
    }
    void block_after_fsync(FileKind kind) {
        block_after_fsync_kind_ = kind;
    }
    void block_after_rename(FileKind kind) {
        block_after_rename_kind_ = kind;
    }
    void block_after_unlink(FileKind kind) {
        block_after_unlink_kind_ = kind;
    }
    void block_before_dir_fsync() {
        block_before_dir_fsync_ = true;
    }
    void block_after_dir_fsync() {
        block_after_dir_fsync_ = true;
    }

    int write_calls() const {
        return write_calls_;
    }
    int fsync_calls() const {
        return fsync_calls_;
    }
    int dir_fsync_calls() const {
        return dir_fsync_calls_;
    }

  private:
    static bool has_suffix(const std::string& value,
                           const std::string& suffix) {
        return value.size() >= suffix.size() &&
               value.compare(value.size() - suffix.size(), suffix.size(),
                             suffix) == 0;
    }

    static FileKind classify_exclusive(const std::string& path) {
        if (path.find(".manifest.tmp.") != std::string::npos) {
            return FileKind::kManifest;
        }
        if (path.find(".tmp.") != std::string::npos) {
            return FileKind::kRdbSnapshot;
        }
        return classify_path(path);
    }

    static FileKind classify_destination(const std::string& path) {
        if (has_suffix(path, ".manifest")) {
            return FileKind::kManifest;
        }
        return FileKind::kRdbSnapshot;
    }

    static FileKind classify_path(const std::string& path) {
        if (path.find("/wal-") != std::string::npos ||
            path.find("wal-") != std::string::npos) {
            return FileKind::kWalSegment;
        }
        if (path.find(".manifest") != std::string::npos) {
            return FileKind::kManifest;
        }
        if (has_suffix(path, ".rdb") ||
            path.find(".tmp.") != std::string::npos) {
            return FileKind::kRdbSnapshot;
        }
        return FileKind::kUnknown;
    }

    int open_directory(const std::string& path) {
        if (fail_open_dir_) {
            errno = open_dir_errno_;
            return -1;
        }
        const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        remember(fd, FileKind::kDirectory);
        return fd;
    }

    void remember(int fd, FileKind kind) {
        if (fd >= 0) {
            open_kinds_[fd] = kind;
        }
    }

    void forget(int fd) {
        open_kinds_.erase(fd);
    }

    FileKind kind_for(int fd) const {
        const auto it = open_kinds_.find(fd);
        return it == open_kinds_.end() ? FileKind::kUnknown : it->second;
    }

    [[noreturn]] void notify_and_block() const {
        if (notify_fd_ >= 0) {
            const char ready = 'R';
            ssize_t sent = -1;
            do {
                sent = ::write(notify_fd_, &ready, 1);
            } while (sent < 0 && errno == EINTR);
            if (sent != 1) {
                ::_exit(92);
            }
        }
        for (;;) {
            ::pause();
        }
    }

    std::unordered_map<int, FileKind> open_kinds_;
    int notify_fd_ = -1;
    bool fail_open_append_ = false;
    bool fail_open_read_ = false;
    bool fail_open_read_write_ = false;
    bool fail_open_exclusive_ = false;
    bool fail_open_dir_ = false;
    int open_append_errno_ = EIO;
    int open_read_errno_ = EIO;
    int open_read_write_errno_ = EIO;
    int open_exclusive_errno_ = EIO;
    int open_dir_errno_ = EIO;
    bool fail_read_ = false;
    int read_errno_ = EIO;
    int eintr_reads_remaining_ = 0;
    std::size_t read_chunk_bytes_ = static_cast<std::size_t>(-1);
    int write_calls_ = 0;
    int eintr_writes_remaining_ = 0;
    int fail_write_after_ = -1;
    int write_errno_ = EIO;
    bool fail_next_write_ = false;
    bool partial_write_then_fail_ = false;
    int partial_write_fail_call_ = 0;
    int partial_write_prefix_bytes_ = 0;
    bool short_writes_ = false;
    int short_write_bytes_ = 1;
    int fsync_calls_ = 0;
    int fail_fsync_after_ = -1;
    int fsync_errno_ = EIO;
    int dir_fsync_calls_ = 0;
    int fail_dir_fsync_after_ = -1;
    int dir_fsync_errno_ = EIO;
    bool fail_truncate_ = false;
    int truncate_errno_ = EIO;
    bool fail_close_ = false;
    int close_errno_ = EIO;
    bool fail_rename_ = false;
    int rename_errno_ = EXDEV;
    bool fail_unlink_ = false;
    int unlink_errno_ = EIO;
    FileKind block_before_write_kind_ = FileKind::kUnknown;
    FileKind block_partial_write_kind_ = FileKind::kUnknown;
    FileKind block_after_write_kind_ = FileKind::kUnknown;
    FileKind block_before_fsync_kind_ = FileKind::kUnknown;
    FileKind block_after_fsync_kind_ = FileKind::kUnknown;
    FileKind block_after_rename_kind_ = FileKind::kUnknown;
    FileKind block_after_unlink_kind_ = FileKind::kUnknown;
    int block_write_prefix_ = 1;
    bool block_before_dir_fsync_ = false;
    bool block_after_dir_fsync_ = false;
};

} // namespace minidb_test
