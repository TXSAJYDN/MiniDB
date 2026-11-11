#include "PersistenceManager.h"

#include "../base/Log.h"
#include "CheckpointManifest.h"
#include "WalFormat.h"

#include <chrono>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace minidb {
namespace {

const char* mutation_io_error = "persistence I/O failure";
const char* group_commit_active_error =
    "group commit is active; use the asynchronous persistence API";

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
    if (elapsed_us > *maximum_us) {
        *maximum_us = elapsed_us;
    }
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

MutationStatus mutation_status_from_wal(WalResult result) {
    switch (result) {
    case WalResult::kOk:
        return MutationStatus::kOk;
    case WalResult::kInvalidArgument:
        return MutationStatus::kInvalidArgument;
    case WalResult::kResourceLimit:
        return MutationStatus::kResourceLimit;
    case WalResult::kIoError:
        return MutationStatus::kIoError;
    }
    return MutationStatus::kIoError;
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

PersistenceManager::~PersistenceManager() {
    // The writer borrows WalManager.  Join it before member destruction so a
    // queued mutation never observes a half-destroyed WAL.
    if (group_commit_writer_) {
        group_commit_writer_->stop();
        group_commit_writer_.reset();
    }
}

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
    if (group_commit_started_) {
        return fail(MutationStatus::kNotReady, group_commit_active_error);
    }
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
    const bool measure_sync = durability_mode_ == DurabilityMode::kSync;
    const std::uint64_t started_us = measure_sync ? steady_now_us() : 0;
    MutationOutcome outcome = set_impl(key, value);
    if (measure_sync && outcome.ok()) {
        observe_timing(steady_now_us() - started_us,
                       &stats_.sync_mutation_total_us,
                       &stats_.sync_mutation_max_us);
    }
    return outcome;
}

MutationOutcome PersistenceManager::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (group_commit_started_) {
        return fail(MutationStatus::kNotReady, group_commit_active_error);
    }
    if (!opened_ || failed_) {
        return fail(MutationStatus::kNotReady,
                    failed_ ? mutation_io_error : "persistence is not open");
    }
    if (key.empty() || key.size() > RdbFormat::MAX_KEY_SIZE) {
        return fail(MutationStatus::kInvalidArgument, "invalid key length");
    }
    const bool measure_sync = durability_mode_ == DurabilityMode::kSync;
    const std::uint64_t started_us = measure_sync ? steady_now_us() : 0;
    MutationOutcome outcome = del_impl(key);
    if (measure_sync && outcome.ok()) {
        observe_timing(steady_now_us() - started_us,
                       &stats_.sync_mutation_total_us,
                       &stats_.sync_mutation_max_us);
    }
    return outcome;
}

MutationOutcome PersistenceManager::expire(const std::string& key,
                                           std::int64_t ttl_ms) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (group_commit_started_) {
        return fail(MutationStatus::kNotReady, group_commit_active_error);
    }
    if (!opened_ || failed_) {
        return fail(MutationStatus::kNotReady,
                    failed_ ? mutation_io_error : "persistence is not open");
    }
    if (key.empty() || key.size() > RdbFormat::MAX_KEY_SIZE) {
        return fail(MutationStatus::kInvalidArgument, "invalid key length");
    }
    const bool measure_sync = durability_mode_ == DurabilityMode::kSync;
    const std::uint64_t started_us = measure_sync ? steady_now_us() : 0;
    MutationOutcome outcome = expire_impl(key, ttl_ms);
    if (measure_sync && outcome.ok()) {
        observe_timing(steady_now_us() - started_us,
                       &stats_.sync_mutation_total_us,
                       &stats_.sync_mutation_max_us);
    }
    return outcome;
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

bool PersistenceManager::start_group_commit(
    GroupCommitWriter::OwnerDispatch owner_dispatch, std::string* error) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (!opened_ || failed_) {
        if (error != nullptr) {
            *error = failed_ ? mutation_io_error : "persistence is not open";
        }
        return false;
    }
    if (group_commit_started_ || group_commit_writer_) {
        if (error != nullptr) {
            *error = "group commit writer has already been started";
        }
        return false;
    }
    if (!owner_dispatch) {
        if (error != nullptr) {
            *error = "group commit requires an owner-loop dispatcher";
        }
        return false;
    }

    owner_dispatch_ = owner_dispatch;
    // A legacy or fresh database has no writable WAL yet.  Preserve the
    // established bootstrap contract: startup must not invent a manifest or
    // WAL directory.  The first mutation or checkpoint starts the writer and
    // performs the one-time bootstrap while the owner is briefly blocked.
    if ((wal_ || !recovery_stats_.legacy_import) &&
        !ensure_group_commit_writer_locked(error)) {
        owner_dispatch_ = {};
        return false;
    }

    group_commit_started_ = true;
    accepting_mutations_ = true;
    shutdown_requested_ = false;
    shutdown_complete_ = false;
    return true;
}

bool PersistenceManager::ensure_group_commit_writer_locked(std::string* error) {
    if (group_commit_writer_) {
        return true;
    }
    if (!opened_ || failed_) {
        if (error != nullptr) {
            *error = failed_ ? mutation_io_error : "persistence is not open";
        }
        return false;
    }
    std::string bootstrap_error;
    if (!ensure_bootstrapped_locked(&bootstrap_error)) {
        if (error != nullptr) {
            *error = bootstrap_error.empty() ? "persistence bootstrap failed"
                                             : bootstrap_error;
        }
        return false;
    }

    std::shared_ptr<GroupCommitWriter> writer;
    try {
        writer = std::make_shared<GroupCommitWriter>(
            wal_.get(), durability_mode_, owner_dispatch_,
            [this](GroupCommitCompletion completion) {
                on_group_commit_completion(std::move(completion));
            });
    } catch (const std::exception& ex) {
        mark_failed();
        if (error != nullptr) {
            *error = ex.what();
        }
        return false;
    } catch (...) {
        mark_failed();
        if (error != nullptr) {
            *error = "failed to construct group commit writer";
        }
        return false;
    }

    std::string start_error;
    if (!writer->start(wal_->next_lsn(), &start_error)) {
        mark_failed();
        if (error != nullptr) {
            *error = start_error.empty() ? "failed to start group commit writer"
                                         : start_error;
        }
        return false;
    }

    group_commit_writer_ = std::move(writer);
    return true;
}

bool PersistenceManager::group_commit_started() const {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    return group_commit_started_;
}

MutationAdmission PersistenceManager::set_async(const std::string& key,
                                                std::string value,
                                                MutationCompletion completion) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (!opened_ || failed_) {
        return admission_fail(failed_ ? MutationAdmissionStatus::kIoError
                                      : MutationAdmissionStatus::kNotReady,
                              failed_ ? mutation_io_error
                                      : "persistence is not open");
    }
    if (!group_commit_started_) {
        return admission_fail(MutationAdmissionStatus::kNotReady,
                              "group commit writer is not started");
    }
    if (shutdown_requested_ || shutdown_complete_) {
        return admission_fail(MutationAdmissionStatus::kShuttingDown,
                              "persistence is shutting down");
    }
    if (!accepting_mutations_ || control_pending_) {
        return admission_fail(MutationAdmissionStatus::kNotReady,
                              "persistence admission is paused");
    }
    if (key.empty() || key.size() > RdbFormat::MAX_KEY_SIZE) {
        return admission_fail(MutationAdmissionStatus::kInvalidArgument,
                              "invalid key length");
    }
    if (value.size() > WalFormat::MAX_VALUE_SIZE) {
        return admission_fail(MutationAdmissionStatus::kResourceLimit,
                              "value exceeds WAL limit");
    }

    std::string writer_error;
    if (!ensure_group_commit_writer_locked(&writer_error)) {
        return admission_fail(failed_ ? MutationAdmissionStatus::kIoError
                                      : MutationAdmissionStatus::kNotReady,
                              writer_error);
    }

    WalRecord record;
    record.lsn = group_commit_writer_->next_lsn();
    record.op = WalOp::kSet;
    record.key = key;
    record.value = std::move(value);
    return admit_async(std::move(record), std::move(completion));
}

MutationAdmission PersistenceManager::del_async(std::string key,
                                                MutationCompletion completion) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (!opened_ || failed_) {
        return admission_fail(failed_ ? MutationAdmissionStatus::kIoError
                                      : MutationAdmissionStatus::kNotReady,
                              failed_ ? mutation_io_error
                                      : "persistence is not open");
    }
    if (!group_commit_started_) {
        return admission_fail(MutationAdmissionStatus::kNotReady,
                              "group commit writer is not started");
    }
    if (shutdown_requested_ || shutdown_complete_) {
        return admission_fail(MutationAdmissionStatus::kShuttingDown,
                              "persistence is shutting down");
    }
    if (!accepting_mutations_ || control_pending_) {
        return admission_fail(MutationAdmissionStatus::kNotReady,
                              "persistence admission is paused");
    }
    if (key.empty() || key.size() > RdbFormat::MAX_KEY_SIZE) {
        return admission_fail(MutationAdmissionStatus::kInvalidArgument,
                              "invalid key length");
    }

    std::string writer_error;
    if (!ensure_group_commit_writer_locked(&writer_error)) {
        return admission_fail(failed_ ? MutationAdmissionStatus::kIoError
                                      : MutationAdmissionStatus::kNotReady,
                              writer_error);
    }

    WalRecord record;
    record.lsn = group_commit_writer_->next_lsn();
    record.op = WalOp::kDel;
    record.key = std::move(key);
    return admit_async(std::move(record), std::move(completion));
}

MutationAdmission
PersistenceManager::expire_async(std::string key, std::int64_t ttl_ms,
                                 MutationCompletion completion) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (!opened_ || failed_) {
        return admission_fail(failed_ ? MutationAdmissionStatus::kIoError
                                      : MutationAdmissionStatus::kNotReady,
                              failed_ ? mutation_io_error
                                      : "persistence is not open");
    }
    if (!group_commit_started_) {
        return admission_fail(MutationAdmissionStatus::kNotReady,
                              "group commit writer is not started");
    }
    if (shutdown_requested_ || shutdown_complete_) {
        return admission_fail(MutationAdmissionStatus::kShuttingDown,
                              "persistence is shutting down");
    }
    if (!accepting_mutations_ || control_pending_) {
        return admission_fail(MutationAdmissionStatus::kNotReady,
                              "persistence admission is paused");
    }
    if (key.empty() || key.size() > RdbFormat::MAX_KEY_SIZE) {
        return admission_fail(MutationAdmissionStatus::kInvalidArgument,
                              "invalid key length");
    }

    std::string writer_error;
    if (!ensure_group_commit_writer_locked(&writer_error)) {
        return admission_fail(failed_ ? MutationAdmissionStatus::kIoError
                                      : MutationAdmissionStatus::kNotReady,
                              writer_error);
    }

    WalRecord record;
    record.lsn = group_commit_writer_->next_lsn();
    record.key = std::move(key);
    if (ttl_ms <= 0) {
        record.op = WalOp::kDel;
    } else {
        record.op = WalOp::kExpire;
        record.expiry_ms =
            add_ttl_saturated(store_->wall_clock_now_ms(), ttl_ms);
    }
    return admit_async(std::move(record), std::move(completion));
}

MutationAdmission
PersistenceManager::admit_async(WalRecord record,
                                MutationCompletion completion) {
    const std::uint64_t lsn = record.lsn;
    const std::uint64_t started_us = steady_now_us();

    // Publish the bookkeeping entry before handing ownership to the writer.
    // The owner dispatch contract prevents a drain from running inline, so
    // the entry is always visible by the time its completion is delivered.
    pending_mutations_.push_back({lsn, started_us, std::move(completion)});

    GroupCommitEntry entry;
    entry.record = std::move(record);
    entry.submitted_us = started_us;
    std::string submit_error;
    if (!group_commit_writer_->submit(std::move(entry), &submit_error)) {
        pending_mutations_.pop_back();
        MutationAdmissionStatus status = MutationAdmissionStatus::kNotReady;
        const GroupCommitWriterState state = group_commit_writer_->state();
        if (state == GroupCommitWriterState::kFailed) {
            status = MutationAdmissionStatus::kIoError;
        } else if (state == GroupCommitWriterState::kStopped ||
                   state == GroupCommitWriterState::kDraining) {
            status = MutationAdmissionStatus::kShuttingDown;
        } else if (submit_error.find("full") != std::string::npos) {
            status = MutationAdmissionStatus::kResourceLimit;
        }
        return admission_fail(status, submit_error);
    }

    MutationAdmission admission;
    admission.status = MutationAdmissionStatus::kAccepted;
    return admission;
}

MutationAdmission
PersistenceManager::admission_fail(MutationAdmissionStatus status,
                                   const std::string& error) const {
    MutationAdmission result;
    result.status = status;
    result.error = error;
    return result;
}

bool PersistenceManager::flush_interval(std::string* error) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (!opened_ || failed_) {
        if (error != nullptr) {
            *error = failed_ ? mutation_io_error : "persistence is not open";
        }
        return false;
    }
    if (group_commit_started_) {
        if (error != nullptr) {
            *error = group_commit_active_error;
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

bool PersistenceManager::flush_interval_async(
    GroupCommitWriter::BarrierCompletion completion, std::string* error) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (!opened_ || failed_) {
        if (error != nullptr) {
            *error = failed_ ? mutation_io_error : "persistence is not open";
        }
        return false;
    }
    if (!group_commit_started_) {
        if (error != nullptr) {
            *error = "group commit writer is not started";
        }
        return false;
    }
    if (shutdown_requested_ || shutdown_complete_) {
        if (error != nullptr) {
            *error = "persistence is shutting down";
        }
        return false;
    }
    if (control_pending_ || !accepting_mutations_) {
        if (error != nullptr) {
            *error = "persistence control operation is in progress";
        }
        return false;
    }
    if (!group_commit_writer_) {
        // No record has been admitted since startup, so there is nothing to
        // fsync.  Preserve asynchronous completion without manufacturing a
        // WAL or manifest just because an INTERVAL timer tick fired.
        if (completion) {
            owner_dispatch_([completion = std::move(completion)]() mutable {
                completion(true, {});
            });
        }
        return true;
    }

    const bool count_interval_flush =
        durability_mode_ == DurabilityMode::kInterval;
    auto wrapped = [this, completion = std::move(completion),
                    count_interval_flush](
                       bool success, const std::string& flush_error) mutable {
        if (success && count_interval_flush) {
            std::lock_guard<std::mutex> lock(operation_mutex_);
            ++stats_.interval_flushes;
        }
        if (completion) {
            completion(success, flush_error);
        }
    };
    return group_commit_writer_->request_flush(std::move(wrapped), error);
}

RdbSaveResult PersistenceManager::checkpoint(std::string* error) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (!opened_ || failed_) {
        if (error != nullptr) {
            *error = failed_ ? mutation_io_error : "persistence is not open";
        }
        return RdbSaveResult::Failed;
    }
    if (group_commit_started_) {
        if (error != nullptr) {
            *error = group_commit_active_error;
        }
        return RdbSaveResult::Failed;
    }
    return checkpoint_locked(error);
}

bool PersistenceManager::checkpoint_async(CheckpointCompletion completion,
                                          std::string* error) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    return start_checkpoint_locked(std::move(completion), false, error);
}

bool PersistenceManager::shutdown_async(CheckpointCompletion completion,
                                        std::string* error) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (shutdown_requested_ || shutdown_complete_) {
        if (error != nullptr) {
            *error = "persistence shutdown has already been requested";
        }
        return false;
    }
    return start_checkpoint_locked(std::move(completion), true, error);
}

bool PersistenceManager::start_checkpoint_locked(
    CheckpointCompletion completion, bool shutdown, std::string* error) {
    if (!opened_ || failed_) {
        if (error != nullptr) {
            *error = failed_ ? mutation_io_error : "persistence is not open";
        }
        return false;
    }
    if (!group_commit_started_) {
        if (error != nullptr) {
            *error = "group commit writer is not started";
        }
        return false;
    }
    if (control_pending_) {
        if (error != nullptr) {
            *error = "another persistence control operation is in progress";
        }
        return false;
    }
    if (shutdown_complete_) {
        if (error != nullptr) {
            *error = "persistence shutdown is complete";
        }
        return false;
    }
    if (!shutdown && shutdown_requested_) {
        if (error != nullptr) {
            *error = "persistence is shutting down";
        }
        return false;
    }

    std::string writer_error;
    if (!ensure_group_commit_writer_locked(&writer_error)) {
        if (error != nullptr) {
            *error = writer_error;
        }
        return false;
    }

    auto operation = std::make_unique<CheckpointOperation>();
    operation->completion = std::move(completion);
    operation->shutdown = shutdown;
    operation->started_us = steady_now_us();
    checkpoint_operation_ = std::move(operation);
    control_pending_ = true;
    accepting_mutations_ = false;
    if (shutdown) {
        shutdown_requested_ = true;
    }

    const GroupCommitWriterState state = group_commit_writer_->state();
    std::string control_error;
    bool requested = false;
    if (state == GroupCommitWriterState::kRunning) {
        requested = group_commit_writer_->request_pause(
            [this](bool success, const std::string& pause_error) {
                on_checkpoint_paused(success, pause_error);
            },
            &control_error);
    } else if (state == GroupCommitWriterState::kPaused) {
        requested = group_commit_writer_->request_rotate(
            [this](const GroupCommitWalBarrierInfo& info) {
                on_checkpoint_rotated(info);
            },
            &control_error);
    } else {
        control_error = "group commit writer is not checkpointable";
    }

    if (!requested) {
        checkpoint_operation_.reset();
        control_pending_ = false;
        if (shutdown) {
            shutdown_requested_ = false;
        }
        if (state == GroupCommitWriterState::kRunning) {
            accepting_mutations_ = true;
        }
        if (error != nullptr) {
            *error = control_error;
        }
        return false;
    }
    return true;
}

void PersistenceManager::on_checkpoint_paused(bool success,
                                              const std::string& error) {
    bool finish_now = false;
    std::string finish_error;
    {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        if (!checkpoint_operation_) {
            return;
        }
        if (!success) {
            mark_failed();
            finish_now = true;
            finish_error =
                error.empty() ? "group commit pause barrier failed" : error;
        } else if (!pending_mutations_.empty()) {
            mark_failed();
            finish_now = true;
            finish_error =
                "checkpoint paused before all mutations were applied";
        } else {
            checkpoint_operation_->paused_us = steady_now_us();
            std::string rotate_error;
            if (!group_commit_writer_->request_rotate(
                    [this](const GroupCommitWalBarrierInfo& info) {
                        on_checkpoint_rotated(info);
                    },
                    &rotate_error)) {
                mark_failed();
                finish_now = true;
                finish_error = rotate_error;
            }
        }
    }
    if (finish_now) {
        finish_checkpoint(RdbSaveResult::Failed, finish_error);
    }
}

void PersistenceManager::on_checkpoint_rotated(
    const GroupCommitWalBarrierInfo& info) {
    bool finish_now = true;
    bool wait_for_cleanup = false;
    bool resume_after_failure = false;
    RdbSaveResult result = RdbSaveResult::Failed;
    std::string finish_error;

    if (!info.success) {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        mark_failed();
        finish_error = info.error.empty() ? "group commit rotate barrier failed"
                                          : info.error;
    } else {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        if (!checkpoint_operation_) {
            return;
        }

        const std::uint64_t checkpoint_lsn =
            info.next_lsn == 0 ? 0 : info.next_lsn - 1;
        if (info.next_lsn == 0 || checkpoint_lsn != last_lsn_) {
            mark_failed();
            finish_error = "checkpoint rotate LSN does not match applied LSN";
        } else if (info.active_segment_start_lsn != checkpoint_lsn + 1) {
            mark_failed();
            finish_error =
                "active WAL segment does not begin after checkpoint LSN";
        } else {
            KvStore::ScannerPause scanner_pause = store_->pause_scanner();
            const RdbSaveResult save_result =
                rdb_manager_->save_checked(*store_, scanner_pause);
            if (save_result != RdbSaveResult::Ok) {
                result = save_result;
                finish_error =
                    save_result == RdbSaveResult::DurabilityUnconfirmed
                        ? "checkpoint snapshot durability is unconfirmed"
                        : "failed to publish checkpoint snapshot";
                if (save_result == RdbSaveResult::DurabilityUnconfirmed) {
                    mark_failed();
                } else if (!checkpoint_operation_->shutdown) {
                    resume_after_failure = true;
                }
            } else {
                CheckpointManifest manifest;
                manifest.checkpoint_lsn = checkpoint_lsn;
                manifest.retained_segment_starts.push_back(
                    info.active_segment_start_lsn);
                std::string manifest_error;
                const ManifestWriteResult manifest_result =
                    write_manifest_atomic(manifest_path_, manifest,
                                          wal_operations_.get(),
                                          &manifest_error);
                if (manifest_result != ManifestWriteResult::kOk) {
                    finish_error = manifest_error.empty()
                                       ? "failed to publish checkpoint manifest"
                                       : manifest_error;
                    if (manifest_result ==
                        ManifestWriteResult::kDurabilityUnconfirmed) {
                        mark_failed();
                        result = RdbSaveResult::DurabilityUnconfirmed;
                    } else {
                        result = RdbSaveResult::Failed;
                        if (!checkpoint_operation_->shutdown) {
                            resume_after_failure = true;
                        }
                    }
                } else {
                    std::string cleanup_error;
                    if (group_commit_writer_->request_cleanup(
                            [this](bool cleanup_success,
                                   const std::string& error) {
                                on_checkpoint_cleanup(cleanup_success, error);
                            },
                            &cleanup_error)) {
                        // The manifest is durable.  Keep the owner paused
                        // until cleanup reports completion.
                        wait_for_cleanup = true;
                    } else {
                        // A checkpoint is already durable.  Leaving the writer
                        // paused lets the next SAVE retry cleanup without
                        // publishing an unsafe manifest.
                        result = RdbSaveResult::Ok;
                    }
                }
            }

            if (resume_after_failure) {
                std::string resume_error;
                if (!group_commit_writer_->resume(&resume_error)) {
                    mark_failed();
                    result = RdbSaveResult::Failed;
                    finish_error =
                        "could not resume after checkpoint failure: " +
                        resume_error;
                }
            }
        }
    }

    if (wait_for_cleanup) {
        return;
    }
    if (finish_now) {
        finish_checkpoint(result, finish_error);
    }
}

void PersistenceManager::on_checkpoint_cleanup(bool success,
                                               const std::string&) {
    bool finish_now = true;
    RdbSaveResult result = RdbSaveResult::Ok;
    std::string finish_error;
    {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        if (!checkpoint_operation_) {
            return;
        }
        if (success && !checkpoint_operation_->shutdown) {
            std::string resume_error;
            if (!group_commit_writer_->resume(&resume_error)) {
                mark_failed();
                result = RdbSaveResult::Failed;
                finish_error =
                    "cleanup completed but group commit could not resume: " +
                    resume_error;
            }
        } else if (!success) {
            // Manifest publication already made this checkpoint durable.
            // Residue is harmless; leave the writer paused so a later SAVE
            // retries cleanup.
        }
    }
    if (finish_now) {
        finish_checkpoint(result, finish_error);
    }
}

void PersistenceManager::finish_checkpoint(RdbSaveResult result,
                                           const std::string& error) {
    CheckpointCompletion completion;
    bool shutdown = false;
    std::uint64_t pause_us = 0;
    {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        if (!checkpoint_operation_) {
            return;
        }
        completion = std::move(checkpoint_operation_->completion);
        shutdown = checkpoint_operation_->shutdown;
        const std::uint64_t paused_us = checkpoint_operation_->paused_us;
        if (paused_us != 0) {
            const std::uint64_t now = steady_now_us();
            if (now >= paused_us) {
                pause_us = now - paused_us;
            }
        }
        checkpoint_operation_.reset();
        control_pending_ = false;
        if (result == RdbSaveResult::Ok) {
            ++stats_.checkpoints;
            add_saturated(&stats_.checkpoint_pause_us, pause_us);
        }
        if (!shutdown && group_commit_writer_ &&
            group_commit_writer_->state() == GroupCommitWriterState::kRunning &&
            !failed_) {
            accepting_mutations_ = true;
        } else {
            accepting_mutations_ = false;
        }
        if (shutdown) {
            shutdown_requested_ = true;
            accepting_mutations_ = false;
            if (result != RdbSaveResult::Ok) {
                failed_ = true;
            }
        }
    }

    if (shutdown && group_commit_writer_) {
        group_commit_writer_->stop();
        std::lock_guard<std::mutex> lock(operation_mutex_);
        shutdown_complete_ = true;
        accepting_mutations_ = false;
    }

    if (completion) {
        try {
            completion(result, error);
        } catch (const std::exception& ex) {
            Log::Error(std::string("checkpoint completion threw: ") +
                       ex.what());
        } catch (...) {
            Log::Error("checkpoint completion threw an unknown exception");
        }
    }
}

void PersistenceManager::on_group_commit_completion(
    GroupCommitCompletion completion) {
    if (completion.kind == GroupCommitCompletion::Kind::kRecords) {
        struct CompletionCall {
            MutationCompletion completion;
            MutationOutcome outcome;
        };
        std::vector<CompletionCall> callbacks;
        callbacks.reserve(completion.records.size());

        {
            std::lock_guard<std::mutex> lock(operation_mutex_);
            for (GroupCommitRecordResult& result : completion.records) {
                if (pending_mutations_.empty()) {
                    mark_failed();
                    break;
                }
                PendingMutation pending = std::move(pending_mutations_.front());
                pending_mutations_.pop_front();

                MutationOutcome outcome;
                if (pending.lsn != result.record.lsn) {
                    mark_failed();
                    outcome.status = MutationStatus::kIoError;
                    outcome.error = "group commit completion LSN mismatch";
                } else if (!result.ok() || !result.written) {
                    mark_failed();
                    outcome.status = mutation_status_from_wal(result.status);
                    outcome.error = result.error.empty()
                                        ? "group commit WAL operation failed"
                                        : result.error;
                } else if (failed_) {
                    outcome.status = MutationStatus::kIoError;
                    outcome.error = mutation_io_error;
                } else if (result.record.lsn != last_lsn_ + 1) {
                    mark_failed();
                    outcome.status = MutationStatus::kIoError;
                    outcome.error = "group commit apply LSN is not contiguous";
                } else {
                    bool changed = true;
                    try {
                        switch (result.record.op) {
                        case WalOp::kSet:
                            store_->set(result.record.key, result.record.value);
                            break;
                        case WalOp::kDel:
                            changed = store_->del(result.record.key);
                            break;
                        case WalOp::kExpire:
                            changed = store_->expire_absolute(
                                result.record.key, result.record.expiry_ms);
                            break;
                        default:
                            mark_failed();
                            outcome.status = MutationStatus::kInvalidArgument;
                            outcome.error = "unknown WAL operation";
                            break;
                        }
                    } catch (const std::exception& ex) {
                        mark_failed();
                        outcome.status = MutationStatus::kIoError;
                        outcome.error =
                            std::string("store apply failed: ") + ex.what();
                    } catch (...) {
                        mark_failed();
                        outcome.status = MutationStatus::kIoError;
                        outcome.error = "store apply failed";
                    }

                    if (outcome.ok()) {
                        last_lsn_ = result.record.lsn;
                        if (after_apply_callback_) {
                            after_apply_callback_(result.record);
                        }
                        group_commit_writer_->observe_applied(result,
                                                              steady_now_us());
                        ++stats_.mutations;
                        if (durability_mode_ == DurabilityMode::kSync) {
                            ++stats_.sync_mutations;
                            const std::uint64_t now = steady_now_us();
                            if (now >= pending.started_us) {
                                observe_timing(now - pending.started_us,
                                               &stats_.sync_mutation_total_us,
                                               &stats_.sync_mutation_max_us);
                            }
                        } else {
                            ++stats_.interval_mutations;
                        }
                        outcome.changed = changed;
                    }
                }

                callbacks.push_back(
                    {std::move(pending.completion), std::move(outcome)});
            }
        }

        for (CompletionCall& call : callbacks) {
            if (!call.completion) {
                continue;
            }
            try {
                call.completion(std::move(call.outcome));
            } catch (const std::exception& ex) {
                Log::Error(std::string("mutation completion threw: ") +
                           ex.what());
            } catch (...) {
                Log::Error("mutation completion threw an unknown exception");
            }
        }
        return;
    }

    if (completion.kind == GroupCommitCompletion::Kind::kBarrier) {
        if (completion.barrier_completion) {
            try {
                completion.barrier_completion(completion.success,
                                              completion.error);
            } catch (const std::exception& ex) {
                Log::Error(std::string("barrier completion threw: ") +
                           ex.what());
            } catch (...) {
                Log::Error("barrier completion threw an unknown exception");
            }
        }
        return;
    }

    if (completion.wal_barrier_completion) {
        try {
            completion.wal_barrier_completion(completion.wal_barrier);
        } catch (const std::exception& ex) {
            Log::Error(std::string("WAL barrier completion threw: ") +
                       ex.what());
        } catch (...) {
            Log::Error("WAL barrier completion threw an unknown exception");
        }
    }
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
    add_saturated(&stats_.checkpoint_pause_us,
                  steady_now_us() - pause_start_us);
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
    if (group_commit_writer_) {
        return group_commit_writer_->wal_snapshot().next_lsn;
    }
    if (wal_) {
        return wal_->next_lsn();
    }
    return last_lsn_ + 1;
}

std::uint64_t PersistenceManager::wal_bytes() const {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (group_commit_writer_) {
        return group_commit_writer_->wal_snapshot().wal_total_bytes;
    }
    return wal_ ? wal_->total_bytes() : 0;
}

WalManagerStats PersistenceManager::wal_stats() const {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (group_commit_writer_) {
        return group_commit_writer_->wal_snapshot().wal;
    }
    return wal_ ? wal_->stats() : WalManagerStats{};
}

GroupCommitStats PersistenceManager::group_commit_stats() const {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    return group_commit_writer_ ? group_commit_writer_->stats()
                                : GroupCommitStats{};
}

PersistenceStats PersistenceManager::stats() const {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    return stats_;
}

bool PersistenceManager::shutdown_complete() const {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    return shutdown_complete_;
}

PersistenceSnapshot PersistenceManager::snapshot() const {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    PersistenceSnapshot result;
    result.durability_mode = durability_mode_;
    result.opened = opened_;
    result.failed = failed_;
    result.last_lsn = last_lsn_;
    result.recovery = recovery_stats_;
    result.persistence = stats_;
    if (group_commit_writer_) {
        const GroupCommitWalSnapshot wal_snapshot =
            group_commit_writer_->wal_snapshot();
        result.next_lsn = wal_snapshot.next_lsn;
        result.wal_bytes = wal_snapshot.wal_total_bytes;
        result.wal_segment_count = wal_snapshot.wal_segment_count;
        result.wal = wal_snapshot.wal;
        result.group_commit = wal_snapshot.group_commit;
    } else {
        result.next_lsn = wal_ ? wal_->next_lsn() : last_lsn_ + 1;
        result.wal_bytes = wal_ ? wal_->total_bytes() : 0;
        result.wal_segment_count = wal_ ? wal_->segments().size() : 0;
        result.wal = wal_ ? wal_->stats() : WalManagerStats{};
        result.group_commit = GroupCommitStats{};
    }
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
