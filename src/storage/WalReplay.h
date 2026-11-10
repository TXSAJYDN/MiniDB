#pragma once

#include "CheckpointManifest.h"
#include "KvStore.h"
#include "WalFileOperations.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace minidb {

enum class WalReplayStatus : std::uint8_t {
    kOk,
    kCorrupt,
    kIoError,
    kResourceLimit,
};

struct WalReplayStats {
    std::uint64_t scanned_records = 0;
    std::uint64_t replayed_records = 0;
    std::uint64_t last_lsn = 0;
    std::uint64_t next_lsn = 1;
    std::uint64_t recovered_bytes = 0;
    std::uint64_t truncated_bytes = 0;
    std::size_t segment_count = 0;
    std::size_t ignored_prefix_segments = 0;
};

// The selection step resolves the manifest's live WAL chain against the
// directory contents.  Files before the first retained start are obsolete
// residue and are ignored; every segment at or after that point participates
// in recovery.  This deliberately includes newer rotated segments that were
// not yet represented in an older manifest.
struct WalReplayPlan {
    std::vector<WalSegmentInfo> segments;
    std::size_t ignored_prefix_segments = 0;
};

WalReplayStatus
select_wal_replay_segments(const std::string& wal_dir,
                           const CheckpointManifest& manifest,
                           WalFileOperations* operations, WalReplayPlan* plan,
                           std::string* error = nullptr,
                           const WalLimits& limits = default_wal_limits());

// Validates the selected segments as one contiguous LSN history beginning
// immediately after checkpoint_lsn.  A single incomplete record at the tail of
// the final segment is truncated and fsynced; every other decode error is
// fatal.  No log at or after the live start is silently skipped.
WalReplayStatus replay_wal_segments(
    const std::vector<WalSegmentInfo>& segments, std::uint64_t checkpoint_lsn,
    KvStore& store, WalFileOperations* operations,
    WalReplayStats* stats = nullptr, std::string* error = nullptr,
    const WalLimits& limits = default_wal_limits());

// Convenience wrapper used by RecoveryManager.  Publication is the caller's
// responsibility: replay mutates only the supplied temporary KvStore.
WalReplayStatus replay_wal(const std::string& wal_dir,
                           const CheckpointManifest& manifest, KvStore& store,
                           WalFileOperations* operations,
                           WalReplayPlan* selected,
                           WalReplayStats* stats = nullptr,
                           std::string* error = nullptr,
                           const WalLimits& limits = default_wal_limits());

} // namespace minidb
