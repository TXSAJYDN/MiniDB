#include "WalManager.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
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

std::uint64_t steady_now_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void add_saturated(std::uint64_t* counter, std::uint64_t delta) {
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    *counter = delta > maximum - *counter ? maximum : *counter + delta;
}

void observe_timing(std::uint64_t elapsed_us, std::uint64_t* total_us,
                    std::uint64_t* maximum_us) {
    add_saturated(total_us, elapsed_us);
    *maximum_us = std::max(*maximum_us, elapsed_us);
}

std::size_t data_fsync_bucket(std::uint64_t elapsed_us) {
    for (std::size_t index = 0; index < kWalDataFsyncLatencyBoundsUs.size();
         ++index) {
        if (elapsed_us <= kWalDataFsyncLatencyBoundsUs[index]) {
            return index;
        }
    }
    return kWalDataFsyncLatencyBoundsUs.size();
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
    return append_batch(std::vector<WalRecord>{record}, mode, error);
}

WalResult WalManager::append_batch(const std::vector<WalRecord>& records,
                                   DurabilityMode mode, std::string* error) {
    last_failure_kind_ = WalFailureKind::kNone;
    if (records.empty()) {
        last_failure_kind_ = WalFailureKind::kInvalidArgument;
        set_error(error, "WAL batch must not be empty");
        return WalResult::kInvalidArgument;
    }
    if (!opened_ || !healthy_ || active_fd_ < 0) {
        last_failure_kind_ = WalFailureKind::kState;
        set_error(error, "WAL manager is not writable");
        return WalResult::kIoError;
    }

    // Validate and encode the complete batch before touching the WAL.  This
    // makes malformed input a clean admission failure and gives resource
    // checks an exact byte count.  A record that was already accepted by the
    // persistence layer but cannot be encoded is a caller invariant failure,
    // so the manager still fails closed.
    std::vector<std::vector<std::uint8_t>> encoded;
    encoded.reserve(records.size());
    std::uint64_t expected_lsn = next_lsn_;
    std::uint64_t batch_bytes = 0;
    for (const WalRecord& record : records) {
        if (record.lsn != expected_lsn) {
            last_failure_kind_ = WalFailureKind::kInvalidArgument;
            set_error(error, "WAL batch LSNs are not contiguous");
            fail("non-contiguous LSN supplied to WAL manager");
            return WalResult::kIoError;
        }
        if (record.lsn == std::numeric_limits<std::uint64_t>::max()) {
            last_failure_kind_ = WalFailureKind::kResourceLimit;
            set_error(error, "WAL LSN space is exhausted");
            fail("WAL LSN overflow");
            return WalResult::kResourceLimit;
        }
        std::vector<std::uint8_t> bytes = WalFormat::encode(record, error);
        if (bytes.empty()) {
            last_failure_kind_ = WalFailureKind::kInvalidArgument;
            healthy_ = false;
            return WalResult::kIoError;
        }
        if (bytes.size() > limits_.max_segment_bytes) {
            last_failure_kind_ = WalFailureKind::kResourceLimit;
            set_error(error, "WAL record exceeds segment size limit");
            return WalResult::kResourceLimit;
        }
        if (!checked_add(batch_bytes, bytes.size(), &batch_bytes)) {
            last_failure_kind_ = WalFailureKind::kResourceLimit;
            set_error(error, "WAL batch byte count overflows");
            return WalResult::kResourceLimit;
        }
        encoded.push_back(std::move(bytes));
        ++expected_lsn;
    }

    // Reject impossible resource requests before writing anything.  Rotation
    // can free no space by itself, so total_bytes_ is a lower bound and the
    // current segment layout determines the minimum segment count.
    if (batch_bytes > limits_.max_total_bytes - total_bytes_) {
        last_failure_kind_ = WalFailureKind::kResourceLimit;
        set_error(error, "WAL total byte limit would be exceeded by batch");
        return WalResult::kResourceLimit;
    }
    std::size_t simulated_segments = segments_.size();
    std::uint64_t simulated_active_size =
        segments_.empty() ? 0 : segments_.back().size;
    std::uint64_t simulated_active_start =
        segments_.empty() ? 0 : segments_.back().start_lsn;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const std::size_t bytes = encoded[i].size();
        if (simulated_active_size > 0 &&
            static_cast<std::uint64_t>(bytes) >
                limits_.max_segment_bytes - simulated_active_size) {
            ++simulated_segments;
            simulated_active_size = 0;
            simulated_active_start = records[i].lsn;
            if (simulated_segments > limits_.max_segments) {
                last_failure_kind_ = WalFailureKind::kResourceLimit;
                set_error(error,
                          "WAL batch would exceed the segment count limit");
                return WalResult::kResourceLimit;
            }
        }
        if (simulated_active_size == 0 &&
            simulated_active_start != records[i].lsn) {
            last_failure_kind_ = WalFailureKind::kState;
            set_error(error,
                      "empty WAL segment does not match the first batch LSN");
            fail("WAL segment start LSN mismatch");
            return WalResult::kIoError;
        }
        simulated_active_size += bytes;
    }

    for (std::size_t i = 0; i < encoded.size(); ++i) {
        const WalRecord& record = records[i];
        const std::vector<std::uint8_t>& bytes = encoded[i];

        WalSegmentInfo* active =
            segments_.empty() ? nullptr : &segments_.back();
        if (active == nullptr) {
            last_failure_kind_ = WalFailureKind::kState;
            set_error(error, "WAL manager has no active segment");
            fail("missing active WAL segment");
            return WalResult::kIoError;
        }
        if (active->size == 0 && active->start_lsn != record.lsn) {
            last_failure_kind_ = WalFailureKind::kState;
            set_error(error,
                      "empty WAL segment does not match the first record LSN");
            fail("WAL segment start LSN mismatch");
            return WalResult::kIoError;
        }
        if (active->size > 0 && static_cast<std::uint64_t>(bytes.size()) >
                                    limits_.max_segment_bytes - active->size) {
            if (!rotate(record.lsn, error)) {
                if (last_failure_kind_ == WalFailureKind::kNone) {
                    last_failure_kind_ = healthy_
                                             ? WalFailureKind::kResourceLimit
                                             : WalFailureKind::kRotation;
                }
                return healthy_ ? WalResult::kResourceLimit
                                : WalResult::kIoError;
            }
            active = &segments_.back();
        }

        if (bytes.size() > limits_.max_total_bytes - total_bytes_) {
            last_failure_kind_ = WalFailureKind::kResourceLimit;
            set_error(error, "WAL total byte limit would be exceeded");
            return WalResult::kResourceLimit;
        }

        const std::uint64_t append_started_us = steady_now_us();
        if (!write_all(operations_.get(), active_fd_, bytes.data(),
                       bytes.size(), error)) {
            last_failure_kind_ = WalFailureKind::kWrite;
            fail("WAL append failed");
            return WalResult::kIoError;
        }
        const std::uint64_t append_elapsed_us =
            steady_now_us() - append_started_us;
        active->size += bytes.size();
        total_bytes_ += bytes.size();
        ++stats_.appends;
        add_saturated(&stats_.bytes_written, bytes.size());
        observe_timing(append_elapsed_us, &stats_.append_total_us,
                       &stats_.append_max_us);
        dirty_ = true;
        ++next_lsn_;
    }

    if (mode == DurabilityMode::kSync && !flush(error)) {
        if (last_failure_kind_ == WalFailureKind::kNone) {
            last_failure_kind_ = WalFailureKind::kFsync;
        }
        return WalResult::kIoError;
    }
    return WalResult::kOk;
}

bool WalManager::flush(std::string* error) {
    last_failure_kind_ = WalFailureKind::kNone;
    if (!healthy_ || !opened_ || active_fd_ < 0) {
        last_failure_kind_ = WalFailureKind::kState;
        set_error(error,
                  healthy_ ? "WAL manager is not open" : "WAL manager failed");
        return false;
    }
    if (!dirty_) {
        return true;
    }
    if (!data_sync(active_fd_)) {
        last_failure_kind_ = WalFailureKind::kFsync;
        set_error(error,
                  "WAL fsync failed: " + std::string(std::strerror(errno)));
        fail("WAL fsync failed");
        return false;
    }
    dirty_ = false;
    return true;
}

bool WalManager::rotate(std::uint64_t next_start_lsn, std::string* error) {
    last_failure_kind_ = WalFailureKind::kNone;
    if (!healthy_ || !opened_ || active_fd_ < 0) {
        last_failure_kind_ = WalFailureKind::kState;
        set_error(error, "WAL manager is not writable");
        return false;
    }
    if (next_start_lsn != next_lsn_) {
        last_failure_kind_ = WalFailureKind::kInvalidArgument;
        set_error(error, "WAL rotation LSN is not the next LSN");
        return false;
    }
    if (segments_.size() >= limits_.max_segments) {
        last_failure_kind_ = WalFailureKind::kResourceLimit;
        set_error(error, "WAL segment rotation would exceed the limit");
        return false;
    }
    if (!flush(error)) {
        return false;
    }

    if (operations_->close_fd(active_fd_) < 0) {
        active_fd_ = -1;
        last_failure_kind_ = WalFailureKind::kRotation;
        set_error(error, "failed to close WAL segment during rotation");
        fail("WAL segment close failed");
        return false;
    }
    active_fd_ = -1;
    if (!create_empty_segment(next_start_lsn, error)) {
        last_failure_kind_ = WalFailureKind::kRotation;
        fail("failed to create rotated WAL segment");
        return false;
    }
    if (!open_active_segment(error)) {
        last_failure_kind_ = WalFailureKind::kRotation;
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

bool WalManager::cleanup_before(std::uint64_t first_retained_start_lsn,
                                std::string* error) {
    if (!opened_ || !healthy_ || segments_.empty()) {
        set_error(error, "WAL manager is not open for cleanup");
        return false;
    }
    if (first_retained_start_lsn == 0) {
        set_error(error, "WAL cleanup boundary must be positive");
        return false;
    }
    const auto boundary =
        std::find_if(segments_.begin(), segments_.end(),
                     [first_retained_start_lsn](const WalSegmentInfo& segment) {
                         return segment.start_lsn == first_retained_start_lsn;
                     });
    if (boundary == segments_.end()) {
        set_error(error,
                  "WAL cleanup boundary is not a live segment start LSN");
        return false;
    }

    // Remove prefix residue as well as live prefix segments.  Residue at or
    // after the boundary is intentionally retained: it can be ignored by
    // recovery, and deleting it without a published manifest would weaken the
    // crash-safety argument.
    std::vector<WalSegmentInfo> on_disk;
    if (!list_wal_segments(wal_dir_, &on_disk, error, limits_)) {
        return false;
    }
    for (const WalSegmentInfo& segment : on_disk) {
        if (segment.start_lsn >= first_retained_start_lsn) {
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

    segments_.erase(segments_.begin(), boundary);
    total_bytes_ = 0;
    for (const WalSegmentInfo& segment : segments_) {
        if (segment.size >
            std::numeric_limits<std::uint64_t>::max() - total_bytes_) {
            total_bytes_ = std::numeric_limits<std::uint64_t>::max();
            break;
        }
        total_bytes_ += segment.size;
    }
    return true;
}

bool WalManager::cleanup_except_active(std::string* error) {
    if (segments_.empty()) {
        set_error(error, "WAL manager is not open for cleanup");
        return false;
    }
    return cleanup_before(segments_.back().start_lsn, error);
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
    if (!data_sync(fd)) {
        const int saved_errno = errno;
        (void)operations_->close_fd(fd);
        (void)operations_->unlink_file(path);
        errno = saved_errno;
        set_error(error, "failed to fsync new WAL segment");
        return false;
    }
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

bool WalManager::data_sync(int fd) {
    const std::uint64_t started_us = steady_now_us();
    if (operations_->fsync_fd(fd) < 0) {
        return false;
    }
    const std::uint64_t elapsed_us = steady_now_us() - started_us;
    ++stats_.data_fsyncs;
    observe_timing(elapsed_us, &stats_.data_fsync_total_us,
                   &stats_.data_fsync_max_us);
    add_saturated(
        &stats_.data_fsync_latency_buckets[data_fsync_bucket(elapsed_us)], 1);
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
