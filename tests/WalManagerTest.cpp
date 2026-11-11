#include <gtest/gtest.h>

#include "../src/storage/WalManager.h"
#include "FaultWalFileOperations.h"
#include "RdbTestUtil.h"

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace minidb {
namespace {

WalRecord MakeRecord(std::uint64_t lsn, std::string key = "key",
                     std::string value = "value") {
    WalRecord record;
    record.lsn = lsn;
    record.op = WalOp::kSet;
    record.key = std::move(key);
    record.value = std::move(value);
    return record;
}

std::uint64_t DataFsyncBucketCount(const WalManagerStats& stats) {
    std::uint64_t total = 0;
    for (const std::uint64_t count : stats.data_fsync_latency_buckets) {
        total += count;
    }
    return total;
}

class WalManagerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        test_dir_ = minidb_test::make_unique_temp_dir("minidb_wal_manager_");
        wal_dir_ = test_dir_ + "/db.rdb.wal";
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    std::string test_dir_;
    std::string wal_dir_;
};

TEST_F(WalManagerTest, OpenCreatesDurableEmptySegment) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager manager(wal_dir_, operations);
    std::string error;
    ASSERT_TRUE(manager.open({}, 1, &error)) << error;
    ASSERT_NE(manager.active_segment(), nullptr);
    EXPECT_EQ(manager.active_segment()->start_lsn, 1u);
    EXPECT_EQ(manager.active_segment()->size, 0u);
    EXPECT_EQ(manager.total_bytes(), 0u);
    EXPECT_EQ(manager.next_lsn(), 1u);
    EXPECT_TRUE(fs::is_directory(wal_dir_));
    EXPECT_TRUE(fs::exists(wal_dir_ + "/" + wal_segment_name(1)));
    EXPECT_GT(operations->dir_fsync_calls(), 0);
    const WalManagerStats stats = manager.stats();
    EXPECT_EQ(DataFsyncBucketCount(stats), stats.data_fsyncs);
    EXPECT_GE(stats.data_fsync_total_us, stats.data_fsync_max_us);
}

TEST_F(WalManagerTest, SyncAppendPersistsAndAdvancesLsn) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager manager(wal_dir_, operations);
    ASSERT_TRUE(manager.open({}, 1));
    std::string error;
    const WalRecord record = MakeRecord(1, "alpha", "beta");
    ASSERT_EQ(manager.append(record, DurabilityMode::kSync, &error),
              WalResult::kOk)
        << error;
    EXPECT_EQ(manager.next_lsn(), 2u);
    EXPECT_EQ(manager.total_bytes(), WalFormat::encoded_size(record));
    const WalManagerStats stats = manager.stats();
    EXPECT_EQ(stats.appends, 1u);
    EXPECT_GE(stats.data_fsyncs, 2u);
    EXPECT_GE(stats.append_total_us, stats.append_max_us);
    EXPECT_GE(stats.data_fsync_total_us, stats.data_fsync_max_us);
    EXPECT_EQ(DataFsyncBucketCount(stats), stats.data_fsyncs);
    EXPECT_EQ(fs::file_size(wal_dir_ + "/" + wal_segment_name(1)),
              WalFormat::encoded_size(record));
}

TEST_F(WalManagerTest, IntervalAppendDefersFsyncUntilFlush) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager manager(wal_dir_, operations);
    ASSERT_TRUE(manager.open({}, 1));
    const int fsync_before = manager.stats().data_fsyncs;
    ASSERT_EQ(manager.append(MakeRecord(1), DurabilityMode::kInterval),
              WalResult::kOk);
    EXPECT_EQ(manager.stats().data_fsyncs,
              static_cast<std::uint64_t>(fsync_before));
    EXPECT_GE(manager.stats().append_total_us, manager.stats().append_max_us);
    EXPECT_EQ(DataFsyncBucketCount(manager.stats()),
              manager.stats().data_fsyncs);
    std::string error;
    ASSERT_TRUE(manager.flush(&error)) << error;
    EXPECT_GT(manager.stats().data_fsyncs,
              static_cast<std::uint64_t>(fsync_before));
    EXPECT_TRUE(manager.flush(&error));
}

TEST_F(WalManagerTest, EintrAndShortWritesAreRetriedToCompletion) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    operations->set_eintr_writes(2);
    operations->set_short_writes(5);
    WalManager manager(wal_dir_, operations);
    ASSERT_TRUE(manager.open({}, 1));
    const WalRecord record = MakeRecord(1, "short", std::string(64, 'v'));
    ASSERT_EQ(manager.append(record, DurabilityMode::kSync), WalResult::kOk);
    EXPECT_GT(operations->write_calls(), 2);
    EXPECT_EQ(manager.next_lsn(), 2u);
}

TEST_F(WalManagerTest, PartialWriteFailureMarksManagerUnhealthy) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager manager(wal_dir_, operations);
    ASSERT_TRUE(manager.open({}, 1));
    operations->set_partial_write_then_fail(1, 11, ENOSPC);
    std::string error;
    EXPECT_EQ(manager.append(MakeRecord(1), DurabilityMode::kSync, &error),
              WalResult::kIoError);
    EXPECT_FALSE(manager.healthy());
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(manager.append(MakeRecord(2), DurabilityMode::kSync),
              WalResult::kIoError);
}

TEST_F(WalManagerTest, FsyncFailureMarksManagerUnhealthy) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager manager(wal_dir_, operations);
    ASSERT_TRUE(manager.open({}, 1));
    const WalManagerStats before = manager.stats();
    operations->set_fail_fsync(EIO);
    std::string error;
    EXPECT_EQ(manager.append(MakeRecord(1), DurabilityMode::kSync, &error),
              WalResult::kIoError);
    EXPECT_FALSE(manager.healthy());
    EXPECT_NE(error.find("fsync"), std::string::npos);
    const WalManagerStats after = manager.stats();
    EXPECT_EQ(after.data_fsyncs, before.data_fsyncs);
    EXPECT_EQ(DataFsyncBucketCount(after), after.data_fsyncs);
}

TEST_F(WalManagerTest, WrongLsnAndInvalidRecordFailClosed) {
    WalManager manager(wal_dir_);
    ASSERT_TRUE(manager.open({}, 5));
    EXPECT_EQ(manager.append(MakeRecord(4), DurabilityMode::kSync),
              WalResult::kIoError);
    EXPECT_FALSE(manager.healthy());

    WalManager second(wal_dir_ + "2");
    ASSERT_TRUE(second.open({}, 1));
    WalRecord invalid = MakeRecord(1);
    invalid.key.clear();
    EXPECT_EQ(second.append(invalid, DurabilityMode::kSync),
              WalResult::kIoError);
    EXPECT_FALSE(second.healthy());
}

TEST_F(WalManagerTest, RotateCreatesContiguousSegmentAndFlushesOld) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager manager(wal_dir_, operations);
    ASSERT_TRUE(manager.open({}, 1));
    ASSERT_EQ(manager.append(MakeRecord(1), DurabilityMode::kInterval),
              WalResult::kOk);
    std::string error;
    ASSERT_TRUE(manager.rotate(2, &error)) << error;
    ASSERT_EQ(manager.segments().size(), 2u);
    EXPECT_EQ(manager.segments()[0].start_lsn, 1u);
    EXPECT_EQ(manager.segments()[1].start_lsn, 2u);
    EXPECT_EQ(manager.active_segment()->size, 0u);
    EXPECT_EQ(manager.stats().rotations, 1u);
    EXPECT_EQ(manager.next_lsn(), 2u);
    EXPECT_TRUE(fs::exists(wal_dir_ + "/" + wal_segment_name(2)));
}

TEST_F(WalManagerTest, RejectsNonNextRotationLsn) {
    WalManager manager(wal_dir_);
    ASSERT_TRUE(manager.open({}, 1));
    std::string error;
    EXPECT_FALSE(manager.rotate(2, &error));
    EXPECT_NE(error.find("next LSN"), std::string::npos);
    EXPECT_TRUE(manager.healthy());
}

TEST_F(WalManagerTest, CleanupRemovesResidueAndKeepsActive) {
    WalManager manager(wal_dir_);
    ASSERT_TRUE(manager.open({}, 1));
    ASSERT_EQ(manager.append(MakeRecord(1), DurabilityMode::kSync),
              WalResult::kOk);
    ASSERT_TRUE(manager.rotate(2));
    ASSERT_EQ(manager.append(MakeRecord(2), DurabilityMode::kSync),
              WalResult::kOk);
    const std::string stale = wal_dir_ + "/unrelated.txt";
    {
        std::ofstream out(stale);
        out << "residue";
    }

    std::string error;
    ASSERT_TRUE(manager.cleanup_except_active(&error)) << error;
    ASSERT_EQ(manager.segments().size(), 1u);
    EXPECT_EQ(manager.active_segment()->start_lsn, 2u);
    EXPECT_FALSE(fs::exists(wal_dir_ + "/" + wal_segment_name(1)));
    EXPECT_TRUE(fs::exists(wal_dir_ + "/" + wal_segment_name(2)));
    EXPECT_TRUE(fs::exists(stale));
}

TEST_F(WalManagerTest, CleanupBeforeRetainsBoundaryAndAllLaterSegments) {
    WalManager manager(wal_dir_);
    ASSERT_TRUE(manager.open({}, 1));
    ASSERT_EQ(manager.append(MakeRecord(1), DurabilityMode::kSync),
              WalResult::kOk);
    ASSERT_TRUE(manager.rotate(2));
    ASSERT_EQ(manager.append(MakeRecord(2), DurabilityMode::kSync),
              WalResult::kOk);
    ASSERT_TRUE(manager.rotate(3));
    ASSERT_EQ(manager.append(MakeRecord(3), DurabilityMode::kSync),
              WalResult::kOk);
    const std::uint64_t total_before = manager.total_bytes();

    std::string error;
    ASSERT_TRUE(manager.cleanup_before(2, &error)) << error;
    ASSERT_EQ(manager.segments().size(), 2u);
    EXPECT_EQ(manager.segments()[0].start_lsn, 2u);
    EXPECT_EQ(manager.segments()[1].start_lsn, 3u);
    EXPECT_FALSE(fs::exists(wal_dir_ + "/" + wal_segment_name(1)));
    EXPECT_TRUE(fs::exists(wal_dir_ + "/" + wal_segment_name(2)));
    EXPECT_TRUE(fs::exists(wal_dir_ + "/" + wal_segment_name(3)));
    EXPECT_LT(manager.total_bytes(), total_before);
    EXPECT_EQ(manager.stats().segments_removed, 1u);

    const std::uint64_t retained_total = manager.total_bytes();
    error.clear();
    EXPECT_FALSE(manager.cleanup_before(99, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(manager.segments().size(), 2u);
    EXPECT_EQ(manager.total_bytes(), retained_total);
}

TEST_F(WalManagerTest, RemoveSegmentRefusesActiveAndRemovesObsolete) {
    WalManager manager(wal_dir_);
    ASSERT_TRUE(manager.open({}, 1));
    ASSERT_EQ(manager.append(MakeRecord(1), DurabilityMode::kSync),
              WalResult::kOk);
    ASSERT_TRUE(manager.rotate(2));
    std::vector<WalSegmentInfo> segments = manager.segments();
    std::string error;
    EXPECT_FALSE(manager.remove_segment(segments.back(), &error));
    EXPECT_TRUE(manager.remove_segment(segments.front(), &error)) << error;
    ASSERT_EQ(manager.segments().size(), 1u);
    EXPECT_EQ(manager.active_segment()->start_lsn, 2u);
}

TEST_F(WalManagerTest, SegmentMetadataLimitsAndEmptyRulesAreEnforced) {
    WalManager too_many(wal_dir_ + "-many");
    std::vector<WalSegmentInfo> segments(WalFormat::MAX_SEGMENTS + 1);
    for (std::size_t i = 0; i < segments.size(); ++i) {
        segments[i].start_lsn = i + 1;
        segments[i].size = 1;
        segments[i].path = "/missing";
    }
    EXPECT_FALSE(too_many.open(segments, segments.size() + 1));

    WalManager total(wal_dir_ + "-total");
    segments.clear();
    for (std::size_t i = 0; i < 17; ++i) {
        segments.push_back({i + 1, "/missing", WalFormat::MAX_SEGMENT_BYTES});
    }
    EXPECT_FALSE(total.open(segments, 1000));

    WalManager empty_middle(wal_dir_ + "-empty");
    segments = {{1, "/missing", 0}, {2, "/missing", 1}};
    EXPECT_FALSE(empty_middle.open(segments, 3));

    WalManager wrong_empty(wal_dir_ + "-wrong");
    segments = {{1, "/missing", 0}};
    EXPECT_FALSE(wrong_empty.open(segments, 2));
}

TEST_F(WalManagerTest, StaleNonEmptySegmentIsRejected) {
    ASSERT_TRUE(fs::create_directories(wal_dir_));
    const std::string path = wal_dir_ + "/" + wal_segment_name(1);
    {
        std::ofstream out(path, std::ios::binary);
        out << "stale";
    }
    WalManager manager(wal_dir_);
    std::string error;
    EXPECT_FALSE(manager.open({}, 1, &error));
    EXPECT_NE(error.find("stale"), std::string::npos);
}

TEST_F(WalManagerTest, MissingActiveSegmentIsRejected) {
    WalManager manager(wal_dir_);
    std::string error;
    EXPECT_FALSE(manager.open({{1, wal_dir_ + "/" + wal_segment_name(1), 1}}, 2,
                              &error));
    EXPECT_FALSE(error.empty());
}

TEST_F(WalManagerTest, DirectoryFsyncFailureFailsOpenClosed) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    operations->set_fail_dir_fsync(EIO);
    WalManager manager(wal_dir_, operations);
    std::string error;
    EXPECT_FALSE(manager.open({}, 1, &error));
    EXPECT_FALSE(manager.healthy());
}

TEST_F(WalManagerTest, BatchSyncUsesOneTailFsyncAndAdvancesContiguousLsn) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager manager(wal_dir_, operations);
    ASSERT_TRUE(manager.open({}, 1));
    const std::uint64_t fsync_before = manager.stats().data_fsyncs;
    const std::vector<WalRecord> batch = {MakeRecord(1, "a", "one"),
                                          MakeRecord(2, "b", "two"),
                                          MakeRecord(3, "c", "three")};
    std::string error;
    ASSERT_EQ(manager.append_batch(batch, DurabilityMode::kSync, &error),
              WalResult::kOk)
        << error;
    EXPECT_EQ(manager.next_lsn(), 4u);
    EXPECT_EQ(manager.stats().data_fsyncs, fsync_before + 1);
    EXPECT_EQ(manager.stats().appends, 3u);
    EXPECT_EQ(manager.stats().bytes_written,
              WalFormat::encoded_size(batch[0]) +
                  WalFormat::encoded_size(batch[1]) +
                  WalFormat::encoded_size(batch[2]));
    EXPECT_EQ(DataFsyncBucketCount(manager.stats()),
              manager.stats().data_fsyncs);
}

TEST_F(WalManagerTest, BatchCanRotateAcrossSegments) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalLimits limits;
    limits.max_segment_bytes = 100;
    limits.max_total_bytes = 4096;
    limits.max_segments = 4;
    WalManager manager(wal_dir_, operations, limits);
    ASSERT_TRUE(manager.open({}, 1));
    std::vector<WalRecord> batch;
    for (std::uint64_t lsn = 1; lsn <= 5; ++lsn) {
        batch.push_back(MakeRecord(lsn, "key", "value"));
    }
    std::string error;
    ASSERT_EQ(manager.append_batch(batch, DurabilityMode::kSync, &error),
              WalResult::kOk)
        << error;
    EXPECT_EQ(manager.next_lsn(), 6u);
    EXPECT_EQ(manager.segments().size(), 3u);
    EXPECT_EQ(manager.stats().rotations, 2u);
    EXPECT_EQ(manager.stats().appends, 5u);
    EXPECT_EQ(manager.active_segment()->start_lsn, 5u);
}

TEST_F(WalManagerTest, BatchPrefersTheWholeBatchBeforeWritingWhenTooLarge) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalLimits limits;
    limits.max_segment_bytes = 100;
    limits.max_total_bytes = 80;
    limits.max_segments = 2;
    WalManager manager(wal_dir_, operations, limits);
    ASSERT_TRUE(manager.open({}, 1));
    const std::uint64_t fsync_before = manager.stats().data_fsyncs;
    std::string error;
    const std::vector<WalRecord> batch = {MakeRecord(1, "key", "value"),
                                          MakeRecord(2, "key", "value")};
    EXPECT_EQ(manager.append_batch(batch, DurabilityMode::kSync, &error),
              WalResult::kResourceLimit);
    EXPECT_TRUE(manager.healthy());
    EXPECT_EQ(manager.next_lsn(), 1u);
    EXPECT_EQ(manager.stats().appends, 0u);
    EXPECT_EQ(manager.stats().data_fsyncs, fsync_before);
}

TEST_F(WalManagerTest, EmptyAndNonContiguousBatchesFailClosed) {
    WalManager manager(wal_dir_);
    ASSERT_TRUE(manager.open({}, 1));
    std::string error;
    EXPECT_EQ(manager.append_batch({}, DurabilityMode::kSync, &error),
              WalResult::kInvalidArgument);
    EXPECT_TRUE(manager.healthy());

    std::vector<WalRecord> batch = {MakeRecord(1), MakeRecord(3)};
    EXPECT_EQ(manager.append_batch(batch, DurabilityMode::kSync, &error),
              WalResult::kIoError);
    EXPECT_FALSE(manager.healthy());
    EXPECT_EQ(manager.stats().appends, 0u);
}

TEST_F(WalManagerTest, BatchPartialWriteAdvancesOnlyCompleteRecords) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager manager(wal_dir_, operations);
    ASSERT_TRUE(manager.open({}, 1));
    operations->set_partial_write_then_fail(2, 5, ENOSPC);
    std::string error;
    const std::vector<WalRecord> batch = {MakeRecord(1), MakeRecord(2),
                                          MakeRecord(3)};
    EXPECT_EQ(manager.append_batch(batch, DurabilityMode::kSync, &error),
              WalResult::kIoError);
    EXPECT_FALSE(manager.healthy());
    EXPECT_EQ(manager.next_lsn(), 2u);
    EXPECT_EQ(manager.stats().appends, 1u);
}

TEST_F(WalManagerTest, BatchFsyncFailureDoesNotApplyOrReportSuccess) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager manager(wal_dir_, operations);
    ASSERT_TRUE(manager.open({}, 1));
    const std::uint64_t fsync_before = manager.stats().data_fsyncs;
    operations->set_fail_fsync(EIO);
    std::string error;
    const std::vector<WalRecord> batch = {MakeRecord(1), MakeRecord(2),
                                          MakeRecord(3)};
    EXPECT_EQ(manager.append_batch(batch, DurabilityMode::kSync, &error),
              WalResult::kIoError);
    EXPECT_FALSE(manager.healthy());
    EXPECT_NE(error.find("fsync"), std::string::npos);
    EXPECT_EQ(manager.next_lsn(), 4u);
    EXPECT_EQ(manager.stats().appends, 3u);
    EXPECT_EQ(manager.stats().data_fsyncs, fsync_before);
    EXPECT_EQ(DataFsyncBucketCount(manager.stats()),
              manager.stats().data_fsyncs);
}

} // namespace
} // namespace minidb
