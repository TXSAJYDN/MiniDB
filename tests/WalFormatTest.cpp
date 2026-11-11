#include <gtest/gtest.h>

#include "../src/storage/WalFormat.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace minidb {
namespace {

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

void store_u64(std::uint8_t* data, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        data[i] = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
}

void refresh_crc(std::vector<std::uint8_t>* bytes) {
    ASSERT_GE(bytes->size(), WalFormat::CHECKSUM_SIZE);
    const std::size_t crc_offset = bytes->size() - WalFormat::CHECKSUM_SIZE;
    store_u32(bytes->data() + crc_offset,
              WalFormat::crc32c(bytes->data(), crc_offset));
}

WalRecord MakeSet(std::uint64_t lsn = 1) {
    WalRecord record;
    record.lsn = lsn;
    record.op = WalOp::kSet;
    record.key = "alpha";
    record.value = "beta";
    return record;
}

std::vector<WalTxnOp> MakeTransactionOperations() {
    WalTxnOp set;
    set.op = WalOp::kSet;
    set.key = "alpha";
    set.value = "one";

    WalTxnOp expire;
    expire.op = WalOp::kExpire;
    expire.key = "ttl";
    expire.expiry_ms = 1760000000123LL;

    WalTxnOp del;
    del.op = WalOp::kDel;
    del.key = "gone";
    return {set, expire, del};
}

WalRecord MakeTransaction(std::uint64_t lsn = 9) {
    WalRecord record;
    record.lsn = lsn;
    record.op = WalOp::kTxn;
    record.key = "txn";
    const std::vector<std::uint8_t> payload =
        WalFormat::encode_transaction(MakeTransactionOperations());
    record.value.assign(reinterpret_cast<const char*>(payload.data()),
                        payload.size());
    return record;
}

TEST(WalFormatTest, EncodedSizeMatchesWireLayout) {
    const WalRecord record = MakeSet(42);
    EXPECT_EQ(WalFormat::encoded_size(record),
              WalFormat::HEADER_SIZE + record.key.size() + record.value.size() +
                  WalFormat::CHECKSUM_SIZE);
}

TEST(WalFormatTest, SetRoundTripsWithBigEndianHeader) {
    const WalRecord original = MakeSet(0x0102030405060708ULL);
    std::string error;
    const std::vector<std::uint8_t> encoded =
        WalFormat::encode(original, &error);
    ASSERT_FALSE(encoded.empty()) << error;
    ASSERT_GE(encoded.size(), WalFormat::HEADER_SIZE + 4);
    EXPECT_EQ(std::memcmp(encoded.data(), WalFormat::MAGIC, 4), 0);
    EXPECT_EQ(encoded[4], 0);
    EXPECT_EQ(encoded[5], 2);
    EXPECT_EQ(encoded[6], 0);
    EXPECT_EQ(encoded[7], 0);
    EXPECT_EQ(encoded[8], 1);
    EXPECT_EQ(encoded[15], 8);

    WalRecord decoded;
    std::size_t consumed = 0;
    EXPECT_EQ(WalFormat::decode(encoded.data(), encoded.size(), &decoded,
                                &consumed, &error),
              WalFormat::DecodeStatus::kOk)
        << error;
    EXPECT_EQ(consumed, encoded.size());
    EXPECT_EQ(decoded.lsn, original.lsn);
    EXPECT_EQ(decoded.op, WalOp::kSet);
    EXPECT_EQ(decoded.key, original.key);
    EXPECT_EQ(decoded.value, original.value);
    EXPECT_EQ(decoded.expiry_ms, 0);
}

TEST(WalFormatTest, DelAndExpireRoundTrip) {
    WalRecord del;
    del.lsn = 7;
    del.op = WalOp::kDel;
    del.key = "gone";

    WalRecord expire;
    expire.lsn = 8;
    expire.op = WalOp::kExpire;
    expire.key = "ttl";
    expire.expiry_ms = 1760000000123LL;

    for (const WalRecord& original : {del, expire}) {
        const std::vector<std::uint8_t> encoded = WalFormat::encode(original);
        ASSERT_FALSE(encoded.empty());
        WalRecord decoded;
        std::size_t consumed = 0;
        ASSERT_EQ(WalFormat::decode(encoded.data(), encoded.size(), &decoded,
                                    &consumed),
                  WalFormat::DecodeStatus::kOk);
        EXPECT_EQ(consumed, encoded.size());
        EXPECT_EQ(decoded.lsn, original.lsn);
        EXPECT_EQ(decoded.op, original.op);
        EXPECT_EQ(decoded.key, original.key);
        EXPECT_TRUE(decoded.value.empty());
        EXPECT_EQ(decoded.expiry_ms, original.expiry_ms);
    }
}

TEST(WalFormatTest, TrailingBytesRemainForCallerToReject) {
    std::vector<std::uint8_t> encoded = WalFormat::encode(MakeSet());
    ASSERT_FALSE(encoded.empty());
    const std::size_t record_size = encoded.size();
    encoded.push_back(0xaa);
    WalRecord decoded;
    std::size_t consumed = 0;
    EXPECT_EQ(
        WalFormat::decode(encoded.data(), encoded.size(), &decoded, &consumed),
        WalFormat::DecodeStatus::kOk);
    EXPECT_EQ(consumed, record_size);
    EXPECT_EQ(record_size + 1, encoded.size());
}

TEST(WalFormatTest, TruncatedPrefixesReportNeedMore) {
    const std::vector<std::uint8_t> encoded = WalFormat::encode(MakeSet());
    ASSERT_GT(encoded.size(), 4u);
    for (std::size_t len = 1; len < encoded.size(); ++len) {
        SCOPED_TRACE(len);
        WalRecord decoded;
        EXPECT_EQ(WalFormat::decode(encoded.data(), len, &decoded),
                  WalFormat::DecodeStatus::kNeedMore);
    }
}

TEST(WalFormatTest, RejectsMalformedHeaderFields) {
    struct Mutation {
        const char* name;
        std::size_t offset;
        std::uint8_t value;
    };
    const Mutation mutations[] = {
        {"magic", 0, 'X'}, {"version-high", 4, 1}, {"version-low", 5, 3},
        {"flags", 7, 1},   {"reserved", 17, 1},    {"operation", 16, 99},
    };
    for (const Mutation& mutation : mutations) {
        SCOPED_TRACE(mutation.name);
        std::vector<std::uint8_t> bytes = WalFormat::encode(MakeSet());
        bytes[mutation.offset] = mutation.value;
        refresh_crc(&bytes);
        WalRecord decoded;
        EXPECT_EQ(WalFormat::decode(bytes.data(), bytes.size(), &decoded),
                  WalFormat::DecodeStatus::kCorrupt);
    }
}

TEST(WalFormatTest, RejectsZeroLsnAndChecksumMismatch) {
    std::vector<std::uint8_t> bytes = WalFormat::encode(MakeSet());
    store_u64(bytes.data() + 8, 0);
    refresh_crc(&bytes);
    WalRecord decoded;
    EXPECT_EQ(WalFormat::decode(bytes.data(), bytes.size(), &decoded),
              WalFormat::DecodeStatus::kCorrupt);

    bytes = WalFormat::encode(MakeSet());
    bytes.back() ^= 0x80;
    EXPECT_EQ(WalFormat::decode(bytes.data(), bytes.size(), &decoded),
              WalFormat::DecodeStatus::kCorrupt);
}

TEST(WalFormatTest, RejectsOutOfRangeKeyAndValueLengths) {
    std::vector<std::uint8_t> bytes = WalFormat::encode(MakeSet());
    store_u32(bytes.data() + 18, 0);
    refresh_crc(&bytes);
    WalRecord decoded;
    EXPECT_EQ(WalFormat::decode(bytes.data(), bytes.size(), &decoded),
              WalFormat::DecodeStatus::kCorrupt);

    bytes = WalFormat::encode(MakeSet());
    store_u32(bytes.data() + 18,
              static_cast<std::uint32_t>(WalFormat::MAX_KEY_SIZE + 1));
    EXPECT_EQ(WalFormat::decode(bytes.data(), bytes.size(), &decoded),
              WalFormat::DecodeStatus::kCorrupt);

    bytes = WalFormat::encode(MakeSet());
    store_u32(bytes.data() + 22,
              static_cast<std::uint32_t>(WalFormat::MAX_VALUE_SIZE + 1));
    EXPECT_EQ(WalFormat::decode(bytes.data(), bytes.size(), &decoded),
              WalFormat::DecodeStatus::kCorrupt);
}

TEST(WalFormatTest, RejectsOperationSpecificPayloadViolations) {
    WalRecord record = MakeSet();
    record.op = WalOp::kDel;
    record.value = "x";
    EXPECT_FALSE(WalFormat::validate(record));
    record.value.clear();
    record.expiry_ms = 1;
    EXPECT_FALSE(WalFormat::validate(record));
    record.expiry_ms = 0;
    EXPECT_TRUE(WalFormat::validate(record));

    record.op = WalOp::kExpire;
    EXPECT_FALSE(WalFormat::validate(record));
    record.expiry_ms = 1;
    EXPECT_TRUE(WalFormat::validate(record));
    record.value = "x";
    EXPECT_FALSE(WalFormat::validate(record));
}

TEST(WalFormatTest, RejectsNegativeExpiryAndEmptyKey) {
    WalRecord record = MakeSet();
    record.expiry_ms = -1;
    EXPECT_FALSE(WalFormat::validate(record));
    record.expiry_ms = 0;
    record.key.clear();
    EXPECT_FALSE(WalFormat::validate(record));
}

TEST(WalFormatTest, AcceptsBoundaryLengthsAndRejectsBeyondThem) {
    WalRecord record = MakeSet();
    record.key.assign(WalFormat::MAX_KEY_SIZE, 'k');
    record.value.assign(WalFormat::MAX_VALUE_SIZE, 'v');
    EXPECT_TRUE(WalFormat::validate(record));
    const std::vector<std::uint8_t> bytes = WalFormat::encode(record);
    EXPECT_EQ(bytes.size(), WalFormat::MAX_RECORD_SIZE);

    record.key.push_back('x');
    EXPECT_FALSE(WalFormat::validate(record));
    record.key.resize(WalFormat::MAX_KEY_SIZE);
    record.value.push_back('x');
    EXPECT_FALSE(WalFormat::validate(record));
}

TEST(WalFormatTest,
     DecodesLegacyStandaloneRecordsButRejectsLegacyTransactions) {
    std::vector<std::uint8_t> standalone = WalFormat::encode(MakeSet());
    store_u16(standalone.data() + 4, WalFormat::LEGACY_VERSION);
    refresh_crc(&standalone);
    WalRecord decoded;
    EXPECT_EQ(WalFormat::decode(standalone.data(), standalone.size(), &decoded),
              WalFormat::DecodeStatus::kOk);
    EXPECT_EQ(decoded.key, "alpha");

    std::vector<std::uint8_t> transaction =
        WalFormat::encode(MakeTransaction());
    ASSERT_FALSE(transaction.empty());
    store_u16(transaction.data() + 4, WalFormat::LEGACY_VERSION);
    refresh_crc(&transaction);
    EXPECT_EQ(
        WalFormat::decode(transaction.data(), transaction.size(), &decoded),
        WalFormat::DecodeStatus::kCorrupt);
}

TEST(WalFormatTest, TransactionRoundTripsInsideOneWalRecord) {
    const WalRecord original = MakeTransaction(17);
    std::string error;
    const std::vector<std::uint8_t> encoded =
        WalFormat::encode(original, &error);
    ASSERT_FALSE(encoded.empty()) << error;
    EXPECT_EQ(encoded[16], static_cast<std::uint8_t>(WalOp::kTxn));

    WalRecord decoded;
    std::size_t consumed = 0;
    ASSERT_EQ(WalFormat::decode(encoded.data(), encoded.size(), &decoded,
                                &consumed, &error),
              WalFormat::DecodeStatus::kOk)
        << error;
    EXPECT_EQ(consumed, encoded.size());
    EXPECT_EQ(decoded.op, WalOp::kTxn);
    EXPECT_EQ(decoded.key, "txn");
    EXPECT_EQ(decoded.expiry_ms, 0);

    std::vector<WalTxnOp> operations;
    ASSERT_EQ(WalFormat::decode_transaction(
                  reinterpret_cast<const std::uint8_t*>(decoded.value.data()),
                  decoded.value.size(), &operations, &error),
              WalFormat::DecodeStatus::kOk)
        << error;
    ASSERT_EQ(operations.size(), 3u);
    EXPECT_EQ(operations[0].op, WalOp::kSet);
    EXPECT_EQ(operations[0].key, "alpha");
    EXPECT_EQ(operations[0].value, "one");
    EXPECT_EQ(operations[1].op, WalOp::kExpire);
    EXPECT_EQ(operations[1].expiry_ms, 1760000000123LL);
    EXPECT_EQ(operations[2].op, WalOp::kDel);
    EXPECT_TRUE(operations[2].value.empty());
}

TEST(WalFormatTest, RejectsInvalidTransactionOuterFields) {
    WalRecord wrong_key = MakeTransaction();
    wrong_key.key = "not-txn";
    EXPECT_FALSE(WalFormat::validate(wrong_key));

    WalRecord wrong_expiry = MakeTransaction();
    wrong_expiry.expiry_ms = 1;
    EXPECT_FALSE(WalFormat::validate(wrong_expiry));

    WalRecord empty_payload = MakeTransaction();
    empty_payload.value.clear();
    EXPECT_FALSE(WalFormat::validate(empty_payload));
}

TEST(WalFormatTest, RejectsMalformedTransactionOperationsInEncoder) {
    std::vector<WalTxnOp> operations = MakeTransactionOperations();
    operations.push_back(operations.front());
    EXPECT_FALSE(WalFormat::validate_transaction(operations));

    operations = MakeTransactionOperations();
    operations[0].key.clear();
    EXPECT_FALSE(WalFormat::validate_transaction(operations));

    operations = MakeTransactionOperations();
    operations[0].op = WalOp::kTxn;
    EXPECT_FALSE(WalFormat::validate_transaction(operations));

    operations = MakeTransactionOperations();
    operations[1].expiry_ms = 0;
    EXPECT_FALSE(WalFormat::validate_transaction(operations));

    operations = MakeTransactionOperations();
    operations[2].value = "unexpected";
    EXPECT_FALSE(WalFormat::validate_transaction(operations));
}

TEST(WalFormatTest, TransactionPayloadRejectsMalformedWireFields) {
    std::vector<WalTxnOp> operations;
    WalTxnOp one;
    one.op = WalOp::kSet;
    one.key = "a";
    WalTxnOp two = one;
    two.key = "b";
    operations = {one, two};
    std::vector<std::uint8_t> payload =
        WalFormat::encode_transaction(operations);
    ASSERT_FALSE(payload.empty());

    auto decode = [](const std::vector<std::uint8_t>& bytes) {
        std::vector<WalTxnOp> decoded;
        return WalFormat::decode_transaction(bytes.data(), bytes.size(),
                                             &decoded);
    };

    std::vector<std::uint8_t> malformed = payload;
    malformed[0] = 'X';
    EXPECT_EQ(decode(malformed), WalFormat::DecodeStatus::kCorrupt);

    malformed = payload;
    store_u16(malformed.data() + 4, 2);
    EXPECT_EQ(decode(malformed), WalFormat::DecodeStatus::kCorrupt);

    malformed = payload;
    store_u16(malformed.data() + 6, 0);
    EXPECT_EQ(decode(malformed), WalFormat::DecodeStatus::kCorrupt);

    malformed = payload;
    store_u16(malformed.data() + 6, 257);
    EXPECT_EQ(decode(malformed), WalFormat::DecodeStatus::kCorrupt);

    malformed = payload;
    malformed[WalFormat::TXN_HEADER_SIZE] = 9;
    EXPECT_EQ(decode(malformed), WalFormat::DecodeStatus::kCorrupt);

    malformed = payload;
    malformed[WalFormat::TXN_HEADER_SIZE + 1] = 1;
    EXPECT_EQ(decode(malformed), WalFormat::DecodeStatus::kCorrupt);

    malformed = payload;
    store_u64(malformed.data() + WalFormat::TXN_HEADER_SIZE + 8,
              static_cast<std::uint64_t>(-1));
    EXPECT_EQ(decode(malformed), WalFormat::DecodeStatus::kCorrupt);

    malformed = payload;
    const std::size_t second_key_offset = WalFormat::TXN_HEADER_SIZE +
                                          WalFormat::TXN_OP_HEADER_SIZE + 1 +
                                          WalFormat::TXN_OP_HEADER_SIZE;
    malformed[second_key_offset] = 'a';
    EXPECT_EQ(decode(malformed), WalFormat::DecodeStatus::kCorrupt);

    malformed = payload;
    malformed.push_back(0);
    EXPECT_EQ(decode(malformed), WalFormat::DecodeStatus::kCorrupt);

    malformed = payload;
    store_u32(malformed.data() + WalFormat::TXN_HEADER_SIZE + 4,
              WalFormat::MAX_VALUE_SIZE + 1);
    EXPECT_EQ(decode(malformed), WalFormat::DecodeStatus::kCorrupt);
}

TEST(WalFormatTest, TransactionPayloadReportsTruncation) {
    const std::vector<std::uint8_t> payload =
        WalFormat::encode_transaction(MakeTransactionOperations());
    ASSERT_FALSE(payload.empty());
    std::vector<WalTxnOp> operations;
    for (std::size_t len = 0; len < payload.size(); ++len) {
        SCOPED_TRACE(len);
        EXPECT_EQ(
            WalFormat::decode_transaction(payload.data(), len, &operations),
            WalFormat::DecodeStatus::kNeedMore);
    }
}

TEST(WalFormatTest, TransactionPayloadEnforcesCountKeyAndSizeBounds) {
    std::vector<WalTxnOp> operations;
    WalTxnOp operation;
    operation.op = WalOp::kSet;
    operation.key.assign(WalFormat::MAX_KEY_SIZE, 'k');
    operations.push_back(operation);
    EXPECT_TRUE(WalFormat::validate_transaction(operations));
    EXPECT_EQ(WalFormat::encode_transaction(operations).size(),
              WalFormat::TXN_HEADER_SIZE + WalFormat::TXN_OP_HEADER_SIZE +
                  WalFormat::MAX_KEY_SIZE);

    operation.key.push_back('x');
    operations[0] = operation;
    EXPECT_FALSE(WalFormat::validate_transaction(operations));

    operations.clear();
    for (std::size_t index = 0; index < WalFormat::MAX_TXN_OPS + 1; ++index) {
        WalTxnOp item;
        item.key = "key-" + std::to_string(index);
        operations.push_back(item);
    }
    EXPECT_FALSE(WalFormat::validate_transaction(operations));

    operations.clear();
    operation.key = "k";
    operation.value.assign(
        WalFormat::MAX_TXN_PAYLOAD_SIZE - WalFormat::TXN_HEADER_SIZE -
            WalFormat::TXN_OP_HEADER_SIZE - operation.key.size(),
        'v');
    operations.push_back(operation);
    EXPECT_TRUE(WalFormat::validate_transaction(operations));
    EXPECT_EQ(WalFormat::encode_transaction(operations).size(),
              WalFormat::MAX_TXN_PAYLOAD_SIZE);

    operation.value.push_back('x');
    operations[0] = operation;
    EXPECT_FALSE(WalFormat::validate_transaction(operations));
}

TEST(WalFormatTest, NullDecoderInputIsCorrupt) {
    WalRecord decoded;
    EXPECT_EQ(WalFormat::decode(nullptr, 12, &decoded),
              WalFormat::DecodeStatus::kCorrupt);
    const std::vector<std::uint8_t> bytes = WalFormat::encode(MakeSet());
    EXPECT_EQ(WalFormat::decode(bytes.data(), bytes.size(), nullptr),
              WalFormat::DecodeStatus::kCorrupt);
}

} // namespace
} // namespace minidb
