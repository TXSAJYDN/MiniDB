#pragma once

#include "WalManager.h"

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace minidb {

enum class GroupCommitWriterState : std::uint8_t {
    kDisabled,
    kRunning,
    kPaused,
    kDraining,
    kStopped,
    kFailed,
};

const char* group_commit_writer_state_name(GroupCommitWriterState state);

inline constexpr std::array<std::uint64_t, 8> kGroupCommitBatchSizeBounds = {
    1, 2, 4, 8, 16, 32, 64, 128};
inline constexpr std::size_t kGroupCommitBatchSizeBucketCount =
    kGroupCommitBatchSizeBounds.size() + 1;

struct GroupCommitLimits {
    std::size_t max_queue_entries = 4096;
    std::uint64_t max_queue_bytes = std::uint64_t{64} * 1024 * 1024;
    std::size_t max_batch_records = 256;
    std::uint64_t max_batch_bytes = std::uint64_t{16} * 1024 * 1024;
    std::size_t max_completion_entries = 4096;
    std::uint64_t max_completion_bytes = std::uint64_t{64} * 1024 * 1024;
};

struct GroupCommitStats {
    std::uint64_t queue_current_entries = 0;
    std::uint64_t queue_max_entries = 0;
    std::uint64_t queue_current_bytes = 0;
    std::uint64_t queue_max_bytes = 0;
    std::uint64_t queue_rejections = 0;

    std::uint64_t batches = 0;
    std::uint64_t batch_records = 0;
    std::uint64_t batch_bytes = 0;
    std::uint64_t max_batch_records = 0;
    std::uint64_t max_batch_bytes = 0;
    std::array<std::uint64_t, kGroupCommitBatchSizeBucketCount>
        batch_size_bucket_counts{};

    std::uint64_t data_fsyncs = 0;
    std::uint64_t writer_loop_total_us = 0;
    std::uint64_t writer_loop_max_us = 0;
    std::uint64_t submit_to_fsync_total_us = 0;
    std::uint64_t submit_to_fsync_max_us = 0;
    std::uint64_t fsync_to_apply_total_us = 0;
    std::uint64_t fsync_to_apply_max_us = 0;

    std::uint64_t completion_backlog_current = 0;
    std::uint64_t completion_backlog_max = 0;
    std::uint64_t completion_backlog_current_bytes = 0;
    std::uint64_t completion_backlog_max_bytes = 0;

    std::uint64_t writer_failures = 0;
    std::uint64_t append_failures = 0;
    std::uint64_t fsync_failures = 0;
    GroupCommitWriterState writer_state = GroupCommitWriterState::kDisabled;
};

// The writer never invokes protocol or store code.  It reports a completed
// batch back to the owner loop, which applies records and calls completions.
struct GroupCommitEntry {
    WalRecord record;
    std::uint64_t submitted_us = 0;
    std::function<void(bool, const std::string&)> completion;
};

struct GroupCommitRecordResult {
    WalRecord record;
    std::uint64_t submitted_us = 0;
    std::uint64_t fsync_completed_us = 0;
    WalResult status = WalResult::kOk;
    bool written = false;
    std::string error;
    std::function<void(bool, const std::string&)> completion;

    bool ok() const noexcept {
        return status == WalResult::kOk;
    }
};

struct GroupCommitWalBarrierInfo {
    bool success = false;
    std::string error;
    std::uint64_t next_lsn = 0;
    std::uint64_t active_segment_start_lsn = 0;
    std::uint64_t active_segment_bytes = 0;
};

// One lock-protected view of the writer and its WAL.  Diagnostics must never
// combine separately-read queue, backlog, WAL-byte, and segment fields.
struct GroupCommitWalSnapshot {
    GroupCommitStats group_commit;
    WalManagerStats wal;
    std::uint64_t next_lsn = 1;
    std::uint64_t wal_total_bytes = 0;
    std::uint64_t wal_segment_count = 0;
    std::uint64_t active_segment_start_lsn = 0;
    std::uint64_t active_segment_bytes = 0;
};

struct GroupCommitCompletion {
    enum class Kind : std::uint8_t {
        kRecords,
        kBarrier,
        kWalBarrier,
    };

    Kind kind = Kind::kRecords;
    bool success = false;
    std::string error;
    std::uint64_t bytes = 0;
    std::uint64_t logical_entries = 0;
    std::vector<GroupCommitRecordResult> records;
    std::function<void(bool, const std::string&)> barrier_completion;
    std::function<void(const GroupCommitWalBarrierInfo&)>
        wal_barrier_completion;
    GroupCommitWalBarrierInfo wal_barrier;
};

// A writer instance is owned by PersistenceManager through a shared_ptr.  The
// owner dispatcher must enqueue the supplied callback for later execution; it
// must not run it inline on the writer thread.
class GroupCommitWriter
    : public std::enable_shared_from_this<GroupCommitWriter> {
  public:
    using OwnerDispatch = std::function<void(std::function<void()>)>;
    using BarrierCompletion = std::function<void(bool, const std::string&)>;
    using WalBarrierCompletion =
        std::function<void(const GroupCommitWalBarrierInfo&)>;
    using DrainCallback = std::function<void(GroupCommitCompletion)>;

    GroupCommitWriter(WalManager* wal, DurabilityMode mode,
                      OwnerDispatch owner_dispatch,
                      DrainCallback drain_callback,
                      GroupCommitLimits limits = GroupCommitLimits{});
    ~GroupCommitWriter();

    GroupCommitWriter(const GroupCommitWriter&) = delete;
    GroupCommitWriter& operator=(const GroupCommitWriter&) = delete;

    // The caller must own this object through shared_ptr before start().
    bool start(std::uint64_t first_lsn, std::string* error = nullptr);
    bool submit(GroupCommitEntry entry, std::string* error = nullptr);
    bool request_flush(BarrierCompletion completion,
                       std::string* error = nullptr);
    bool request_pause(BarrierCompletion completion,
                       std::string* error = nullptr);
    // Checkpoint controls are accepted only while paused.  They execute on
    // the writer thread so rotate/cleanup never race the WAL owner.
    bool request_rotate(WalBarrierCompletion completion,
                        std::string* error = nullptr);
    bool request_cleanup(BarrierCompletion completion,
                         std::string* error = nullptr);
    bool resume(std::string* error = nullptr);
    void stop();

    GroupCommitStats stats() const;
    GroupCommitWalSnapshot wal_snapshot() const;
    GroupCommitWriterState state() const;
    bool failed() const;
    std::string last_error() const;
    std::uint64_t next_lsn() const;

    // Called by the owner after a successful record has been applied for
    // every completed operation.  This keeps the fsync-to-apply metric at the
    // only thread that is allowed to mutate KvStore state.
    void observe_applied(const GroupCommitRecordResult& result,
                         std::uint64_t applied_us);

  private:
    enum class CommandKind : std::uint8_t {
        kMutation,
        kFlush,
        kPause,
        kRotate,
        kCleanup,
    };

    struct Command {
        CommandKind kind = CommandKind::kMutation;
        GroupCommitEntry entry;
        BarrierCompletion barrier_completion;
        WalBarrierCompletion wal_barrier_completion;
        std::uint64_t bytes = 0;
    };

    void run();
    GroupCommitCompletion process_batch(std::vector<GroupCommitEntry>& batch);
    void process_barrier(Command command);
    void process_wal_barrier(Command command);
    void publish_locked(GroupCommitCompletion completion);
    void fail_stop_locked(const std::string& error);
    GroupCommitCompletion make_queued_failure_locked(const std::string& error);
    void schedule_drain_locked();
    void drain();
    GroupCommitStats stats_locked() const;
    void refresh_wal_snapshot_locked();
    void set_state_locked(GroupCommitWriterState state);
    void set_state_locked(GroupCommitWriterState state,
                          const std::string& error);
    bool enqueue_barrier_locked(CommandKind kind, BarrierCompletion completion,
                                std::string* error);
    bool enqueue_wal_barrier_locked(CommandKind kind,
                                    WalBarrierCompletion completion,
                                    std::string* error);
    static std::uint64_t command_bytes(const GroupCommitEntry& entry);
    static std::uint64_t
    completion_bytes(const GroupCommitCompletion& completion);
    std::uint64_t now_us() const;
    static std::size_t batch_bucket(std::size_t batch_records);
    static void add_saturated(std::uint64_t* counter, std::uint64_t delta);
    static void observe_timing(std::uint64_t elapsed_us,
                               std::uint64_t* total_us,
                               std::uint64_t* maximum_us);

    WalManager* wal_ = nullptr;
    DurabilityMode mode_ = DurabilityMode::kSync;
    OwnerDispatch owner_dispatch_;
    DrainCallback drain_callback_;
    GroupCommitLimits limits_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Command> queue_;
    std::deque<GroupCommitCompletion> completions_;
    std::vector<GroupCommitCompletion> emergency_completions_;

    std::thread thread_;
    GroupCommitWriterState state_ = GroupCommitWriterState::kDisabled;
    bool accepting_ = false;
    bool stopping_ = false;
    bool drain_scheduled_ = false;
    std::size_t queued_mutations_ = 0;
    std::size_t pending_barriers_ = 0;
    std::uint64_t next_lsn_ = 1;
    std::uint64_t queue_bytes_ = 0;
    std::string last_error_;
    GroupCommitStats stats_;
    GroupCommitWalSnapshot wal_snapshot_;
};

} // namespace minidb
