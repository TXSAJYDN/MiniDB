#pragma once

#include "DurabilityPolicy.h"
#include "GroupCommitWriter.h"
#include "KvStore.h"
#include "RdbManager.h"
#include "RecoveryManager.h"
#include "SingleWriterLock.h"
#include "WalFileOperations.h"
#include "WalManager.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace minidb {

namespace detail {
struct PersistenceManagerCallbackState;
}

enum class MutationStatus : std::uint8_t {
    kOk,
    kInvalidArgument,
    kIoError,
    kResourceLimit,
    kNotReady,
    kNoTransaction,
    kConflict,
};

struct MutationOutcome {
    MutationStatus status = MutationStatus::kOk;
    bool changed = false;
    // Assigned for an accepted persistence write.  A read-only transaction
    // commit deliberately reports zero.
    std::uint64_t lsn = 0;
    std::string error;

    bool ok() const noexcept {
        return status == MutationStatus::kOk;
    }
};

// Async admission is deliberately distinct from mutation completion.  An
// accepted request has not been applied or acknowledged yet; its completion
// runs later on the owner loop.
enum class MutationAdmissionStatus : std::uint8_t {
    kAccepted,
    kInvalidArgument,
    kIoError,
    kResourceLimit,
    kNotReady,
    kShuttingDown,
    kNoTransaction,
    kConflict,
};

struct MutationAdmission {
    MutationAdmissionStatus status = MutationAdmissionStatus::kNotReady;
    std::string error;

    bool accepted() const noexcept {
        return status == MutationAdmissionStatus::kAccepted;
    }
};

using MutationCompletion = std::function<void(MutationOutcome)>;
using CheckpointCompletion =
    std::function<void(RdbSaveResult, const std::string&)>;

enum class BackgroundCheckpointState : std::uint8_t {
    kIdle,
    kCapture,
    kWriting,
    kFinalizing,
    kCleanup,
    kFailed,
};

const char*
background_checkpoint_state_name(BackgroundCheckpointState state) noexcept;

// Transaction counters and resource observations.  Every field is copied under
// operation_mutex_ into PersistenceSnapshot so diagnostics cannot combine
// values from unrelated admissions, applies, or GC passes.
struct TransactionStats {
    std::uint64_t active = 0;
    std::uint64_t started = 0;
    std::uint64_t committed = 0;
    std::uint64_t conflicted = 0;
    std::uint64_t rolled_back = 0;
    std::uint64_t failed = 0;
    std::uint64_t write_conflicts = 0;
    std::uint64_t max_active = 0;
    std::uint64_t versions_current = 0;
    std::uint64_t versions_max = 0;
    std::uint64_t versions_reclaimed = 0;
    std::uint64_t oldest_snapshot_lag_lsn = 0;
    std::uint64_t max_oldest_snapshot_lag_lsn = 0;
    std::uint64_t commit_records = 0;
    std::uint64_t commit_bytes = 0;
    std::uint64_t commit_total_us = 0;
    std::uint64_t commit_max_us = 0;
    std::uint64_t gc_total_us = 0;
    std::uint64_t gc_max_us = 0;
};

struct PersistenceStats {
    std::uint64_t mutations = 0;
    std::uint64_t sync_mutations = 0;
    std::uint64_t interval_mutations = 0;
    std::uint64_t checkpoints = 0;
    std::uint64_t interval_flushes = 0;
    std::uint64_t checkpoint_pause_us = 0;

    // Successful SYNC mutations only.  The interval starts after mutation
    // validation and spans bootstrap, WAL append/fsync, and the in-memory
    // apply; it excludes network parsing and response serialization.
    std::uint64_t sync_mutation_total_us = 0;
    std::uint64_t sync_mutation_max_us = 0;

    // Background checkpoint timings.  Admission-blocked covers only the two
    // short cut points (pause/drain/rotate and final drain), never the RDB
    // serialization or durable write.
    std::uint64_t checkpoint_admission_blocked_total_us = 0;
    std::uint64_t checkpoint_admission_blocked_max_us = 0;
    std::uint64_t checkpoint_snapshot_capture_total_us = 0;
    std::uint64_t checkpoint_snapshot_capture_max_us = 0;
    std::uint64_t checkpoint_snapshot_serialize_total_us = 0;
    std::uint64_t checkpoint_snapshot_serialize_max_us = 0;
    std::uint64_t checkpoint_snapshot_write_total_us = 0;
    std::uint64_t checkpoint_snapshot_write_max_us = 0;
    std::uint64_t checkpoint_snapshot_fsync_total_us = 0;
    std::uint64_t checkpoint_snapshot_fsync_max_us = 0;
    std::uint64_t checkpoint_manifest_total_us = 0;
    std::uint64_t checkpoint_wal_cleanup_total_us = 0;
    std::uint64_t checkpoint_total_us = 0;
    std::uint64_t checkpoint_max_us = 0;
    std::uint64_t checkpoint_failures = 0;
    std::uint64_t save_client_latency_total_us = 0;
    std::uint64_t save_client_latency_max_us = 0;
    BackgroundCheckpointState background_checkpoint_state =
        BackgroundCheckpointState::kIdle;
};

// A single-lock snapshot for diagnostic endpoints. Reading these values via
// separate accessors can observe unrelated intermediate states (for example,
// next_lsn_ already advanced while the corresponding WAL bytes counter has
// not). The endpoint layer uses this aggregate so every reported field belongs
// to one serialized point in the mutation stream.
struct PersistenceSnapshot {
    DurabilityMode durability_mode = DurabilityMode::kSync;
    bool opened = false;
    bool failed = false;
    std::uint64_t last_lsn = 0;
    std::uint64_t next_lsn = 1;
    std::uint64_t wal_bytes = 0;
    std::size_t wal_segment_count = 0;
    RecoveryStats recovery;
    WalManagerStats wal;
    PersistenceStats persistence;
    TransactionStats transactions;
    GroupCommitStats group_commit;
};

// Owns the single-writer lock and all persistence side effects after startup
// recovery has succeeded.  Network code sees only mutation outcomes; WAL byte
// layout and LSN assignment stay inside the storage layer.
class PersistenceManager {
  public:
    PersistenceManager(
        std::shared_ptr<KvStore> store, std::string rdb_path,
        DurabilityMode mode = DurabilityMode::kSync,
        std::shared_ptr<WalFileOperations> wal_operations =
            default_wal_file_operations(),
        std::shared_ptr<RdbFileOperations> rdb_operations = nullptr,
        WalLimits limits = default_wal_limits());
    ~PersistenceManager();

    PersistenceManager(const PersistenceManager&) = delete;
    PersistenceManager& operator=(const PersistenceManager&) = delete;

    // Acquires the single-writer lock and recovers a complete image before
    // exposing the manager.  A fresh database is not bootstrapped here; its
    // first mutation or checkpoint publishes the initial RDB/manifest/WAL.
    bool open(std::string* error = nullptr);

    // Synchronous mutation APIs are retained for tests and for the period
    // before group commit starts.  Once start_group_commit() succeeds they
    // return kNotReady and must not bypass the writer thread.
    MutationOutcome set(const std::string& key, const std::string& value);
    MutationOutcome del(const std::string& key);
    MutationOutcome expire(const std::string& key, std::int64_t ttl_ms);

    struct TransactionBeginResult {
        MutationStatus status = MutationStatus::kNotReady;
        std::uint64_t txn_id = 0;
        std::uint64_t snapshot_lsn = 0;
        std::string error;

        bool ok() const noexcept {
            return status == MutationStatus::kOk;
        }
    };

    struct TransactionResult {
        MutationStatus status = MutationStatus::kOk;
        bool changed = false;
        bool found = false;
        std::string value;
        KvStore::TtlResult ttl;
        std::string error;

        bool ok() const noexcept {
            return status == MutationStatus::kOk;
        }
    };

    // Explicit transactions use the same owner-loop admission discipline as
    // standalone writes.  Read helpers are synchronous because they only touch
    // the immutable snapshot plus the private write set.
    TransactionBeginResult begin_transaction();
    TransactionResult transaction_set(std::uint64_t txn_id,
                                      const std::string& key,
                                      const std::string& value);
    TransactionResult transaction_get(std::uint64_t txn_id,
                                      const std::string& key);
    TransactionResult transaction_del(std::uint64_t txn_id,
                                      const std::string& key);
    TransactionResult transaction_expire(std::uint64_t txn_id,
                                         const std::string& key,
                                         std::int64_t ttl_ms);
    TransactionResult transaction_ttl(std::uint64_t txn_id,
                                      const std::string& key);
    MutationAdmission commit_transaction_async(std::uint64_t txn_id,
                                               MutationCompletion completion);
    TransactionResult rollback_transaction(std::uint64_t txn_id);

    // Bootstraps a writable WAL (if needed), then starts the dedicated writer
    // thread.  The dispatch must enqueue work on the owner EventLoop and must
    // never run it inline on the writer thread.
    bool start_group_commit(GroupCommitWriter::OwnerDispatch owner_dispatch,
                            std::string* error = nullptr);
    bool group_commit_started() const;

    MutationAdmission set_async(const std::string& key, std::string value,
                                MutationCompletion completion);
    MutationAdmission del_async(std::string key, MutationCompletion completion);
    MutationAdmission expire_async(std::string key, std::int64_t ttl_ms,
                                   MutationCompletion completion);

    // INTERVAL timer hooks.  The synchronous form remains available before
    // group commit starts and returns false afterwards; the asynchronous form
    // is serialized through the writer barrier.
    bool flush_interval(std::string* error = nullptr);
    bool flush_interval_async(GroupCommitWriter::BarrierCompletion completion,
                              std::string* error = nullptr);

    // SAVE and shutdown use the same non-blocking checkpoint state machine:
    // pause admission and the writer, drain/apply, rotate, snapshot, publish
    // the manifest, cleanup, then resume or stop the writer.  Completion runs
    // on the owner loop.  A SAVE response must not be sent before it fires.
    RdbSaveResult checkpoint(std::string* error = nullptr);
    bool checkpoint_async(CheckpointCompletion completion,
                          std::string* error = nullptr);
    bool shutdown_async(CheckpointCompletion completion,
                        std::string* error = nullptr);
    bool shutdown_complete() const;

    DurabilityMode durability_mode() const noexcept {
        return durability_mode_;
    }
    bool opened() const noexcept {
        return opened_;
    }
    bool failed() const noexcept {
        return failed_;
    }
    std::uint64_t next_lsn() const;
    std::uint64_t wal_bytes() const;
    WalManagerStats wal_stats() const;
    GroupCommitStats group_commit_stats() const;
    const RecoveryStats& recovery_stats() const noexcept {
        return recovery_stats_;
    }
    PersistenceStats stats() const;
    PersistenceSnapshot snapshot() const;

    // Deterministic crash-test seam.  Invoked while operation_mutex_ is held,
    // after the memory mutation and before the mutation returns to the caller.
    // Production leaves this empty; it must not be used to change semantics.
    void
    set_after_apply_callback(std::function<void(const WalRecord&)> callback) {
        after_apply_callback_ = std::move(callback);
    }

  private:
    enum class PendingMutationKind : std::uint8_t {
        kStandalone,
        kTransaction,
    };

    struct PendingMutation {
        std::uint64_t lsn = 0;
        std::uint64_t started_us = 0;
        MutationCompletion completion;
        PendingMutationKind kind = PendingMutationKind::kStandalone;
        std::uint64_t txn_id = 0;
        std::vector<std::string> keys;
    };

    enum class TransactionStateKind : std::uint8_t {
        kActive,
        kCommitting,
    };

    struct PendingWrite {
        bool deleted = false;
        std::string value;
        std::int64_t absolute_expiry_ms = 0; // 0 means no expiry
    };

    struct TransactionState {
        std::uint64_t txn_id = 0;
        std::uint64_t snapshot_lsn = 0;
        std::uint64_t started_us = 0;
        TransactionStateKind state = TransactionStateKind::kActive;
        std::unordered_map<std::string, PendingWrite> writes;
        std::size_t write_bytes = 0;
        std::size_t reserved_versions = 0;
    };

    enum class CheckpointPhase : std::uint8_t {
        kPause,
        kRotate,
        kWriting,
        kFinalPause,
        kPublish,
        kCleanup,
    };

    struct CheckpointOperation {
        CheckpointCompletion completion;
        bool shutdown = false;
        CheckpointPhase phase = CheckpointPhase::kPause;
        std::uint64_t started_us = 0;
        std::uint64_t admission_blocked_started_us = 0;
        std::uint64_t checkpoint_lsn = 0;
        std::uint64_t first_retained_start_lsn = 0;
        std::uint64_t capture_us = 0;
        std::uint64_t serialize_us = 0;
        std::uint64_t write_us = 0;
        std::uint64_t fsync_us = 0;
        std::uint64_t manifest_us = 0;
        std::uint64_t cleanup_started_us = 0;
        std::uint64_t cleanup_us = 0;
        bool cleanup_started = false;
    };

    struct CheckpointJob {
        RdbSnapshot snapshot;
    };

    MutationOutcome set_impl(const std::string& key, const std::string& value);
    MutationOutcome del_impl(const std::string& key);
    MutationOutcome expire_impl(const std::string& key, std::int64_t ttl_ms);
    MutationOutcome append_and_apply(const WalRecord& record);
    MutationAdmission
    admit_async(WalRecord record, MutationCompletion completion,
                PendingMutationKind kind = PendingMutationKind::kStandalone,
                std::uint64_t txn_id = 0);
    MutationAdmission admission_fail(MutationAdmissionStatus status,
                                     const std::string& error) const;
    MutationOutcome fail(MutationStatus status, const std::string& error);
    TransactionResult transaction_fail(MutationStatus status,
                                       const std::string& error) const;
    TransactionState* find_transaction_locked(std::uint64_t txn_id);
    TransactionResult abort_transaction_locked(std::uint64_t txn_id,
                                               MutationStatus status,
                                               const std::string& error);
    void finish_transaction_locked(std::uint64_t txn_id);
    bool garbage_collect_locked(std::string* error);
    std::vector<std::uint64_t> active_snapshots_locked() const;
    void observe_transaction_metrics_locked();
    void add_pending_key_lsn_locked(const std::string& key, std::uint64_t lsn);
    void remove_pending_keys_locked(const std::vector<std::string>& keys,
                                    std::uint64_t lsn);
    std::uint64_t pending_key_lsn_locked(const std::string& key) const;

    bool ensure_bootstrapped_locked(std::string* error);
    bool ensure_group_commit_writer_locked(std::string* error);
    bool start_checkpoint_locked(CheckpointCompletion completion, bool shutdown,
                                 std::string* error);
    void on_checkpoint_paused(bool success, const std::string& error);
    void on_checkpoint_rotated(const GroupCommitWalBarrierInfo& info);
    void on_checkpoint_written(RdbSaveResult result, const std::string& error,
                               std::uint64_t serialize_us,
                               std::uint64_t write_us, std::uint64_t fsync_us);
    void on_checkpoint_final_paused(bool success, const std::string& error);
    void on_checkpoint_cleanup(bool success, const std::string& error);
    void finish_checkpoint(RdbSaveResult result, const std::string& error);
    void join_checkpoint_worker();
    void dispatch_owner(std::function<void()> work);
    void pause_checkpoint_admission_locked(std::uint64_t now_us);
    void resume_checkpoint_admission_locked(std::uint64_t now_us);
    void on_group_commit_completion(GroupCommitCompletion completion);
    WalResult append_with_checkpoint_retry(const WalRecord& record,
                                           std::string* error);
    RdbSaveResult checkpoint_locked(std::string* error);
    void mark_failed();

    std::shared_ptr<KvStore> store_;
    std::string rdb_path_;
    std::string manifest_path_;
    std::string wal_dir_;
    DurabilityMode durability_mode_;
    std::shared_ptr<WalFileOperations> wal_operations_;
    std::shared_ptr<RdbFileOperations> rdb_operations_;
    WalLimits limits_;
    std::unique_ptr<RdbManager> rdb_manager_;
    std::unique_ptr<SingleWriterLock> writer_lock_;
    RecoveryStats recovery_stats_;
    std::unique_ptr<WalManager> wal_;
    std::shared_ptr<GroupCommitWriter> group_commit_writer_;
    GroupCommitWriter::OwnerDispatch owner_dispatch_;
    std::shared_ptr<detail::PersistenceManagerCallbackState>
        owner_callback_state_;
    mutable std::mutex operation_mutex_;
    std::uint64_t last_lsn_ = 0;
    bool opened_ = false;
    bool failed_ = false;
    bool group_commit_started_ = false;
    bool accepting_mutations_ = true;
    bool control_pending_ = false;
    bool shutdown_requested_ = false;
    bool shutdown_complete_ = false;
    std::deque<PendingMutation> pending_mutations_;
    std::unordered_map<std::uint64_t, TransactionState> transactions_;
    std::unordered_map<std::string, std::multiset<std::uint64_t>>
        pending_key_lsns_;
    std::uint64_t next_txn_id_ = 1;
    std::size_t reserved_versions_ = 0;
    std::unique_ptr<CheckpointOperation> checkpoint_operation_;
    PersistenceStats stats_;
    TransactionStats transaction_stats_;
    std::function<void(const WalRecord&)> after_apply_callback_;
    std::thread checkpoint_worker_;
};

const char* mutation_status_name(MutationStatus status);

} // namespace minidb
