#pragma once

#include "DurabilityPolicy.h"
#include "WalFileOperations.h"
#include "WalFormat.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace minidb {

enum class WalResult : std::uint8_t {
    kOk,
    kIoError,
    kResourceLimit,
};

// Fixed latency buckets for successful WAL data-fsync observations.  The
// final bucket covers values greater than the last bound.  Keeping these as a
// compile-time array avoids dynamic allocation or high-cardinality histograms
// on the write path.
inline constexpr std::array<std::uint64_t, 11> kWalDataFsyncLatencyBoundsUs = {
    50, 100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000, 100000};
inline constexpr std::size_t kWalDataFsyncLatencyBucketCount =
    kWalDataFsyncLatencyBoundsUs.size() + 1;

struct WalManagerStats {
    std::uint64_t appends = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t data_fsyncs = 0;
    std::uint64_t directory_fsyncs = 0;
    std::uint64_t rotations = 0;
    std::uint64_t segments_removed = 0;

    // Cumulative monotonic-clock timings for successful operations only.
    // Append timing covers write_all(), excluding record encoding, rotation,
    // and any fsync.  Data-fsync timing covers every successful WAL data
    // fsync, including creation of an empty segment.
    std::uint64_t append_total_us = 0;
    std::uint64_t append_max_us = 0;
    std::uint64_t data_fsync_total_us = 0;
    std::uint64_t data_fsync_max_us = 0;
    std::array<std::uint64_t, kWalDataFsyncLatencyBucketCount>
        data_fsync_latency_buckets{};
};

// Appends validated WAL records to one active segment.  The manager never
// assigns an LSN implicitly: the caller supplies the next expected LSN so a
// mutation cannot be applied before its exact record has been written.
class WalManager {
  public:
    explicit WalManager(std::string wal_dir,
                        std::shared_ptr<WalFileOperations> operations =
                            default_wal_file_operations(),
                        WalLimits limits = default_wal_limits());
    ~WalManager();

    WalManager(const WalManager&) = delete;
    WalManager& operator=(const WalManager&) = delete;

    // existing_segments is the ordered set of live segments selected by
    // recovery.  When empty, an empty segment at next_lsn is created.  An
    // empty final segment is reopened at next_lsn; any earlier empty segment
    // is rejected.  The manager never creates a missing, non-empty segment.
    bool open(const std::vector<WalSegmentInfo>& existing_segments,
              std::uint64_t next_lsn, std::string* error = nullptr);

    WalResult append(const WalRecord& record, DurabilityMode mode,
                     std::string* error = nullptr);
    bool flush(std::string* error = nullptr);
    bool rotate(std::uint64_t next_start_lsn, std::string* error = nullptr);

    // Removes an obsolete segment from the live set.  The active segment can
    // never be removed.  A missing file is treated as an already-completed
    // unlink, but the WAL directory is still fsynced.
    bool remove_segment(const WalSegmentInfo& segment,
                        std::string* error = nullptr);

    // Called only after a new manifest has been durably published.  Removes
    // every segment file except the active one, including residue that was
    // not part of the recovered live set, then fsyncs the WAL directory.
    bool cleanup_except_active(std::string* error = nullptr);

    const std::vector<WalSegmentInfo>& segments() const noexcept {
        return segments_;
    }
    const WalSegmentInfo* active_segment() const noexcept;
    std::uint64_t next_lsn() const noexcept {
        return next_lsn_;
    }
    std::uint64_t total_bytes() const noexcept {
        return total_bytes_;
    }
    bool healthy() const noexcept {
        return healthy_;
    }
    WalManagerStats stats() const noexcept {
        return stats_;
    }
    const WalLimits& limits() const noexcept {
        return limits_;
    }

  private:
    bool create_empty_segment(std::uint64_t start_lsn,
                              std::string* error = nullptr);
    bool open_active_segment(std::string* error = nullptr);
    bool data_sync(int fd);
    bool directory_sync(std::string* error = nullptr);
    void fail(const std::string& message);

    std::string wal_dir_;
    std::shared_ptr<WalFileOperations> operations_;
    WalLimits limits_;
    std::vector<WalSegmentInfo> segments_;
    int active_fd_ = -1;
    std::uint64_t next_lsn_ = 1;
    std::uint64_t total_bytes_ = 0;
    bool opened_ = false;
    bool healthy_ = true;
    bool dirty_ = false;
    WalManagerStats stats_;
};

} // namespace minidb
