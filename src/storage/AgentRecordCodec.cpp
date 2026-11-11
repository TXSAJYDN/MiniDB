#include "AgentRecordCodec.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace minidb {
namespace {

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

void append_u16(std::string* out, std::uint16_t value) {
    out->push_back(static_cast<char>((value >> 8u) & 0xffu));
    out->push_back(static_cast<char>(value & 0xffu));
}

void append_u32(std::string* out, std::uint32_t value) {
    out->push_back(static_cast<char>((value >> 24u) & 0xffu));
    out->push_back(static_cast<char>((value >> 16u) & 0xffu));
    out->push_back(static_cast<char>((value >> 8u) & 0xffu));
    out->push_back(static_cast<char>(value & 0xffu));
}

void append_u64(std::string* out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out->push_back(static_cast<char>((value >> shift) & 0xffu));
    }
}

void append_i64(std::string* out, std::int64_t value) {
    append_u64(out, static_cast<std::uint64_t>(value));
}

void append_magic(std::string* out, std::string_view magic) {
    out->append(magic.data(), magic.size());
}

class Reader {
  public:
    explicit Reader(std::string_view data) : data_(data) {}

    bool u8(std::uint8_t* out) {
        if (remaining() < 1) {
            return false;
        }
        *out = static_cast<std::uint8_t>(data_[offset_++]);
        return true;
    }

    bool u16(std::uint16_t* out) {
        if (remaining() < 2) {
            return false;
        }
        const auto first = static_cast<std::uint8_t>(data_[offset_]);
        const auto second = static_cast<std::uint8_t>(data_[offset_ + 1]);
        *out = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(first) << 8u) |
            static_cast<std::uint16_t>(second));
        offset_ += 2;
        return true;
    }

    bool u32(std::uint32_t* out) {
        if (remaining() < 4) {
            return false;
        }
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            value = (value << 8u) |
                    static_cast<std::uint8_t>(data_[offset_ + index]);
        }
        *out = value;
        offset_ += 4;
        return true;
    }

    bool u64(std::uint64_t* out) {
        if (remaining() < 8) {
            return false;
        }
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < 8; ++index) {
            value = (value << 8u) |
                    static_cast<std::uint8_t>(data_[offset_ + index]);
        }
        *out = value;
        offset_ += 8;
        return true;
    }

    bool i64(std::int64_t* out) {
        std::uint64_t value = 0;
        if (!u64(&value)) {
            return false;
        }
        *out = static_cast<std::int64_t>(value);
        return true;
    }

    bool bytes(std::size_t size, std::string_view* out) {
        if (size > remaining()) {
            return false;
        }
        *out = data_.substr(offset_, size);
        offset_ += size;
        return true;
    }

    bool magic(std::string_view expected) {
        std::string_view actual;
        return bytes(expected.size(), &actual) && actual == expected;
    }

    std::size_t remaining() const noexcept {
        return data_.size() - offset_;
    }

  private:
    std::string_view data_;
    std::size_t offset_ = 0;
};

bool valid_record_type(AgentRecordCodec::RecordType type) noexcept {
    switch (type) {
    case AgentRecordCodec::RecordType::kSession:
    case AgentRecordCodec::RecordType::kTool:
    case AgentRecordCodec::RecordType::kIdempotency:
    case AgentRecordCodec::RecordType::kGeneric:
        return true;
    }
    return false;
}

bool valid_response_status(std::uint8_t status) noexcept {
    return status <= static_cast<std::uint8_t>(
                         AgentRecordCodec::ResponseStatus::kCapacity);
}

bool valid_record_status(AgentRecordCodec::RecordType type,
                         std::uint8_t status) noexcept {
    switch (type) {
    case AgentRecordCodec::RecordType::kSession:
        return AgentRecordCodec::valid_session_status(status);
    case AgentRecordCodec::RecordType::kTool:
        return AgentRecordCodec::valid_tool_status(status);
    case AgentRecordCodec::RecordType::kIdempotency:
    case AgentRecordCodec::RecordType::kGeneric:
        return status == 0;
    }
    return false;
}

} // namespace

bool AgentRecordCodec::is_reserved_key(std::string_view key) noexcept {
    return key.size() >= kReservedPrefix.size() &&
           key.substr(0, kReservedPrefix.size()) == kReservedPrefix;
}

bool AgentRecordCodec::is_valid_name(std::string_view value) noexcept {
    return !value.empty() && value.size() <= Limits::kMaxNameSize &&
           value.find('\0') == std::string_view::npos;
}

bool AgentRecordCodec::is_valid_idempotency_key(
    std::string_view value) noexcept {
    return !value.empty() && value.size() <= Limits::kMaxIdempotencyKeySize;
}

std::string AgentRecordCodec::marker_key() {
    std::string out(kReservedPrefix);
    out.push_back('m');
    return out;
}
bool AgentRecordCodec::session_key(std::string_view session_id,
                                   std::string* out, std::string* error) {
    if (!is_valid_name(session_id)) {
        set_error(error, "session_id must be 1..128 bytes without NUL");
        return false;
    }
    std::string encoded(kReservedPrefix);
    encoded.append("s\0", 2);
    encoded.append(session_id.data(), session_id.size());
    *out = std::move(encoded);
    return true;
}

bool AgentRecordCodec::tool_key(std::string_view session_id,
                                std::string_view tool_id, std::string* out,
                                std::string* error) {
    if (!is_valid_name(session_id)) {
        set_error(error, "session_id must be 1..128 bytes without NUL");
        return false;
    }
    if (!is_valid_name(tool_id)) {
        set_error(error, "tool_id must be 1..128 bytes without NUL");
        return false;
    }
    std::string encoded(kReservedPrefix);
    encoded.append("t\0", 2);
    encoded.append(session_id.data(), session_id.size());
    encoded.push_back('\0');
    encoded.append(tool_id.data(), tool_id.size());
    *out = std::move(encoded);
    return true;
}

bool AgentRecordCodec::idempotency_key(std::string_view key, std::string* out,
                                       std::string* error) {
    if (!is_valid_idempotency_key(key)) {
        set_error(error, "idempotency key must be 1..128 bytes");
        return false;
    }
    std::string encoded(kReservedPrefix);
    encoded.append("i\0", 2);
    encoded.append(key.data(), key.size());
    *out = std::move(encoded);
    return true;
}

bool AgentRecordCodec::generic_key(std::string_view record_name,
                                   std::string* out, std::string* error) {
    if (!is_valid_name(record_name)) {
        set_error(error, "record_name must be 1..128 bytes without NUL");
        return false;
    }
    std::string encoded(kReservedPrefix);
    encoded.append("r\0", 2);
    encoded.append(record_name.data(), record_name.size());
    *out = std::move(encoded);
    return true;
}

bool AgentRecordCodec::decode_key(std::string_view key, KeyInfo* out,
                                  std::string* error) {
    if (!is_reserved_key(key)) {
        set_error(error, "key is outside the reserved Agent namespace");
        return false;
    }
    if (key.size() > 1024) {
        set_error(error, "Agent key exceeds the KV key limit");
        return false;
    }
    const std::string_view suffix = key.substr(kReservedPrefix.size());
    if (suffix == "m") {
        if (out != nullptr) {
            *out = KeyInfo{};
            out->kind = KeyKind::kMarker;
        }
        return true;
    }
    if (suffix.size() < 3 || suffix[1] != '\0') {
        set_error(error, "Agent key has an invalid type prefix");
        return false;
    }

    const char kind = suffix[0];
    const std::string_view payload = suffix.substr(2);
    KeyInfo decoded;
    switch (kind) {
    case 's':
        if (!is_valid_name(payload)) {
            set_error(error, "session key has an invalid session_id");
            return false;
        }
        decoded.kind = KeyKind::kSession;
        decoded.session_id.assign(payload.data(), payload.size());
        break;
    case 't': {
        const std::size_t separator = payload.find('\0');
        if (separator == std::string_view::npos) {
            set_error(error, "tool key is missing its tool_id separator");
            return false;
        }
        const std::string_view session_id = payload.substr(0, separator);
        const std::string_view tool_id = payload.substr(separator + 1);
        if (!is_valid_name(session_id) || !is_valid_name(tool_id)) {
            set_error(error, "tool key has an invalid session_id or tool_id");
            return false;
        }
        decoded.kind = KeyKind::kTool;
        decoded.session_id.assign(session_id.data(), session_id.size());
        decoded.tool_id.assign(tool_id.data(), tool_id.size());
        break;
    }
    case 'i':
        if (!is_valid_idempotency_key(payload)) {
            set_error(error, "idempotency key has an invalid key payload");
            return false;
        }
        decoded.kind = KeyKind::kIdempotency;
        decoded.idempotency_key.assign(payload.data(), payload.size());
        break;
    case 'r':
        if (!is_valid_name(payload)) {
            set_error(error, "generic key has an invalid record_name");
            return false;
        }
        decoded.kind = KeyKind::kGeneric;
        decoded.name.assign(payload.data(), payload.size());
        break;
    default:
        set_error(error, "Agent key has an unknown type prefix");
        return false;
    }

    if (out != nullptr) {
        *out = std::move(decoded);
    }
    return true;
}

std::string AgentRecordCodec::encode_marker() {
    std::string out;
    out.reserve(Limits::kMarkerSize);
    append_magic(&out, kMarkerMagic);
    append_u16(&out, kFormatVersion);
    append_u16(&out, 0);
    return out;
}

bool AgentRecordCodec::decode_marker(std::string_view value,
                                     std::string* error) {
    if (value.size() != Limits::kMarkerSize) {
        set_error(error, "Agent marker must be exactly 8 bytes");
        return false;
    }
    Reader reader(value);
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    if (!reader.magic(kMarkerMagic) || !reader.u16(&version) ||
        !reader.u16(&reserved)) {
        set_error(error, "Agent marker is truncated");
        return false;
    }
    if (version != kFormatVersion) {
        set_error(error, "Agent marker version is unsupported");
        return false;
    }
    if (reserved != 0) {
        set_error(error, "Agent marker reserved field is non-zero");
        return false;
    }
    if (reader.remaining() != 0) {
        set_error(error, "Agent marker has trailing bytes");
        return false;
    }
    return true;
}
bool AgentRecordCodec::encode_record(const Record& record, std::string* out,
                                     std::string* error) {
    if (!valid_record_type(record.type)) {
        set_error(error, "Agent record type is unknown");
        return false;
    }
    if (!valid_record_status(record.type, record.status)) {
        set_error(error, "Agent record status is invalid for its type");
        return false;
    }
    if (record.reserved != 0) {
        set_error(error, "Agent record reserved byte is non-zero");
        return false;
    }
    if (record.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "Agent record payload length overflows u32");
        return false;
    }
    if (record.type == RecordType::kIdempotency ||
        record.type == RecordType::kGeneric) {
        if (record.last_fence != 0 || record.lease_expires_at_ms != 0) {
            set_error(error,
                      "idempotency/generic record has session lease fields");
            return false;
        }
    }
    if (record.type == RecordType::kIdempotency && record.revision != 0) {
        set_error(error, "idempotency record revision must be zero");
        return false;
    }
    const std::size_t total_size =
        Limits::kRecordEnvelopeSize + record.payload.size();
    if (total_size > Limits::kMaxRecordSize) {
        set_error(error, "Agent record exceeds the 64 KiB limit");
        return false;
    }

    std::string encoded;
    encoded.reserve(total_size);
    append_magic(&encoded, kRecordMagic);
    encoded.push_back(static_cast<char>(kFormatVersion));
    encoded.push_back(static_cast<char>(record.type));
    encoded.push_back(static_cast<char>(record.status));
    encoded.push_back('\0');
    append_u64(&encoded, record.revision);
    append_u64(&encoded, record.last_fence);
    append_i64(&encoded, record.created_at_ms);
    append_i64(&encoded, record.updated_at_ms);
    append_i64(&encoded, record.lease_expires_at_ms);
    append_u32(&encoded, static_cast<std::uint32_t>(record.payload.size()));
    encoded.append(record.payload.data(), record.payload.size());
    *out = std::move(encoded);
    return true;
}

bool AgentRecordCodec::decode_record(std::string_view value, Record* out,
                                     std::string* error) {
    if (value.size() < Limits::kRecordEnvelopeSize) {
        set_error(error, "Agent record is shorter than its fixed envelope");
        return false;
    }
    if (value.size() > Limits::kMaxRecordSize) {
        set_error(error, "Agent record exceeds the 64 KiB limit");
        return false;
    }

    Reader reader(value);
    std::uint8_t version = 0;
    std::uint8_t type = 0;
    std::uint8_t status = 0;
    std::uint8_t reserved = 0;
    std::uint32_t payload_len = 0;
    Record decoded;
    if (!reader.magic(kRecordMagic)) {
        set_error(error, "Agent record magic mismatch");
        return false;
    }
    if (!reader.u8(&version) || !reader.u8(&type) || !reader.u8(&status) ||
        !reader.u8(&reserved) || !reader.u64(&decoded.revision) ||
        !reader.u64(&decoded.last_fence) ||
        !reader.i64(&decoded.created_at_ms) ||
        !reader.i64(&decoded.updated_at_ms) ||
        !reader.i64(&decoded.lease_expires_at_ms) ||
        !reader.u32(&payload_len)) {
        set_error(error, "Agent record envelope is truncated");
        return false;
    }
    if (version != kFormatVersion) {
        set_error(error, "Agent record version is unsupported");
        return false;
    }
    decoded.type = static_cast<RecordType>(type);
    if (!valid_record_type(decoded.type)) {
        set_error(error, "Agent record type is unknown");
        return false;
    }
    if (reserved != 0) {
        set_error(error, "Agent record reserved byte is non-zero");
        return false;
    }
    if (!valid_record_status(decoded.type, status)) {
        set_error(error, "Agent record status is invalid for its type");
        return false;
    }
    if (payload_len != reader.remaining()) {
        set_error(error,
                  "Agent record payload length does not match its value");
        return false;
    }
    std::string_view payload;
    if (!reader.bytes(payload_len, &payload)) {
        set_error(error, "Agent record payload is truncated");
        return false;
    }
    if (decoded.type == RecordType::kIdempotency ||
        decoded.type == RecordType::kGeneric) {
        if (decoded.last_fence != 0 || decoded.lease_expires_at_ms != 0) {
            set_error(error,
                      "idempotency/generic record has session lease fields");
            return false;
        }
    }
    if (decoded.type == RecordType::kIdempotency && decoded.revision != 0) {
        set_error(error, "idempotency record revision must be zero");
        return false;
    }
    decoded.status = status;
    decoded.reserved = reserved;
    decoded.payload.assign(payload.data(), payload.size());
    *out = std::move(decoded);
    return true;
}

bool AgentRecordCodec::encode_session_payload(const SessionPayload& payload,
                                              std::string* out,
                                              std::string* error) {
    if (payload.state.size() > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "session state length overflows u32");
        return false;
    }
    const std::size_t total_size =
        Limits::kSessionPayloadHeaderSize + payload.state.size();
    if (total_size > Limits::kMaxRecordSize - Limits::kRecordEnvelopeSize) {
        set_error(error, "session payload exceeds the record size budget");
        return false;
    }

    std::string encoded;
    encoded.reserve(total_size);
    append_magic(&encoded, kSessionPayloadMagic);
    append_u16(&encoded, kFormatVersion);
    append_u16(&encoded, 0);
    append_i64(&encoded, payload.session_expires_at_ms);
    append_u32(&encoded, static_cast<std::uint32_t>(payload.state.size()));
    encoded.append(payload.state.data(), payload.state.size());
    *out = std::move(encoded);
    return true;
}

bool AgentRecordCodec::decode_session_payload(std::string_view value,
                                              SessionPayload* out,
                                              std::string* error) {
    if (value.size() < Limits::kSessionPayloadHeaderSize) {
        set_error(error, "session payload is shorter than its fixed header");
        return false;
    }
    if (value.size() > Limits::kMaxRecordSize - Limits::kRecordEnvelopeSize) {
        set_error(error, "session payload exceeds the record size budget");
        return false;
    }

    Reader reader(value);
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint32_t state_len = 0;
    SessionPayload decoded;
    if (!reader.magic(kSessionPayloadMagic)) {
        set_error(error, "session payload magic mismatch");
        return false;
    }
    if (!reader.u16(&version) || !reader.u16(&reserved) ||
        !reader.i64(&decoded.session_expires_at_ms) ||
        !reader.u32(&state_len)) {
        set_error(error, "session payload header is truncated");
        return false;
    }
    if (version != kFormatVersion) {
        set_error(error, "session payload version is unsupported");
        return false;
    }
    if (reserved != 0) {
        set_error(error, "session payload reserved field is non-zero");
        return false;
    }
    if (state_len != reader.remaining()) {
        set_error(error, "session state length does not match its payload");
        return false;
    }
    std::string_view state;
    if (!reader.bytes(state_len, &state)) {
        set_error(error, "session state is truncated");
        return false;
    }
    decoded.state.assign(state.data(), state.size());
    *out = std::move(decoded);
    return true;
}

bool AgentRecordCodec::encode_tool_payload(const ToolPayload& payload,
                                           std::string* out,
                                           std::string* error) {
    if (!is_valid_name(payload.tool_id)) {
        set_error(error, "tool_id must be 1..128 bytes without NUL");
        return false;
    }
    if (payload.name.empty() ||
        payload.name.size() > Limits::kMaxToolNameSize) {
        set_error(error, "tool name must be 1..256 bytes");
        return false;
    }
    if (payload.tool_id.size() > std::numeric_limits<std::uint16_t>::max() ||
        payload.name.size() > std::numeric_limits<std::uint16_t>::max()) {
        set_error(error, "tool_id or name length overflows u16");
        return false;
    }
    if (payload.request.size() > std::numeric_limits<std::uint32_t>::max() ||
        payload.result.size() > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "tool request or result length overflows u32");
        return false;
    }
    const std::size_t body_size = payload.tool_id.size() + payload.name.size() +
                                  payload.request.size() +
                                  payload.result.size();
    const std::size_t total_size = Limits::kToolPayloadHeaderSize + body_size;
    if (total_size < body_size ||
        total_size > Limits::kMaxRecordSize - Limits::kRecordEnvelopeSize) {
        set_error(error, "tool payload exceeds the record size budget");
        return false;
    }

    std::string encoded;
    encoded.reserve(total_size);
    append_magic(&encoded, kToolPayloadMagic);
    append_u16(&encoded, kFormatVersion);
    append_u16(&encoded, 0);
    append_u16(&encoded, static_cast<std::uint16_t>(payload.tool_id.size()));
    append_u16(&encoded, static_cast<std::uint16_t>(payload.name.size()));
    append_u32(&encoded, static_cast<std::uint32_t>(payload.request.size()));
    append_u32(&encoded, static_cast<std::uint32_t>(payload.result.size()));
    encoded.append(payload.tool_id.data(), payload.tool_id.size());
    encoded.append(payload.name.data(), payload.name.size());
    encoded.append(payload.request.data(), payload.request.size());
    encoded.append(payload.result.data(), payload.result.size());
    *out = std::move(encoded);
    return true;
}

bool AgentRecordCodec::decode_tool_payload(std::string_view value,
                                           ToolPayload* out,
                                           std::string* error) {
    if (value.size() < Limits::kToolPayloadHeaderSize) {
        set_error(error, "tool payload is shorter than its fixed header");
        return false;
    }
    if (value.size() > Limits::kMaxRecordSize - Limits::kRecordEnvelopeSize) {
        set_error(error, "tool payload exceeds the record size budget");
        return false;
    }

    Reader reader(value);
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint16_t tool_id_len = 0;
    std::uint16_t name_len = 0;
    std::uint32_t request_len = 0;
    std::uint32_t result_len = 0;
    if (!reader.magic(kToolPayloadMagic) || !reader.u16(&version) ||
        !reader.u16(&reserved) || !reader.u16(&tool_id_len) ||
        !reader.u16(&name_len) || !reader.u32(&request_len) ||
        !reader.u32(&result_len)) {
        set_error(error, "tool payload header is truncated");
        return false;
    }
    if (version != kFormatVersion) {
        set_error(error, "tool payload version is unsupported");
        return false;
    }
    if (reserved != 0) {
        set_error(error, "tool payload reserved field is non-zero");
        return false;
    }
    const std::size_t body_size = static_cast<std::size_t>(tool_id_len) +
                                  name_len + request_len + result_len;
    if (body_size != reader.remaining()) {
        set_error(error, "tool payload lengths do not match the value");
        return false;
    }

    std::string_view tool_id;
    std::string_view name;
    std::string_view request;
    std::string_view result;
    if (!reader.bytes(tool_id_len, &tool_id) ||
        !reader.bytes(name_len, &name) ||
        !reader.bytes(request_len, &request) ||
        !reader.bytes(result_len, &result)) {
        set_error(error, "tool payload body is truncated");
        return false;
    }
    if (!is_valid_name(tool_id)) {
        set_error(error, "tool payload has an invalid tool_id");
        return false;
    }
    if (name.empty() || name.size() > Limits::kMaxToolNameSize) {
        set_error(error, "tool payload has an invalid tool name");
        return false;
    }

    ToolPayload decoded;
    decoded.tool_id.assign(tool_id.data(), tool_id.size());
    decoded.name.assign(name.data(), name.size());
    decoded.request.assign(request.data(), request.size());
    decoded.result.assign(result.data(), result.size());
    *out = std::move(decoded);
    return true;
}

bool AgentRecordCodec::encode_idempotency_payload(
    const IdempotencyPayload& payload, std::string* out, std::string* error) {
    if (payload.canonical_request_len == 0) {
        set_error(error,
                  "idempotency canonical request length must be non-zero");
        return false;
    }
    if (payload.response.size() > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "idempotency response length overflows u32");
        return false;
    }
    const std::size_t total_size =
        Limits::kIdempotencyPayloadHeaderSize + payload.response.size();
    if (total_size < payload.response.size() ||
        total_size > Limits::kMaxRecordSize - Limits::kRecordEnvelopeSize) {
        set_error(error, "idempotency payload exceeds the record size budget");
        return false;
    }
    Response response;
    if (!decode_response(payload.response, &response, error)) {
        return false;
    }

    std::string encoded;
    encoded.reserve(total_size);
    append_magic(&encoded, kIdempotencyPayloadMagic);
    append_u16(&encoded, kFormatVersion);
    append_u16(&encoded, 0);
    encoded.append(reinterpret_cast<const char*>(payload.request_hash.data()),
                   payload.request_hash.size());
    append_i64(&encoded, payload.expires_at_ms);
    append_u32(&encoded, payload.canonical_request_len);
    append_u32(&encoded, static_cast<std::uint32_t>(payload.response.size()));
    encoded.append(payload.response.data(), payload.response.size());
    *out = std::move(encoded);
    return true;
}

bool AgentRecordCodec::decode_idempotency_payload(std::string_view value,
                                                  IdempotencyPayload* out,
                                                  std::string* error) {
    if (value.size() < Limits::kIdempotencyPayloadHeaderSize) {
        set_error(error,
                  "idempotency payload is shorter than its fixed header");
        return false;
    }
    if (value.size() > Limits::kMaxRecordSize - Limits::kRecordEnvelopeSize) {
        set_error(error, "idempotency payload exceeds the record size budget");
        return false;
    }

    Reader reader(value);
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::string_view hash;
    std::int64_t expires_at_ms = 0;
    std::uint32_t canonical_request_len = 0;
    std::uint32_t response_len = 0;
    if (!reader.magic(kIdempotencyPayloadMagic) || !reader.u16(&version) ||
        !reader.u16(&reserved) || !reader.bytes(32, &hash) ||
        !reader.i64(&expires_at_ms) || !reader.u32(&canonical_request_len) ||
        !reader.u32(&response_len)) {
        set_error(error, "idempotency payload header is truncated");
        return false;
    }
    if (version != kFormatVersion) {
        set_error(error, "idempotency payload version is unsupported");
        return false;
    }
    if (reserved != 0) {
        set_error(error, "idempotency payload reserved field is non-zero");
        return false;
    }
    if (canonical_request_len == 0) {
        set_error(error,
                  "idempotency canonical request length must be non-zero");
        return false;
    }
    if (response_len != reader.remaining()) {
        set_error(error,
                  "idempotency response length does not match its payload");
        return false;
    }
    std::string_view response;
    if (!reader.bytes(response_len, &response)) {
        set_error(error, "idempotency response is truncated");
        return false;
    }

    IdempotencyPayload decoded;
    std::memcpy(decoded.request_hash.data(), hash.data(), hash.size());
    decoded.expires_at_ms = expires_at_ms;
    decoded.canonical_request_len = canonical_request_len;
    decoded.response.assign(response.data(), response.size());
    Response decoded_response;
    if (!decode_response(decoded.response, &decoded_response, error)) {
        return false;
    }
    *out = std::move(decoded);
    return true;
}

bool AgentRecordCodec::encode_response(const Response& response,
                                       std::string* out, std::string* error) {
    if (!valid_response_status(response.status)) {
        set_error(error, "MDOR response status is unknown");
        return false;
    }
    if (response.flags != 0) {
        set_error(error, "MDOR flags must be zero");
        return false;
    }
    if (response.reserved != 0) {
        set_error(error, "MDOR reserved byte is non-zero");
        return false;
    }
    if (response.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "MDOR payload length overflows u32");
        return false;
    }
    const std::size_t total_size =
        Limits::kResponseEnvelopeSize + response.payload.size();
    if (total_size < response.payload.size() ||
        total_size > Limits::kMaxResponseSize) {
        set_error(error, "MDOR response exceeds the size limit");
        return false;
    }

    std::string encoded;
    encoded.reserve(total_size);
    append_magic(&encoded, kResponseMagic);
    encoded.push_back(static_cast<char>(kFormatVersion));
    encoded.push_back(static_cast<char>(response.status));
    encoded.push_back(static_cast<char>(response.flags));
    encoded.push_back(static_cast<char>(response.reserved));
    append_u64(&encoded, response.revision);
    append_u64(&encoded, response.fence);
    append_u64(&encoded, response.lsn);
    append_u32(&encoded, static_cast<std::uint32_t>(response.payload.size()));
    encoded.append(response.payload.data(), response.payload.size());
    *out = std::move(encoded);
    return true;
}

bool AgentRecordCodec::decode_response(std::string_view value, Response* out,
                                       std::string* error) {
    if (value.size() < Limits::kResponseEnvelopeSize) {
        set_error(error, "MDOR response is shorter than its fixed envelope");
        return false;
    }
    if (value.size() > Limits::kMaxResponseSize) {
        set_error(error, "MDOR response exceeds the size limit");
        return false;
    }

    Reader reader(value);
    std::uint8_t version = 0;
    std::uint8_t status = 0;
    std::uint8_t flags = 0;
    std::uint8_t reserved = 0;
    std::uint32_t payload_len = 0;
    Response decoded;
    if (!reader.magic(kResponseMagic) || !reader.u8(&version) ||
        !reader.u8(&status) || !reader.u8(&flags) || !reader.u8(&reserved) ||
        !reader.u64(&decoded.revision) || !reader.u64(&decoded.fence) ||
        !reader.u64(&decoded.lsn) || !reader.u32(&payload_len)) {
        set_error(error, "MDOR response header is truncated");
        return false;
    }
    if (version != kFormatVersion) {
        set_error(error, "MDOR response version is unsupported");
        return false;
    }
    if (!valid_response_status(status)) {
        set_error(error, "MDOR response status is unknown");
        return false;
    }
    if (flags != 0) {
        set_error(error, "MDOR flags must be zero");
        return false;
    }
    if (reserved != 0) {
        set_error(error, "MDOR reserved byte is non-zero");
        return false;
    }
    if (payload_len != reader.remaining()) {
        set_error(error, "MDOR payload length does not match its value");
        return false;
    }
    std::string_view payload;
    if (!reader.bytes(payload_len, &payload)) {
        set_error(error, "MDOR payload is truncated");
        return false;
    }
    decoded.status = status;
    decoded.flags = flags;
    decoded.reserved = reserved;
    decoded.payload.assign(payload.data(), payload.size());
    *out = std::move(decoded);
    return true;
}

bool AgentRecordCodec::encode_canonical_request(
    std::string_view subcommand, const std::vector<std::string>& args,
    std::string* out, std::string* error) {
    if (subcommand.empty()) {
        set_error(error, "canonical request subcommand must be non-empty");
        return false;
    }
    for (const char byte : subcommand) {
        const auto value = static_cast<unsigned char>(byte);
        if (value < 0x21u || value > 0x7eu) {
            set_error(error,
                      "canonical request subcommand must be printable ASCII");
            return false;
        }
    }
    if (args.size() > std::numeric_limits<std::uint8_t>::max()) {
        set_error(error, "canonical request has too many arguments");
        return false;
    }

    std::uint64_t total_size = subcommand.size() + 1u;
    for (const std::string& arg : args) {
        if (arg.size() > std::numeric_limits<std::uint32_t>::max()) {
            set_error(error, "canonical request argument length overflows u32");
            return false;
        }
        total_size += 4u + arg.size();
        if (total_size > std::numeric_limits<std::uint32_t>::max()) {
            set_error(error, "canonical request length overflows u32");
            return false;
        }
    }

    std::string encoded;
    encoded.reserve(static_cast<std::size_t>(total_size));
    encoded.append(subcommand.data(), subcommand.size());
    encoded.push_back(static_cast<char>(args.size()));
    for (const std::string& arg : args) {
        append_u32(&encoded, static_cast<std::uint32_t>(arg.size()));
        encoded.append(arg.data(), arg.size());
    }
    *out = std::move(encoded);
    return true;
}

bool AgentRecordCodec::canonical_request_hash(
    std::string_view subcommand, const std::vector<std::string>& args,
    Sha256::Digest* out, std::uint32_t* encoded_len, std::string* error) {
    std::string canonical;
    if (!encode_canonical_request(subcommand, args, &canonical, error)) {
        return false;
    }
    *out = Sha256::digest(canonical);
    if (encoded_len != nullptr) {
        *encoded_len = static_cast<std::uint32_t>(canonical.size());
    }
    return true;
}

bool AgentRecordCodec::decode_typed_record(
    std::string_view key, std::string_view value, Record* record,
    SessionPayload* session, ToolPayload* tool, IdempotencyPayload* idempotency,
    std::string* error) {
    KeyInfo key_info;
    if (!decode_key(key, &key_info, error)) {
        return false;
    }
    if (key_info.kind == KeyKind::kMarker) {
        set_error(error, "marker key does not carry an MDAR record");
        return false;
    }

    Record decoded_record;
    if (!decode_record(value, &decoded_record, error)) {
        return false;
    }
    SessionPayload decoded_session;
    ToolPayload decoded_tool;
    IdempotencyPayload decoded_idempotency;
    switch (key_info.kind) {
    case KeyKind::kMarker:
        set_error(error, "marker key does not carry an MDAR record");
        return false;
    case KeyKind::kSession:
        if (decoded_record.type != RecordType::kSession) {
            set_error(error, "session key carries a non-session record");
            return false;
        }
        if (!decode_session_payload(decoded_record.payload, &decoded_session,
                                    error)) {
            return false;
        }
        break;
    case KeyKind::kTool:
        if (decoded_record.type != RecordType::kTool) {
            set_error(error, "tool key carries a non-tool record");
            return false;
        }
        if (!decode_tool_payload(decoded_record.payload, &decoded_tool,
                                 error)) {
            return false;
        }
        if (decoded_tool.tool_id != key_info.tool_id) {
            set_error(error, "tool payload tool_id does not match its key");
            return false;
        }
        break;
    case KeyKind::kIdempotency: {
        if (decoded_record.type != RecordType::kIdempotency) {
            set_error(error,
                      "idempotency key carries a non-idempotency record");
            return false;
        }
        if (!decode_idempotency_payload(decoded_record.payload,
                                        &decoded_idempotency, error)) {
            return false;
        }
        Response response;
        if (!decode_response(decoded_idempotency.response, &response, error)) {
            return false;
        }
        break;
    }
    case KeyKind::kGeneric:
        if (decoded_record.type != RecordType::kGeneric) {
            set_error(error, "generic key carries a non-generic record");
            return false;
        }
        break;
    }

    if (record != nullptr) {
        *record = std::move(decoded_record);
    }
    if (session != nullptr) {
        *session = std::move(decoded_session);
    }
    if (tool != nullptr) {
        *tool = std::move(decoded_tool);
    }
    if (idempotency != nullptr) {
        *idempotency = std::move(decoded_idempotency);
    }
    return true;
}

bool AgentRecordCodec::valid_session_status(std::uint8_t status) noexcept {
    return status >= static_cast<std::uint8_t>(SessionStatus::kActive) &&
           status <= static_cast<std::uint8_t>(SessionStatus::kCancelled);
}

bool AgentRecordCodec::valid_tool_status(std::uint8_t status) noexcept {
    return status >= static_cast<std::uint8_t>(ToolStatus::kPending) &&
           status <= static_cast<std::uint8_t>(ToolStatus::kCancelled);
}

bool AgentRecordCodec::session_terminal(std::uint8_t status) noexcept {
    return status >= static_cast<std::uint8_t>(SessionStatus::kCompleted) &&
           status <= static_cast<std::uint8_t>(SessionStatus::kCancelled);
}

bool AgentRecordCodec::tool_terminal(std::uint8_t status) noexcept {
    return status >= static_cast<std::uint8_t>(ToolStatus::kSucceeded) &&
           status <= static_cast<std::uint8_t>(ToolStatus::kCancelled);
}

const char* agent_record_type_name(AgentRecordCodec::RecordType type) noexcept {
    switch (type) {
    case AgentRecordCodec::RecordType::kSession:
        return "SESSION";
    case AgentRecordCodec::RecordType::kTool:
        return "TOOL";
    case AgentRecordCodec::RecordType::kIdempotency:
        return "IDEMPOTENCY";
    case AgentRecordCodec::RecordType::kGeneric:
        return "GENERIC";
    }
    return "UNKNOWN";
}

const char* agent_session_status_name(std::uint8_t status) noexcept {
    switch (static_cast<AgentRecordCodec::SessionStatus>(status)) {
    case AgentRecordCodec::SessionStatus::kActive:
        return "ACTIVE";
    case AgentRecordCodec::SessionStatus::kWaitingTool:
        return "WAITING_TOOL";
    case AgentRecordCodec::SessionStatus::kCompleted:
        return "COMPLETED";
    case AgentRecordCodec::SessionStatus::kFailed:
        return "FAILED";
    case AgentRecordCodec::SessionStatus::kCancelled:
        return "CANCELLED";
    }
    return "UNKNOWN";
}

const char* agent_tool_status_name(std::uint8_t status) noexcept {
    switch (static_cast<AgentRecordCodec::ToolStatus>(status)) {
    case AgentRecordCodec::ToolStatus::kPending:
        return "PENDING";
    case AgentRecordCodec::ToolStatus::kRunning:
        return "RUNNING";
    case AgentRecordCodec::ToolStatus::kSucceeded:
        return "SUCCEEDED";
    case AgentRecordCodec::ToolStatus::kFailed:
        return "FAILED";
    case AgentRecordCodec::ToolStatus::kCancelled:
        return "CANCELLED";
    }
    return "UNKNOWN";
}

const char* agent_response_status_name(std::uint8_t status) noexcept {
    switch (static_cast<AgentRecordCodec::ResponseStatus>(status)) {
    case AgentRecordCodec::ResponseStatus::kOk:
        return "OK";
    case AgentRecordCodec::ResponseStatus::kCreated:
        return "CREATED";
    case AgentRecordCodec::ResponseStatus::kNotFound:
        return "NOT_FOUND";
    case AgentRecordCodec::ResponseStatus::kVersionMismatch:
        return "VERSION_MISMATCH";
    case AgentRecordCodec::ResponseStatus::kAlreadyExists:
        return "ALREADY_EXISTS";
    case AgentRecordCodec::ResponseStatus::kLeaseHeld:
        return "LEASE_HELD";
    case AgentRecordCodec::ResponseStatus::kLeaseExpired:
        return "LEASE_EXPIRED";
    case AgentRecordCodec::ResponseStatus::kFenced:
        return "FENCED";
    case AgentRecordCodec::ResponseStatus::kInvalidState:
        return "INVALID_STATE";
    case AgentRecordCodec::ResponseStatus::kIdempotencyConflict:
        return "IDEMPOTENCY_CONFLICT";
    case AgentRecordCodec::ResponseStatus::kResourceLimit:
        return "RESOURCE_LIMIT";
    case AgentRecordCodec::ResponseStatus::kCapacity:
        return "CAPACITY";
    }
    return "UNKNOWN";
}

} // namespace minidb
