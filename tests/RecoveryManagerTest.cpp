#include <gtest/gtest.h>

#include "../src/storage/RecoveryManager.h"
#include "../src/storage/RdbManager.h"
#include "../src/storage/PersistenceManager.h"
#include "FaultWalFileOperations.h"
#include "RdbTestUtil.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace minidb {
namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

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

void seed_with_wal(const std::string& rdb_path,
                   const std::shared_ptr<minidb_test::FrozenClock>& clock,
                   bool take_checkpoint) {
    auto store = std::make_shared<KvStore>(clock);
    PersistenceManager manager(store, rdb_path, DurabilityMode::kSync);
    std::string error;
    require(manager.open(&error), "open seed database: " + error);
    require(manager.set("base", "value").ok(), "seed base");
    require(manager.set("delete-me", "old").ok(), "seed delete");
    require(manager.del("delete-me").ok(), "seed del");
    require(manager.expire("base", 60000).ok(), "seed expire");
    if (take_checkpoint) {
        require(manager.checkpoint(&error) == RdbSaveResult::Ok,
                "seed checkpoint: " + error);
        require(manager.set("after-checkpoint", "tail").ok(),
                "seed post-checkpoint write");
    }
}

std::vector<WalSegmentInfo> segments(const std::string& wal_dir) {
    std::vector<WalSegmentInfo> result;
    std::string error;
    require(list_wal_segments(wal_dir, &result, &error),
            "list WAL segments: " + error);
    return result;
}

void append_prefix(const std::string& path, std::size_t prefix_bytes) {
    WalRecord record;
    record.lsn = 999;
    record.op = WalOp::kSet;
    record.key = "torn";
    record.value = "record";
    const std::vector<std::uint8_t> bytes = WalFormat::encode(record);
    require(!bytes.empty(), "encode torn test record");
    require(prefix_bytes <= bytes.size(), "torn prefix too long");
    std::ofstream output(path, std::ios::binary | std::ios::app);
    require(output.is_open(), "open segment for torn append");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(prefix_bytes));
    output.close();
    require(output.good(), "append torn prefix");
}

void flip_payload_byte(const std::string& path) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    require(file.is_open(), "open WAL segment for corruption");
    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    require(size >= 40, "WAL segment unexpectedly short");
    file.seekg(WalFormat::HEADER_SIZE + 1, std::ios::beg);
    char byte = 0;
    file.read(&byte, 1);
    require(file.good(), "read WAL payload for corruption");
    byte ^= 0x5a;
    file.seekp(WalFormat::HEADER_SIZE + 1, std::ios::beg);
    file.write(&byte, 1);
    file.close();
    require(file.good(), "write WAL corruption");
}

TEST(RecoveryManagerTest, FreshDatabaseClearsTargetAndNeedsBootstrap) {
    DatabasePaths paths("minidb_recovery_fresh_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    KvStore target(clock);
    target.set("stale", "must-disappear");

    RecoveryManager recovery(paths.rdb);
    std::string error;
    ASSERT_EQ(recovery.recover(target, &error), RecoveryStatus::kOk) << error;
    EXPECT_FALSE(target.get("stale").has_value());
    EXPECT_EQ(target.size(), 0u);
    EXPECT_TRUE(recovery.stats().needs_bootstrap);
    EXPECT_FALSE(recovery.stats().manifest_present);
    EXPECT_EQ(recovery.stats().next_lsn, 1u);
}

TEST(RecoveryManagerTest, LegacyRdbIsImportedOnlyWithoutWalSidecars) {
    DatabasePaths paths("minidb_recovery_legacy_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto source = std::make_shared<KvStore>(clock);
    source->set("legacy-key", "legacy-value");
    source->set_with_expiry("legacy-ttl", "ttl-value",
                            1700000000000LL + 120000);
    RdbManager rdb(paths.rdb);
    ASSERT_TRUE(rdb.save(*source));

    KvStore target(clock);
    target.set("stale", "must-disappear");
    RecoveryManager recovery(paths.rdb);
    std::string error;
    ASSERT_EQ(recovery.recover(target, &error), RecoveryStatus::kOk) << error;
    EXPECT_EQ(target.get("legacy-key"),
              std::optional<std::string>("legacy-value"));
    EXPECT_EQ(target.get("legacy-ttl"),
              std::optional<std::string>("ttl-value"));
    EXPECT_TRUE(recovery.stats().legacy_import);
    EXPECT_TRUE(recovery.stats().needs_bootstrap);
    EXPECT_FALSE(recovery.stats().manifest_present);
    EXPECT_EQ(recovery.stats().rdb_records, 2u);
}

TEST(RecoveryManagerTest, CorruptRdbDoesNotPublishPartialState) {
    DatabasePaths paths("minidb_recovery_bad_rdb_");
    const std::vector<std::uint8_t> garbage = {'n', 'o', 't', 'r', 'd', 'b'};
    ASSERT_TRUE(minidb_test::write_file(paths.rdb, garbage));

    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    KvStore target(clock);
    target.set("sentinel", "kept");
    RecoveryManager recovery(paths.rdb);
    std::string error;
    EXPECT_EQ(recovery.recover(target, &error), RecoveryStatus::kCorrupt);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(target.get("sentinel"), std::optional<std::string>("kept"));
    EXPECT_EQ(target.size(), 1u);
}

TEST(RecoveryManagerTest, ManifestWithoutRdbIsRejected) {
    DatabasePaths paths("minidb_recovery_missing_rdb_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    seed_with_wal(paths.rdb, clock, false);
    ASSERT_TRUE(fs::remove(paths.rdb));

    KvStore target(clock);
    target.set("sentinel", "kept");
    RecoveryManager recovery(paths.rdb);
    std::string error;
    EXPECT_EQ(recovery.recover(target, &error), RecoveryStatus::kCorrupt);
    EXPECT_NE(error.find("RDB"), std::string::npos);
    EXPECT_EQ(target.get("sentinel"), std::optional<std::string>("kept"));
}

TEST(RecoveryManagerTest, ManifestReferencingMissingWalSegmentIsRejected) {
    DatabasePaths paths("minidb_recovery_missing_wal_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    seed_with_wal(paths.rdb, clock, true);
    const std::vector<WalSegmentInfo> live = segments(paths.wal_dir);
    ASSERT_FALSE(live.empty());
    ASSERT_TRUE(fs::remove(live.back().path));

    KvStore target(clock);
    target.set("sentinel", "kept");
    RecoveryManager recovery(paths.rdb);
    std::string error;
    EXPECT_EQ(recovery.recover(target, &error), RecoveryStatus::kCorrupt);
    EXPECT_NE(error.find("missing WAL segment"), std::string::npos);
    EXPECT_EQ(target.get("sentinel"), std::optional<std::string>("kept"));
}

TEST(RecoveryManagerTest, MiddleWalCorruptionIsNotSkipped) {
    DatabasePaths paths("minidb_recovery_corrupt_wal_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    seed_with_wal(paths.rdb, clock, false);
    const std::vector<WalSegmentInfo> live = segments(paths.wal_dir);
    ASSERT_EQ(live.size(), 1u);
    flip_payload_byte(live.front().path);

    KvStore target(clock);
    target.set("sentinel", "kept");
    RecoveryManager recovery(paths.rdb);
    std::string error;
    EXPECT_EQ(recovery.recover(target, &error), RecoveryStatus::kCorrupt);
    EXPECT_EQ(target.get("sentinel"), std::optional<std::string>("kept"));
}

TEST(RecoveryManagerTest, TornTailIsTruncatedAndFsynced) {
    DatabasePaths paths("minidb_recovery_torn_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    seed_with_wal(paths.rdb, clock, false);
    const std::vector<WalSegmentInfo> live = segments(paths.wal_dir);
    ASSERT_EQ(live.size(), 1u);
    const std::uint64_t valid_size = live.front().size;
    append_prefix(live.front().path, 13);
    ASSERT_GT(fs::file_size(live.front().path), valid_size);

    KvStore target(clock);
    RecoveryManager recovery(paths.rdb);
    std::string error;
    ASSERT_EQ(recovery.recover(target, &error), RecoveryStatus::kOk) << error;
    EXPECT_EQ(fs::file_size(live.front().path), valid_size);
    EXPECT_EQ(recovery.stats().truncated_bytes, 13u);
    EXPECT_EQ(target.get("base"), std::optional<std::string>("value"));
    EXPECT_EQ(target.get("delete-me").has_value(), false);
    EXPECT_EQ(target.get("torn").has_value(), false);
}

TEST(RecoveryManagerTest,
     ObsoletePrefixResidueIsIgnoredButNotSkippedInLiveChain) {
    DatabasePaths paths("minidb_recovery_residue_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    seed_with_wal(paths.rdb, clock, true);
    const std::string residue = paths.wal_dir + "/" + wal_segment_name(1);
    ASSERT_TRUE(minidb_test::write_file(residue, {'j', 'u', 'n', 'k'}));

    KvStore target(clock);
    RecoveryManager recovery(paths.rdb);
    std::string error;
    ASSERT_EQ(recovery.recover(target, &error), RecoveryStatus::kOk) << error;
    EXPECT_EQ(recovery.stats().ignored_residue_segments, 1u);
    EXPECT_EQ(target.get("after-checkpoint"),
              std::optional<std::string>("tail"));
    EXPECT_EQ(target.get("base"), std::optional<std::string>("value"));
    EXPECT_EQ(target.get("delete-me").has_value(), false);
}

TEST(RecoveryManagerTest, WalWithoutManifestRefusesAutomaticMigration) {
    DatabasePaths paths("minidb_recovery_no_manifest_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    seed_with_wal(paths.rdb, clock, false);
    ASSERT_TRUE(fs::remove(paths.manifest));

    KvStore target(clock);
    target.set("sentinel", "kept");
    RecoveryManager recovery(paths.rdb);
    std::string error;
    EXPECT_EQ(recovery.recover(target, &error), RecoveryStatus::kConflict);
    EXPECT_NE(error.find("without a checkpoint manifest"), std::string::npos);
    EXPECT_EQ(target.get("sentinel"), std::optional<std::string>("kept"));
}

TEST(RecoveryManagerTest, LsnStatsAndAbsoluteTtlSurviveReplay) {
    DatabasePaths paths("minidb_recovery_stats_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    auto source = std::make_shared<KvStore>(clock);
    PersistenceManager manager(source, paths.rdb, DurabilityMode::kSync);
    std::string error;
    ASSERT_TRUE(manager.open(&error)) << error;
    ASSERT_TRUE(manager.set("plain", "value").ok());
    ASSERT_TRUE(manager.set("ttl", "value").ok());
    ASSERT_TRUE(manager.expire("ttl", 90000).ok());
    ASSERT_EQ(manager.snapshot().last_lsn, 3u);

    clock->advance(10000);
    KvStore target(clock);
    RecoveryManager recovery(paths.rdb);
    ASSERT_EQ(recovery.recover(target, &error), RecoveryStatus::kOk) << error;
    EXPECT_EQ(recovery.stats().next_lsn, 4u);
    EXPECT_EQ(recovery.stats().last_lsn, 3u);
    EXPECT_EQ(recovery.stats().wal_records_replayed, 3u);
    EXPECT_EQ(recovery.stats().checkpoint_lsn, 0u);
    EXPECT_EQ(target.get("plain"), std::optional<std::string>("value"));
    const auto ttl = target.ttl_info("ttl");
    ASSERT_EQ(ttl.state, KvStore::TtlState::kExpiring);
    EXPECT_GT(ttl.remaining_ms, 79000);
    EXPECT_LE(ttl.remaining_ms, 80000);

    clock->advance(80001);
    EXPECT_FALSE(target.get("ttl").has_value());
    EXPECT_EQ(target.get("plain"), std::optional<std::string>("value"));
}

TEST(RecoveryManagerTest, CheckpointStatsDescribeSnapshotAndLiveWal) {
    DatabasePaths paths("minidb_recovery_checkpoint_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    seed_with_wal(paths.rdb, clock, true);

    KvStore target(clock);
    RecoveryManager recovery(paths.rdb);
    std::string error;
    ASSERT_EQ(recovery.recover(target, &error), RecoveryStatus::kOk) << error;
    EXPECT_TRUE(recovery.stats().manifest_present);
    EXPECT_GE(recovery.stats().checkpoint_lsn, 1u);
    EXPECT_EQ(recovery.stats().rdb_records, 1u);
    EXPECT_EQ(recovery.stats().wal_records_replayed, 1u);
    EXPECT_EQ(recovery.stats().last_lsn, recovery.stats().checkpoint_lsn + 1u);
    EXPECT_EQ(target.get("after-checkpoint"),
              std::optional<std::string>("tail"));
}

TEST(RecoveryManagerTest,
     OldManifestReplaysSetDeleteAndAbsoluteExpireIdempotently) {
    DatabasePaths paths("minidb_recovery_old_manifest_idempotent_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    {
        auto store = std::make_shared<KvStore>(clock);
        PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
        std::string error;
        ASSERT_TRUE(manager.open(&error)) << error;
        ASSERT_TRUE(manager.set("seed", "base").ok());
        ASSERT_EQ(manager.checkpoint(&error), RdbSaveResult::Ok) << error;

        ASSERT_TRUE(manager.set("set-after", "value").ok());
        ASSERT_TRUE(manager.del("seed").ok());
        ASSERT_TRUE(manager.set("ttl", "kept").ok());
        ASSERT_TRUE(manager.expire("ttl", 30000).ok());
        ASSERT_TRUE(manager.set("expired", "gone").ok());
        ASSERT_TRUE(manager.expire("expired", 0).ok());

        clock->advance(5000);
        // Simulate a rename of a newer RDB snapshot immediately before the
        // manifest publication: the old manifest is still authoritative, so
        // recovery must tolerate replaying an older WAL prefix over it.
        RdbManager newer_snapshot(paths.rdb);
        ASSERT_TRUE(newer_snapshot.save(*store));

        CheckpointManifest manifest;
        ASSERT_EQ(load_manifest(paths.manifest, &manifest, &error),
                  ManifestLoadStatus::kOk)
            << error;
        EXPECT_EQ(manifest.checkpoint_lsn, 1u);
    }

    KvStore recovered(clock);
    RecoveryManager recovery(paths.rdb);
    std::string error;
    ASSERT_EQ(recovery.recover(recovered, &error), RecoveryStatus::kOk)
        << error;
    EXPECT_EQ(recovered.get("seed").has_value(), false);
    EXPECT_EQ(recovered.get("set-after"), std::optional<std::string>("value"));
    EXPECT_EQ(recovered.get("expired").has_value(), false);
    const auto ttl = recovered.ttl_info("ttl");
    ASSERT_EQ(ttl.state, KvStore::TtlState::kExpiring);
    EXPECT_GT(ttl.remaining_ms, 24000);
    EXPECT_LE(ttl.remaining_ms, 25000);

    clock->advance(25001);
    EXPECT_FALSE(recovered.get("ttl").has_value());
    EXPECT_EQ(recovered.get("set-after"), std::optional<std::string>("value"));
}

} // namespace
} // namespace minidb
