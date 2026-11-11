#pragma once

#include "DurabilityPolicy.h"
#include "KvStore.h"
#include "RdbManager.h"
#include "RecoveryManager.h"
#include "SingleWriterLock.h"
#include "WalFileOperations.h"
#include "WalManager.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace minidb {

enum class MutationStatus : std::uint8_t {
    kOk,
    kInvalidArgument,
    kIoError,
    kResourceLimit,
    kNotReady,
};

struct MutationOutcome {
    MutationStatus status = MutationStatus::kOk;
    bool changed = false;
    std::string error;

    bool ok() const noexcept {
        return status == MutationStatus::kOk;
    }
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

    MutationOutcome set(const std::string& key, const std::string& value);
    MutationOutcome del(const std::string& key);
    MutationOutcome expire(const std::string& key, std::int64_t ttl_ms);

    // INTERVAL timer hook.  SYNC is a no-op.  The caller runs this on the
    // owner EventLoop; the manager serializes it with mutations/checkpoints.
    bool flush_interval(std::string* error = nullptr);

    // SAVE and shutdown both use a stop-the-world checkpoint.  The snapshot
    // and manifest are published before any old WAL segment is removed.
    RdbSaveResult checkpoint(std::string* error = nullptr);

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
    MutationOutcome set_impl(const std::string& key, const std::string& value);
    MutationOutcome del_impl(const std::string& key);
    MutationOutcome expire_impl(const std::string& key, std::int64_t ttl_ms);
    MutationOutcome append_and_apply(const WalRecord& record, bool apply_set,
                                     const std::string& apply_value,
                                     bool apply_del,
                                     std::int64_t apply_expiry_ms);
    MutationOutcome fail(MutationStatus status, const std::string& error);

    bool ensure_bootstrapped_locked(std::string* error);
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
    mutable std::mutex operation_mutex_;
    std::uint64_t last_lsn_ = 0;
    bool opened_ = false;
    bool failed_ = false;
    PersistenceStats stats_;
    std::function<void(const WalRecord&)> after_apply_callback_;
};

const char* mutation_status_name(MutationStatus status);

} // namespace minidb
