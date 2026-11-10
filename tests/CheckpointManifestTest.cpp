#include <gtest/gtest.h>

#include "../src/storage/CheckpointManifest.h"
#include "../src/storage/WalFormat.h"
#include "FaultWalFileOperations.h"
#include "RdbTestUtil.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace minidb {
namespace {

void store_u32(std::uint8_t* data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>(value >> 24);
    data[1] = static_cast<std::uint8_t>(value >> 16);
    data[2] = static_cast<std::uint8_t>(value >> 8);
    data[3] = static_cast<std::uint8_t>(value);
}

void refresh_crc(std::vector<std::uint8_t>* bytes) {
    ASSERT_GE(bytes->size(), CheckpointManifest::CHECKSUM_SIZE);
    const std::size_t crc_offset =
        bytes->size() - CheckpointManifest::CHECKSUM_SIZE;
    store_u32(bytes->data() + crc_offset,
              WalFormat::crc32c(bytes->data(), crc_offset));
}

CheckpointManifest MakeManifest() {
    CheckpointManifest manifest;
    manifest.checkpoint_lsn = 100;
    manifest.retained_segment_starts = {101, 108, 120};
    return manifest;
}

class CheckpointManifestTest : public ::testing::Test {
  protected:
    void SetUp() override {
        test_dir_ = minidb_test::make_unique_temp_dir("minidb_manifest_");
        path_ = test_dir_ + "/db.rdb.manifest";
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    void WriteOldManifest() {
        const CheckpointManifest old = MakeManifest();
        ASSERT_EQ(write_manifest_atomic(path_, old, operations_.get()),
                  ManifestWriteResult::kOk);
    }

    CheckpointManifest LoadManifestOrFail() {
        CheckpointManifest manifest;
        std::string error;
        EXPECT_EQ(load_manifest(path_, &manifest, &error),
                  ManifestLoadStatus::kOk)
            << error;
        return manifest;
    }

    std::string test_dir_;
    std::string path_;
    std::shared_ptr<minidb_test::FaultWalFileOperations> operations_ =
        std::make_shared<minidb_test::FaultWalFileOperations>();
};

TEST_F(CheckpointManifestTest, RoundTripsEmptyAndPopulatedManifests) {
    for (const CheckpointManifest& original :
         {CheckpointManifest{}, MakeManifest()}) {
        std::string error;
        const std::vector<std::uint8_t> bytes =
            CheckpointManifestCodec::encode(original, &error);
        ASSERT_FALSE(bytes.empty()) << error;
        EXPECT_EQ(bytes.size(),
                  CheckpointManifestCodec::encoded_size(original));
        CheckpointManifest decoded;
        std::size_t consumed = 0;
        EXPECT_EQ(CheckpointManifestCodec::decode(bytes.data(), bytes.size(),
                                                  &decoded, &consumed, &error),
                  CheckpointManifest::DecodeStatus::kOk)
            << error;
        EXPECT_EQ(consumed, bytes.size());
        EXPECT_EQ(decoded.checkpoint_lsn, original.checkpoint_lsn);
        EXPECT_EQ(decoded.retained_segment_starts,
                  original.retained_segment_starts);
    }
}

TEST_F(CheckpointManifestTest, BigEndianHeaderAndChecksumAreStable) {
    const CheckpointManifest manifest = MakeManifest();
    const std::vector<std::uint8_t> bytes =
        CheckpointManifestCodec::encode(manifest);
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(std::memcmp(bytes.data(), CheckpointManifest::MAGIC, 4), 0);
    EXPECT_EQ(bytes[4], 0);
    EXPECT_EQ(bytes[5], 1);
    EXPECT_EQ(bytes[6], 0);
    EXPECT_EQ(bytes[7], 0);
    EXPECT_EQ(bytes[15], 100);
    EXPECT_EQ(bytes[19], 3);
}

TEST_F(CheckpointManifestTest, RejectsInvalidRetainedOrderingAndLimits) {
    CheckpointManifest manifest;
    manifest.retained_segment_starts = {2, 1};
    EXPECT_FALSE(CheckpointManifestCodec::validate(manifest));
    manifest.retained_segment_starts = {0};
    EXPECT_FALSE(CheckpointManifestCodec::validate(manifest));
    manifest.retained_segment_starts = {1, 1};
    EXPECT_FALSE(CheckpointManifestCodec::validate(manifest));
    manifest.retained_segment_starts.assign(
        CheckpointManifest::MAX_RETAINED_SEGMENTS + 1, 1);
    EXPECT_FALSE(CheckpointManifestCodec::validate(manifest));
}

TEST_F(CheckpointManifestTest, TruncatedPrefixesReportNeedMore) {
    const std::vector<std::uint8_t> bytes =
        CheckpointManifestCodec::encode(MakeManifest());
    for (std::size_t len = 1; len < bytes.size(); ++len) {
        SCOPED_TRACE(len);
        CheckpointManifest decoded;
        EXPECT_EQ(CheckpointManifestCodec::decode(bytes.data(), len, &decoded),
                  CheckpointManifest::DecodeStatus::kNeedMore);
    }
}

TEST_F(CheckpointManifestTest, RejectsMalformedHeaderAndChecksum) {
    struct Mutation {
        const char* name;
        std::size_t offset;
        std::uint8_t value;
    };
    const Mutation mutations[] = {
        {"magic", 0, 'X'},
        {"version", 5, 2},
        {"flags", 7, 1},
        {"reserved", 20, 1},
    };
    for (const Mutation& mutation : mutations) {
        SCOPED_TRACE(mutation.name);
        std::vector<std::uint8_t> bytes =
            CheckpointManifestCodec::encode(MakeManifest());
        bytes[mutation.offset] = mutation.value;
        refresh_crc(&bytes);
        CheckpointManifest decoded;
        EXPECT_EQ(CheckpointManifestCodec::decode(bytes.data(), bytes.size(),
                                                  &decoded),
                  CheckpointManifest::DecodeStatus::kCorrupt);
    }

    std::vector<std::uint8_t> bytes =
        CheckpointManifestCodec::encode(MakeManifest());
    bytes.back() ^= 0x40;
    CheckpointManifest decoded;
    EXPECT_EQ(
        CheckpointManifestCodec::decode(bytes.data(), bytes.size(), &decoded),
        CheckpointManifest::DecodeStatus::kCorrupt);
}

TEST_F(CheckpointManifestTest, RejectsOversizedRetainedCount) {
    std::vector<std::uint8_t> bytes =
        CheckpointManifestCodec::encode(MakeManifest());
    store_u32(bytes.data() + 16, CheckpointManifest::MAX_RETAINED_SEGMENTS + 1);
    CheckpointManifest decoded;
    EXPECT_EQ(
        CheckpointManifestCodec::decode(bytes.data(), bytes.size(), &decoded),
        CheckpointManifest::DecodeStatus::kCorrupt);
}

TEST_F(CheckpointManifestTest, LoadDistinguishesMissingValidAndCorrupt) {
    CheckpointManifest manifest;
    std::string error;
    EXPECT_EQ(load_manifest(path_, &manifest, &error),
              ManifestLoadStatus::kMissing);
    WriteOldManifest();
    EXPECT_EQ(load_manifest(path_, &manifest, &error), ManifestLoadStatus::kOk);
    EXPECT_EQ(manifest.retained_segment_starts,
              MakeManifest().retained_segment_starts);

    std::ofstream append(path_, std::ios::binary | std::ios::app);
    append.put('x');
    append.close();
    EXPECT_EQ(load_manifest(path_, &manifest, &error),
              ManifestLoadStatus::kCorrupt);
}

TEST_F(CheckpointManifestTest, LoadRejectsTruncatedAndNonRegularFiles) {
    const std::vector<std::uint8_t> bytes =
        CheckpointManifestCodec::encode(MakeManifest());
    const std::vector<std::uint8_t> truncated(bytes.begin(),
                                              bytes.begin() + 30);
    ASSERT_TRUE(minidb_test::write_file(path_, truncated));
    CheckpointManifest manifest;
    EXPECT_EQ(load_manifest(path_, &manifest), ManifestLoadStatus::kCorrupt);

    std::error_code ec;
    fs::remove(path_, ec);
    fs::create_directory(path_, ec);
    ASSERT_FALSE(ec);
    EXPECT_EQ(load_manifest(path_, &manifest), ManifestLoadStatus::kCorrupt);
}

TEST_F(CheckpointManifestTest,
       AtomicWritePublishesCompleteManifestWithoutTemp) {
    const CheckpointManifest manifest = MakeManifest();
    std::string error;
    ASSERT_EQ(write_manifest_atomic(path_, manifest, operations_.get(), &error),
              ManifestWriteResult::kOk)
        << error;
    const CheckpointManifest loaded = LoadManifestOrFail();
    EXPECT_EQ(loaded.checkpoint_lsn, manifest.checkpoint_lsn);
    EXPECT_EQ(loaded.retained_segment_starts, manifest.retained_segment_starts);
    for (const auto& entry : fs::directory_iterator(test_dir_)) {
        EXPECT_EQ(entry.path().filename().string().find(".tmp."),
                  std::string::npos);
    }
}

TEST_F(CheckpointManifestTest, PreRenameFailuresPreserveOldManifest) {
    enum class Stage { kOpen, kWrite, kFsync, kClose, kRename };
    for (Stage stage : {Stage::kOpen, Stage::kWrite, Stage::kFsync,
                        Stage::kClose, Stage::kRename}) {
        SCOPED_TRACE(static_cast<int>(stage));
        std::error_code ec;
        fs::remove(path_, ec);
        WriteOldManifest();
        auto operations =
            std::make_shared<minidb_test::FaultWalFileOperations>();
        switch (stage) {
        case Stage::kOpen:
            operations->set_fail_open_exclusive(EACCES);
            break;
        case Stage::kWrite:
            operations->set_partial_write_then_fail(1, 4, ENOSPC);
            break;
        case Stage::kFsync:
            operations->set_fail_fsync(EIO);
            break;
        case Stage::kClose:
            operations->set_fail_close(EIO);
            break;
        case Stage::kRename:
            operations->set_fail_rename(EXDEV);
            break;
        }
        CheckpointManifest replacement = MakeManifest();
        replacement.checkpoint_lsn = 999;
        std::string error;
        EXPECT_EQ(
            write_manifest_atomic(path_, replacement, operations.get(), &error),
            ManifestWriteResult::kFailed);
        const CheckpointManifest loaded = LoadManifestOrFail();
        EXPECT_EQ(loaded.checkpoint_lsn, MakeManifest().checkpoint_lsn);
        for (const auto& entry : fs::directory_iterator(test_dir_)) {
            EXPECT_EQ(entry.path().filename().string().find(".tmp."),
                      std::string::npos);
        }
    }
}

TEST_F(CheckpointManifestTest, DirectoryFsyncFailureIsDurabilityUnconfirmed) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    operations->set_fail_dir_fsync(EIO);
    CheckpointManifest manifest = MakeManifest();
    manifest.checkpoint_lsn = 777;
    std::string error;
    EXPECT_EQ(write_manifest_atomic(path_, manifest, operations.get(), &error),
              ManifestWriteResult::kDurabilityUnconfirmed);
    const CheckpointManifest loaded = LoadManifestOrFail();
    EXPECT_EQ(loaded.checkpoint_lsn, 777u);
}

} // namespace
} // namespace minidb
