#include "../src/storage/AgentRecordCodec.h"

#include <array>
#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <string_view>
#include <vector>

namespace minidb {
namespace {

using C = AgentRecordCodec;

std::string make_session_payload(std::string_view state = "state") {
    C::SessionPayload payload;
    payload.session_expires_at_ms = 1760000000123LL;
    payload.state.assign(state.data(), state.size());
    std::string encoded;
    std::string error;
    EXPECT_TRUE(C::encode_session_payload(payload, &encoded, &error)) << error;
    return encoded;
}

std::string make_tool_payload(std::string_view tool_id = "tool-1") {
    C::ToolPayload payload;
    payload.tool_id = std::string(tool_id);
    payload.name = "shell";
    payload.request = "echo hi";
    payload.result = "hi";
    std::string encoded;
    std::string error;
    EXPECT_TRUE(C::encode_tool_payload(payload, &encoded, &error)) << error;
    return encoded;
}

std::string make_response(std::string_view payload = "") {
    C::Response response;
    response.status = static_cast<std::uint8_t>(C::ResponseStatus::kOk);
    response.revision = 9;
    response.fence = 4;
    response.lsn = 12;
    response.payload.assign(payload.data(), payload.size());
    std::string encoded;
    std::string error;
    EXPECT_TRUE(C::encode_response(response, &encoded, &error)) << error;
    return encoded;
}

std::string make_idempotency_payload(std::string_view response = {}) {
    C::IdempotencyPayload payload;
    payload.request_hash = Sha256::digest("canonical");
    payload.expires_at_ms = 1760000000123LL;
    payload.canonical_request_len = 37;
    payload.response =
        response.empty() ? make_response("saved") : std::string(response);
    std::string encoded;
    std::string error;
    EXPECT_TRUE(C::encode_idempotency_payload(payload, &encoded, &error))
        << error;
    return encoded;
}

C::Record make_record(C::RecordType type, std::uint8_t status,
                      std::string payload) {
    C::Record record;
    record.type = type;
    record.status = status;
    record.revision = type == C::RecordType::kIdempotency ? 0 : 3;
    const bool has_fence =
        type == C::RecordType::kSession || type == C::RecordType::kTool;
    record.last_fence = has_fence ? 2 : 0;
    record.created_at_ms = 1700000000000LL;
    record.updated_at_ms = 1700000000100LL;
    record.lease_expires_at_ms = has_fence ? 1700000001000LL : 0;
    record.payload = std::move(payload);
    return record;
}

void expect_encoded_record_decodes(const C::Record& original) {
    std::string encoded;
    std::string error;
    ASSERT_TRUE(C::encode_record(original, &encoded, &error)) << error;
    C::Record decoded;
    ASSERT_TRUE(C::decode_record(encoded, &decoded, &error)) << error;
    EXPECT_EQ(decoded.type, original.type);
    EXPECT_EQ(decoded.status, original.status);
    EXPECT_EQ(decoded.revision, original.revision);
    EXPECT_EQ(decoded.last_fence, original.last_fence);
    EXPECT_EQ(decoded.created_at_ms, original.created_at_ms);
    EXPECT_EQ(decoded.updated_at_ms, original.updated_at_ms);
    EXPECT_EQ(decoded.lease_expires_at_ms, original.lease_expires_at_ms);
    EXPECT_EQ(decoded.payload, original.payload);
}

} // namespace

TEST(AgentRecordCodecTest, ReservedNamespaceAndKeys) {
    EXPECT_EQ(C::kReservedPrefix.size(), C::Limits::kReservedPrefixSize);
    EXPECT_TRUE(C::is_reserved_key(std::string(C::kReservedPrefix) + "m"));
    EXPECT_FALSE(C::is_reserved_key("minidb-agent-v1"));
    EXPECT_FALSE(C::is_reserved_key(""));

    const std::string marker_key = C::marker_key();
    EXPECT_EQ(marker_key, std::string(C::kReservedPrefix) + "m");
    EXPECT_TRUE(C::is_reserved_key(marker_key));

    std::string key;
    std::string error;
    ASSERT_TRUE(C::session_key("session-1", &key, &error)) << error;
    C::KeyInfo info;
    ASSERT_TRUE(C::decode_key(key, &info, &error)) << error;
    EXPECT_EQ(info.kind, C::KeyKind::kSession);
    EXPECT_EQ(info.session_id, "session-1");

    ASSERT_TRUE(C::tool_key("session-1", "tool-1", &key, &error)) << error;
    ASSERT_TRUE(C::decode_key(key, &info, &error)) << error;
    EXPECT_EQ(info.kind, C::KeyKind::kTool);
    EXPECT_EQ(info.session_id, "session-1");
    EXPECT_EQ(info.tool_id, "tool-1");

    const std::string binary_idem("idem\0key", 8);
    ASSERT_TRUE(C::idempotency_key(binary_idem, &key, &error)) << error;
    ASSERT_TRUE(C::decode_key(key, &info, &error)) << error;
    EXPECT_EQ(info.kind, C::KeyKind::kIdempotency);
    EXPECT_EQ(info.idempotency_key, binary_idem);

    ASSERT_TRUE(C::generic_key("record-1", &key, &error)) << error;
    ASSERT_TRUE(C::decode_key(key, &info, &error)) << error;
    EXPECT_EQ(info.kind, C::KeyKind::kGeneric);
    EXPECT_EQ(info.name, "record-1");
}

TEST(AgentRecordCodecTest, RejectsInvalidAgentKeys) {
    std::string ignored;
    std::string error;
    EXPECT_FALSE(C::session_key("", &ignored, &error));
    EXPECT_FALSE(C::session_key(std::string("x\0y", 3), &ignored, &error));
    EXPECT_FALSE(C::session_key(std::string(129, 's'), &ignored, &error));
    EXPECT_FALSE(C::tool_key("session", "", &ignored, &error));
    EXPECT_FALSE(C::idempotency_key("", &ignored, &error));
    EXPECT_FALSE(C::idempotency_key(std::string(129, 'i'), &ignored, &error));
    EXPECT_FALSE(C::generic_key(std::string("a\0b", 3), &ignored, &error));

    C::KeyInfo info;
    EXPECT_FALSE(C::decode_key("plain", &info, &error));
    EXPECT_FALSE(
        C::decode_key(std::string(C::kReservedPrefix) + "x", &info, &error));
    EXPECT_FALSE(
        C::decode_key(std::string(C::kReservedPrefix) + "s", &info, &error));
    EXPECT_FALSE(C::decode_key(std::string(C::kReservedPrefix) + "t\0only",
                               &info, &error));
}

TEST(AgentRecordCodecTest, MarkerRoundTripAndCorruption) {
    const std::string marker = C::encode_marker();
    ASSERT_EQ(marker.size(), C::Limits::kMarkerSize);
    EXPECT_EQ(marker.substr(0, 4), "MDAM");
    std::string error;
    EXPECT_TRUE(C::decode_marker(marker, &error)) << error;

    for (std::size_t length = 0; length < marker.size(); ++length) {
        SCOPED_TRACE(length);
        EXPECT_FALSE(C::decode_marker(marker.substr(0, length), &error));
    }
    EXPECT_FALSE(C::decode_marker(marker + std::string(1, '\0'), &error));

    std::string bad = marker;
    bad[0] = 'X';
    EXPECT_FALSE(C::decode_marker(bad, &error));
    bad = marker;
    bad[5] = 2;
    EXPECT_FALSE(C::decode_marker(bad, &error));
    bad = marker;
    bad[7] = 1;
    EXPECT_FALSE(C::decode_marker(bad, &error));
}

TEST(AgentRecordCodecTest, SessionPayloadRoundTripAndStrictLength) {
    C::SessionPayload original;
    original.session_expires_at_ms = 1760000000123LL;
    original.state.assign("a\0b", 3);
    std::string encoded;
    std::string error;
    ASSERT_TRUE(C::encode_session_payload(original, &encoded, &error)) << error;
    EXPECT_EQ(encoded.substr(0, 4), "MSES");

    C::SessionPayload decoded;
    ASSERT_TRUE(C::decode_session_payload(encoded, &decoded, &error)) << error;
    EXPECT_EQ(decoded.session_expires_at_ms, original.session_expires_at_ms);
    EXPECT_EQ(decoded.state, original.state);
    EXPECT_EQ(std::string_view(encoded).substr(4, 2),
              std::string_view("\0\1", 2));

    for (std::size_t length = 0; length < encoded.size(); ++length) {
        SCOPED_TRACE(length);
        EXPECT_FALSE(C::decode_session_payload(encoded.substr(0, length),
                                               &decoded, &error));
    }
    EXPECT_FALSE(C::decode_session_payload(encoded + std::string(1, 'x'),
                                           &decoded, &error));
    std::string bad = encoded;
    bad[0] = 'X';
    EXPECT_FALSE(C::decode_session_payload(bad, &decoded, &error));
    bad = encoded;
    bad[5] = 2;
    EXPECT_FALSE(C::decode_session_payload(bad, &decoded, &error));
    bad = encoded;
    bad[7] = 1;
    EXPECT_FALSE(C::decode_session_payload(bad, &decoded, &error));
    bad = encoded;
    bad[18] = 0;
    bad[19] = 4;
    EXPECT_FALSE(C::decode_session_payload(bad, &decoded, &error));
}

TEST(AgentRecordCodecTest, ToolPayloadRoundTripWithBinaryFields) {
    C::ToolPayload original;
    original.tool_id = "tool-1";
    original.name.assign("na\0me", 5);
    original.request.assign("req\0uest", 8);
    original.result.assign("res\0ult", 8);
    std::string encoded;
    std::string error;
    ASSERT_TRUE(C::encode_tool_payload(original, &encoded, &error)) << error;
    EXPECT_EQ(encoded.size(),
              C::Limits::kToolPayloadHeaderSize + 6 + 5 + 8 + 8);
    EXPECT_EQ(encoded.substr(0, 4), "MTOL");

    C::ToolPayload decoded;
    ASSERT_TRUE(C::decode_tool_payload(encoded, &decoded, &error)) << error;
    EXPECT_EQ(decoded.tool_id, original.tool_id);
    EXPECT_EQ(decoded.name, original.name);
    EXPECT_EQ(decoded.request, original.request);
    EXPECT_EQ(decoded.result, original.result);

    for (std::size_t length = 0; length < encoded.size(); ++length) {
        SCOPED_TRACE(length);
        EXPECT_FALSE(C::decode_tool_payload(encoded.substr(0, length), &decoded,
                                            &error));
    }
    EXPECT_FALSE(C::decode_tool_payload(encoded + std::string(1, 'x'), &decoded,
                                        &error));
    std::string bad = encoded;
    bad[4] = 2;
    EXPECT_FALSE(C::decode_tool_payload(bad, &decoded, &error));
    bad = encoded;
    bad[7] = 1;
    EXPECT_FALSE(C::decode_tool_payload(bad, &decoded, &error));
    bad = encoded;
    bad[11] = 0;
    bad[12] = 0;
    EXPECT_FALSE(C::decode_tool_payload(bad, &decoded, &error));

    original.tool_id = "";
    EXPECT_FALSE(C::encode_tool_payload(original, &encoded, &error));
    original.tool_id = "tool";
    original.name.clear();
    EXPECT_FALSE(C::encode_tool_payload(original, &encoded, &error));
    original.name = std::string(C::Limits::kMaxToolNameSize + 1, 'n');
    EXPECT_FALSE(C::encode_tool_payload(original, &encoded, &error));
}

TEST(AgentRecordCodecTest, RecordEnvelopeRoundTripAndWireLayout) {
    const C::Record original =
        make_record(C::RecordType::kSession,
                    static_cast<std::uint8_t>(C::SessionStatus::kWaitingTool),
                    make_session_payload());
    std::string encoded;
    std::string error;
    ASSERT_TRUE(C::encode_record(original, &encoded, &error)) << error;
    EXPECT_EQ(encoded.size(),
              C::Limits::kRecordEnvelopeSize + original.payload.size());
    EXPECT_EQ(encoded.substr(0, 4), "MDAR");
    EXPECT_EQ(static_cast<std::uint8_t>(encoded[4]), 1);
    EXPECT_EQ(static_cast<std::uint8_t>(encoded[5]),
              static_cast<std::uint8_t>(C::RecordType::kSession));
    EXPECT_EQ(static_cast<std::uint8_t>(encoded[6]), original.status);
    EXPECT_EQ(static_cast<std::uint8_t>(encoded[7]), 0);
    expect_encoded_record_decodes(original);

    for (std::size_t length = 0; length < encoded.size(); ++length) {
        SCOPED_TRACE(length);
        C::Record decoded;
        EXPECT_FALSE(
            C::decode_record(encoded.substr(0, length), &decoded, &error));
    }
    C::Record decoded;
    EXPECT_FALSE(
        C::decode_record(encoded + std::string(1, 'x'), &decoded, &error));

    std::string bad = encoded;
    bad[5] = 9;
    EXPECT_FALSE(C::decode_record(bad, &decoded, &error));
    bad = encoded;
    bad[6] = 9;
    EXPECT_FALSE(C::decode_record(bad, &decoded, &error));
    bad = encoded;
    bad[7] = 1;
    EXPECT_FALSE(C::decode_record(bad, &decoded, &error));
    bad = encoded;
    bad[51] = 0;
    EXPECT_FALSE(C::decode_record(bad, &decoded, &error));
}

TEST(AgentRecordCodecTest, EveryRecordTypeRoundTrips) {
    expect_encoded_record_decodes(
        make_record(C::RecordType::kSession,
                    static_cast<std::uint8_t>(C::SessionStatus::kActive),
                    make_session_payload()));
    expect_encoded_record_decodes(
        make_record(C::RecordType::kTool,
                    static_cast<std::uint8_t>(C::ToolStatus::kRunning),
                    make_tool_payload()));
    expect_encoded_record_decodes(make_record(C::RecordType::kIdempotency, 0,
                                              make_idempotency_payload()));
    expect_encoded_record_decodes(make_record(
        C::RecordType::kGeneric, 0, std::string("opaque\0bytes", 12)));
}

TEST(AgentRecordCodecTest, RecordRejectsTypeSpecificInvalidFields) {
    std::string encoded;
    std::string error;
    C::Record record = make_record(
        C::RecordType::kSession,
        static_cast<std::uint8_t>(C::SessionStatus::kActive), "payload");
    record.reserved = 1;
    EXPECT_FALSE(C::encode_record(record, &encoded, &error));
    record.reserved = 0;
    record.status = 0;
    EXPECT_FALSE(C::encode_record(record, &encoded, &error));
    record.status = static_cast<std::uint8_t>(C::SessionStatus::kActive);

    C::Record idempotency =
        make_record(C::RecordType::kIdempotency, 0, "payload");
    idempotency.revision = 1;
    EXPECT_FALSE(C::encode_record(idempotency, &encoded, &error));
    idempotency.revision = 0;
    idempotency.last_fence = 1;
    EXPECT_FALSE(C::encode_record(idempotency, &encoded, &error));

    C::Record generic = make_record(C::RecordType::kGeneric, 0, "payload");
    generic.lease_expires_at_ms = 1;
    EXPECT_FALSE(C::encode_record(generic, &encoded, &error));
}

TEST(AgentRecordCodecTest, IdempotencyPayloadRoundTripAndNestedResponse) {
    C::IdempotencyPayload original;
    original.request_hash = Sha256::digest("canonical request");
    original.expires_at_ms = 1760000000999LL;
    original.canonical_request_len = 42;
    original.response = make_response("saved-response");
    std::string encoded;
    std::string error;
    ASSERT_TRUE(C::encode_idempotency_payload(original, &encoded, &error))
        << error;
    EXPECT_EQ(encoded.substr(0, 4), "MIDP");
    EXPECT_EQ(encoded.size(), C::Limits::kIdempotencyPayloadHeaderSize +
                                  original.response.size());

    C::IdempotencyPayload decoded;
    ASSERT_TRUE(C::decode_idempotency_payload(encoded, &decoded, &error))
        << error;
    EXPECT_EQ(decoded.request_hash, original.request_hash);
    EXPECT_EQ(decoded.expires_at_ms, original.expires_at_ms);
    EXPECT_EQ(decoded.canonical_request_len, original.canonical_request_len);
    EXPECT_EQ(decoded.response, original.response);

    for (std::size_t length = 0; length < encoded.size(); ++length) {
        SCOPED_TRACE(length);
        EXPECT_FALSE(C::decode_idempotency_payload(encoded.substr(0, length),
                                                   &decoded, &error));
    }
    EXPECT_FALSE(C::decode_idempotency_payload(encoded + std::string(1, 'x'),
                                               &decoded, &error));
    std::string bad = encoded;
    bad[4] = 2;
    EXPECT_FALSE(C::decode_idempotency_payload(bad, &decoded, &error));
    bad = encoded;
    bad[7] = 1;
    EXPECT_FALSE(C::decode_idempotency_payload(bad, &decoded, &error));
    bad = encoded;
    bad[52] = 0;
    bad[53] = 0;
    bad[54] = 0;
    bad[55] = 0;
    EXPECT_FALSE(C::decode_idempotency_payload(bad, &decoded, &error));
    bad = encoded;
    bad[C::Limits::kIdempotencyPayloadHeaderSize] = 'X';
    EXPECT_FALSE(C::decode_idempotency_payload(bad, &decoded, &error));

    original.response.clear();
    EXPECT_FALSE(C::encode_idempotency_payload(original, &encoded, &error));
    original.response = "not an MDOR";
    EXPECT_FALSE(C::encode_idempotency_payload(original, &encoded, &error));
    original.response = make_response();
    original.canonical_request_len = 0;
    EXPECT_FALSE(C::encode_idempotency_payload(original, &encoded, &error));
}

TEST(AgentRecordCodecTest, ResponseEnvelopeRoundTripAndCorruption) {
    C::Response original;
    original.status = static_cast<std::uint8_t>(C::ResponseStatus::kCreated);
    original.revision = 0x0102030405060708ULL;
    original.fence = 0x1112131415161718ULL;
    original.lsn = 0x2122232425262728ULL;
    original.payload.assign("view\0bytes", 10);
    std::string encoded;
    std::string error;
    ASSERT_TRUE(C::encode_response(original, &encoded, &error)) << error;
    EXPECT_EQ(encoded.size(),
              C::Limits::kResponseEnvelopeSize + original.payload.size());
    EXPECT_EQ(encoded.substr(0, 4), "MDOR");
    EXPECT_EQ(static_cast<std::uint8_t>(encoded[4]), 1);
    EXPECT_EQ(static_cast<std::uint8_t>(encoded[5]), original.status);
    EXPECT_EQ(static_cast<std::uint8_t>(encoded[6]), 0);
    EXPECT_EQ(static_cast<std::uint8_t>(encoded[7]), 0);

    C::Response decoded;
    ASSERT_TRUE(C::decode_response(encoded, &decoded, &error)) << error;
    EXPECT_EQ(decoded.status, original.status);
    EXPECT_EQ(decoded.revision, original.revision);
    EXPECT_EQ(decoded.fence, original.fence);
    EXPECT_EQ(decoded.lsn, original.lsn);
    EXPECT_EQ(decoded.payload, original.payload);

    for (std::size_t length = 0; length < encoded.size(); ++length) {
        SCOPED_TRACE(length);
        EXPECT_FALSE(
            C::decode_response(encoded.substr(0, length), &decoded, &error));
    }
    EXPECT_FALSE(
        C::decode_response(encoded + std::string(1, 'x'), &decoded, &error));
    std::string bad = encoded;
    bad[5] = 12;
    EXPECT_FALSE(C::decode_response(bad, &decoded, &error));
    bad = encoded;
    bad[6] = 1;
    EXPECT_FALSE(C::decode_response(bad, &decoded, &error));
    bad = encoded;
    bad[7] = 1;
    EXPECT_FALSE(C::decode_response(bad, &decoded, &error));
    bad = encoded;
    bad[35] = 0;
    EXPECT_FALSE(C::decode_response(bad, &decoded, &error));

    original.flags = 1;
    EXPECT_FALSE(C::encode_response(original, &encoded, &error));
    original.flags = 0;
    original.reserved = 1;
    EXPECT_FALSE(C::encode_response(original, &encoded, &error));
}

TEST(AgentRecordCodecTest, TypedRecordCrossChecksKeyAndPayload) {
    C::Record record =
        make_record(C::RecordType::kSession,
                    static_cast<std::uint8_t>(C::SessionStatus::kActive),
                    make_session_payload("typed"));
    std::string encoded;
    std::string error;
    ASSERT_TRUE(C::encode_record(record, &encoded, &error)) << error;
    std::string key;
    ASSERT_TRUE(C::session_key("session-1", &key, &error)) << error;

    C::Record decoded_record;
    C::SessionPayload decoded_session;
    ASSERT_TRUE(C::decode_typed_record(key, encoded, &decoded_record,
                                       &decoded_session, nullptr, nullptr,
                                       &error))
        << error;
    EXPECT_EQ(decoded_record.type, C::RecordType::kSession);
    EXPECT_EQ(decoded_session.state, "typed");

    C::Record tool_record =
        make_record(C::RecordType::kTool,
                    static_cast<std::uint8_t>(C::ToolStatus::kPending),
                    make_tool_payload("tool-1"));
    ASSERT_TRUE(C::encode_record(tool_record, &encoded, &error)) << error;
    ASSERT_TRUE(C::tool_key("session-1", "tool-1", &key, &error)) << error;
    C::ToolPayload decoded_tool;
    ASSERT_TRUE(C::decode_typed_record(key, encoded, nullptr, nullptr,
                                       &decoded_tool, nullptr, &error))
        << error;
    EXPECT_EQ(decoded_tool.tool_id, "tool-1");

    ASSERT_TRUE(C::tool_key("session-1", "tool-2", &key, &error)) << error;
    EXPECT_FALSE(C::decode_typed_record(key, encoded, nullptr, nullptr,
                                        &decoded_tool, nullptr, &error));

    C::Record idempotency =
        make_record(C::RecordType::kIdempotency, 0, make_idempotency_payload());
    ASSERT_TRUE(C::encode_record(idempotency, &encoded, &error)) << error;
    ASSERT_TRUE(C::idempotency_key("idem-1", &key, &error)) << error;
    C::IdempotencyPayload decoded_idempotency;
    ASSERT_TRUE(C::decode_typed_record(key, encoded, nullptr, nullptr, nullptr,
                                       &decoded_idempotency, &error))
        << error;
    EXPECT_FALSE(decoded_idempotency.response.empty());

    C::Record generic =
        make_record(C::RecordType::kGeneric, 0, std::string("opaque", 6));
    ASSERT_TRUE(C::encode_record(generic, &encoded, &error)) << error;
    ASSERT_TRUE(C::generic_key("record-1", &key, &error)) << error;
    ASSERT_TRUE(C::decode_typed_record(key, encoded, nullptr, nullptr, nullptr,
                                       nullptr, &error))
        << error;

    ASSERT_TRUE(C::session_key("session-1", &key, &error)) << error;
    EXPECT_FALSE(C::decode_typed_record(key, encoded, nullptr, nullptr, nullptr,
                                        nullptr, &error));
    EXPECT_FALSE(C::decode_typed_record(C::marker_key(), encoded, nullptr,
                                        nullptr, nullptr, nullptr, &error));

    idempotency =
        make_record(C::RecordType::kIdempotency, 0, make_idempotency_payload());
    idempotency.payload[C::Limits::kIdempotencyPayloadHeaderSize] = 'X';
    ASSERT_TRUE(C::encode_record(idempotency, &encoded, &error)) << error;
    ASSERT_TRUE(C::idempotency_key("idem-1", &key, &error)) << error;
    EXPECT_FALSE(C::decode_typed_record(key, encoded, nullptr, nullptr, nullptr,
                                        nullptr, &error));
}

TEST(AgentRecordCodecTest, CanonicalRequestIsDeterministicAndBinarySafe) {
    std::string encoded;
    std::string error;
    ASSERT_TRUE(C::encode_canonical_request("PING", {"abc"}, &encoded, &error))
        << error;
    ASSERT_EQ(encoded.size(), 12u);
    EXPECT_EQ(encoded.substr(0, 4), "PING");
    EXPECT_EQ(static_cast<std::uint8_t>(encoded[4]), 1);
    EXPECT_EQ(encoded.substr(5, 3), std::string("\0\0\0", 3));
    EXPECT_EQ(static_cast<std::uint8_t>(encoded[8]), 3);
    EXPECT_EQ(encoded.substr(9), "abc");

    std::string first;
    std::string second;
    const std::vector<std::string> args = {"", std::string("a\0b", 3), "z"};
    ASSERT_TRUE(
        C::encode_canonical_request("TOOL_CREATE", args, &first, &error))
        << error;
    ASSERT_TRUE(
        C::encode_canonical_request("TOOL_CREATE", args, &second, &error))
        << error;
    EXPECT_EQ(first, second);
    EXPECT_GT(first.size(), args[0].size() + args[1].size() + args[2].size());

    const std::vector<std::string> reordered = {args[2], args[1], args[0]};
    ASSERT_TRUE(
        C::encode_canonical_request("TOOL_CREATE", reordered, &second, &error))
        << error;
    EXPECT_NE(first, second);

    Sha256::Digest digest;
    std::uint32_t encoded_len = 0;
    ASSERT_TRUE(C::canonical_request_hash("TOOL_CREATE", args, &digest,
                                          &encoded_len, &error))
        << error;
    EXPECT_EQ(encoded_len, first.size());
    EXPECT_EQ(digest, Sha256::digest(first));
}

TEST(AgentRecordCodecTest, CanonicalRequestRejectsInvalidInputs) {
    std::string encoded;
    std::string error;
    EXPECT_FALSE(C::encode_canonical_request("", {}, &encoded, &error));
    EXPECT_FALSE(
        C::encode_canonical_request("BAD\nCOMMAND", {}, &encoded, &error));
    std::vector<std::string> too_many(256, "x");
    EXPECT_FALSE(
        C::encode_canonical_request("TOO_MANY", too_many, &encoded, &error));

    Sha256::Digest digest;
    EXPECT_FALSE(C::canonical_request_hash("", {}, &digest, nullptr, &error));
}

TEST(AgentRecordCodecTest, EnforcesRecordAndPayloadSizeLimits) {
    const std::size_t max_payload =
        C::Limits::kMaxRecordSize - C::Limits::kRecordEnvelopeSize;

    C::SessionPayload session;
    session.state.assign(max_payload - C::Limits::kSessionPayloadHeaderSize,
                         's');
    std::string encoded;
    std::string error;
    ASSERT_TRUE(C::encode_session_payload(session, &encoded, &error)) << error;
    EXPECT_EQ(encoded.size(), max_payload);
    session.state.push_back('x');
    EXPECT_FALSE(C::encode_session_payload(session, &encoded, &error));

    C::ToolPayload tool;
    tool.tool_id = "t";
    tool.name = "n";
    tool.request.assign(max_payload - C::Limits::kToolPayloadHeaderSize - 2,
                        'r');
    ASSERT_TRUE(C::encode_tool_payload(tool, &encoded, &error)) << error;
    EXPECT_EQ(encoded.size(), max_payload);
    tool.request.push_back('x');
    EXPECT_FALSE(C::encode_tool_payload(tool, &encoded, &error));

    C::Record record =
        make_record(C::RecordType::kGeneric, 0, std::string(max_payload, 'g'));
    ASSERT_TRUE(C::encode_record(record, &encoded, &error)) << error;
    EXPECT_EQ(encoded.size(), C::Limits::kMaxRecordSize);
    record.payload.push_back('x');
    EXPECT_FALSE(C::encode_record(record, &encoded, &error));
}

TEST(AgentRecordCodecTest, StatusPredicatesAndNames) {
    EXPECT_TRUE(C::valid_session_status(1));
    EXPECT_TRUE(C::valid_session_status(5));
    EXPECT_FALSE(C::valid_session_status(0));
    EXPECT_FALSE(C::valid_session_status(6));
    EXPECT_TRUE(C::valid_tool_status(1));
    EXPECT_TRUE(C::valid_tool_status(5));
    EXPECT_FALSE(C::valid_tool_status(0));
    EXPECT_FALSE(C::valid_tool_status(6));

    EXPECT_FALSE(C::session_terminal(1));
    EXPECT_FALSE(C::session_terminal(2));
    EXPECT_TRUE(C::session_terminal(3));
    EXPECT_TRUE(C::session_terminal(5));
    EXPECT_FALSE(C::tool_terminal(1));
    EXPECT_FALSE(C::tool_terminal(2));
    EXPECT_TRUE(C::tool_terminal(3));
    EXPECT_TRUE(C::tool_terminal(5));

    EXPECT_STREQ(agent_record_type_name(C::RecordType::kSession), "SESSION");
    EXPECT_STREQ(agent_session_status_name(2), "WAITING_TOOL");
    EXPECT_STREQ(agent_tool_status_name(2), "RUNNING");
    EXPECT_STREQ(agent_response_status_name(9), "IDEMPOTENCY_CONFLICT");
    EXPECT_STREQ(agent_record_type_name(static_cast<C::RecordType>(99)),
                 "UNKNOWN");
}

} // namespace minidb
