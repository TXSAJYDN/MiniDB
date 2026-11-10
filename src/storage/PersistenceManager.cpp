#include "PersistenceManager.h"

#include "CheckpointManifest.h"
#include "WalFormat.h"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace minidb {
namespace {

const char* mutation_io_error = "persistence I/O failure";

std::uint64_t steady_now_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::int64_t add_ttl_saturated(std::int64_t wall_now_ms, std::int64_t ttl_ms) {
    if (ttl_ms <= 0) {
        return 0;
    }
    if (wall_now_ms > std::numeric_limits<std::int64_t>::max() - ttl_ms) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return wall_now_ms + ttl_ms;
}

} // namespace

PersistenceManager::PersistenceManager(
    std::shared_ptr<KvStore> store, std::string rdb_path, DurabilityMode mode,
    std::shared_ptr<WalFileOperations> wal_operations,
    std::shared_ptr<RdbFileOperations> rdb_operations, WalLimits limits)
    : store_(std::move(store)), rdb_path_(std::move(rdb_path)),
      manifest_path_(checkpoint_manifest_path(rdb_path_)),
      wal_dir_(wal_directory_path(rdb_path_)), durability_mode_(mode),
      wal_operations_(std::move(wal_operations)),
      rdb_operations_(std::move(rdb_operations)), limits_(limits),
      rdb_manager_(std::make_unique<RdbManager>(rdb_path_, rdb_operations_)) {
    if (!store_) {
        throw std::invalid_argument("PersistenceManager requires a KvStore");
    }
    if (rdb_path_.empty()) {
        throw std::invalid_argument("PersistenceManager requires an RDB path");
    }
    if (!wal_operations_) {
        wal_operations_ = default_wal_file_operations();
    }
    writer_lock_ =
        std::make_unique<SingleWriterLock>(writer_lock_path(rdb_path_));
}

PersistenceManager::~PersistenceManager() = default;

bool PersistenceManager::open(std::string* error) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (opened_) {
        return true;
    }
    if (failed_) {
        if (error != nullptr) {
            *error = mutation_io_error;
        }
        return false;
    }

    const SingleWriterLockStatus lock_status = writer_lock_->acquire(error);
    if (lock_status != SingleWriterLockStatus::kAcquired) {
        failed_ = true;
        return false;
    }

    RecoveryManager recovery(rdb_path_, wal_operations_, rdb_operations_,
                             limits_);
    std::string recovery_error;
    const RecoveryStatus recovery_status =
        recovery.recover(*store_, &recovery_error);
    recovery_stats_ = recovery.stats();
    if (recovery_status != RecoveryStatus::kOk) {
        if (error != nullptr) {
            *error = "startup recovery failed (" +
                     std::string(recovery_status_name(recovery_status)) +
                     "): " + recovery_error;
        }
        failed_ = true;
        return false;
    }

    last_lsn_ = recovery_stats_.last_lsn;
    if (recovery_stats_.manifest_present) {
        wal_ = std::make_unique<WalManager>(wal_dir_, wal_operations_, limits_);
        std::string wal_error;
        if (!wal_->open(recovery.live_segments(), recovery_stats_.next_lsn,
                        &wal_error)) {
            if (error != nullptr) {
                *error = "failed to open recovery WAL: " + wal_error;
            }
            failed_ = true;
            return false;
        }
    }

    opened_ = true;
    return true;
}

MutationOutcome PersistenceManager::set(const std::string& key,
                                        const std::string& value) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (!opened_ || failed_) {
        return fail(MutationStatus::kNotReady,
                    failed_ ? mutation_io_error : "persistence is not open");
    }
    if (key.empty() || key.size() > RdbFormat::MAX_KEY_SIZE) {
        return fail(MutationStatus::kInvalidArgument, "invalid key length");
    }
    if (value.size() > WalFormat::MAX_VALUE_SIZE) {
        return fail(MutationStatus::kResourceLimit, "value exceeds WAL limit");
    }
    return set_impl(key, value);
}

MutationOutcome PersistenceManager::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (!opened_ || failed_) {
        return fail(MutationStatus::kNotReady,
                    failed_ ? mutation_io_error : "persistence is not open");
    }
    if (key.empty() || key.size() > RdbFormat::MAX_KEY_SIZE) {
        return fail(MutationStatus::kInvalidArgument, "invalid key length");
    }
    return del_impl(key);
}

MutationOutcome PersistenceManager::expire(const std::string& key,
                                           std::int64_t ttl_ms) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (!opened_ || failed_) {
        return fail(MutationStatus::kNotReady,
                    failed_ ? mutation_io_error : "persistence is not open");
    }
    if (key.empty() || key.size() > RdbFormat::MAX_KEY_SIZE) {
        return fail(MutationStatus::kInvalidArgument, "invalid key length");
    }
    return expire_impl(key, ttl_ms);
}

MutationOutcome PersistenceManager::set_impl(const std::string& key,
                                             const std::string& value) {
    std::string bootstrap_error;
    if (!ensure_bootstrapped_locked(&bootstrap_error)) {
        return fail(MutationStatus::kIoError, bootstrap_error);
    }
    WalRecord record;
    record.lsn = wal_->next_lsn();
    record.op = WalOp::kSet;
    record.key = key;
    record.value = value;
    return append_and_apply(record, true, value, false, 0);
}

MutationOutcome PersistenceManager::del_impl(const std::string& key) {
    std::string bootstrap_error;
    if (!ensure_bootstrapped_locked(&bootstrap_error)) {
        return fail(MutationStatus::kIoError, bootstrap_error);
    }
    WalRecord record;
    record.lsn = wal_->next_lsn();
    record.op = WalOp::kDel;
    record.key = key;
    return append_and_apply(record, false, std::string(), true, 0);
}

MutationOutcome PersistenceManager::expire_impl(const std::string& key,
                                                std::int64_t ttl_ms) {
    if (ttl_ms <= 0) {
        return del_impl(key);
    }
    std::string bootstrap_error;
    if (!ensure_bootstrapped_locked(&bootstrap_error)) {
        return fail(MutationStatus::kIoError, bootstrap_error);
    }
    const std::int64_t deadline =
        add_ttl_saturated(store_->wall_clock_now_ms(), ttl_ms);
    WalRecord record;
    record.lsn = wal_->next_lsn();
    record.op = WalOp::kExpire;
    record.key = key;
    record.expiry_ms = deadline;
    return append_and_apply(record, false, std::string(), false, deadline);
}

MutationOutcome PersistenceManager::append_and_apply(
    const WalRecord& record, bool apply_set, const std::string& apply_value,
    bool apply_del, std::int64_t apply_expiry_ms) {
    std::string bootstrap_error;
    if (!ensure_bootstrapped_locked(&bootstrap_error)) {
        return fail(MutationStatus::kIoError, bootstrap_error);
    }

    std::string append_error;
    const WalResult append_result =
        append_with_checkpoint_retry(record, &append_error);
    if (append_result == WalResult::kResourceLimit) {
        return fail(MutationStatus::kResourceLimit, append_error);
    }
    if (append_result != WalResult::kOk) {
        mark_failed();
        return fail(MutationStatus::kIoError, append_error);
    }

    last_lsn_ = record.lsn;
    bool changed = true;
    if (apply_set) {
        store_->set(record.key, apply_value);
    } else if (apply_del) {
        changed = store_->del(record.key);
    } else if (apply_expiry_ms > 0) {
        changed = store_->expire_absolute(record.key, apply_expiry_ms);
    }

    if (after_apply_callback_) {
        after_apply_callback_(record);
    }

    ++stats_.mutations;
    if (durability_mode_ == DurabilityMode::kSync) {
        ++stats_.sync_mutations;
    } else {
        ++stats_.interval_mutations;
    }

    MutationOutcome outcome;
    outcome.changed = changed;
    return outcome;
}

WalResult
PersistenceManager::append_with_checkpoint_retry(const WalRecord& record,
                                                 std::string* error) {
    if (!wal_) {
        if (error != nullptr) {
            *error = "WAL is not initialized";
        }
        return WalResult::kIoError;
    }

    // Keep one segment free so a later checkpoint can always rotate the
    // active segment before publishing a manifest.
    if (wal_->segments().size() >= limits_.max_segments - 1) {
        std::string checkpoint_error;
        const RdbSaveResult checkpoint_result =
            checkpoint_locked(&checkpoint_error);
        if (checkpoint_result != RdbSaveResult::Ok) {
            if (error != nullptr) {
                *error = "WAL segment limit requires checkpoint: " +
                         checkpoint_error;
            }
            return checkpoint_result == RdbSaveResult::Failed
                       ? WalResult::kIoError
                       : WalResult::kResourceLimit;
        }
    }

    WalResult result = wal_->append(record, durability_mode_, error);
    if (result != WalResult::kResourceLimit) {
        return result;
    }

    std::string checkpoint_error;
    const RdbSaveResult checkpoint_result =
        checkpoint_locked(&checkpoint_error);
    if (checkpoint_result != RdbSaveResult::Ok) {
        if (error != nullptr) {
            *error =
                "WAL resource limit and checkpoint failed: " + checkpoint_error;
        }
        return checkpoint_result == RdbSaveResult::Failed
                   ? WalResult::kIoError
                   : WalResult::kResourceLimit;
    }

    // The checkpoint advanced the active segment without changing the logical
    // LSN stream. Retrying the exact same record once keeps the operation
    // deterministic.
    return wal_->append(record, durability_mode_, error);
}

bool PersistenceManager::ensure_bootstrapped_locked(std::string* error) {
    if (wal_) {
        return true;
    }

    KvStore::ScannerPause scanner_pause = store_->pause_scanner();
    const RdbSaveResult save_result =
        rdb_manager_->save_checked(*store_, scanner_pause);
    if (save_result != RdbSaveResult::Ok) {
        mark_failed();
        if (error != nullptr) {
            *error = save_result == RdbSaveResult::DurabilityUnconfirmed
                         ? "initial snapshot durability is unconfirmed"
                         : "failed to publish initial snapshot";
        }
        return false;
    }

    CheckpointManifest manifest;
    manifest.checkpoint_lsn = 0;
    const ManifestWriteResult manifest_result = write_manifest_atomic(
        manifest_path_, manifest, wal_operations_.get(), error);
    if (manifest_result != ManifestWriteResult::kOk) {
        mark_failed();
        return false;
    }

    auto wal = std::make_unique<WalManager>(wal_dir_, wal_operations_, limits_);
    std::string wal_error;
    if (!wal->open({}, 1, &wal_error)) {
        if (error != nullptr) {
            *error = "failed to create initial WAL: " + wal_error;
        }
        mark_failed();
        return false;
    }
    wal_ = std::move(wal);
    recovery_stats_.manifest_present = true;
    recovery_stats_.checkpoint_lsn = 0;
    recovery_stats_.last_lsn = 0;
    recovery_stats_.next_lsn = 1;
    recovery_stats_.needs_bootstrap = false;
    last_lsn_ = 0;
    return true;
}

bool PersistenceManager::flush_interval(std::string* error) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (!opened_ || failed_) {
        if (error != nullptr) {
            *error = failed_ ? mutation_io_error : "persistence is not open";
        }
        return false;
    }
    if (durability_mode_ != DurabilityMode::kInterval || !wal_) {
        return true;
    }
    if (!wal_->flush(error)) {
        mark_failed();
        return false;
    }
    ++stats_.interval_flushes;
    return true;
}

RdbSaveResult PersistenceManager::checkpoint(std::string* error) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (!opened_ || failed_) {
        if (error != nullptr) {
            *error = failed_ ? mutation_io_error : "persistence is not open";
        }
        return RdbSaveResult::Failed;
    }
    return checkpoint_locked(error);
}

RdbSaveResult PersistenceManager::checkpoint_locked(std::string* error) {
    std::string bootstrap_error;
    if (!ensure_bootstrapped_locked(&bootstrap_error)) {
        if (error != nullptr) {
            *error = bootstrap_error;
        }
        return RdbSaveResult::Failed;
    }

    const std::uint64_t pause_start_us = steady_now_us();
    KvStore::ScannerPause scanner_pause = store_->pause_scanner();

    std::string wal_error;
    if (!wal_->flush(&wal_error)) {
        mark_failed();
        if (error != nullptr) {
            *error = "checkpoint could not flush WAL: " + wal_error;
        }
        return RdbSaveResult::Failed;
    }

    const WalSegmentInfo* active = wal_->active_segment();
    if (active == nullptr) {
        mark_failed();
        if (error != nullptr) {
            *error = "checkpoint has no active WAL segment";
        }
        return RdbSaveResult::Failed;
    }
    if (active->size != 0 && !wal_->rotate(wal_->next_lsn(), &wal_error)) {
        mark_failed();
        if (error != nullptr) {
            *error = "checkpoint could not rotate WAL: " + wal_error;
        }
        return RdbSaveResult::Failed;
    }

    const std::uint64_t checkpoint_lsn = wal_->next_lsn() - 1;
    if (checkpoint_lsn != last_lsn_) {
        mark_failed();
        if (error != nullptr) {
            *error = "checkpoint LSN does not match the applied LSN";
        }
        return RdbSaveResult::Failed;
    }

    const RdbSaveResult save_result =
        rdb_manager_->save_checked(*store_, scanner_pause);
    if (save_result != RdbSaveResult::Ok) {
        if (save_result == RdbSaveResult::DurabilityUnconfirmed) {
            // The new snapshot may be visible but its directory entry is not
            // proven durable. Continuing could publish a manifest over an
            // unconfirmed snapshot, so fail closed and retain the old WAL.
            mark_failed();
        }
        if (error != nullptr) {
            *error = save_result == RdbSaveResult::DurabilityUnconfirmed
                         ? "checkpoint snapshot durability is unconfirmed"
                         : "failed to publish checkpoint snapshot";
        }
        return save_result;
    }

    active = wal_->active_segment();
    if (active == nullptr || active->start_lsn != checkpoint_lsn + 1) {
        mark_failed();
        if (error != nullptr) {
            *error = "active WAL segment does not begin after checkpoint LSN";
        }
        return RdbSaveResult::Failed;
    }

    CheckpointManifest manifest;
    manifest.checkpoint_lsn = checkpoint_lsn;
    manifest.retained_segment_starts.push_back(active->start_lsn);
    const ManifestWriteResult manifest_result = write_manifest_atomic(
        manifest_path_, manifest, wal_operations_.get(), error);
    if (manifest_result != ManifestWriteResult::kOk) {
        if (manifest_result == ManifestWriteResult::kDurabilityUnconfirmed) {
            // The manifest rename may have happened but its parent directory
            // was not synced. The old and new manifest are both plausible
            // after a crash; reject further mutations and preserve all WAL.
            mark_failed();
            return RdbSaveResult::DurabilityUnconfirmed;
        }
        return RdbSaveResult::Failed;
    }

    // Old WAL cleanup is safe only after the new manifest is durable. A
    // cleanup failure does not invalidate the checkpoint: recovery ignores
    // residue before the first retained segment and the next checkpoint
    // retries cleanup.
    std::string cleanup_error;
    (void)wal_->cleanup_except_active(&cleanup_error);

    ++stats_.checkpoints;
    stats_.checkpoint_pause_us += steady_now_us() - pause_start_us;
    return RdbSaveResult::Ok;
}

void PersistenceManager::mark_failed() {
    failed_ = true;
}

MutationOutcome PersistenceManager::fail(MutationStatus status,
                                         const std::string& error) {
    MutationOutcome outcome;
    outcome.status = status;
    outcome.error = error;
    return outcome;
}

std::uint64_t PersistenceManager::next_lsn() const {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (wal_) {
        return wal_->next_lsn();
    }
    return last_lsn_ + 1;
}

std::uint64_t PersistenceManager::wal_bytes() const {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    return wal_ ? wal_->total_bytes() : 0;
}

WalManagerStats PersistenceManager::wal_stats() const {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    return wal_ ? wal_->stats() : WalManagerStats{};
}

PersistenceStats PersistenceManager::stats() const {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    return stats_;
}

PersistenceSnapshot PersistenceManager::snapshot() const {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    PersistenceSnapshot result;
    result.durability_mode = durability_mode_;
    result.opened = opened_;
    result.failed = failed_;
    result.last_lsn = last_lsn_;
    result.next_lsn = wal_ ? wal_->next_lsn() : last_lsn_ + 1;
    result.wal_bytes = wal_ ? wal_->total_bytes() : 0;
    result.wal_segment_count = wal_ ? wal_->segments().size() : 0;
    result.recovery = recovery_stats_;
    result.wal = wal_ ? wal_->stats() : WalManagerStats{};
    result.persistence = stats_;
    return result;
}

const char* mutation_status_name(MutationStatus status) {
    switch (status) {
    case MutationStatus::kOk:
        return "ok";
    case MutationStatus::kInvalidArgument:
        return "invalid-argument";
    case MutationStatus::kIoError:
        return "io-error";
    case MutationStatus::kResourceLimit:
        return "resource-limit";
    case MutationStatus::kNotReady:
        return "not-ready";
    }
    return "unknown";
}

} // namespace minidb
