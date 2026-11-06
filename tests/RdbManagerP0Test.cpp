#include <gtest/gtest.h>
#include "../src/storage/RdbManager.h"
#include "../src/storage/RdbFormat.h"
#include "../src/storage/KvStore.h"
#include "RdbTestUtil.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

namespace fs = std::filesystem;

namespace minidb {

class RdbManagerP0Test : public ::testing::Test {
  protected:
    void SetUp() override {
        test_dir_ = minidb_test::make_unique_temp_dir("minidb_rdb_p0_");
        test_rdb_ = test_dir_ + "/test.rdb";
        clock_ =
            std::make_shared<minidb_test::FrozenClock>(5000, 1700000000000);
    }

    void TearDown() override {
        std::error_code ec;
        fs::permissions(test_dir_, fs::perms::owner_all, fs::perm_options::add,
                        ec);
        fs::remove_all(test_dir_, ec);
    }

    void SeedStore(KvStore& store) {
        store.set("alpha", "value-alpha");
        std::string binary = "bin";
        binary.push_back('\0');
        binary.append("ary");
        binary.push_back(static_cast<char>(0xff));
        store.set("beta", binary);
        store.set("gamma", "value-gamma");
        ASSERT_TRUE(store.expire("beta", 60000));
    }

    void ExpectSeedStoreUnchanged(KvStore& store) {
        ASSERT_EQ(store.size(), 3u);
        EXPECT_EQ(store.get("alpha"),
                  std::optional<std::string>("value-alpha"));
        std::string binary = "bin";
        binary.push_back('\0');
        binary.append("ary");
        binary.push_back(static_cast<char>(0xff));
        EXPECT_EQ(store.get("beta"), std::optional<std::string>(binary));
        EXPECT_EQ(store.get("gamma"),
                  std::optional<std::string>("value-gamma"));
        EXPECT_FALSE(store.get("intruder").has_value());

        const auto alpha_ttl = store.ttl_info("alpha");
        const auto beta_ttl = store.ttl_info("beta");
        const auto gamma_ttl = store.ttl_info("gamma");
        EXPECT_EQ(alpha_ttl.state, KvStore::TtlState::kNoExpiry);
        EXPECT_EQ(beta_ttl.state, KvStore::TtlState::kExpiring);
        EXPECT_EQ(beta_ttl.remaining_ms, 60000);
        EXPECT_EQ(gamma_ttl.state, KvStore::TtlState::kNoExpiry);
    }

    std::string test_dir_;
    std::string test_rdb_;
    std::shared_ptr<minidb_test::FrozenClock> clock_;
};

TEST_F(RdbManagerP0Test, Load_MissingFileReturnsZeroAndPreservesStore) {
    RdbManager manager(test_rdb_);
    KvStore store(clock_);
    SeedStore(store);

    EXPECT_EQ(manager.load(store), 0);
    ExpectSeedStoreUnchanged(store);
}

TEST_F(RdbManagerP0Test, Load_PathIsDirectoryRejectedAndPreservesStore) {
    RdbManager manager(test_dir_);
    KvStore store(clock_);
    SeedStore(store);

    EXPECT_EQ(manager.load(store), -1);
    ExpectSeedStoreUnchanged(store);
}

TEST_F(RdbManagerP0Test, Load_FileNameTooLongRejectedAndPreservesStore) {
    const std::string long_name(300, 'x');
    RdbManager manager(test_dir_ + "/" + long_name);
    KvStore store(clock_);
    SeedStore(store);

    EXPECT_EQ(manager.load(store), -1);
    ExpectSeedStoreUnchanged(store);
}

TEST_F(RdbManagerP0Test, Load_UnreadableFileRejectedAndPreservesStore) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root can bypass permission bits";
    }

    auto buffer = minidb_test::make_header(0);
    minidb_test::append_checksum(buffer);
    ASSERT_TRUE(minidb_test::write_file(test_rdb_, buffer));
    ASSERT_EQ(::chmod(test_rdb_.c_str(), 0000), 0);

    RdbManager manager(test_rdb_);
    KvStore store(clock_);
    SeedStore(store);

    EXPECT_EQ(manager.load(store), -1);
    ExpectSeedStoreUnchanged(store);
}

TEST_F(RdbManagerP0Test, Load_TruncatedRecordWithValidChecksumRejected) {
    auto buffer = minidb_test::make_header(1);
    const auto key_len = RdbFormat::encode_varint(5);
    buffer.insert(buffer.end(), key_len.begin(), key_len.end());
    minidb_test::append_bytes(buffer, "short", 5);
    minidb_test::append_checksum(buffer);
    ASSERT_TRUE(minidb_test::write_file(test_rdb_, buffer));

    RdbManager manager(test_rdb_);
    KvStore store(clock_);
    SeedStore(store);

    EXPECT_EQ(manager.load(store), -1);
    ExpectSeedStoreUnchanged(store);
}

TEST_F(RdbManagerP0Test, Load_BitFlippedChecksumRejected) {
    auto buffer = minidb_test::make_header(1);
    minidb_test::append_record(buffer, "key", "value");
    minidb_test::append_checksum(buffer);
    ASSERT_FALSE(buffer.empty());
    buffer.back() ^= 0x01;
    ASSERT_TRUE(minidb_test::write_file(test_rdb_, buffer));

    RdbManager manager(test_rdb_);
    KvStore store(clock_);
    SeedStore(store);

    EXPECT_EQ(manager.load(store), -1);
    ExpectSeedStoreUnchanged(store);
}

TEST_F(RdbManagerP0Test, Load_ValidImageReplacesStore) {
    auto buffer = minidb_test::make_header(2);
    minidb_test::append_record(buffer, "new-a", "new-value-a");
    minidb_test::append_record(buffer, "new-b", "new-value-b");
    minidb_test::append_checksum(buffer);
    ASSERT_TRUE(minidb_test::write_file(test_rdb_, buffer));

    RdbManager manager(test_rdb_);
    KvStore store(clock_);
    SeedStore(store);

    EXPECT_EQ(manager.load(store), 2);
    EXPECT_EQ(store.size(), 2u);
    EXPECT_FALSE(store.get("alpha").has_value());
    EXPECT_EQ(store.get("new-a"), std::optional<std::string>("new-value-a"));
    EXPECT_EQ(store.get("new-b"), std::optional<std::string>("new-value-b"));
}

TEST_F(RdbManagerP0Test, RejectFileWithTrailingBytes) {
    auto buffer = minidb_test::make_header(1);
    minidb_test::append_record(buffer, "testkey", "testvalue");
    const std::string garbage = "TRAILING_GARBAGE";
    minidb_test::append_bytes(buffer, garbage.data(), garbage.size());
    minidb_test::append_checksum(buffer);
    ASSERT_TRUE(minidb_test::write_file(test_rdb_, buffer));

    RdbManager manager(test_rdb_);
    KvStore store(clock_);
    SeedStore(store);
    EXPECT_EQ(manager.load(store), -1);
    ExpectSeedStoreUnchanged(store);
}

TEST_F(RdbManagerP0Test, RejectDuplicateKeys) {
    auto buffer = minidb_test::make_header(2);
    minidb_test::append_record(buffer, "duplicatekey", "first");
    minidb_test::append_record(buffer, "duplicatekey", "second");
    minidb_test::append_checksum(buffer);
    ASSERT_TRUE(minidb_test::write_file(test_rdb_, buffer));

    RdbManager manager(test_rdb_);
    KvStore store(clock_);
    SeedStore(store);
    EXPECT_EQ(manager.load(store), -1);
    ExpectSeedStoreUnchanged(store);
}

TEST_F(RdbManagerP0Test, RejectDeclaredRecordCountMismatch) {
    auto buffer = minidb_test::make_header(2);
    minidb_test::append_record(buffer, "only-one", "value");
    minidb_test::append_checksum(buffer);
    ASSERT_TRUE(minidb_test::write_file(test_rdb_, buffer));

    RdbManager manager(test_rdb_);
    KvStore store(clock_);
    SeedStore(store);
    EXPECT_EQ(manager.load(store), -1);
    ExpectSeedStoreUnchanged(store);
}

TEST_F(RdbManagerP0Test, RejectRecordCountAboveFormatLimit) {
    auto buffer = minidb_test::make_header(RdbFormat::MAX_RECORDS + 1);
    minidb_test::append_checksum(buffer);
    ASSERT_TRUE(minidb_test::write_file(test_rdb_, buffer));

    RdbManager manager(test_rdb_);
    KvStore store(clock_);
    SeedStore(store);
    EXPECT_EQ(manager.load(store), -1);
    ExpectSeedStoreUnchanged(store);
}

TEST_F(RdbManagerP0Test, RejectValidChecksumWithVersionMismatch) {
    std::vector<uint8_t> buffer;
    minidb_test::append_bytes(buffer, RdbFormat::MAGIC, 4);
    minidb_test::append_u32(buffer, RdbFormat::VERSION + 1);
    minidb_test::append_u32(buffer, 0);
    minidb_test::append_checksum(buffer);
    ASSERT_TRUE(minidb_test::write_file(test_rdb_, buffer));

    RdbManager manager(test_rdb_);
    KvStore store(clock_);
    SeedStore(store);
    EXPECT_EQ(manager.load(store), -1);
    ExpectSeedStoreUnchanged(store);
}

} // namespace minidb
