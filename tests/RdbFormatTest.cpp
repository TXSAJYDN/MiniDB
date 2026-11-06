#include <gtest/gtest.h>
#include "../src/storage/RdbFormat.h"

using namespace minidb;

TEST(RdbFormat, Varint_SingleByte) {
    auto encoded = RdbFormat::encode_varint(0x42);
    EXPECT_EQ(encoded.size(), 1);
    EXPECT_EQ(encoded[0], 0x42);
}

TEST(RdbFormat, Varint_MultiByte) {
    auto encoded = RdbFormat::encode_varint(0x4000);
    EXPECT_GE(encoded.size(), 2);
    auto [value, bytes] =
        RdbFormat::decode_varint(encoded.data(), encoded.size());
    EXPECT_EQ(value, 0x4000);
    EXPECT_EQ(bytes, encoded.size());
}

TEST(RdbFormat, Varint_Max) {
    auto encoded = RdbFormat::encode_varint(0xFFFFFFFF);
    auto [value, bytes] =
        RdbFormat::decode_varint(encoded.data(), encoded.size());
    EXPECT_EQ(value, 0xFFFFFFFF);
}

TEST(RdbFormat, CRC32C_Empty) {
    uint32_t crc = RdbFormat::crc32c((uint8_t*)"", 0);
    EXPECT_EQ(crc, 0); // CRC of empty data
}

TEST(RdbFormat, CRC32C_KnownVector) {
    const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    EXPECT_EQ(RdbFormat::crc32c(data, sizeof(data)), 0xE3069283u);
}

TEST(RdbFormat, CRC32C_Deterministic) {
    const char data[] = "Hello, World!";
    uint32_t crc1 = RdbFormat::crc32c((uint8_t*)data, sizeof(data) - 1);
    uint32_t crc2 = RdbFormat::crc32c((uint8_t*)data, sizeof(data) - 1);
    EXPECT_EQ(crc1, crc2);
}

TEST(RdbFormat, NetworkByteOrder) {
    uint32_t value = 0x12345678;
    uint32_t net = RdbFormat::hton32(value);
    uint32_t back = RdbFormat::ntoh32(net);
    EXPECT_EQ(back, value);
}

TEST(RdbFormat, NetworkByteOrder64) {
    uint64_t value = 0x123456789ABCDEF0ULL;
    uint64_t net = RdbFormat::hton64(value);
    uint64_t back = RdbFormat::ntoh64(net);
    EXPECT_EQ(back, value);
}

// P0: 测试超长varint被拒绝（UBSan复现案例）
TEST(RdbFormat, Varint_TooLong) {
    // 输入: 80 80 80 80 80 00 - 6字节varint，会导致35位移位
    uint8_t data[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x00};
    auto result = RdbFormat::decode_varint(data, sizeof(data));

    // 应该返回错误（bytes_read = 0）
    EXPECT_EQ(0u, result.second) << "超长varint应该被拒绝";
}

// P0: 测试5字节varint的边界
// P0: 测试5字节varint的边界
TEST(RdbFormat, Varint_FiveByteMax) {
    // 最大5字节varint: 0xFFFFFFFF (全32位)
    // 编码: 0xFF 0xFF 0xFF 0xFF 0x0F
    uint8_t data[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
    auto result = RdbFormat::decode_varint(data, sizeof(data));

    EXPECT_EQ(5u, result.second) << "5字节varint应该被接受";
    EXPECT_EQ(0xFFFFFFFFu, result.first);
}

// P0: 测试第5字节仍有continuation bit的情况
TEST(RdbFormat, Varint_FifthByteContinuation) {
    // 第5字节(i=4)有continuation bit应该被拒绝
    uint8_t data[] = {0x80, 0x80, 0x80, 0x80, 0x80};
    auto result = RdbFormat::decode_varint(data, sizeof(data));

    EXPECT_EQ(0u, result.second) << "第5字节有continuation bit应该被拒绝";
}
// AI-014: Test that 5th byte with payload > 0x0f is rejected
TEST(RdbFormat, Varint_FifthBytePayloadOverflow) {
    // ff ff ff ff 10: 5 bytes, last byte has no continuation but payload=0x10
    // (> 0x0f)
    uint8_t data[] = {0xff, 0xff, 0xff, 0xff, 0x10};

    auto [value, bytes_read] = RdbFormat::decode_varint(data, sizeof(data));

    // Should reject: 5th byte payload exceeds 4 bits
    EXPECT_EQ(bytes_read, 0) << "5th byte payload 0x10 should be rejected";
    EXPECT_EQ(value, 0);
}

// AI-014: Test that 5th byte with payload = 0x0f is accepted (boundary)
TEST(RdbFormat, Varint_FifthBytePayloadMax) {
    // ff ff ff ff 0f: 5 bytes, payload=0x0f (maximum valid)
    uint8_t data[] = {0xff, 0xff, 0xff, 0xff, 0x0f};

    auto [value, bytes_read] = RdbFormat::decode_varint(data, sizeof(data));

    // Should accept: 0xFFFFFFFF is the maximum 32-bit value
    EXPECT_EQ(bytes_read, 5);
    EXPECT_EQ(value, 0xFFFFFFFF);
}

// AI-015: 测试超长 varint 导致的溢出
TEST(RdbFormat, Varint_OverflowPayload) {
    uint8_t overflow[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x10};
    auto [value, bytes_read] =
        RdbFormat::decode_varint(overflow, sizeof(overflow));
    EXPECT_EQ(bytes_read, 0);
}
