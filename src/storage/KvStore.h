#pragma once
#include "../base/Clock.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace minidb {

// A thread-safe key/value store with lazy and periodic TTL expiry.
//
// Committed values are kept in per-key version chains ordered by commit LSN.
// Snapshot reads select the newest version whose commit_lsn is not greater
// than the caller-provided snapshot.  The legacy set/get/del/expire helpers
// synthesize commit LSNs so in-memory users and tests share the same version
// representation as persistent/WAL-backed operation.
//
// Time semantics:
//   * a version stores a monotonic deadline, so wall-clock jumps cannot make a
//     key expire early or late while the process is running;
//   * Entry::expiry_ms is an absolute Unix epoch millisecond timestamp used
//     only at the RDB boundary;
//   * reads select a version first and then apply TTL expiry.  Expiry never
//     falls back to an older version.
class KvStore {
  public:
    static constexpr std::size_t MAX_KEY_SIZE = 1024;
    static constexpr std::size_t MAX_VALUE_SIZE = std::size_t{1024} * 1024;
    static constexpr std::size_t DEFAULT_MAX_VERSIONS = 1000000;

    struct Limits {
        std::size_t max_versions = DEFAULT_MAX_VERSIONS;
    };

    explicit KvStore(std::shared_ptr<ClockInterface> clock = nullptr);
    KvStore(std::shared_ptr<ClockInterface> clock, Limits limits);
    ~KvStore();

    KvStore(const KvStore&) = delete;
    KvStore& operator=(const KvStore&) = delete;

    // SET replaces the value and clears any existing TTL.
    void set(const std::string& key, const std::string& value);

    // Returns no value for a missing, expired, already-reaped, or tombstoned
    // key.
    std::optional<std::string> get(const std::string& key);

    // Returns true only when a live key was removed.
    bool del(const std::string& key);

    // EXPIRE with a non-positive TTL removes a live key.  Returns true when
    // the key existed and the operation was applied.
    bool expire(const std::string& key, int64_t ttl_ms);

    // Recovery/WAL helpers.  The deadline is an absolute Unix epoch
    // millisecond value and is converted to the monotonic deadline clock at
    // application time.  A non-positive or already-expired deadline removes
    // the key.  The return value reports whether a live key was changed.
    bool expire_absolute(const std::string& key, int64_t absolute_deadline_ms);

    // SET with an absolute expiry.  This is used by WAL replay so a restart
    // cannot extend or shorten a persisted TTL.  An expired record is valid
    // input but leaves the key logically absent.
    bool set_with_expiry(const std::string& key, const std::string& value,
                         int64_t absolute_expiry_ms);

    struct AppliedMutation {
        enum class Kind : std::uint8_t { kSet, kDelete, kExpire };
        Kind kind = Kind::kSet;
        std::string key;
        std::string value;
        std::int64_t absolute_expiry_ms = 0;
    };

    // Explicit outcome for callers that must distinguish malformed WAL
    // input from a bounded-resource rejection.  kInvalidArgument covers all
    // caller/data validation failures; kResourceLimit is returned only when
    // publishing would exceed the configured version budget.  Neither result
    // publishes a version or advances the store LSN.
    enum class ApplyStatus : std::uint8_t {
        kOk,
        kInvalidArgument,
        kResourceLimit,
    };

    // Validates and atomically publishes one committed WAL record.  Every key
    // appears at most once in the resulting version metadata: duplicate
    // operations in the input are folded to their final state first.  On
    // validation/allocation failure no version or LSN is published.
    ApplyStatus
    apply_committed_status(std::uint64_t commit_lsn,
                           const std::vector<AppliedMutation>& mutations,
                           std::string* error = nullptr);

    // Compatibility wrapper.  New persistence/recovery callers should use
    // apply_committed_status() so they never infer failure kinds from error
    // text.
    bool apply_committed(std::uint64_t commit_lsn,
                         const std::vector<AppliedMutation>& mutations,
                         std::string* error = nullptr) {
        return apply_committed_status(commit_lsn, mutations, error) ==
               ApplyStatus::kOk;
    }

    // Snapshot read.  snapshot_lsn == 0 addresses versions restored from an
    // RDB image, which predate the replayed WAL.
    std::optional<std::string> get_at(const std::string& key,
                                      std::uint64_t snapshot_lsn);

    // Exposed for the persistence layer, which must record the same absolute
    // deadline that the in-memory store will use.
    int64_t wall_clock_now_ms() const {
        return system_now_ms();
    }

    enum class TtlState : std::uint8_t { kMissing, kNoExpiry, kExpiring };

    struct TtlResult {
        TtlState state = TtlState::kMissing;
        int64_t remaining_ms = 0;
    };

    // Distinguishes a missing/expired key from an existing key without TTL.
    TtlResult ttl_info(const std::string& key);
    TtlResult ttl_info_at(const std::string& key, std::uint64_t snapshot_lsn);

    // Compatibility helper: Missing/expired: nullopt.  Existing without TTL:
    // -1.  Otherwise the remaining monotonic duration rounded down to
    // milliseconds.  A live TTL less than 1 ms before its deadline reports
    // 0; only now >= deadline is treated as expired.
    std::optional<int64_t> ttl(const std::string& key);

    // Number of logical live entries at the latest commit LSN.  Expired
    // versions are retired while computing the result.
    size_t size() const;

    void clear();

    // Atomic publication of a fully validated recovery image.  The source
    // store must use the same clock policy as the target.  Data, synthetic
    // LSN progress, and version metadata are swapped together.
    // NOLINTNEXTLINE(performance-noexcept-swap): locking may throw.
    void swap(KvStore& other);

    struct Entry {
        std::string key;
        std::string value;
        // Absolute Unix epoch milliseconds. 0 means no expiry.
        int64_t expiry_ms = 0;
    };

    // Export only the latest live version of each key.  TTLs are converted to
    // absolute wall-clock timestamps under the same lock used by mutations.
    std::vector<Entry> export_all() const;

    // Restore a single RDB entry using an absolute Unix epoch millisecond
    // timestamp.  RDB state predates the WAL, so restored versions use the
    // synthetic commit LSN 0.  Expired entries are discarded.  This is
    // deliberately separate from set()+expire() so recovery does not extend a
    // TTL.
    bool restore_entry(const std::string& key, const std::string& value,
                       int64_t absolute_expiry_ms);

    // Largest commit LSN applied through apply_committed() (or synthesized by
    // the legacy helpers).
    std::uint64_t current_lsn() const;

    // Last committed LSN for a key, including tombstones.  Returns 0 when no
    // version metadata remains.
    std::uint64_t latest_committed_lsn(const std::string& key) const;

    std::size_t version_count() const;
    std::size_t max_versions() const;

    struct GcResult {
        std::size_t versions_before = 0;
        std::size_t versions_after = 0;
        std::size_t versions_reclaimed = 0;
    };

    // Reclaims versions that cannot affect any supplied active snapshot or the
    // latest committed state.  The caller includes every active and committing
    // transaction snapshot.  A failure may have safely reclaimed earlier keys
    // but never changes the logical result of a listed snapshot.
    bool garbage_collect(const std::vector<std::uint64_t>& snapshot_lsns,
                         GcResult* result = nullptr,
                         std::string* error = nullptr);

    std::shared_ptr<ClockInterface> clock() const {
        return clock_;
    }

    // Periodic TTL reaper.  A scanner thread is shared by all owners of the
    // same KvStore.  ScannerLease is a move-only RAII handle: releases are
    // reference counted, and the thread stops only after the last lease (and
    // the legacy manual start_scanner() owner) is released.
    class ScannerLease {
      public:
        ScannerLease() noexcept = default;
        ~ScannerLease();
        ScannerLease(const ScannerLease&) = delete;
        ScannerLease& operator=(const ScannerLease&) = delete;
        ScannerLease(ScannerLease&& other) noexcept;
        ScannerLease& operator=(ScannerLease&& other) noexcept;

        explicit operator bool() const noexcept {
            return store_ != nullptr;
        }
        void reset() noexcept;

      private:
        friend class KvStore;
        explicit ScannerLease(KvStore* store) noexcept : store_(store) {}

        KvStore* store_ = nullptr;
    };

    ScannerLease acquire_scanner(
        std::chrono::milliseconds interval = std::chrono::seconds(1));
    // Compatibility/manual-owner API.  stop_scanner() does not force a stop
    // while scanner leases are still active.
    void
    start_scanner(std::chrono::milliseconds interval = std::chrono::seconds(1));
    void stop_scanner();
    bool scanner_running() const;

    // Blocks scanner sweeps and waits for an in-flight sweep to finish.  The
    // returned move-only guard keeps the pause active until it is destroyed.
    // RdbManager holds this guard from before the snapshot point through the
    // final directory-fsync result, so expiry reaping cannot mutate the Store
    // while a stop-the-world SAVE is in progress.
    class ScannerPause {
      public:
        ScannerPause() noexcept = default;
        ~ScannerPause() = default;
        ScannerPause(const ScannerPause&) = delete;
        ScannerPause& operator=(const ScannerPause&) = delete;
        ScannerPause(ScannerPause&&) noexcept = default;
        ScannerPause& operator=(ScannerPause&&) noexcept = default;

      private:
        friend class KvStore;
        explicit ScannerPause(std::mutex& mutex) : lock_(mutex) {}

        std::unique_lock<std::mutex> lock_;
    };

    ScannerPause pause_scanner() const;
    size_t sweep_expired();

  private:
    struct Version {
        std::uint64_t commit_lsn = 0;
        std::string value;
        std::chrono::steady_clock::time_point deadline;
        bool has_ttl = false;
        bool deleted = false;
    };

    using VersionChain = std::vector<Version>; // commit_lsn ascending
    using InternalMap = std::unordered_map<std::string, VersionChain>;

    std::chrono::steady_clock::time_point now() const;
    int64_t system_now_ms() const;
    bool is_expired(const Version& version,
                    std::chrono::steady_clock::time_point current) const;
    bool is_live(const Version& version,
                 std::chrono::steady_clock::time_point current) const;
    const Version* visible_version_locked(const std::string& key,
                                          std::uint64_t snapshot_lsn) const;
    const Version* latest_version_locked(const std::string& key) const;
    std::uint64_t next_synthetic_lsn_locked();
    bool append_version_locked(const std::string& key, Version version,
                               std::string* error);
    bool append_synthetic_version(const std::string& key, Version version);
    bool retire_expired_chain_locked(
        VersionChain& chain,
        std::chrono::steady_clock::time_point current) const;
    size_t
    purge_expired_locked(std::chrono::steady_clock::time_point current) const;
    void scanner_loop();

    std::shared_ptr<ClockInterface> clock_;
    mutable std::mutex mutex_;
    mutable InternalMap versions_;
    mutable std::uint64_t max_applied_lsn_ = 0;
    mutable std::size_t version_count_ = 0;
    std::size_t max_versions_ = DEFAULT_MAX_VERSIONS;

    void ensure_scanner_locked(std::chrono::milliseconds interval);
    void stop_scanner_locked(std::thread* thread);
    void release_scanner() noexcept;

    // Serializes complete scanner start/stop transitions.  scanner_mutex_ only
    // guards the thread state observed by scanner_loop().
    mutable std::mutex scanner_lifecycle_mutex_;
    mutable std::mutex scanner_mutex_;
    // Serializes scanner sweeps with SAVE.  It is deliberately separate from
    // scanner_mutex_ so the scanner can sleep without holding this barrier.
    mutable std::mutex scanner_pause_mutex_;
    std::condition_variable scanner_cv_;
    std::thread scanner_thread_;
    bool scanner_stop_ = false;
    bool scanner_running_ = false;
    bool manual_scanner_owner_ = false;
    size_t scanner_lease_count_ = 0;
    std::chrono::milliseconds scanner_interval_{1000};
};

} // namespace minidb
