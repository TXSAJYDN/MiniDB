#include "AgentStateStore.h"

#include "../base/Clock.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace minidb {
namespace {

using C = AgentRecordCodec;

constexpr C::ResponseStatus kOk = C::ResponseStatus::kOk;
constexpr C::ResponseStatus kCreated = C::ResponseStatus::kCreated;
constexpr C::ResponseStatus kNotFound = C::ResponseStatus::kNotFound;
constexpr C::ResponseStatus kVersionMismatch =
    C::ResponseStatus::kVersionMismatch;
constexpr C::ResponseStatus kAlreadyExists = C::ResponseStatus::kAlreadyExists;
constexpr C::ResponseStatus kLeaseHeld = C::ResponseStatus::kLeaseHeld;
constexpr C::ResponseStatus kLeaseExpired = C::ResponseStatus::kLeaseExpired;
constexpr C::ResponseStatus kFenced = C::ResponseStatus::kFenced;
constexpr C::ResponseStatus kInvalidState = C::ResponseStatus::kInvalidState;
constexpr C::ResponseStatus kIdempotencyConflict =
    C::ResponseStatus::kIdempotencyConflict;
constexpr C::ResponseStatus kResourceLimit = C::ResponseStatus::kResourceLimit;
constexpr C::ResponseStatus kCapacity = C::ResponseStatus::kCapacity;

void set_error(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
}

bool parse_u64(std::string_view text, std::uint64_t* value) {
    if (text.empty()) {
        return false;
    }
    std::uint64_t parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    *value = parsed;
    return true;
}

bool parse_i64_positive(std::string_view text, std::int64_t* value) {
    if (text.empty()) {
        return false;
    }
    std::int64_t parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    if (result.ec != std::errc{} || result.ptr != end || parsed <= 0) {
        return false;
    }
    *value = parsed;
    return true;
}

bool valid_binary_name(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
           value.find('\0') == std::string_view::npos;
}

bool parse_session_target(std::string_view text, bool final,
                          std::uint8_t* status) {
    if (text == "ACTIVE") {
        if (final) {
            return false;
        }
        *status = static_cast<std::uint8_t>(C::SessionStatus::kActive);
        return true;
    }
    if (text == "WAITING_TOOL") {
        if (final) {
            return false;
        }
        *status = static_cast<std::uint8_t>(C::SessionStatus::kWaitingTool);
        return true;
    }
    if (text == "COMPLETED") {
        if (!final) {
            return false;
        }
        *status = static_cast<std::uint8_t>(C::SessionStatus::kCompleted);
        return true;
    }
    if (text == "FAILED") {
        if (!final) {
            return false;
        }
        *status = static_cast<std::uint8_t>(C::SessionStatus::kFailed);
        return true;
    }
    if (text == "CANCELLED") {
        if (!final) {
            return false;
        }
        *status = static_cast<std::uint8_t>(C::SessionStatus::kCancelled);
        return true;
    }
    return false;
}

bool parse_tool_final(std::string_view text, std::uint8_t* status) {
    if (text == "SUCCEEDED") {
        *status = static_cast<std::uint8_t>(C::ToolStatus::kSucceeded);
        return true;
    }
    if (text == "FAILED") {
        *status = static_cast<std::uint8_t>(C::ToolStatus::kFailed);
        return true;
    }
    if (text == "CANCELLED") {
        *status = static_cast<std::uint8_t>(C::ToolStatus::kCancelled);
        return true;
    }
    return false;
}

bool parse_idempotency_tail(const std::vector<std::string>& args,
                            std::size_t required_without_ttl,
                            std::string_view prefix, AgentRequest* request,
                            std::string* error) {
    if (args.size() != required_without_ttl &&
        args.size() != required_without_ttl + 1) {
        set_error(error, std::string(prefix) + ": wrong argument count");
        return false;
    }
    request->idempotency_key = args[required_without_ttl - 1];
    if (!C::is_valid_idempotency_key(request->idempotency_key)) {
        set_error(error, std::string(prefix) + ": invalid idempotency key");
        return false;
    }
    if (args.size() == required_without_ttl + 1) {
        if (!parse_i64_positive(args.back(), &request->idempotency_ttl_ms) ||
            request->idempotency_ttl_ms < C::Limits::kMinIdempotencyTtlMs ||
            request->idempotency_ttl_ms > C::Limits::kMaxIdempotencyTtlMs) {
            set_error(error, std::string(prefix) + ": invalid idempotency TTL");
            return false;
        }
    }
    return true;
}

bool valid_session_ttl(std::int64_t ttl_ms) {
    return ttl_ms >= C::Limits::kMinSessionTtlMs &&
           ttl_ms <= C::Limits::kMaxSessionTtlMs;
}

bool valid_lease_ttl(std::int64_t ttl_ms) {
    return ttl_ms >= C::Limits::kMinLeaseTtlMs &&
           ttl_ms <= C::Limits::kMaxLeaseTtlMs;
}

std::int64_t saturating_add_i64(std::int64_t left, std::int64_t right) {
    if (right > 0 && left > std::numeric_limits<std::int64_t>::max() - right) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return left + right;
}

std::uint64_t saturating_add_u64(std::uint64_t left, std::uint64_t right) {
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    return right > maximum - left ? maximum : left + right;
}

bool add_checked(std::uint64_t* total, std::uint64_t delta) {
    const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
    if (delta > maximum - *total) {
        return false;
    }
    *total += delta;
    return true;
}

} // namespace

AgentStateStore::AgentStateStore(std::shared_ptr<ClockInterface> clock)
    : clock_(std::move(clock)) {
    stats_.logical_bytes_max = C::Limits::kMaxLogicalBytes;
}

std::int64_t AgentStateStore::now_wall_ms() const {
    return clock_ ? clock_->system_now_ms() : Clock::system_now_ms();
}

AgentStateStore::TimePoint AgentStateStore::now_monotonic() const {
    return clock_ ? clock_->now() : Clock::now();
}

AgentStateStore::TimePoint
AgentStateStore::deadline_from_ttl(std::int64_t ttl_ms) const {
    return now_monotonic() + Clock::ms_to_duration(ttl_ms);
}

AgentStateStore::TimePoint
AgentStateStore::deadline_from_absolute(std::int64_t absolute_ms) const {
    const TimePoint current = now_monotonic();
    if (absolute_ms <= 0) {
        return TimePoint::max();
    }
    const std::int64_t wall_now = now_wall_ms();
    if (absolute_ms <= wall_now) {
        return TimePoint::min();
    }
    const std::int64_t remaining = absolute_ms - wall_now;
    const auto duration = Clock::ms_to_duration(remaining);
    if (duration >= TimePoint::max() - current) {
        return TimePoint::max();
    }
    return current + duration;
}

bool AgentStateStore::is_mutation(AgentCommand command) noexcept {
    switch (command) {
    case AgentCommand::kSessionGet:
    case AgentCommand::kToolGet:
    case AgentCommand::kRecordGet:
        return false;
    default:
        return true;
    }
}

bool AgentStateStore::parse_request(std::string_view subcommand,
                                    const std::vector<std::string>& args,
                                    AgentRequest* request, std::string* error) {
    if (request == nullptr) {
        set_error(error, "agent request is null");
        return false;
    }
    AgentRequest parsed;
    parsed.subcommand.assign(subcommand.data(), subcommand.size());
    parsed.args = args;
    const auto fail = [&](std::string message) {
        set_error(error, std::move(message));
        return false;
    };
    const auto parse_u64_arg = [&](std::size_t index, std::uint64_t* value,
                                   const char* field) {
        if (!parse_u64(args[index], value)) {
            set_error(error, std::string("invalid ") + field);
            return false;
        }
        return true;
    };
    const auto parse_positive_arg = [&](std::size_t index, std::int64_t* value,
                                        const char* field) {
        if (!parse_i64_positive(args[index], value)) {
            set_error(error, std::string("invalid ") + field);
            return false;
        }
        return true;
    };

    if (subcommand == "SESSION_CREATE") {
        if (args.size() != 4 && args.size() != 5) {
            return fail("SESSION_CREATE: wrong argument count");
        }
        parsed.command = AgentCommand::kSessionCreate;
        parsed.session_id = args[0];
        if (!valid_binary_name(parsed.session_id, C::Limits::kMaxNameSize)) {
            return fail("SESSION_CREATE: invalid session id");
        }
        if (!parse_positive_arg(1, &parsed.ttl_ms, "session TTL") ||
            !valid_session_ttl(parsed.ttl_ms)) {
            return fail("SESSION_CREATE: invalid session TTL");
        }
        parsed.state = args[2];
        if (!parse_idempotency_tail(args, 4, subcommand, &parsed, error)) {
            return false;
        }
    } else if (subcommand == "SESSION_GET") {
        if (args.size() != 1) {
            return fail("SESSION_GET: wrong argument count");
        }
        parsed.command = AgentCommand::kSessionGet;
        parsed.session_id = args[0];
        if (!valid_binary_name(parsed.session_id, C::Limits::kMaxNameSize)) {
            return fail("SESSION_GET: invalid session id");
        }
    } else if (subcommand == "SESSION_UPDATE") {
        if (args.size() != 6 && args.size() != 7) {
            return fail("SESSION_UPDATE: wrong argument count");
        }
        parsed.command = AgentCommand::kSessionUpdate;
        parsed.session_id = args[0];
        if (!valid_binary_name(parsed.session_id, C::Limits::kMaxNameSize)) {
            return fail("SESSION_UPDATE: invalid session id");
        }
        if (!parse_u64_arg(1, &parsed.expected_revision, "expected revision")) {
            return false;
        }
        if (!parse_session_target(args[2], false, &parsed.target_status)) {
            return fail("SESSION_UPDATE: invalid target state");
        }
        if (!parse_positive_arg(3, &parsed.ttl_ms, "session TTL") ||
            !valid_session_ttl(parsed.ttl_ms)) {
            return fail("SESSION_UPDATE: invalid session TTL");
        }
        parsed.state = args[4];
        if (!parse_idempotency_tail(args, 6, subcommand, &parsed, error)) {
            return false;
        }
    } else if (subcommand == "SESSION_CLOSE") {
        if (args.size() != 4 && args.size() != 5) {
            return fail("SESSION_CLOSE: wrong argument count");
        }
        parsed.command = AgentCommand::kSessionClose;
        parsed.session_id = args[0];
        if (!valid_binary_name(parsed.session_id, C::Limits::kMaxNameSize)) {
            return fail("SESSION_CLOSE: invalid session id");
        }
        if (!parse_u64_arg(1, &parsed.expected_revision, "expected revision")) {
            return false;
        }
        if (!parse_session_target(args[2], true, &parsed.target_status)) {
            return fail("SESSION_CLOSE: invalid final state");
        }
        if (!parse_idempotency_tail(args, 4, subcommand, &parsed, error)) {
            return false;
        }
    } else if (subcommand == "SESSION_PURGE") {
        if (args.size() != 3 && args.size() != 4) {
            return fail("SESSION_PURGE: wrong argument count");
        }
        parsed.command = AgentCommand::kSessionPurge;
        parsed.session_id = args[0];
        if (!valid_binary_name(parsed.session_id, C::Limits::kMaxNameSize)) {
            return fail("SESSION_PURGE: invalid session id");
        }
        if (!parse_u64_arg(1, &parsed.expected_revision, "expected revision")) {
            return false;
        }
        if (!parse_idempotency_tail(args, 3, subcommand, &parsed, error)) {
            return false;
        }
    } else if (subcommand == "TOOL_CREATE") {
        if (args.size() != 5 && args.size() != 6) {
            return fail("TOOL_CREATE: wrong argument count");
        }
        parsed.command = AgentCommand::kToolCreate;
        parsed.session_id = args[0];
        parsed.tool_id = args[1];
        parsed.tool_name = args[2];
        parsed.result.clear();
        if (!valid_binary_name(parsed.session_id, C::Limits::kMaxNameSize)) {
            return fail("TOOL_CREATE: invalid session id");
        }
        if (!valid_binary_name(parsed.tool_id, C::Limits::kMaxNameSize)) {
            return fail("TOOL_CREATE: invalid tool id");
        }
        if (!valid_binary_name(parsed.tool_name, C::Limits::kMaxToolNameSize)) {
            return fail("TOOL_CREATE: invalid tool name");
        }
        // Reuse value for the request body to keep AgentRequest compact.
        parsed.value = args[3];
        if (!parse_idempotency_tail(args, 5, subcommand, &parsed, error)) {
            return false;
        }
    } else if (subcommand == "TOOL_GET") {
        if (args.size() != 2) {
            return fail("TOOL_GET: wrong argument count");
        }
        parsed.command = AgentCommand::kToolGet;
        parsed.session_id = args[0];
        parsed.tool_id = args[1];
        if (!valid_binary_name(parsed.session_id, C::Limits::kMaxNameSize) ||
            !valid_binary_name(parsed.tool_id, C::Limits::kMaxNameSize)) {
            return fail("TOOL_GET: invalid id");
        }
    } else if (subcommand == "TOOL_CLAIM") {
        if (args.size() != 6 && args.size() != 7) {
            return fail("TOOL_CLAIM: wrong argument count");
        }
        parsed.command = AgentCommand::kToolClaim;
        parsed.session_id = args[0];
        parsed.tool_id = args[1];
        if (!valid_binary_name(parsed.session_id, C::Limits::kMaxNameSize) ||
            !valid_binary_name(parsed.tool_id, C::Limits::kMaxNameSize)) {
            return fail("TOOL_CLAIM: invalid id");
        }
        if (!parse_u64_arg(2, &parsed.expected_revision,
                           "expected tool revision") ||
            !parse_u64_arg(3, &parsed.fence, "fence") ||
            !parse_positive_arg(4, &parsed.ttl_ms, "lease TTL") ||
            !valid_lease_ttl(parsed.ttl_ms)) {
            return fail("TOOL_CLAIM: invalid numeric argument");
        }
        if (!parse_idempotency_tail(args, 6, subcommand, &parsed, error)) {
            return false;
        }
    } else if (subcommand == "TOOL_COMPLETE") {
        if (args.size() != 7 && args.size() != 8) {
            return fail("TOOL_COMPLETE: wrong argument count");
        }
        parsed.command = AgentCommand::kToolComplete;
        parsed.session_id = args[0];
        parsed.tool_id = args[1];
        if (!valid_binary_name(parsed.session_id, C::Limits::kMaxNameSize) ||
            !valid_binary_name(parsed.tool_id, C::Limits::kMaxNameSize)) {
            return fail("TOOL_COMPLETE: invalid id");
        }
        if (!parse_u64_arg(2, &parsed.expected_revision,
                           "expected tool revision") ||
            !parse_u64_arg(3, &parsed.fence, "fence")) {
            return false;
        }
        if (!parse_tool_final(args[4], &parsed.target_status)) {
            return fail("TOOL_COMPLETE: invalid final state");
        }
        parsed.result = args[5];
        if (!parse_idempotency_tail(args, 7, subcommand, &parsed, error)) {
            return false;
        }
    } else if (subcommand == "TOOL_REQUEUE") {
        if (args.size() != 5 && args.size() != 6) {
            return fail("TOOL_REQUEUE: wrong argument count");
        }
        parsed.command = AgentCommand::kToolRequeue;
        parsed.session_id = args[0];
        parsed.tool_id = args[1];
        if (!valid_binary_name(parsed.session_id, C::Limits::kMaxNameSize) ||
            !valid_binary_name(parsed.tool_id, C::Limits::kMaxNameSize)) {
            return fail("TOOL_REQUEUE: invalid id");
        }
        if (!parse_u64_arg(2, &parsed.expected_revision,
                           "expected tool revision") ||
            !parse_u64_arg(3, &parsed.fence, "fence")) {
            return false;
        }
        if (!parse_idempotency_tail(args, 5, subcommand, &parsed, error)) {
            return false;
        }
    } else if (subcommand == "LEASE_ACQUIRE") {
        if (args.size() != 3 && args.size() != 4) {
            return fail("LEASE_ACQUIRE: wrong argument count");
        }
        parsed.command = AgentCommand::kLeaseAcquire;
        parsed.session_id = args[0];
        if (!valid_binary_name(parsed.session_id, C::Limits::kMaxNameSize)) {
            return fail("LEASE_ACQUIRE: invalid session id");
        }
        if (!parse_positive_arg(1, &parsed.ttl_ms, "lease TTL") ||
            !valid_lease_ttl(parsed.ttl_ms)) {
            return fail("LEASE_ACQUIRE: invalid lease TTL");
        }
        if (!parse_idempotency_tail(args, 3, subcommand, &parsed, error)) {
            return false;
        }
    } else if (subcommand == "LEASE_RENEW") {
        if (args.size() != 5 && args.size() != 6) {
            return fail("LEASE_RENEW: wrong argument count");
        }
        parsed.command = AgentCommand::kLeaseRenew;
        parsed.session_id = args[0];
        if (!valid_binary_name(parsed.session_id, C::Limits::kMaxNameSize)) {
            return fail("LEASE_RENEW: invalid session id");
        }
        if (!parse_u64_arg(1, &parsed.expected_revision, "expected revision") ||
            !parse_u64_arg(2, &parsed.fence, "fence") ||
            !parse_positive_arg(3, &parsed.ttl_ms, "lease TTL") ||
            !valid_lease_ttl(parsed.ttl_ms)) {
            return fail("LEASE_RENEW: invalid numeric argument");
        }
        if (!parse_idempotency_tail(args, 5, subcommand, &parsed, error)) {
            return false;
        }
    } else if (subcommand == "LEASE_RELEASE") {
        if (args.size() != 4 && args.size() != 5) {
            return fail("LEASE_RELEASE: wrong argument count");
        }
        parsed.command = AgentCommand::kLeaseRelease;
        parsed.session_id = args[0];
        if (!valid_binary_name(parsed.session_id, C::Limits::kMaxNameSize)) {
            return fail("LEASE_RELEASE: invalid session id");
        }
        if (!parse_u64_arg(1, &parsed.expected_revision, "expected revision") ||
            !parse_u64_arg(2, &parsed.fence, "fence")) {
            return false;
        }
        if (!parse_idempotency_tail(args, 4, subcommand, &parsed, error)) {
            return false;
        }
    } else if (subcommand == "RECORD_GET") {
        if (args.size() != 1) {
            return fail("RECORD_GET: wrong argument count");
        }
        parsed.command = AgentCommand::kRecordGet;
        parsed.record_name = args[0];
        if (!valid_binary_name(parsed.record_name, C::Limits::kMaxNameSize)) {
            return fail("RECORD_GET: invalid record name");
        }
    } else if (subcommand == "RECORD_CAS") {
        if (args.size() != 4 && args.size() != 5) {
            return fail("RECORD_CAS: wrong argument count");
        }
        parsed.command = AgentCommand::kRecordCas;
        parsed.record_name = args[0];
        if (!valid_binary_name(parsed.record_name, C::Limits::kMaxNameSize)) {
            return fail("RECORD_CAS: invalid record name");
        }
        if (!parse_u64_arg(1, &parsed.expected_revision, "expected revision")) {
            return false;
        }
        parsed.value = args[2];
        if (!parse_idempotency_tail(args, 4, subcommand, &parsed, error)) {
            return false;
        }
    } else {
        return fail("unknown AGENT subcommand");
    }

    *request = std::move(parsed);
    return true;
}

bool AgentStateStore::rebuild(const std::vector<KvStore::Entry>& entries,
                              std::string* error) {
    AgentStateStore rebuilt(clock_);
    const std::string marker_key = C::marker_key();
    std::size_t marker_count = 0;
    std::uint64_t logical_bytes = 0;
    std::set<std::string> seen_keys;

    for (const KvStore::Entry& entry : entries) {
        if (!C::is_reserved_key(entry.key)) {
            continue;
        }
        if (!seen_keys.insert(entry.key).second) {
            set_error(error, "duplicate key in agent rebuild input");
            return false;
        }
        if (!add_checked(&logical_bytes, entry.key.size()) ||
            !add_checked(&logical_bytes, entry.value.size())) {
            set_error(error, "agent logical byte count overflow");
            return false;
        }
        rebuilt.key_sizes_[entry.key] = entry.key.size();
        if (entry.key == marker_key) {
            ++marker_count;
            std::string marker_error;
            if (!C::decode_marker(entry.value, &marker_error)) {
                set_error(error, "invalid agent marker: " + marker_error);
                return false;
            }
            rebuilt.marker_present_ = true;
            continue;
        }
        if (entry.value.size() > C::Limits::kMaxRecordSize) {
            set_error(error, "agent record exceeds the size limit");
            return false;
        }
    }

    if (marker_count > 1) {
        set_error(error, "agent namespace contains more than one marker");
        return false;
    }

    if (marker_count == 0) {
        for (const KvStore::Entry& entry : entries) {
            if (C::is_reserved_key(entry.key)) {
                set_error(error,
                          "agent namespace exists without the MDAM marker");
                return false;
            }
        }
        rebuilt.stats_.logical_bytes = 0;
        rebuilt.stats_.logical_bytes_max = C::Limits::kMaxLogicalBytes;
        sessions_ = std::move(rebuilt.sessions_);
        tools_ = std::move(rebuilt.tools_);
        idempotency_ = std::move(rebuilt.idempotency_);
        generic_ = std::move(rebuilt.generic_);
        key_sizes_ = std::move(rebuilt.key_sizes_);
        marker_present_ = false;
        stats_ = rebuilt.stats_;
        reservations_.clear();
        reserved_totals_ = Reservation{};
        marker_reservations_ = 0;
        last_gc_candidates_.clear();
        return true;
    }

    // Sessions must exist before tools are validated.
    for (const KvStore::Entry& entry : entries) {
        if (!C::is_reserved_key(entry.key) || entry.key == marker_key) {
            continue;
        }
        C::KeyInfo info;
        C::Record record;
        C::SessionPayload session_payload;
        C::ToolPayload tool_payload;
        C::IdempotencyPayload idempotency_payload;
        std::string decode_error;
        if (!C::decode_key(entry.key, &info, &decode_error) ||
            !C::decode_typed_record(entry.key, entry.value, &record,
                                    &session_payload, &tool_payload,
                                    &idempotency_payload, &decode_error)) {
            set_error(error, "invalid agent record: " + decode_error);
            return false;
        }
        if (info.kind == C::KeyKind::kSession) {
            if (!C::valid_session_status(record.status) ||
                record.revision == 0 ||
                session_payload.session_expires_at_ms <= 0 ||
                record.created_at_ms <= 0 ||
                record.updated_at_ms < record.created_at_ms ||
                record.lease_expires_at_ms < 0 ||
                (record.lease_expires_at_ms > 0 &&
                 record.lease_expires_at_ms >
                     session_payload.session_expires_at_ms)) {
                set_error(error, "invalid session record invariants");
                return false;
            }
            SessionMeta meta;
            meta.status = record.status;
            meta.revision = record.revision;
            meta.last_fence = record.last_fence;
            meta.created_at_ms = record.created_at_ms;
            meta.updated_at_ms = record.updated_at_ms;
            meta.session_expires_at_ms = session_payload.session_expires_at_ms;
            meta.lease_expires_at_ms = record.lease_expires_at_ms;
            meta.encoded_size = entry.value.size();
            meta.session_deadline =
                rebuilt.deadline_from_absolute(meta.session_expires_at_ms);
            if (meta.lease_expires_at_ms > 0) {
                meta.lease_deadline =
                    rebuilt.deadline_from_absolute(meta.lease_expires_at_ms);
            }
            rebuilt.sessions_[info.session_id] = std::move(meta);
        } else if (info.kind == C::KeyKind::kIdempotency) {
            if (record.status != 0 || record.revision != 0 ||
                record.last_fence != 0 || record.lease_expires_at_ms != 0 ||
                record.created_at_ms <= 0 ||
                record.updated_at_ms < record.created_at_ms ||
                idempotency_payload.expires_at_ms <= 0) {
                set_error(error, "invalid idempotency record invariants");
                return false;
            }
            C::Response response;
            if (!C::decode_response(idempotency_payload.response, &response,
                                    &decode_error)) {
                set_error(error, "invalid persisted idempotency response: " +
                                     decode_error);
                return false;
            }
            IdempotencyMeta meta;
            meta.request_hash = idempotency_payload.request_hash;
            meta.canonical_request_len =
                idempotency_payload.canonical_request_len;
            meta.expires_at_ms = idempotency_payload.expires_at_ms;
            meta.encoded_size = entry.value.size();
            meta.response_size = idempotency_payload.response.size();
            meta.deadline = rebuilt.deadline_from_absolute(meta.expires_at_ms);
            rebuilt.idempotency_[entry.key] = meta;
        } else if (info.kind == C::KeyKind::kGeneric) {
            if (record.status != 0 || record.revision == 0 ||
                record.last_fence != 0 || record.lease_expires_at_ms != 0 ||
                record.created_at_ms <= 0 ||
                record.updated_at_ms < record.created_at_ms) {
                set_error(error, "invalid generic record invariants");
                return false;
            }
            GenericMeta meta;
            meta.revision = record.revision;
            meta.created_at_ms = record.created_at_ms;
            meta.updated_at_ms = record.updated_at_ms;
            meta.encoded_size = entry.value.size();
            rebuilt.generic_[entry.key] = meta;
        } else if (info.kind == C::KeyKind::kTool ||
                   info.kind == C::KeyKind::kMarker) {
            // Tools are validated in the second pass. A second marker is
            // rejected before this point.
        }
    }

    for (const KvStore::Entry& entry : entries) {
        if (!C::is_reserved_key(entry.key) || entry.key == marker_key) {
            continue;
        }
        C::KeyInfo info;
        C::Record record;
        C::SessionPayload session_payload;
        C::ToolPayload tool_payload;
        C::IdempotencyPayload idempotency_payload;
        std::string decode_error;
        if (!C::decode_key(entry.key, &info, &decode_error) ||
            !C::decode_typed_record(entry.key, entry.value, &record,
                                    &session_payload, &tool_payload,
                                    &idempotency_payload, &decode_error)) {
            set_error(error, "invalid agent record: " + decode_error);
            return false;
        }
        if (info.kind != C::KeyKind::kTool) {
            continue;
        }
        if (!C::valid_tool_status(record.status) || record.revision == 0 ||
            tool_payload.tool_id != info.tool_id || record.created_at_ms <= 0 ||
            record.updated_at_ms < record.created_at_ms ||
            record.lease_expires_at_ms < 0) {
            set_error(error, "invalid tool record invariants");
            return false;
        }
        const auto session_it = rebuilt.sessions_.find(info.session_id);
        if (session_it == rebuilt.sessions_.end()) {
            set_error(error, "orphan agent tool record");
            return false;
        }
        if (session_it->second.tool_count >= C::Limits::kMaxToolsPerSession) {
            set_error(error, "agent session exceeds the tool limit");
            return false;
        }
        ++session_it->second.tool_count;
        ToolMeta meta;
        meta.session_id = info.session_id;
        meta.tool_id = info.tool_id;
        meta.status = record.status;
        meta.revision = record.revision;
        meta.last_fence = record.last_fence;
        meta.created_at_ms = record.created_at_ms;
        meta.updated_at_ms = record.updated_at_ms;
        meta.lease_expires_at_ms = record.lease_expires_at_ms;
        meta.encoded_size = entry.value.size();
        if (meta.lease_expires_at_ms > 0) {
            meta.lease_deadline =
                rebuilt.deadline_from_absolute(meta.lease_expires_at_ms);
        }
        rebuilt.tools_[entry.key] = std::move(meta);
    }

    if (rebuilt.sessions_.size() > C::Limits::kMaxSessionRecords ||
        rebuilt.tools_.size() > C::Limits::kMaxToolRecords ||
        rebuilt.generic_.size() > C::Limits::kMaxGenericRecords ||
        rebuilt.idempotency_.size() > C::Limits::kMaxIdempotencyRecords) {
        set_error(error, "agent namespace exceeds a record-count limit");
        return false;
    }
    if (logical_bytes > C::Limits::kMaxLogicalBytes) {
        set_error(error, "agent namespace exceeds the logical byte limit");
        return false;
    }

    rebuilt.stats_.logical_bytes = logical_bytes;
    rebuilt.stats_.logical_bytes_max = C::Limits::kMaxLogicalBytes;
    rebuilt.refresh_derived_stats();

    sessions_ = std::move(rebuilt.sessions_);
    tools_ = std::move(rebuilt.tools_);
    idempotency_ = std::move(rebuilt.idempotency_);
    generic_ = std::move(rebuilt.generic_);
    key_sizes_ = std::move(rebuilt.key_sizes_);
    marker_present_ = true;
    stats_ = rebuilt.stats_;
    reservations_.clear();
    reserved_totals_ = Reservation{};
    marker_reservations_ = 0;
    last_gc_candidates_.clear();
    return true;
}
bool AgentStateStore::session_expired(const SessionMeta& session,
                                      TimePoint now) const {
    return now >= session.session_deadline;
}

bool AgentStateStore::lease_expired(const SessionMeta& session,
                                    TimePoint now) const {
    return session.lease_expires_at_ms <= 0 || now >= session.lease_deadline;
}

void AgentStateStore::refresh_derived_stats() {
    std::uint64_t sessions_live = 0;
    std::uint64_t tools_pending = 0;
    std::uint64_t tools_running = 0;
    std::uint64_t tools_succeeded = 0;
    std::uint64_t tools_failed = 0;
    std::uint64_t tools_cancelled = 0;
    const TimePoint now = now_monotonic();

    for (const auto& [session_id, session] : sessions_) {
        (void)session_id;
        if (!C::session_terminal(session.status) &&
            !session_expired(session, now)) {
            ++sessions_live;
        }
    }
    for (const auto& [key, tool] : tools_) {
        (void)key;
        const auto parent = sessions_.find(tool.session_id);
        const bool parent_terminal =
            parent == sessions_.end() ||
            C::session_terminal(parent->second.status) ||
            session_expired(parent->second, now);
        const C::ToolStatus effective =
            parent_terminal && !C::tool_terminal(tool.status)
                ? C::ToolStatus::kCancelled
                : static_cast<C::ToolStatus>(tool.status);
        switch (effective) {
        case C::ToolStatus::kPending:
            ++tools_pending;
            break;
        case C::ToolStatus::kRunning:
            ++tools_running;
            break;
        case C::ToolStatus::kSucceeded:
            ++tools_succeeded;
            break;
        case C::ToolStatus::kFailed:
            ++tools_failed;
            break;
        case C::ToolStatus::kCancelled:
            ++tools_cancelled;
            break;
        }
    }

    stats_.sessions_stored = sessions_.size();
    stats_.sessions_live = sessions_live;
    stats_.tools_stored = tools_.size();
    stats_.tools_pending = tools_pending;
    stats_.tools_running = tools_running;
    stats_.tools_succeeded = tools_succeeded;
    stats_.tools_failed = tools_failed;
    stats_.tools_cancelled = tools_cancelled;
    stats_.records_stored = generic_.size();
    stats_.idempotency_stored = idempotency_.size();
    stats_.logical_bytes_max = C::Limits::kMaxLogicalBytes;
}

AgentStateStore::Stats AgentStateStore::stats() const {
    Stats result = stats_;
    result.sessions_stored = sessions_.size();
    result.tools_stored = tools_.size();
    result.records_stored = generic_.size();
    result.idempotency_stored = idempotency_.size();
    result.logical_bytes_max = C::Limits::kMaxLogicalBytes;
    result.sessions_live = 0;
    result.tools_pending = 0;
    result.tools_running = 0;
    result.tools_succeeded = 0;
    result.tools_failed = 0;
    result.tools_cancelled = 0;
    const TimePoint now = now_monotonic();
    for (const auto& [session_id, session] : sessions_) {
        (void)session_id;
        if (!C::session_terminal(session.status) &&
            !session_expired(session, now)) {
            ++result.sessions_live;
        }
    }
    for (const auto& [key, tool] : tools_) {
        (void)key;
        const auto parent = sessions_.find(tool.session_id);
        const bool parent_terminal =
            parent == sessions_.end() ||
            C::session_terminal(parent->second.status) ||
            session_expired(parent->second, now);
        const C::ToolStatus effective =
            parent_terminal && !C::tool_terminal(tool.status)
                ? C::ToolStatus::kCancelled
                : static_cast<C::ToolStatus>(tool.status);
        switch (effective) {
        case C::ToolStatus::kPending:
            ++result.tools_pending;
            break;
        case C::ToolStatus::kRunning:
            ++result.tools_running;
            break;
        case C::ToolStatus::kSucceeded:
            ++result.tools_succeeded;
            break;
        case C::ToolStatus::kFailed:
            ++result.tools_failed;
            break;
        case C::ToolStatus::kCancelled:
            ++result.tools_cancelled;
            break;
        }
    }
    return result;
}

void AgentStateStore::observe_session_transition(const SessionMeta* before,
                                                 const SessionMeta* after) {
    if (before == nullptr && after != nullptr) {
        ++stats_.sessions_created;
    }
    if (before == nullptr && after == nullptr) {
        return;
    }
    if (before != nullptr && after == nullptr) {
        ++stats_.sessions_purged;
        return;
    }
    if (before != nullptr && after != nullptr) {
        if (!C::session_terminal(before->status) &&
            C::session_terminal(after->status)) {
            switch (static_cast<C::SessionStatus>(after->status)) {
            case C::SessionStatus::kCompleted:
                ++stats_.sessions_completed;
                break;
            case C::SessionStatus::kFailed:
                ++stats_.sessions_failed;
                break;
            case C::SessionStatus::kCancelled:
                ++stats_.sessions_cancelled;
                break;
            default:
                break;
            }
        }
        const TimePoint now = now_monotonic();
        const bool old_lease_live = !lease_expired(*before, now);
        const bool new_lease_live = !lease_expired(*after, now);
        if (!old_lease_live && new_lease_live &&
            after->last_fence > before->last_fence) {
            ++stats_.lease_acquires;
        } else if (old_lease_live && new_lease_live &&
                   before->last_fence == after->last_fence &&
                   before->lease_expires_at_ms != after->lease_expires_at_ms) {
            ++stats_.lease_renews;
        }
    }
}

void AgentStateStore::observe_tool_transition(const ToolMeta* before,
                                              const ToolMeta* after) {
    (void)before;
    (void)after;
}

bool AgentStateStore::validate_record_for_apply(std::string_view key,
                                                std::string_view value,
                                                C::KeyInfo* key_info,
                                                std::string* error) const {
    C::Record record;
    C::SessionPayload session_payload;
    C::ToolPayload tool_payload;
    C::IdempotencyPayload idempotency_payload;
    if (!C::decode_key(key, key_info, error) ||
        !C::decode_typed_record(key, value, &record, &session_payload,
                                &tool_payload, &idempotency_payload, error)) {
        return false;
    }
    switch (key_info->kind) {
    case C::KeyKind::kMarker:
        set_error(error, "marker cannot be written as a typed record");
        return false;
    case C::KeyKind::kSession:
        if (!C::valid_session_status(record.status) || record.revision == 0) {
            set_error(error, "invalid session record invariants");
            return false;
        }
        break;
    case C::KeyKind::kTool:
        if (!C::valid_tool_status(record.status) || record.revision == 0 ||
            tool_payload.tool_id != key_info->tool_id) {
            set_error(error, "invalid tool record invariants");
            return false;
        }
        break;
    case C::KeyKind::kIdempotency:
        if (record.status != 0 || record.revision != 0 ||
            record.last_fence != 0 || record.lease_expires_at_ms != 0 ||
            idempotency_payload.expires_at_ms <= 0) {
            set_error(error, "invalid idempotency record invariants");
            return false;
        }
        break;
    case C::KeyKind::kGeneric:
        if (record.status != 0 || record.revision == 0 ||
            record.last_fence != 0 || record.lease_expires_at_ms != 0) {
            set_error(error, "invalid generic record invariants");
            return false;
        }
        break;
    }
    return true;
}

bool AgentStateStore::reserve_operations(
    const std::vector<WalTxnOp>& operations, std::uint64_t* reservation_id,
    C::ResponseStatus* failure, std::string* error) {
    if (operations.empty()) {
        set_error(error, "agent mutation has no operations");
        return false;
    }
    std::string txn_error;
    if (!WalFormat::validate_transaction(operations, &txn_error)) {
        *failure = kResourceLimit;
        set_error(error, txn_error);
        return false;
    }

    Reservation delta;
    for (const WalTxnOp& operation : operations) {
        if (operation.op != WalOp::kSet && operation.op != WalOp::kDel) {
            set_error(error, "agent transaction contains an unsupported op");
            return false;
        }
        C::KeyInfo info;
        std::string decode_error;
        if (!C::decode_key(operation.key, &info, &decode_error)) {
            set_error(error, "invalid agent operation key: " + decode_error);
            return false;
        }
        if (info.kind == C::KeyKind::kMarker) {
            if (operation.op != WalOp::kSet) {
                set_error(error, "agent marker cannot be deleted");
                return false;
            }
            if (marker_present_) {
                continue;
            }
            ++delta.markers;
            // Every pre-marker plan carries the marker so that a cancelled
            // admission cannot starve another pending mutation. Each such
            // reservation accounts for its own marker bytes until it commits
            // or is cancelled; projection then cannot understate the bound.
            const std::uint64_t cost =
                operation.key.size() + operation.value.size();
            if (!add_checked(&delta.bytes, cost)) {
                *failure = kCapacity;
                return false;
            }
            continue;
        }
        if (operation.op == WalOp::kDel) {
            // Deletions never release reserved capacity before they commit.
            continue;
        }

        C::KeyInfo validated_info;
        if (!validate_record_for_apply(operation.key, operation.value,
                                       &validated_info, &decode_error)) {
            set_error(error, decode_error);
            return false;
        }
        const std::uint64_t new_cost =
            operation.key.size() + operation.value.size();
        std::uint64_t old_cost = 0;
        std::size_t old_value_size = 0;
        switch (info.kind) {
        case C::KeyKind::kSession: {
            const auto it = sessions_.find(info.session_id);
            if (it == sessions_.end()) {
                ++delta.sessions;
            } else {
                old_value_size = it->second.encoded_size;
            }
            break;
        }
        case C::KeyKind::kTool: {
            const auto it = tools_.find(operation.key);
            if (it == tools_.end()) {
                ++delta.tools;
                ++delta.tools_per_session[info.session_id];
            } else {
                old_value_size = it->second.encoded_size;
            }
            break;
        }
        case C::KeyKind::kGeneric: {
            const auto it = generic_.find(operation.key);
            if (it == generic_.end()) {
                ++delta.generic;
            } else {
                old_value_size = it->second.encoded_size;
            }
            break;
        }
        case C::KeyKind::kIdempotency: {
            const auto it = idempotency_.find(operation.key);
            if (it == idempotency_.end()) {
                ++delta.idempotency;
            } else {
                old_value_size = it->second.encoded_size;
            }
            break;
        }
        case C::KeyKind::kMarker:
            break;
        }
        if (old_value_size != 0) {
            old_cost = operation.key.size() + old_value_size;
        }
        if (new_cost > old_cost &&
            !add_checked(&delta.bytes, new_cost - old_cost)) {
            *failure = kCapacity;
            return false;
        }
    }

    const auto projected_sessions =
        saturating_add_u64(stats_.sessions_stored, reserved_totals_.sessions);
    const auto projected_tools =
        saturating_add_u64(stats_.tools_stored, reserved_totals_.tools);
    const auto projected_generic =
        saturating_add_u64(stats_.records_stored, reserved_totals_.generic);
    const auto projected_idempotency = saturating_add_u64(
        stats_.idempotency_stored, reserved_totals_.idempotency);
    const auto projected_bytes =
        saturating_add_u64(stats_.logical_bytes, reserved_totals_.bytes);

    const auto exceeds = [](std::uint64_t projected, std::uint64_t delta,
                            std::uint64_t maximum) {
        return projected > maximum || delta > maximum - projected;
    };
    if (exceeds(projected_sessions, delta.sessions,
                C::Limits::kMaxSessionRecords) ||
        exceeds(projected_tools, delta.tools, C::Limits::kMaxToolRecords) ||
        exceeds(projected_generic, delta.generic,
                C::Limits::kMaxGenericRecords) ||
        exceeds(projected_idempotency, delta.idempotency,
                C::Limits::kMaxIdempotencyRecords)) {
        *failure = kCapacity;
        return false;
    }
    std::uint64_t projected_after_bytes = projected_bytes;
    if (!add_checked(&projected_after_bytes, delta.bytes) ||
        projected_after_bytes > C::Limits::kMaxLogicalBytes) {
        *failure = kCapacity;
        return false;
    }
    for (const auto& [session_id, added] : delta.tools_per_session) {
        const auto session_it = sessions_.find(session_id);
        std::uint64_t current = 0;
        if (session_it != sessions_.end()) {
            current = session_it->second.tool_count;
        }
        const auto reserved_it =
            reserved_totals_.tools_per_session.find(session_id);
        if (reserved_it != reserved_totals_.tools_per_session.end()) {
            current = saturating_add_u64(current, reserved_it->second);
        }
        if (current > C::Limits::kMaxToolsPerSession ||
            added > C::Limits::kMaxToolsPerSession - current) {
            *failure = kResourceLimit;
            return false;
        }
    }
    if (next_reservation_id_ == 0) {
        set_error(error, "agent reservation id space is exhausted");
        return false;
    }
    const std::uint64_t id = next_reservation_id_++;
    reservations_[id] = delta;
    if (!add_checked(&reserved_totals_.sessions, delta.sessions) ||
        !add_checked(&reserved_totals_.tools, delta.tools) ||
        !add_checked(&reserved_totals_.generic, delta.generic) ||
        !add_checked(&reserved_totals_.idempotency, delta.idempotency) ||
        !add_checked(&reserved_totals_.bytes, delta.bytes) ||
        !add_checked(&reserved_totals_.markers, delta.markers) ||
        !add_checked(&marker_reservations_, delta.markers)) {
        reservations_.erase(id);
        *failure = kCapacity;
        set_error(error, "agent reservation accounting overflow");
        return false;
    }
    for (const auto& [session_id, added] : delta.tools_per_session) {
        std::uint64_t& total = reserved_totals_.tools_per_session[session_id];
        if (!add_checked(&total, added)) {
            reservations_.erase(id);
            reserved_totals_.sessions -= delta.sessions;
            reserved_totals_.tools -= delta.tools;
            reserved_totals_.generic -= delta.generic;
            reserved_totals_.idempotency -= delta.idempotency;
            reserved_totals_.bytes -= delta.bytes;
            reserved_totals_.markers -= delta.markers;
            marker_reservations_ -= delta.markers;
            *failure = kCapacity;
            set_error(error, "agent per-session reservation overflow");
            return false;
        }
    }
    *reservation_id = id;
    *failure = kOk;
    return true;
}

void AgentStateStore::release_reservation_locked(
    std::uint64_t reservation_id) noexcept {
    if (reservation_id == 0) {
        return;
    }
    const auto it = reservations_.find(reservation_id);
    if (it == reservations_.end()) {
        return;
    }
    const Reservation& reservation = it->second;
    auto subtract = [](std::uint64_t* total, std::uint64_t value) {
        *total = value > *total ? 0 : *total - value;
    };
    subtract(&reserved_totals_.sessions, reservation.sessions);
    subtract(&reserved_totals_.tools, reservation.tools);
    subtract(&reserved_totals_.generic, reservation.generic);
    subtract(&reserved_totals_.idempotency, reservation.idempotency);
    subtract(&reserved_totals_.bytes, reservation.bytes);
    subtract(&reserved_totals_.markers, reservation.markers);
    subtract(&marker_reservations_, reservation.markers);
    for (const auto& [session_id, added] : reservation.tools_per_session) {
        const auto total_it =
            reserved_totals_.tools_per_session.find(session_id);
        if (total_it == reserved_totals_.tools_per_session.end()) {
            continue;
        }
        if (added >= total_it->second) {
            reserved_totals_.tools_per_session.erase(total_it);
        } else {
            total_it->second -= added;
        }
    }
    reservations_.erase(it);
}

void AgentStateStore::cancel_reservation(
    std::uint64_t reservation_id) noexcept {
    release_reservation_locked(reservation_id);
}

bool AgentStateStore::apply_set(std::string_view key, std::string_view value,
                                std::string* error) {
    C::KeyInfo info;
    if (!C::decode_key(key, &info, error)) {
        return false;
    }
    const std::string owned_key(key);
    std::uint64_t old_cost = 0;
    std::uint64_t new_cost = key.size() + value.size();
    if (info.kind == C::KeyKind::kMarker) {
        std::string marker_error;
        if (!C::decode_marker(value, &marker_error)) {
            set_error(error, "invalid agent marker: " + marker_error);
            return false;
        }
        const auto size_it = key_sizes_.find(owned_key);
        if (size_it != key_sizes_.end()) {
            new_cost -= size_it->second;
        }
        marker_present_ = true;
        key_sizes_[owned_key] = key.size();
        stats_.logical_bytes += new_cost;
        return true;
    }

    C::Record record;
    C::SessionPayload session_payload;
    C::ToolPayload tool_payload;
    C::IdempotencyPayload idempotency_payload;
    if (!C::decode_typed_record(key, value, &record, &session_payload,
                                &tool_payload, &idempotency_payload, error)) {
        return false;
    }

    const auto size_it = key_sizes_.find(owned_key);
    if (size_it != key_sizes_.end()) {
        old_cost = size_it->second;
    }

    switch (info.kind) {
    case C::KeyKind::kSession: {
        SessionMeta old;
        const auto old_it = sessions_.find(info.session_id);
        const bool had_old = old_it != sessions_.end();
        if (had_old) {
            old = old_it->second;
            old_cost += old.encoded_size;
        }
        SessionMeta next;
        next.status = record.status;
        next.revision = record.revision;
        next.last_fence = record.last_fence;
        next.created_at_ms = record.created_at_ms;
        next.updated_at_ms = record.updated_at_ms;
        next.session_expires_at_ms = session_payload.session_expires_at_ms;
        next.lease_expires_at_ms = record.lease_expires_at_ms;
        next.encoded_size = value.size();
        next.tool_count = had_old ? old.tool_count : 0;
        next.session_deadline =
            deadline_from_absolute(next.session_expires_at_ms);
        if (next.lease_expires_at_ms > 0) {
            next.lease_deadline =
                deadline_from_absolute(next.lease_expires_at_ms);
        }
        sessions_[info.session_id] = std::move(next);
        observe_session_transition(had_old ? &old : nullptr,
                                   &sessions_[info.session_id]);
        break;
    }
    case C::KeyKind::kTool: {
        ToolMeta old;
        const auto old_it = tools_.find(owned_key);
        const bool had_old = old_it != tools_.end();
        if (had_old) {
            old = old_it->second;
            old_cost += old.encoded_size;
        }
        const auto session_it = sessions_.find(info.session_id);
        if (session_it == sessions_.end()) {
            set_error(error, "tool parent session does not exist");
            return false;
        }
        if (!had_old) {
            ++session_it->second.tool_count;
        }
        ToolMeta next;
        next.session_id = info.session_id;
        next.tool_id = info.tool_id;
        next.status = record.status;
        next.revision = record.revision;
        next.last_fence = record.last_fence;
        next.created_at_ms = record.created_at_ms;
        next.updated_at_ms = record.updated_at_ms;
        next.lease_expires_at_ms = record.lease_expires_at_ms;
        next.encoded_size = value.size();
        if (next.lease_expires_at_ms > 0) {
            next.lease_deadline =
                deadline_from_absolute(next.lease_expires_at_ms);
        }
        tools_[owned_key] = std::move(next);
        observe_tool_transition(had_old ? &old : nullptr, &tools_[owned_key]);
        break;
    }
    case C::KeyKind::kIdempotency: {
        const auto old_it = idempotency_.find(owned_key);
        if (old_it != idempotency_.end()) {
            old_cost += old_it->second.encoded_size;
        }
        C::Response response;
        if (!C::decode_response(idempotency_payload.response, &response,
                                error)) {
            return false;
        }
        IdempotencyMeta next;
        next.request_hash = idempotency_payload.request_hash;
        next.canonical_request_len = idempotency_payload.canonical_request_len;
        next.expires_at_ms = idempotency_payload.expires_at_ms;
        next.encoded_size = value.size();
        next.response_size = idempotency_payload.response.size();
        next.deadline = deadline_from_absolute(next.expires_at_ms);
        idempotency_[owned_key] = next;
        break;
    }
    case C::KeyKind::kGeneric: {
        const auto old_it = generic_.find(owned_key);
        if (old_it != generic_.end()) {
            old_cost += old_it->second.encoded_size;
        }
        GenericMeta next;
        next.revision = record.revision;
        next.created_at_ms = record.created_at_ms;
        next.updated_at_ms = record.updated_at_ms;
        next.encoded_size = value.size();
        generic_[owned_key] = next;
        break;
    }
    case C::KeyKind::kMarker:
        break;
    }

    key_sizes_[owned_key] = key.size();
    if (new_cost >= old_cost) {
        stats_.logical_bytes =
            saturating_add_u64(stats_.logical_bytes, new_cost - old_cost);
    } else {
        stats_.logical_bytes -= old_cost - new_cost;
    }
    return true;
}

bool AgentStateStore::apply_delete(std::string_view key, std::string* error) {
    C::KeyInfo info;
    if (!C::decode_key(key, &info, error)) {
        return false;
    }
    const std::string owned_key(key);
    std::uint64_t cost = 0;
    const auto size_it = key_sizes_.find(owned_key);
    if (size_it != key_sizes_.end()) {
        cost += size_it->second;
    }
    switch (info.kind) {
    case C::KeyKind::kMarker:
        set_error(error, "agent marker cannot be deleted");
        return false;
    case C::KeyKind::kSession: {
        const auto it = sessions_.find(info.session_id);
        if (it == sessions_.end()) {
            return true;
        }
        if (it->second.tool_count != 0) {
            set_error(error, "cannot delete a session with live tool records");
            return false;
        }
        SessionMeta old = it->second;
        cost += old.encoded_size;
        sessions_.erase(it);
        observe_session_transition(&old, nullptr);
        break;
    }
    case C::KeyKind::kTool: {
        const auto it = tools_.find(owned_key);
        if (it == tools_.end()) {
            return true;
        }
        ToolMeta old = it->second;
        cost += old.encoded_size;
        const auto session_it = sessions_.find(old.session_id);
        if (session_it != sessions_.end() &&
            session_it->second.tool_count > 0) {
            --session_it->second.tool_count;
        }
        tools_.erase(it);
        observe_tool_transition(&old, nullptr);
        break;
    }
    case C::KeyKind::kIdempotency: {
        const auto it = idempotency_.find(owned_key);
        if (it == idempotency_.end()) {
            return true;
        }
        cost += it->second.encoded_size;
        idempotency_.erase(it);
        break;
    }
    case C::KeyKind::kGeneric: {
        const auto it = generic_.find(owned_key);
        if (it == generic_.end()) {
            return true;
        }
        cost += it->second.encoded_size;
        generic_.erase(it);
        break;
    }
    }
    key_sizes_.erase(owned_key);
    stats_.logical_bytes =
        cost > stats_.logical_bytes ? 0 : stats_.logical_bytes - cost;
    return true;
}

namespace {

bool encode_session_set(std::string key, std::uint8_t status,
                        std::uint64_t revision, std::uint64_t fence,
                        std::int64_t created_at_ms, std::int64_t updated_at_ms,
                        std::int64_t lease_expires_at_ms,
                        std::int64_t session_expires_at_ms, std::string state,
                        WalTxnOp* operation, std::string* error) {
    C::SessionPayload payload;
    payload.session_expires_at_ms = session_expires_at_ms;
    payload.state = std::move(state);
    std::string encoded_payload;
    if (!C::encode_session_payload(payload, &encoded_payload, error)) {
        return false;
    }
    C::Record record;
    record.type = C::RecordType::kSession;
    record.status = status;
    record.reserved = 0;
    record.revision = revision;
    record.last_fence = fence;
    record.created_at_ms = created_at_ms;
    record.updated_at_ms = updated_at_ms;
    record.lease_expires_at_ms = lease_expires_at_ms;
    record.payload = std::move(encoded_payload);
    std::string encoded_record;
    if (!C::encode_record(record, &encoded_record, error)) {
        return false;
    }
    operation->op = WalOp::kSet;
    operation->key = std::move(key);
    operation->value = std::move(encoded_record);
    operation->expiry_ms = 0;
    return true;
}

bool encode_tool_set(std::string key, std::uint8_t status,
                     std::uint64_t revision, std::uint64_t fence,
                     std::int64_t created_at_ms, std::int64_t updated_at_ms,
                     const std::string& tool_id, const std::string& name,
                     const std::string& request, std::string result,
                     WalTxnOp* operation, std::string* error) {
    C::ToolPayload payload;
    payload.tool_id = tool_id;
    payload.name = name;
    payload.request = request;
    payload.result = std::move(result);
    std::string encoded_payload;
    if (!C::encode_tool_payload(payload, &encoded_payload, error)) {
        return false;
    }
    C::Record record;
    record.type = C::RecordType::kTool;
    record.status = status;
    record.reserved = 0;
    record.revision = revision;
    record.last_fence = fence;
    record.created_at_ms = created_at_ms;
    record.updated_at_ms = updated_at_ms;
    record.lease_expires_at_ms = 0;
    record.payload = std::move(encoded_payload);
    std::string encoded_record;
    if (!C::encode_record(record, &encoded_record, error)) {
        return false;
    }
    operation->op = WalOp::kSet;
    operation->key = std::move(key);
    operation->value = std::move(encoded_record);
    operation->expiry_ms = 0;
    return true;
}

bool encode_generic_set(std::string key, std::uint64_t revision,
                        std::int64_t created_at_ms, std::int64_t updated_at_ms,
                        std::string value, WalTxnOp* operation,
                        std::string* error) {
    C::Record record;
    record.type = C::RecordType::kGeneric;
    record.status = 0;
    record.reserved = 0;
    record.revision = revision;
    record.last_fence = 0;
    record.created_at_ms = created_at_ms;
    record.updated_at_ms = updated_at_ms;
    record.lease_expires_at_ms = 0;
    record.payload = std::move(value);
    std::string encoded_record;
    if (!C::encode_record(record, &encoded_record, error)) {
        return false;
    }
    operation->op = WalOp::kSet;
    operation->key = std::move(key);
    operation->value = std::move(encoded_record);
    operation->expiry_ms = 0;
    return true;
}

bool encode_marker_set(WalTxnOp* operation) {
    operation->op = WalOp::kSet;
    operation->key = C::marker_key();
    operation->value = C::encode_marker();
    operation->expiry_ms = 0;
    return true;
}

C::ResponseStatus next_revision(std::uint64_t current, std::uint64_t* next) {
    if (current == std::numeric_limits<std::uint64_t>::max()) {
        return C::ResponseStatus::kCapacity;
    }
    *next = current + 1;
    return C::ResponseStatus::kOk;
}

bool append_delete(std::vector<WalTxnOp>* operations, std::string key) {
    WalTxnOp operation;
    operation.op = WalOp::kDel;
    operation.key = std::move(key);
    operation.expiry_ms = 0;
    operations->push_back(std::move(operation));
    return true;
}

} // namespace

bool AgentStateStore::prepare_mutation(const AgentRequest& request,
                                       KvStore& store,
                                       std::uint64_t assigned_lsn,
                                       MutationPlan* plan, std::string* error) {
    if (plan == nullptr) {
        set_error(error, "agent mutation plan is null");
        return false;
    }
    if (assigned_lsn == 0) {
        set_error(error, "agent mutation requires a non-zero assigned LSN");
        return false;
    }
    if (!is_mutation(request.command)) {
        set_error(error, "agent command is not a mutation");
        return false;
    }
    if (!C::is_valid_idempotency_key(request.idempotency_key)) {
        set_error(error, "agent mutation requires a valid idempotency key");
        return false;
    }

    plan->disposition = MutationPlanDisposition::kImmediate;
    plan->operations.clear();
    plan->encoded_response.clear();
    plan->reservation_id = 0;

    const auto reject = [&](C::ResponseStatus status, std::uint64_t revision,
                            std::uint64_t fence) {
        encode_immediate(plan, status, revision, fence);
        return true;
    };
    const auto reject_resource = [&](std::uint64_t revision,
                                     std::uint64_t fence) {
        return reject(C::ResponseStatus::kResourceLimit, revision, fence);
    };
    const auto reject_lease = [&](std::uint64_t revision, std::uint64_t fence) {
        ++stats_.lease_expiries;
        return reject(kLeaseExpired, revision, fence);
    };
    const auto reject_fence = [&](std::uint64_t revision, std::uint64_t fence) {
        ++stats_.fenced_rejections;
        return reject(kFenced, revision, fence);
    };

    AgentRecordCodec::Response replay;
    bool matched = false;
    bool conflict = false;
    if (!check_idempotency(request, store, &replay, &matched, &conflict,
                           error)) {
        return false;
    }
    if (conflict) {
        return reject(kIdempotencyConflict, 0, 0);
    }
    if (matched) {
        std::string encoded;
        if (!C::encode_response(replay, &encoded, error)) {
            return false;
        }
        plan->encoded_response = std::move(encoded);
        return true;
    }

    const std::int64_t now_ms = now_wall_ms();
    const TimePoint now = now_monotonic();

    auto finalize = [&](C::ResponseStatus status, std::uint64_t revision,
                        std::uint64_t fence) -> bool {
        if (plan->operations.empty()) {
            set_error(error, "agent mutation produced no operations");
            return false;
        }
        if (!marker_present_) {
            plan->operations.insert(plan->operations.begin(), WalTxnOp{});
            encode_marker_set(&plan->operations.front());
        }
        if (!encode_success(plan, status, revision, fence, assigned_lsn,
                            error)) {
            return false;
        }
        WalTxnOp idempotency_operation;
        if (!build_idempotency_operation(request, plan->encoded_response,
                                         now_ms, &idempotency_operation,
                                         error)) {
            return false;
        }
        plan->operations.push_back(std::move(idempotency_operation));

        std::uint64_t reservation_id = 0;
        C::ResponseStatus failure = C::ResponseStatus::kResourceLimit;
        std::string reserve_error;
        if (!reserve_operations(plan->operations, &reservation_id, &failure,
                                &reserve_error)) {
            plan->operations.clear();
            plan->reservation_id = 0;
            encode_immediate(plan, failure, revision, fence);
            return true;
        }
        plan->disposition = MutationPlanDisposition::kWal;
        plan->reservation_id = reservation_id;
        return true;
    };

    if (request.command == AgentCommand::kSessionCreate) {
        if (!valid_session_ttl(request.ttl_ms)) {
            set_error(error, "SESSION_CREATE: invalid session TTL");
            return false;
        }
        std::string key;
        if (!C::session_key(request.session_id, &key, error)) {
            return false;
        }
        C::Record existing_record;
        C::SessionPayload existing_payload;
        if (!load_session(store, request.session_id, &existing_record,
                          &existing_payload, error)) {
            return false;
        }
        if (existing_record.revision != 0) {
            return reject(kAlreadyExists, existing_record.revision,
                          existing_record.last_fence);
        }
        // A physically retained but logically expired session cannot be
        // reused because old tool records may still occupy its namespace.
        const auto physical = sessions_.find(request.session_id);
        if (physical != sessions_.end()) {
            return reject(kAlreadyExists, physical->second.revision,
                          physical->second.last_fence);
        }
        WalTxnOp session_operation;
        const std::int64_t session_expires =
            saturating_add_i64(now_ms, request.ttl_ms);
        if (!encode_session_set(
                key, static_cast<std::uint8_t>(C::SessionStatus::kActive), 1, 0,
                now_ms, now_ms, 0, session_expires, request.state,
                &session_operation, error)) {
            return reject_resource(0, 0);
        }
        plan->operations.push_back(std::move(session_operation));
        return finalize(kCreated, 1, 0);
    }

    if (request.command == AgentCommand::kSessionUpdate) {
        if (!valid_session_ttl(request.ttl_ms)) {
            set_error(error, "SESSION_UPDATE: invalid session TTL");
            return false;
        }
        C::Record record;
        C::SessionPayload payload;
        if (!load_session(store, request.session_id, &record, &payload,
                          error)) {
            return false;
        }
        if (record.revision == 0) {
            return reject(kNotFound, 0, 0);
        }
        const TimePoint session_deadline =
            deadline_from_absolute(payload.session_expires_at_ms);
        if (now >= session_deadline) {
            return reject(kNotFound, record.revision, record.last_fence);
        }
        if (request.expected_revision == 0) {
            return reject(kAlreadyExists, record.revision, record.last_fence);
        }
        if (request.expected_revision != record.revision) {
            ++stats_.revision_mismatches;
            return reject(kVersionMismatch, record.revision, record.last_fence);
        }
        if (!C::valid_session_status(record.status) ||
            C::session_terminal(record.status)) {
            return reject(kInvalidState, record.revision, record.last_fence);
        }
        std::uint64_t next = 0;
        C::ResponseStatus revision_status =
            next_revision(record.revision, &next);
        if (revision_status != kOk) {
            return reject(revision_status, record.revision, record.last_fence);
        }
        const std::int64_t new_session_expiry =
            saturating_add_i64(now_ms, request.ttl_ms);
        const bool live_lease =
            record.lease_expires_at_ms > 0 &&
            now < deadline_from_absolute(record.lease_expires_at_ms);
        if (live_lease && new_session_expiry < record.lease_expires_at_ms) {
            return reject(kInvalidState, record.revision, record.last_fence);
        }
        std::string key;
        if (!C::session_key(request.session_id, &key, error)) {
            return false;
        }
        WalTxnOp operation;
        if (!encode_session_set(key, request.target_status, next,
                                record.last_fence, record.created_at_ms, now_ms,
                                record.lease_expires_at_ms, new_session_expiry,
                                request.state, &operation, error)) {
            return reject_resource(record.revision, record.last_fence);
        }
        plan->operations.push_back(std::move(operation));
        return finalize(kOk, next, record.last_fence);
    }

    if (request.command == AgentCommand::kSessionClose) {
        C::Record record;
        C::SessionPayload payload;
        if (!load_session(store, request.session_id, &record, &payload,
                          error)) {
            return false;
        }
        if (record.revision == 0) {
            return reject(kNotFound, 0, 0);
        }
        if (now >= deadline_from_absolute(payload.session_expires_at_ms)) {
            return reject(kNotFound, record.revision, record.last_fence);
        }
        if (request.expected_revision == 0) {
            return reject(kAlreadyExists, record.revision, record.last_fence);
        }
        if (request.expected_revision != record.revision) {
            ++stats_.revision_mismatches;
            return reject(kVersionMismatch, record.revision, record.last_fence);
        }
        if (C::session_terminal(record.status)) {
            return reject(kInvalidState, record.revision, record.last_fence);
        }
        std::uint64_t next = 0;
        C::ResponseStatus revision_status =
            next_revision(record.revision, &next);
        if (revision_status != kOk) {
            return reject(revision_status, record.revision, record.last_fence);
        }
        std::string key;
        if (!C::session_key(request.session_id, &key, error)) {
            return false;
        }
        WalTxnOp operation;
        if (!encode_session_set(key, request.target_status, next,
                                record.last_fence, record.created_at_ms, now_ms,
                                0, payload.session_expires_at_ms, payload.state,
                                &operation, error)) {
            return reject_resource(record.revision, record.last_fence);
        }
        plan->operations.push_back(std::move(operation));
        return finalize(kOk, next, record.last_fence);
    }

    if (request.command == AgentCommand::kSessionPurge) {
        C::Record record;
        C::SessionPayload payload;
        if (!load_session(store, request.session_id, &record, &payload,
                          error)) {
            return false;
        }
        if (record.revision == 0) {
            return reject(kNotFound, 0, 0);
        }
        const bool logically_expired =
            now >= deadline_from_absolute(payload.session_expires_at_ms);
        if (request.expected_revision == 0) {
            return reject(kAlreadyExists, record.revision, record.last_fence);
        }
        if (request.expected_revision != record.revision) {
            ++stats_.revision_mismatches;
            return reject(kVersionMismatch, record.revision, record.last_fence);
        }
        if (!C::session_terminal(record.status) && !logically_expired) {
            return reject(kInvalidState, record.revision, record.last_fence);
        }
        const bool live_lease =
            record.lease_expires_at_ms > 0 &&
            now < deadline_from_absolute(record.lease_expires_at_ms);
        if (live_lease) {
            return reject(kLeaseHeld, record.revision, record.last_fence);
        }
        std::string session_key;
        if (!C::session_key(request.session_id, &session_key, error)) {
            return false;
        }
        for (const auto& [key, tool] : tools_) {
            if (tool.session_id == request.session_id) {
                append_delete(&plan->operations, key);
            }
        }
        append_delete(&plan->operations, std::move(session_key));
        return finalize(kOk, record.revision, record.last_fence);
    }

    if (request.command == AgentCommand::kToolCreate) {
        C::Record session_record;
        C::SessionPayload session_payload;
        if (!load_session(store, request.session_id, &session_record,
                          &session_payload, error)) {
            return false;
        }
        if (session_record.revision == 0 ||
            now >=
                deadline_from_absolute(session_payload.session_expires_at_ms)) {
            return reject(kNotFound, 0, 0);
        }
        C::Record tool_record;
        C::ToolPayload tool_payload;
        if (!load_tool(store, request.session_id, request.tool_id, &tool_record,
                       &tool_payload, error)) {
            return false;
        }
        if (tool_record.revision != 0) {
            return reject(kAlreadyExists, tool_record.revision,
                          session_record.last_fence);
        }
        if (!C::valid_session_status(session_record.status) ||
            C::session_terminal(session_record.status)) {
            return reject(kInvalidState, session_record.revision,
                          session_record.last_fence);
        }
        std::uint64_t next_session_revision = 0;
        C::ResponseStatus revision_status =
            next_revision(session_record.revision, &next_session_revision);
        if (revision_status != kOk) {
            return reject(revision_status, session_record.revision,
                          session_record.last_fence);
        }
        std::string session_key;
        std::string tool_key;
        if (!C::session_key(request.session_id, &session_key, error) ||
            !C::tool_key(request.session_id, request.tool_id, &tool_key,
                         error)) {
            return false;
        }
        WalTxnOp session_operation;
        if (!encode_session_set(
                session_key, session_record.status, next_session_revision,
                session_record.last_fence, session_record.created_at_ms, now_ms,
                session_record.lease_expires_at_ms,
                session_payload.session_expires_at_ms, session_payload.state,
                &session_operation, error)) {
            return reject_resource(session_record.revision,
                                   session_record.last_fence);
        }
        WalTxnOp tool_operation;
        if (!encode_tool_set(
                tool_key, static_cast<std::uint8_t>(C::ToolStatus::kPending), 1,
                0, now_ms, now_ms, request.tool_id, request.tool_name,
                request.value, {}, &tool_operation, error)) {
            return reject_resource(session_record.revision,
                                   session_record.last_fence);
        }
        plan->operations.push_back(std::move(session_operation));
        plan->operations.push_back(std::move(tool_operation));
        return finalize(kCreated, 1, session_record.last_fence);
    }

    if (request.command == AgentCommand::kToolClaim) {
        C::Record session_record;
        C::SessionPayload session_payload;
        if (!load_session(store, request.session_id, &session_record,
                          &session_payload, error)) {
            return false;
        }
        if (session_record.revision == 0 ||
            now >=
                deadline_from_absolute(session_payload.session_expires_at_ms)) {
            return reject(kNotFound, 0, 0);
        }
        C::Record tool_record;
        C::ToolPayload tool_payload;
        if (!load_tool(store, request.session_id, request.tool_id, &tool_record,
                       &tool_payload, error)) {
            return false;
        }
        if (tool_record.revision == 0) {
            return reject(kNotFound, 0, session_record.last_fence);
        }
        if (request.expected_revision == 0) {
            return reject(kAlreadyExists, tool_record.revision,
                          session_record.last_fence);
        }
        if (request.expected_revision != tool_record.revision) {
            ++stats_.revision_mismatches;
            return reject(kVersionMismatch, tool_record.revision,
                          session_record.last_fence);
        }
        if (!C::valid_session_status(session_record.status) ||
            C::session_terminal(session_record.status) ||
            tool_record.status !=
                static_cast<std::uint8_t>(C::ToolStatus::kPending)) {
            return reject(kInvalidState, tool_record.revision,
                          session_record.last_fence);
        }
        const bool live_lease =
            session_record.lease_expires_at_ms > 0 &&
            now < deadline_from_absolute(session_record.lease_expires_at_ms);
        if (!live_lease) {
            return reject_lease(tool_record.revision,
                                session_record.last_fence);
        }
        if (request.fence != session_record.last_fence) {
            return reject_fence(tool_record.revision,
                                session_record.last_fence);
        }
        std::uint64_t next_tool_revision = 0;
        std::uint64_t next_session_revision = 0;
        C::ResponseStatus revision_status =
            next_revision(tool_record.revision, &next_tool_revision);
        if (revision_status != kOk) {
            return reject(revision_status, tool_record.revision,
                          session_record.last_fence);
        }
        revision_status =
            next_revision(session_record.revision, &next_session_revision);
        if (revision_status != kOk) {
            return reject(revision_status, tool_record.revision,
                          session_record.last_fence);
        }
        const std::int64_t requested_lease_expiry =
            saturating_add_i64(now_ms, request.ttl_ms);
        const std::int64_t next_lease_expiry = std::max(
            session_record.lease_expires_at_ms, requested_lease_expiry);
        if (next_lease_expiry > session_payload.session_expires_at_ms) {
            return reject(kInvalidState, tool_record.revision,
                          session_record.last_fence);
        }
        std::string session_key;
        std::string tool_key;
        if (!C::session_key(request.session_id, &session_key, error) ||
            !C::tool_key(request.session_id, request.tool_id, &tool_key,
                         error)) {
            return false;
        }
        WalTxnOp session_operation;
        if (!encode_session_set(
                session_key, session_record.status, next_session_revision,
                session_record.last_fence, session_record.created_at_ms, now_ms,
                next_lease_expiry, session_payload.session_expires_at_ms,
                session_payload.state, &session_operation, error)) {
            return reject_resource(tool_record.revision,
                                   session_record.last_fence);
        }
        WalTxnOp tool_operation;
        if (!encode_tool_set(tool_key,
                             static_cast<std::uint8_t>(C::ToolStatus::kRunning),
                             next_tool_revision, session_record.last_fence,
                             tool_record.created_at_ms, now_ms, request.tool_id,
                             tool_payload.name, tool_payload.request, {},
                             &tool_operation, error)) {
            return reject_resource(tool_record.revision,
                                   session_record.last_fence);
        }
        plan->operations.push_back(std::move(session_operation));
        plan->operations.push_back(std::move(tool_operation));
        return finalize(kOk, next_tool_revision, session_record.last_fence);
    }

    if (request.command == AgentCommand::kToolComplete ||
        request.command == AgentCommand::kToolRequeue) {
        const bool completing = request.command == AgentCommand::kToolComplete;
        C::Record session_record;
        C::SessionPayload session_payload;
        if (!load_session(store, request.session_id, &session_record,
                          &session_payload, error)) {
            return false;
        }
        if (session_record.revision == 0 ||
            now >=
                deadline_from_absolute(session_payload.session_expires_at_ms)) {
            return reject(kNotFound, 0, 0);
        }
        C::Record tool_record;
        C::ToolPayload tool_payload;
        if (!load_tool(store, request.session_id, request.tool_id, &tool_record,
                       &tool_payload, error)) {
            return false;
        }
        if (tool_record.revision == 0) {
            return reject(kNotFound, 0, session_record.last_fence);
        }
        if (request.expected_revision == 0) {
            return reject(kAlreadyExists, tool_record.revision,
                          session_record.last_fence);
        }
        if (request.expected_revision != tool_record.revision) {
            ++stats_.revision_mismatches;
            return reject(kVersionMismatch, tool_record.revision,
                          session_record.last_fence);
        }
        if (!C::valid_session_status(session_record.status) ||
            C::session_terminal(session_record.status) ||
            tool_record.status !=
                static_cast<std::uint8_t>(C::ToolStatus::kRunning)) {
            return reject(kInvalidState, tool_record.revision,
                          session_record.last_fence);
        }
        const bool live_lease =
            session_record.lease_expires_at_ms > 0 &&
            now < deadline_from_absolute(session_record.lease_expires_at_ms);
        if (!live_lease) {
            return reject_lease(tool_record.revision,
                                session_record.last_fence);
        }
        if (request.fence != session_record.last_fence) {
            return reject_fence(tool_record.revision,
                                session_record.last_fence);
        }
        std::uint64_t next_tool_revision = 0;
        std::uint64_t next_session_revision = 0;
        C::ResponseStatus revision_status =
            next_revision(tool_record.revision, &next_tool_revision);
        if (revision_status != kOk) {
            return reject(revision_status, tool_record.revision,
                          session_record.last_fence);
        }
        revision_status =
            next_revision(session_record.revision, &next_session_revision);
        if (revision_status != kOk) {
            return reject(revision_status, tool_record.revision,
                          session_record.last_fence);
        }
        const std::uint8_t next_tool_status =
            completing ? request.target_status
                       : static_cast<std::uint8_t>(C::ToolStatus::kPending);
        std::string session_key;
        std::string tool_key;
        if (!C::session_key(request.session_id, &session_key, error) ||
            !C::tool_key(request.session_id, request.tool_id, &tool_key,
                         error)) {
            return false;
        }
        WalTxnOp session_operation;
        if (!encode_session_set(
                session_key, session_record.status, next_session_revision,
                session_record.last_fence, session_record.created_at_ms, now_ms,
                session_record.lease_expires_at_ms,
                session_payload.session_expires_at_ms, session_payload.state,
                &session_operation, error)) {
            return reject_resource(tool_record.revision,
                                   session_record.last_fence);
        }
        WalTxnOp tool_operation;
        if (!encode_tool_set(tool_key, next_tool_status, next_tool_revision,
                             session_record.last_fence,
                             tool_record.created_at_ms, now_ms, request.tool_id,
                             tool_payload.name, tool_payload.request,
                             completing ? request.result : std::string{},
                             &tool_operation, error)) {
            return reject_resource(tool_record.revision,
                                   session_record.last_fence);
        }
        plan->operations.push_back(std::move(session_operation));
        plan->operations.push_back(std::move(tool_operation));
        return finalize(kOk, next_tool_revision, session_record.last_fence);
    }

    if (request.command == AgentCommand::kLeaseAcquire) {
        C::Record record;
        C::SessionPayload payload;
        if (!load_session(store, request.session_id, &record, &payload,
                          error)) {
            return false;
        }
        if (record.revision == 0 ||
            now >= deadline_from_absolute(payload.session_expires_at_ms)) {
            return reject(kNotFound, 0, 0);
        }
        if (!C::valid_session_status(record.status) ||
            C::session_terminal(record.status)) {
            return reject(kInvalidState, record.revision, record.last_fence);
        }
        const bool live_lease =
            record.lease_expires_at_ms > 0 &&
            now < deadline_from_absolute(record.lease_expires_at_ms);
        if (live_lease) {
            return reject(kLeaseHeld, record.revision, record.last_fence);
        }
        if (record.last_fence == std::numeric_limits<std::uint64_t>::max()) {
            return reject(kCapacity, record.revision, record.last_fence);
        }
        std::uint64_t next_revision_value = 0;
        C::ResponseStatus revision_status =
            next_revision(record.revision, &next_revision_value);
        if (revision_status != kOk) {
            return reject(revision_status, record.revision, record.last_fence);
        }
        const std::uint64_t next_fence = record.last_fence + 1;
        const std::int64_t lease_expiry =
            saturating_add_i64(now_ms, request.ttl_ms);
        if (lease_expiry > payload.session_expires_at_ms) {
            return reject(kInvalidState, record.revision, record.last_fence);
        }
        std::string key;
        if (!C::session_key(request.session_id, &key, error)) {
            return false;
        }
        WalTxnOp operation;
        if (!encode_session_set(key, record.status, next_revision_value,
                                next_fence, record.created_at_ms, now_ms,
                                lease_expiry, payload.session_expires_at_ms,
                                payload.state, &operation, error)) {
            return reject_resource(record.revision, record.last_fence);
        }
        plan->operations.push_back(std::move(operation));
        return finalize(kOk, next_revision_value, next_fence);
    }

    if (request.command == AgentCommand::kLeaseRenew) {
        C::Record record;
        C::SessionPayload payload;
        if (!load_session(store, request.session_id, &record, &payload,
                          error)) {
            return false;
        }
        if (record.revision == 0 ||
            now >= deadline_from_absolute(payload.session_expires_at_ms)) {
            return reject(kNotFound, 0, 0);
        }
        if (request.expected_revision == 0) {
            return reject(kAlreadyExists, record.revision, record.last_fence);
        }
        if (request.expected_revision != record.revision) {
            ++stats_.revision_mismatches;
            return reject(kVersionMismatch, record.revision, record.last_fence);
        }
        if (!C::valid_session_status(record.status) ||
            C::session_terminal(record.status)) {
            return reject(kInvalidState, record.revision, record.last_fence);
        }
        const bool live_lease =
            record.lease_expires_at_ms > 0 &&
            now < deadline_from_absolute(record.lease_expires_at_ms);
        if (!live_lease) {
            return reject_lease(record.revision, record.last_fence);
        }
        if (request.fence != record.last_fence) {
            return reject_fence(record.revision, record.last_fence);
        }
        std::uint64_t next_revision_value = 0;
        C::ResponseStatus revision_status =
            next_revision(record.revision, &next_revision_value);
        if (revision_status != kOk) {
            return reject(revision_status, record.revision, record.last_fence);
        }
        const std::int64_t lease_expiry =
            saturating_add_i64(now_ms, request.ttl_ms);
        if (lease_expiry > payload.session_expires_at_ms) {
            return reject(kInvalidState, record.revision, record.last_fence);
        }
        std::string key;
        if (!C::session_key(request.session_id, &key, error)) {
            return false;
        }
        WalTxnOp operation;
        if (!encode_session_set(key, record.status, next_revision_value,
                                record.last_fence, record.created_at_ms, now_ms,
                                lease_expiry, payload.session_expires_at_ms,
                                payload.state, &operation, error)) {
            return reject_resource(record.revision, record.last_fence);
        }
        plan->operations.push_back(std::move(operation));
        return finalize(kOk, next_revision_value, record.last_fence);
    }

    if (request.command == AgentCommand::kLeaseRelease) {
        C::Record record;
        C::SessionPayload payload;
        if (!load_session(store, request.session_id, &record, &payload,
                          error)) {
            return false;
        }
        if (record.revision == 0 ||
            now >= deadline_from_absolute(payload.session_expires_at_ms)) {
            return reject(kNotFound, 0, 0);
        }
        if (request.expected_revision == 0) {
            return reject(kAlreadyExists, record.revision, record.last_fence);
        }
        if (request.expected_revision != record.revision) {
            ++stats_.revision_mismatches;
            return reject(kVersionMismatch, record.revision, record.last_fence);
        }
        if (!C::valid_session_status(record.status) ||
            C::session_terminal(record.status)) {
            return reject(kInvalidState, record.revision, record.last_fence);
        }
        const bool live_lease =
            record.lease_expires_at_ms > 0 &&
            now < deadline_from_absolute(record.lease_expires_at_ms);
        if (!live_lease) {
            return reject_lease(record.revision, record.last_fence);
        }
        if (request.fence != record.last_fence) {
            return reject_fence(record.revision, record.last_fence);
        }
        std::uint64_t next_revision_value = 0;
        C::ResponseStatus revision_status =
            next_revision(record.revision, &next_revision_value);
        if (revision_status != kOk) {
            return reject(revision_status, record.revision, record.last_fence);
        }
        std::string key;
        if (!C::session_key(request.session_id, &key, error)) {
            return false;
        }
        WalTxnOp operation;
        if (!encode_session_set(key, record.status, next_revision_value,
                                record.last_fence, record.created_at_ms, now_ms,
                                0, payload.session_expires_at_ms, payload.state,
                                &operation, error)) {
            return reject_resource(record.revision, record.last_fence);
        }
        plan->operations.push_back(std::move(operation));
        return finalize(kOk, next_revision_value, record.last_fence);
    }

    if (request.command == AgentCommand::kRecordCas) {
        std::string key;
        if (!C::generic_key(request.record_name, &key, error)) {
            return false;
        }
        const auto existing = store.get(key);
        C::Record record;
        std::int64_t created_at_ms = now_ms;
        std::uint64_t next_revision_value = 1;
        C::ResponseStatus final_status = kCreated;
        if (existing) {
            if (!C::decode_typed_record(key, *existing, &record, nullptr,
                                        nullptr, nullptr, error)) {
                return false;
            }
            if (record.type != C::RecordType::kGeneric ||
                record.revision == 0) {
                set_error(error, "invalid committed generic record");
                return false;
            }
            if (request.expected_revision == 0) {
                return reject(kAlreadyExists, record.revision, 0);
            }
            if (request.expected_revision != record.revision) {
                ++stats_.revision_mismatches;
                return reject(kVersionMismatch, record.revision, 0);
            }
            C::ResponseStatus revision_status =
                next_revision(record.revision, &next_revision_value);
            if (revision_status != kOk) {
                return reject(revision_status, record.revision, 0);
            }
            created_at_ms = record.created_at_ms;
            final_status = kOk;
        } else if (request.expected_revision != 0) {
            ++stats_.revision_mismatches;
            return reject(kVersionMismatch, 0, 0);
        }
        WalTxnOp operation;
        if (!encode_generic_set(key, next_revision_value, created_at_ms, now_ms,
                                request.value, &operation, error)) {
            return reject_resource(existing ? request.expected_revision : 0, 0);
        }
        plan->operations.push_back(std::move(operation));
        return finalize(final_status, next_revision_value, 0);
    }

    set_error(error, "unsupported agent mutation");
    return false;
}

std::vector<AgentStateStore::GcCandidate>
AgentStateStore::collect_gc_candidates() const {
    std::vector<GcCandidate> candidates;
    const TimePoint now = now_monotonic();
    const std::int64_t wall_now = now_wall_ms();

    for (const auto& [key, idempotency] : idempotency_) {
        if (now >= idempotency.deadline ||
            wall_now >= idempotency.expires_at_ms) {
            GcCandidate candidate;
            candidate.keys.push_back(key);
            candidates.push_back(std::move(candidate));
        }
    }

    for (const auto& [session_id, session] : sessions_) {
        const bool removable_status = C::session_terminal(session.status) ||
                                      session_expired(session, now);
        if (!removable_status || !lease_expired(session, now)) {
            continue;
        }
        GcCandidate candidate;
        std::string session_key;
        if (!C::session_key(session_id, &session_key, nullptr)) {
            continue;
        }
        for (const auto& [key, tool] : tools_) {
            if (tool.session_id == session_id) {
                candidate.keys.push_back(key);
            }
        }
        candidate.keys.push_back(std::move(session_key));
        candidates.push_back(std::move(candidate));
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const GcCandidate& left, const GcCandidate& right) {
                  return left.keys.front() < right.keys.front();
              });
    return candidates;
}

void AgentStateStore::note_gc_success(std::size_t candidates,
                                      std::size_t deleted_keys,
                                      std::uint64_t elapsed_us) {
    ++stats_.gc_runs;
    stats_.gc_candidates = saturating_add_u64(stats_.gc_candidates, candidates);
    stats_.gc_deleted = saturating_add_u64(stats_.gc_deleted, deleted_keys);
    stats_.gc_total_us = saturating_add_u64(stats_.gc_total_us, elapsed_us);
    stats_.gc_max_us = std::max(stats_.gc_max_us, elapsed_us);
}

void AgentStateStore::note_gc_failure() {
    ++stats_.gc_failures;
}

const char* agent_command_name(AgentCommand command) noexcept {
    switch (command) {
    case AgentCommand::kSessionCreate:
        return "SESSION_CREATE";
    case AgentCommand::kSessionGet:
        return "SESSION_GET";
    case AgentCommand::kSessionUpdate:
        return "SESSION_UPDATE";
    case AgentCommand::kSessionClose:
        return "SESSION_CLOSE";
    case AgentCommand::kSessionPurge:
        return "SESSION_PURGE";
    case AgentCommand::kToolCreate:
        return "TOOL_CREATE";
    case AgentCommand::kToolGet:
        return "TOOL_GET";
    case AgentCommand::kToolClaim:
        return "TOOL_CLAIM";
    case AgentCommand::kToolComplete:
        return "TOOL_COMPLETE";
    case AgentCommand::kToolRequeue:
        return "TOOL_REQUEUE";
    case AgentCommand::kLeaseAcquire:
        return "LEASE_ACQUIRE";
    case AgentCommand::kLeaseRenew:
        return "LEASE_RENEW";
    case AgentCommand::kLeaseRelease:
        return "LEASE_RELEASE";
    case AgentCommand::kRecordGet:
        return "RECORD_GET";
    case AgentCommand::kRecordCas:
        return "RECORD_CAS";
    }
    return "UNKNOWN";
}

const char* agent_request_status_name(AgentRequestStatus status) noexcept {
    switch (status) {
    case AgentRequestStatus::kCompleted:
        return "COMPLETED";
    case AgentRequestStatus::kAccepted:
        return "ACCEPTED";
    case AgentRequestStatus::kInvalidArgument:
        return "INVALID_ARGUMENT";
    case AgentRequestStatus::kNotReady:
        return "NOT_READY";
    case AgentRequestStatus::kShuttingDown:
        return "SHUTTING_DOWN";
    case AgentRequestStatus::kIoError:
        return "IO_ERROR";
    case AgentRequestStatus::kResourceLimit:
        return "RESOURCE_LIMIT";
    case AgentRequestStatus::kConflict:
        return "CONFLICT";
    }
    return "UNKNOWN";
}

bool AgentStateStore::apply_committed(const std::vector<WalTxnOp>& operations,
                                      std::uint64_t lsn,
                                      std::uint64_t reservation_id,
                                      std::string* error) {
    (void)lsn;
    if (reservation_id != 0 &&
        reservations_.find(reservation_id) == reservations_.end()) {
        set_error(error, "agent reservation does not exist");
        return false;
    }

    // Apply to a private copy first.  A malformed or internally inconsistent
    // transaction must never publish a prefix of its index changes.
    AgentStateStore candidate(*this);
    candidate.release_reservation_locked(reservation_id);

    std::set<std::string> keys;
    for (const WalTxnOp& operation : operations) {
        if (operation.op != WalOp::kSet && operation.op != WalOp::kDel) {
            set_error(error, "agent transaction contains an unsupported op");
            return false;
        }
        C::KeyInfo info;
        std::string validation_error;
        if (!C::decode_key(operation.key, &info, &validation_error)) {
            set_error(error, validation_error);
            return false;
        }
        if (!keys.insert(operation.key).second) {
            set_error(error, "agent transaction contains a duplicate key");
            return false;
        }
        if (info.kind == C::KeyKind::kMarker) {
            if (operation.op != WalOp::kSet) {
                set_error(error, "agent marker cannot be deleted");
                return false;
            }
            std::string marker_error;
            if (!C::decode_marker(operation.value, &marker_error)) {
                set_error(error, marker_error);
                return false;
            }
            if (!candidate.apply_set(operation.key, operation.value,
                                     &validation_error)) {
                set_error(error, validation_error);
                return false;
            }
            continue;
        }
        if (operation.op == WalOp::kSet) {
            if (!validate_record_for_apply(operation.key, operation.value,
                                           &info, &validation_error)) {
                set_error(error, validation_error);
                return false;
            }
            if (!candidate.apply_set(operation.key, operation.value,
                                     &validation_error)) {
                set_error(error, validation_error);
                return false;
            }
        } else if (!candidate.apply_delete(operation.key, &validation_error)) {
            set_error(error, validation_error);
            return false;
        }
    }
    candidate.refresh_derived_stats();

    sessions_ = std::move(candidate.sessions_);
    tools_ = std::move(candidate.tools_);
    idempotency_ = std::move(candidate.idempotency_);
    generic_ = std::move(candidate.generic_);
    key_sizes_ = std::move(candidate.key_sizes_);
    reservations_ = std::move(candidate.reservations_);
    reserved_totals_ = candidate.reserved_totals_;
    marker_reservations_ = candidate.marker_reservations_;
    marker_present_ = candidate.marker_present_;
    stats_ = candidate.stats_;
    last_gc_candidates_ = std::move(candidate.last_gc_candidates_);
    return true;
}

bool AgentStateStore::load_session(KvStore& store, std::string_view session_id,
                                   AgentRecordCodec::Record* record,
                                   AgentRecordCodec::SessionPayload* payload,
                                   std::string* error) const {
    if (record == nullptr || payload == nullptr) {
        set_error(error, "session output is null");
        return false;
    }
    *record = AgentRecordCodec::Record{};
    *payload = AgentRecordCodec::SessionPayload{};
    std::string key;
    if (!AgentRecordCodec::session_key(session_id, &key, error)) {
        return false;
    }
    const auto value = store.get(key);
    if (!value) {
        return true;
    }
    AgentRecordCodec::KeyInfo info;
    if (!AgentRecordCodec::decode_key(key, &info, error) ||
        info.kind != AgentRecordCodec::KeyKind::kSession ||
        !AgentRecordCodec::decode_typed_record(key, *value, record, payload,
                                               nullptr, nullptr, error)) {
        return false;
    }
    if (record->revision == 0 ||
        !AgentRecordCodec::valid_session_status(record->status) ||
        payload->session_expires_at_ms <= 0) {
        set_error(error, "invalid committed session record");
        return false;
    }
    return true;
}

bool AgentStateStore::load_tool(KvStore& store, std::string_view session_id,
                                std::string_view tool_id,
                                AgentRecordCodec::Record* record,
                                AgentRecordCodec::ToolPayload* payload,
                                std::string* error) const {
    if (record == nullptr || payload == nullptr) {
        set_error(error, "tool output is null");
        return false;
    }
    *record = AgentRecordCodec::Record{};
    *payload = AgentRecordCodec::ToolPayload{};
    std::string key;
    if (!AgentRecordCodec::tool_key(session_id, tool_id, &key, error)) {
        return false;
    }
    const auto value = store.get(key);
    if (!value) {
        return true;
    }
    AgentRecordCodec::KeyInfo info;
    if (!AgentRecordCodec::decode_key(key, &info, error) ||
        info.kind != AgentRecordCodec::KeyKind::kTool ||
        !AgentRecordCodec::decode_typed_record(key, *value, record, nullptr,
                                               payload, nullptr, error)) {
        return false;
    }
    if (record->revision == 0 ||
        !AgentRecordCodec::valid_tool_status(record->status) ||
        payload->tool_id != tool_id) {
        set_error(error, "invalid committed tool record");
        return false;
    }
    return true;
}

bool AgentStateStore::check_idempotency(const AgentRequest& request,
                                        KvStore& store,
                                        AgentRecordCodec::Response* replay,
                                        bool* matched, bool* conflict,
                                        std::string* error) {
    if (replay == nullptr || matched == nullptr || conflict == nullptr) {
        set_error(error, "idempotency output is null");
        return false;
    }
    *replay = AgentRecordCodec::Response{};
    *matched = false;
    *conflict = false;

    std::string encoded_key;
    if (!AgentRecordCodec::idempotency_key(request.idempotency_key,
                                           &encoded_key, error)) {
        return false;
    }
    const auto value = store.get(encoded_key);
    if (!value) {
        return true;
    }

    AgentRecordCodec::Record record;
    AgentRecordCodec::IdempotencyPayload payload;
    if (!AgentRecordCodec::decode_typed_record(
            encoded_key, *value, &record, nullptr, nullptr, &payload, error)) {
        return false;
    }
    if (record.type != AgentRecordCodec::RecordType::kIdempotency ||
        record.status != 0 || record.revision != 0 || record.last_fence != 0 ||
        record.lease_expires_at_ms != 0 || payload.expires_at_ms <= 0) {
        set_error(error, "invalid committed idempotency record");
        return false;
    }

    const std::int64_t wall_now = now_wall_ms();
    TimePoint deadline = deadline_from_absolute(payload.expires_at_ms);
    const auto indexed = idempotency_.find(encoded_key);
    if (indexed != idempotency_.end()) {
        deadline = indexed->second.deadline;
    }
    if (wall_now >= payload.expires_at_ms || now_monotonic() >= deadline) {
        return true;
    }

    Sha256::Digest hash{};
    std::uint32_t canonical_len = 0;
    if (!AgentRecordCodec::canonical_request_hash(
            request.subcommand, request.args, &hash, &canonical_len, error)) {
        return false;
    }
    const bool same_request = hash == payload.request_hash &&
                              canonical_len == payload.canonical_request_len;
    if (!same_request) {
        ++stats_.idempotency_conflicts;
        *conflict = true;
        return true;
    }

    AgentRecordCodec::Response decoded;
    if (!AgentRecordCodec::decode_response(payload.response, &decoded, error)) {
        return false;
    }
    ++stats_.idempotency_replays;
    *replay = std::move(decoded);
    *matched = true;
    return true;
}

bool AgentStateStore::build_idempotency_operation(
    const AgentRequest& request, const std::string& encoded_response,
    std::int64_t now_ms, WalTxnOp* operation, std::string* error) {
    if (operation == nullptr) {
        set_error(error, "idempotency operation output is null");
        return false;
    }
    Sha256::Digest hash{};
    std::uint32_t canonical_len = 0;
    if (!AgentRecordCodec::canonical_request_hash(
            request.subcommand, request.args, &hash, &canonical_len, error)) {
        return false;
    }

    AgentRecordCodec::IdempotencyPayload payload;
    payload.request_hash = hash;
    payload.expires_at_ms =
        saturating_add_i64(now_ms, request.idempotency_ttl_ms);
    payload.canonical_request_len = canonical_len;
    payload.response = encoded_response;

    std::string encoded_payload;
    if (!AgentRecordCodec::encode_idempotency_payload(payload, &encoded_payload,
                                                      error)) {
        return false;
    }
    AgentRecordCodec::Record record;
    record.type = AgentRecordCodec::RecordType::kIdempotency;
    record.status = 0;
    record.reserved = 0;
    record.revision = 0;
    record.last_fence = 0;
    record.created_at_ms = now_ms;
    record.updated_at_ms = now_ms;
    record.lease_expires_at_ms = 0;
    record.payload = std::move(encoded_payload);

    std::string encoded_record;
    if (!AgentRecordCodec::encode_record(record, &encoded_record, error)) {
        return false;
    }
    operation->op = WalOp::kSet;
    std::string key;
    if (!AgentRecordCodec::idempotency_key(request.idempotency_key, &key,
                                           error)) {
        return false;
    }
    operation->key = std::move(key);
    operation->value = std::move(encoded_record);
    operation->expiry_ms = 0;
    return true;
}

void AgentStateStore::encode_immediate(MutationPlan* plan,
                                       AgentRecordCodec::ResponseStatus status,
                                       std::uint64_t revision,
                                       std::uint64_t fence,
                                       std::string payload) const {
    AgentRecordCodec::Response response;
    response.status = static_cast<std::uint8_t>(status);
    response.flags = 0;
    response.reserved = 0;
    response.revision = revision;
    response.fence = fence;
    response.lsn = 0;
    response.payload = std::move(payload);
    std::string encoded;
    std::string error;
    if (!AgentRecordCodec::encode_response(response, &encoded, &error)) {
        // All fixed status values and empty payloads are encodable.  Keep a
        // conservative fallback so a rejected request still receives a
        // well-formed MDOR envelope if an unexpected allocator failure occurs.
        AgentRecordCodec::Response fallback;
        fallback.status = static_cast<std::uint8_t>(
            AgentRecordCodec::ResponseStatus::kResourceLimit);
        if (AgentRecordCodec::encode_response(fallback, &encoded, &error)) {
            plan->encoded_response = std::move(encoded);
        }
        plan->disposition = MutationPlanDisposition::kImmediate;
        plan->operations.clear();
        plan->reservation_id = 0;
        return;
    }
    plan->encoded_response = std::move(encoded);
    plan->disposition = MutationPlanDisposition::kImmediate;
    plan->operations.clear();
    plan->reservation_id = 0;
}

bool AgentStateStore::encode_success(MutationPlan* plan,
                                     AgentRecordCodec::ResponseStatus status,
                                     std::uint64_t revision,
                                     std::uint64_t fence, std::uint64_t lsn,
                                     std::string* error) const {
    if (plan == nullptr) {
        set_error(error, "mutation plan is null");
        return false;
    }
    AgentRecordCodec::Response response;
    response.status = static_cast<std::uint8_t>(status);
    response.flags = 0;
    response.reserved = 0;
    response.revision = revision;
    response.fence = fence;
    response.lsn = lsn;
    std::string encoded;
    if (!AgentRecordCodec::encode_response(response, &encoded, error)) {
        return false;
    }
    plan->encoded_response = std::move(encoded);
    return true;
}

bool AgentStateStore::prepare_query(const AgentRequest& request, KvStore& store,
                                    AgentRecordCodec::Response* response,
                                    std::string* error) {
    if (response == nullptr) {
        set_error(error, "query response output is null");
        return false;
    }
    *response = AgentRecordCodec::Response{};
    const auto not_found = [&]() {
        response->status = static_cast<std::uint8_t>(
            AgentRecordCodec::ResponseStatus::kNotFound);
        response->revision = 0;
        response->fence = 0;
        response->lsn = 0;
    };

    if (request.command == AgentCommand::kSessionGet) {
        AgentRecordCodec::Record record;
        AgentRecordCodec::SessionPayload payload;
        if (!load_session(store, request.session_id, &record, &payload,
                          error)) {
            return false;
        }
        if (record.revision == 0) {
            not_found();
            return true;
        }
        const TimePoint deadline =
            deadline_from_absolute(payload.session_expires_at_ms);
        if (now_monotonic() >= deadline) {
            not_found();
            return true;
        }
        std::string encoded_record;
        if (!AgentRecordCodec::encode_record(record, &encoded_record, error)) {
            return false;
        }
        response->status =
            static_cast<std::uint8_t>(AgentRecordCodec::ResponseStatus::kOk);
        response->revision = record.revision;
        response->fence = record.last_fence;
        response->payload = std::move(encoded_record);
        return true;
    }

    if (request.command == AgentCommand::kToolGet) {
        AgentRecordCodec::Record record;
        AgentRecordCodec::ToolPayload payload;
        if (!load_tool(store, request.session_id, request.tool_id, &record,
                       &payload, error)) {
            return false;
        }
        if (record.revision == 0) {
            not_found();
            return true;
        }
        AgentRecordCodec::Record parent_record;
        AgentRecordCodec::SessionPayload parent_payload;
        if (!load_session(store, request.session_id, &parent_record,
                          &parent_payload, error)) {
            return false;
        }
        if (parent_record.revision == 0) {
            set_error(error, "tool parent session is missing");
            return false;
        }
        const TimePoint parent_deadline =
            deadline_from_absolute(parent_payload.session_expires_at_ms);
        const bool effective_cancel =
            AgentRecordCodec::session_terminal(parent_record.status) ||
            now_monotonic() >= parent_deadline;
        if (effective_cancel &&
            !AgentRecordCodec::tool_terminal(record.status)) {
            record.status = static_cast<std::uint8_t>(
                AgentRecordCodec::ToolStatus::kCancelled);
        }
        std::string encoded_record;
        if (!AgentRecordCodec::encode_record(record, &encoded_record, error)) {
            return false;
        }
        response->status =
            static_cast<std::uint8_t>(AgentRecordCodec::ResponseStatus::kOk);
        response->revision = record.revision;
        response->fence = parent_record.last_fence;
        response->payload = std::move(encoded_record);
        return true;
    }

    if (request.command == AgentCommand::kRecordGet) {
        std::string key;
        if (!AgentRecordCodec::generic_key(request.record_name, &key, error)) {
            return false;
        }
        const auto value = store.get(key);
        if (!value) {
            not_found();
            return true;
        }
        AgentRecordCodec::Record record;
        if (!AgentRecordCodec::decode_typed_record(
                key, *value, &record, nullptr, nullptr, nullptr, error)) {
            return false;
        }
        if (record.revision == 0 ||
            record.type != AgentRecordCodec::RecordType::kGeneric) {
            set_error(error, "invalid committed generic record");
            return false;
        }
        std::string encoded_record;
        if (!AgentRecordCodec::encode_record(record, &encoded_record, error)) {
            return false;
        }
        response->status =
            static_cast<std::uint8_t>(AgentRecordCodec::ResponseStatus::kOk);
        response->revision = record.revision;
        response->fence = 0;
        response->payload = std::move(encoded_record);
        return true;
    }

    set_error(error, "command is not an Agent query");
    return false;
}

} // namespace minidb
