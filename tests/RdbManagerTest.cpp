#include <gtest/gtest.h>
#include <fstream>
#include "../src/storage/RdbManager.h"
#include "../src/storage/KvStore.h"
#include "RdbTestUtil.h"
#include <filesystem>
#include <cstdlib>

using namespace minidb;
namespace fs = std::filesystem;

class RdbManagerTest : public ::testing::Test {
  protected:
    void SetUp() override {
        test_dir = minidb_test::make_unique_temp_dir("minidb_rdb_test_");
        rdb_path = test_dir + "/test.rdb";
    }

    void TearDown() override {
        fs::remove_all(test_dir);
    }

    std::string test_dir;
    std::string rdb_path;
};

TEST_F(RdbManagerTest, Save_EmptyStore) {
    RdbManager manager(rdb_path);
    KvStore store;

    EXPECT_TRUE(manager.save(store));
    EXPECT_TRUE(manager.exists());
    EXPECT_TRUE(fs::exists(rdb_path));
}

TEST_F(RdbManagerTest, Load_EmptyFile) {
    RdbManager manager(rdb_path);
    KvStore store;

    EXPECT_TRUE(manager.save(store));

    KvStore loaded;
    int count = manager.load(loaded);
    EXPECT_GE(count, 0);         // 成功加载
    EXPECT_EQ(loaded.size(), 0); // 空库
}

TEST_F(RdbManagerTest, Save_WithData) {
    RdbManager manager(rdb_path);
    KvStore store;

    store.set("key1", "value1");
    store.set("key2", "value2");

    EXPECT_TRUE(manager.save(store));
    EXPECT_TRUE(manager.exists());
}

TEST_F(RdbManagerTest, SaveAndLoad) {
    RdbManager manager(rdb_path);
    KvStore store;

    store.set("hello", "world");
    store.set("foo", "bar");

    EXPECT_TRUE(manager.save(store));

    KvStore loaded;
    int count = manager.load(loaded);
    EXPECT_GE(count, 0);

    auto value1 = loaded.get("hello");
    EXPECT_TRUE(value1.has_value());
    EXPECT_EQ(value1.value(), "world");

    auto value2 = loaded.get("foo");
    EXPECT_TRUE(value2.has_value());
    EXPECT_EQ(value2.value(), "bar");
}

TEST_F(RdbManagerTest, SaveAndLoadWithTTL) {
    RdbManager manager(rdb_path);
    KvStore store;

    store.set("persistent", "data1");
    store.set("expiring", "data2");
    store.expire("expiring", 3600000); // 1 hour

    EXPECT_TRUE(manager.save(store));

    KvStore loaded;
    int count = manager.load(loaded);
    EXPECT_GE(count, 0);

    auto value1 = loaded.get("persistent");
    EXPECT_TRUE(value1.has_value());

    auto value2 = loaded.get("expiring");
    EXPECT_TRUE(value2.has_value());

    auto ttl = loaded.ttl("expiring");
    EXPECT_TRUE(ttl.has_value());
    EXPECT_GT(ttl.value(), 0);
}

TEST_F(RdbManagerTest, Load_NonExistent) {
    RdbManager manager(rdb_path);
    KvStore store;

    int count = manager.load(store);
    EXPECT_EQ(count, 0); // 无文件，返回 0
}

TEST_F(RdbManagerTest, Load_CorruptedFile) {
    // 创建损坏的文件
    std::ofstream file(rdb_path, std::ios::binary);
    file.write("BAD_MAGIC!!!!", 12);
    file.close();

    RdbManager manager(rdb_path);
    KvStore store;

    int count = manager.load(store);
    EXPECT_EQ(count, -1); // 损坏文件返回 -1
}

TEST_F(RdbManagerTest, Remove) {
    RdbManager manager(rdb_path);
    KvStore store;

    manager.save(store);
    EXPECT_TRUE(manager.exists());

    EXPECT_TRUE(manager.remove());
    EXPECT_FALSE(manager.exists());
}

// AI-016: 改进的 RDB 损坏测试 - 使用正确构造的样本

// 测试1: 截断的记录(header正确,但记录不完整)
TEST_F(RdbManagerTest, Load_TruncatedRecord) {
    // 先保存一个有效文件
    RdbManager manager(rdb_path);
    KvStore store;
    store.set("key1", "value1_with_long_data_to_ensure_truncation");
    store.set("key2", "value2");
    EXPECT_TRUE(manager.save(store));

    // 读取完整文件并计算正确的部分CRC
    std::ifstream file(rdb_path, std::ios::binary);
    std::vector<uint8_t> content((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
    file.close();

    ASSERT_GT(content.size(), 30u);

    // 截断到第一条记录的中间(移除最后的CRC和部分数据)
    size_t truncate_size = content.size() / 2;
    content.resize(truncate_size);

    // 计算截断内容的正确CRC并附加
    uint32_t new_crc = RdbFormat::crc32c(content.data(), content.size());
    uint32_t new_crc_net = RdbFormat::hton32(new_crc);
    content.insert(content.end(), (uint8_t*)&new_crc_net,
                   (uint8_t*)&new_crc_net + 4);

    // 写入截断且CRC正确的文件
    std::ofstream out(rdb_path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(content.data()), content.size());
    out.close();

    // 尝试加载 - 应该失败(记录格式不完整)
    KvStore loaded;
    int count = manager.load(loaded);
    EXPECT_EQ(count, -1) << "截断的记录应该加载失败,即使CRC正确";
}

// 测试2: 真实 varint 第五字节溢出，且非空 Store 在加载失败后保持不变
TEST_F(RdbManagerTest, Load_RealVarintOverflowPreservesNonEmptyStore) {
    // record_count 必须与实际记录数一致，CRC 必须覆盖完整 header+record。
    std::vector<uint8_t> content = minidb_test::make_header(1);

    // 仅 key_len 是真实溢出 varint：第五字节无 continuation bit，
    // 但 payload=0x10 超过 uint32 可容纳的 4 位。
    const uint8_t overflow_key_len[] = {0xff, 0xff, 0xff, 0xff, 0x10};
    minidb_test::append_bytes(content, overflow_key_len,
                              sizeof(overflow_key_len));
    minidb_test::append_bytes(content, "bad", 3);
    minidb_test::append_length_prefixed(content, "value1");
    minidb_test::append_u64(content, 0);
    minidb_test::append_checksum(content);
    ASSERT_TRUE(minidb_test::write_file(rdb_path, content));

    auto clock =
        std::make_shared<minidb_test::FrozenClock>(5000, 1700000000000);
    KvStore store(clock);
    store.set("alpha", "value-alpha");
    std::string binary_value = "bin";
    binary_value.push_back('\0');
    binary_value.append("ary");
    binary_value.push_back(static_cast<char>(0xff));
    store.set("beta", binary_value);
    store.set("gamma", "value-gamma");
    ASSERT_TRUE(store.expire("beta", 60000));

    const size_t original_size = store.size();
    const auto original_alpha = store.get("alpha");
    const auto original_beta = store.get("beta");
    const auto original_gamma = store.get("gamma");
    const auto original_ttl = store.ttl_info("beta");
    ASSERT_EQ(original_ttl.state, KvStore::TtlState::kExpiring);
    ASSERT_EQ(original_ttl.remaining_ms, 60000);

    RdbManager manager(rdb_path);
    EXPECT_EQ(manager.load(store), -1);

    EXPECT_EQ(store.size(), original_size);
    EXPECT_EQ(store.get("alpha"), original_alpha);
    EXPECT_EQ(store.get("beta"), original_beta);
    EXPECT_EQ(store.get("gamma"), original_gamma);
    EXPECT_FALSE(store.get("bad").has_value());

    const auto alpha_ttl = store.ttl_info("alpha");
    const auto gamma_ttl = store.ttl_info("gamma");
    EXPECT_EQ(alpha_ttl.state, KvStore::TtlState::kNoExpiry);
    EXPECT_EQ(gamma_ttl.state, KvStore::TtlState::kNoExpiry);
    const auto remaining_ttl = store.ttl_info("beta");
    EXPECT_EQ(remaining_ttl.state, KvStore::TtlState::kExpiring);
    EXPECT_EQ(remaining_ttl.remaining_ms, original_ttl.remaining_ms);
}

// 测试3: Store不变性 - 多条记录的场景
TEST_F(RdbManagerTest, Load_FailurePreservesStoreWithMultipleKeys) {
    // 创建一个格式完全错误的文件(magic错误)
    std::ofstream file(rdb_path, std::ios::binary);
    file << "INVALID_MAGIC_HEADER";
    file.close();

    // 准备一个有多条记录的Store
    KvStore store;
    store.set("key1", "value1");
    store.set("key2", "value2");
    store.set("key3", "value3");
    store.expire("key2", 5000);

    size_t original_size = store.size();
    auto original_key1 = store.get("key1");
    auto original_key2 = store.get("key2");
    auto original_key3 = store.get("key3");
    auto original_ttl2 = store.ttl("key2");

    // 尝试加载损坏文件
    RdbManager manager(rdb_path);
    int count = manager.load(store);

    // 验证Store完全不变
    EXPECT_EQ(count, -1);
    EXPECT_EQ(store.size(), original_size);

    EXPECT_EQ(store.get("key1"), original_key1);
    EXPECT_EQ(store.get("key2"), original_key2);
    EXPECT_EQ(store.get("key3"), original_key3);
    EXPECT_EQ(store.ttl("key2"), original_ttl2);

    // 确认没有新增任何key
    EXPECT_FALSE(store.get("nonexistent").has_value());
}

// 测试4: 部分加载后失败的Store不变性
TEST_F(RdbManagerTest, Load_PartialLoadFailurePreservesStore) {
    // 声明 3 条记录；前两条完整，第三条 key 长度声称 100 但数据不足。
    std::vector<uint8_t> content = minidb_test::make_header(3);
    minidb_test::append_record(content, "key1", "value1");
    minidb_test::append_record(content, "key2", "value2");
    const auto bad_key_length = RdbFormat::encode_varint(100);
    content.insert(content.end(), bad_key_length.begin(), bad_key_length.end());
    minidb_test::append_bytes(content, "short", 5);
    minidb_test::append_checksum(content);
    ASSERT_TRUE(minidb_test::write_file(rdb_path, content));

    KvStore store;
    store.set("original", "data");
    const size_t original_size = store.size();

    RdbManager manager(rdb_path);
    EXPECT_EQ(manager.load(store), -1);
    EXPECT_EQ(store.size(), original_size);
    EXPECT_TRUE(store.get("original").has_value());
    EXPECT_FALSE(store.get("key1").has_value());
    EXPECT_FALSE(store.get("key2").has_value());
}
