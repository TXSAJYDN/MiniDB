#pragma once

#include "CheckpointManifest.h"
#include "KvStore.h"
#include "RdbManager.h"
#include "WalFileOperations.h"
#include "WalReplay.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace minidb {

std::string checkpoint_manifest_path(const std::string& rdb_path);
std::string wal_directory_path(const std::string& rdb_path);
std::string writer_lock_path(const std::string& rdb_path);

enum class RecoveryStatus : std::uint8_t {
    kOk,
    kCorrupt,
    kIoError,
    kResourceLimit,
    kConflict,
};

struct RecoveryStats {
    std::uint64_t rdb_records = 0;
    std::uint64_t checkpoint_lsn = 0;
    std::uint64_t last_lsn = 0;
    std::uint64_t next_lsn = 1;
    std::uint64_t wal_records_scanned = 0;
    std::uint64_t wal_records_replayed = 0;
    std::uint64_t wal_bytes = 0;
    std::uint64_t truncated_bytes = 0;
    std::size_t wal_segments = 0;
    std::size_t ignored_residue_segments = 0;
    bool legacy_import = false;
    bool needs_bootstrap = false;
    bool manifest_present = false;
};

// Builds a complete recovery image in a temporary KvStore and publishes it only
// after the RDB, manifest and WAL chain all validate.  The caller must already
// own the single-writer lock.
class RecoveryManager {
  public:
    explicit RecoveryManager(
        std::string rdb_path,
        std::shared_ptr<WalFileOperations> wal_operations =
            default_wal_file_operations(),
        std::shared_ptr<RdbFileOperations> rdb_operations = nullptr,
        WalLimits limits = default_wal_limits());
    ~RecoveryManager();

    RecoveryManager(const RecoveryManager&) = delete;
    RecoveryManager& operator=(const RecoveryManager&) = delete;

    RecoveryStatus recover(KvStore& store, std::string* error = nullptr);

    const RecoveryStats& stats() const noexcept {
        return stats_;
    }
    const CheckpointManifest& manifest() const noexcept {
        return manifest_;
    }
    const std::vector<WalSegmentInfo>& live_segments() const noexcept {
        return live_segments_;
    }
    const std::string& rdb_path() const noexcept {
        return rdb_path_;
    }
    const std::string& manifest_path() const noexcept {
        return manifest_path_;
    }
    const std::string& wal_directory() const noexcept {
        return wal_directory_;
    }

  private:
    RecoveryStatus recover_missing_manifest(
        bool rdb_exists, const std::vector<WalSegmentInfo>& on_disk_segments,
        KvStore& target, std::string* error);

    std::string rdb_path_;
    std::string manifest_path_;
    std::string wal_directory_;
    std::shared_ptr<WalFileOperations> wal_operations_;
    WalLimits limits_;
    std::unique_ptr<RdbManager> rdb_manager_;
    CheckpointManifest manifest_;
    std::vector<WalSegmentInfo> live_segments_;
    RecoveryStats stats_;
};

const char* recovery_status_name(RecoveryStatus status);

} // namespace minidb
