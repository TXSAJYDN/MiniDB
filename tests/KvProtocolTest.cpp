#include <gtest/gtest.h>

#include "../src/protocol/KvProtocol.h"

#include <arpa/inet.h>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace minidb {
namespace {

void append_net_u32(std::vector<uint8_t>* out, uint32_t value) {
    const uint32_t network = htonl(value);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&network);
    out->insert(out->end(), bytes, bytes + sizeof(network));
}

std::vector<uint8_t> make_raw_frame(uint32_t declared_len,
                                    const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> frame;
    append_net_u32(&frame, declared_len);
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

} // namespace

TEST(KvProtocolTest, RoundTripPingWithExactConsumption) {
    const auto encoded = KvProtocol::encode_command(KvProtocol::kPing, {});
    ASSERT_EQ(encoded.size(), 6u);

    KvProtocol::Command parsed;
    EXPECT_EQ(
        KvProtocol::parse_command(encoded.data(), encoded.size(), &parsed),
        static_cast<int>(encoded.size()));
    EXPECT_EQ(parsed.type, KvProtocol::kPing);
    EXPECT_TRUE(parsed.args.empty());
}

TEST(KvProtocolTest, FragmentedFrameWaitsAtEveryPrefix) {
    const auto encoded = KvProtocol::encode_command(
        KvProtocol::kSet,
        {std::string("key\0suffix", 10), std::string("value\0binary\xff", 13)});

    for (size_t prefix = 0; prefix < encoded.size(); ++prefix) {
        KvProtocol::Command parsed;
        ASSERT_EQ(KvProtocol::parse_command(encoded.data(), prefix, &parsed), 0)
            << "prefix=" << prefix;
    }

    KvProtocol::Command parsed;
    EXPECT_EQ(
        KvProtocol::parse_command(encoded.data(), encoded.size(), &parsed),
        static_cast<int>(encoded.size()));
    ASSERT_EQ(parsed.args.size(), 2u);
    EXPECT_EQ(parsed.args[0], std::string("key\0suffix", 10));
    EXPECT_EQ(parsed.args[1], std::string("value\0binary\xff", 13));
}

TEST(KvProtocolTest, PipelineConsumesExactlyOneFrame) {
    auto first = KvProtocol::encode_command(KvProtocol::kPing, {});
    const auto second = KvProtocol::encode_command(
        KvProtocol::kGet, {std::string("binary\0key", 10)});
    first.insert(first.end(), second.begin(), second.end());

    KvProtocol::Command parsed_first;
    const int consumed =
        KvProtocol::parse_command(first.data(), first.size(), &parsed_first);
    ASSERT_GT(consumed, 0);
    EXPECT_EQ(parsed_first.type, KvProtocol::kPing);

    KvProtocol::Command parsed_second;
    const size_t remaining = first.size() - static_cast<size_t>(consumed);
    EXPECT_EQ(KvProtocol::parse_command(first.data() + consumed, remaining,
                                        &parsed_second),
              static_cast<int>(remaining));
    ASSERT_EQ(parsed_second.args.size(), 1u);
    EXPECT_EQ(parsed_second.args[0], std::string("binary\0key", 10));
}

TEST(KvProtocolTest, MaximumKeyAndValueRoundTrip) {
    const std::string key(KvProtocol::kMaxKeySize, 'k');
    std::string value(KvProtocol::kMaxArgumentSize, '\0');
    for (size_t i = 0; i < value.size(); i += 4093) {
        value[i] = static_cast<char>((i * 17u) & 0xffu);
    }

    const auto encoded =
        KvProtocol::encode_command(KvProtocol::kSet, {key, value});
    ASSERT_LE(encoded.size(), KvProtocol::kMaxFrameSize + 4u);

    KvProtocol::Command parsed;
    EXPECT_EQ(
        KvProtocol::parse_command(encoded.data(), encoded.size(), &parsed),
        static_cast<int>(encoded.size()));
    ASSERT_EQ(parsed.args.size(), 2u);
    EXPECT_EQ(parsed.args[0], key);
    EXPECT_EQ(parsed.args[1], value);
}

TEST(KvProtocolTest, EncodeRejectsArgumentAndFrameLimits) {
    EXPECT_THROW(
        KvProtocol::encode_command(
            KvProtocol::kSet,
            {std::string(static_cast<size_t>(KvProtocol::kMaxArgumentSize) + 1,
                         'v')}),
        std::invalid_argument);

    std::vector<std::string> too_many(KvProtocol::kMaxArgumentCount + 1,
                                      std::string());
    EXPECT_THROW(KvProtocol::encode_command(KvProtocol::kPing, too_many),
                 std::invalid_argument);

    const std::string large_key(64u * 1024u, 'k');
    const std::string max_value(KvProtocol::kMaxArgumentSize, 'v');
    EXPECT_THROW(
        KvProtocol::encode_command(KvProtocol::kSet, {large_key, max_value}),
        std::invalid_argument);
}

TEST(KvProtocolTest, RejectsMalformedDeclaredFrameLengths) {
    const uint8_t too_short[] = {0, 0, 0, 1};
    KvProtocol::Command parsed;
    EXPECT_EQ(KvProtocol::parse_command(too_short, sizeof(too_short), &parsed),
              -1);

    std::vector<uint8_t> too_large;
    append_net_u32(&too_large,
                   static_cast<uint32_t>(KvProtocol::kMaxFrameSize) + 1u);
    EXPECT_EQ(
        KvProtocol::parse_command(too_large.data(), too_large.size(), &parsed),
        -1);
}

TEST(KvProtocolTest, RejectsArgumentLengthAndTrailingBytes) {
    std::vector<uint8_t> payload{KvProtocol::kGet, 1};
    append_net_u32(&payload,
                   static_cast<uint32_t>(KvProtocol::kMaxArgumentSize) + 1u);
    const auto oversized_arg =
        make_raw_frame(static_cast<uint32_t>(payload.size()), payload);
    KvProtocol::Command parsed;
    EXPECT_EQ(KvProtocol::parse_command(oversized_arg.data(),
                                        oversized_arg.size(), &parsed),
              -1);

    std::vector<uint8_t> truncated_payload{KvProtocol::kGet, 1};
    append_net_u32(&truncated_payload, 4);
    truncated_payload.insert(truncated_payload.end(), {'a', 'b', 'c'});
    const auto truncated = make_raw_frame(
        static_cast<uint32_t>(truncated_payload.size()), truncated_payload);
    EXPECT_EQ(
        KvProtocol::parse_command(truncated.data(), truncated.size(), &parsed),
        -1);

    std::vector<uint8_t> trailing_payload{KvProtocol::kPing, 0, 0};
    const auto trailing = make_raw_frame(
        static_cast<uint32_t>(trailing_payload.size()), trailing_payload);
    EXPECT_EQ(
        KvProtocol::parse_command(trailing.data(), trailing.size(), &parsed),
        -1);
}

TEST(KvProtocolTest, PreservesUnknownCommandTypeForSessionValidation) {
    std::vector<uint8_t> payload{0xfe, 0};
    const auto frame =
        make_raw_frame(static_cast<uint32_t>(payload.size()), payload);
    KvProtocol::Command parsed;
    EXPECT_EQ(KvProtocol::parse_command(frame.data(), frame.size(), &parsed),
              static_cast<int>(frame.size()));
    EXPECT_EQ(static_cast<int>(parsed.type), 0xfe);
    EXPECT_TRUE(parsed.args.empty());
}

TEST(KvProtocolTest, TransactionCommandCodesAndNamesAreFrozen) {
    EXPECT_EQ(static_cast<int>(KvProtocol::kBegin), 9);
    EXPECT_EQ(static_cast<int>(KvProtocol::kCommit), 10);
    EXPECT_EQ(static_cast<int>(KvProtocol::kRollback), 11);
    EXPECT_EQ(static_cast<int>(KvProtocol::kConflict), 105);
    EXPECT_EQ(KvProtocol::command_type_to_string(KvProtocol::kBegin), "BEGIN");
    EXPECT_EQ(KvProtocol::command_type_to_string(KvProtocol::kCommit),
              "COMMIT");
    EXPECT_EQ(KvProtocol::command_type_to_string(KvProtocol::kRollback),
              "ROLLBACK");
    EXPECT_EQ(KvProtocol::command_type_to_string(KvProtocol::kConflict),
              "CONFLICT");

    for (const auto type :
         {KvProtocol::kBegin, KvProtocol::kCommit, KvProtocol::kRollback}) {
        const auto encoded = KvProtocol::encode_command(type, {});
        KvProtocol::Command parsed;
        EXPECT_EQ(
            KvProtocol::parse_command(encoded.data(), encoded.size(), &parsed),
            static_cast<int>(encoded.size()));
        EXPECT_EQ(static_cast<int>(parsed.type), static_cast<int>(type));
        EXPECT_TRUE(parsed.args.empty());
    }
}

TEST(KvProtocolTest, NullInputsAreRejected) {
    KvProtocol::Command parsed;
    const uint8_t byte = 0;
    EXPECT_EQ(KvProtocol::parse_command(nullptr, 4, &parsed), -1);
    EXPECT_EQ(KvProtocol::parse_command(&byte, 1, nullptr), -1);
}

} // namespace minidb
