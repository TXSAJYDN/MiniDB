#include "WalManager.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace minidb {
namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

bool checked_add(std::uint64_t left, std::uint64_t right,
                 std::uint64_t* result) {
    if (result == nullptr ||
        right > std::numeric_limits<std::uint64_t>::max() - left) {
        return false;
    }
    *result = left + right;
    return true;
}

} // namespace

WalManager::WalManager(std::string wal_dir,
                       std::shared_ptr<WalFileOperations> operations,
                       WalLimits limits)
    : wal_dir_(std::move(wal_dir)), operations_(std::move(operations)),
      limits_(limits) {
    std::string limit_error;
    if (!validate_wal_limits(limits_, &limit_error)) {
        throw std::invalid_argument("invalid WAL limits: " + limit_error);
    }
    if (!operations_) {
        operations_ = default_wal_file_operations();
    }
}

WalManager::~WalManager() {
    if (active_fd_ >= 0) {
        (void)operations_->close_fd(active_fd_);
        active_fd_ = -1;
    }
}

bool WalManager::open(const std::vector<WalSegmentInfo>& existing_segments,
                      std::uint64_t next_lsn, std::string* error) {
    if (opened_) {
        set_error(error, "WAL manager is already open");
        return false;
    }
    if (next_lsn == 0) {
        set_error(error, "WAL next LSN must be positive");
        healthy_ = false;
        return false;
    }
    if (existing_segments.size() > limits_.max_segments) {
        set_error(error, "WAL segment count exceeds the configured limit");
        healthy_ = false;
        return false;
    }

    std::uint64_t total = 0;
    std::uint64_t previous_start = 0;
    for (std::size_t i = 0; i < existing_segments.size(); ++i) {
        const WalSegmentInfo& segment = existing_segments[i];
        if (segment.start_lsn == 0 ||
            segment.size > limits_.max_segment_bytes) {
            set_error(error, "WAL segment metadata is out of range");
            healthy_ = false;
            return false;
        }
        if (i != 0 && segment.start_lsn <= previous_start) {
            set_error(error, "WAL segment starts are not strictly increasing");
            healthy_ = false;
            return false;
        }
        if (i + 1 < existing_segments.size() && segment.size == 0) {
            set_error(error, "only the final WAL segment may be empty");
            healthy_ = false;
            return false;
        }
        if (segment.size > limits_.max_total_bytes - total) {
            set_error(error, "WAL total byte limit is exceeded");
            healthy_ = false;
            return false;
        }
        total += segment.size;
        previous_start = segment.start_lsn;
    }

    if (!existing_segments.empty()) {
        const WalSegmentInfo& active = existing_segments.back();
        if (active.size == 0) {
            if (active.start_lsn != next_lsn) {
                set_error(
                    error,
                    "empty active WAL segment does not start at next LSN");
                healthy_ = false;
                return false;
            }
        } else if (active.start_lsn >= next_lsn) {
            set_error(
                error,
                "non-empty active WAL segment has no room before next LSN");
            healthy_ = false;
            return false;
        }
    }

    if (!ensure_directory_durable(wal_dir_, operations_.get(), error)) {
        healthy_ = false;
        return false;
    }

    segments_ = existing_segments;
    next_lsn_ = next_lsn;
    total_bytes_ = total;

    if (segments_.empty()) {
        if (!create_empty_segment(next_lsn_, error)) {
            healthy_ = false;
            return false;
        }
    }
    if (!open_active_segment(error)) {
        healthy_ = false;
        return false;
    }

    opened_ = true;
    healthy_ = true;
    return true;
}

WalResult WalManager::append(const WalRecord& record, DurabilityMode mode,
                             std::string* error) {
    if (!opened_ || !healthy_ || active_fd_ < 0) {
        set_error(error, "WAL manager is not writable");
        return WalResult::kIoError;
    }
    if (record.lsn != next_lsn_) {
        set_error(error, "WAL record LSN is not the next expected LSN");
        fail("non-contiguous LSN supplied to WAL manager");
        return WalResult::kIoError;
    }
    if (record.lsn == std::numeric_limits<std::uint64_t>::max()) {
        set_error(error, "WAL LSN space is exhausted");
        fail("WAL LSN overflow");
        return WalResult::kResourceLimit;
    }

    const std::vector<std::uint8_t> encoded = WalFormat::encode(record, error);
    if (encoded.empty()) {
        healthy_ = false;
        return WalResult::kIoError;
    }
    if (encoded.size() > limits_.max_segment_bytes) {
        set_error(error, "WAL record exceeds segment size limit");
        return WalResult::kResourceLimit;
    }

    WalSegmentInfo* active = segments_.empty() ? nullptr : &segments_.back();
    if (active == nullptr) {
        set_error(error, "WAL manager has no active segment");
        fail("missing active WAL segment");
        return WalResult::kIoError;
    }
    if (active->size == 0 && active->start_lsn != record.lsn) {
        set_error(error,
                  "empty WAL segment does not match the first record LSN");
        fail("WAL segment start LSN mismatch");
        return WalResult::kIoError;
    }
    if (active->size > 0 && static_cast<std::uint64_t>(encoded.size()) >
                                limits_.max_segment_bytes - active->size) {
        if (!rotate(next_lsn_, error)) {
            return healthy_ ? WalResult::kResourceLimit : WalResult::kIoError;
        }
        active = &segments_.back();
    }

    if (segments_.size() > limits_.max_segments) {
        set_error(error, "WAL segment count would exceed the limit");
        return WalResult::kResourceLimit;
    }
    std::uint64_t new_total = 0;
    if (!checked_add(total_bytes_, encoded.size(), &new_total) ||
        new_total > limits_.max_total_bytes) {
        set_error(error, "WAL total byte limit would be exceeded");
        return WalResult::kResourceLimit;
    }

    if (!write_all(operations_.get(), active_fd_, encoded.data(),
                   encoded.size(), error)) {
        fail("WAL append failed");
        return WalResult::kIoError;
    }
    active->size += encoded.size();
    total_bytes_ = new_total;
    stats_.appends++;
    stats_.bytes_written += encoded.size();
    dirty_ = true;
    next_lsn_++;

    if (mode == DurabilityMode::kSync && !flush(error)) {
        return WalResult::kIoError;
    }
    return WalResult::kOk;
}

bool WalManager::flush(std::string* error) {
    if (!healthy_ || !opened_ || active_fd_ < 0) {
        set_error(error,
                  healthy_ ? "WAL manager is not open" : "WAL manager failed");
        return false;
    }
    if (!dirty_) {
        return true;
    }
    if (operations_->fsync_fd(active_fd_) < 0) {
        set_error(error,
                  "WAL fsync failed: " + std::string(std::strerror(errno)));
        fail("WAL fsync failed");
        return false;
    }
    stats_.data_fsyncs++;
    dirty_ = false;
    return true;
}

bool WalManager::rotate(std::uint64_t next_start_lsn, std::string* error) {
    if (!healthy_ || !opened_ || active_fd_ < 0) {
        set_error(error, "WAL manager is not writable");
        return false;
    }
    if (next_start_lsn != next_lsn_) {
        set_error(error, "WAL rotation LSN is not the next LSN");
        return false;
    }
    if (segments_.size() >= limits_.max_segments) {
        set_error(error, "WAL segment rotation would exceed the limit");
        return false;
    }
    if (!flush(error)) {
        return false;
    }

    if (operations_->close_fd(active_fd_) < 0) {
        active_fd_ = -1;
        set_error(error, "failed to close WAL segment during rotation");
        fail("WAL segment close failed");
        return false;
    }
    active_fd_ = -1;
    if (!create_empty_segment(next_start_lsn, error)) {
        fail("failed to create rotated WAL segment");
        return false;
    }
    if (!open_active_segment(error)) {
        fail("rotated WAL segment could not be reopened");
        return false;
    }
    stats_.rotations++;
    return true;
}

bool WalManager::remove_segment(const WalSegmentInfo& segment,
                                std::string* error) {
    if (segments_.empty() || segments_.back().start_lsn == segment.start_lsn) {
        set_error(error, "refusing to remove the active WAL segment");
        return false;
    }
    const auto it =
        std::find_if(segments_.begin(), segments_.end(),
                     [&segment](const WalSegmentInfo& candidate) {
                         return candidate.start_lsn == segment.start_lsn &&
                                candidate.path == segment.path;
                     });
    if (it == segments_.end()) {
        set_error(error, "WAL segment is not part of the live WAL set");
        return false;
    }
    if (operations_->unlink_file(segment.path) < 0 && errno != ENOENT) {
        set_error(error, "failed to remove obsolete WAL segment: " +
                             std::string(std::strerror(errno)));
        return false;
    }
    if (total_bytes_ >= segment.size) {
        total_bytes_ -= segment.size;
    } else {
        total_bytes_ = 0;
    }
    segments_.erase(it);
    stats_.segments_removed++;
    if (!directory_sync(error)) {
        return false;
    }
    return true;
}

bool WalManager::cleanup_except_active(std::string* error) {
    if (!opened_ || !healthy_ || segments_.empty()) {
        set_error(error, "WAL manager is not open for cleanup");
        return false;
    }
    const WalSegmentInfo active = segments_.back();
    std::vector<WalSegmentInfo> on_disk;
    if (!list_wal_segments(wal_dir_, &on_disk, error, limits_)) {
        return false;
    }
    for (const WalSegmentInfo& segment : on_disk) {
        if (segment.start_lsn == active.start_lsn) {
            continue;
        }
        if (operations_->unlink_file(segment.path) < 0 && errno != ENOENT) {
            set_error(error, "failed to remove obsolete WAL residue: " +
                                 std::string(std::strerror(errno)));
            return false;
        }
        stats_.segments_removed++;
    }
    if (!directory_sync(error)) {
        return false;
    }
    segments_.clear();
    segments_.push_back(active);
    total_bytes_ = active.size;
    return true;
}

const WalSegmentInfo* WalManager::active_segment() const noexcept {
    if (segments_.empty()) {
        return nullptr;
    }
    return &segments_.back();
}

bool WalManager::create_empty_segment(std::uint64_t start_lsn,
                                      std::string* error) {
    if (start_lsn == 0) {
        set_error(error, "WAL segment start LSN must be positive");
        return false;
    }
    if (segments_.size() >= limits_.max_segments) {
        set_error(error, "WAL segment count exceeds the limit");
        return false;
    }
    for (const WalSegmentInfo& segment : segments_) {
        if (segment.start_lsn == start_lsn) {
            set_error(error, "WAL segment already exists in the live set");
            return false;
        }
    }

    const std::string name = wal_segment_name(start_lsn);
    const std::string path = (fs::path(wal_dir_) / name).string();
    int fd = operations_->open_exclusive(path, 0600);
    if (fd < 0 && errno == EEXIST) {
        std::error_code ec;
        const std::uintmax_t size = fs::file_size(path, ec);
        if (ec || size != 0) {
            set_error(error, "stale WAL segment is not empty: " + name);
            return false;
        }
        fd = operations_->open_append(path);
    }
    if (fd < 0) {
        set_error(error, "failed to create WAL segment: " + name + ": " +
                             std::string(std::strerror(errno)));
        return false;
    }
    if (operations_->fsync_fd(fd) < 0) {
        const int saved_errno = errno;
        (void)operations_->close_fd(fd);
        (void)operations_->unlink_file(path);
        errno = saved_errno;
        set_error(error, "failed to fsync new WAL segment");
        return false;
    }
    stats_.data_fsyncs++;
    if (operations_->close_fd(fd) < 0) {
        set_error(error, "failed to close new WAL segment");
        return false;
    }
    if (!directory_sync(error)) {
        return false;
    }
    segments_.push_back({start_lsn, path, 0});
    return true;
}

bool WalManager::open_active_segment(std::string* error) {
    if (segments_.empty()) {
        set_error(error, "WAL manager has no active segment to open");
        return false;
    }
    active_fd_ = operations_->open_append(segments_.back().path);
    if (active_fd_ < 0) {
        set_error(error, "failed to reopen active WAL segment: " +
                             std::string(std::strerror(errno)));
        return false;
    }
    return true;
}

bool WalManager::directory_sync(std::string* error) {
    if (!fsync_directory(wal_dir_, operations_.get(), error)) {
        fail("WAL directory fsync failed");
        return false;
    }
    stats_.directory_fsyncs++;
    return true;
}

void WalManager::fail(const std::string& message) {
    healthy_ = false;
    (void)message;
}

} // namespace minidb
