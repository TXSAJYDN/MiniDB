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
        << ",\"persistence\":{\"mutations\":" << stats.persistence.mutations
        << ",\"sync_mutations\":" << stats.persistence.sync_mutations
        << ",\"interval_mutations\":" << stats.persistence.interval_mutations
        << ",\"checkpoints\":" << stats.persistence.checkpoints
        << ",\"interval_flushes\":" << stats.persistence.interval_flushes
        << ",\"checkpoint_pause_us\":" << stats.persistence.checkpoint_pause_us
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
    out << "]},\"recovery\":{\"rdb_records\":" << stats.recovery.rdb_records
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
