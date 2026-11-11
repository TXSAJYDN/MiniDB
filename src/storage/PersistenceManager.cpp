#include "PersistenceManager.h"

#include "../base/Log.h"
#include "CheckpointManifest.h"
#include "WalFormat.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <string>
#include <utility>
#include <vector>

namespace minidb {

namespace detail {

// A cross-thread completion may already be sitting in the owner EventLoop
// queue when the manager is destroyed.  The queue owns that std::function, so
// it cannot be cancelled; instead, both the worker and the owner share this
// state and the callback becomes a no-op once destruction starts.
struct PersistenceManagerCallbackState {
    void run(std::function<void()> work) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping || owner == nullptr) {
                return;
            }
            ++active_callbacks;
        }
        struct ActiveGuard {
            PersistenceManagerCallbackState* state;
            ~ActiveGuard() {
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    --state->active_callbacks;
                }
                state->condition.notify_all();
            }
        } guard{this};
        work();
    }

    void stop() {
        std::lock_guard<std::mutex> lock(mutex);
        stopping = true;
        owner = nullptr;
    }

    void wait_for_idle() {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [this]() { return active_callbacks == 0; });
    }

    std::mutex mutex;
    std::condition_variable condition;
    PersistenceManager* owner = nullptr;
    bool stopping = false;
    std::size_t active_callbacks = 0;
};

} // namespace detail

namespace {

namespace fs = std::filesystem;

// Crash-test seam for the asynchronous checkpoint path only.  A process test
// can observe that the new RDB and parent directory are durable while the old
// manifest is still authoritative, then SIGKILL the server.  Production never
// sets these variables, so this is a no-op outside crash tests.
void maybe_pause_after_checkpoint_rdb_publish() {
    const char* marker_path = std::getenv("MINIDB_TEST_RDB_PUBLISH_MARKER");
    if (marker_path == nullptr || *marker_path == '\0') {
        return;
    }
    std::ofstream marker(marker_path, std::ios::trunc);
    if (!marker.is_open()) {
        return;
    }
    marker << "rdb-published\n";
    marker.close();

    const char* release_path = std::getenv("MINIDB_TEST_RDB_PUBLISH_RELEASE");
    if (release_path == nullptr || *release_path == '\0') {
        return;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(60);
    std::error_code ec;
    while (std::chrono::steady_clock::now() < deadline) {
        if (fs::exists(release_path, ec) && !ec) {
            return;
        }
        ec.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

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
    owner_callback_state_ =
        std::make_shared<detail::PersistenceManagerCallbackState>();
    owner_callback_state_->owner = this;
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
    // Stop accepting owner-loop callbacks first.  A checkpoint worker may have
    // already queued its completion; that queued closure must not touch a
    // half-destroyed manager.  Wait for a callback that already started, then
    // join the worker before destroying the RDB manager or dispatcher.
    if (owner_callback_state_) {
        owner_callback_state_->stop();
        owner_callback_state_->wait_for_idle();
    }
    join_checkpoint_worker();
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
        auto callback_state = owner_callback_state_;
        auto raw_dispatch = owner_dispatch_;
        GroupCommitWriter::OwnerDispatch guarded_dispatch =
            [callback_state, raw_dispatch = std::move(raw_dispatch)](
                std::function<void()> work) mutable {
                if (!raw_dispatch || !work) {
                    return;
                }
                raw_dispatch(
                    [callback_state, work = std::move(work)]() mutable {
                        callback_state->run(std::move(work));
                    });
            };
        writer = std::make_shared<GroupCommitWriter>(
            wal_.get(), durability_mode_, std::move(guarded_dispatch),
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
    if (!accepting_mutations_) {
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
    if (!accepting_mutations_) {
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
    if (!accepting_mutations_) {
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
            dispatch_owner([completion = std::move(completion)]() mutable {
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
    if (checkpoint_operation_) {
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
    if (checkpoint_worker_.joinable()) {
        if (error != nullptr) {
            *error = "previous checkpoint worker has not been joined";
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
    if (shutdown) {
        shutdown_requested_ = true;
    }
    pause_checkpoint_admission_locked(steady_now_us());
    stats_.background_checkpoint_state = BackgroundCheckpointState::kCapture;

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
        checkpoint_operation_->phase = CheckpointPhase::kRotate;
        requested = group_commit_writer_->request_rotate(
            [this](const GroupCommitWalBarrierInfo& info) {
                on_checkpoint_rotated(info);
            },
            &control_error);
    } else {
        control_error = "group commit writer is not checkpointable";
    }

    if (!requested) {
        resume_checkpoint_admission_locked(steady_now_us());
        checkpoint_operation_.reset();
        control_pending_ = false;
        if (shutdown) {
            shutdown_requested_ = false;
        }
        if (state == GroupCommitWriterState::kRunning && !failed_) {
            accepting_mutations_ = true;
        }
        stats_.background_checkpoint_state =
            failed_ ? BackgroundCheckpointState::kFailed
                    : BackgroundCheckpointState::kIdle;
        if (error != nullptr) {
            *error = control_error;
        }
        return false;
    }
    return true;
}

void PersistenceManager::pause_checkpoint_admission_locked(
    std::uint64_t now_us) {
    accepting_mutations_ = false;
    if (checkpoint_operation_ &&
        checkpoint_operation_->admission_blocked_started_us == 0) {
        checkpoint_operation_->admission_blocked_started_us = now_us;
    }
}

void PersistenceManager::resume_checkpoint_admission_locked(
    std::uint64_t now_us) {
    if (!checkpoint_operation_ ||
        checkpoint_operation_->admission_blocked_started_us == 0) {
        return;
    }
    const std::uint64_t started_us =
        checkpoint_operation_->admission_blocked_started_us;
    checkpoint_operation_->admission_blocked_started_us = 0;
    if (now_us >= started_us) {
        const std::uint64_t elapsed_us = now_us - started_us;
        observe_timing(elapsed_us,
                       &stats_.checkpoint_admission_blocked_total_us,
                       &stats_.checkpoint_admission_blocked_max_us);
        add_saturated(&stats_.checkpoint_pause_us, elapsed_us);
    }
}

void PersistenceManager::on_checkpoint_paused(bool success,
                                              const std::string& error) {
    bool finish_now = false;
    RdbSaveResult result = RdbSaveResult::Failed;
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
            checkpoint_operation_->phase = CheckpointPhase::kRotate;
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
        finish_checkpoint(result, finish_error);
    }
}

void PersistenceManager::on_checkpoint_rotated(
    const GroupCommitWalBarrierInfo& info) {
    bool finish_now = false;
    RdbSaveResult result = RdbSaveResult::Failed;
    std::string finish_error;
    {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        if (!checkpoint_operation_) {
            return;
        }
        if (!info.success) {
            mark_failed();
            finish_now = true;
            finish_error = info.error.empty()
                               ? "group commit rotate barrier failed"
                               : info.error;
        } else {
            const std::uint64_t checkpoint_lsn =
                info.next_lsn == 0 ? 0 : info.next_lsn - 1;
            if (info.next_lsn == 0 || checkpoint_lsn != last_lsn_) {
                mark_failed();
                finish_now = true;
                finish_error =
                    "checkpoint rotate LSN does not match applied LSN";
            } else if (info.active_segment_start_lsn != checkpoint_lsn + 1) {
                mark_failed();
                finish_now = true;
                finish_error =
                    "active WAL segment does not begin after checkpoint LSN";
            } else {
                RdbSnapshot snapshot;
                std::string capture_error;
                const std::uint64_t capture_started_us = steady_now_us();
                const bool captured =
                    rdb_manager_->capture(*store_, &snapshot, &capture_error);
                const std::uint64_t capture_finished_us = steady_now_us();
                checkpoint_operation_->capture_us =
                    capture_finished_us >= capture_started_us
                        ? capture_finished_us - capture_started_us
                        : 0;
                if (!captured) {
                    finish_now = true;
                    finish_error = capture_error.empty()
                                       ? "failed to capture checkpoint snapshot"
                                       : capture_error;
                    // The existing RDB, manifest and WAL remain untouched.
                    // Resume normal admissions before reporting the failure.
                    std::string resume_error;
                    if (!group_commit_writer_->resume(&resume_error)) {
                        mark_failed();
                        finish_error +=
                            "; group commit could not resume: " + resume_error;
                    }
                } else {
                    checkpoint_operation_->checkpoint_lsn = checkpoint_lsn;
                    checkpoint_operation_->phase = CheckpointPhase::kWriting;
                    stats_.background_checkpoint_state =
                        BackgroundCheckpointState::kWriting;

                    std::string resume_error;
                    if (!group_commit_writer_->resume(&resume_error)) {
                        mark_failed();
                        finish_now = true;
                        finish_error = "captured snapshot but group commit "
                                       "could not resume: " +
                                       resume_error;
                    } else {
                        // The snapshot is independent of the live store.  Only
                        // this short handoff remains inside the admission cut;
                        // serialization and durable I/O run on the worker.
                        resume_checkpoint_admission_locked(steady_now_us());
                        accepting_mutations_ = true;
                    }

                    if (!finish_now) {
                        try {
                            CheckpointJob job;
                            job.snapshot = std::move(snapshot);
                            checkpoint_worker_ = std::thread([this,
                                                              job = std::move(
                                                                  job)]() mutable {
                                std::vector<std::uint8_t> image;
                                RdbSaveTimings timings;
                                std::string serialize_error;
                                RdbSaveResult write_result =
                                    RdbSaveResult::Failed;
                                std::string write_error;
                                if (!rdb_manager_->serialize(
                                        job.snapshot, &image, &timings,
                                        &serialize_error)) {
                                    write_error =
                                        serialize_error.empty()
                                            ? "RDB serialization failed"
                                            : serialize_error;
                                } else {
                                    write_result = rdb_manager_->write_durable(
                                        image, &timings, &write_error);
                                    if (write_result == RdbSaveResult::Ok) {
                                        maybe_pause_after_checkpoint_rdb_publish();
                                    }
                                    if (write_result != RdbSaveResult::Ok &&
                                        write_error.empty()) {
                                        write_error =
                                            write_result ==
                                                    RdbSaveResult::
                                                        DurabilityUnconfirmed
                                                ? "checkpoint snapshot "
                                                  "durability is "
                                                  "unconfirmed"
                                                : "failed to publish "
                                                  "checkpoint snapshot";
                                    }
                                }
                                try {
                                    dispatch_owner([this, write_result,
                                                    write_error =
                                                        std::move(write_error),
                                                    timings]() mutable {
                                        on_checkpoint_written(
                                            write_result, write_error,
                                            timings.serialize_us,
                                            timings.write_us, timings.fsync_us);
                                    });
                                } catch (const std::exception& ex) {
                                    Log::Error(
                                        std::string("checkpoint completion "
                                                    "dispatch failed: ") +
                                        ex.what());
                                } catch (...) {
                                    Log::Error("checkpoint completion "
                                               "dispatch failed");
                                }
                            });
                        } catch (const std::exception& ex) {
                            mark_failed();
                            finish_now = true;
                            finish_error =
                                std::string(
                                    "failed to start checkpoint worker: ") +
                                ex.what();
                        } catch (...) {
                            mark_failed();
                            finish_now = true;
                            finish_error = "failed to start checkpoint worker";
                        }
                    }
                }
            }
        }
    }
    if (finish_now) {
        finish_checkpoint(result, finish_error);
    }
}

void PersistenceManager::on_checkpoint_written(RdbSaveResult result,
                                               const std::string& error,
                                               std::uint64_t serialize_us,
                                               std::uint64_t write_us,
                                               std::uint64_t fsync_us) {
    join_checkpoint_worker();
    bool finish_now = false;
    bool finalize_immediately = false;
    std::string finish_error = error;
    {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        if (!checkpoint_operation_) {
            return;
        }
        if (checkpoint_operation_->phase != CheckpointPhase::kWriting) {
            mark_failed();
            finish_now = true;
            finish_error = "checkpoint worker completed in the wrong phase";
        } else {
            checkpoint_operation_->serialize_us = serialize_us;
            checkpoint_operation_->write_us = write_us;
            checkpoint_operation_->fsync_us = fsync_us;
            if (result != RdbSaveResult::Ok) {
                finish_now = true;
                if (result == RdbSaveResult::DurabilityUnconfirmed) {
                    mark_failed();
                } else {
                    if (finish_error.empty()) {
                        finish_error = "failed to publish checkpoint snapshot";
                    }
                    if (!checkpoint_operation_->shutdown &&
                        group_commit_writer_->state() ==
                            GroupCommitWriterState::kPaused) {
                        std::string resume_error;
                        if (!group_commit_writer_->resume(&resume_error)) {
                            mark_failed();
                            finish_error +=
                                "; group commit could not resume: " +
                                resume_error;
                        }
                    }
                }
            } else {
                checkpoint_operation_->phase = CheckpointPhase::kFinalPause;
                stats_.background_checkpoint_state =
                    BackgroundCheckpointState::kFinalizing;
                // Cut admission again before draining writes that arrived
                // while the snapshot was being serialized and written.
                pause_checkpoint_admission_locked(steady_now_us());
                const GroupCommitWriterState state =
                    group_commit_writer_->state();
                if (state == GroupCommitWriterState::kRunning) {
                    std::string pause_error;
                    if (!group_commit_writer_->request_pause(
                            [this](bool success,
                                   const std::string& barrier_error) {
                                on_checkpoint_final_paused(success,
                                                           barrier_error);
                            },
                            &pause_error)) {
                        mark_failed();
                        finish_now = true;
                        finish_error = pause_error;
                    }
                } else if (state == GroupCommitWriterState::kPaused &&
                           pending_mutations_.empty()) {
                    finalize_immediately = true;
                } else {
                    mark_failed();
                    finish_now = true;
                    finish_error = "group commit writer is not available for "
                                   "checkpoint finalization";
                }
            }
        }
    }
    if (finalize_immediately) {
        on_checkpoint_final_paused(true, {});
        return;
    }
    if (finish_now) {
        finish_checkpoint(result, finish_error);
    }
}

void PersistenceManager::on_checkpoint_final_paused(bool success,
                                                    const std::string& error) {
    bool finish_now = false;
    bool wait_for_cleanup = false;
    RdbSaveResult result = RdbSaveResult::Failed;
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
                error.empty() ? "checkpoint final pause barrier failed" : error;
        } else if (!pending_mutations_.empty()) {
            mark_failed();
            finish_now = true;
            finish_error =
                "checkpoint final pause completed before all mutations applied";
        } else {
            checkpoint_operation_->phase = CheckpointPhase::kPublish;
            const GroupCommitWalSnapshot wal_snapshot =
                group_commit_writer_->wal_snapshot();
            std::vector<std::uint64_t> retained;
            const std::uint64_t first_required =
                checkpoint_operation_->checkpoint_lsn + 1;
            const auto first = std::lower_bound(
                wal_snapshot.live_segment_starts.begin(),
                wal_snapshot.live_segment_starts.end(), first_required);
            if (first == wal_snapshot.live_segment_starts.end() ||
                *first != first_required) {
                mark_failed();
                finish_now = true;
                finish_error = "retained WAL does not begin immediately after "
                               "checkpoint LSN";
            } else {
                retained.assign(first, wal_snapshot.live_segment_starts.end());
                for (std::size_t index = 1; index < retained.size(); ++index) {
                    if (retained[index] <= retained[index - 1]) {
                        mark_failed();
                        finish_now = true;
                        finish_error =
                            "retained WAL segment list is not strictly ordered";
                        break;
                    }
                }
                if (!finish_now) {
                    checkpoint_operation_->first_retained_start_lsn =
                        retained.front();
                    CheckpointManifest manifest;
                    manifest.checkpoint_lsn =
                        checkpoint_operation_->checkpoint_lsn;
                    manifest.retained_segment_starts = retained;
                    std::string manifest_error;
                    const std::uint64_t manifest_started_us = steady_now_us();
                    const ManifestWriteResult manifest_result =
                        write_manifest_atomic(manifest_path_, manifest,
                                              wal_operations_.get(),
                                              &manifest_error);
                    const std::uint64_t manifest_finished_us = steady_now_us();
                    checkpoint_operation_->manifest_us =
                        manifest_finished_us >= manifest_started_us
                            ? manifest_finished_us - manifest_started_us
                            : 0;
                    if (manifest_result != ManifestWriteResult::kOk) {
                        finish_now = true;
                        finish_error =
                            manifest_error.empty()
                                ? "failed to publish checkpoint manifest"
                                : manifest_error;
                        if (manifest_result ==
                            ManifestWriteResult::kDurabilityUnconfirmed) {
                            mark_failed();
                            result = RdbSaveResult::DurabilityUnconfirmed;
                        } else {
                            result = RdbSaveResult::Failed;
                            std::string resume_error;
                            if (!group_commit_writer_->resume(&resume_error)) {
                                mark_failed();
                                finish_error +=
                                    "; group commit could not resume: " +
                                    resume_error;
                            }
                        }
                    } else {
                        checkpoint_operation_->phase =
                            CheckpointPhase::kCleanup;
                        stats_.background_checkpoint_state =
                            BackgroundCheckpointState::kCleanup;
                        std::string cleanup_schedule_error;
                        checkpoint_operation_->cleanup_started_us =
                            steady_now_us();
                        checkpoint_operation_->cleanup_started = true;
                        if (group_commit_writer_->request_cleanup(
                                retained.front(),
                                [this](bool cleanup_success,
                                       const std::string& cleanup_error) {
                                    on_checkpoint_cleanup(cleanup_success,
                                                          cleanup_error);
                                },
                                &cleanup_schedule_error)) {
                            wait_for_cleanup = true;
                        } else {
                            // The manifest is already durable, so the
                            // checkpoint itself succeeded.  Keep the writer
                            // paused for the same retry contract as a cleanup
                            // I/O failure.
                            checkpoint_operation_->cleanup_started = false;
                            checkpoint_operation_->cleanup_us = 0;
                            result = RdbSaveResult::Ok;
                            finish_now = true;
                            finish_error.clear();
                        }
                    }
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
                                               const std::string& error) {
    bool finish_now = true;
    RdbSaveResult result = RdbSaveResult::Ok;
    std::string finish_error;
    {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        if (!checkpoint_operation_) {
            return;
        }
        if (checkpoint_operation_->cleanup_started) {
            const std::uint64_t now = steady_now_us();
            checkpoint_operation_->cleanup_us =
                now >= checkpoint_operation_->cleanup_started_us
                    ? now - checkpoint_operation_->cleanup_started_us
                    : 0;
            checkpoint_operation_->cleanup_started = false;
        }
        if (success) {
            if (!checkpoint_operation_->shutdown) {
                std::string resume_error;
                if (!group_commit_writer_->resume(&resume_error)) {
                    mark_failed();
                    result = RdbSaveResult::Failed;
                    finish_error = "cleanup completed but group commit could "
                                   "not resume: " +
                                   resume_error;
                }
            }
        } else {
            // Manifest publication already made this checkpoint durable.
            // Residue is harmless; leave the writer paused so a later SAVE
            // retries prefix cleanup without reusing an unsafe boundary.
            Log::Error("checkpoint WAL cleanup failed: " +
                       (error.empty() ? std::string("unknown error") : error));
            finish_error.clear();
        }
    }
    if (finish_now) {
        finish_checkpoint(result, finish_error);
    }
}

void PersistenceManager::join_checkpoint_worker() {
    if (checkpoint_worker_.joinable()) {
        checkpoint_worker_.join();
    }
}

void PersistenceManager::dispatch_owner(std::function<void()> work) {
    if (!work || !owner_dispatch_ || !owner_callback_state_) {
        return;
    }
    auto callback_state = owner_callback_state_;
    owner_dispatch_([callback_state, work = std::move(work)]() mutable {
        callback_state->run(std::move(work));
    });
}

void PersistenceManager::finish_checkpoint(RdbSaveResult result,
                                           const std::string& error) {
    join_checkpoint_worker();
    CheckpointCompletion completion;
    bool shutdown = false;
    std::uint64_t admission_blocked_us = 0;
    std::uint64_t capture_us = 0;
    std::uint64_t serialize_us = 0;
    std::uint64_t write_us = 0;
    std::uint64_t fsync_us = 0;
    std::uint64_t manifest_us = 0;
    std::uint64_t cleanup_us = 0;
    bool measure_save_latency = false;
    {
        std::lock_guard<std::mutex> lock(operation_mutex_);
        if (!checkpoint_operation_) {
            return;
        }
        const std::uint64_t now = steady_now_us();
        if (checkpoint_operation_->admission_blocked_started_us != 0) {
            const std::uint64_t started =
                checkpoint_operation_->admission_blocked_started_us;
            if (now >= started) {
                admission_blocked_us = now - started;
                observe_timing(admission_blocked_us,
                               &stats_.checkpoint_admission_blocked_total_us,
                               &stats_.checkpoint_admission_blocked_max_us);
                add_saturated(&stats_.checkpoint_pause_us,
                              admission_blocked_us);
            }
            checkpoint_operation_->admission_blocked_started_us = 0;
        }
        capture_us = checkpoint_operation_->capture_us;
        serialize_us = checkpoint_operation_->serialize_us;
        write_us = checkpoint_operation_->write_us;
        fsync_us = checkpoint_operation_->fsync_us;
        manifest_us = checkpoint_operation_->manifest_us;
        cleanup_us = checkpoint_operation_->cleanup_us;
        completion = std::move(checkpoint_operation_->completion);
        shutdown = checkpoint_operation_->shutdown;
        measure_save_latency = !shutdown;
        if (result == RdbSaveResult::Ok) {
            ++stats_.checkpoints;
            observe_timing(capture_us,
                           &stats_.checkpoint_snapshot_capture_total_us,
                           &stats_.checkpoint_snapshot_capture_max_us);
            observe_timing(serialize_us,
                           &stats_.checkpoint_snapshot_serialize_total_us,
                           &stats_.checkpoint_snapshot_serialize_max_us);
            observe_timing(write_us, &stats_.checkpoint_snapshot_write_total_us,
                           &stats_.checkpoint_snapshot_write_max_us);
            observe_timing(fsync_us, &stats_.checkpoint_snapshot_fsync_total_us,
                           &stats_.checkpoint_snapshot_fsync_max_us);
            add_saturated(&stats_.checkpoint_manifest_total_us, manifest_us);
            add_saturated(&stats_.checkpoint_wal_cleanup_total_us, cleanup_us);
            if (now >= checkpoint_operation_->started_us) {
                observe_timing(now - checkpoint_operation_->started_us,
                               &stats_.checkpoint_total_us,
                               &stats_.checkpoint_max_us);
                if (measure_save_latency) {
                    observe_timing(now - checkpoint_operation_->started_us,
                                   &stats_.save_client_latency_total_us,
                                   &stats_.save_client_latency_max_us);
                }
            }
        } else {
            ++stats_.checkpoint_failures;
        }
        checkpoint_operation_.reset();
        control_pending_ = false;
        stats_.background_checkpoint_state =
            failed_ ? BackgroundCheckpointState::kFailed
                    : BackgroundCheckpointState::kIdle;
        if (!shutdown && !failed_ && group_commit_writer_ &&
            group_commit_writer_->state() == GroupCommitWriterState::kRunning) {
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
        stats_.background_checkpoint_state =
            failed_ ? BackgroundCheckpointState::kFailed
                    : BackgroundCheckpointState::kIdle;
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

const char*
background_checkpoint_state_name(BackgroundCheckpointState state) noexcept {
    switch (state) {
    case BackgroundCheckpointState::kIdle:
        return "idle";
    case BackgroundCheckpointState::kCapture:
        return "capture";
    case BackgroundCheckpointState::kWriting:
        return "writing";
    case BackgroundCheckpointState::kFinalizing:
        return "finalizing";
    case BackgroundCheckpointState::kCleanup:
        return "cleanup";
    case BackgroundCheckpointState::kFailed:
        return "failed";
    }
    return "unknown";
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
