#include "GroupCommitWriter.h"

#include "../base/Log.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace minidb {
namespace {

constexpr std::uint64_t kGroupCommitEntryOverheadBytes = 128;
constexpr std::uint64_t kGroupCommitBarrierBytes = 64;
constexpr std::size_t kMaxPendingBarriers = 8;

std::uint64_t steady_now_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
} // namespace

const char* group_commit_writer_state_name(GroupCommitWriterState state) {
    switch (state) {
    case GroupCommitWriterState::kDisabled:
        return "disabled";
    case GroupCommitWriterState::kRunning:
        return "running";
    case GroupCommitWriterState::kPaused:
        return "paused";
    case GroupCommitWriterState::kDraining:
        return "draining";
    case GroupCommitWriterState::kStopped:
        return "stopped";
    case GroupCommitWriterState::kFailed:
        return "failed";
    }
    return "unknown";
}

GroupCommitWriter::GroupCommitWriter(WalManager* wal, DurabilityMode mode,
                                     OwnerDispatch owner_dispatch,
                                     DrainCallback drain_callback,
                                     GroupCommitLimits limits)
    : wal_(wal), mode_(mode), owner_dispatch_(std::move(owner_dispatch)),
      drain_callback_(std::move(drain_callback)), limits_(limits) {
    if (wal_ == nullptr) {
        throw std::invalid_argument("GroupCommitWriter requires a WAL manager");
    }
    if (!owner_dispatch_ || !drain_callback_) {
        throw std::invalid_argument(
            "GroupCommitWriter requires owner dispatch and drain callbacks");
    }
    if (limits_.max_queue_entries == 0 || limits_.max_queue_bytes == 0 ||
        limits_.max_batch_records == 0 || limits_.max_batch_bytes == 0 ||
        limits_.max_completion_entries == 0 ||
        limits_.max_completion_bytes == 0) {
        throw std::invalid_argument(
            "GroupCommitWriter limits must be positive");
    }
}

GroupCommitWriter::~GroupCommitWriter() {
    stop();
}

bool GroupCommitWriter::start(std::uint64_t first_lsn, std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable()) {
        if (error != nullptr) {
            *error = "group commit writer has already been started";
        }
        return false;
    }
    if (state_ != GroupCommitWriterState::kDisabled &&
        state_ != GroupCommitWriterState::kStopped) {
        if (error != nullptr) {
            *error = "group commit writer is not startable";
        }
        return false;
    }
    if (first_lsn == 0) {
        if (error != nullptr) {
            *error = "group commit first LSN must be positive";
        }
        return false;
    }

    queue_.clear();
    completions_.clear();
    emergency_completions_.clear();
    accepting_ = true;
    stopping_ = false;
    drain_scheduled_ = false;
    queued_mutations_ = 0;
    pending_barriers_ = 0;
    next_lsn_ = first_lsn;
    queue_bytes_ = 0;
    last_error_.clear();
    stats_.queue_current_entries = 0;
    stats_.queue_current_bytes = 0;
    stats_.completion_backlog_current = 0;
    stats_.completion_backlog_current_bytes = 0;
    set_state_locked(GroupCommitWriterState::kRunning);
    refresh_wal_snapshot_locked();

    try {
        thread_ = std::thread([this]() { run(); });
    } catch (const std::exception& ex) {
        accepting_ = false;
        set_state_locked(GroupCommitWriterState::kFailed, ex.what());
        if (error != nullptr) {
            *error = ex.what();
        }
        return false;
    }
    return true;
}

bool GroupCommitWriter::submit(GroupCommitEntry entry, std::string* error) {
    if (!WalFormat::validate(entry.record, error)) {
        return false;
    }

    const std::uint64_t bytes = command_bytes(entry);
    if (bytes == std::numeric_limits<std::uint64_t>::max()) {
        if (error != nullptr) {
            *error = "group commit entry byte count overflows";
        }
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != GroupCommitWriterState::kRunning || !accepting_) {
        ++stats_.queue_rejections;
        if (error != nullptr) {
            *error = "group commit writer is not accepting mutations";
        }
        return false;
    }
    if (entry.record.lsn != next_lsn_) {
        ++stats_.queue_rejections;
        if (error != nullptr) {
            *error = "group commit mutation LSN is not the next admitted LSN";
        }
        return false;
    }
    if (entry.record.lsn == std::numeric_limits<std::uint64_t>::max()) {
        ++stats_.queue_rejections;
        if (error != nullptr) {
            *error = "group commit LSN space is exhausted";
        }
        return false;
    }
    if (queued_mutations_ >= limits_.max_queue_entries ||
        bytes > limits_.max_queue_bytes - queue_bytes_) {
        ++stats_.queue_rejections;
        if (error != nullptr) {
            *error = "group commit admission queue is full";
        }
        return false;
    }

    Command command;
    command.kind = CommandKind::kMutation;
    command.bytes = bytes;
    if (entry.submitted_us == 0) {
        entry.submitted_us = now_us();
    }
    command.entry = std::move(entry);
    queue_.push_back(std::move(command));
    ++queued_mutations_;
    ++next_lsn_;
    queue_bytes_ += bytes;
    stats_.queue_current_entries = queued_mutations_;
    stats_.queue_current_bytes = queue_bytes_;
    stats_.queue_max_entries =
        std::max(stats_.queue_max_entries, stats_.queue_current_entries);
    stats_.queue_max_bytes =
        std::max(stats_.queue_max_bytes, stats_.queue_current_bytes);
    condition_.notify_one();
    return true;
}

bool GroupCommitWriter::enqueue_barrier_locked(CommandKind kind,
                                               BarrierCompletion completion,
                                               std::string* error) {
    if (pending_barriers_ >= kMaxPendingBarriers) {
        ++stats_.queue_rejections;
        if (error != nullptr) {
            *error = "too many pending group commit barriers";
        }
        return false;
    }

    Command command;
    command.kind = kind;
    command.bytes = kGroupCommitBarrierBytes;
    command.barrier_completion = std::move(completion);
    queue_.push_back(std::move(command));
    ++pending_barriers_;
    condition_.notify_one();
    return true;
}

bool GroupCommitWriter::request_flush(BarrierCompletion completion,
                                      std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != GroupCommitWriterState::kRunning || !accepting_) {
        if (error != nullptr) {
            *error = "group commit writer cannot accept a flush barrier";
        }
        return false;
    }
    return enqueue_barrier_locked(CommandKind::kFlush, std::move(completion),
                                  error);
}

bool GroupCommitWriter::request_pause(BarrierCompletion completion,
                                      std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != GroupCommitWriterState::kRunning || !accepting_) {
        if (error != nullptr) {
            *error = "group commit writer cannot pause in its current state";
        }
        return false;
    }
    if (!enqueue_barrier_locked(CommandKind::kPause, std::move(completion),
                                error)) {
        return false;
    }
    // Pause is an admission cut.  Every mutation accepted before this call is
    // already ahead of the barrier in the command queue.
    accepting_ = false;
    return true;
}

bool GroupCommitWriter::enqueue_wal_barrier_locked(
    CommandKind kind, WalBarrierCompletion completion, std::string* error) {
    if (pending_barriers_ >= kMaxPendingBarriers) {
        ++stats_.queue_rejections;
        if (error != nullptr) {
            *error = "too many pending group commit barriers";
        }
        return false;
    }

    Command command;
    command.kind = kind;
    command.bytes = kGroupCommitBarrierBytes;
    command.wal_barrier_completion = std::move(completion);
    queue_.push_back(std::move(command));
    ++pending_barriers_;
    condition_.notify_one();
    return true;
}

bool GroupCommitWriter::request_rotate(WalBarrierCompletion completion,
                                       std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != GroupCommitWriterState::kPaused) {
        if (error != nullptr) {
            *error = "group commit writer must be paused before rotate";
        }
        return false;
    }
    return enqueue_wal_barrier_locked(CommandKind::kRotate,
                                      std::move(completion), error);
}

bool GroupCommitWriter::request_cleanup(std::uint64_t first_retained_start_lsn,
                                        BarrierCompletion completion,
                                        std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != GroupCommitWriterState::kPaused) {
        if (error != nullptr) {
            *error = "group commit writer must be paused before cleanup";
        }
        return false;
    }
    if (first_retained_start_lsn == 0) {
        if (error != nullptr) {
            *error = "group commit cleanup boundary must be positive";
        }
        return false;
    }
    if (pending_barriers_ >= kMaxPendingBarriers) {
        ++stats_.queue_rejections;
        if (error != nullptr) {
            *error = "too many pending group commit barriers";
        }
        return false;
    }
    Command command;
    command.kind = CommandKind::kCleanup;
    command.bytes = kGroupCommitBarrierBytes;
    command.cleanup_boundary_lsn = first_retained_start_lsn;
    command.barrier_completion = std::move(completion);
    queue_.push_back(std::move(command));
    ++pending_barriers_;
    condition_.notify_one();
    return true;
}

bool GroupCommitWriter::resume(std::string* error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != GroupCommitWriterState::kPaused) {
        if (error != nullptr) {
            *error = "group commit writer is not paused";
        }
        return false;
    }
    if (pending_barriers_ != 0) {
        if (error != nullptr) {
            *error = "group commit writer still has pending barriers";
        }
        return false;
    }
    accepting_ = true;
    set_state_locked(GroupCommitWriterState::kRunning);
    condition_.notify_one();
    return true;
}

void GroupCommitWriter::stop() {
    std::thread thread_to_join;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == GroupCommitWriterState::kDisabled &&
            !thread_.joinable()) {
            return;
        }
        accepting_ = false;
        stopping_ = true;
        if (state_ != GroupCommitWriterState::kFailed) {
            set_state_locked(GroupCommitWriterState::kDraining);
        }
        condition_.notify_all();
        if (thread_.joinable() &&
            thread_.get_id() != std::this_thread::get_id()) {
            thread_to_join = std::move(thread_);
        }
    }
    if (thread_to_join.joinable()) {
        thread_to_join.join();
    }
}

void GroupCommitWriter::run() {
    while (true) {
        std::vector<GroupCommitEntry> batch;
        Command barrier;
        bool has_barrier = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this]() {
                return !queue_.empty() || stopping_ ||
                       state_ == GroupCommitWriterState::kFailed;
            });
            if (state_ == GroupCommitWriterState::kFailed) {
                break;
            }
            if (queue_.empty()) {
                if (stopping_) {
                    set_state_locked(GroupCommitWriterState::kStopped);
                    break;
                }
                continue;
            }
            if (state_ == GroupCommitWriterState::kPaused && !stopping_ &&
                queue_.front().kind == CommandKind::kMutation) {
                // A mutation cannot legitimately follow the pause barrier.
                // Leaving it queued would spin the writer and strand callers;
                // treat the ordering violation as fail-stop instead.
                fail_stop_locked(
                    "paused group commit writer found a queued mutation");
                break;
            }

            if (queue_.front().kind == CommandKind::kMutation) {
                std::uint64_t batch_bytes = 0;
                while (!queue_.empty() &&
                       queue_.front().kind == CommandKind::kMutation) {
                    Command command = std::move(queue_.front());
                    queue_.pop_front();
                    --queued_mutations_;
                    queue_bytes_ -= command.bytes;
                    batch_bytes += command.bytes;
                    batch.push_back(std::move(command.entry));
                    if (batch.size() >= limits_.max_batch_records ||
                        batch_bytes >= limits_.max_batch_bytes) {
                        break;
                    }
                }
                stats_.queue_current_entries = queued_mutations_;
                stats_.queue_current_bytes = queue_bytes_;
            } else {
                barrier = std::move(queue_.front());
                queue_.pop_front();
                --pending_barriers_;
                has_barrier = true;
            }
        }

        if (has_barrier) {
            if (barrier.kind == CommandKind::kRotate ||
                barrier.kind == CommandKind::kCleanup) {
                process_wal_barrier(std::move(barrier));
            } else {
                process_barrier(std::move(barrier));
            }
            continue;
        }

        const std::uint64_t started_us = now_us();
        GroupCommitCompletion completion = process_batch(batch);
        const bool failed_batch = !completion.success;
        const std::string batch_error = completion.error;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            observe_timing(now_us() - started_us, &stats_.writer_loop_total_us,
                           &stats_.writer_loop_max_us);
            refresh_wal_snapshot_locked();
            publish_locked(std::move(completion));
            if (failed_batch) {
                fail_stop_locked(batch_error);
            }
        }
    }
}

GroupCommitCompletion
GroupCommitWriter::process_batch(std::vector<GroupCommitEntry>& batch) {
    GroupCommitCompletion completion;
    completion.kind = GroupCommitCompletion::Kind::kRecords;
    completion.logical_entries = batch.size();
    completion.records.reserve(batch.size());

    std::vector<WalRecord> records;
    records.reserve(batch.size());
    std::uint64_t batch_bytes = 0;
    for (auto& entry : batch) {
        completion.bytes += command_bytes(entry);
        records.push_back(entry.record);
        GroupCommitRecordResult result;
        result.record = std::move(entry.record);
        result.submitted_us = entry.submitted_us;
        result.completion = std::move(entry.completion);
        completion.records.push_back(std::move(result));
        const std::uint64_t wire_bytes =
            WalFormat::encoded_size(completion.records.back().record);
        batch_bytes += wire_bytes;
    }

    const WalManagerStats before = wal_->stats();
    std::string error;
    const WalResult status = wal_->append_batch(records, mode_, &error);
    const std::uint64_t completed_us = now_us();
    const WalManagerStats after = wal_->stats();
    const WalFailureKind failure_kind = wal_->last_failure_kind();

    if (status == WalResult::kOk) {
        completion.success = true;
        for (GroupCommitRecordResult& result : completion.records) {
            result.status = WalResult::kOk;
            result.written = true;
            result.fsync_completed_us =
                mode_ == DurabilityMode::kSync ? completed_us : 0;
        }
    } else {
        completion.success = false;
        completion.error = error.empty() ? "WAL group append failed" : error;
        const std::uint64_t written_prefix_lsn = wal_->next_lsn();
        for (GroupCommitRecordResult& result : completion.records) {
            result.status = status;
            result.written = result.record.lsn < written_prefix_lsn;
            result.error = completion.error;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    add_saturated(&stats_.data_fsyncs, after.data_fsyncs - before.data_fsyncs);
    if (status == WalResult::kOk) {
        ++stats_.batches;
        add_saturated(&stats_.batch_records, completion.records.size());
        add_saturated(&stats_.batch_bytes, batch_bytes);
        stats_.max_batch_records = std::max<std::uint64_t>(
            stats_.max_batch_records, completion.records.size());
        stats_.max_batch_bytes = std::max(stats_.max_batch_bytes, batch_bytes);
        ++stats_.batch_size_bucket_counts[batch_bucket(
            completion.records.size())];
        if (mode_ == DurabilityMode::kSync) {
            for (const GroupCommitRecordResult& result : completion.records) {
                if (result.submitted_us != 0 &&
                    result.fsync_completed_us >= result.submitted_us) {
                    observe_timing(result.fsync_completed_us -
                                       result.submitted_us,
                                   &stats_.submit_to_fsync_total_us,
                                   &stats_.submit_to_fsync_max_us);
                }
            }
        }
    } else {
        ++stats_.append_failures;
        if (failure_kind == WalFailureKind::kFsync) {
            ++stats_.fsync_failures;
        }
    }
    return completion;
}

void GroupCommitWriter::process_barrier(Command command) {
    const std::uint64_t started_us = now_us();
    bool success = true;
    std::string error;
    if (mode_ == DurabilityMode::kInterval &&
        command.kind != CommandKind::kMutation) {
        const WalManagerStats before = wal_->stats();
        success = wal_->flush(&error);
        const WalManagerStats after = wal_->stats();
        std::lock_guard<std::mutex> lock(mutex_);
        add_saturated(&stats_.data_fsyncs,
                      after.data_fsyncs - before.data_fsyncs);
        if (!success) {
            ++stats_.fsync_failures;
        }
    }

    GroupCommitCompletion completion;
    completion.kind = GroupCommitCompletion::Kind::kBarrier;
    completion.success = success;
    completion.error = success ? std::string() : error;
    completion.bytes = kGroupCommitBarrierBytes;
    completion.logical_entries = 1;
    completion.barrier_completion = std::move(command.barrier_completion);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        observe_timing(now_us() - started_us, &stats_.writer_loop_total_us,
                       &stats_.writer_loop_max_us);
        if (success && command.kind == CommandKind::kPause) {
            set_state_locked(GroupCommitWriterState::kPaused);
        }
        refresh_wal_snapshot_locked();
        publish_locked(std::move(completion));
        if (!success) {
            fail_stop_locked(error);
        }
    }
}

void GroupCommitWriter::process_wal_barrier(Command command) {
    const std::uint64_t started_us = now_us();
    GroupCommitWalBarrierInfo info;
    std::string error;
    const WalManagerStats before = wal_->stats();

    if (command.kind == CommandKind::kRotate) {
        info.success = wal_->flush(&error);
        if (info.success) {
            const WalSegmentInfo* active = wal_->active_segment();
            if (active == nullptr) {
                info.success = false;
                error = "group commit rotate has no active WAL segment";
            } else if (active->size != 0) {
                info.success = wal_->rotate(wal_->next_lsn(), &error);
            }
        }
        if (info.success) {
            const WalSegmentInfo* active = wal_->active_segment();
            if (active == nullptr) {
                info.success = false;
                error = "group commit rotate left no active WAL segment";
            } else {
                info.next_lsn = wal_->next_lsn();
                info.active_segment_start_lsn = active->start_lsn;
                info.active_segment_bytes = active->size;
            }
        }
    } else {
        info.success =
            wal_->cleanup_before(command.cleanup_boundary_lsn, &error);
        info.next_lsn = wal_->next_lsn();
        const WalSegmentInfo* active = wal_->active_segment();
        if (active != nullptr) {
            info.active_segment_start_lsn = active->start_lsn;
            info.active_segment_bytes = active->size;
        }
    }
    if (info.success) {
        info.live_segment_starts.clear();
        info.live_segment_starts.reserve(wal_->segments().size());
        for (const WalSegmentInfo& segment : wal_->segments()) {
            info.live_segment_starts.push_back(segment.start_lsn);
        }
    }
    if (!info.success) {
        info.error = error.empty() ? "group commit WAL control failed" : error;
    }

    const WalManagerStats after = wal_->stats();
    GroupCommitCompletion completion;
    completion.kind = command.kind == CommandKind::kRotate
                          ? GroupCommitCompletion::Kind::kWalBarrier
                          : GroupCommitCompletion::Kind::kBarrier;
    completion.success = info.success;
    completion.error = info.error;
    completion.bytes = kGroupCommitBarrierBytes;
    completion.logical_entries = 1;
    if (command.kind == CommandKind::kRotate) {
        completion.wal_barrier = info;
        completion.wal_barrier_completion =
            std::move(command.wal_barrier_completion);
    } else {
        completion.barrier_completion = std::move(command.barrier_completion);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        add_saturated(&stats_.data_fsyncs,
                      after.data_fsyncs - before.data_fsyncs);
        observe_timing(now_us() - started_us, &stats_.writer_loop_total_us,
                       &stats_.writer_loop_max_us);
        refresh_wal_snapshot_locked();
        publish_locked(std::move(completion));
        if (!info.success && command.kind == CommandKind::kRotate) {
            ++stats_.append_failures;
            fail_stop_locked(info.error);
        }
        // A cleanup failure after a durable manifest leaves the checkpoint
        // valid.  Keep the writer paused so the owner can retry cleanup on a
        // later checkpoint; the failed barrier is still reported exactly
        // once and no accepted mutation may be stranded.
    }
}

std::uint64_t
GroupCommitWriter::completion_bytes(const GroupCommitCompletion& completion) {
    std::uint64_t bytes = 64;
    for (const GroupCommitRecordResult& result : completion.records) {
        const std::uint64_t wire_bytes = WalFormat::encoded_size(result.record);
        if (wire_bytes > std::numeric_limits<std::uint64_t>::max() - bytes ||
            kGroupCommitEntryOverheadBytes >
                std::numeric_limits<std::uint64_t>::max() - bytes -
                    wire_bytes) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        bytes += wire_bytes + kGroupCommitEntryOverheadBytes;
    }
    return bytes;
}

void GroupCommitWriter::publish_locked(GroupCommitCompletion completion) {
    std::uint64_t bytes = completion_bytes(completion);
    if (bytes == std::numeric_limits<std::uint64_t>::max()) {
        emergency_completions_.push_back(std::move(completion));
        fail_stop_locked("group commit completion byte count overflows");
        schedule_drain_locked();
        return;
    }

    const bool entries_exceeded =
        completion.logical_entries >
        limits_.max_completion_entries -
            std::min<std::uint64_t>(stats_.completion_backlog_current,
                                    limits_.max_completion_entries);
    const bool bytes_exceeded =
        bytes >
        limits_.max_completion_bytes -
            std::min<std::uint64_t>(stats_.completion_backlog_current_bytes,
                                    limits_.max_completion_bytes);
    if (entries_exceeded || bytes_exceeded) {
        // A single emergency completion is allowed to exceed the normal
        // backlog bound, but it contains only callbacks that were already
        // admitted and is therefore bounded by the admission queue plus one
        // physical batch.
        emergency_completions_.push_back(std::move(completion));
        fail_stop_locked("group commit completion backlog limit exceeded");
        schedule_drain_locked();
        return;
    }

    const std::uint64_t next_entries =
        stats_.completion_backlog_current + completion.logical_entries;
    const std::uint64_t next_bytes =
        stats_.completion_backlog_current_bytes + bytes;
    completions_.push_back(std::move(completion));
    stats_.completion_backlog_current = next_entries;
    stats_.completion_backlog_current_bytes = next_bytes;
    stats_.completion_backlog_max =
        std::max(stats_.completion_backlog_max, next_entries);
    stats_.completion_backlog_max_bytes =
        std::max(stats_.completion_backlog_max_bytes, next_bytes);
    schedule_drain_locked();
}

GroupCommitCompletion
GroupCommitWriter::make_queued_failure_locked(const std::string& error) {
    GroupCommitCompletion aggregate;
    aggregate.kind = GroupCommitCompletion::Kind::kRecords;
    aggregate.success = false;
    aggregate.error = error;

    auto flush_aggregate = [this, &aggregate]() {
        if (aggregate.records.empty()) {
            return;
        }
        emergency_completions_.push_back(std::move(aggregate));
        aggregate = GroupCommitCompletion{};
        aggregate.kind = GroupCommitCompletion::Kind::kRecords;
        aggregate.success = false;
    };

    while (!queue_.empty()) {
        Command command = std::move(queue_.front());
        queue_.pop_front();
        if (command.kind == CommandKind::kMutation) {
            --queued_mutations_;
            queue_bytes_ -= command.bytes;
            GroupCommitRecordResult result;
            result.record = std::move(command.entry.record);
            result.submitted_us = command.entry.submitted_us;
            result.status = WalResult::kIoError;
            result.error = error;
            result.completion = std::move(command.entry.completion);
            aggregate.bytes += command.bytes;
            ++aggregate.logical_entries;
            aggregate.records.push_back(std::move(result));
        } else {
            flush_aggregate();
            --pending_barriers_;
            GroupCommitCompletion barrier;
            barrier.success = false;
            barrier.error = error;
            barrier.bytes = kGroupCommitBarrierBytes;
            barrier.logical_entries = 1;
            if (command.kind == CommandKind::kRotate) {
                barrier.kind = GroupCommitCompletion::Kind::kWalBarrier;
                barrier.wal_barrier.success = false;
                barrier.wal_barrier.error = error;
                barrier.wal_barrier_completion =
                    std::move(command.wal_barrier_completion);
            } else {
                barrier.kind = GroupCommitCompletion::Kind::kBarrier;
                barrier.barrier_completion =
                    std::move(command.barrier_completion);
            }
            emergency_completions_.push_back(std::move(barrier));
        }
    }
    flush_aggregate();
    stats_.queue_current_entries = 0;
    stats_.queue_current_bytes = 0;
    return aggregate;
}

void GroupCommitWriter::fail_stop_locked(const std::string& error) {
    const bool first_failure = state_ != GroupCommitWriterState::kFailed;
    accepting_ = false;
    stopping_ = true;
    if (first_failure || last_error_.empty()) {
        last_error_ = error.empty() ? "group commit writer failed" : error;
    }
    if (first_failure) {
        ++stats_.writer_failures;
        set_state_locked(GroupCommitWriterState::kFailed, last_error_);
    }

    GroupCommitCompletion queued = make_queued_failure_locked(
        last_error_.empty() ? "group commit writer failed" : last_error_);
    if (!queued.records.empty()) {
        emergency_completions_.push_back(std::move(queued));
    }
    schedule_drain_locked();
    condition_.notify_all();
}

void GroupCommitWriter::schedule_drain_locked() {
    if (drain_scheduled_ || !owner_dispatch_ ||
        (completions_.empty() && emergency_completions_.empty())) {
        return;
    }
    drain_scheduled_ = true;
    std::weak_ptr<GroupCommitWriter> weak = weak_from_this();
    try {
        owner_dispatch_([weak]() {
            if (auto self = weak.lock()) {
                self->drain();
            }
        });
    } catch (const std::exception& ex) {
        drain_scheduled_ = false;
        set_state_locked(GroupCommitWriterState::kFailed, ex.what());
    } catch (...) {
        drain_scheduled_ = false;
        set_state_locked(GroupCommitWriterState::kFailed,
                         "group commit owner dispatch failed");
    }
}

void GroupCommitWriter::drain() {
    std::vector<GroupCommitCompletion> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drain_scheduled_ = false;
        pending.reserve(completions_.size() + emergency_completions_.size());
        while (!completions_.empty()) {
            pending.push_back(std::move(completions_.front()));
            completions_.pop_front();
        }
        for (GroupCommitCompletion& completion : emergency_completions_) {
            pending.push_back(std::move(completion));
        }
        emergency_completions_.clear();
        stats_.completion_backlog_current = 0;
        stats_.completion_backlog_current_bytes = 0;
    }

    for (GroupCommitCompletion& completion : pending) {
        try {
            drain_callback_(std::move(completion));
        } catch (const std::exception& ex) {
            Log::Error(std::string("group commit drain callback threw: ") +
                       ex.what());
        } catch (...) {
            Log::Error(
                "group commit drain callback threw an unknown exception");
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!completions_.empty() || !emergency_completions_.empty()) {
        schedule_drain_locked();
    }
}

void GroupCommitWriter::set_state_locked(GroupCommitWriterState state) {
    state_ = state;
    stats_.writer_state = state;
}

void GroupCommitWriter::set_state_locked(GroupCommitWriterState state,
                                         const std::string& error) {
    last_error_ = error;
    set_state_locked(state);
}

GroupCommitStats GroupCommitWriter::stats_locked() const {
    GroupCommitStats copy = stats_;
    copy.writer_state = state_;
    copy.queue_current_entries = queued_mutations_;
    copy.queue_current_bytes = queue_bytes_;
    return copy;
}

GroupCommitStats GroupCommitWriter::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_locked();
}

GroupCommitWalSnapshot GroupCommitWriter::wal_snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    GroupCommitWalSnapshot copy = wal_snapshot_;
    copy.group_commit = stats_locked();
    copy.next_lsn = next_lsn_;
    return copy;
}

void GroupCommitWriter::refresh_wal_snapshot_locked() {
    wal_snapshot_.group_commit = stats_locked();
    wal_snapshot_.next_lsn = next_lsn_;
    wal_snapshot_.wal = wal_->stats();
    wal_snapshot_.wal_total_bytes = wal_->total_bytes();
    wal_snapshot_.wal_segment_count = wal_->segments().size();
    const WalSegmentInfo* active = wal_->active_segment();
    if (active != nullptr) {
        wal_snapshot_.active_segment_start_lsn = active->start_lsn;
        wal_snapshot_.active_segment_bytes = active->size;
    } else {
        wal_snapshot_.active_segment_start_lsn = 0;
        wal_snapshot_.active_segment_bytes = 0;
    }
    wal_snapshot_.live_segment_starts.clear();
    wal_snapshot_.live_segment_starts.reserve(wal_->segments().size());
    for (const WalSegmentInfo& segment : wal_->segments()) {
        wal_snapshot_.live_segment_starts.push_back(segment.start_lsn);
    }
}

GroupCommitWriterState GroupCommitWriter::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool GroupCommitWriter::failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == GroupCommitWriterState::kFailed;
}

std::string GroupCommitWriter::last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_error_;
}

std::uint64_t GroupCommitWriter::next_lsn() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_lsn_;
}

void GroupCommitWriter::observe_applied(const GroupCommitRecordResult& result,
                                        std::uint64_t applied_us) {
    if (result.fsync_completed_us == 0 ||
        applied_us < result.fsync_completed_us) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    observe_timing(applied_us - result.fsync_completed_us,
                   &stats_.fsync_to_apply_total_us,
                   &stats_.fsync_to_apply_max_us);
}

std::uint64_t GroupCommitWriter::command_bytes(const GroupCommitEntry& entry) {
    const std::uint64_t wire_bytes = WalFormat::encoded_size(entry.record);
    if (wire_bytes > std::numeric_limits<std::uint64_t>::max() -
                         kGroupCommitEntryOverheadBytes) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return wire_bytes + kGroupCommitEntryOverheadBytes;
}

std::uint64_t GroupCommitWriter::now_us() const {
    return steady_now_us();
}

std::size_t GroupCommitWriter::batch_bucket(std::size_t batch_records) {
    for (std::size_t index = 0; index < kGroupCommitBatchSizeBounds.size();
         ++index) {
        if (batch_records <= kGroupCommitBatchSizeBounds[index]) {
            return index;
        }
    }
    return kGroupCommitBatchSizeBounds.size();
}

void GroupCommitWriter::add_saturated(std::uint64_t* counter,
                                      std::uint64_t delta) {
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    *counter = delta > maximum - *counter ? maximum : *counter + delta;
}

void GroupCommitWriter::observe_timing(std::uint64_t elapsed_us,
                                       std::uint64_t* total_us,
                                       std::uint64_t* maximum_us) {
    add_saturated(total_us, elapsed_us);
    *maximum_us = std::max(*maximum_us, elapsed_us);
}

} // namespace minidb
