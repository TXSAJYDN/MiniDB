#include <gtest/gtest.h>

#include "../src/storage/RecoveryManager.h"
#include "../src/storage/RdbManager.h"
#include "../src/storage/PersistenceManager.h"
#include "../src/storage/WalFormat.h"
#include "FaultWalFileOperations.h"
#include "RdbTestUtil.h"

#include <cstring>
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

void store_u16(std::uint8_t* data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>(value >> 8);
    data[1] = static_cast<std::uint8_t>(value);
}

void store_u32(std::uint8_t* data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>(value >> 24);
    data[1] = static_cast<std::uint8_t>(value >> 16);
    data[2] = static_cast<std::uint8_t>(value >> 8);
    data[3] = static_cast<std::uint8_t>(value);
}

std::uint32_t load_u32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

void refresh_record_crc(std::vector<std::uint8_t>* bytes) {
    require(bytes->size() >= WalFormat::HEADER_SIZE + WalFormat::CHECKSUM_SIZE,
            "record is too short for its checksum");
    const std::uint32_t key_len = load_u32(bytes->data() + 18);
    const std::uint32_t value_len = load_u32(bytes->data() + 22);
    const std::size_t record_size =
        WalFormat::HEADER_SIZE + key_len + value_len + WalFormat::CHECKSUM_SIZE;
    require(record_size == bytes->size(),
            "record bytes must contain exactly one record");
    const std::size_t crc_offset = record_size - WalFormat::CHECKSUM_SIZE;
    store_u32(bytes->data() + crc_offset,
              WalFormat::crc32c(bytes->data(), crc_offset));
}

std::vector<std::uint8_t> encode_record(const WalRecord& record) {
    std::string error;
    std::vector<std::uint8_t> bytes = WalFormat::encode(record, &error);
    require(!bytes.empty(), "encode test WAL record: " + error);
    return bytes;
}

WalRecord transaction_record(std::uint64_t lsn,
                             std::vector<WalTxnOp> operations) {
    std::string error;
    const std::vector<std::uint8_t> payload =
        WalFormat::encode_transaction(operations, &error);
    require(!payload.empty(), "encode test transaction: " + error);
    WalRecord record;
    record.lsn = lsn;
    record.op = WalOp::kTxn;
    record.key = "txn";
    record.value.assign(reinterpret_cast<const char*>(payload.data()),
                        payload.size());
    return record;
}

void append_bytes(const std::string& path,
                  const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::app);
    require(output.is_open(), "open WAL segment for append");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(output.good(), "append WAL bytes");
}

void append_prefix(const std::string& path,
                   const std::vector<std::uint8_t>& bytes,
                   std::size_t prefix_bytes) {
    require(prefix_bytes <= bytes.size(), "record prefix too long");
    std::ofstream output(path, std::ios::binary | std::ios::app);
    require(output.is_open(), "open WAL segment for partial append");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(prefix_bytes));
    output.close();
    require(output.good(), "append partial WAL record");
}

void append_complete_record(const std::string& path, const WalRecord& record) {
    append_bytes(path, encode_record(record));
}

std::size_t transaction_payload_offset(const WalRecord& record) {
    return WalFormat::HEADER_SIZE + record.key.size();
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
    WalRecord torn;
    torn.lsn = 999;
    torn.op = WalOp::kSet;
    torn.key = "torn";
    torn.value = "record";
    append_prefix(live.front().path, encode_record(torn), 13);
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
    EXPECT_EQ(target.current_lsn(), 3u);
    EXPECT_EQ(target.latest_committed_lsn("plain"), 1u);
    EXPECT_EQ(target.latest_committed_lsn("ttl"), 3u);
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

TEST(RecoveryManagerTest, TransactionRecoveryPublishesEveryKeyAtOneLsn) {
    DatabasePaths paths("minidb_recovery_txn_atomic_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    seed_with_wal(paths.rdb, clock, false);
    const std::vector<WalSegmentInfo> live = segments(paths.wal_dir);
    ASSERT_EQ(live.size(), 1u);

    WalTxnOp set_a;
    set_a.op = WalOp::kSet;
    set_a.key = "txn-a";
    set_a.value = "one";
    WalTxnOp set_b;
    set_b.op = WalOp::kSet;
    set_b.key = "txn-b";
    set_b.value = "two";
    append_complete_record(live.front().path,
                           transaction_record(5, {set_a, set_b}));

    KvStore target(clock);
    RecoveryManager recovery(paths.rdb);
    std::string error;
    ASSERT_EQ(recovery.recover(target, &error), RecoveryStatus::kOk) << error;
    EXPECT_EQ(recovery.stats().last_lsn, 5u);
    EXPECT_EQ(recovery.stats().next_lsn, 6u);
    EXPECT_EQ(recovery.stats().wal_records_scanned, 5u);
    EXPECT_EQ(recovery.stats().wal_records_replayed, 5u);
    EXPECT_EQ(target.current_lsn(), 5u);
    EXPECT_EQ(target.latest_committed_lsn("txn-a"), 5u);
    EXPECT_EQ(target.latest_committed_lsn("txn-b"), 5u);
    EXPECT_EQ(target.get("txn-a"), std::optional<std::string>("one"));
    EXPECT_EQ(target.get("txn-b"), std::optional<std::string>("two"));
    EXPECT_EQ(target.get("base"), std::optional<std::string>("value"));
    EXPECT_EQ(target.get("delete-me"), std::nullopt);
}

TEST(RecoveryManagerTest, RecoveryReadsMixedLegacyAndV2Records) {
    DatabasePaths paths("minidb_recovery_mixed_wal_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    seed_with_wal(paths.rdb, clock, false);
    const std::vector<WalSegmentInfo> live = segments(paths.wal_dir);
    ASSERT_EQ(live.size(), 1u);

    WalRecord legacy;
    legacy.lsn = 5;
    legacy.op = WalOp::kSet;
    legacy.key = "legacy-tail";
    legacy.value = "v1";
    std::vector<std::uint8_t> legacy_bytes = encode_record(legacy);
    store_u16(legacy_bytes.data() + 4, WalFormat::LEGACY_VERSION);
    refresh_record_crc(&legacy_bytes);
    append_bytes(live.front().path, legacy_bytes);

    WalTxnOp transaction_set;
    transaction_set.op = WalOp::kSet;
    transaction_set.key = "v2-tail";
    transaction_set.value = "v2";
    append_complete_record(live.front().path,
                           transaction_record(6, {transaction_set}));

    KvStore target(clock);
    RecoveryManager recovery(paths.rdb);
    std::string error;
    ASSERT_EQ(recovery.recover(target, &error), RecoveryStatus::kOk) << error;
    EXPECT_EQ(recovery.stats().last_lsn, 6u);
    EXPECT_EQ(recovery.stats().wal_records_replayed, 6u);
    EXPECT_EQ(target.get("legacy-tail"), std::optional<std::string>("v1"));
    EXPECT_EQ(target.get("v2-tail"), std::optional<std::string>("v2"));
    EXPECT_EQ(target.latest_committed_lsn("legacy-tail"), 5u);
    EXPECT_EQ(target.latest_committed_lsn("v2-tail"), 6u);
}

TEST(RecoveryManagerTest, LegacyVersionCannotContainATransactionRecord) {
    DatabasePaths paths("minidb_recovery_v1_txn_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    seed_with_wal(paths.rdb, clock, false);
    const std::vector<WalSegmentInfo> live = segments(paths.wal_dir);
    ASSERT_EQ(live.size(), 1u);

    WalTxnOp operation;
    operation.op = WalOp::kSet;
    operation.key = "must-not-publish";
    operation.value = "x";
    WalRecord transaction = transaction_record(5, {operation});
    std::vector<std::uint8_t> bytes = encode_record(transaction);
    store_u16(bytes.data() + 4, WalFormat::LEGACY_VERSION);
    refresh_record_crc(&bytes);
    append_bytes(live.front().path, bytes);

    KvStore target(clock);
    target.set("sentinel", "kept");
    RecoveryManager recovery(paths.rdb);
    std::string error;
    EXPECT_EQ(recovery.recover(target, &error), RecoveryStatus::kCorrupt);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(target.get("sentinel"), std::optional<std::string>("kept"));
    EXPECT_EQ(target.get("must-not-publish"), std::nullopt);
}

TEST(RecoveryManagerTest, CorruptTransactionCrcFailsClosed) {
    DatabasePaths paths("minidb_recovery_txn_crc_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    seed_with_wal(paths.rdb, clock, false);
    const std::vector<WalSegmentInfo> live = segments(paths.wal_dir);
    ASSERT_EQ(live.size(), 1u);

    WalTxnOp operation;
    operation.op = WalOp::kSet;
    operation.key = "crc-victim";
    operation.value = "original";
    WalRecord transaction = transaction_record(5, {operation});
    std::vector<std::uint8_t> bytes = encode_record(transaction);
    const std::size_t payload_offset = transaction_payload_offset(transaction);
    bytes[payload_offset + WalFormat::TXN_HEADER_SIZE +
          WalFormat::TXN_OP_HEADER_SIZE] ^= 0x5a;
    append_bytes(live.front().path, bytes);

    KvStore target(clock);
    target.set("sentinel", "kept");
    RecoveryManager recovery(paths.rdb);
    std::string error;
    EXPECT_EQ(recovery.recover(target, &error), RecoveryStatus::kCorrupt);
    EXPECT_EQ(target.get("sentinel"), std::optional<std::string>("kept"));
}

TEST(RecoveryManagerTest, InvalidTransactionPayloadNeverPublishesPrefixKeys) {
    DatabasePaths paths("minidb_recovery_txn_payload_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    seed_with_wal(paths.rdb, clock, false);
    const std::vector<WalSegmentInfo> live = segments(paths.wal_dir);
    ASSERT_EQ(live.size(), 1u);

    WalTxnOp first;
    first.op = WalOp::kSet;
    first.key = "prefix";
    first.value = "must-not-survive";
    WalTxnOp second;
    second.op = WalOp::kSet;
    second.key = "unknown";
    second.value = "x";
    WalRecord transaction = transaction_record(5, {first, second});
    std::vector<std::uint8_t> bytes = encode_record(transaction);
    const std::size_t payload_offset = transaction_payload_offset(transaction);
    bytes[payload_offset + WalFormat::TXN_HEADER_SIZE] = 9;
    refresh_record_crc(&bytes);
    append_bytes(live.front().path, bytes);

    KvStore target(clock);
    target.set("sentinel", "kept");
    RecoveryManager recovery(paths.rdb);
    std::string error;
    EXPECT_EQ(recovery.recover(target, &error), RecoveryStatus::kCorrupt);
    EXPECT_EQ(target.get("sentinel"), std::optional<std::string>("kept"));
    EXPECT_EQ(target.get("prefix"), std::nullopt);
    EXPECT_EQ(target.latest_committed_lsn("prefix"), 0u);
}

TEST(RecoveryManagerTest, DuplicateTransactionKeyIsRejectedAtomically) {
    DatabasePaths paths("minidb_recovery_txn_duplicate_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    seed_with_wal(paths.rdb, clock, false);
    const std::vector<WalSegmentInfo> live = segments(paths.wal_dir);
    ASSERT_EQ(live.size(), 1u);

    WalTxnOp first;
    first.op = WalOp::kSet;
    first.key = "key-one";
    first.value = "one";
    WalTxnOp second;
    second.op = WalOp::kSet;
    second.key = "key-two";
    second.value = "two";
    WalRecord transaction = transaction_record(5, {first, second});
    std::vector<std::uint8_t> bytes = encode_record(transaction);
    const std::size_t payload_offset = transaction_payload_offset(transaction);
    const std::size_t first_key_offset = payload_offset +
                                         WalFormat::TXN_HEADER_SIZE +
                                         WalFormat::TXN_OP_HEADER_SIZE;
    const std::size_t second_key_offset = first_key_offset + first.key.size() +
                                          first.value.size() +
                                          WalFormat::TXN_OP_HEADER_SIZE;
    std::memcpy(bytes.data() + second_key_offset, first.key.data(),
                first.key.size());
    refresh_record_crc(&bytes);
    append_bytes(live.front().path, bytes);

    KvStore target(clock);
    target.set("sentinel", "kept");
    RecoveryManager recovery(paths.rdb);
    std::string error;
    EXPECT_EQ(recovery.recover(target, &error), RecoveryStatus::kCorrupt);
    EXPECT_EQ(target.get("sentinel"), std::optional<std::string>("kept"));
    EXPECT_EQ(target.get("key-one"), std::nullopt);
    EXPECT_EQ(target.latest_committed_lsn("key-one"), 0u);
}

TEST(RecoveryManagerTest, TornTransactionTailIsTruncatedWithoutPartialPublish) {
    DatabasePaths paths("minidb_recovery_txn_torn_");
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    seed_with_wal(paths.rdb, clock, false);
    const std::vector<WalSegmentInfo> live = segments(paths.wal_dir);
    ASSERT_EQ(live.size(), 1u);
    const std::uint64_t valid_size = live.front().size;

    WalTxnOp operation;
    operation.op = WalOp::kSet;
    operation.key = "torn-txn";
    operation.value = "must-not-publish";
    WalRecord transaction = transaction_record(5, {operation});
    const std::vector<std::uint8_t> bytes = encode_record(transaction);
    const std::size_t torn_size = bytes.size() - 1;
    append_prefix(live.front().path, bytes, torn_size);
    ASSERT_EQ(fs::file_size(live.front().path), valid_size + torn_size);

    KvStore target(clock);
    RecoveryManager recovery(paths.rdb);
    std::string error;
    ASSERT_EQ(recovery.recover(target, &error), RecoveryStatus::kOk) << error;
    EXPECT_EQ(fs::file_size(live.front().path), valid_size);
    EXPECT_EQ(recovery.stats().truncated_bytes, torn_size);
    EXPECT_EQ(recovery.stats().last_lsn, 4u);
    EXPECT_EQ(target.get("torn-txn"), std::nullopt);
    EXPECT_EQ(target.latest_committed_lsn("torn-txn"), 0u);
}

} // namespace
} // namespace minidb
