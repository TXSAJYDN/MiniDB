#include "../src/storage/AgentStateStore.h"

#include "../src/base/Clock.h"
#include "../src/storage/AgentRecordCodec.h"
#include "../src/storage/KvStore.h"

#include <atomic>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace minidb {
namespace {

using C = AgentRecordCodec;

class TestClock final : public ClockInterface {
  public:
    TestClock() : monotonic_ms_(1'000'000), wall_ms_(1'760'000'000'000LL) {}

    std::chrono::steady_clock::time_point now() const override {
        return Clock::from_ms(monotonic_ms_.load(std::memory_order_relaxed));
    }

    std::int64_t system_now_ms() const override {
        return wall_ms_.load(std::memory_order_relaxed);
    }

    void advance(std::int64_t milliseconds) {
        monotonic_ms_.fetch_add(milliseconds, std::memory_order_relaxed);
        wall_ms_.fetch_add(milliseconds, std::memory_order_relaxed);
    }

  private:
    std::atomic<std::int64_t> monotonic_ms_;
    std::atomic<std::int64_t> wall_ms_;
};

class AgentStateStoreTest : public ::testing::Test {
  protected:
    void SetUp() override {
        clock_ = std::make_shared<TestClock>();
        store_ = std::make_shared<KvStore>(clock_);
        state_ = std::make_shared<AgentStateStore>(clock_);
        const std::vector<KvStore::Entry> empty;
        std::string error;
        ASSERT_TRUE(state_->rebuild(empty, &error)) << error;
    }

    bool mutate(const std::vector<std::string>& args, C::Response* response,
                std::string* error) {
        AgentRequest request;
        if (!AgentStateStore::parse_request(
                args.front(),
                std::vector<std::string>(args.begin() + 1, args.end()),
                &request, error)) {
            return false;
        }
        AgentStateStore::MutationPlan plan;
        if (!state_->prepare_mutation(request, *store_, next_lsn_, &plan,
                                      error)) {
            return false;
        }
        if (plan.disposition ==
            AgentStateStore::MutationPlanDisposition::kImmediate) {
            return C::decode_response(plan.encoded_response, response, error);
        }

        std::vector<KvStore::AppliedMutation> applied;
        applied.reserve(plan.operations.size());
        for (const WalTxnOp& operation : plan.operations) {
            KvStore::AppliedMutation mutation;
            mutation.kind = operation.op == WalOp::kDel
                                ? KvStore::AppliedMutation::Kind::kDelete
                                : KvStore::AppliedMutation::Kind::kSet;
            mutation.key = operation.key;
            mutation.value = operation.value;
            mutation.absolute_expiry_ms = operation.expiry_ms;
            applied.push_back(std::move(mutation));
        }
        const KvStore::ApplyStatus status =
            store_->apply_committed_status(next_lsn_, applied, error);
        if (status != KvStore::ApplyStatus::kOk) {
            state_->cancel_reservation(plan.reservation_id);
            return false;
        }
        if (!state_->apply_committed(plan.operations, next_lsn_,
                                     plan.reservation_id, error)) {
            return false;
        }
        ++next_lsn_;
        return C::decode_response(plan.encoded_response, response, error);
    }

    bool query(const std::vector<std::string>& args, C::Response* response,
               std::string* error) {
        AgentRequest request;
        if (!AgentStateStore::parse_request(
                args.front(),
                std::vector<std::string>(args.begin() + 1, args.end()),
                &request, error)) {
            return false;
        }
        return state_->prepare_query(request, *store_, response, error);
    }

    std::shared_ptr<TestClock> clock_;
    std::shared_ptr<KvStore> store_;
    std::shared_ptr<AgentStateStore> state_;
    std::uint64_t next_lsn_ = 1;
};

TEST_F(AgentStateStoreTest, SessionLifecycleAndIdempotentReplay) {
    C::Response response;
    std::string error;
    ASSERT_TRUE(
        mutate({"SESSION_CREATE", "s1", "60000", "state-1", "create-s1"},
               &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kCreated));
    EXPECT_EQ(response.revision, 1U);
    EXPECT_EQ(response.fence, 0U);
    EXPECT_EQ(response.lsn, 1U);

    EXPECT_TRUE(store_->get(C::marker_key()).has_value());
    EXPECT_EQ(store_->current_lsn(), 1U);
    const std::uint64_t committed_lsn = store_->current_lsn();

    C::Response replay;
    ASSERT_TRUE(
        mutate({"SESSION_CREATE", "s1", "60000", "state-1", "create-s1"},
               &replay, &error))
        << error;
    EXPECT_EQ(replay.status, response.status);
    EXPECT_EQ(replay.revision, response.revision);
    EXPECT_EQ(replay.lsn, response.lsn);
    EXPECT_EQ(store_->current_lsn(), committed_lsn);

    C::Response duplicate;
    ASSERT_TRUE(
        mutate({"SESSION_CREATE", "s1", "60000", "state-1", "create-s1-other"},
               &duplicate, &error))
        << error;
    EXPECT_EQ(duplicate.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kAlreadyExists));
    EXPECT_EQ(duplicate.revision, 1U);
    EXPECT_EQ(duplicate.lsn, 0U);

    ASSERT_TRUE(mutate({"SESSION_UPDATE", "s1", "1", "WAITING_TOOL", "120000",
                        "state-2", "update-s1"},
                       &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kOk));
    EXPECT_EQ(response.revision, 2U);

    ASSERT_TRUE(mutate({"SESSION_UPDATE", "s1", "1", "ACTIVE", "120000",
                        "stale", "stale-update"},
                       &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kVersionMismatch));
    EXPECT_EQ(response.revision, 2U);

    ASSERT_TRUE(mutate({"SESSION_CLOSE", "s1", "2", "COMPLETED", "close-s1"},
                       &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kOk));
    EXPECT_EQ(response.revision, 3U);

    ASSERT_TRUE(mutate({"SESSION_CLOSE", "s1", "3", "FAILED", "close-again"},
                       &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kInvalidState));
    EXPECT_EQ(response.revision, 3U);

    C::Response session;
    ASSERT_TRUE(query({"SESSION_GET", "s1"}, &session, &error)) << error;
    EXPECT_EQ(session.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kOk));
    EXPECT_EQ(session.revision, 3U);
    C::Record record;
    ASSERT_TRUE(C::decode_record(session.payload, &record, &error)) << error;
    EXPECT_EQ(record.status,
              static_cast<std::uint8_t>(C::SessionStatus::kCompleted));

    ASSERT_TRUE(
        mutate({"SESSION_PURGE", "s1", "3", "purge-s1"}, &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kOk));
    ASSERT_TRUE(query({"SESSION_GET", "s1"}, &session, &error)) << error;
    EXPECT_EQ(session.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kNotFound));
    EXPECT_EQ(state_->stats().sessions_created, 1U);
    EXPECT_EQ(state_->stats().sessions_completed, 1U);
    EXPECT_EQ(state_->stats().sessions_purged, 1U);
}

TEST_F(AgentStateStoreTest, ToolLeaseFencingAndTakeover) {
    C::Response response;
    std::string error;
    ASSERT_TRUE(mutate({"SESSION_CREATE", "s1", "600000", "state", "create-s1"},
                       &response, &error))
        << error;

    ASSERT_TRUE(
        mutate({"LEASE_ACQUIRE", "s1", "10000", "lease-1"}, &response, &error))
        << error;
    EXPECT_EQ(response.fence, 1U);
    EXPECT_EQ(response.revision, 2U);

    ASSERT_TRUE(mutate({"LEASE_ACQUIRE", "s1", "10000", "lease-held"},
                       &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kLeaseHeld));
    EXPECT_EQ(response.fence, 1U);

    ASSERT_TRUE(
        mutate({"TOOL_CREATE", "s1", "t1", "shell", "echo hi", "tool-create-1"},
               &response, &error))
        << error;
    EXPECT_EQ(response.revision, 1U);
    EXPECT_EQ(response.fence, 1U);

    ASSERT_TRUE(mutate({"TOOL_CLAIM", "s1", "t1", "1", "1", "5000", "claim-1"},
                       &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kOk));
    EXPECT_EQ(response.revision, 2U);
    EXPECT_EQ(response.fence, 1U);

    C::Response tool;
    ASSERT_TRUE(query({"TOOL_GET", "s1", "t1"}, &tool, &error)) << error;
    EXPECT_EQ(tool.status, static_cast<std::uint8_t>(C::ResponseStatus::kOk));
    C::Record tool_record;
    ASSERT_TRUE(C::decode_record(tool.payload, &tool_record, &error)) << error;
    EXPECT_EQ(tool_record.status,
              static_cast<std::uint8_t>(C::ToolStatus::kRunning));

    clock_->advance(10001);
    ASSERT_TRUE(
        mutate({"LEASE_RENEW", "s1", "4", "1", "10000", "renew-expired"},
               &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kLeaseExpired));

    ASSERT_TRUE(
        mutate({"LEASE_ACQUIRE", "s1", "10000", "lease-2"}, &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kOk));
    EXPECT_EQ(response.fence, 2U);

    ASSERT_TRUE(mutate({"TOOL_COMPLETE", "s1", "t1", "2", "1", "SUCCEEDED",
                        "ok", "stale-fence"},
                       &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kFenced));
    EXPECT_EQ(response.fence, 2U);

    ASSERT_TRUE(mutate({"TOOL_REQUEUE", "s1", "t1", "2", "2", "requeue-1"},
                       &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kOk));
    EXPECT_EQ(response.revision, 3U);

    ASSERT_TRUE(
        mutate({"TOOL_CLAIM", "s1", "t1", "3", "2", "5000", "reclaim-1"},
               &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kOk));
    EXPECT_EQ(response.revision, 4U);

    ASSERT_TRUE(mutate({"TOOL_COMPLETE", "s1", "t1", "4", "2", "SUCCEEDED",
                        "done", "complete-1"},
                       &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kOk));
    EXPECT_EQ(response.revision, 5U);

    const auto stats = state_->stats();
    EXPECT_GE(stats.lease_acquires, 2U);
    EXPECT_EQ(stats.lease_expiries, 1U);
    EXPECT_EQ(stats.fenced_rejections, 1U);
    EXPECT_EQ(stats.tools_stored, 1U);
    EXPECT_EQ(stats.tools_succeeded, 1U);
}

TEST_F(AgentStateStoreTest, RecordCasIdempotencyAndRebuild) {
    C::Response response;
    std::string error;
    ASSERT_TRUE(mutate({"RECORD_CAS", "r1", "0", "value-1", "record-create"},
                       &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kCreated));
    EXPECT_EQ(response.revision, 1U);

    ASSERT_TRUE(mutate({"RECORD_CAS", "r1", "0", "different", "record-create"},
                       &response, &error))
        << error;
    EXPECT_EQ(response.status, static_cast<std::uint8_t>(
                                   C::ResponseStatus::kIdempotencyConflict));

    ASSERT_TRUE(mutate({"RECORD_CAS", "r1", "1", "value-2", "record-update"},
                       &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kOk));
    EXPECT_EQ(response.revision, 2U);

    ASSERT_TRUE(mutate({"RECORD_CAS", "r1", "1", "stale", "stale-record"},
                       &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kVersionMismatch));

    C::Response record_response;
    ASSERT_TRUE(query({"RECORD_GET", "r1"}, &record_response, &error)) << error;
    EXPECT_EQ(record_response.revision, 2U);

    AgentStateStore rebuilt(clock_);
    ASSERT_TRUE(rebuilt.rebuild(store_->export_all(), &error)) << error;
    C::Response rebuilt_record;
    AgentRequest get_request;
    ASSERT_TRUE(AgentStateStore::parse_request("RECORD_GET", {"r1"},
                                               &get_request, &error));
    ASSERT_TRUE(
        rebuilt.prepare_query(get_request, *store_, &rebuilt_record, &error))
        << error;
    EXPECT_EQ(rebuilt_record.revision, 2U);
    EXPECT_EQ(rebuilt_record.payload, record_response.payload);

    AgentRequest replay_request;
    ASSERT_TRUE(AgentStateStore::parse_request(
        "RECORD_CAS", {"r1", "1", "value-2", "record-update"}, &replay_request,
        &error));
    AgentStateStore::MutationPlan replay_plan;
    ASSERT_TRUE(rebuilt.prepare_mutation(replay_request, *store_, 99,
                                         &replay_plan, &error))
        << error;
    EXPECT_EQ(replay_plan.disposition,
              AgentStateStore::MutationPlanDisposition::kImmediate);
    C::Response replay;
    ASSERT_TRUE(
        C::decode_response(replay_plan.encoded_response, &replay, &error))
        << error;
    EXPECT_EQ(replay.revision, 2U);
    EXPECT_EQ(replay.lsn, 2U);
}

TEST_F(AgentStateStoreTest, RebuildRejectsMissingMarkerAndOrphanTool) {
    C::Response response;
    std::string error;
    ASSERT_TRUE(mutate({"SESSION_CREATE", "s1", "60000", "state", "create"},
                       &response, &error))
        << error;
    ASSERT_TRUE(
        mutate({"TOOL_CREATE", "s1", "t1", "tool", "request", "tool-create"},
               &response, &error))
        << error;

    std::vector<KvStore::Entry> entries = store_->export_all();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [](const KvStore::Entry& entry) {
                                     return entry.key == C::marker_key();
                                 }),
                  entries.end());
    AgentStateStore missing_marker(clock_);
    EXPECT_FALSE(missing_marker.rebuild(entries, &error));
    EXPECT_NE(error.find("marker"), std::string::npos);

    entries = store_->export_all();
    std::string session_key;
    ASSERT_TRUE(C::session_key("s1", &session_key, &error));
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const KvStore::Entry& entry) {
                                     return entry.key == session_key;
                                 }),
                  entries.end());
    AgentStateStore orphan(clock_);
    EXPECT_FALSE(orphan.rebuild(entries, &error));
    EXPECT_NE(error.find("orphan"), std::string::npos);
}

TEST_F(AgentStateStoreTest, GcCandidateDeletesToolsBeforeSessionAndIsBounded) {
    C::Response response;
    std::string error;
    ASSERT_TRUE(mutate({"SESSION_CREATE", "s1", "60000", "state", "create"},
                       &response, &error))
        << error;
    ASSERT_TRUE(
        mutate({"TOOL_CREATE", "s1", "t1", "tool", "request", "tool-create"},
               &response, &error))
        << error;
    ASSERT_TRUE(
        mutate({"LEASE_ACQUIRE", "s1", "10000", "lease"}, &response, &error))
        << error;
    ASSERT_TRUE(
        mutate({"SESSION_PURGE", "s1", "3", "too-early"}, &response, &error))
        << error;
    EXPECT_EQ(response.status,
              static_cast<std::uint8_t>(C::ResponseStatus::kInvalidState));

    clock_->advance(60001);
    const auto candidates = state_->collect_gc_candidates();
    ASSERT_EQ(candidates.size(), 1U);
    ASSERT_EQ(candidates.front().keys.size(), 2U);
    std::string session_key;
    std::string tool_key;
    ASSERT_TRUE(C::session_key("s1", &session_key, &error));
    ASSERT_TRUE(C::tool_key("s1", "t1", &tool_key, &error));
    EXPECT_EQ(candidates.front().keys.back(), session_key);
    EXPECT_EQ(candidates.front().keys.front(), tool_key);
}

} // namespace
} // namespace minidb
