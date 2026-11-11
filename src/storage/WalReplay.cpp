#include "WalReplay.h"

#include "WalFormat.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <vector>

namespace fs = std::filesystem;

namespace minidb {
namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

KvStore::AppliedMutation to_applied_mutation(const WalRecord& record) {
    KvStore::AppliedMutation mutation;
    mutation.key = record.key;
    switch (record.op) {
    case WalOp::kSet:
        mutation.kind = KvStore::AppliedMutation::Kind::kSet;
        mutation.value = record.value;
        mutation.absolute_expiry_ms = record.expiry_ms;
        break;
    case WalOp::kDel:
        mutation.kind = KvStore::AppliedMutation::Kind::kDelete;
        break;
    case WalOp::kExpire:
        mutation.kind = KvStore::AppliedMutation::Kind::kExpire;
        mutation.absolute_expiry_ms = record.expiry_ms;
        break;
    case WalOp::kTxn:
        break;
    }
    return mutation;
}

WalReplayStatus apply_record(KvStore& store, const WalRecord& record,
                             std::string* error) {
    if (record.op != WalOp::kTxn) {
        const KvStore::AppliedMutation mutation = to_applied_mutation(record);
        const KvStore::ApplyStatus apply_status =
            store.apply_committed_status(record.lsn, {mutation}, error);
        if (apply_status == KvStore::ApplyStatus::kResourceLimit) {
            return WalReplayStatus::kResourceLimit;
        }
        if (apply_status != KvStore::ApplyStatus::kOk) {
            return WalReplayStatus::kCorrupt;
        }
        return WalReplayStatus::kOk;
    }

    std::vector<WalTxnOp> operations;
    std::string payload_error;
    const WalFormat::DecodeStatus status = WalFormat::decode_transaction(
        reinterpret_cast<const std::uint8_t*>(record.value.data()),
        record.value.size(), &operations, &payload_error);
    if (status != WalFormat::DecodeStatus::kOk) {
        set_error(error, payload_error.empty()
                             ? "WAL transaction payload is invalid"
                             : payload_error);
        return WalReplayStatus::kCorrupt;
    }

    std::vector<KvStore::AppliedMutation> mutations;
    mutations.reserve(operations.size());
    for (const WalTxnOp& operation : operations) {
        KvStore::AppliedMutation mutation;
        mutation.key = operation.key;
        mutation.value = operation.value;
        mutation.absolute_expiry_ms = operation.expiry_ms;
        switch (operation.op) {
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
            set_error(error, "WAL transaction contains a nested transaction");
            return WalReplayStatus::kCorrupt;
        }
        mutations.push_back(std::move(mutation));
    }
    const KvStore::ApplyStatus apply_status =
        store.apply_committed_status(record.lsn, mutations, error);
    if (apply_status == KvStore::ApplyStatus::kResourceLimit) {
        return WalReplayStatus::kResourceLimit;
    }
    if (apply_status != KvStore::ApplyStatus::kOk) {
        return WalReplayStatus::kCorrupt;
    }
    return WalReplayStatus::kOk;
}

bool read_all(WalFileOperations* operations, int fd, std::uint8_t* data,
              std::size_t len, std::string* error) {
    std::size_t consumed = 0;
    while (consumed < len) {
        const ssize_t n = operations->read(fd, data + consumed, len - consumed);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            set_error(error,
                      "WAL read failed: " + std::string(std::strerror(errno)));
            return false;
        }
        if (n == 0) {
            set_error(error, "WAL segment ended before its recorded size");
            return false;
        }
        consumed += static_cast<std::size_t>(n);
    }
    return true;
}

bool truncate_tail(const WalSegmentInfo& segment, std::uint64_t valid_size,
                   WalFileOperations* operations, std::string* error) {
    const int fd = operations->open_read_write(segment.path);
    if (fd < 0) {
        set_error(error, "failed to open WAL segment for tail truncation");
        return false;
    }
    if (operations->truncate_fd(fd, static_cast<off_t>(valid_size)) < 0) {
        const int saved_errno = errno;
        (void)operations->close_fd(fd);
        errno = saved_errno;
        set_error(error, "failed to truncate WAL tail");
        return false;
    }
    if (operations->fsync_fd(fd) < 0) {
        const int saved_errno = errno;
        (void)operations->close_fd(fd);
        errno = saved_errno;
        set_error(error, "failed to fsync truncated WAL segment");
        return false;
    }
    if (operations->close_fd(fd) < 0) {
        set_error(error, "failed to close truncated WAL segment");
        return false;
    }
    const fs::path parent = fs::path(segment.path).parent_path();
    return fsync_directory(parent.empty() ? "." : parent.string(), operations,
                           error);
}

bool contains_start(const std::vector<WalSegmentInfo>& segments,
                    std::uint64_t start_lsn) {
    return std::any_of(segments.begin(), segments.end(),
                       [start_lsn](const WalSegmentInfo& segment) {
                           return segment.start_lsn == start_lsn;
                       });
}

} // namespace

WalReplayStatus select_wal_replay_segments(const std::string& wal_dir,
                                           const CheckpointManifest& manifest,
                                           WalFileOperations* operations,
                                           WalReplayPlan* plan,
                                           std::string* error,
                                           const WalLimits& limits) {
    if (!validate_wal_limits(limits, error)) {
        return WalReplayStatus::kResourceLimit;
    }
    if (operations == nullptr) {
        set_error(error, "WAL selection operations are null");
        return WalReplayStatus::kIoError;
    }
    if (plan == nullptr) {
        set_error(error, "WAL selection output is null");
        return WalReplayStatus::kCorrupt;
    }
    plan->segments.clear();
    plan->ignored_prefix_segments = 0;

    if (!CheckpointManifestCodec::validate(manifest, error)) {
        return WalReplayStatus::kCorrupt;
    }
    if (manifest.checkpoint_lsn == std::numeric_limits<std::uint64_t>::max()) {
        set_error(error, "checkpoint LSN leaves no room for another record");
        return WalReplayStatus::kCorrupt;
    }

    std::vector<WalSegmentInfo> on_disk;
    if (!list_wal_segments(wal_dir, &on_disk, error, limits)) {
        return WalReplayStatus::kIoError;
    }
    if (on_disk.size() > limits.max_segments) {
        set_error(error, "WAL segment count exceeds the recovery limit");
        return WalReplayStatus::kResourceLimit;
    }

    const std::uint64_t expected_first = manifest.checkpoint_lsn + 1;
    std::size_t first = 0;
    if (!manifest.retained_segment_starts.empty()) {
        if (manifest.retained_segment_starts.front() != expected_first) {
            set_error(error,
                      "manifest's first retained segment does not begin after "
                      "the checkpoint");
            return WalReplayStatus::kCorrupt;
        }
        for (const std::uint64_t retained : manifest.retained_segment_starts) {
            if (!contains_start(on_disk, retained)) {
                set_error(error,
                          "manifest references a missing WAL segment at LSN " +
                              std::to_string(retained));
                return WalReplayStatus::kCorrupt;
            }
        }
        const auto first_it =
            std::find_if(on_disk.begin(), on_disk.end(),
                         [expected_first](const WalSegmentInfo& segment) {
                             return segment.start_lsn == expected_first;
                         });
        first =
            static_cast<std::size_t>(std::distance(on_disk.begin(), first_it));
        plan->ignored_prefix_segments = first;
    } else if (!on_disk.empty() &&
               on_disk.front().start_lsn != expected_first) {
        set_error(error, "WAL history does not begin immediately after the "
                         "checkpoint");
        return WalReplayStatus::kCorrupt;
    }

    plan->segments.assign(on_disk.begin() + static_cast<std::ptrdiff_t>(first),
                          on_disk.end());
    if (plan->segments.size() > limits.max_segments) {
        set_error(error, "live WAL segment count exceeds the recovery limit");
        return WalReplayStatus::kResourceLimit;
    }
    return WalReplayStatus::kOk;
}

WalReplayStatus replay_wal_segments(const std::vector<WalSegmentInfo>& segments,
                                    std::uint64_t checkpoint_lsn,
                                    KvStore& store,
                                    WalFileOperations* operations,
                                    WalReplayStats* stats, std::string* error,
                                    const WalLimits& limits) {
    if (!validate_wal_limits(limits, error)) {
        return WalReplayStatus::kResourceLimit;
    }
    WalReplayStats local_stats;
    local_stats.segment_count = segments.size();
    local_stats.last_lsn = checkpoint_lsn;
    local_stats.next_lsn = checkpoint_lsn + 1;
    if (stats != nullptr) {
        *stats = local_stats;
    }
    if (operations == nullptr) {
        set_error(error, "WAL replay operations are null");
        return WalReplayStatus::kIoError;
    }
    if (segments.size() > limits.max_segments) {
        set_error(error, "WAL segment count exceeds the recovery limit");
        return WalReplayStatus::kResourceLimit;
    }
    if (checkpoint_lsn == std::numeric_limits<std::uint64_t>::max()) {
        set_error(error, "checkpoint LSN leaves no room for another record");
        return WalReplayStatus::kCorrupt;
    }

    std::uint64_t recovered_bytes = 0;
    std::uint64_t previous_segment_start = 0;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        const WalSegmentInfo& segment = segments[i];
        if (segment.start_lsn == 0 || segment.size > limits.max_segment_bytes) {
            set_error(error, "WAL segment metadata is out of range");
            return WalReplayStatus::kCorrupt;
        }
        if (i != 0 && segment.start_lsn <= previous_segment_start) {
            set_error(error, "WAL segment starts are not strictly increasing");
            return WalReplayStatus::kCorrupt;
        }
        if (i + 1 < segments.size() && segment.size == 0) {
            set_error(error, "empty WAL segment appears before the tail");
            return WalReplayStatus::kCorrupt;
        }
        if (recovered_bytes > limits.max_recovery_bytes ||
            segment.size > limits.max_recovery_bytes - recovered_bytes) {
            set_error(error, "WAL recovery byte limit is exceeded");
            return WalReplayStatus::kResourceLimit;
        }
        recovered_bytes += segment.size;
        previous_segment_start = segment.start_lsn;
    }
    local_stats.recovered_bytes = recovered_bytes;

    const std::uint64_t first_expected = checkpoint_lsn + 1;
    std::uint64_t last_lsn = checkpoint_lsn;
    std::uint64_t scanned_records = 0;
    std::uint64_t replayed_records = 0;
    bool seen_record = false;

    for (std::size_t segment_index = 0; segment_index < segments.size();
         ++segment_index) {
        const WalSegmentInfo& segment = segments[segment_index];
        const bool final_segment = segment_index + 1 == segments.size();
        const std::uint64_t expected_start =
            seen_record ? last_lsn + 1 : first_expected;
        if (segment.start_lsn != expected_start) {
            set_error(error, "WAL segment start LSN is not contiguous");
            return WalReplayStatus::kCorrupt;
        }
        if (segment.size == 0) {
            if (!final_segment) {
                set_error(error, "empty WAL segment appears before the tail");
                return WalReplayStatus::kCorrupt;
            }
            continue;
        }

        const int fd = operations->open_read(segment.path);
        if (fd < 0) {
            set_error(error,
                      "failed to open WAL segment for replay: " + segment.path);
            return WalReplayStatus::kIoError;
        }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(segment.size));
        const bool read_ok =
            read_all(operations, fd, bytes.data(), bytes.size(), error);
        const int close_result = operations->close_fd(fd);
        if (!read_ok) {
            return WalReplayStatus::kIoError;
        }
        if (close_result < 0) {
            set_error(error, "failed to close WAL segment after replay read");
            return WalReplayStatus::kIoError;
        }

        std::size_t offset = 0;
        bool first_in_segment = true;
        while (offset < bytes.size()) {
            WalRecord record;
            std::size_t consumed = 0;
            const WalFormat::DecodeStatus decode_status =
                WalFormat::decode(bytes.data() + offset, bytes.size() - offset,
                                  &record, &consumed, error);
            if (decode_status == WalFormat::DecodeStatus::kNeedMore) {
                if (!final_segment) {
                    set_error(error,
                              "incomplete WAL record appears in a non-tail "
                              "segment");
                    return WalReplayStatus::kCorrupt;
                }
                const std::uint64_t truncated = bytes.size() - offset;
                if (!truncate_tail(segment, offset, operations, error)) {
                    return WalReplayStatus::kIoError;
                }
                local_stats.truncated_bytes += truncated;
                offset = bytes.size();
                break;
            }
            if (decode_status == WalFormat::DecodeStatus::kCorrupt) {
                return WalReplayStatus::kCorrupt;
            }
            if (consumed == 0 || consumed > bytes.size() - offset) {
                set_error(error, "WAL decoder made invalid progress");
                return WalReplayStatus::kCorrupt;
            }
            if (record.lsn == std::numeric_limits<std::uint64_t>::max()) {
                set_error(error, "WAL LSN space is exhausted");
                return WalReplayStatus::kResourceLimit;
            }
            if (first_in_segment && record.lsn != segment.start_lsn) {
                set_error(error,
                          "WAL segment's first record does not match its start "
                          "LSN");
                return WalReplayStatus::kCorrupt;
            }
            if (seen_record && record.lsn != last_lsn + 1) {
                set_error(error, "WAL record LSN is not contiguous");
                return WalReplayStatus::kCorrupt;
            }
            if (!seen_record && record.lsn != first_expected) {
                set_error(error, "WAL history does not begin after checkpoint");
                return WalReplayStatus::kCorrupt;
            }
            if (++scanned_records > limits.max_recovery_records) {
                set_error(error, "WAL record count exceeds the recovery limit");
                return WalReplayStatus::kResourceLimit;
            }

            if (record.lsn > checkpoint_lsn) {
                const WalReplayStatus apply_status =
                    apply_record(store, record, error);
                if (apply_status != WalReplayStatus::kOk) {
                    return apply_status;
                }
                ++replayed_records;
            }

            last_lsn = record.lsn;
            seen_record = true;
            first_in_segment = false;
            offset += consumed;
        }

        if (offset != bytes.size()) {
            set_error(error, "WAL segment contains trailing bytes");
            return WalReplayStatus::kCorrupt;
        }
    }

    local_stats.scanned_records = scanned_records;
    local_stats.replayed_records = replayed_records;
    local_stats.last_lsn = last_lsn;
    local_stats.next_lsn = last_lsn + 1;
    if (stats != nullptr) {
        *stats = local_stats;
    }
    return WalReplayStatus::kOk;
}

WalReplayStatus replay_wal(const std::string& wal_dir,
                           const CheckpointManifest& manifest, KvStore& store,
                           WalFileOperations* operations,
                           WalReplayPlan* selected, WalReplayStats* stats,
                           std::string* error, const WalLimits& limits) {
    WalReplayPlan local_plan;
    WalReplayPlan* plan = selected == nullptr ? &local_plan : selected;
    const WalReplayStatus selection = select_wal_replay_segments(
        wal_dir, manifest, operations, plan, error, limits);
    if (selection != WalReplayStatus::kOk) {
        return selection;
    }
    WalReplayStats replay_stats;
    const WalReplayStatus result =
        replay_wal_segments(plan->segments, manifest.checkpoint_lsn, store,
                            operations, &replay_stats, error, limits);
    if (result == WalReplayStatus::kOk) {
        replay_stats.ignored_prefix_segments = plan->ignored_prefix_segments;
        if (stats != nullptr) {
            *stats = replay_stats;
        }
    }
    return result;
}

} // namespace minidb
