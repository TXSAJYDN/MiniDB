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

TEST(PersistenceManagerTest, MsetCommitsOneAtomicRecordAndRecoversEveryKey) {
    DatabasePaths paths("minidb_persistence_mset_");
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

        std::size_t completions = 0;
        MutationOutcome completed;
        ASSERT_TRUE(manager
                        .mset_async({{"beta", "two"}, {"alpha", "one"}},
                                    [&](MutationOutcome outcome) {
                                        completed = std::move(outcome);
                                        ++completions;
                                    })
                        .accepted());
        ASSERT_TRUE(pump.RunUntil([&]() { return completions == 1; }));
        ASSERT_TRUE(completed.ok()) << completed.error;
        EXPECT_TRUE(completed.changed);
        EXPECT_EQ(completed.lsn, 1u);
        EXPECT_EQ(store->get("alpha"), std::optional<std::string>("one"));
        EXPECT_EQ(store->get("beta"), std::optional<std::string>("two"));

        const PersistenceSnapshot snapshot = manager.snapshot();
        EXPECT_EQ(snapshot.last_lsn, 1u);
        EXPECT_EQ(snapshot.next_lsn, 2u);
        EXPECT_EQ(snapshot.persistence.mutations, 1u);
        EXPECT_EQ(snapshot.persistence.sync_mutations, 1u);
        EXPECT_EQ(snapshot.transactions.started, 0u);
        EXPECT_EQ(snapshot.transactions.committed, 0u);
        EXPECT_EQ(snapshot.transactions.commit_records, 0u);
    }

    auto recovered = std::make_shared<KvStore>(clock);
    PersistenceManager manager(recovered, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    EXPECT_EQ(recovered->get("alpha"), std::optional<std::string>("one"));
    EXPECT_EQ(recovered->get("beta"), std::optional<std::string>("two"));
    EXPECT_EQ(manager.snapshot().recovery.wal_records_replayed, 1u);
    EXPECT_EQ(manager.snapshot().last_lsn, 1u);
}

TEST(PersistenceManagerTest, MsetRejectsInvalidBatchesBeforeAdmission) {
    DatabasePaths paths("minidb_persistence_mset_invalid_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    auto expect_rejected = [&](MutationAdmission admission,
                               MutationAdmissionStatus expected) {
        EXPECT_EQ(admission.status, expected) << admission.error;
        EXPECT_FALSE(admission.accepted());
    };

    expect_rejected(manager.mset_async({}, {}),
                    MutationAdmissionStatus::kInvalidArgument);
    expect_rejected(manager.mset_async({{"same", "one"}, {"same", "two"}}, {}),
                    MutationAdmissionStatus::kInvalidArgument);
    const std::string reserved =
        std::string(AgentRecordCodec::kReservedPrefix) + "mset";
    expect_rejected(manager.mset_async({{reserved, "value"}}, {}),
                    MutationAdmissionStatus::kInvalidArgument);

    std::vector<std::pair<std::string, std::string>> too_many;
    too_many.reserve(WalFormat::MAX_TXN_OPS + 1);
    for (std::size_t index = 0; index <= WalFormat::MAX_TXN_OPS; ++index) {
        too_many.emplace_back("key-" + std::to_string(index), "value");
    }
    expect_rejected(manager.mset_async(std::move(too_many), {}),
                    MutationAdmissionStatus::kResourceLimit);

    const PersistenceSnapshot snapshot = manager.snapshot();
    EXPECT_EQ(snapshot.last_lsn, 0u);
    EXPECT_EQ(snapshot.persistence.mutations, 0u);
    EXPECT_EQ(snapshot.wal.appends, 0u);
    EXPECT_FALSE(store->get("same").has_value());
    EXPECT_FALSE(store->get(reserved).has_value());
}

TEST(PersistenceManagerTest, MsetRejectsEncodedPayloadOverWalLimit) {
    DatabasePaths paths("minidb_persistence_mset_payload_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    // The value fits the individual WAL value limit, but the transaction
    // header/op overhead and key push the encoded MTXN payload over 1 MiB.
    const std::string large_value(WalFormat::MAX_VALUE_SIZE, 'x');
    const MutationAdmission admission =
        manager.mset_async({{"large", large_value}}, {});
    EXPECT_EQ(admission.status, MutationAdmissionStatus::kResourceLimit);
    EXPECT_FALSE(admission.accepted());
    EXPECT_FALSE(store->get("large").has_value());

    const PersistenceSnapshot snapshot = manager.snapshot();
    EXPECT_EQ(snapshot.last_lsn, 0u);
    EXPECT_EQ(snapshot.persistence.mutations, 0u);
    EXPECT_EQ(snapshot.wal.appends, 0u);
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

std::optional<MutationOutcome> commit_and_wait(PersistenceManager* manager,
                                               OwnerPump* pump,
                                               std::uint64_t txn_id,
                                               int* callback_count = nullptr) {
    bool done = false;
    MutationOutcome outcome;
    const MutationAdmission admission = manager->commit_transaction_async(
        txn_id, [&](MutationOutcome committed) {
            outcome = std::move(committed);
            if (callback_count != nullptr) {
                ++*callback_count;
            }
            done = true;
        });
    EXPECT_TRUE(admission.accepted()) << admission.error;
    if (!admission.accepted()) {
        return std::nullopt;
    }
    EXPECT_TRUE(pump->RunUntil([&]() { return done; }));
    return outcome;
}

bool parse_agent_request(const std::string& subcommand,
                         const std::vector<std::string>& args,
                         AgentRequest* request, std::string* error) {
    return AgentStateStore::parse_request(subcommand, args, request, error);
}

std::optional<AgentResult> mutate_agent_and_wait(PersistenceManager* manager,
                                                 OwnerPump* pump,
                                                 const AgentRequest& request) {
    bool done = false;
    AgentResult result;
    const AgentResult admission =
        manager->agent_mutate_async(request, [&](AgentResult completed) {
            result = std::move(completed);
            done = true;
        });
    EXPECT_EQ(admission.status, AgentRequestStatus::kAccepted)
        << admission.error;
    if (admission.status != AgentRequestStatus::kAccepted) {
        return std::nullopt;
    }
    EXPECT_TRUE(pump->RunUntil([&]() { return done; }));
    return result;
}

bool create_terminal_session_group(PersistenceManager* manager, OwnerPump* pump,
                                   const std::string& session_id,
                                   std::size_t tool_count) {
    std::string error;
    AgentRequest request;
    if (!parse_agent_request(
            "SESSION_CREATE",
            {session_id, "60000", "ready", "create-" + session_id}, &request,
            &error)) {
        ADD_FAILURE() << error;
        return false;
    }
    const auto created = mutate_agent_and_wait(manager, pump, request);
    if (!created.has_value() ||
        created->status != AgentRequestStatus::kCompleted) {
        ADD_FAILURE() << (created.has_value() ? created->error
                                              : "create was not admitted");
        return false;
    }
    AgentRecordCodec::Response create_response;
    if (!AgentRecordCodec::decode_response(created->encoded_response,
                                           &create_response, &error) ||
        create_response.status !=
            static_cast<std::uint8_t>(
                AgentRecordCodec::ResponseStatus::kCreated)) {
        ADD_FAILURE() << "unexpected create response: " << error;
        return false;
    }

    for (std::size_t index = 0; index < tool_count; ++index) {
        std::string tool_id = "t";
        const std::string digits = std::to_string(index);
        tool_id.append(3 - std::min<std::size_t>(3, digits.size()), '0');
        tool_id += digits;
        if (!parse_agent_request("TOOL_CREATE",
                                 {session_id, tool_id, "tool", "request",
                                  "tool-" + session_id + "-" + digits},
                                 &request, &error)) {
            ADD_FAILURE() << error;
            return false;
        }
        const auto tool = mutate_agent_and_wait(manager, pump, request);
        if (!tool.has_value() ||
            tool->status != AgentRequestStatus::kCompleted) {
            ADD_FAILURE() << (tool.has_value()
                                  ? tool->error
                                  : "tool create was not admitted");
            return false;
        }
        AgentRecordCodec::Response tool_response;
        if (!AgentRecordCodec::decode_response(tool->encoded_response,
                                               &tool_response, &error) ||
            tool_response.status !=
                static_cast<std::uint8_t>(
                    AgentRecordCodec::ResponseStatus::kCreated)) {
            ADD_FAILURE() << "unexpected tool response: " << error;
            return false;
        }
    }

    const std::string close_revision = std::to_string(1 + tool_count);
    if (!parse_agent_request(
            "SESSION_CLOSE",
            {session_id, close_revision, "COMPLETED", "close-" + session_id},
            &request, &error)) {
        ADD_FAILURE() << error;
        return false;
    }
    const auto closed = mutate_agent_and_wait(manager, pump, request);
    if (!closed.has_value() ||
        closed->status != AgentRequestStatus::kCompleted) {
        ADD_FAILURE() << (closed.has_value() ? closed->error
                                             : "close was not admitted");
        return false;
    }
    AgentRecordCodec::Response close_response;
    if (!AgentRecordCodec::decode_response(closed->encoded_response,
                                           &close_response, &error) ||
        close_response.status !=
            static_cast<std::uint8_t>(AgentRecordCodec::ResponseStatus::kOk)) {
        ADD_FAILURE() << "unexpected close response status="
                      << static_cast<unsigned>(close_response.status)
                      << " error=" << error
                      << " manager failure=" << manager->failed();
        return false;
    }
    return true;
}

TEST(PersistenceManagerTest,
     TransactionsProvideSnapshotIsolationRepeatableReadAndReadYourWrites) {
    DatabasePaths paths("minidb_transaction_isolation_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    ASSERT_TRUE(manager.set("key", "before").ok());
    ASSERT_TRUE(manager.set("other", "stable").ok());
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    const auto writer = manager.begin_transaction();
    ASSERT_TRUE(writer.ok()) << writer.error;
    const auto reader = manager.begin_transaction();
    ASSERT_TRUE(reader.ok()) << reader.error;

    ASSERT_TRUE(manager.transaction_set(writer.txn_id, "key", "tx1").ok());
    const auto own_read = manager.transaction_get(writer.txn_id, "key");
    ASSERT_TRUE(own_read.ok()) << own_read.error;
    ASSERT_TRUE(own_read.found);
    EXPECT_EQ(own_read.value, "tx1");

    const auto committed = commit_and_wait(&manager, &pump, writer.txn_id);
    ASSERT_TRUE(committed.has_value());
    ASSERT_TRUE(committed->ok()) << committed->error;
    EXPECT_EQ(store->get("key"), std::optional<std::string>("tx1"));

    const auto repeated = manager.transaction_get(reader.txn_id, "key");
    ASSERT_TRUE(repeated.ok()) << repeated.error;
    ASSERT_TRUE(repeated.found);
    EXPECT_EQ(repeated.value, "before");
    const auto stable = manager.transaction_get(reader.txn_id, "other");
    ASSERT_TRUE(stable.ok()) << stable.error;
    ASSERT_TRUE(stable.found);
    EXPECT_EQ(stable.value, "stable");

    int reader_callbacks = 0;
    const auto read_only =
        commit_and_wait(&manager, &pump, reader.txn_id, &reader_callbacks);
    ASSERT_TRUE(read_only.has_value());
    ASSERT_TRUE(read_only->ok()) << read_only->error;
    EXPECT_EQ(read_only->lsn, 0u);
    EXPECT_EQ(reader_callbacks, 1);
}

TEST(PersistenceManagerTest,
     TwoTransactionsWritingOneKeyHaveExactlyOneCommittedWinner) {
    DatabasePaths paths("minidb_transaction_conflict_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    const auto first = manager.begin_transaction();
    const auto second = manager.begin_transaction();
    ASSERT_TRUE(first.ok()) << first.error;
    ASSERT_TRUE(second.ok()) << second.error;
    ASSERT_TRUE(manager.transaction_set(first.txn_id, "same", "first").ok());
    ASSERT_TRUE(manager.transaction_set(second.txn_id, "same", "second").ok());

    const auto first_outcome = commit_and_wait(&manager, &pump, first.txn_id);
    ASSERT_TRUE(first_outcome.has_value());
    ASSERT_TRUE(first_outcome->ok()) << first_outcome->error;

    const MutationAdmission second_outcome =
        manager.commit_transaction_async(second.txn_id, {});
    EXPECT_EQ(second_outcome.status, MutationAdmissionStatus::kConflict);
    EXPECT_EQ(manager.transaction_get(second.txn_id, "same").status,
              MutationStatus::kNoTransaction);
    EXPECT_EQ(store->get("same"), std::optional<std::string>("first"));

    const PersistenceSnapshot snapshot = manager.snapshot();
    EXPECT_EQ(snapshot.transactions.started, 2u);
    EXPECT_EQ(snapshot.transactions.committed, 1u);
    EXPECT_EQ(snapshot.transactions.conflicted, 1u);
    EXPECT_EQ(snapshot.transactions.write_conflicts, 1u);
    EXPECT_EQ(snapshot.transactions.active, 0u);
}

TEST(PersistenceManagerTest,
     PendingCommitConflictWindowRejectsASecondWriterBeforeApply) {
    DatabasePaths paths("minidb_transaction_pending_conflict_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    const auto first = manager.begin_transaction();
    const auto second = manager.begin_transaction();
    ASSERT_TRUE(first.ok()) << first.error;
    ASSERT_TRUE(second.ok()) << second.error;
    ASSERT_TRUE(manager.transaction_set(first.txn_id, "pending", "one").ok());
    ASSERT_TRUE(manager.transaction_set(second.txn_id, "pending", "two").ok());

    int callback_count = 0;
    MutationOutcome first_completed;
    ASSERT_TRUE(manager
                    .commit_transaction_async(first.txn_id,
                                              [&](MutationOutcome outcome) {
                                                  first_completed =
                                                      std::move(outcome);
                                                  ++callback_count;
                                              })
                    .accepted());
    const MutationAdmission second_admission =
        manager.commit_transaction_async(second.txn_id, {});
    EXPECT_EQ(second_admission.status, MutationAdmissionStatus::kConflict);
    EXPECT_EQ(store->get("pending"), std::nullopt);

    ASSERT_TRUE(pump.RunUntil([&]() { return callback_count != 0; }));
    pump.RunPending();
    EXPECT_EQ(callback_count, 1);
    EXPECT_TRUE(first_completed.ok()) << first_completed.error;
    EXPECT_EQ(first_completed.lsn, 1u);
    EXPECT_EQ(store->get("pending"), std::optional<std::string>("one"));
}

TEST(PersistenceManagerTest,
     TransactionWriteSetFoldsSetDeleteExpireTtlAndSetClearsTtl) {
    DatabasePaths paths("minidb_transaction_write_set_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    ASSERT_TRUE(manager.set("ttl", "old").ok());
    ASSERT_TRUE(manager.expire("ttl", 5000).ok());
    ASSERT_TRUE(manager.set("deleted", "remove").ok());
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    const auto transaction = manager.begin_transaction();
    ASSERT_TRUE(transaction.ok()) << transaction.error;
    ASSERT_TRUE(
        manager.transaction_set(transaction.txn_id, "ttl", "temporary").ok());
    auto ttl = manager.transaction_ttl(transaction.txn_id, "ttl");
    ASSERT_TRUE(ttl.ok()) << ttl.error;
    EXPECT_EQ(ttl.ttl.state, KvStore::TtlState::kNoExpiry);

    ASSERT_TRUE(
        manager.transaction_expire(transaction.txn_id, "ttl", 2000).ok());
    ttl = manager.transaction_ttl(transaction.txn_id, "ttl");
    ASSERT_TRUE(ttl.ok()) << ttl.error;
    EXPECT_EQ(ttl.ttl.state, KvStore::TtlState::kExpiring);
    EXPECT_EQ(ttl.ttl.remaining_ms, 2000);

    ASSERT_TRUE(
        manager.transaction_set(transaction.txn_id, "ttl", "final").ok());
    ttl = manager.transaction_ttl(transaction.txn_id, "ttl");
    ASSERT_TRUE(ttl.ok()) << ttl.error;
    EXPECT_EQ(ttl.ttl.state, KvStore::TtlState::kNoExpiry);

    auto deleted = manager.transaction_del(transaction.txn_id, "deleted");
    ASSERT_TRUE(deleted.ok()) << deleted.error;
    EXPECT_TRUE(deleted.changed);
    deleted = manager.transaction_del(transaction.txn_id, "deleted");
    ASSERT_TRUE(deleted.ok()) << deleted.error;
    EXPECT_FALSE(deleted.changed);
    EXPECT_FALSE(manager.transaction_get(transaction.txn_id, "deleted").found);

    ASSERT_TRUE(
        manager.transaction_set(transaction.txn_id, "expired", "value").ok());
    ASSERT_TRUE(
        manager.transaction_expire(transaction.txn_id, "expired", 0).ok());
    EXPECT_FALSE(manager.transaction_get(transaction.txn_id, "expired").found);

    const std::uint64_t before_commit = manager.snapshot().last_lsn;
    const auto committed = commit_and_wait(&manager, &pump, transaction.txn_id);
    ASSERT_TRUE(committed.has_value());
    ASSERT_TRUE(committed->ok()) << committed->error;
    EXPECT_EQ(committed->lsn, before_commit + 1);

    EXPECT_EQ(store->get("ttl"), std::optional<std::string>("final"));
    EXPECT_EQ(store->ttl_info("ttl").state, KvStore::TtlState::kNoExpiry);
    EXPECT_EQ(store->get("deleted"), std::nullopt);
    EXPECT_EQ(store->get("expired"), std::nullopt);
    const PersistenceSnapshot snapshot = manager.snapshot();
    EXPECT_EQ(snapshot.transactions.commit_records, 1u);
    EXPECT_GT(snapshot.transactions.commit_bytes, 0u);
}

TEST(PersistenceManagerTest, ReadOnlyCommitAndRollbackWriteNoWalRecords) {
    DatabasePaths paths("minidb_transaction_no_wal_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    const PersistenceSnapshot before = manager.snapshot();
    const auto read_only = manager.begin_transaction();
    ASSERT_TRUE(read_only.ok()) << read_only.error;
    const auto read = manager.transaction_get(read_only.txn_id, "missing");
    ASSERT_TRUE(read.ok()) << read.error;
    EXPECT_FALSE(read.found);

    int callbacks = 0;
    const auto committed =
        commit_and_wait(&manager, &pump, read_only.txn_id, &callbacks);
    ASSERT_TRUE(committed.has_value());
    ASSERT_TRUE(committed->ok()) << committed->error;
    EXPECT_EQ(committed->lsn, 0u);
    EXPECT_EQ(callbacks, 1);

    const auto rolled_back = manager.begin_transaction();
    ASSERT_TRUE(rolled_back.ok()) << rolled_back.error;
    ASSERT_TRUE(
        manager.transaction_set(rolled_back.txn_id, "unused", "value").ok());
    const auto rollback = manager.rollback_transaction(rolled_back.txn_id);
    ASSERT_TRUE(rollback.ok()) << rollback.error;

    const PersistenceSnapshot after = manager.snapshot();
    EXPECT_EQ(after.last_lsn, before.last_lsn);
    EXPECT_EQ(after.wal.appends, before.wal.appends);
    EXPECT_EQ(after.persistence.mutations, before.persistence.mutations);
    EXPECT_EQ(after.transactions.commit_records, 0u);
    EXPECT_EQ(after.transactions.committed, 1u);
    EXPECT_EQ(after.transactions.rolled_back, 1u);
    EXPECT_EQ(after.transactions.active, 0u);
}

TEST(PersistenceManagerTest, ActiveSnapshotPinsVersionsUntilRollback) {
    DatabasePaths paths("minidb_transaction_gc_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    ASSERT_TRUE(manager.set("gc-key", "v1").ok());
    ASSERT_TRUE(manager.set("gc-key", "v2").ok());
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    const auto reader = manager.begin_transaction();
    ASSERT_TRUE(reader.ok()) << reader.error;
    ASSERT_EQ(reader.snapshot_lsn, 2u);

    bool write_done = false;
    ASSERT_TRUE(manager
                    .set_async("gc-key", "v3",
                               [&](MutationOutcome outcome) {
                                   ASSERT_TRUE(outcome.ok()) << outcome.error;
                                   write_done = true;
                               })
                    .accepted());
    ASSERT_TRUE(pump.RunUntil([&]() { return write_done; }));
    EXPECT_EQ(store->version_count(), 2u);
    const auto isolated = manager.transaction_get(reader.txn_id, "gc-key");
    ASSERT_TRUE(isolated.ok()) << isolated.error;
    ASSERT_TRUE(isolated.found);
    EXPECT_EQ(isolated.value, "v2");

    ASSERT_TRUE(manager.rollback_transaction(reader.txn_id).ok());
    write_done = false;
    ASSERT_TRUE(manager
                    .set_async("gc-key", "v4",
                               [&](MutationOutcome outcome) {
                                   ASSERT_TRUE(outcome.ok()) << outcome.error;
                                   write_done = true;
                               })
                    .accepted());
    ASSERT_TRUE(pump.RunUntil([&]() { return write_done; }));
    EXPECT_EQ(store->version_count(), 1u);
    EXPECT_EQ(store->get("gc-key"), std::optional<std::string>("v4"));
    EXPECT_GE(manager.snapshot().transactions.versions_reclaimed, 2u);
}

TEST(PersistenceManagerTest,
     VersionReservationRejectsCommitBeforeAssigningWalLsn) {
    DatabasePaths paths("minidb_transaction_version_limit_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    KvStore::Limits limits;
    limits.max_versions = 2;
    auto store = std::make_shared<KvStore>(clock, limits);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    ASSERT_TRUE(manager.set("base", "value").ok());
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    const auto transaction = manager.begin_transaction();
    ASSERT_TRUE(transaction.ok()) << transaction.error;
    ASSERT_TRUE(manager.transaction_set(transaction.txn_id, "a", "1").ok());
    ASSERT_TRUE(manager.transaction_set(transaction.txn_id, "b", "2").ok());
    const std::uint64_t before = manager.snapshot().last_lsn;
    const MutationAdmission admission =
        manager.commit_transaction_async(transaction.txn_id, {});
    EXPECT_EQ(admission.status, MutationAdmissionStatus::kResourceLimit);
    EXPECT_EQ(manager.snapshot().last_lsn, before);
    EXPECT_EQ(manager.transaction_get(transaction.txn_id, "a").status,
              MutationStatus::kNoTransaction);
    EXPECT_EQ(store->version_count(), 1u);
    EXPECT_EQ(manager.snapshot().transactions.failed, 1u);
    EXPECT_EQ(manager.snapshot().transactions.active, 0u);
}

TEST(PersistenceManagerTest,
     ShutdownRollsBackOpenTransactionsAndDrainsAcceptedCommit) {
    DatabasePaths paths("minidb_transaction_shutdown_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    const auto active = manager.begin_transaction();
    const auto committing = manager.begin_transaction();
    ASSERT_TRUE(active.ok()) << active.error;
    ASSERT_TRUE(committing.ok()) << committing.error;
    ASSERT_TRUE(
        manager.transaction_set(active.txn_id, "uncommitted", "no").ok());
    ASSERT_TRUE(
        manager.transaction_set(committing.txn_id, "committed", "yes").ok());
    ASSERT_TRUE(
        manager.commit_transaction_async(committing.txn_id, {}).accepted());

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
    EXPECT_EQ(store->get("committed"), std::optional<std::string>("yes"));
    EXPECT_EQ(store->get("uncommitted"), std::nullopt);
    const PersistenceSnapshot snapshot = manager.snapshot();
    EXPECT_EQ(snapshot.transactions.active, 0u);
    EXPECT_EQ(snapshot.transactions.committed, 1u);
    EXPECT_EQ(snapshot.transactions.rolled_back, 1u);
}

TEST(PersistenceManagerTest,
     MultiKeyTransactionRecoversAtomicallyAfterRestart) {
    DatabasePaths paths("minidb_transaction_restart_");
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

        const auto transaction = manager.begin_transaction();
        ASSERT_TRUE(transaction.ok()) << transaction.error;
        ASSERT_TRUE(
            manager.transaction_set(transaction.txn_id, "left", "l").ok());
        ASSERT_TRUE(
            manager.transaction_set(transaction.txn_id, "right", "r").ok());
        const auto committed =
            commit_and_wait(&manager, &pump, transaction.txn_id);
        ASSERT_TRUE(committed.has_value());
        ASSERT_TRUE(committed->ok()) << committed->error;
        ASSERT_EQ(committed->lsn, 1u);

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

    auto recovered = std::make_shared<KvStore>(clock);
    PersistenceManager restarted(recovered, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(restarted.open(&error)) << error;
    EXPECT_EQ(recovered->get("left"), std::optional<std::string>("l"));
    EXPECT_EQ(recovered->get("right"), std::optional<std::string>("r"));
    EXPECT_EQ(restarted.snapshot().recovery.checkpoint_lsn, 1u);
    EXPECT_EQ(restarted.snapshot().recovery.wal_records_replayed, 0u);
    EXPECT_EQ(restarted.snapshot().last_lsn, 1u);
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

std::string make_agent_session_record(const std::string& state) {
    AgentRecordCodec::SessionPayload payload;
    payload.session_expires_at_ms = 1700000600000LL;
    payload.state = state;
    std::string encoded_payload;
    std::string error;
    EXPECT_TRUE(AgentRecordCodec::encode_session_payload(
        payload, &encoded_payload, &error))
        << error;
    AgentRecordCodec::Record record;
    record.type = AgentRecordCodec::RecordType::kSession;
    record.status =
        static_cast<std::uint8_t>(AgentRecordCodec::SessionStatus::kActive);
    record.revision = 1;
    record.created_at_ms = 1700000000000LL;
    record.updated_at_ms = 1700000000000LL;
    record.payload = std::move(encoded_payload);
    std::string encoded_record;
    EXPECT_TRUE(
        AgentRecordCodec::encode_record(record, &encoded_record, &error))
        << error;
    return encoded_record;
}

std::string make_agent_tool_record(const std::string& session_id,
                                   const std::string& tool_id) {
    AgentRecordCodec::ToolPayload payload;
    payload.tool_id = tool_id;
    payload.name = "test-tool";
    payload.request = "{}";
    std::string encoded_payload;
    std::string error;
    EXPECT_TRUE(AgentRecordCodec::encode_tool_payload(payload, &encoded_payload,
                                                      &error))
        << error;
    AgentRecordCodec::Record record;
    record.type = AgentRecordCodec::RecordType::kTool;
    record.status =
        static_cast<std::uint8_t>(AgentRecordCodec::ToolStatus::kPending);
    record.revision = 1;
    record.created_at_ms = 1700000000000LL;
    record.updated_at_ms = 1700000000000LL;
    record.payload = std::move(encoded_payload);
    std::string encoded_record;
    EXPECT_TRUE(
        AgentRecordCodec::encode_record(record, &encoded_record, &error))
        << error;
    (void)session_id;
    return encoded_record;
}

TEST(PersistenceManagerTest,
     AgentMutationCommitsStateAndIdempotencyInOneTransaction) {
    DatabasePaths paths("minidb_agent_pm_single_txn_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    AgentRequest request;
    ASSERT_TRUE(parse_agent_request("SESSION_CREATE",
                                    {"s1", "60000", "ready", "create-s1"},
                                    &request, &error))
        << error;
    const auto result = mutate_agent_and_wait(&manager, &pump, request);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(result->status, AgentRequestStatus::kCompleted) << result->error;
    AgentRecordCodec::Response response;
    ASSERT_TRUE(AgentRecordCodec::decode_response(result->encoded_response,
                                                  &response, &error))
        << error;
    EXPECT_EQ(response.status, static_cast<std::uint8_t>(
                                   AgentRecordCodec::ResponseStatus::kCreated));
    EXPECT_EQ(response.lsn, 1u);

    std::string session_key;
    std::string idem_key;
    ASSERT_TRUE(AgentRecordCodec::session_key("s1", &session_key, &error))
        << error;
    ASSERT_TRUE(
        AgentRecordCodec::idempotency_key("create-s1", &idem_key, &error))
        << error;
    const std::vector<std::string> keys = {AgentRecordCodec::marker_key(),
                                           session_key, idem_key};
    for (const std::string& key : keys) {
        EXPECT_EQ(store->latest_committed_lsn(key), response.lsn)
            << "key=" << key;
        EXPECT_TRUE(store->get(key).has_value()) << "key=" << key;
    }

    const PersistenceSnapshot snapshot = manager.snapshot();
    EXPECT_EQ(snapshot.last_lsn, 1u);
    EXPECT_EQ(snapshot.next_lsn, 2u);
    EXPECT_EQ(snapshot.agent.sessions_created, 1u);
    EXPECT_EQ(snapshot.agent.idempotency_stored, 1u);
}

TEST(PersistenceManagerTest, AgentIdempotencyReplaySurvivesRestartWithoutWAL) {
    DatabasePaths paths("minidb_agent_pm_restart_replay_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    AgentRequest request;
    std::string error;
    ASSERT_TRUE(parse_agent_request("SESSION_CREATE",
                                    {"s1", "60000", "ready", "create-s1"},
                                    &request, &error))
        << error;
    std::string original_response;

    {
        auto store = std::make_shared<KvStore>(clock);
        PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
        ASSERT_TRUE(manager.open(&error)) << error;
        OwnerPump pump;
        ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error))
            << error;
        const auto result = mutate_agent_and_wait(&manager, &pump, request);
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(result->status, AgentRequestStatus::kCompleted)
            << result->error;
        original_response = result->encoded_response;

        bool shutdown_done = false;
        ASSERT_TRUE(manager.shutdown_async(
            [&](RdbSaveResult save_result, const std::string& shutdown_error) {
                EXPECT_EQ(save_result, RdbSaveResult::Ok) << shutdown_error;
                shutdown_done = true;
            },
            &error))
            << error;
        ASSERT_TRUE(pump.RunUntil([&]() { return shutdown_done; }));
    }

    auto recovered = std::make_shared<KvStore>(clock);
    PersistenceManager restarted(recovered, paths.rdb, DurabilityMode::kSync);
    ASSERT_TRUE(restarted.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(restarted.start_group_commit(pump.dispatcher(), &error))
        << error;
    const PersistenceSnapshot before = restarted.snapshot();

    const auto replay = mutate_agent_and_wait(&restarted, &pump, request);
    ASSERT_TRUE(replay.has_value());
    ASSERT_EQ(replay->status, AgentRequestStatus::kCompleted) << replay->error;
    EXPECT_EQ(replay->encoded_response, original_response);

    const PersistenceSnapshot after = restarted.snapshot();
    EXPECT_EQ(after.last_lsn, before.last_lsn);
    EXPECT_EQ(after.next_lsn, before.next_lsn);
    EXPECT_EQ(after.wal.appends, before.wal.appends);
    EXPECT_EQ(after.agent.idempotency_replays,
              before.agent.idempotency_replays + 1u);
    std::string session_key;
    ASSERT_TRUE(AgentRecordCodec::session_key("s1", &session_key, &error))
        << error;
    EXPECT_EQ(before.last_lsn, 1u);
    EXPECT_TRUE(recovered->get(session_key).has_value());
}

TEST(PersistenceManagerTest,
     AgentPendingKeyConflictReleasesAdmissionAndThenReplays) {
    DatabasePaths paths("minidb_agent_pm_pending_conflict_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    AgentRequest request;
    ASSERT_TRUE(parse_agent_request("SESSION_CREATE",
                                    {"s1", "60000", "ready", "create-s1"},
                                    &request, &error))
        << error;
    bool done = false;
    AgentResult first;
    const AgentResult admitted =
        manager.agent_mutate_async(request, [&](AgentResult result) {
            first = std::move(result);
            done = true;
        });
    ASSERT_EQ(admitted.status, AgentRequestStatus::kAccepted) << admitted.error;

    const AgentResult conflict = manager.agent_mutate_async(request, {});
    EXPECT_EQ(conflict.status, AgentRequestStatus::kConflict) << conflict.error;

    ASSERT_TRUE(pump.RunUntil([&]() { return done; }));
    ASSERT_EQ(first.status, AgentRequestStatus::kCompleted) << first.error;
    EXPECT_EQ(manager.snapshot().last_lsn, 1u);

    const auto replay = mutate_agent_and_wait(&manager, &pump, request);
    ASSERT_TRUE(replay.has_value());
    ASSERT_EQ(replay->status, AgentRequestStatus::kCompleted) << replay->error;
    AgentRecordCodec::Response response;
    ASSERT_TRUE(AgentRecordCodec::decode_response(replay->encoded_response,
                                                  &response, &error))
        << error;
    EXPECT_EQ(response.lsn, 1u);
    EXPECT_EQ(manager.snapshot().last_lsn, 1u);
    EXPECT_EQ(manager.snapshot().next_lsn, 2u);
}

TEST(PersistenceManagerTest, ReservedAgentNamespaceIsRejectedOnEveryKvPath) {
    DatabasePaths paths("minidb_agent_pm_reserved_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;

    const std::string reserved =
        std::string(AgentRecordCodec::kReservedPrefix) + "probe";
    const auto expect_invalid = [](const MutationOutcome& outcome) {
        EXPECT_EQ(outcome.status, MutationStatus::kInvalidArgument)
            << outcome.error;
        EXPECT_NE(outcome.error.find("reserved"), std::string::npos);
    };
    expect_invalid(manager.set(reserved, "value"));
    expect_invalid(manager.del(reserved));
    expect_invalid(manager.expire(reserved, 1000));

    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;
    const auto expect_admission_invalid =
        [](const MutationAdmission& admission) {
            EXPECT_EQ(admission.status,
                      MutationAdmissionStatus::kInvalidArgument)
                << admission.error;
            EXPECT_NE(admission.error.find("reserved"), std::string::npos);
        };
    expect_admission_invalid(manager.set_async(reserved, "value", {}));
    expect_admission_invalid(manager.del_async(reserved, {}));
    expect_admission_invalid(manager.expire_async(reserved, 1000, {}));

    const auto transaction = manager.begin_transaction();
    ASSERT_TRUE(transaction.ok()) << transaction.error;
    const auto expect_txn_invalid =
        [](const PersistenceManager::TransactionResult& result) {
            EXPECT_EQ(result.status, MutationStatus::kInvalidArgument)
                << result.error;
            EXPECT_NE(result.error.find("reserved"), std::string::npos);
        };
    expect_txn_invalid(
        manager.transaction_set(transaction.txn_id, reserved, "value"));
    expect_txn_invalid(manager.transaction_get(transaction.txn_id, reserved));
    expect_txn_invalid(manager.transaction_del(transaction.txn_id, reserved));
    expect_txn_invalid(
        manager.transaction_expire(transaction.txn_id, reserved, 1000));
    expect_txn_invalid(manager.transaction_ttl(transaction.txn_id, reserved));
    EXPECT_TRUE(manager.rollback_transaction(transaction.txn_id).ok());

    const PersistenceSnapshot snapshot = manager.snapshot();
    EXPECT_EQ(snapshot.last_lsn, 0u);
    EXPECT_EQ(snapshot.next_lsn, 1u);
    EXPECT_FALSE(store->get(reserved).has_value());
}

TEST(PersistenceManagerTest, AgentStartupFailsClosedOnCorruptNamespace) {
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    std::string error;
    std::string session_key;
    ASSERT_TRUE(AgentRecordCodec::session_key("s1", &session_key, &error))
        << error;
    std::string tool_key;
    ASSERT_TRUE(AgentRecordCodec::tool_key("s1", "t1", &tool_key, &error))
        << error;

    {
        DatabasePaths paths("minidb_agent_pm_no_marker_");
        KvStore legacy(clock);
        legacy.set(session_key, make_agent_session_record("ready"));
        RdbManager rdb(paths.rdb);
        ASSERT_EQ(rdb.save_checked(legacy), RdbSaveResult::Ok);
        auto store = std::make_shared<KvStore>(clock);
        PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
        ASSERT_FALSE(manager.open(&error));
        EXPECT_NE(error.find("marker"), std::string::npos) << error;
        EXPECT_TRUE(manager.failed());
    }

    {
        DatabasePaths paths("minidb_agent_pm_orphan_");
        KvStore legacy(clock);
        legacy.set(AgentRecordCodec::marker_key(),
                   AgentRecordCodec::encode_marker());
        legacy.set(tool_key, make_agent_tool_record("s1", "t1"));
        RdbManager rdb(paths.rdb);
        ASSERT_EQ(rdb.save_checked(legacy), RdbSaveResult::Ok);
        auto store = std::make_shared<KvStore>(clock);
        PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
        ASSERT_FALSE(manager.open(&error));
        EXPECT_NE(error.find("orphan"), std::string::npos) << error;
        EXPECT_TRUE(manager.failed());
    }
}

TEST(PersistenceManagerTest, AgentSnapshotStatsComeFromOneCommittedPoint) {
    DatabasePaths paths("minidb_agent_pm_stats_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    AgentRequest create;
    ASSERT_TRUE(parse_agent_request("SESSION_CREATE",
                                    {"s1", "60000", "ready", "create-s1"},
                                    &create, &error))
        << error;
    const auto created = mutate_agent_and_wait(&manager, &pump, create);
    ASSERT_TRUE(created.has_value());
    ASSERT_EQ(created->status, AgentRequestStatus::kCompleted)
        << created->error;

    AgentRequest close;
    ASSERT_TRUE(parse_agent_request(
        "SESSION_CLOSE", {"s1", "1", "COMPLETED", "close-s1"}, &close, &error))
        << error;
    const auto closed = mutate_agent_and_wait(&manager, &pump, close);
    ASSERT_TRUE(closed.has_value());
    ASSERT_EQ(closed->status, AgentRequestStatus::kCompleted) << closed->error;

    const PersistenceSnapshot snapshot = manager.snapshot();
    EXPECT_EQ(snapshot.last_lsn, 2u);
    EXPECT_EQ(snapshot.agent.sessions_stored, 1u);
    EXPECT_EQ(snapshot.agent.sessions_live, 0u);
    EXPECT_EQ(snapshot.agent.sessions_created, 1u);
    EXPECT_EQ(snapshot.agent.sessions_completed, 1u);
    EXPECT_EQ(snapshot.agent.idempotency_stored, 2u);
    EXPECT_EQ(snapshot.agent.logical_bytes_max,
              AgentRecordCodec::Limits::kMaxLogicalBytes);
    EXPECT_GE(snapshot.agent.gc_total_us, snapshot.agent.gc_max_us);
}

TEST(PersistenceManagerTest,
     AgentGcDeletesSessionAnd128ToolsInOneBoundedTransaction) {
    DatabasePaths paths("minidb_agent_pm_gc_128_tools_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    ASSERT_TRUE(create_terminal_session_group(
        &manager, &pump, "s1", AgentRecordCodec::Limits::kMaxToolsPerSession));
    const PersistenceSnapshot before = manager.snapshot();
    ASSERT_EQ(before.agent.sessions_stored, 1u);
    ASSERT_EQ(before.agent.tools_stored,
              AgentRecordCodec::Limits::kMaxToolsPerSession);
    ASSERT_EQ(before.agent.idempotency_stored, 130u);

    ASSERT_TRUE(manager.agent_gc_async(&error)) << error;
    ASSERT_TRUE(pump.RunUntil(
        [&]() { return manager.snapshot().agent.gc_runs == 1u; }));

    const PersistenceSnapshot after = manager.snapshot();
    EXPECT_EQ(after.agent.gc_runs, 1u);
    EXPECT_EQ(after.agent.gc_candidates, 1u);
    EXPECT_EQ(after.agent.gc_deleted,
              1u + AgentRecordCodec::Limits::kMaxToolsPerSession);
    EXPECT_EQ(after.agent.gc_failures, 0u);
    EXPECT_EQ(after.agent.sessions_stored, 0u);
    EXPECT_EQ(after.agent.tools_stored, 0u);
    EXPECT_EQ(after.agent.idempotency_stored, before.agent.idempotency_stored);

    std::string session_key;
    ASSERT_TRUE(AgentRecordCodec::session_key("s1", &session_key, &error))
        << error;
    EXPECT_FALSE(store->get(session_key).has_value());
    for (std::size_t index = 0;
         index < AgentRecordCodec::Limits::kMaxToolsPerSession; ++index) {
        std::string tool_id = "t";
        const std::string digits = std::to_string(index);
        tool_id.append(3 - std::min<std::size_t>(3, digits.size()), '0');
        tool_id += digits;
        std::string tool_key;
        ASSERT_TRUE(
            AgentRecordCodec::tool_key("s1", tool_id, &tool_key, &error))
            << error;
        EXPECT_FALSE(store->get(tool_key).has_value()) << tool_id;
    }
    EXPECT_TRUE(store->get(AgentRecordCodec::marker_key()).has_value());
}

TEST(PersistenceManagerTest,
     AgentGcNeverSplitsASessionGroupAcrossTransactions) {
    DatabasePaths paths("minidb_agent_pm_gc_group_limit_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    ASSERT_TRUE(create_terminal_session_group(
        &manager, &pump, "a", AgentRecordCodec::Limits::kMaxToolsPerSession));
    ASSERT_TRUE(create_terminal_session_group(&manager, &pump, "b", 2));

    ASSERT_TRUE(manager.agent_gc_async(&error)) << error;
    const bool first_gc_ran =
        pump.RunUntil([&]() { return manager.snapshot().agent.gc_runs == 1u; });
    PersistenceSnapshot snapshot = manager.snapshot();
    ASSERT_TRUE(first_gc_ran)
        << "runs=" << snapshot.agent.gc_runs
        << " candidates=" << snapshot.agent.gc_candidates
        << " deleted=" << snapshot.agent.gc_deleted
        << " failures=" << snapshot.agent.gc_failures
        << " sessions_live=" << snapshot.agent.sessions_live
        << " wal_appends=" << snapshot.wal.appends
        << " manager_failed=" << manager.failed();
    EXPECT_EQ(snapshot.agent.gc_candidates, 1u);
    EXPECT_EQ(snapshot.agent.gc_deleted, 129u);
    EXPECT_EQ(snapshot.agent.sessions_stored, 1u);
    EXPECT_EQ(snapshot.agent.tools_stored, 2u);

    std::string session_key;
    std::string tool_key;
    ASSERT_TRUE(AgentRecordCodec::session_key("b", &session_key, &error))
        << error;
    ASSERT_TRUE(AgentRecordCodec::tool_key("b", "t000", &tool_key, &error))
        << error;
    EXPECT_TRUE(store->get(session_key).has_value());
    EXPECT_TRUE(store->get(tool_key).has_value());

    ASSERT_TRUE(manager.agent_gc_async(&error)) << error;
    ASSERT_TRUE(pump.RunUntil(
        [&]() { return manager.snapshot().agent.gc_runs == 2u; }));
    snapshot = manager.snapshot();
    EXPECT_EQ(snapshot.agent.gc_candidates, 2u);
    EXPECT_EQ(snapshot.agent.gc_deleted, 132u);
    EXPECT_EQ(snapshot.agent.sessions_stored, 0u);
    EXPECT_EQ(snapshot.agent.tools_stored, 0u);
    EXPECT_FALSE(store->get(session_key).has_value());
    EXPECT_FALSE(store->get(tool_key).has_value());
}

TEST(PersistenceManagerTest, AgentGcSkipsPendingGroupsWithoutLosingCandidates) {
    DatabasePaths paths("minidb_agent_pm_gc_pending_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    AgentRequest create_record;
    ASSERT_TRUE(parse_agent_request(
        "RECORD_CAS", {"r1", "0", "value-1", "gc-pending", "60000"},
        &create_record, &error))
        << error;
    const auto created = mutate_agent_and_wait(&manager, &pump, create_record);
    ASSERT_TRUE(created.has_value());
    ASSERT_EQ(created->status, AgentRequestStatus::kCompleted)
        << created->error;

    ASSERT_TRUE(create_terminal_session_group(&manager, &pump, "s1", 1));
    clock->advance(60000);

    AgentRequest update_record;
    ASSERT_TRUE(parse_agent_request(
        "RECORD_CAS", {"r1", "1", "value-2", "gc-pending", "60000"},
        &update_record, &error))
        << error;

    bool update_done = false;
    AgentResult update_result;
    const AgentResult update_admission =
        manager.agent_mutate_async(update_record, [&](AgentResult result) {
            update_result = std::move(result);
            update_done = true;
        });
    ASSERT_EQ(update_admission.status, AgentRequestStatus::kAccepted)
        << update_admission.error;
    ASSERT_FALSE(update_done);
    ASSERT_EQ(manager.snapshot().agent.gc_runs, 0u);

    // The expired idempotency key from the first record write is now pending
    // again for the record update. GC must skip that group without starving
    // the independently removable terminal session group.
    ASSERT_TRUE(manager.agent_gc_async(&error)) << error;
    const bool completed = pump.RunUntil([&]() {
        return update_done && manager.snapshot().agent.gc_runs == 1u;
    });
    const PersistenceSnapshot snapshot = manager.snapshot();
    ASSERT_TRUE(completed) << "update_done=" << update_done
                           << " runs=" << snapshot.agent.gc_runs
                           << " candidates=" << snapshot.agent.gc_candidates
                           << " deleted=" << snapshot.agent.gc_deleted
                           << " failures=" << snapshot.agent.gc_failures
                           << " sessions_live=" << snapshot.agent.sessions_live
                           << " manager_failed=" << manager.failed();

    ASSERT_EQ(update_result.status, AgentRequestStatus::kCompleted)
        << update_result.error;
    AgentRecordCodec::Response update_response;
    ASSERT_TRUE(AgentRecordCodec::decode_response(
        update_result.encoded_response, &update_response, &error))
        << error;
    EXPECT_EQ(update_response.status,
              static_cast<std::uint8_t>(AgentRecordCodec::ResponseStatus::kOk));
    EXPECT_EQ(update_response.revision, 2u);

    EXPECT_EQ(snapshot.agent.gc_runs, 1u);
    EXPECT_EQ(snapshot.agent.gc_candidates, 1u);
    EXPECT_EQ(snapshot.agent.gc_deleted, 2u);
    EXPECT_EQ(snapshot.agent.gc_failures, 0u);
    EXPECT_EQ(snapshot.agent.sessions_stored, 0u);
    EXPECT_EQ(snapshot.agent.tools_stored, 0u);

    std::string session_key;
    std::string tool_key;
    std::string generic_key;
    std::string idempotency_key;
    ASSERT_TRUE(AgentRecordCodec::session_key("s1", &session_key, &error))
        << error;
    ASSERT_TRUE(AgentRecordCodec::tool_key("s1", "t000", &tool_key, &error))
        << error;
    ASSERT_TRUE(AgentRecordCodec::generic_key("r1", &generic_key, &error))
        << error;
    ASSERT_TRUE(AgentRecordCodec::idempotency_key("gc-pending",
                                                  &idempotency_key, &error))
        << error;
    EXPECT_FALSE(store->get(session_key).has_value());
    EXPECT_FALSE(store->get(tool_key).has_value());
    EXPECT_TRUE(store->get(generic_key).has_value());
    EXPECT_TRUE(store->get(idempotency_key).has_value());
}

TEST(PersistenceManagerTest, AgentGcIsSuppressedDuringShutdown) {
    DatabasePaths paths("minidb_agent_pm_gc_shutdown_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    OwnerPump pump;
    ASSERT_TRUE(manager.start_group_commit(pump.dispatcher(), &error)) << error;

    ASSERT_TRUE(create_terminal_session_group(&manager, &pump, "s1", 1));
    const PersistenceSnapshot before = manager.snapshot();

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

    // Once shutdown has been admitted, a timer tick must become a no-op rather
    // than racing the final checkpoint or deleting state after it is captured.
    ASSERT_TRUE(manager.agent_gc_async(&error)) << error;
    ASSERT_TRUE(pump.RunUntil([&]() { return shutdown_done; }));
    ASSERT_EQ(shutdown_result, RdbSaveResult::Ok) << shutdown_error;

    const PersistenceSnapshot after = manager.snapshot();
    EXPECT_EQ(after.agent.gc_runs, before.agent.gc_runs);
    EXPECT_EQ(after.agent.gc_candidates, before.agent.gc_candidates);
    EXPECT_EQ(after.agent.gc_deleted, before.agent.gc_deleted);
    EXPECT_EQ(after.agent.gc_failures, before.agent.gc_failures);
    EXPECT_EQ(after.agent.sessions_stored, 1u);
    EXPECT_EQ(after.agent.tools_stored, 1u);

    std::string session_key;
    ASSERT_TRUE(AgentRecordCodec::session_key("s1", &session_key, &error))
        << error;
    EXPECT_TRUE(store->get(session_key).has_value());
}

} // namespace
} // namespace minidb
