#include <gtest/gtest.h>

#include "../src/storage/CheckpointManifest.h"
#include "../src/storage/PersistenceManager.h"
#include "../src/storage/RdbManager.h"
#include "../src/storage/WalManager.h"
#include "FaultWalFileOperations.h"
#include "RdbTestUtil.h"

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
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

class OwnerPump {
  public:
    GroupCommitWriter::OwnerDispatch dispatcher() {
        return [this](std::function<void()> callback) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                callbacks_.push_back(std::move(callback));
            }
            condition_.notify_one();
        };
    }

    bool WaitForPending(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_until(lock, deadline,
                                     [this]() { return !callbacks_.empty(); });
    }

    void RunPending() {
        std::deque<std::function<void()>> callbacks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            callbacks.swap(callbacks_);
        }
        for (auto& callback : callbacks) {
            callback();
        }
    }

    bool RunUntil(
        const std::function<bool()>& predicate,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::deque<std::function<void()>> callbacks;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    callbacks.swap(callbacks_);
                }
                for (auto& callback : callbacks) {
                    callback();
                }
            }
            if (predicate()) {
                return true;
            }
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait_for(lock, std::chrono::milliseconds(10));
        }
        return predicate();
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> callbacks_;
};

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
        const PersistenceSnapshot snapshot = manager.snapshot();
        EXPECT_EQ(snapshot.last_lsn, 1u);
        EXPECT_EQ(snapshot.next_lsn, 2u);
        EXPECT_EQ(snapshot.persistence.sync_mutations, 1u);
        EXPECT_EQ(snapshot.persistence.interval_mutations, 0u);
        EXPECT_GE(snapshot.persistence.sync_mutation_total_us,
                  snapshot.persistence.sync_mutation_max_us);
        EXPECT_GE(snapshot.wal.append_total_us, snapshot.wal.append_max_us);
        EXPECT_GE(snapshot.wal.data_fsync_total_us,
                  snapshot.wal.data_fsync_max_us);
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
        const PersistenceSnapshot before_failure = manager.snapshot();
        operations->set_fail_fsync(EIO);
        const MutationOutcome failed = manager.set("unacked", "possible");
        EXPECT_EQ(failed.status, MutationStatus::kIoError);
        EXPECT_TRUE(manager.failed());
        const PersistenceSnapshot after_failure = manager.snapshot();
        EXPECT_EQ(after_failure.persistence.sync_mutations,
                  before_failure.persistence.sync_mutations);
        EXPECT_EQ(after_failure.persistence.sync_mutation_total_us,
                  before_failure.persistence.sync_mutation_total_us);
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
    EXPECT_GE(snapshot.persistence.sync_mutation_total_us,
              snapshot.persistence.sync_mutation_max_us);
    EXPECT_GE(snapshot.wal.append_total_us, snapshot.wal.append_max_us);
    EXPECT_GE(snapshot.wal.data_fsync_total_us, snapshot.wal.data_fsync_max_us);
}

TEST(PersistenceManagerTest,
     GroupCommitAppliesAsyncMutationsInOrderAndShutsDown) {
    DatabasePaths paths("minidb_persistence_async_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    EXPECT_EQ(manager.set("bypass", "no").status, MutationStatus::kNotReady);
    EXPECT_FALSE(manager.flush_interval(&error));
    EXPECT_EQ(manager.checkpoint(&error), RdbSaveResult::Failed);

    std::vector<std::string> order;
    std::vector<MutationOutcome> outcomes;
    auto record = [&](const std::string& name) {
        return [&order, &outcomes, name](MutationOutcome outcome) {
            order.push_back(name);
            outcomes.push_back(std::move(outcome));
        };
    };
    ASSERT_TRUE(manager.set_async("a", "1", record("set-a")).accepted());
    ASSERT_TRUE(manager.set_async("b", "2", record("set-b")).accepted());
    ASSERT_TRUE(manager.del_async("a", record("del-a")).accepted());
    ASSERT_TRUE(manager.expire_async("b", 1000, record("expire-b")).accepted());

    ASSERT_TRUE(pump.RunUntil([&]() { return outcomes.size() == 4; }));
    ASSERT_EQ(order.size(), 4u);
    EXPECT_EQ(order[0], "set-a");
    EXPECT_EQ(order[1], "set-b");
    EXPECT_EQ(order[2], "del-a");
    EXPECT_EQ(order[3], "expire-b");
    for (const MutationOutcome& outcome : outcomes) {
        EXPECT_TRUE(outcome.ok()) << outcome.error;
        EXPECT_TRUE(outcome.changed);
    }
    EXPECT_FALSE(store->get("a").has_value());
    EXPECT_EQ(store->get("b"), std::optional<std::string>("2"));
    EXPECT_EQ(store->ttl_info("b").state, KvStore::TtlState::kExpiring);

    const PersistenceSnapshot applied = manager.snapshot();
    EXPECT_EQ(applied.last_lsn, 4u);
    EXPECT_EQ(applied.next_lsn, 5u);
    EXPECT_EQ(applied.persistence.mutations, 4u);
    EXPECT_EQ(applied.persistence.sync_mutations, 4u);
    EXPECT_EQ(applied.group_commit.writer_state,
              GroupCommitWriterState::kRunning);
    EXPECT_GE(applied.group_commit.batch_records, 4u);

    bool shutdown_done = false;
    RdbSaveResult shutdown_result = RdbSaveResult::Failed;
    std::string shutdown_error;
    ASSERT_TRUE(manager.shutdown_async(
        [&](RdbSaveResult result, const std::string& completion_error) {
            shutdown_result = result;
            shutdown_error = completion_error;
            shutdown_done = true;
        },
        &error))
        << error;
    ASSERT_TRUE(pump.RunUntil([&]() { return shutdown_done; }));
    EXPECT_EQ(shutdown_result, RdbSaveResult::Ok) << shutdown_error;
    EXPECT_TRUE(manager.shutdown_complete());
    EXPECT_EQ(manager.group_commit_stats().writer_state,
              GroupCommitWriterState::kStopped);
    EXPECT_EQ(manager.set_async("late", "value", record("late")).status,
              MutationAdmissionStatus::kShuttingDown);
}

TEST(PersistenceManagerTest,
     AsyncCheckpointDrainsAppliesAndResumesAtTheManifestBoundary) {
    DatabasePaths paths("minidb_persistence_async_checkpoint_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    std::size_t mutation_completions = 0;
    ASSERT_TRUE(manager
                    .set_async("one", "1",
                               [&](MutationOutcome outcome) {
                                   EXPECT_TRUE(outcome.ok());
                                   ++mutation_completions;
                               })
                    .accepted());
    ASSERT_TRUE(manager
                    .set_async("two", "2",
                               [&](MutationOutcome outcome) {
                                   EXPECT_TRUE(outcome.ok());
                                   ++mutation_completions;
                               })
                    .accepted());
    ASSERT_TRUE(manager
                    .set_async("three", "3",
                               [&](MutationOutcome outcome) {
                                   EXPECT_TRUE(outcome.ok());
                                   ++mutation_completions;
                               })
                    .accepted());

    bool checkpoint_done = false;
    bool observed_all_applied = false;
    RdbSaveResult checkpoint_result = RdbSaveResult::Failed;
    std::string checkpoint_error;
    ASSERT_TRUE(manager.checkpoint_async(
        [&](RdbSaveResult result, const std::string& completion_error) {
            checkpoint_result = result;
            checkpoint_error = completion_error;
            observed_all_applied =
                store->get("one") == std::optional<std::string>("1") &&
                store->get("two") == std::optional<std::string>("2") &&
                store->get("three") == std::optional<std::string>("3");
            checkpoint_done = true;
        },
        &error))
        << error;

    ASSERT_TRUE(pump.RunUntil([&]() { return checkpoint_done; }));
    EXPECT_EQ(checkpoint_result, RdbSaveResult::Ok) << checkpoint_error;
    EXPECT_TRUE(observed_all_applied);
    EXPECT_EQ(mutation_completions, 3u);
    EXPECT_EQ(manager.snapshot().persistence.checkpoints, 1u);
    EXPECT_EQ(manager.snapshot().last_lsn, 3u);
    EXPECT_EQ(manager.group_commit_stats().writer_state,
              GroupCommitWriterState::kRunning);

    CheckpointManifest manifest;
    ASSERT_EQ(load_manifest(paths.manifest, &manifest, &error),
              ManifestLoadStatus::kOk)
        << error;
    EXPECT_EQ(manifest.checkpoint_lsn, 3u);
    ASSERT_EQ(manifest.retained_segment_starts.size(), 1u);
    EXPECT_EQ(manifest.retained_segment_starts.front(), 4u);

    bool after_done = false;
    ASSERT_TRUE(manager
                    .set_async("after", "tail",
                               [&](MutationOutcome outcome) {
                                   EXPECT_TRUE(outcome.ok()) << outcome.error;
                                   after_done = true;
                               })
                    .accepted());
    ASSERT_TRUE(pump.RunUntil([&]() { return after_done; }));
    EXPECT_EQ(store->get("after"), std::optional<std::string>("tail"));
    EXPECT_EQ(manager.snapshot().last_lsn, 4u);

    bool shutdown_done = false;
    ASSERT_TRUE(manager.shutdown_async(
        [&](RdbSaveResult result, const std::string& shutdown_error) {
            EXPECT_EQ(result, RdbSaveResult::Ok) << shutdown_error;
            shutdown_done = true;
        },
        &error))
        << error;
    ASSERT_TRUE(pump.RunUntil([&]() { return shutdown_done; }));
}

TEST(PersistenceManagerTest,
     CleanupFailureKeepsWriterPausedAndTheNextCheckpointRetries) {
    DatabasePaths paths("minidb_persistence_cleanup_retry_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync,
                               operations, operations);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    bool mutation_done = false;
    ASSERT_TRUE(manager
                    .set_async("kept", "value",
                               [&](MutationOutcome outcome) {
                                   EXPECT_TRUE(outcome.ok()) << outcome.error;
                                   mutation_done = true;
                               })
                    .accepted());
    ASSERT_TRUE(pump.RunUntil([&]() { return mutation_done; }));

    operations->set_fail_unlink(EXDEV);
    bool first_done = false;
    RdbSaveResult first_result = RdbSaveResult::Failed;
    std::string first_error;
    ASSERT_TRUE(manager.checkpoint_async(
        [&](RdbSaveResult result, const std::string& completion_error) {
            first_result = result;
            first_error = completion_error;
            first_done = true;
        },
        &error))
        << error;
    ASSERT_TRUE(pump.RunUntil([&]() { return first_done; }));
    EXPECT_EQ(first_result, RdbSaveResult::Ok) << first_error;
    EXPECT_EQ(manager.group_commit_stats().writer_state,
              GroupCommitWriterState::kPaused);
    EXPECT_EQ(manager.snapshot().persistence.checkpoints, 1u);
    EXPECT_EQ(manager.set_async("blocked", "value", {}).status,
              MutationAdmissionStatus::kNotReady);

    operations->clear_fail_unlink();
    bool second_done = false;
    RdbSaveResult second_result = RdbSaveResult::Failed;
    std::string second_error;
    ASSERT_TRUE(manager.checkpoint_async(
        [&](RdbSaveResult result, const std::string& completion_error) {
            second_result = result;
            second_error = completion_error;
            second_done = true;
        },
        &error))
        << error;
    ASSERT_TRUE(pump.RunUntil([&]() { return second_done; }));
    EXPECT_EQ(second_result, RdbSaveResult::Ok) << second_error;
    EXPECT_EQ(manager.snapshot().persistence.checkpoints, 2u);
    EXPECT_EQ(manager.group_commit_stats().writer_state,
              GroupCommitWriterState::kRunning);

    bool resumed_done = false;
    ASSERT_TRUE(manager
                    .set_async("resumed", "yes",
                               [&](MutationOutcome outcome) {
                                   EXPECT_TRUE(outcome.ok()) << outcome.error;
                                   resumed_done = true;
                               })
                    .accepted());
    ASSERT_TRUE(pump.RunUntil([&]() { return resumed_done; }));
    EXPECT_EQ(store->get("resumed"), std::optional<std::string>("yes"));

    bool shutdown_done = false;
    ASSERT_TRUE(manager.shutdown_async(
        [&](RdbSaveResult result, const std::string& shutdown_error) {
            EXPECT_EQ(result, RdbSaveResult::Ok) << shutdown_error;
            shutdown_done = true;
        },
        &error))
        << error;
    ASSERT_TRUE(pump.RunUntil([&]() { return shutdown_done; }));
}

TEST(PersistenceManagerTest, CheckpointSnapshotFailureResumesTheWriter) {
    DatabasePaths paths("minidb_persistence_snapshot_retry_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync,
                               operations, operations);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    bool initial_done = false;
    ASSERT_TRUE(manager
                    .set_async("before", "snapshot",
                               [&](MutationOutcome outcome) {
                                   ASSERT_TRUE(outcome.ok()) << outcome.error;
                                   initial_done = true;
                               })
                    .accepted());
    ASSERT_TRUE(pump.RunUntil([&]() { return initial_done; }));

    operations->set_fail_rename(EXDEV);
    bool checkpoint_done = false;
    RdbSaveResult checkpoint_result = RdbSaveResult::Ok;
    std::string checkpoint_error;
    ASSERT_TRUE(manager.checkpoint_async(
        [&](RdbSaveResult result, const std::string& completion_error) {
            checkpoint_result = result;
            checkpoint_error = completion_error;
            checkpoint_done = true;
        },
        &error))
        << error;
    ASSERT_TRUE(pump.RunUntil([&]() { return checkpoint_done; }));
    EXPECT_EQ(checkpoint_result, RdbSaveResult::Failed);
    EXPECT_FALSE(checkpoint_error.empty());
    EXPECT_FALSE(manager.failed());
    EXPECT_EQ(manager.group_commit_stats().writer_state,
              GroupCommitWriterState::kRunning);
    EXPECT_EQ(manager.snapshot().persistence.checkpoint_failures, 1u);

    operations->clear_fail_rename();
    bool mutation_done = false;
    ASSERT_TRUE(manager
                    .set_async("after", "retry",
                               [&](MutationOutcome outcome) {
                                   ASSERT_TRUE(outcome.ok()) << outcome.error;
                                   mutation_done = true;
                               })
                    .accepted());
    ASSERT_TRUE(pump.RunUntil([&]() { return mutation_done; }));
    EXPECT_EQ(store->get("after"), std::optional<std::string>("retry"));
}

TEST(PersistenceManagerTest,
     UnconfirmedSnapshotDurabilityFailsClosedAndDoesNotResumeWriter) {
    DatabasePaths paths("minidb_persistence_unconfirmed_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync,
                               operations, operations);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    bool initial_done = false;
    ASSERT_TRUE(manager
                    .set_async("before", "snapshot",
                               [&](MutationOutcome outcome) {
                                   ASSERT_TRUE(outcome.ok()) << outcome.error;
                                   initial_done = true;
                               })
                    .accepted());
    ASSERT_TRUE(pump.RunUntil([&]() { return initial_done; }));

    // The checkpoint rotates WAL (one directory fsync), then the RDB parent
    // directory fsync is the next call.  Failing exactly that call leaves the
    // rename visible but its durability unconfirmed.
    operations->set_fail_dir_fsync_after(operations->dir_fsync_calls() + 1,
                                         EIO);
    bool checkpoint_done = false;
    RdbSaveResult checkpoint_result = RdbSaveResult::Ok;
    std::string checkpoint_error;
    ASSERT_TRUE(manager.checkpoint_async(
        [&](RdbSaveResult result, const std::string& completion_error) {
            checkpoint_result = result;
            checkpoint_error = completion_error;
            checkpoint_done = true;
        },
        &error))
        << error;
    ASSERT_TRUE(pump.RunUntil([&]() { return checkpoint_done; }));
    EXPECT_EQ(checkpoint_result, RdbSaveResult::DurabilityUnconfirmed);
    EXPECT_FALSE(checkpoint_error.empty());
    EXPECT_TRUE(manager.failed());
    EXPECT_EQ(manager.group_commit_stats().writer_state,
              GroupCommitWriterState::kRunning);
    EXPECT_NE(manager.set_async("after", "must-not-run", {}).status,
              MutationAdmissionStatus::kAccepted);
    EXPECT_FALSE(manager.checkpoint_async({}, &error));
}

TEST(PersistenceManagerTest,
     MutationsDuringBackgroundWriteAreReplayedAfterTheCapturedLsn) {
    DatabasePaths paths("minidb_persistence_background_write_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    std::mutex hook_mutex;
    std::condition_variable hook_condition;
    std::atomic<bool> rdb_write_entered{false};
    bool release_rdb_write = false;

    {
        auto store = std::make_shared<KvStore>(clock);
        PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync,
                                   operations, operations);
        std::string error;
        ASSERT_TRUE(manager.open(&error)) << error;
        OwnerPump pump;
        ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error))
            << error;

        bool first_done = false;
        ASSERT_TRUE(manager
                        .set_async("before", "snapshot",
                                   [&](MutationOutcome outcome) {
                                       ASSERT_TRUE(outcome.ok())
                                           << outcome.error;
                                       first_done = true;
                                   })
                        .accepted());
        ASSERT_TRUE(pump.RunUntil([&]() { return first_done; }));

        operations->set_before_write_hook(
            minidb_test::FaultWalFileOperations::FileKind::kRdbSnapshot, [&]() {
                rdb_write_entered.store(true);
                std::unique_lock<std::mutex> lock(hook_mutex);
                hook_condition.wait(lock, [&]() { return release_rdb_write; });
            });

        bool checkpoint_done = false;
        RdbSaveResult checkpoint_result = RdbSaveResult::Failed;
        std::string checkpoint_error;
        ASSERT_TRUE(manager.checkpoint_async(
            [&](RdbSaveResult result, const std::string& completion_error) {
                checkpoint_result = result;
                checkpoint_error = completion_error;
                checkpoint_done = true;
            },
            &error))
            << error;
        ASSERT_TRUE(pump.RunUntil([&]() { return rdb_write_entered.load(); }));
        EXPECT_EQ(manager.snapshot().persistence.background_checkpoint_state,
                  BackgroundCheckpointState::kWriting);

        // A second SAVE or shutdown cannot race the in-flight checkpoint.
        std::string rejected_error;
        EXPECT_FALSE(manager.checkpoint_async({}, &rejected_error));
        EXPECT_FALSE(rejected_error.empty());
        rejected_error.clear();
        EXPECT_FALSE(manager.shutdown_async({}, &rejected_error));
        EXPECT_FALSE(rejected_error.empty());

        bool after_done = false;
        ASSERT_TRUE(manager
                        .set_async("after", "capture",
                                   [&](MutationOutcome outcome) {
                                       ASSERT_TRUE(outcome.ok())
                                           << outcome.error;
                                       after_done = true;
                                   })
                        .accepted());
        ASSERT_TRUE(pump.RunUntil([&]() { return after_done; }));

        {
            std::lock_guard<std::mutex> lock(hook_mutex);
            release_rdb_write = true;
        }
        hook_condition.notify_all();
        ASSERT_TRUE(pump.RunUntil([&]() { return checkpoint_done; }));
        EXPECT_EQ(checkpoint_result, RdbSaveResult::Ok) << checkpoint_error;
        EXPECT_EQ(manager.snapshot().persistence.checkpoints, 1u);
        EXPECT_EQ(manager.group_commit_stats().writer_state,
                  GroupCommitWriterState::kRunning);

        bool shutdown_done = false;
        ASSERT_TRUE(manager.shutdown_async(
            [&](RdbSaveResult result, const std::string& shutdown_error) {
                ASSERT_EQ(result, RdbSaveResult::Ok) << shutdown_error;
                shutdown_done = true;
            },
            &error))
            << error;
        ASSERT_TRUE(pump.RunUntil([&]() { return shutdown_done; }));
    }

    auto recovered = std::make_shared<KvStore>(clock);
    PersistenceManager manager(recovered, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    EXPECT_EQ(recovered->get("before"), std::optional<std::string>("snapshot"));
    EXPECT_EQ(recovered->get("after"), std::optional<std::string>("capture"));
}

TEST(PersistenceManagerTest,
     SequentialBackgroundCheckpointsAdvanceTheManifestBoundary) {
    DatabasePaths paths("minidb_persistence_sequential_checkpoint_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    {
        auto store = std::make_shared<KvStore>(clock);
        PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
        std::string error;
        ASSERT_TRUE(manager.open(&error)) << error;
        OwnerPump pump;
        ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error))
            << error;

        bool one_done = false;
        ASSERT_TRUE(manager
                        .set_async("one", "first",
                                   [&](MutationOutcome outcome) {
                                       ASSERT_TRUE(outcome.ok())
                                           << outcome.error;
                                       one_done = true;
                                   })
                        .accepted());
        ASSERT_TRUE(pump.RunUntil([&]() { return one_done; }));

        bool first_checkpoint_done = false;
        RdbSaveResult first_checkpoint_result = RdbSaveResult::Failed;
        std::string first_checkpoint_error;
        ASSERT_TRUE(manager.checkpoint_async(
            [&](RdbSaveResult result, const std::string& completion_error) {
                first_checkpoint_result = result;
                first_checkpoint_error = completion_error;
                first_checkpoint_done = true;
            },
            &error))
            << error;
        ASSERT_TRUE(pump.RunUntil([&]() { return first_checkpoint_done; }));
        ASSERT_EQ(first_checkpoint_result, RdbSaveResult::Ok)
            << first_checkpoint_error;
        EXPECT_EQ(manager.group_commit_stats().writer_state,
                  GroupCommitWriterState::kRunning);

        bool two_done = false;
        ASSERT_TRUE(manager
                        .set_async("two", "second",
                                   [&](MutationOutcome outcome) {
                                       ASSERT_TRUE(outcome.ok())
                                           << outcome.error;
                                       two_done = true;
                                   })
                        .accepted());
        ASSERT_TRUE(pump.RunUntil([&]() { return two_done; }));

        bool second_checkpoint_done = false;
        RdbSaveResult second_checkpoint_result = RdbSaveResult::Failed;
        std::string second_checkpoint_error;
        ASSERT_TRUE(manager.checkpoint_async(
            [&](RdbSaveResult result, const std::string& completion_error) {
                second_checkpoint_result = result;
                second_checkpoint_error = completion_error;
                second_checkpoint_done = true;
            },
            &error))
            << error;
        ASSERT_TRUE(pump.RunUntil([&]() { return second_checkpoint_done; }));
        ASSERT_EQ(second_checkpoint_result, RdbSaveResult::Ok)
            << second_checkpoint_error;
        EXPECT_EQ(manager.snapshot().persistence.checkpoints, 2u);
        EXPECT_EQ(manager.group_commit_stats().writer_state,
                  GroupCommitWriterState::kRunning);

        CheckpointManifest manifest;
        ASSERT_EQ(load_manifest(paths.manifest, &manifest, &error),
                  ManifestLoadStatus::kOk)
            << error;
        EXPECT_EQ(manifest.checkpoint_lsn, 2u);
        ASSERT_EQ(manifest.retained_segment_starts.size(), 1u);
        EXPECT_EQ(manifest.retained_segment_starts.front(), 3u);

        bool shutdown_done = false;
        ASSERT_TRUE(manager.shutdown_async(
            [&](RdbSaveResult result, const std::string& shutdown_error) {
                ASSERT_EQ(result, RdbSaveResult::Ok) << shutdown_error;
                shutdown_done = true;
            },
            &error))
            << error;
        ASSERT_TRUE(pump.RunUntil([&]() { return shutdown_done; }));
    }

    auto recovered = std::make_shared<KvStore>(clock);
    PersistenceManager restarted(recovered, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(restarted.open(&error)) << error;
    EXPECT_EQ(recovered->get("one"), std::optional<std::string>("first"));
    EXPECT_EQ(recovered->get("two"), std::optional<std::string>("second"));
}

TEST(PersistenceManagerTest,
     ManifestRetainsEverySegmentWrittenDuringBackgroundSnapshot) {
    DatabasePaths paths("minidb_persistence_retained_segments_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalLimits limits;
    limits.max_segment_bytes = 260;
    limits.max_total_bytes = 64 * 1024;
    limits.max_segments = 24;
    std::mutex hook_mutex;
    std::condition_variable hook_condition;
    std::atomic<bool> rdb_write_entered{false};
    bool release_rdb_write = false;

    {
        auto store = std::make_shared<KvStore>(clock);
        PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync,
                                   operations, operations, limits);
        std::string error;
        ASSERT_TRUE(manager.open(&error)) << error;
        OwnerPump pump;
        ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error))
            << error;

        bool seed_done = false;
        ASSERT_TRUE(manager
                        .set_async("seed", "value",
                                   [&](MutationOutcome outcome) {
                                       ASSERT_TRUE(outcome.ok())
                                           << outcome.error;
                                       seed_done = true;
                                   })
                        .accepted());
        ASSERT_TRUE(pump.RunUntil([&]() { return seed_done; }));

        operations->set_before_write_hook(
            minidb_test::FaultWalFileOperations::FileKind::kRdbSnapshot, [&]() {
                rdb_write_entered.store(true);
                std::unique_lock<std::mutex> lock(hook_mutex);
                hook_condition.wait(lock, [&]() { return release_rdb_write; });
            });

        bool checkpoint_done = false;
        RdbSaveResult checkpoint_result = RdbSaveResult::Failed;
        std::string checkpoint_error;
        ASSERT_TRUE(manager.checkpoint_async(
            [&](RdbSaveResult result, const std::string& completion_error) {
                checkpoint_result = result;
                checkpoint_error = completion_error;
                checkpoint_done = true;
            },
            &error))
            << error;
        ASSERT_TRUE(pump.RunUntil([&]() { return rdb_write_entered.load(); }));

        const std::string value(150, 'x');
        for (int index = 0; index < 5; ++index) {
            const std::string key = "background-" + std::to_string(index);
            bool mutation_done = false;
            ASSERT_TRUE(manager
                            .set_async(key, value,
                                       [&](MutationOutcome outcome) {
                                           ASSERT_TRUE(outcome.ok())
                                               << outcome.error;
                                           mutation_done = true;
                                       })
                            .accepted());
            ASSERT_TRUE(pump.RunUntil([&]() { return mutation_done; }));
        }
        EXPECT_GT(manager.snapshot().wal_segment_count, 1u);

        {
            std::lock_guard<std::mutex> lock(hook_mutex);
            release_rdb_write = true;
        }
        hook_condition.notify_all();
        ASSERT_TRUE(pump.RunUntil([&]() { return checkpoint_done; }));
        ASSERT_EQ(checkpoint_result, RdbSaveResult::Ok) << checkpoint_error;

        CheckpointManifest manifest;
        ASSERT_EQ(load_manifest(paths.manifest, &manifest, &error),
                  ManifestLoadStatus::kOk)
            << error;
        EXPECT_GE(manifest.retained_segment_starts.size(), 2u);
        EXPECT_EQ(manifest.retained_segment_starts.front(),
                  manifest.checkpoint_lsn + 1);
        for (const std::uint64_t start_lsn : manifest.retained_segment_starts) {
            EXPECT_TRUE(fs::is_regular_file(paths.wal_dir + "/" +
                                            wal_segment_name(start_lsn)))
                << start_lsn;
        }
        EXPECT_EQ(segment_count(paths.wal_dir),
                  manifest.retained_segment_starts.size());

        bool shutdown_done = false;
        ASSERT_TRUE(manager.shutdown_async(
            [&](RdbSaveResult result, const std::string& shutdown_error) {
                ASSERT_EQ(result, RdbSaveResult::Ok) << shutdown_error;
                shutdown_done = true;
            },
            &error))
            << error;
        ASSERT_TRUE(pump.RunUntil([&]() { return shutdown_done; }));
    }

    auto recovered = std::make_shared<KvStore>(clock);
    PersistenceManager restarted(recovered, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(restarted.open(&error)) << error;
    EXPECT_EQ(recovered->get("seed"), std::optional<std::string>("value"));
    for (int index = 0; index < 5; ++index) {
        EXPECT_EQ(recovered->get("background-" + std::to_string(index)),
                  std::optional<std::string>(std::string(150, 'x')));
    }
}

TEST(PersistenceManagerTest,
     FailedBackgroundSnapshotLeavesPriorImageRecoverable) {
    DatabasePaths paths("minidb_persistence_failed_background_restart_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    {
        auto store = std::make_shared<KvStore>(clock);
        PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync,
                                   operations, operations);
        std::string error;
        ASSERT_TRUE(manager.open(&error)) << error;
        OwnerPump pump;
        ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error))
            << error;

        bool first_done = false;
        ASSERT_TRUE(manager
                        .set_async("before", "checkpoint",
                                   [&](MutationOutcome outcome) {
                                       ASSERT_TRUE(outcome.ok())
                                           << outcome.error;
                                       first_done = true;
                                   })
                        .accepted());
        ASSERT_TRUE(pump.RunUntil([&]() { return first_done; }));

        operations->set_fail_rename(EXDEV);
        bool checkpoint_done = false;
        RdbSaveResult checkpoint_result = RdbSaveResult::Ok;
        std::string checkpoint_error;
        ASSERT_TRUE(manager.checkpoint_async(
            [&](RdbSaveResult result, const std::string& completion_error) {
                checkpoint_result = result;
                checkpoint_error = completion_error;
                checkpoint_done = true;
            },
            &error))
            << error;
        ASSERT_TRUE(pump.RunUntil([&]() { return checkpoint_done; }));
        EXPECT_EQ(checkpoint_result, RdbSaveResult::Failed);
        EXPECT_FALSE(checkpoint_error.empty());

        bool after_done = false;
        ASSERT_TRUE(manager
                        .set_async("after", "failure",
                                   [&](MutationOutcome outcome) {
                                       ASSERT_TRUE(outcome.ok())
                                           << outcome.error;
                                       after_done = true;
                                   })
                        .accepted());
        ASSERT_TRUE(pump.RunUntil([&]() { return after_done; }));
        EXPECT_FALSE(manager.failed());
    }

    auto recovered = std::make_shared<KvStore>(clock);
    PersistenceManager restarted(recovered, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(restarted.open(&error)) << error;
    EXPECT_EQ(recovered->get("before"),
              std::optional<std::string>("checkpoint"));
    EXPECT_EQ(recovered->get("after"), std::optional<std::string>("failure"));
}

TEST(PersistenceManagerTest,
     DestroyingManagerMakesQueuedCheckpointCallbackANoop) {
    DatabasePaths paths("minidb_persistence_callback_lifecycle_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    auto manager = std::make_unique<PersistenceManager>(store, paths.rdb,
                                                        DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager->open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager->start_group_commit(pump.dispatcher(), &error))
        << error;

    bool mutation_done = false;
    ASSERT_TRUE(manager
                    ->set_async("before", "callback",
                                [&](MutationOutcome outcome) {
                                    ASSERT_TRUE(outcome.ok()) << outcome.error;
                                    mutation_done = true;
                                })
                    .accepted());
    ASSERT_TRUE(pump.RunUntil([&]() { return mutation_done; }));

    bool checkpoint_completed = false;
    ASSERT_TRUE(manager->checkpoint_async(
        [&](RdbSaveResult, const std::string&) { checkpoint_completed = true; },
        &error))
        << error;
    ASSERT_TRUE(pump.RunUntil([&]() {
        return manager->snapshot().persistence.background_checkpoint_state ==
               BackgroundCheckpointState::kWriting;
    }));
    ASSERT_TRUE(pump.WaitForPending());

    manager.reset();
    ASSERT_FALSE(checkpoint_completed);
    pump.RunPending();
    EXPECT_FALSE(checkpoint_completed);
}

TEST(PersistenceManagerTest,
     WriterFailureCompletesTheAcceptedMutationAndFailsClosed) {
    DatabasePaths paths("minidb_persistence_writer_failure_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync,
                               operations, operations);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    operations->set_fail_fsync(ENOSPC);
    bool completion_done = false;
    MutationOutcome completed;
    ASSERT_TRUE(manager
                    .set_async("fails", "value",
                               [&](MutationOutcome outcome) {
                                   completed = std::move(outcome);
                                   completion_done = true;
                               })
                    .accepted());
    ASSERT_TRUE(pump.RunUntil([&]() { return completion_done; }));
    EXPECT_EQ(completed.status, MutationStatus::kIoError);
    EXPECT_TRUE(manager.failed());
    EXPECT_EQ(manager.group_commit_stats().writer_state,
              GroupCommitWriterState::kFailed);
    EXPECT_EQ(manager.group_commit_stats().writer_failures, 1u);
    EXPECT_GE(manager.group_commit_stats().fsync_failures, 1u);
    EXPECT_EQ(store->get("fails").has_value(), false);
    EXPECT_EQ(manager.set_async("after", "value", {}).status,
              MutationAdmissionStatus::kIoError);
}

} // namespace
} // namespace minidb
