#include "RecoveryManager.h"

#include "WalFormat.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace minidb {
namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

RecoveryStatus status_from_replay(WalReplayStatus status) {
    switch (status) {
    case WalReplayStatus::kOk:
        return RecoveryStatus::kOk;
    case WalReplayStatus::kCorrupt:
        return RecoveryStatus::kCorrupt;
    case WalReplayStatus::kIoError:
        return RecoveryStatus::kIoError;
    case WalReplayStatus::kResourceLimit:
        return RecoveryStatus::kResourceLimit;
    }
    return RecoveryStatus::kCorrupt;
}

} // namespace

std::string checkpoint_manifest_path(const std::string& rdb_path) {
    return rdb_path + ".manifest";
}

std::string wal_directory_path(const std::string& rdb_path) {
    return rdb_path + ".wal";
}

std::string writer_lock_path(const std::string& rdb_path) {
    return rdb_path + ".lock";
}

const char* recovery_status_name(RecoveryStatus status) {
    switch (status) {
    case RecoveryStatus::kOk:
        return "ok";
    case RecoveryStatus::kCorrupt:
        return "corrupt";
    case RecoveryStatus::kIoError:
        return "io-error";
    case RecoveryStatus::kResourceLimit:
        return "resource-limit";
    case RecoveryStatus::kConflict:
        return "conflict";
    }
    return "unknown";
}

RecoveryManager::RecoveryManager(
    std::string rdb_path, std::shared_ptr<WalFileOperations> wal_operations,
    std::shared_ptr<RdbFileOperations> rdb_operations, WalLimits limits)
    : rdb_path_(std::move(rdb_path)),
      manifest_path_(checkpoint_manifest_path(rdb_path_)),
      wal_directory_(wal_directory_path(rdb_path_)),
      wal_operations_(std::move(wal_operations)), limits_(limits),
      rdb_manager_(rdb_operations == nullptr
                       ? std::make_unique<RdbManager>(rdb_path_)
                       : std::make_unique<RdbManager>(
                             rdb_path_, std::move(rdb_operations))) {
    if (!wal_operations_) {
        wal_operations_ = default_wal_file_operations();
    }
}

RecoveryManager::~RecoveryManager() = default;

RecoveryStatus RecoveryManager::recover(KvStore& store, std::string* error) {
    stats_ = RecoveryStats{};
    manifest_ = CheckpointManifest{};
    live_segments_.clear();

    std::error_code ec;
    const fs::file_status rdb_status = fs::status(rdb_path_, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        rdb_status.type() == fs::file_type::not_found) {
        ec.clear();
    } else if (ec) {
        set_error(error, "failed to inspect RDB path: " + ec.message());
        return RecoveryStatus::kIoError;
    } else if (!fs::is_regular_file(rdb_status)) {
        set_error(error, "RDB path is not a regular file");
        return RecoveryStatus::kCorrupt;
    }
    const bool rdb_exists = !ec && fs::is_regular_file(rdb_status);

    std::vector<WalSegmentInfo> on_disk;
    if (!list_wal_segments(wal_directory_, &on_disk, error, limits_)) {
        return RecoveryStatus::kIoError;
    }

    CheckpointManifest loaded_manifest;
    const ManifestLoadStatus manifest_status =
        load_manifest(manifest_path_, &loaded_manifest, error);
    if (manifest_status == ManifestLoadStatus::kMissing) {
        return recover_missing_manifest(rdb_exists, on_disk, store, error);
    }
    if (manifest_status == ManifestLoadStatus::kCorrupt) {
        return RecoveryStatus::kCorrupt;
    }

    stats_.manifest_present = true;
    manifest_ = std::move(loaded_manifest);
    if (!rdb_exists) {
        set_error(error, "manifest exists but its RDB snapshot is missing");
        return RecoveryStatus::kCorrupt;
    }

    KvStore temp_store(store.clock());
    const int loaded_records = rdb_manager_->load(temp_store);
    if (loaded_records < 0) {
        set_error(error, "RDB snapshot is corrupt or unreadable");
        return RecoveryStatus::kCorrupt;
    }
    stats_.rdb_records = static_cast<std::uint64_t>(loaded_records);
    stats_.checkpoint_lsn = manifest_.checkpoint_lsn;

    WalReplayPlan plan;
    WalReplayStats replay_stats;
    const WalReplayStatus replay_status =
        replay_wal(wal_directory_, manifest_, temp_store, wal_operations_.get(),
                   &plan, &replay_stats, error, limits_);
    if (replay_status != WalReplayStatus::kOk) {
        return status_from_replay(replay_status);
    }

    stats_.last_lsn = replay_stats.last_lsn;
    stats_.next_lsn = replay_stats.next_lsn;
    stats_.wal_records_scanned = replay_stats.scanned_records;
    stats_.wal_records_replayed = replay_stats.replayed_records;
    stats_.wal_bytes = replay_stats.recovered_bytes;
    stats_.truncated_bytes = replay_stats.truncated_bytes;
    stats_.wal_segments = replay_stats.segment_count;
    stats_.ignored_residue_segments = replay_stats.ignored_prefix_segments;
    stats_.needs_bootstrap = false;
    stats_.legacy_import = false;
    live_segments_ = std::move(plan.segments);
    store.swap(temp_store);
    return RecoveryStatus::kOk;
}

RecoveryStatus RecoveryManager::recover_missing_manifest(
    bool rdb_exists, const std::vector<WalSegmentInfo>& on_disk_segments,
    KvStore& target, std::string* error) {
    if (!on_disk_segments.empty()) {
        set_error(error,
                  rdb_exists
                      ? "WAL segments exist without a checkpoint manifest; "
                        "automatic migration is refused"
                      : "WAL segments exist without an RDB snapshot or "
                        "checkpoint manifest");
        return RecoveryStatus::kConflict;
    }

    if (!rdb_exists) {
        // Publish an empty image through the same swap path as every other
        // successful recovery. Returning without swapping would leave a
        // caller-provided store untouched, violating the RecoveryManager
        // contract and making a fresh database's visible state depend on how
        // the caller initialized it.
        KvStore empty_store(target.clock());
        target.swap(empty_store);
        stats_.checkpoint_lsn = 0;
        stats_.last_lsn = 0;
        stats_.next_lsn = 1;
        stats_.needs_bootstrap = true;
        stats_.legacy_import = false;
        return RecoveryStatus::kOk;
    }

    KvStore temp_store(target.clock());
    const int loaded_records = rdb_manager_->load(temp_store);
    if (loaded_records < 0) {
        set_error(error, "legacy RDB snapshot is corrupt or unreadable");
        return RecoveryStatus::kCorrupt;
    }
    stats_.rdb_records = static_cast<std::uint64_t>(loaded_records);
    stats_.checkpoint_lsn = 0;
    stats_.last_lsn = 0;
    stats_.next_lsn = 1;
    stats_.needs_bootstrap = true;
    stats_.legacy_import = true;
    target.swap(temp_store);
    return RecoveryStatus::kOk;
}

} // namespace minidb
