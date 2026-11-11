#pragma once

#include "../base/Clock.h"
#include "AgentRecordCodec.h"
#include "KvStore.h"
#include "WalFormat.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace minidb {

enum class AgentCommand : std::uint8_t {
    kSessionCreate,
    kSessionGet,
    kSessionUpdate,
    kSessionClose,
    kSessionPurge,
    kToolCreate,
    kToolGet,
    kToolClaim,
    kToolComplete,
    kToolRequeue,
    kLeaseAcquire,
    kLeaseRenew,
    kLeaseRelease,
    kRecordGet,
    kRecordCas,
};

enum class AgentRequestStatus : std::uint8_t {
    kCompleted,
    kAccepted,
    kInvalidArgument,
    kNotReady,
    kShuttingDown,
    kIoError,
    kResourceLimit,
    kConflict,
};

struct AgentRequest {
    AgentCommand command = AgentCommand::kSessionGet;
    std::string subcommand;
    std::vector<std::string> args;

    std::string session_id;
    std::string tool_id;
    std::string tool_name;
    std::string state;
    std::string result;
    std::string idempotency_key;
    std::string record_name;
    std::string value;

    std::uint64_t expected_revision = 0;
    std::uint64_t fence = 0;
    std::int64_t ttl_ms = 0;
    std::int64_t idempotency_ttl_ms =
        AgentRecordCodec::Limits::kDefaultIdempotencyTtlMs;
    std::uint8_t target_status = 0;
};

struct AgentResult {
    AgentRequestStatus status = AgentRequestStatus::kNotReady;
    // A complete MDOR envelope for every successfully parsed application
    // result, including deterministic business rejections and replays.
    std::string encoded_response;
    std::string error;

    bool accepted() const noexcept {
        return status == AgentRequestStatus::kAccepted;
    }
    bool completed() const noexcept {
        return status == AgentRequestStatus::kCompleted;
    }
};

// Owner-loop, single-writer state machine over the reserved Agent namespace.
// Index entries contain only bounded metadata; large MDAR payloads remain in
// KvStore and are fetched only for reads.
class AgentStateStore {
  public:
    struct Stats {
        std::uint64_t sessions_stored = 0;
        std::uint64_t sessions_live = 0;
        std::uint64_t sessions_created = 0;
        std::uint64_t sessions_completed = 0;
        std::uint64_t sessions_failed = 0;
        std::uint64_t sessions_cancelled = 0;
        std::uint64_t sessions_purged = 0;
        std::uint64_t tools_stored = 0;
        std::uint64_t tools_pending = 0;
        std::uint64_t tools_running = 0;
        std::uint64_t tools_succeeded = 0;
        std::uint64_t tools_failed = 0;
        std::uint64_t tools_cancelled = 0;
        std::uint64_t records_stored = 0;
        std::uint64_t idempotency_stored = 0;
        std::uint64_t logical_bytes = 0;
        std::uint64_t logical_bytes_max =
            AgentRecordCodec::Limits::kMaxLogicalBytes;
        std::uint64_t idempotency_replays = 0;
        std::uint64_t idempotency_conflicts = 0;
        std::uint64_t revision_mismatches = 0;
        std::uint64_t lease_acquires = 0;
        std::uint64_t lease_renews = 0;
        std::uint64_t lease_expiries = 0;
        std::uint64_t fenced_rejections = 0;
        std::uint64_t gc_runs = 0;
        std::uint64_t gc_candidates = 0;
        std::uint64_t gc_deleted = 0;
        std::uint64_t gc_failures = 0;
        std::uint64_t gc_total_us = 0;
        std::uint64_t gc_max_us = 0;
    };

    enum class MutationPlanDisposition : std::uint8_t {
        kImmediate,
        kWal,
    };

    struct MutationPlan {
        MutationPlanDisposition disposition =
            MutationPlanDisposition::kImmediate;
        std::vector<WalTxnOp> operations;
        // Complete MDOR bytes. For kWal this is persisted in the idempotency
        // record and returned by the completion callback.
        std::string encoded_response;
        // Non-zero only for kWal plans. The caller must return it through
        // apply_committed() or cancel_reservation() exactly once.
        std::uint64_t reservation_id = 0;
    };

    struct GcCandidate {
        // Complete delete set for one candidate group. Idempotency expiry
        // is one key; a session group includes the session and all tools.
        std::vector<std::string> keys;
    };

    explicit AgentStateStore(std::shared_ptr<ClockInterface> clock = nullptr);

    // Strictly reconstructs the reserved namespace. The store is unchanged
    // on failure. An empty namespace is valid.
    bool rebuild(const std::vector<KvStore::Entry>& entries,
                 std::string* error = nullptr);

    static bool parse_request(std::string_view subcommand,
                              const std::vector<std::string>& args,
                              AgentRequest* request,
                              std::string* error = nullptr);
    static bool is_mutation(AgentCommand command) noexcept;

    // Read-only commands. Returns false only for an internal decode/index
    // inconsistency; NOT_FOUND is a normal encoded MDOR result.
    bool prepare_query(const AgentRequest& request, KvStore& store,
                       AgentRecordCodec::Response* response,
                       std::string* error = nullptr);

    // Builds one bounded Agent mutation. The caller must serialize admission
    // and pass the LSN that will be assigned if this plan commits. Business
    // rejections and idempotency replays return kImmediate and no operations.
    // Successful mutations reserve capacity until apply_committed() or
    // cancel_reservation() is called.
    bool prepare_mutation(const AgentRequest& request, KvStore& store,
                          std::uint64_t assigned_lsn, MutationPlan* plan,
                          std::string* error = nullptr);

    // Publishes metadata only after the corresponding kTxn was durably
    // applied to KvStore. reservation_id is 0 for internal GC transactions.
    bool apply_committed(const std::vector<WalTxnOp>& operations,
                         std::uint64_t lsn, std::uint64_t reservation_id,
                         std::string* error = nullptr);
    void cancel_reservation(std::uint64_t reservation_id) noexcept;

    // Returns deterministic candidate groups ordered by their first key.
    // Selection and the 64-group batch bound live in PersistenceManager so a
    // pending or oversized group cannot starve later groups.
    std::vector<GcCandidate> collect_gc_candidates() const;

    void note_gc_success(std::size_t candidates, std::size_t deleted_keys,
                         std::uint64_t elapsed_us);
    void note_gc_failure();

    Stats stats() const;
    const std::vector<GcCandidate>& last_gc_candidates() const noexcept {
        return last_gc_candidates_;
    }
    void set_last_gc_candidates(std::vector<GcCandidate> candidates) {
        last_gc_candidates_ = std::move(candidates);
    }

  private:
    struct SessionMeta {
        std::uint8_t status = 0;
        std::uint64_t revision = 0;
        std::uint64_t last_fence = 0;
        std::int64_t created_at_ms = 0;
        std::int64_t updated_at_ms = 0;
        std::int64_t session_expires_at_ms = 0;
        std::int64_t lease_expires_at_ms = 0;
        std::size_t encoded_size = 0;
        std::size_t tool_count = 0;
        std::chrono::steady_clock::time_point session_deadline;
        std::chrono::steady_clock::time_point lease_deadline;
    };

    struct ToolMeta {
        std::string session_id;
        std::string tool_id;
        std::uint8_t status = 0;
        std::uint64_t revision = 0;
        std::uint64_t last_fence = 0;
        std::int64_t created_at_ms = 0;
        std::int64_t updated_at_ms = 0;
        std::int64_t lease_expires_at_ms = 0;
        std::size_t encoded_size = 0;
        std::chrono::steady_clock::time_point lease_deadline;
    };

    struct IdempotencyMeta {
        Sha256::Digest request_hash{};
        std::uint32_t canonical_request_len = 0;
        std::int64_t expires_at_ms = 0;
        std::size_t encoded_size = 0;
        std::size_t response_size = 0;
        std::chrono::steady_clock::time_point deadline;
    };

    struct GenericMeta {
        std::uint64_t revision = 0;
        std::int64_t created_at_ms = 0;
        std::int64_t updated_at_ms = 0;
        std::size_t encoded_size = 0;
    };

    struct Reservation {
        std::uint64_t sessions = 0;
        std::uint64_t tools = 0;
        std::uint64_t generic = 0;
        std::uint64_t idempotency = 0;
        std::uint64_t bytes = 0;
        // One per reservation that carries the initial namespace marker.
        std::uint64_t markers = 0;
        std::map<std::string, std::uint64_t> tools_per_session;
    };

    struct PendingIds {
        bool session = false;
        bool tool = false;
        bool generic = false;
        bool idempotency = false;
    };

    using TimePoint = std::chrono::steady_clock::time_point;

    bool load_session(KvStore& store, std::string_view session_id,
                      AgentRecordCodec::Record* record,
                      AgentRecordCodec::SessionPayload* payload,
                      std::string* error) const;
    bool load_tool(KvStore& store, std::string_view session_id,
                   std::string_view tool_id, AgentRecordCodec::Record* record,
                   AgentRecordCodec::ToolPayload* payload,
                   std::string* error) const;

    bool check_idempotency(const AgentRequest& request, KvStore& store,
                           AgentRecordCodec::Response* replay, bool* matched,
                           bool* conflict, std::string* error);
    bool build_idempotency_operation(const AgentRequest& request,
                                     const std::string& encoded_response,
                                     std::int64_t now_ms, WalTxnOp* operation,
                                     std::string* error);

    bool reserve_operations(const std::vector<WalTxnOp>& operations,
                            std::uint64_t* reservation_id,
                            AgentRecordCodec::ResponseStatus* failure,
                            std::string* error);
    void release_reservation_locked(std::uint64_t reservation_id) noexcept;

    void encode_immediate(MutationPlan* plan,
                          AgentRecordCodec::ResponseStatus status,
                          std::uint64_t revision, std::uint64_t fence,
                          std::string payload = {}) const;
    bool encode_success(MutationPlan* plan,
                        AgentRecordCodec::ResponseStatus status,
                        std::uint64_t revision, std::uint64_t fence,
                        std::uint64_t lsn, std::string* error) const;

    bool session_expired(const SessionMeta& session, TimePoint now) const;
    bool lease_expired(const SessionMeta& session, TimePoint now) const;
    TimePoint deadline_from_absolute(std::int64_t absolute_ms) const;
    TimePoint deadline_from_ttl(std::int64_t ttl_ms) const;
    std::int64_t now_wall_ms() const;
    TimePoint now_monotonic() const;

    void refresh_derived_stats();
    void observe_session_transition(const SessionMeta* before,
                                    const SessionMeta* after);
    void observe_tool_transition(const ToolMeta* before, const ToolMeta* after);

    bool apply_set(std::string_view key, std::string_view value,
                   std::string* error);
    bool apply_delete(std::string_view key, std::string* error);
    bool validate_record_for_apply(std::string_view key, std::string_view value,
                                   AgentRecordCodec::KeyInfo* key_info,
                                   std::string* error) const;

    std::shared_ptr<ClockInterface> clock_;
    std::map<std::string, SessionMeta> sessions_;
    // Keyed by the complete reserved tool key so deletion and replay remain
    // byte-exact without reconstructing names.
    std::map<std::string, ToolMeta> tools_;
    std::map<std::string, IdempotencyMeta> idempotency_;
    std::map<std::string, GenericMeta> generic_;
    std::map<std::string, std::size_t> key_sizes_;
    std::map<std::uint64_t, Reservation> reservations_;
    std::uint64_t next_reservation_id_ = 1;
    Reservation reserved_totals_;
    std::uint64_t marker_reservations_ = 0;
    bool marker_present_ = false;
    Stats stats_;
    std::vector<GcCandidate> last_gc_candidates_;
};

const char* agent_command_name(AgentCommand command) noexcept;
const char* agent_request_status_name(AgentRequestStatus status) noexcept;

} // namespace minidb
