#pragma once

#include "../base/Sha256.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace minidb {

// Strict, dependency-free codec for the reserved Agent state namespace.
// Every decoder in this class is fail-closed: callers receive false on any
// malformed field, non-canonical length, trailing byte, unsupported version,
// or resource-limit violation.
class AgentRecordCodec {
  public:
    enum class RecordType : std::uint8_t {
        kSession = 1,
        kTool = 2,
        kIdempotency = 3,
        kGeneric = 4,
    };

    enum class SessionStatus : std::uint8_t {
        kActive = 1,
        kWaitingTool = 2,
        kCompleted = 3,
        kFailed = 4,
        kCancelled = 5,
    };

    enum class ToolStatus : std::uint8_t {
        kPending = 1,
        kRunning = 2,
        kSucceeded = 3,
        kFailed = 4,
        kCancelled = 5,
    };

    enum class ResponseStatus : std::uint8_t {
        kOk = 0,
        kCreated = 1,
        kNotFound = 2,
        kVersionMismatch = 3,
        kAlreadyExists = 4,
        kLeaseHeld = 5,
        kLeaseExpired = 6,
        kFenced = 7,
        kInvalidState = 8,
        kIdempotencyConflict = 9,
        kResourceLimit = 10,
        kCapacity = 11,
    };

    enum class KeyKind : std::uint8_t {
        kMarker,
        kSession,
        kTool,
        kIdempotency,
        kGeneric,
    };

    struct Limits {
        static constexpr std::size_t kReservedPrefixSize = 17;
        static constexpr std::size_t kMarkerSize = 8;
        static constexpr std::size_t kRecordEnvelopeSize = 52;
        static constexpr std::size_t kResponseEnvelopeSize = 36;
        static constexpr std::size_t kMaxRecordSize = std::size_t{64} * 1024;
        static constexpr std::size_t kMaxResponseSize =
            kMaxRecordSize + kResponseEnvelopeSize;
        static constexpr std::size_t kMaxNameSize = 128;
        static constexpr std::size_t kMaxToolNameSize = 256;
        static constexpr std::size_t kMaxIdempotencyKeySize = 128;
        static constexpr std::size_t kSessionPayloadHeaderSize = 20;
        static constexpr std::size_t kToolPayloadHeaderSize = 20;
        static constexpr std::size_t kIdempotencyPayloadHeaderSize = 56;
        static constexpr std::size_t kMaxSessionRecords = 1024;
        static constexpr std::size_t kMaxToolRecords = 8192;
        static constexpr std::size_t kMaxToolsPerSession = 128;
        static constexpr std::size_t kMaxGenericRecords = 16384;
        static constexpr std::size_t kMaxIdempotencyRecords = 8192;
        static constexpr std::uint64_t kMaxLogicalBytes =
            std::uint64_t{64} * 1024 * 1024;
        static constexpr std::int64_t kMinSessionTtlMs = 60 * 1000;
        static constexpr std::int64_t kMaxSessionTtlMs =
            std::int64_t{30} * 24 * 60 * 60 * 1000;
        static constexpr std::int64_t kMinLeaseTtlMs = 1000;
        static constexpr std::int64_t kMaxLeaseTtlMs = 60 * 60 * 1000;
        static constexpr std::int64_t kMinIdempotencyTtlMs = 60 * 1000;
        static constexpr std::int64_t kMaxIdempotencyTtlMs =
            std::int64_t{7} * 24 * 60 * 60 * 1000;
        static constexpr std::int64_t kDefaultIdempotencyTtlMs =
            std::int64_t{24} * 60 * 60 * 1000;
        static constexpr std::size_t kMaxGcCandidates = 64;
    };

    static constexpr std::string_view kReservedPrefix{
        "\x00minidb-agent-v1\x00", Limits::kReservedPrefixSize};
    static constexpr std::string_view kMarkerMagic = "MDAM";
    static constexpr std::string_view kRecordMagic = "MDAR";
    static constexpr std::string_view kResponseMagic = "MDOR";
    static constexpr std::uint16_t kFormatVersion = 1;
    static constexpr std::string_view kSessionPayloadMagic = "MSES";
    static constexpr std::string_view kToolPayloadMagic = "MTOL";
    static constexpr std::string_view kIdempotencyPayloadMagic = "MIDP";

    struct Record {
        RecordType type = RecordType::kGeneric;
        std::uint8_t status = 0;
        std::uint8_t reserved = 0;
        std::uint64_t revision = 0;
        std::uint64_t last_fence = 0;
        std::int64_t created_at_ms = 0;
        std::int64_t updated_at_ms = 0;
        std::int64_t lease_expires_at_ms = 0;
        std::string payload;
    };

    struct SessionPayload {
        std::int64_t session_expires_at_ms = 0;
        std::string state;
    };

    struct ToolPayload {
        std::string tool_id;
        std::string name;
        std::string request;
        std::string result;
    };

    struct IdempotencyPayload {
        Sha256::Digest request_hash{};
        std::int64_t expires_at_ms = 0;
        std::uint32_t canonical_request_len = 0;
        std::string response;
    };

    struct Response {
        std::uint8_t status = 0;
        std::uint8_t flags = 0;
        std::uint8_t reserved = 0;
        std::uint64_t revision = 0;
        std::uint64_t fence = 0;
        std::uint64_t lsn = 0;
        std::string payload;
    };

    struct KeyInfo {
        KeyKind kind = KeyKind::kMarker;
        std::string session_id;
        std::string tool_id;
        std::string name;
        std::string idempotency_key;
    };

    static bool is_reserved_key(std::string_view key) noexcept;
    static bool is_valid_name(std::string_view value) noexcept;
    static bool is_valid_idempotency_key(std::string_view value) noexcept;

    static std::string marker_key();
    static bool session_key(std::string_view session_id, std::string* out,
                            std::string* error = nullptr);
    static bool tool_key(std::string_view session_id, std::string_view tool_id,
                         std::string* out, std::string* error = nullptr);
    static bool idempotency_key(std::string_view idempotency_key,
                                std::string* out, std::string* error = nullptr);
    static bool generic_key(std::string_view record_name, std::string* out,
                            std::string* error = nullptr);
    static bool decode_key(std::string_view key, KeyInfo* out,
                           std::string* error = nullptr);

    static std::string encode_marker();
    static bool decode_marker(std::string_view value,
                              std::string* error = nullptr);

    static bool encode_record(const Record& record, std::string* out,
                              std::string* error = nullptr);
    static bool decode_record(std::string_view value, Record* out,
                              std::string* error = nullptr);

    static bool encode_session_payload(const SessionPayload& payload,
                                       std::string* out,
                                       std::string* error = nullptr);
    static bool decode_session_payload(std::string_view value,
                                       SessionPayload* out,
                                       std::string* error = nullptr);
    static bool encode_tool_payload(const ToolPayload& payload,
                                    std::string* out,
                                    std::string* error = nullptr);
    static bool decode_tool_payload(std::string_view value, ToolPayload* out,
                                    std::string* error = nullptr);
    static bool encode_idempotency_payload(const IdempotencyPayload& payload,
                                           std::string* out,
                                           std::string* error = nullptr);
    static bool decode_idempotency_payload(std::string_view value,
                                           IdempotencyPayload* out,
                                           std::string* error = nullptr);

    static bool encode_response(const Response& response, std::string* out,
                                std::string* error = nullptr);
    static bool decode_response(std::string_view value, Response* out,
                                std::string* error = nullptr);

    static bool encode_canonical_request(std::string_view subcommand,
                                         const std::vector<std::string>& args,
                                         std::string* out,
                                         std::string* error = nullptr);
    static bool canonical_request_hash(std::string_view subcommand,
                                       const std::vector<std::string>& args,
                                       Sha256::Digest* out,
                                       std::uint32_t* encoded_len = nullptr,
                                       std::string* error = nullptr);

    static bool decode_typed_record(std::string_view key,
                                    std::string_view value, Record* record,
                                    SessionPayload* session, ToolPayload* tool,
                                    IdempotencyPayload* idempotency,
                                    std::string* error = nullptr);

    static bool valid_session_status(std::uint8_t status) noexcept;
    static bool valid_tool_status(std::uint8_t status) noexcept;
    static bool session_terminal(std::uint8_t status) noexcept;
    static bool tool_terminal(std::uint8_t status) noexcept;
};

const char* agent_record_type_name(AgentRecordCodec::RecordType type) noexcept;
const char* agent_session_status_name(std::uint8_t status) noexcept;
const char* agent_tool_status_name(std::uint8_t status) noexcept;
const char* agent_response_status_name(std::uint8_t status) noexcept;

} // namespace minidb
