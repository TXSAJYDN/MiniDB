#include "PersistenceManager.h"

#include "../base/Log.h"
#include "CheckpointManifest.h"
#include "WalFormat.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <filesystem>
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

const char* mutation_io_error = "persistence I/O failure";
const char* group_commit_active_error =
    "group commit is active; use the asynchronous persistence API";
const char* reserved_agent_key_error = "key is in the reserved agent namespace";

MutationAdmissionStatus
admission_status_from_submit(GroupCommitSubmitStatus status,
                             GroupCommitWriterState writer_state) noexcept {
    switch (status) {
    case GroupCommitSubmitStatus::kQueueFull:
    case GroupCommitSubmitStatus::kByteOverflow:
        return MutationAdmissionStatus::kResourceLimit;
    case GroupCommitSubmitStatus::kNotAccepting:
        if (writer_state == GroupCommitWriterState::kFailed) {
            return MutationAdmissionStatus::kIoError;
        }
        if (writer_state == GroupCommitWriterState::kStopped ||
            writer_state == GroupCommitWriterState::kDraining) {
            return MutationAdmissionStatus::kShuttingDown;
        }
        return MutationAdmissionStatus::kNotReady;
    case GroupCommitSubmitStatus::kAccepted:
    case GroupCommitSubmitStatus::kInvalidLsn:
    case GroupCommitSubmitStatus::kLsnExhausted:
    case GroupCommitSubmitStatus::kInvalidRecord:
        break;
    }
    return MutationAdmissionStatus::kNotReady;
}

bool is_reserved_agent_key(const std::string& key) noexcept {
    return AgentRecordCodec::is_reserved_key(key);
}

AgentRequestStatus
agent_status_from_mutation_status(MutationStatus status) noexcept {
    switch (status) {
    case MutationStatus::kOk:
        return AgentRequestStatus::kCompleted;
    case MutationStatus::kInvalidArgument:
        return AgentRequestStatus::kInvalidArgument;
    case MutationStatus::kIoError:
        return AgentRequestStatus::kIoError;
    case MutationStatus::kResourceLimit:
        return AgentRequestStatus::kResourceLimit;
    case MutationStatus::kNotReady:
        return AgentRequestStatus::kNotReady;
    case MutationStatus::kNoTransaction:
        return AgentRequestStatus::kInvalidArgument;
    case MutationStatus::kConflict:
        return AgentRequestStatus::kConflict;
    }
    return AgentRequestStatus::kIoError;
}

AgentRequestStatus
agent_status_from_admission_status(MutationAdmissionStatus status) noexcept {
    switch (status) {
    case MutationAdmissionStatus::kAccepted:
        return AgentRequestStatus::kAccepted;
    case MutationAdmissionStatus::kInvalidArgument:
        return AgentRequestStatus::kInvalidArgument;
    case MutationAdmissionStatus::kIoError:
        return AgentRequestStatus::kIoError;
    case MutationAdmissionStatus::kResourceLimit:
        return AgentRequestStatus::kResourceLimit;
    case MutationAdmissionStatus::kNotReady:
        return AgentRequestStatus::kNotReady;
    case MutationAdmissionStatus::kShuttingDown:
        return AgentRequestStatus::kShuttingDown;
    case MutationAdmissionStatus::kNoTransaction:
        return AgentRequestStatus::kInvalidArgument;
    case MutationAdmissionStatus::kConflict:
        return AgentRequestStatus::kConflict;
    }
    return AgentRequestStatus::kIoError;
}
constexpr std::size_t kMaxActiveTransactions = 64;
constexpr std::size_t kMaxTransactionWriteKeys = WalFormat::MAX_TXN_OPS;

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

bool decode_record_mutations(const WalRecord& record,
                             std::vector<KvStore::AppliedMutation>* mutations,
                             std::string* error) {
    mutations->clear();
    const auto append_operation = [&](WalOp op, const std::string& key,
                                      const std::string& value,
                                      std::int64_t expiry_ms) {
        KvStore::AppliedMutation mutation;
        mutation.key = key;
        mutation.value = value;
        mutation.absolute_expiry_ms = expiry_ms;
        switch (op) {
        case WalOp::kSet:
            mutation.kind = KvStore::AppliedMutation::Kind::kSet;
            break;
        case WalOp::kDel:
            mutation.kind = KvStore::AppliedMutation::Kind::kDelete;
            break;
        case WalOp::kExpire:
            mutation.kind = KvStore::AppliedMutation::Kind::kExpire;
            break;
        case WalOp::kTxn:
            if (error != nullptr) {
                *error = "nested WAL transaction is not allowed";
            }
            return false;
        }
        mutations->push_back(std::move(mutation));
        return true;
    };

    if (record.op != WalOp::kTxn) {
        return append_operation(record.op, record.key, record.value,
                                record.expiry_ms);
    }

    std::vector<WalTxnOp> operations;
    std::string payload_error;
    if (WalFormat::decode_transaction(
            reinterpret_cast<const std::uint8_t*>(record.value.data()),
            record.value.size(), &operations,
            &payload_error) != WalFormat::DecodeStatus::kOk) {
        if (error != nullptr) {
            *error = payload_error.empty()
                         ? "WAL transaction payload is invalid"
                         : payload_error;
        }
        return false;
    }
    mutations->reserve(operations.size());
    for (const WalTxnOp& operation : operations) {
        if (!append_operation(operation.op, operation.key, operation.value,
                              operation.expiry_ms)) {
            return false;
        }
    }
    return true;
}

bool mutation_changed(KvStore& store,
                      const KvStore::AppliedMutation& mutation) {
    if (mutation.kind == KvStore::AppliedMutation::Kind::kSet) {
        return true;
    }
    return store.get(mutation.key).has_value();
}

} // namespace

PersistenceManager::PersistenceManager(
    std::shared_ptr<KvStore> store, std::string rdb_path, DurabilityMode mode,
    std::shared_ptr<WalFileOperations> wal_operations,
    std::shared_ptr<RdbFileOperations> rdb_operations, WalLimits limits)
    : store_(std::move(store)),
      agent_store_(store_ ? store_->clock() : nullptr),
      rdb_path_(std::move(rdb_path)),
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

    std::string agent_rebuild_error;
    if (!agent_store_.rebuild(store_->export_all(), &agent_rebuild_error)) {
        if (error != nullptr) {
            *error =
                "startup agent state validation failed: " + agent_rebuild_error;
        }
        failed_ = true;
        return false;
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
    if (is_reserved_agent_key(key)) {
        return fail(MutationStatus::kInvalidArgument, reserved_agent_key_error);
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
    if (is_reserved_agent_key(key)) {
        return fail(MutationStatus::kInvalidArgument, reserved_agent_key_error);
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
    if (is_reserved_agent_key(key)) {
        return fail(MutationStatus::kInvalidArgument, reserved_agent_key_error);
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
    return append_and_apply(record);
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
    return append_and_apply(record);
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
    return append_and_apply(record);
}

PersistenceManager::TransactionBeginResult
PersistenceManager::begin_transaction() {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    TransactionBeginResult result;
    if (!opened_ || failed_) {
        result.status =
            failed_ ? MutationStatus::kIoError : MutationStatus::kNotReady;
        result.error = failed_ ? mutation_io_error : "persistence is not open";
        return result;
    }
    if (!group_commit_started_) {
        result.status = MutationStatus::kNotReady;
        result.error = "group commit writer is not started";
        return result;
    }
    if (shutdown_requested_ || shutdown_complete_ || !accepting_mutations_) {
        result.status = MutationStatus::kNotReady;
        result.error = "persistence is not accepting transactions";
        return result;
    }
    if (transactions_.size() >= kMaxActiveTransactions) {
        result.status = MutationStatus::kResourceLimit;
        result.error = "active transaction limit exceeded";
        return result;
    }
    if (next_txn_id_ == std::numeric_limits<std::uint64_t>::max()) {
        result.status = MutationStatus::kResourceLimit;
        result.error = "transaction id space is exhausted";
        return result;
    }

    TransactionState transaction;
    transaction.txn_id = next_txn_id_++;
    transaction.snapshot_lsn = last_lsn_;
    transaction.started_us = steady_now_us();
    transaction.write_bytes = WalFormat::TXN_HEADER_SIZE;
    result.txn_id = transaction.txn_id;
    result.snapshot_lsn = transaction.snapshot_lsn;
    result.status = MutationStatus::kOk;
    transactions_.emplace(result.txn_id, std::move(transaction));
    ++transaction_stats_.started;
    transaction_stats_.active = transactions_.size();
    transaction_stats_.max_active = std::max<std::uint64_t>(
        transaction_stats_.max_active, transaction_stats_.active);
    observe_transaction_metrics_locked();
    return result;
}

PersistenceManager::TransactionState*
PersistenceManager::find_transaction_locked(std::uint64_t txn_id) {
    const auto it = transactions_.find(txn_id);
    return it == transactions_.end() ? nullptr : &it->second;
}

PersistenceManager::TransactionResult
PersistenceManager::transaction_fail(MutationStatus status,
                                     const std::string& error) const {
    TransactionResult result;
    result.status = status;
    result.error = error;
    return result;
}

PersistenceManager::TransactionResult
PersistenceManager::abort_transaction_locked(std::uint64_t txn_id,
                                             MutationStatus status,
                                             const std::string& error) {
    TransactionState* transaction = find_transaction_locked(txn_id);
    if (transaction == nullptr) {
        return transaction_fail(status, error);
    }
    if (transaction->reserved_versions <= reserved_versions_) {
        reserved_versions_ -= transaction->reserved_versions;
    } else {
        reserved_versions_ = 0;
    }
    transactions_.erase(txn_id);
    ++transaction_stats_.failed;
    observe_transaction_metrics_locked();
    return transaction_fail(status, error);
}

void PersistenceManager::finish_transaction_locked(std::uint64_t txn_id) {
    const auto it = transactions_.find(txn_id);
    if (it == transactions_.end()) {
        return;
    }
    if (it->second.reserved_versions <= reserved_versions_) {
        reserved_versions_ -= it->second.reserved_versions;
    } else {
        reserved_versions_ = 0;
    }
    transactions_.erase(it);
    observe_transaction_metrics_locked();
}

void PersistenceManager::add_pending_key_lsn_locked(const std::string& key,
                                                    std::uint64_t lsn) {
    pending_key_lsns_[key].insert(lsn);
}

void PersistenceManager::remove_pending_keys_locked(
    const std::vector<std::string>& keys, std::uint64_t lsn) {
    for (const std::string& key : keys) {
        const auto it = pending_key_lsns_.find(key);
        if (it == pending_key_lsns_.end()) {
            continue;
        }
        const auto range = it->second.equal_range(lsn);
        it->second.erase(range.first, range.second);
        if (it->second.empty()) {
            pending_key_lsns_.erase(it);
        }
    }
}

std::uint64_t
PersistenceManager::pending_key_lsn_locked(const std::string& key) const {
    const auto it = pending_key_lsns_.find(key);
    return it == pending_key_lsns_.end() || it->second.empty()
               ? 0
               : *it->second.rbegin();
}

std::vector<std::uint64_t> PersistenceManager::active_snapshots_locked() const {
    std::vector<std::uint64_t> snapshots;
    snapshots.reserve(transactions_.size());
    for (const auto& [txn_id, transaction] : transactions_) {
        (void)txn_id;
        snapshots.push_back(transaction.snapshot_lsn);
    }
    return snapshots;
}

void PersistenceManager::observe_transaction_metrics_locked() {
    transaction_stats_.active = transactions_.size();
    if (transaction_stats_.active > transaction_stats_.max_active) {
        transaction_stats_.max_active = transaction_stats_.active;
    }

    const std::uint64_t versions = store_ ? store_->version_count() : 0;
    transaction_stats_.versions_current = versions;
    if (versions > transaction_stats_.versions_max) {
        transaction_stats_.versions_max = versions;
    }

    std::uint64_t oldest_snapshot = 0;
    bool has_snapshot = false;
    for (const auto& [txn_id, transaction] : transactions_) {
        (void)txn_id;
        if (!has_snapshot || transaction.snapshot_lsn < oldest_snapshot) {
            oldest_snapshot = transaction.snapshot_lsn;
            has_snapshot = true;
        }
    }
    transaction_stats_.oldest_snapshot_lag_lsn =
        has_snapshot && last_lsn_ > oldest_snapshot
            ? last_lsn_ - oldest_snapshot
            : 0;
    if (transaction_stats_.oldest_snapshot_lag_lsn >
        transaction_stats_.max_oldest_snapshot_lag_lsn) {
        transaction_stats_.max_oldest_snapshot_lag_lsn =
            transaction_stats_.oldest_snapshot_lag_lsn;
    }
}

bool PersistenceManager::garbage_collect_locked(std::string* error) {
    const std::uint64_t started_us = steady_now_us();
    KvStore::GcResult gc;
    const bool ok =
        store_->garbage_collect(active_snapshots_locked(), &gc, error);
    add_saturated(&transaction_stats_.versions_reclaimed,
                  gc.versions_reclaimed);
    const std::uint64_t finished_us = steady_now_us();
    if (finished_us >= started_us) {
        observe_timing(finished_us - started_us,
                       &transaction_stats_.gc_total_us,
                       &transaction_stats_.gc_max_us);
    }
    observe_transaction_metrics_locked();
    return ok;
}

PersistenceManager::TransactionResult PersistenceManager::transaction_set(
    std::uint64_t txn_id, const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    TransactionState* transaction = find_transaction_locked(txn_id);
    if (transaction == nullptr ||
        transaction->state != TransactionStateKind::kActive) {
        return transaction_fail(MutationStatus::kNoTransaction,
                                "transaction is not active");
    }
    if (key.empty() || key.size() > WalFormat::MAX_KEY_SIZE) {
        return transaction_fail(MutationStatus::kInvalidArgument,
                                "invalid key length");
    }
    if (is_reserved_agent_key(key)) {
        return transaction_fail(MutationStatus::kInvalidArgument,
                                reserved_agent_key_error);
    }
    if (value.size() > WalFormat::MAX_VALUE_SIZE) {
        return transaction_fail(MutationStatus::kResourceLimit,
                                "transaction value exceeds the size limit");
    }

    const auto it = transaction->writes.find(key);
    if (it == transaction->writes.end() &&
        transaction->writes.size() >= kMaxTransactionWriteKeys) {
        return abort_transaction_locked(txn_id, MutationStatus::kResourceLimit,
                                        "transaction write-key limit exceeded");
    }
    const std::size_t old_cost = it == transaction->writes.end()
                                     ? 0
                                     : WalFormat::TXN_OP_HEADER_SIZE +
                                           key.size() + it->second.value.size();
    const std::size_t new_cost =
        WalFormat::TXN_OP_HEADER_SIZE + key.size() + value.size();
    const std::size_t current_cost = transaction->write_bytes >= old_cost
                                         ? transaction->write_bytes - old_cost
                                         : 0;
    if (new_cost > WalFormat::MAX_TXN_PAYLOAD_SIZE ||
        current_cost > WalFormat::MAX_TXN_PAYLOAD_SIZE - new_cost) {
        return abort_transaction_locked(
            txn_id, MutationStatus::kResourceLimit,
            "transaction payload exceeds the size limit");
    }

    PendingWrite write;
    write.value = value;
    transaction->writes[key] = std::move(write);
    transaction->write_bytes = current_cost + new_cost;
    TransactionResult result;
    result.changed = true;
    return result;
}

PersistenceManager::TransactionResult
PersistenceManager::transaction_get(std::uint64_t txn_id,
                                    const std::string& key) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    TransactionState* transaction = find_transaction_locked(txn_id);
    if (transaction == nullptr ||
        transaction->state != TransactionStateKind::kActive) {
        return transaction_fail(MutationStatus::kNoTransaction,
                                "transaction is not active");
    }
    if (key.empty() || key.size() > WalFormat::MAX_KEY_SIZE) {
        return transaction_fail(MutationStatus::kInvalidArgument,
                                "invalid key length");
    }
    if (is_reserved_agent_key(key)) {
        return transaction_fail(MutationStatus::kInvalidArgument,
                                reserved_agent_key_error);
    }

    const auto it = transaction->writes.find(key);
    if (it != transaction->writes.end()) {
        TransactionResult result;
        if (it->second.deleted) {
            return result;
        }
        if (it->second.absolute_expiry_ms > 0 &&
            it->second.absolute_expiry_ms <= store_->wall_clock_now_ms()) {
            return result;
        }
        result.found = true;
        result.value = it->second.value;
        return result;
    }

    const std::optional<std::string> value =
        store_->get_at(key, transaction->snapshot_lsn);
    TransactionResult result;
    if (value.has_value()) {
        result.found = true;
        result.value = value.value();
    }
    return result;
}

PersistenceManager::TransactionResult
PersistenceManager::transaction_del(std::uint64_t txn_id,
                                    const std::string& key) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    TransactionState* transaction = find_transaction_locked(txn_id);
    if (transaction == nullptr ||
        transaction->state != TransactionStateKind::kActive) {
        return transaction_fail(MutationStatus::kNoTransaction,
                                "transaction is not active");
    }
    if (key.empty() || key.size() > WalFormat::MAX_KEY_SIZE) {
        return transaction_fail(MutationStatus::kInvalidArgument,
                                "invalid key length");
    }
    if (is_reserved_agent_key(key)) {
        return transaction_fail(MutationStatus::kInvalidArgument,
                                reserved_agent_key_error);
    }

    const auto it = transaction->writes.find(key);
    bool exists = false;
    if (it != transaction->writes.end()) {
        exists = !it->second.deleted &&
                 (it->second.absolute_expiry_ms == 0 ||
                  it->second.absolute_expiry_ms > store_->wall_clock_now_ms());
        if (!exists) {
            return TransactionResult{};
        }
    } else {
        exists = store_->get_at(key, transaction->snapshot_lsn).has_value();
        if (!exists) {
            return TransactionResult{};
        }
    }
    if (it == transaction->writes.end() &&
        transaction->writes.size() >= kMaxTransactionWriteKeys) {
        return abort_transaction_locked(txn_id, MutationStatus::kResourceLimit,
                                        "transaction write-key limit exceeded");
    }

    const std::size_t old_cost = it == transaction->writes.end()
                                     ? 0
                                     : WalFormat::TXN_OP_HEADER_SIZE +
                                           key.size() + it->second.value.size();
    const std::size_t new_cost = WalFormat::TXN_OP_HEADER_SIZE + key.size();
    const std::size_t current_cost = transaction->write_bytes >= old_cost
                                         ? transaction->write_bytes - old_cost
                                         : 0;
    if (new_cost > WalFormat::MAX_TXN_PAYLOAD_SIZE ||
        current_cost > WalFormat::MAX_TXN_PAYLOAD_SIZE - new_cost) {
        return abort_transaction_locked(
            txn_id, MutationStatus::kResourceLimit,
            "transaction payload exceeds the size limit");
    }

    PendingWrite write;
    write.deleted = true;
    transaction->writes[key] = std::move(write);
    transaction->write_bytes = current_cost + new_cost;
    TransactionResult result;
    result.changed = true;
    return result;
}

PersistenceManager::TransactionResult PersistenceManager::transaction_expire(
    std::uint64_t txn_id, const std::string& key, std::int64_t ttl_ms) {
    if (ttl_ms <= 0) {
        return transaction_del(txn_id, key);
    }

    std::lock_guard<std::mutex> lock(operation_mutex_);
    TransactionState* transaction = find_transaction_locked(txn_id);
    if (transaction == nullptr ||
        transaction->state != TransactionStateKind::kActive) {
        return transaction_fail(MutationStatus::kNoTransaction,
                                "transaction is not active");
    }
    if (key.empty() || key.size() > WalFormat::MAX_KEY_SIZE) {
        return transaction_fail(MutationStatus::kInvalidArgument,
                                "invalid key length");
    }
    if (is_reserved_agent_key(key)) {
        return transaction_fail(MutationStatus::kInvalidArgument,
                                reserved_agent_key_error);
    }

    const auto it = transaction->writes.find(key);
    std::string value;
    if (it != transaction->writes.end()) {
        if (it->second.deleted ||
            (it->second.absolute_expiry_ms > 0 &&
             it->second.absolute_expiry_ms <= store_->wall_clock_now_ms())) {
            return TransactionResult{};
        }
        value = it->second.value;
    } else {
        const std::optional<std::string> existing =
            store_->get_at(key, transaction->snapshot_lsn);
        if (!existing.has_value()) {
            return TransactionResult{};
        }
        value = existing.value();
    }
    if (it == transaction->writes.end() &&
        transaction->writes.size() >= kMaxTransactionWriteKeys) {
        return abort_transaction_locked(txn_id, MutationStatus::kResourceLimit,
                                        "transaction write-key limit exceeded");
    }

    const std::size_t old_cost = it == transaction->writes.end()
                                     ? 0
                                     : WalFormat::TXN_OP_HEADER_SIZE +
                                           key.size() + it->second.value.size();
    const std::size_t new_cost =
        WalFormat::TXN_OP_HEADER_SIZE + key.size() + value.size();
    const std::size_t current_cost = transaction->write_bytes >= old_cost
                                         ? transaction->write_bytes - old_cost
                                         : 0;
    if (new_cost > WalFormat::MAX_TXN_PAYLOAD_SIZE ||
        current_cost > WalFormat::MAX_TXN_PAYLOAD_SIZE - new_cost) {
        return abort_transaction_locked(
            txn_id, MutationStatus::kResourceLimit,
            "transaction payload exceeds the size limit");
    }

    PendingWrite write;
    write.value = std::move(value);
    write.absolute_expiry_ms =
        add_ttl_saturated(store_->wall_clock_now_ms(), ttl_ms);
    transaction->writes[key] = std::move(write);
    transaction->write_bytes = current_cost + new_cost;
    TransactionResult result;
    result.changed = true;
    return result;
}

PersistenceManager::TransactionResult
PersistenceManager::transaction_ttl(std::uint64_t txn_id,
                                    const std::string& key) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    TransactionState* transaction = find_transaction_locked(txn_id);
    if (transaction == nullptr ||
        transaction->state != TransactionStateKind::kActive) {
        return transaction_fail(MutationStatus::kNoTransaction,
                                "transaction is not active");
    }
    if (key.empty() || key.size() > WalFormat::MAX_KEY_SIZE) {
        return transaction_fail(MutationStatus::kInvalidArgument,
                                "invalid key length");
    }
    if (is_reserved_agent_key(key)) {
        return transaction_fail(MutationStatus::kInvalidArgument,
                                reserved_agent_key_error);
    }

    TransactionResult result;
    result.status = MutationStatus::kOk;
    const auto it = transaction->writes.find(key);
    if (it != transaction->writes.end()) {
        if (it->second.deleted) {
            return result;
        }
        const int64_t now_ms = store_->wall_clock_now_ms();
        if (it->second.absolute_expiry_ms > 0 &&
            it->second.absolute_expiry_ms <= now_ms) {
            return result;
        }
        result.found = true;
        if (it->second.absolute_expiry_ms == 0) {
            result.ttl.state = KvStore::TtlState::kNoExpiry;
            result.ttl.remaining_ms = -1;
        } else {
            result.ttl.state = KvStore::TtlState::kExpiring;
            result.ttl.remaining_ms = it->second.absolute_expiry_ms - now_ms;
        }
        return result;
    }

    result.ttl = store_->ttl_info_at(key, transaction->snapshot_lsn);
    result.found = result.ttl.state != KvStore::TtlState::kMissing;
    return result;
}

MutationAdmission
PersistenceManager::commit_transaction_async(std::uint64_t txn_id,
                                             MutationCompletion completion) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    TransactionState* transaction = find_transaction_locked(txn_id);
    if (transaction == nullptr ||
        transaction->state != TransactionStateKind::kActive) {
        return admission_fail(MutationAdmissionStatus::kNoTransaction,
                              "transaction is not active");
    }
    if (!opened_ || failed_) {
        abort_transaction_locked(txn_id, MutationStatus::kIoError,
                                 failed_ ? mutation_io_error
                                         : "persistence is not open");
        return admission_fail(failed_ ? MutationAdmissionStatus::kIoError
                                      : MutationAdmissionStatus::kNotReady,
                              failed_ ? mutation_io_error
                                      : "persistence is not open");
    }
    if (!group_commit_started_) {
        abort_transaction_locked(txn_id, MutationStatus::kNotReady,
                                 "group commit writer is not started");
        return admission_fail(MutationAdmissionStatus::kNotReady,
                              "group commit writer is not started");
    }
    if (shutdown_requested_ || shutdown_complete_ || !accepting_mutations_) {
        abort_transaction_locked(txn_id, MutationStatus::kNotReady,
                                 "persistence is not accepting commits");
        return admission_fail(MutationAdmissionStatus::kShuttingDown,
                              "persistence is not accepting commits");
    }

    const std::uint64_t commit_started_us = steady_now_us();
    for (const auto& [key, write] : transaction->writes) {
        (void)write;
        if (is_reserved_agent_key(key)) {
            abort_transaction_locked(txn_id, MutationStatus::kInvalidArgument,
                                     reserved_agent_key_error);
            return admission_fail(MutationAdmissionStatus::kInvalidArgument,
                                  reserved_agent_key_error);
        }
    }
    if (transaction->writes.empty()) {
        ++transaction_stats_.committed;
        finish_transaction_locked(txn_id);
        MutationOutcome outcome;
        outcome.lsn = 0;
        if (completion) {
            auto measured = [this, commit_started_us,
                             completion = std::move(completion)](
                                MutationOutcome committed) mutable {
                {
                    std::lock_guard<std::mutex> guard(operation_mutex_);
                    const std::uint64_t now = steady_now_us();
                    if (now >= commit_started_us) {
                        observe_timing(now - commit_started_us,
                                       &transaction_stats_.commit_total_us,
                                       &transaction_stats_.commit_max_us);
                    }
                }
                completion(std::move(committed));
            };
            dispatch_owner([measured = std::move(measured),
                            outcome = std::move(outcome)]() mutable {
                measured(std::move(outcome));
            });
        }
        MutationAdmission admission;
        admission.status = MutationAdmissionStatus::kAccepted;
        return admission;
    }

    std::uint64_t conflicting_keys = 0;
    for (const auto& [key, write] : transaction->writes) {
        (void)write;
        const std::uint64_t committed_lsn = store_->latest_committed_lsn(key);
        const std::uint64_t pending_lsn = pending_key_lsn_locked(key);
        if (std::max(committed_lsn, pending_lsn) > transaction->snapshot_lsn) {
            ++conflicting_keys;
        }
    }
    if (conflicting_keys != 0) {
        finish_transaction_locked(txn_id);
        ++transaction_stats_.conflicted;
        add_saturated(&transaction_stats_.write_conflicts, conflicting_keys);
        observe_transaction_metrics_locked();
        return admission_fail(MutationAdmissionStatus::kConflict,
                              "transaction write conflict");
    }

    if (transaction->writes.size() > store_->max_versions()) {
        const std::string error =
            "transaction version reservation exceeds the global limit";
        abort_transaction_locked(txn_id, MutationStatus::kResourceLimit, error);
        return admission_fail(MutationAdmissionStatus::kResourceLimit, error);
    }
    const std::size_t current_versions = store_->version_count();
    const std::size_t version_limit = store_->max_versions();
    const std::size_t requested_versions = transaction->writes.size();
    if (current_versions > version_limit ||
        reserved_versions_ > version_limit - current_versions ||
        requested_versions >
            version_limit - current_versions - reserved_versions_) {
        abort_transaction_locked(txn_id, MutationStatus::kResourceLimit,
                                 "global version limit exceeded");
        return admission_fail(MutationAdmissionStatus::kResourceLimit,
                              "global version limit exceeded");
    }

    std::string writer_error;
    if (!ensure_group_commit_writer_locked(&writer_error)) {
        abort_transaction_locked(txn_id,
                                 failed_ ? MutationStatus::kIoError
                                         : MutationStatus::kNotReady,
                                 writer_error);
        return admission_fail(failed_ ? MutationAdmissionStatus::kIoError
                                      : MutationAdmissionStatus::kNotReady,
                              writer_error);
    }

    std::vector<std::string> keys;
    keys.reserve(transaction->writes.size());
    for (const auto& [key, write] : transaction->writes) {
        (void)write;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());

    std::vector<WalTxnOp> operations;
    operations.reserve(keys.size());
    for (const std::string& key : keys) {
        const PendingWrite& write = transaction->writes.at(key);
        WalTxnOp operation;
        operation.key = key;
        if (write.deleted) {
            operation.op = WalOp::kDel;
        } else {
            operation.op = WalOp::kSet;
            operation.value = write.value;
            operation.expiry_ms = write.absolute_expiry_ms;
        }
        operations.push_back(std::move(operation));
    }
    std::string payload_error;
    const std::vector<std::uint8_t> payload =
        WalFormat::encode_transaction(operations, &payload_error);
    if (payload.empty()) {
        abort_transaction_locked(txn_id, MutationStatus::kResourceLimit,
                                 payload_error.empty()
                                     ? "transaction payload is invalid"
                                     : payload_error);
        return admission_fail(MutationAdmissionStatus::kResourceLimit,
                              payload_error);
    }

    WalRecord record;
    record.lsn = group_commit_writer_->next_lsn();
    record.op = WalOp::kTxn;
    record.key = "txn";
    record.value.assign(reinterpret_cast<const char*>(payload.data()),
                        payload.size());
    transaction->state = TransactionStateKind::kCommitting;
    transaction->reserved_versions = requested_versions;
    reserved_versions_ += requested_versions;

    const MutationAdmission admission =
        admit_async(std::move(record), std::move(completion),
                    PendingMutationKind::kTransaction, txn_id);
    if (!admission.accepted()) {
        TransactionState* failed_transaction = find_transaction_locked(txn_id);
        if (failed_transaction != nullptr) {
            if (failed_transaction->reserved_versions <= reserved_versions_) {
                reserved_versions_ -= failed_transaction->reserved_versions;
            } else {
                reserved_versions_ = 0;
            }
            transactions_.erase(txn_id);
            ++transaction_stats_.failed;
            observe_transaction_metrics_locked();
        }
    }
    return admission;
}

PersistenceManager::TransactionResult
PersistenceManager::rollback_transaction(std::uint64_t txn_id) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    TransactionState* transaction = find_transaction_locked(txn_id);
    if (transaction == nullptr ||
        transaction->state != TransactionStateKind::kActive) {
        return transaction_fail(MutationStatus::kNoTransaction,
                                "transaction is not active");
    }
    finish_transaction_locked(txn_id);
    ++transaction_stats_.rolled_back;
    observe_transaction_metrics_locked();
    TransactionResult result;
    result.changed = true;
    return result;
}

MutationOutcome PersistenceManager::append_and_apply(const WalRecord& record) {
    std::string bootstrap_error;
    if (!ensure_bootstrapped_locked(&bootstrap_error)) {
        return fail(MutationStatus::kIoError, bootstrap_error);
    }

    std::vector<KvStore::AppliedMutation> mutations;
    std::string decode_error;
    if (!decode_record_mutations(record, &mutations, &decode_error)) {
        return fail(MutationStatus::kInvalidArgument, decode_error);
    }

    bool changed = false;
    for (const KvStore::AppliedMutation& mutation : mutations) {
        changed = changed || mutation_changed(*store_, mutation);
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

    std::string apply_error;
    const KvStore::ApplyStatus apply_status =
        store_->apply_committed_status(record.lsn, mutations, &apply_error);
    if (apply_status != KvStore::ApplyStatus::kOk) {
        // The record is already durable.  A rejected in-memory publication is
        // therefore fail-stop; recovery will retry the complete record.
        mark_failed();
        return fail(apply_status == KvStore::ApplyStatus::kResourceLimit
                        ? MutationStatus::kResourceLimit
                        : MutationStatus::kIoError,
                    apply_error);
    }
    last_lsn_ = record.lsn;

    if (!garbage_collect_locked(&apply_error)) {
        mark_failed();
        return fail(MutationStatus::kIoError, apply_error);
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
    outcome.lsn = record.lsn;
    observe_transaction_metrics_locked();
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
    if (is_reserved_agent_key(key)) {
        return admission_fail(MutationAdmissionStatus::kInvalidArgument,
                              reserved_agent_key_error);
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

MutationAdmission PersistenceManager::mset_async(
    std::vector<std::pair<std::string, std::string>> entries,
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
    if (entries.empty()) {
        return admission_fail(MutationAdmissionStatus::kInvalidArgument,
                              "MSET requires at least one key value pair");
    }
    if (entries.size() > WalFormat::MAX_TXN_OPS) {
        return admission_fail(MutationAdmissionStatus::kResourceLimit,
                              "MSET key count exceeds the limit");
    }

    // A canonical key order makes the MTXN payload deterministic.  Duplicate
    // keys are rejected because the operation would otherwise be ambiguous.
    std::sort(entries.begin(), entries.end(),
              [](const auto& left, const auto& right) {
                  return left.first < right.first;
              });

    std::size_t payload_size = WalFormat::TXN_HEADER_SIZE;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const std::string& key = entries[index].first;
        const std::string& value = entries[index].second;
        if (key.empty() || key.size() > WalFormat::MAX_KEY_SIZE) {
            return admission_fail(MutationAdmissionStatus::kInvalidArgument,
                                  "invalid key length");
        }
        if (is_reserved_agent_key(key)) {
            return admission_fail(MutationAdmissionStatus::kInvalidArgument,
                                  reserved_agent_key_error);
        }
        if (value.size() > WalFormat::MAX_VALUE_SIZE) {
            return admission_fail(MutationAdmissionStatus::kResourceLimit,
                                  "value exceeds WAL limit");
        }
        if (index > 0 && entries[index - 1].first == key) {
            return admission_fail(MutationAdmissionStatus::kInvalidArgument,
                                  "MSET contains a duplicate key");
        }
        const std::size_t operation_size =
            WalFormat::TXN_OP_HEADER_SIZE + key.size() + value.size();
        if (operation_size > WalFormat::MAX_TXN_PAYLOAD_SIZE ||
            payload_size > WalFormat::MAX_TXN_PAYLOAD_SIZE - operation_size) {
            return admission_fail(MutationAdmissionStatus::kResourceLimit,
                                  "MSET payload exceeds 1 MiB");
        }
        payload_size += operation_size;
    }

    std::vector<WalTxnOp> operations;
    operations.reserve(entries.size());
    for (auto& [key, value] : entries) {
        WalTxnOp operation;
        operation.op = WalOp::kSet;
        operation.key = std::move(key);
        operation.value = std::move(value);
        operations.push_back(std::move(operation));
    }
    std::string payload_error;
    const std::vector<std::uint8_t> payload =
        WalFormat::encode_transaction(operations, &payload_error);
    if (payload.empty()) {
        return admission_fail(MutationAdmissionStatus::kResourceLimit,
                              payload_error.empty() ? "MSET payload is invalid"
                                                    : payload_error);
    }

    std::string writer_error;
    if (!ensure_group_commit_writer_locked(&writer_error)) {
        return admission_fail(failed_ ? MutationAdmissionStatus::kIoError
                                      : MutationAdmissionStatus::kNotReady,
                              writer_error);
    }

    WalRecord record;
    record.lsn = group_commit_writer_->next_lsn();
    record.op = WalOp::kTxn;
    record.key = "txn";
    record.value.assign(reinterpret_cast<const char*>(payload.data()),
                        payload.size());
    return admit_async(std::move(record), std::move(completion),
                       PendingMutationKind::kMset);
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
    if (is_reserved_agent_key(key)) {
        return admission_fail(MutationAdmissionStatus::kInvalidArgument,
                              reserved_agent_key_error);
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
    if (is_reserved_agent_key(key)) {
        return admission_fail(MutationAdmissionStatus::kInvalidArgument,
                              reserved_agent_key_error);
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

AgentResult PersistenceManager::agent_query(const AgentRequest& request) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    AgentResult result;
    if (!opened_) {
        result.status = AgentRequestStatus::kNotReady;
        result.error = "persistence is not open";
        return result;
    }
    if (failed_) {
        result.status = AgentRequestStatus::kIoError;
        result.error = mutation_io_error;
        return result;
    }
    if (AgentStateStore::is_mutation(request.command)) {
        result.status = AgentRequestStatus::kInvalidArgument;
        result.error = "command is not an Agent query";
        return result;
    }

    AgentRecordCodec::Response response;
    std::string query_error;
    if (!agent_store_.prepare_query(request, *store_, &response,
                                    &query_error)) {
        mark_failed();
        result.status = AgentRequestStatus::kIoError;
        result.error = query_error.empty() ? "Agent query failed" : query_error;
        return result;
    }
    if (!AgentRecordCodec::encode_response(response, &result.encoded_response,
                                           &query_error)) {
        mark_failed();
        result.status = AgentRequestStatus::kIoError;
        result.error = query_error.empty() ? "Agent response encoding failed"
                                           : query_error;
        return result;
    }
    result.status = AgentRequestStatus::kCompleted;
    return result;
}

AgentResult
PersistenceManager::agent_mutate_async(const AgentRequest& request,
                                       AgentMutationCompletion completion) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    AgentResult result;
    if (!opened_ || failed_) {
        result.status = failed_ ? AgentRequestStatus::kIoError
                                : AgentRequestStatus::kNotReady;
        result.error = failed_ ? mutation_io_error : "persistence is not open";
        return result;
    }
    if (!group_commit_started_) {
        result.status = AgentRequestStatus::kNotReady;
        result.error = "group commit writer is not started";
        return result;
    }
    if (shutdown_requested_ || shutdown_complete_) {
        result.status = AgentRequestStatus::kShuttingDown;
        result.error = "persistence is shutting down";
        return result;
    }
    if (!accepting_mutations_) {
        result.status = AgentRequestStatus::kNotReady;
        result.error = "persistence admission is paused";
        return result;
    }
    if (!AgentStateStore::is_mutation(request.command)) {
        result.status = AgentRequestStatus::kInvalidArgument;
        result.error = "command is not an Agent mutation";
        return result;
    }

    std::string writer_error;
    if (!ensure_group_commit_writer_locked(&writer_error)) {
        result.status = failed_ ? AgentRequestStatus::kIoError
                                : AgentRequestStatus::kNotReady;
        result.error =
            writer_error.empty() ? "group commit is not ready" : writer_error;
        return result;
    }

    AgentStateStore::MutationPlan plan;
    std::string prepare_error;
    const std::uint64_t assigned_lsn = group_commit_writer_->next_lsn();
    if (!agent_store_.prepare_mutation(request, *store_, assigned_lsn, &plan,
                                       &prepare_error)) {
        result.status = AgentRequestStatus::kInvalidArgument;
        result.error =
            prepare_error.empty() ? "invalid Agent mutation" : prepare_error;
        return result;
    }

    if (plan.disposition ==
        AgentStateStore::MutationPlanDisposition::kImmediate) {
        result.status = AgentRequestStatus::kAccepted;
        if (completion) {
            AgentResult immediate;
            immediate.status = AgentRequestStatus::kCompleted;
            immediate.encoded_response = std::move(plan.encoded_response);
            dispatch_owner([completion = std::move(completion),
                            immediate = std::move(immediate)]() mutable {
                completion(std::move(immediate));
            });
        }
        return result;
    }

    if (plan.operations.empty() || plan.reservation_id == 0) {
        agent_store_.cancel_reservation(plan.reservation_id);
        result.status = AgentRequestStatus::kResourceLimit;
        result.error = "Agent mutation plan is invalid";
        return result;
    }

    std::string payload_error;
    const std::vector<std::uint8_t> payload =
        WalFormat::encode_transaction(plan.operations, &payload_error);
    if (payload.empty()) {
        agent_store_.cancel_reservation(plan.reservation_id);
        result.status = AgentRequestStatus::kResourceLimit;
        result.error = payload_error.empty() ? "Agent transaction is invalid"
                                             : payload_error;
        return result;
    }

    WalRecord record;
    record.lsn = assigned_lsn;
    record.op = WalOp::kTxn;
    record.key = "txn";
    record.value.assign(reinterpret_cast<const char*>(payload.data()),
                        payload.size());

    const MutationAdmission admission =
        admit_agent_async(std::move(record), std::move(plan.operations),
                          std::move(plan.encoded_response), plan.reservation_id,
                          PendingMutationKind::kAgent, std::move(completion));
    result.status = agent_status_from_admission_status(admission.status);
    result.error = admission.error;
    return result;
}

PersistenceControlResult PersistenceManager::agent_gc_async_result() {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (!opened_) {
        return control_fail(PersistenceControlStatus::kNotOpen,
                            "persistence is not open");
    }
    if (failed_) {
        return control_fail(PersistenceControlStatus::kIoError,
                            mutation_io_error);
    }
    if (!group_commit_started_) {
        return control_fail(PersistenceControlStatus::kGroupCommitNotStarted,
                            "group commit writer is not started");
    }
    if (shutdown_requested_ || shutdown_complete_ || !accepting_mutations_) {
        return control_fail(PersistenceControlStatus::kShuttingDown,
                            "persistence is shutting down");
    }

    std::string writer_error;
    if (!ensure_group_commit_writer_locked(&writer_error)) {
        return control_fail(failed_ ? PersistenceControlStatus::kIoError
                                    : PersistenceControlStatus::kNotReady,
                            writer_error.empty() ? "group commit is not ready"
                                                 : writer_error);
    }

    const std::vector<AgentStateStore::GcCandidate> candidates =
        agent_store_.collect_gc_candidates();
    std::vector<std::string> selected_keys;
    std::size_t selected_candidates = 0;
    for (const AgentStateStore::GcCandidate& candidate : candidates) {
        bool pending = false;
        for (const std::string& key : candidate.keys) {
            if (pending_key_lsn_locked(key) != 0) {
                pending = true;
                break;
            }
        }
        if (pending || selected_keys.size() + candidate.keys.size() >
                           AgentRecordCodec::Limits::kMaxGcTxnOps) {
            continue;
        }
        if (selected_candidates >= AgentRecordCodec::Limits::kMaxGcCandidates) {
            break;
        }
        selected_keys.insert(selected_keys.end(), candidate.keys.begin(),
                             candidate.keys.end());
        ++selected_candidates;
    }

    if (selected_keys.empty()) {
        return control_fail(PersistenceControlStatus::kAccepted, {});
    }

    std::vector<WalTxnOp> operations;
    operations.reserve(selected_keys.size());
    for (const std::string& key : selected_keys) {
        WalTxnOp operation;
        operation.op = WalOp::kDel;
        operation.key = key;
        operations.push_back(std::move(operation));
    }

    std::string payload_error;
    const std::vector<std::uint8_t> payload =
        WalFormat::encode_transaction(operations, &payload_error);
    if (payload.empty()) {
        agent_store_.note_gc_failure();
        return control_fail(PersistenceControlStatus::kNotReady,
                            payload_error.empty()
                                ? "Agent GC transaction is invalid"
                                : payload_error);
    }

    WalRecord record;
    record.lsn = group_commit_writer_->next_lsn();
    record.op = WalOp::kTxn;
    record.key = "txn";
    record.value.assign(reinterpret_cast<const char*>(payload.data()),
                        payload.size());

    const MutationAdmission admission =
        admit_agent_async(std::move(record), std::move(operations), {}, 0,
                          PendingMutationKind::kAgentGc, {},
                          selected_candidates, selected_keys.size());
    if (admission.accepted()) {
        return control_fail(PersistenceControlStatus::kAccepted, {});
    }

    agent_store_.note_gc_failure();
    PersistenceControlStatus status = PersistenceControlStatus::kNotReady;
    if (admission.status == MutationAdmissionStatus::kIoError) {
        status = PersistenceControlStatus::kIoError;
    } else if (admission.status == MutationAdmissionStatus::kShuttingDown) {
        status = PersistenceControlStatus::kShuttingDown;
    } else if (admission.status == MutationAdmissionStatus::kResourceLimit) {
        status = PersistenceControlStatus::kBusy;
    }
    return control_fail(status, admission.error.empty()
                                    ? "Agent GC admission failed"
                                    : admission.error);
}

bool PersistenceManager::agent_gc_async(std::string* error) {
    PersistenceControlResult result = agent_gc_async_result();
    if (!result.accepted() && error != nullptr) {
        *error = result.error;
    }
    // The legacy API treated a shutdown no-op and a transient deferred GC as
    // successful fire-and-forget scheduling.  Preserve that contract while the
    // typed API lets protocol code distinguish the cases.
    return result.status == PersistenceControlStatus::kAccepted ||
           result.status == PersistenceControlStatus::kBusy ||
           result.status == PersistenceControlStatus::kShuttingDown ||
           result.status == PersistenceControlStatus::kShutdownComplete ||
           result.status == PersistenceControlStatus::kNotReady;
}

MutationAdmission PersistenceManager::admit_agent_async(
    WalRecord record, std::vector<WalTxnOp> operations, std::string response,
    std::uint64_t reservation_id, PendingMutationKind kind,
    AgentMutationCompletion completion, std::size_t gc_candidates,
    std::size_t gc_keys) {
    if (kind != PendingMutationKind::kAgent &&
        kind != PendingMutationKind::kAgentGc) {
        agent_store_.cancel_reservation(reservation_id);
        return admission_fail(MutationAdmissionStatus::kInvalidArgument,
                              "invalid Agent pending mutation kind");
    }
    if ((kind == PendingMutationKind::kAgent && reservation_id == 0) ||
        (kind == PendingMutationKind::kAgentGc && reservation_id != 0) ||
        (kind == PendingMutationKind::kAgent && response.empty()) ||
        operations.empty()) {
        agent_store_.cancel_reservation(reservation_id);
        return admission_fail(MutationAdmissionStatus::kInvalidArgument,
                              "invalid Agent admission metadata");
    }

    PendingMutation pending;
    pending.lsn = record.lsn;
    pending.started_us = steady_now_us();
    pending.agent_completion = std::move(completion);
    pending.kind = kind;
    pending.keys.reserve(operations.size());
    for (const WalTxnOp& operation : operations) {
        pending.keys.push_back(operation.key);
    }
    std::sort(pending.keys.begin(), pending.keys.end());
    if (std::adjacent_find(pending.keys.begin(), pending.keys.end()) !=
        pending.keys.end()) {
        agent_store_.cancel_reservation(reservation_id);
        return admission_fail(MutationAdmissionStatus::kInvalidArgument,
                              "Agent transaction contains duplicate keys");
    }
    pending.agent_operations = std::move(operations);
    pending.agent_response = std::move(response);
    pending.agent_reservation_id = reservation_id;
    pending.agent_gc_candidates = gc_candidates;
    pending.agent_gc_keys = gc_keys;

    for (const std::string& key : pending.keys) {
        if (pending_key_lsn_locked(key) != 0) {
            agent_store_.cancel_reservation(reservation_id);
            return admission_fail(MutationAdmissionStatus::kConflict,
                                  "Agent key has a pending mutation");
        }
    }

    for (const std::string& key : pending.keys) {
        add_pending_key_lsn_locked(key, record.lsn);
    }
    pending_mutations_.push_back(std::move(pending));

    GroupCommitEntry entry;
    entry.record = std::move(record);
    entry.submitted_us = steady_now_us();
    const GroupCommitSubmitResult submit =
        group_commit_writer_->submit_result(std::move(entry));
    if (!submit.accepted()) {
        const PendingMutation& rejected = pending_mutations_.back();
        remove_pending_keys_locked(rejected.keys, rejected.lsn);
        agent_store_.cancel_reservation(rejected.agent_reservation_id);
        pending_mutations_.pop_back();

        return admission_fail(admission_status_from_submit(
                                  submit.status, group_commit_writer_->state()),
                              submit.error);
    }

    MutationAdmission admission;
    admission.status = MutationAdmissionStatus::kAccepted;
    return admission;
}

MutationAdmission PersistenceManager::admit_async(WalRecord record,
                                                  MutationCompletion completion,
                                                  PendingMutationKind kind,
                                                  std::uint64_t txn_id) {
    const std::uint64_t lsn = record.lsn;
    const std::uint64_t started_us = steady_now_us();

    std::vector<KvStore::AppliedMutation> mutations;
    std::string decode_error;
    if (!decode_record_mutations(record, &mutations, &decode_error)) {
        return admission_fail(MutationAdmissionStatus::kInvalidArgument,
                              decode_error);
    }

    PendingMutation pending;
    pending.lsn = lsn;
    pending.started_us = started_us;
    pending.completion = std::move(completion);
    pending.kind = kind;
    pending.txn_id = txn_id;
    pending.keys.reserve(mutations.size());
    for (const KvStore::AppliedMutation& mutation : mutations) {
        pending.keys.push_back(mutation.key);
    }
    std::sort(pending.keys.begin(), pending.keys.end());
    pending.keys.erase(std::unique(pending.keys.begin(), pending.keys.end()),
                       pending.keys.end());

    // Publish both halves of the admission bookkeeping before handing the
    // record to the writer.  Conflict detection must observe records that are
    // queued but not yet applied.
    for (const std::string& key : pending.keys) {
        add_pending_key_lsn_locked(key, lsn);
    }
    pending_mutations_.push_back(std::move(pending));

    GroupCommitEntry entry;
    entry.record = std::move(record);
    entry.submitted_us = started_us;
    const GroupCommitSubmitResult submit =
        group_commit_writer_->submit_result(std::move(entry));
    if (!submit.accepted()) {
        const PendingMutation& rejected = pending_mutations_.back();
        remove_pending_keys_locked(rejected.keys, rejected.lsn);
        const PendingMutationKind rejected_kind = rejected.kind;
        const std::uint64_t rejected_txn_id = rejected.txn_id;
        pending_mutations_.pop_back();

        if (rejected_kind == PendingMutationKind::kTransaction &&
            find_transaction_locked(rejected_txn_id) != nullptr) {
            finish_transaction_locked(rejected_txn_id);
            ++transaction_stats_.failed;
            observe_transaction_metrics_locked();
        }

        return admission_fail(admission_status_from_submit(
                                  submit.status, group_commit_writer_->state()),
                              submit.error);
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

PersistenceControlResult PersistenceManager::flush_interval_async_result(
    GroupCommitWriter::BarrierCompletion completion) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (!opened_) {
        return control_fail(PersistenceControlStatus::kNotOpen,
                            "persistence is not open");
    }
    if (failed_) {
        return control_fail(PersistenceControlStatus::kIoError,
                            mutation_io_error);
    }
    if (!group_commit_started_) {
        return control_fail(PersistenceControlStatus::kGroupCommitNotStarted,
                            "group commit writer is not started");
    }
    if (shutdown_requested_ || shutdown_complete_) {
        return control_fail(PersistenceControlStatus::kShuttingDown,
                            "persistence is shutting down");
    }
    if (control_pending_ || !accepting_mutations_) {
        return control_fail(PersistenceControlStatus::kBusy,
                            "persistence control operation is in progress");
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
        return control_fail(PersistenceControlStatus::kAccepted, {});
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
    std::string request_error;
    if (!group_commit_writer_->request_flush(std::move(wrapped),
                                             &request_error)) {
        const GroupCommitWriterState state = group_commit_writer_->state();
        PersistenceControlStatus status = PersistenceControlStatus::kBusy;
        if (state == GroupCommitWriterState::kFailed || failed_) {
            status = PersistenceControlStatus::kIoError;
        } else if (state == GroupCommitWriterState::kStopped ||
                   state == GroupCommitWriterState::kDraining) {
            status = PersistenceControlStatus::kShuttingDown;
        }
        return control_fail(status, request_error);
    }
    return control_fail(PersistenceControlStatus::kAccepted, {});
}

bool PersistenceManager::flush_interval_async(
    GroupCommitWriter::BarrierCompletion completion, std::string* error) {
    PersistenceControlResult result =
        flush_interval_async_result(std::move(completion));
    if (!result.accepted() && error != nullptr) {
        *error = result.error;
    }
    return result.accepted();
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

PersistenceControlResult
PersistenceManager::checkpoint_async_result(CheckpointCompletion completion) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    return start_checkpoint_locked(std::move(completion), false);
}

bool PersistenceManager::checkpoint_async(CheckpointCompletion completion,
                                          std::string* error) {
    PersistenceControlResult result =
        checkpoint_async_result(std::move(completion));
    if (!result.accepted() && error != nullptr) {
        *error = result.error;
    }
    return result.accepted();
}

PersistenceControlResult
PersistenceManager::shutdown_async_result(CheckpointCompletion completion) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    if (shutdown_complete_) {
        return control_fail(PersistenceControlStatus::kShutdownComplete,
                            "persistence shutdown is complete");
    }
    if (shutdown_requested_) {
        return control_fail(PersistenceControlStatus::kShuttingDown,
                            "persistence shutdown has already been requested");
    }
    return start_checkpoint_locked(std::move(completion), true);
}

bool PersistenceManager::shutdown_async(CheckpointCompletion completion,
                                        std::string* error) {
    PersistenceControlResult result =
        shutdown_async_result(std::move(completion));
    if (!result.accepted() && error != nullptr) {
        *error = result.error;
    }
    return result.accepted();
}

PersistenceControlResult
PersistenceManager::start_checkpoint_locked(CheckpointCompletion completion,
                                            bool shutdown) {
    if (!opened_) {
        return control_fail(PersistenceControlStatus::kNotOpen,
                            "persistence is not open");
    }
    if (failed_) {
        return control_fail(PersistenceControlStatus::kIoError,
                            mutation_io_error);
    }
    if (!group_commit_started_) {
        return control_fail(PersistenceControlStatus::kGroupCommitNotStarted,
                            "group commit writer is not started");
    }
    if (checkpoint_operation_) {
        return control_fail(
            PersistenceControlStatus::kBusy,
            "another persistence control operation is in progress");
    }
    if (shutdown_complete_) {
        return control_fail(PersistenceControlStatus::kShutdownComplete,
                            "persistence shutdown is complete");
    }
    if (!shutdown && shutdown_requested_) {
        return control_fail(PersistenceControlStatus::kShuttingDown,
                            "persistence is shutting down");
    }
    if (checkpoint_worker_.joinable()) {
        return control_fail(PersistenceControlStatus::kBusy,
                            "previous checkpoint worker has not been joined");
    }

    std::string writer_error;
    if (!ensure_group_commit_writer_locked(&writer_error)) {
        return control_fail(failed_ ? PersistenceControlStatus::kIoError
                                    : PersistenceControlStatus::kNotReady,
                            writer_error.empty() ? "group commit is not ready"
                                                 : writer_error);
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
        PersistenceControlStatus status = PersistenceControlStatus::kBusy;
        if (failed_) {
            status = PersistenceControlStatus::kIoError;
        } else if (state == GroupCommitWriterState::kStopped ||
                   state == GroupCommitWriterState::kDraining) {
            status = PersistenceControlStatus::kShuttingDown;
        }
        return control_fail(status, control_error.empty()
                                        ? "checkpoint could not start"
                                        : control_error);
    }

    if (shutdown) {
        std::vector<std::uint64_t> active_txn_ids;
        active_txn_ids.reserve(transactions_.size());
        for (const auto& [txn_id, transaction] : transactions_) {
            if (transaction.state == TransactionStateKind::kActive) {
                active_txn_ids.push_back(txn_id);
            }
        }
        for (const std::uint64_t txn_id : active_txn_ids) {
            finish_transaction_locked(txn_id);
            ++transaction_stats_.rolled_back;
        }
        observe_transaction_metrics_locked();
    }
    return control_fail(PersistenceControlStatus::kAccepted, {});
}

PersistenceControlResult
PersistenceManager::control_fail(PersistenceControlStatus status,
                                 const std::string& error) const {
    PersistenceControlResult result;
    result.status = status;
    result.error = error;
    return result;
}

void PersistenceManager::set_checkpoint_rdb_publish_hook(
    std::function<void()> callback) {
    std::lock_guard<std::mutex> lock(operation_mutex_);
    checkpoint_rdb_publish_hook_ = std::move(callback);
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
                            std::function<void()> rdb_publish_hook =
                                checkpoint_rdb_publish_hook_;
                            checkpoint_worker_ =
                                std::thread([this, job = std::move(job),
                                             rdb_publish_hook = std::move(
                                                 rdb_publish_hook)]() mutable {
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
                                        write_result =
                                            rdb_manager_->write_durable(
                                                image, &timings, &write_error);
                                        if (write_result == RdbSaveResult::Ok &&
                                            rdb_publish_hook) {
                                            try {
                                                rdb_publish_hook();
                                            } catch (const std::exception& ex) {
                                                Log::Error(std::string(
                                                               "checkpoint RDB "
                                                               "publish hook "
                                                               "threw: ") +
                                                           ex.what());
                                            } catch (...) {
                                                Log::Error("checkpoint RDB "
                                                           "publish hook "
                                                           "threw an unknown "
                                                           "exception");
                                            }
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
                                                        write_error = std::move(
                                                            write_error),
                                                        timings]() mutable {
                                            on_checkpoint_written(
                                                write_result, write_error,
                                                timings.serialize_us,
                                                timings.write_us,
                                                timings.fsync_us);
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
            AgentMutationCompletion agent_completion;
            MutationOutcome outcome;
            AgentResult agent_outcome;
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
                remove_pending_keys_locked(pending.keys, pending.lsn);

                const bool is_transaction =
                    pending.kind == PendingMutationKind::kTransaction;
                const bool is_agent =
                    pending.kind == PendingMutationKind::kAgent;
                const bool is_agent_gc =
                    pending.kind == PendingMutationKind::kAgentGc;
                const bool has_agent_state = is_agent || is_agent_gc;
                bool transaction_finished = false;
                bool agent_state_applied = !has_agent_state;
                bool gc_failure_recorded = false;
                MutationOutcome outcome;
                AgentResult agent_outcome;
                agent_outcome.status = AgentRequestStatus::kIoError;

                const auto finish_pending_transaction = [&]() {
                    if (!is_transaction || transaction_finished) {
                        return;
                    }
                    if (find_transaction_locked(pending.txn_id) != nullptr) {
                        finish_transaction_locked(pending.txn_id);
                    }
                    transaction_finished = true;
                };
                const auto note_agent_failure = [&](AgentRequestStatus status,
                                                    const std::string& error) {
                    if (has_agent_state && !agent_state_applied) {
                        agent_store_.cancel_reservation(
                            pending.agent_reservation_id);
                        if (is_agent_gc && !gc_failure_recorded) {
                            agent_store_.note_gc_failure();
                            gc_failure_recorded = true;
                        }
                    }
                    if (is_agent) {
                        agent_outcome.status = status;
                        agent_outcome.error = error;
                    }
                };
                const auto persist_failure = [&](MutationStatus status,
                                                 const std::string& error) {
                    outcome.status = status;
                    outcome.error = error;
                    note_agent_failure(
                        agent_status_from_mutation_status(status), error);
                };

                if (pending.lsn != result.record.lsn) {
                    mark_failed();
                    persist_failure(MutationStatus::kIoError,
                                    "group commit completion LSN mismatch");
                } else if (!result.ok() || !result.written) {
                    mark_failed();
                    persist_failure(mutation_status_from_wal(result.status),
                                    result.error.empty()
                                        ? "group commit WAL operation failed"
                                        : result.error);
                } else if (failed_) {
                    persist_failure(MutationStatus::kIoError,
                                    mutation_io_error);
                } else if (result.record.lsn != last_lsn_ + 1) {
                    mark_failed();
                    persist_failure(MutationStatus::kIoError,
                                    "group commit apply LSN is not contiguous");
                } else {
                    bool changed = false;
                    std::string apply_error;
                    bool apply_succeeded = false;
                    try {
                        std::vector<KvStore::AppliedMutation> mutations;
                        std::string decode_error;
                        if (!decode_record_mutations(result.record, &mutations,
                                                     &decode_error)) {
                            // GroupCommitWriter validated the record before
                            // enqueueing it.  A decode failure here means the
                            // durable WAL is inconsistent with the in-memory
                            // admission path, so fail closed.
                            mark_failed();
                            persist_failure(MutationStatus::kInvalidArgument,
                                            decode_error);
                        } else {
                            for (const KvStore::AppliedMutation& mutation :
                                 mutations) {
                                changed = changed ||
                                          mutation_changed(*store_, mutation);
                            }
                            const KvStore::ApplyStatus apply_status =
                                store_->apply_committed_status(
                                    result.record.lsn, mutations, &apply_error);
                            if (apply_status != KvStore::ApplyStatus::kOk) {
                                mark_failed();
                                persist_failure(
                                    apply_status ==
                                            KvStore::ApplyStatus::kResourceLimit
                                        ? MutationStatus::kResourceLimit
                                        : MutationStatus::kIoError,
                                    apply_error);
                            } else {
                                apply_succeeded = true;
                                last_lsn_ = result.record.lsn;
                                group_commit_writer_->observe_applied(
                                    result, steady_now_us());
                            }
                        }
                    } catch (const std::exception& ex) {
                        mark_failed();
                        persist_failure(MutationStatus::kIoError,
                                        std::string("store apply failed: ") +
                                            ex.what());
                    } catch (...) {
                        mark_failed();
                        persist_failure(MutationStatus::kIoError,
                                        "store apply failed");
                    }

                    if (apply_succeeded && has_agent_state) {
                        std::string agent_error;
                        if (!agent_store_.apply_committed(
                                pending.agent_operations, result.record.lsn,
                                pending.agent_reservation_id, &agent_error)) {
                            mark_failed();
                            persist_failure(MutationStatus::kIoError,
                                            agent_error.empty()
                                                ? "Agent state apply failed"
                                                : agent_error);
                        } else {
                            agent_state_applied = true;
                            if (is_agent) {
                                agent_outcome.status =
                                    AgentRequestStatus::kCompleted;
                                agent_outcome.encoded_response =
                                    pending.agent_response;
                                agent_outcome.error.clear();
                            } else {
                                std::uint64_t elapsed_us = 0;
                                const std::uint64_t now_us = steady_now_us();
                                if (now_us >= pending.started_us) {
                                    elapsed_us = now_us - pending.started_us;
                                }
                                agent_store_.note_gc_success(
                                    pending.agent_gc_candidates,
                                    pending.agent_gc_keys, elapsed_us);
                            }
                        }
                    }

                    if (apply_succeeded && agent_state_applied) {
                        // A committing transaction has already built its WAL
                        // record and can no longer read its snapshot.  Release
                        // it before GC so the new commit is not artificially
                        // pinned by its own snapshot.
                        finish_pending_transaction();

                        std::string gc_error;
                        if (!garbage_collect_locked(&gc_error)) {
                            mark_failed();
                            outcome.status = MutationStatus::kIoError;
                            outcome.error = gc_error.empty()
                                                ? "version GC failed"
                                                : gc_error;
                            if (is_agent) {
                                agent_outcome.status =
                                    AgentRequestStatus::kIoError;
                                agent_outcome.error = outcome.error;
                                agent_outcome.encoded_response.clear();
                            }
                        } else {
                            if (after_apply_callback_) {
                                after_apply_callback_(result.record);
                            }
                            const std::uint64_t now_us = steady_now_us();
                            ++stats_.mutations;
                            if (durability_mode_ == DurabilityMode::kSync) {
                                ++stats_.sync_mutations;
                                if (now_us >= pending.started_us) {
                                    observe_timing(
                                        now_us - pending.started_us,
                                        &stats_.sync_mutation_total_us,
                                        &stats_.sync_mutation_max_us);
                                }
                            } else {
                                ++stats_.interval_mutations;
                            }
                            outcome.status = MutationStatus::kOk;
                            outcome.changed = changed;
                            outcome.lsn = result.record.lsn;
                            outcome.error.clear();

                            if (is_transaction) {
                                ++transaction_stats_.committed;
                                ++transaction_stats_.commit_records;
                                add_saturated(
                                    &transaction_stats_.commit_bytes,
                                    WalFormat::encoded_size(result.record));
                                if (now_us >= pending.started_us) {
                                    observe_timing(
                                        now_us - pending.started_us,
                                        &transaction_stats_.commit_total_us,
                                        &transaction_stats_.commit_max_us);
                                }
                            }
                        }
                    }
                }

                finish_pending_transaction();
                if (outcome.status != MutationStatus::kOk && is_transaction) {
                    ++transaction_stats_.failed;
                }
                observe_transaction_metrics_locked();

                CompletionCall call;
                call.completion = std::move(pending.completion);
                call.agent_completion = std::move(pending.agent_completion);
                call.outcome = std::move(outcome);
                call.agent_outcome = std::move(agent_outcome);
                callbacks.push_back(std::move(call));
            }
        }

        for (CompletionCall& call : callbacks) {
            if (call.agent_completion) {
                try {
                    call.agent_completion(std::move(call.agent_outcome));
                } catch (const std::exception& ex) {
                    Log::Error(
                        std::string("Agent mutation completion threw: ") +
                        ex.what());
                } catch (...) {
                    Log::Error(
                        "Agent mutation completion threw an unknown exception");
                }
                continue;
            }
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
    result.transactions = transaction_stats_;
    result.agent = agent_store_.stats();
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
    case MutationStatus::kNoTransaction:
        return "no-transaction";
    case MutationStatus::kConflict:
        return "conflict";
    }
    return "unknown";
}

} // namespace minidb
