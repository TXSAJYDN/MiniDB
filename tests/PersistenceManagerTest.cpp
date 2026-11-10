#include <gtest/gtest.h>

#include "../src/storage/CheckpointManifest.h"
#include "../src/storage/PersistenceManager.h"
#include "../src/storage/RdbManager.h"
#include "../src/storage/WalManager.h"
#include "FaultWalFileOperations.h"
#include "RdbTestUtil.h"

#include <cerrno>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace minidb {
namespace {

struct DatabasePaths {
    std::string directory;
    std::string rdb;
    std::string manifest;
    std::string wal_dir;

    explicit DatabasePaths(const std::string& prefix) {
        directory = minidb_test::make_unique_temp_dir(prefix);
        rdb = directory + "/db.rdb";
        manifest = checkpoint_manifest_path(rdb);
        wal_dir = wal_directory_path(rdb);
    }

    ~DatabasePaths() {
        std::error_code ec;
        fs::remove_all(directory, ec);
    }
};

std::size_t segment_count(const std::string& wal_dir) {
    std::vector<WalSegmentInfo> segments;
    std::string error;
    EXPECT_TRUE(list_wal_segments(wal_dir, &segments, &error)) << error;
    return segments.size();
}

TEST(PersistenceManagerTest, SyncAcknowledgedMutationSurvivesManagerRestart) {
    DatabasePaths paths("minidb_persistence_sync_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    {
        auto store = std::make_shared<KvStore>(clock);
        PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
        std::string error;
        ASSERT_TRUE(manager.open(&error)) << error;
        const MutationOutcome outcome = manager.set("durable", "value");
        ASSERT_TRUE(outcome.ok()) << outcome.error;
        EXPECT_TRUE(outcome.changed);
        EXPECT_EQ(manager.snapshot().last_lsn, 1u);
        EXPECT_EQ(manager.snapshot().next_lsn, 2u);
        EXPECT_EQ(manager.snapshot().persistence.sync_mutations, 1u);
        EXPECT_EQ(manager.snapshot().persistence.interval_mutations, 0u);
        EXPECT_TRUE(fs::is_regular_file(paths.rdb));
        EXPECT_TRUE(fs::is_regular_file(paths.manifest));
        EXPECT_EQ(segment_count(paths.wal_dir), 1u);
    }

    auto recovered = std::make_shared<KvStore>(clock);
    PersistenceManager manager(recovered, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    EXPECT_EQ(recovered->get("durable"), std::optional<std::string>("value"));
    EXPECT_EQ(manager.snapshot().recovery.wal_records_replayed, 1u);
    EXPECT_EQ(manager.snapshot().last_lsn, 1u);
    EXPECT_EQ(manager.snapshot().next_lsn, 2u);
}

TEST(PersistenceManagerTest, IntervalDefersFsyncUntilExplicitFlush) {
    DatabasePaths paths("minidb_persistence_interval_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kInterval);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    ASSERT_TRUE(manager.set("interval", "value").ok());

    const PersistenceSnapshot before = manager.snapshot();
    EXPECT_EQ(before.persistence.interval_mutations, 1u);
    EXPECT_EQ(before.persistence.sync_mutations, 0u);
    EXPECT_EQ(before.persistence.interval_flushes, 0u);

    ASSERT_TRUE(manager.flush_interval(&error)) << error;
    const PersistenceSnapshot after = manager.snapshot();
    EXPECT_EQ(after.persistence.interval_flushes, 1u);
    EXPECT_GE(after.wal.data_fsyncs, before.wal.data_fsyncs + 1u);
    EXPECT_EQ(after.last_lsn, 1u);
    EXPECT_EQ(after.next_lsn, 2u);
}

TEST(PersistenceManagerTest, SyncFlushHookIsANoop) {
    DatabasePaths paths("minidb_persistence_sync_flush_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    ASSERT_TRUE(manager.set("sync", "value").ok());
    const PersistenceSnapshot before = manager.snapshot();
    ASSERT_TRUE(manager.flush_interval(&error)) << error;
    const PersistenceSnapshot after = manager.snapshot();
    EXPECT_EQ(after.persistence.interval_flushes, 0u);
    EXPECT_EQ(after.wal.data_fsyncs, before.wal.data_fsyncs);
}

TEST(PersistenceManagerTest, SetDeleteExpireAndAbsoluteTtlSurviveRestart) {
    DatabasePaths paths("minidb_persistence_ttl_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    {
        auto store = std::make_shared<KvStore>(clock);
        PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
        std::string error;
        ASSERT_TRUE(manager.open(&error)) << error;
        ASSERT_TRUE(manager.set("base", "value").ok());
        ASSERT_TRUE(manager.set("delete-me", "old").ok());
        ASSERT_TRUE(manager.del("delete-me").ok());
        ASSERT_TRUE(manager.set("with-ttl", "alive").ok());
        const MutationOutcome ttl = manager.expire("with-ttl", 60000);
        ASSERT_TRUE(ttl.ok()) << ttl.error;
        EXPECT_TRUE(ttl.changed);
        ASSERT_TRUE(manager.set("immediate", "gone").ok());
        const MutationOutcome immediate = manager.expire("immediate", 0);
        ASSERT_TRUE(immediate.ok()) << immediate.error;
        EXPECT_TRUE(immediate.changed);
        EXPECT_FALSE(store->get("immediate").has_value());
        clock->advance(10000);
    }

    auto recovered = std::make_shared<KvStore>(clock);
    PersistenceManager manager(recovered, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    EXPECT_EQ(recovered->get("base"), std::optional<std::string>("value"));
    EXPECT_FALSE(recovered->get("delete-me").has_value());
    EXPECT_FALSE(recovered->get("immediate").has_value());
    const auto ttl = recovered->ttl_info("with-ttl");
    ASSERT_EQ(ttl.state, KvStore::TtlState::kExpiring);
    EXPECT_GT(ttl.remaining_ms, 49000);
    EXPECT_LE(ttl.remaining_ms, 50000);

    clock->advance(50001);
    EXPECT_FALSE(recovered->get("with-ttl").has_value());
    EXPECT_EQ(recovered->get("base"), std::optional<std::string>("value"));
}

TEST(PersistenceManagerTest, CheckpointPublishesManifestAndCleansObsoleteWal) {
    DatabasePaths paths("minidb_persistence_checkpoint_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    const std::string rdb = paths.rdb;
    std::string error;
    {
        auto store = std::make_shared<KvStore>(clock);
        PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
        ASSERT_TRUE(manager.open(&error)) << error;
        ASSERT_TRUE(manager.set("one", "1").ok());
        ASSERT_TRUE(manager.set("two", "2").ok());
        ASSERT_TRUE(manager.set("three", "3").ok());

        ASSERT_EQ(manager.checkpoint(&error), RdbSaveResult::Ok) << error;
        const PersistenceSnapshot checkpointed = manager.snapshot();
        EXPECT_EQ(checkpointed.persistence.checkpoints, 1u);
        EXPECT_EQ(checkpointed.recovery.checkpoint_lsn, 0u);
        EXPECT_EQ(segment_count(paths.wal_dir), 1u);
        ASSERT_TRUE(manager.set("after", "tail").ok());
        EXPECT_EQ(manager.snapshot().last_lsn, 4u);

        CheckpointManifest manifest;
        ASSERT_EQ(load_manifest(paths.manifest, &manifest, &error),
                  ManifestLoadStatus::kOk)
            << error;
        EXPECT_EQ(manifest.checkpoint_lsn, 3u);
        ASSERT_EQ(manifest.retained_segment_starts.size(), 1u);
        EXPECT_EQ(manifest.retained_segment_starts.front(), 4u);
    }

    auto recovered = std::make_shared<KvStore>(clock);
    PersistenceManager restarted(recovered, rdb, DurabilityMode::kSync);
    ASSERT_TRUE(restarted.open(&error)) << error;
    EXPECT_EQ(recovered->get("one"), std::optional<std::string>("1"));
    EXPECT_EQ(recovered->get("two"), std::optional<std::string>("2"));
    EXPECT_EQ(recovered->get("three"), std::optional<std::string>("3"));
    EXPECT_EQ(recovered->get("after"), std::optional<std::string>("tail"));
    EXPECT_EQ(restarted.snapshot().recovery.checkpoint_lsn, 3u);
    EXPECT_EQ(restarted.snapshot().recovery.wal_records_replayed, 1u);
}

TEST(PersistenceManagerTest,
     CheckpointFailureRetainsWalAndRecoversAcknowledgedWrite) {
    DatabasePaths paths("minidb_persistence_checkpoint_fail_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    {
        auto store = std::make_shared<KvStore>(clock);
        PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync,
                                   operations, operations);
        std::string error;
        ASSERT_TRUE(manager.open(&error)) << error;
        const MutationOutcome kept = manager.set("kept", "value");
        ASSERT_TRUE(kept.ok()) << kept.error;
        operations->set_fail_rename(EXDEV);
        EXPECT_EQ(manager.checkpoint(&error), RdbSaveResult::Failed);
        EXPECT_FALSE(error.empty());
        EXPECT_GE(segment_count(paths.wal_dir), 1u);
    }

    auto recovered = std::make_shared<KvStore>(clock);
    PersistenceManager manager(recovered, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    EXPECT_EQ(recovered->get("kept"), std::optional<std::string>("value"));
}

TEST(PersistenceManagerTest,
     WalGrowthTriggersCheckpointAndRetriesTheSameRecord) {
    DatabasePaths paths("minidb_persistence_wal_limit_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    WalLimits limits;
    limits.max_segment_bytes = 256;
    limits.max_total_bytes = 256;
    limits.max_segments = 8;
    limits.max_recovery_records = 100;
    limits.max_recovery_bytes = 4096;
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync,
                               default_wal_file_operations(), nullptr, limits);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    ASSERT_TRUE(manager.set("a", std::string(16, 'x')).ok());
    ASSERT_TRUE(manager.set("b", std::string(160, 'y')).ok());
    const MutationOutcome third = manager.set("c", "z");
    ASSERT_TRUE(third.ok()) << third.error;
    EXPECT_GE(manager.snapshot().persistence.checkpoints, 1u);
    EXPECT_EQ(store->get("a"),
              std::optional<std::string>(std::string(16, 'x')));
    EXPECT_EQ(store->get("c"), std::optional<std::string>("z"));
    EXPECT_LE(manager.wal_bytes(), limits.max_total_bytes);
}

TEST(PersistenceManagerTest,
     OversizedValueIsRejectedWithoutPoisoningTheManager) {
    DatabasePaths paths("minidb_persistence_large_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    const MutationOutcome large =
        manager.set("large", std::string(WalFormat::MAX_VALUE_SIZE + 1, 'x'));
    EXPECT_EQ(large.status, MutationStatus::kResourceLimit);
    EXPECT_FALSE(manager.failed());
    EXPECT_FALSE(store->get("large").has_value());
    EXPECT_EQ(manager.snapshot().persistence.mutations, 0u);
    const MutationOutcome valid = manager.set("valid", "value");
    EXPECT_TRUE(valid.ok()) << valid.error;
}

TEST(PersistenceManagerTest, InvalidKeyIsRejectedWithoutWalMutation) {
    DatabasePaths paths("minidb_persistence_key_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    const MutationOutcome empty = manager.set("", "value");
    EXPECT_EQ(empty.status, MutationStatus::kInvalidArgument);
    const MutationOutcome huge =
        manager.set(std::string(RdbFormat::MAX_KEY_SIZE + 1, 'k'), "value");
    EXPECT_EQ(huge.status, MutationStatus::kInvalidArgument);
    EXPECT_EQ(manager.snapshot().last_lsn, 0u);
    EXPECT_EQ(manager.snapshot().persistence.mutations, 0u);
}

TEST(PersistenceManagerTest, SingleWriterLockRejectsAConcurrentOwner) {
    DatabasePaths paths("minidb_persistence_lock_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    const std::string rdb = paths.rdb;
    std::string error;
    {
        auto first_store = std::make_shared<KvStore>(clock);
        PersistenceManager first(first_store, paths.rdb, DurabilityMode::kSync);
        ASSERT_TRUE(first.open(&error)) << error;

        auto second_store = std::make_shared<KvStore>(clock);
        second_store->set("sentinel", "unchanged");
        PersistenceManager second(second_store, paths.rdb,
                                  DurabilityMode::kSync);
        EXPECT_FALSE(second.open(&error));
        EXPECT_NE(error.find("already owned"), std::string::npos);
        EXPECT_EQ(second_store->get("sentinel"),
                  std::optional<std::string>("unchanged"));
    }

    auto third_store = std::make_shared<KvStore>(clock);
    PersistenceManager third(third_store, rdb, DurabilityMode::kSync);
    EXPECT_TRUE(third.open(&error)) << error;
}

TEST(PersistenceManagerTest,
     FsyncFailureFailStopsAndLeavesOnlyAnUnconfirmedCompleteRecord) {
    DatabasePaths paths("minidb_persistence_fsync_fail_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    {
        auto store = std::make_shared<KvStore>(clock);
        PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync,
                                   operations, operations);
        std::string error;
        ASSERT_TRUE(manager.open(&error)) << error;
        const MutationOutcome acked = manager.set("acked", "value");
        ASSERT_TRUE(acked.ok()) << acked.error;
        operations->set_fail_fsync(EIO);
        const MutationOutcome failed = manager.set("unacked", "possible");
        EXPECT_EQ(failed.status, MutationStatus::kIoError);
        EXPECT_TRUE(manager.failed());
        EXPECT_FALSE(store->get("unacked").has_value());
        EXPECT_EQ(manager.set("later", "must-not-run").status,
                  MutationStatus::kNotReady);
        EXPECT_EQ(manager.checkpoint(&error), RdbSaveResult::Failed);
    }

    auto recovered = std::make_shared<KvStore>(clock);
    PersistenceManager manager(recovered, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    EXPECT_EQ(recovered->get("acked"), std::optional<std::string>("value"));
    // The complete record was written before fsync failed. Recovery may expose
    // the unacknowledged write, but it must never expose a partial value.
    EXPECT_EQ(recovered->get("unacked"),
              std::optional<std::string>("possible"));
    EXPECT_FALSE(recovered->get("later").has_value());
}

TEST(PersistenceManagerTest,
     LegacyRdbImportsOnlyWithoutWalAndBootstrapsOnFirstWrite) {
    DatabasePaths paths("minidb_persistence_legacy_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    KvStore legacy(clock);
    legacy.set("legacy", "value");
    RdbManager rdb(paths.rdb);
    ASSERT_EQ(rdb.save_checked(legacy), RdbSaveResult::Ok);
    ASSERT_FALSE(fs::exists(paths.manifest));
    ASSERT_FALSE(fs::exists(paths.wal_dir));

    {
        auto store = std::make_shared<KvStore>(clock);
        PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
        std::string error;
        ASSERT_TRUE(manager.open(&error)) << error;
        EXPECT_TRUE(manager.snapshot().recovery.legacy_import);
        EXPECT_TRUE(manager.snapshot().recovery.needs_bootstrap);
        EXPECT_EQ(store->get("legacy"), std::optional<std::string>("value"));
        ASSERT_TRUE(manager.set("new", "value").ok());
        EXPECT_FALSE(manager.snapshot().recovery.needs_bootstrap);
        EXPECT_TRUE(fs::exists(paths.manifest));
        EXPECT_TRUE(fs::exists(paths.wal_dir));
    }

    auto recovered = std::make_shared<KvStore>(clock);
    PersistenceManager manager(recovered, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    EXPECT_EQ(recovered->get("legacy"), std::optional<std::string>("value"));
    EXPECT_EQ(recovered->get("new"), std::optional<std::string>("value"));
}

TEST(PersistenceManagerTest, SnapshotReportsOneCoherentPersistencePoint) {
    DatabasePaths paths("minidb_persistence_snapshot_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    ASSERT_TRUE(manager.set("one", "1").ok());
    ASSERT_TRUE(manager.del("one").ok());
    ASSERT_TRUE(manager.set("two", "2").ok());
    ASSERT_EQ(manager.checkpoint(&error), RdbSaveResult::Ok) << error;

    const PersistenceSnapshot snapshot = manager.snapshot();
    EXPECT_TRUE(snapshot.opened);
    EXPECT_FALSE(snapshot.failed);
    EXPECT_EQ(snapshot.durability_mode, DurabilityMode::kSync);
    EXPECT_EQ(snapshot.last_lsn, 3u);
    EXPECT_EQ(snapshot.next_lsn, 4u);
    EXPECT_EQ(snapshot.persistence.mutations, 3u);
    EXPECT_EQ(snapshot.persistence.sync_mutations, 3u);
    EXPECT_EQ(snapshot.persistence.checkpoints, 1u);
    EXPECT_EQ(snapshot.wal.appends, 3u);
    EXPECT_EQ(snapshot.wal_segment_count, snapshot.wal.appends > 0 ? 1u : 0u);
}

} // namespace
} // namespace minidb
