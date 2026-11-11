#pragma once

#include "../storage/PersistenceManager.h"

#include <sstream>
#include <string>

namespace minidb {

// Stable, non-sensitive diagnostic payload shared by HTTP GET /stats and the
// KV STATS command.  Every field comes from one PersistenceSnapshot taken
// under the persistence mutex.
inline std::string persistence_stats_json(const PersistenceSnapshot& stats) {
    std::ostringstream out;
    out << "{\"durability_mode\":\""
        << durability_mode_name(stats.durability_mode)
        << "\",\"opened\":" << (stats.opened ? "true" : "false")
        << ",\"failed\":" << (stats.failed ? "true" : "false")
        << ",\"last_lsn\":" << stats.last_lsn
        << ",\"next_lsn\":" << stats.next_lsn
        << ",\"wal_bytes\":" << stats.wal_bytes
        << ",\"wal_segment_count\":" << stats.wal_segment_count
        << ",\"transactions\":{\"active\":" << stats.transactions.active
        << ",\"started\":" << stats.transactions.started
        << ",\"committed\":" << stats.transactions.committed
        << ",\"conflicted\":" << stats.transactions.conflicted
        << ",\"rolled_back\":" << stats.transactions.rolled_back
        << ",\"failed\":" << stats.transactions.failed
        << ",\"write_conflicts\":" << stats.transactions.write_conflicts
        << ",\"max_active\":" << stats.transactions.max_active
        << ",\"versions_current\":" << stats.transactions.versions_current
        << ",\"versions_max\":" << stats.transactions.versions_max
        << ",\"versions_reclaimed\":" << stats.transactions.versions_reclaimed
        << ",\"oldest_snapshot_lag_lsn\":"
        << stats.transactions.oldest_snapshot_lag_lsn
        << ",\"max_oldest_snapshot_lag_lsn\":"
        << stats.transactions.max_oldest_snapshot_lag_lsn
        << ",\"commit_records\":" << stats.transactions.commit_records
        << ",\"commit_bytes\":" << stats.transactions.commit_bytes
        << ",\"commit_total_us\":" << stats.transactions.commit_total_us
        << ",\"commit_max_us\":" << stats.transactions.commit_max_us
        << ",\"gc_total_us\":" << stats.transactions.gc_total_us
        << ",\"gc_max_us\":" << stats.transactions.gc_max_us
        << "},\"persistence\":{\"mutations\":" << stats.persistence.mutations
        << ",\"sync_mutations\":" << stats.persistence.sync_mutations
        << ",\"interval_mutations\":" << stats.persistence.interval_mutations
        << ",\"checkpoints\":" << stats.persistence.checkpoints
        << ",\"interval_flushes\":" << stats.persistence.interval_flushes
        << ",\"checkpoint_pause_us\":" << stats.persistence.checkpoint_pause_us
        << ",\"checkpoint_admission_blocked_total_us\":"
        << stats.persistence.checkpoint_admission_blocked_total_us
        << ",\"checkpoint_admission_blocked_max_us\":"
        << stats.persistence.checkpoint_admission_blocked_max_us
        << ",\"checkpoint_snapshot_capture_total_us\":"
        << stats.persistence.checkpoint_snapshot_capture_total_us
        << ",\"checkpoint_snapshot_capture_max_us\":"
        << stats.persistence.checkpoint_snapshot_capture_max_us
        << ",\"checkpoint_snapshot_serialize_total_us\":"
        << stats.persistence.checkpoint_snapshot_serialize_total_us
        << ",\"checkpoint_snapshot_serialize_max_us\":"
        << stats.persistence.checkpoint_snapshot_serialize_max_us
        << ",\"checkpoint_snapshot_write_total_us\":"
        << stats.persistence.checkpoint_snapshot_write_total_us
        << ",\"checkpoint_snapshot_write_max_us\":"
        << stats.persistence.checkpoint_snapshot_write_max_us
        << ",\"checkpoint_snapshot_fsync_total_us\":"
        << stats.persistence.checkpoint_snapshot_fsync_total_us
        << ",\"checkpoint_snapshot_fsync_max_us\":"
        << stats.persistence.checkpoint_snapshot_fsync_max_us
        << ",\"checkpoint_manifest_total_us\":"
        << stats.persistence.checkpoint_manifest_total_us
        << ",\"checkpoint_wal_cleanup_total_us\":"
        << stats.persistence.checkpoint_wal_cleanup_total_us
        << ",\"checkpoint_total_us\":" << stats.persistence.checkpoint_total_us
        << ",\"checkpoint_max_us\":" << stats.persistence.checkpoint_max_us
        << ",\"checkpoint_failures\":" << stats.persistence.checkpoint_failures
        << ",\"background_checkpoint_state\":\""
        << background_checkpoint_state_name(
               stats.persistence.background_checkpoint_state)
        << "\",\"save_client_latency_total_us\":"
        << stats.persistence.save_client_latency_total_us
        << ",\"save_client_latency_max_us\":"
        << stats.persistence.save_client_latency_max_us
        << "},\"wal\":{\"appends\":" << stats.wal.appends
        << ",\"bytes_written\":" << stats.wal.bytes_written
        << ",\"data_fsyncs\":" << stats.wal.data_fsyncs
        << ",\"directory_fsyncs\":" << stats.wal.directory_fsyncs
        << ",\"rotations\":" << stats.wal.rotations
        << ",\"segments_removed\":" << stats.wal.segments_removed
        << "},\"timing\":{\"wal_append_total_us\":" << stats.wal.append_total_us
        << ",\"wal_append_max_us\":" << stats.wal.append_max_us
        << ",\"wal_data_fsync_total_us\":" << stats.wal.data_fsync_total_us
        << ",\"wal_data_fsync_max_us\":" << stats.wal.data_fsync_max_us
        << ",\"sync_mutation_total_us\":"
        << stats.persistence.sync_mutation_total_us
        << ",\"sync_mutation_max_us\":"
        << stats.persistence.sync_mutation_max_us
        << ",\"fsync_latency_bucket_bounds_us\":[";
    for (std::size_t index = 0; index < kWalDataFsyncLatencyBoundsUs.size();
         ++index) {
        if (index != 0) {
            out << ',';
        }
        out << kWalDataFsyncLatencyBoundsUs[index];
    }
    out << "],\"fsync_latency_bucket_counts\":[";
    for (std::size_t index = 0;
         index < stats.wal.data_fsync_latency_buckets.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        out << stats.wal.data_fsync_latency_buckets[index];
    }
    out << "]},\"group_commit\":{\"queue_current_entries\":"
        << stats.group_commit.queue_current_entries
        << ",\"queue_max_entries\":" << stats.group_commit.queue_max_entries
        << ",\"queue_current_bytes\":" << stats.group_commit.queue_current_bytes
        << ",\"queue_max_bytes\":" << stats.group_commit.queue_max_bytes
        << ",\"queue_rejections\":" << stats.group_commit.queue_rejections
        << ",\"batches\":" << stats.group_commit.batches
        << ",\"batch_records\":" << stats.group_commit.batch_records
        << ",\"batch_bytes\":" << stats.group_commit.batch_bytes
        << ",\"max_batch_records\":" << stats.group_commit.max_batch_records
        << ",\"max_batch_bytes\":" << stats.group_commit.max_batch_bytes
        << ",\"batch_size_bucket_bounds\":[";
    for (std::size_t index = 0; index < kGroupCommitBatchSizeBounds.size();
         ++index) {
        if (index != 0) {
            out << ',';
        }
        out << kGroupCommitBatchSizeBounds[index];
    }
    out << "],\"batch_size_bucket_counts\":[";
    for (std::size_t index = 0;
         index < stats.group_commit.batch_size_bucket_counts.size(); ++index) {
        if (index != 0) {
            out << ',';
        }
        out << stats.group_commit.batch_size_bucket_counts[index];
    }
    out << "],\"data_fsyncs\":" << stats.group_commit.data_fsyncs
        << ",\"writer_loop_total_us\":"
        << stats.group_commit.writer_loop_total_us
        << ",\"writer_loop_max_us\":" << stats.group_commit.writer_loop_max_us
        << ",\"submit_to_fsync_total_us\":"
        << stats.group_commit.submit_to_fsync_total_us
        << ",\"submit_to_fsync_max_us\":"
        << stats.group_commit.submit_to_fsync_max_us
        << ",\"fsync_to_apply_total_us\":"
        << stats.group_commit.fsync_to_apply_total_us
        << ",\"fsync_to_apply_max_us\":"
        << stats.group_commit.fsync_to_apply_max_us
        << ",\"completion_backlog_current\":"
        << stats.group_commit.completion_backlog_current
        << ",\"completion_backlog_max\":"
        << stats.group_commit.completion_backlog_max
        << ",\"completion_backlog_current_bytes\":"
        << stats.group_commit.completion_backlog_current_bytes
        << ",\"completion_backlog_max_bytes\":"
        << stats.group_commit.completion_backlog_max_bytes
        << ",\"writer_failures\":" << stats.group_commit.writer_failures
        << ",\"append_failures\":" << stats.group_commit.append_failures
        << ",\"fsync_failures\":" << stats.group_commit.fsync_failures
        << ",\"writer_state\":\""
        << group_commit_writer_state_name(stats.group_commit.writer_state)
        << "\"},\"recovery\":{\"rdb_records\":" << stats.recovery.rdb_records
        << ",\"checkpoint_lsn\":" << stats.recovery.checkpoint_lsn
        << ",\"last_lsn\":" << stats.recovery.last_lsn
        << ",\"next_lsn\":" << stats.recovery.next_lsn
        << ",\"wal_records_scanned\":" << stats.recovery.wal_records_scanned
        << ",\"wal_records_replayed\":" << stats.recovery.wal_records_replayed
        << ",\"wal_bytes\":" << stats.recovery.wal_bytes
        << ",\"truncated_bytes\":" << stats.recovery.truncated_bytes
        << ",\"wal_segments\":" << stats.recovery.wal_segments
        << ",\"ignored_residue_segments\":"
        << stats.recovery.ignored_residue_segments << ",\"legacy_import\":"
        << (stats.recovery.legacy_import ? "true" : "false")
        << ",\"needs_bootstrap\":"
        << (stats.recovery.needs_bootstrap ? "true" : "false")
        << ",\"manifest_present\":"
        << (stats.recovery.manifest_present ? "true" : "false") << "}}";
    return out.str();
}

} // namespace minidb
