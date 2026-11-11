#include "../src/base/Clock.h"
#include "../src/storage/AgentRecordCodec.h"
#include "../src/storage/AgentStateStore.h"
#include "../src/storage/KvStore.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using C = minidb::AgentRecordCodec;

class FrozenClock final : public minidb::ClockInterface {
  public:
    std::chrono::steady_clock::time_point now() const override {
        return minidb::Clock::from_ms(1'000'000);
    }

    std::int64_t system_now_ms() const override {
        return 1'760'000'000'000LL;
    }
};

class Harness {
  public:
    Harness()
        : clock_(std::make_shared<FrozenClock>()), store_(clock_),
          state_(clock_) {
        const std::vector<minidb::KvStore::Entry> empty;
        std::string error;
        (void)state_.rebuild(empty, &error);
    }

    void execute(std::string_view subcommand,
                 const std::vector<std::string>& args) {
        minidb::AgentRequest request;
        std::string error;
        if (!minidb::AgentStateStore::parse_request(subcommand, args, &request,
                                                    &error)) {
            return;
        }

        if (!minidb::AgentStateStore::is_mutation(request.command)) {
            C::Response response;
            if (!state_.prepare_query(request, store_, &response, &error)) {
                return;
            }
            std::string encoded;
            (void)C::encode_response(response, &encoded, &error);
            return;
        }

        minidb::AgentStateStore::MutationPlan plan;
        if (!state_.prepare_mutation(request, store_, next_lsn_, &plan,
                                     &error)) {
            return;
        }
        if (plan.disposition ==
            minidb::AgentStateStore::MutationPlanDisposition::kImmediate) {
            C::Response response;
            if (!plan.encoded_response.empty()) {
                (void)C::decode_response(plan.encoded_response, &response,
                                         &error);
            }
            return;
        }

        std::vector<minidb::KvStore::AppliedMutation> applied;
        applied.reserve(plan.operations.size());
        for (const minidb::WalTxnOp& operation : plan.operations) {
            minidb::KvStore::AppliedMutation mutation;
            switch (operation.op) {
            case minidb::WalOp::kSet:
                mutation.kind = minidb::KvStore::AppliedMutation::Kind::kSet;
                break;
            case minidb::WalOp::kDel:
                mutation.kind = minidb::KvStore::AppliedMutation::Kind::kDelete;
                break;
            case minidb::WalOp::kExpire:
                mutation.kind = minidb::KvStore::AppliedMutation::Kind::kExpire;
                break;
            case minidb::WalOp::kTxn:
                state_.cancel_reservation(plan.reservation_id);
                return;
            }
            mutation.key = operation.key;
            mutation.value = operation.value;
            mutation.absolute_expiry_ms = operation.expiry_ms;
            applied.push_back(std::move(mutation));
        }

        const minidb::KvStore::ApplyStatus apply_status =
            store_.apply_committed_status(next_lsn_, applied, &error);
        if (apply_status != minidb::KvStore::ApplyStatus::kOk) {
            state_.cancel_reservation(plan.reservation_id);
            return;
        }
        if (!state_.apply_committed(plan.operations, next_lsn_,
                                    plan.reservation_id, &error)) {
            __builtin_trap();
        }
        ++next_lsn_;

        if (!plan.encoded_response.empty()) {
            C::Response response;
            (void)C::decode_response(plan.encoded_response, &response, &error);
        }
    }

    void run_bootstrap() {
        execute("SESSION_CREATE", {"s1", "600000", "state-1", "boot-create"});
        execute("SESSION_UPDATE",
                {"s1", "1", "ACTIVE", "600000", "state-2", "boot-update"});
        execute("LEASE_ACQUIRE", {"s1", "100000", "boot-lease"});
        execute("TOOL_CREATE",
                {"s1", "t1", "shell", "echo boot", "boot-tool-create"});
        execute("TOOL_CLAIM",
                {"s1", "t1", "1", "1", "5000", "boot-tool-claim"});
        execute("LEASE_RENEW", {"s1", "5", "1", "100000", "boot-lease-renew"});
        execute("TOOL_COMPLETE",
                {"s1", "t1", "2", "1", "SUCCEEDED", "done", "boot-complete"});
        execute("LEASE_RELEASE", {"s1", "7", "1", "boot-release"});
        execute("RECORD_CAS", {"r1", "0", "value", "boot-record-create"});
        execute("RECORD_GET", {"r1"});
        execute("SESSION_GET", {"s1"});
        execute("TOOL_GET", {"s1", "t1"});

        execute("SESSION_CREATE", {"s2", "600000", "state-1", "boot-create-2"});
        execute("SESSION_UPDATE", {"s2", "1", "WAITING_TOOL", "600000",
                                   "state-2", "boot-update-2"});
        execute("SESSION_CLOSE", {"s2", "2", "COMPLETED", "boot-close-2"});
        execute("SESSION_GET", {"s2"});
        execute("SESSION_PURGE", {"s2", "3", "boot-purge-2"});
        execute("SESSION_GET", {"s2"});
    }

  private:
    std::shared_ptr<FrozenClock> clock_;
    minidb::KvStore store_;
    minidb::AgentStateStore state_;
    std::uint64_t next_lsn_ = 1;
};

class Cursor {
  public:
    Cursor(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {}

    bool read_u8(std::uint8_t* value) {
        if (offset_ >= size_) {
            return false;
        }
        *value = data_[offset_++];
        return true;
    }

    bool read_u16(std::uint16_t* value) {
        if (size_ - offset_ < 2) {
            return false;
        }
        *value = static_cast<std::uint16_t>(data_[offset_]) |
                 (static_cast<std::uint16_t>(data_[offset_ + 1]) << 8);
        offset_ += 2;
        return true;
    }

    bool read_string(std::string* value) {
        std::uint16_t length = 0;
        if (!read_u16(&length) || length > size_ - offset_) {
            return false;
        }
        value->assign(reinterpret_cast<const char*>(data_ + offset_), length);
        offset_ += length;
        return true;
    }

    bool done() const {
        return offset_ >= size_;
    }

  private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_ = 0;
};

constexpr std::array<std::string_view, 15> kCommands = {
    "SESSION_CREATE", "SESSION_GET",  "SESSION_UPDATE", "SESSION_CLOSE",
    "SESSION_PURGE",  "TOOL_CREATE",  "TOOL_GET",       "TOOL_CLAIM",
    "TOOL_COMPLETE",  "TOOL_REQUEUE", "LEASE_ACQUIRE",  "LEASE_RENEW",
    "LEASE_RELEASE",  "RECORD_GET",   "RECORD_CAS",
};

void run_stream(const std::uint8_t* data, std::size_t size, Harness* harness) {
    Cursor cursor(data, size);
    std::size_t actions = 0;
    while (!cursor.done() && actions < 16) {
        std::uint8_t selector = 0;
        std::uint8_t argument_count = 0;
        if (!cursor.read_u8(&selector) || !cursor.read_u8(&argument_count)) {
            return;
        }
        if (argument_count > 8) {
            return;
        }
        std::vector<std::string> args;
        args.reserve(argument_count);
        for (std::uint8_t index = 0; index < argument_count; ++index) {
            std::string arg;
            if (!cursor.read_string(&arg)) {
                return;
            }
            args.push_back(std::move(arg));
        }
        harness->execute(kCommands[selector % kCommands.size()], args);
        ++actions;
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    constexpr std::size_t kMaxInputSize = 256u * 1024u;
    if (size > kMaxInputSize) {
        return 0;
    }

    Harness harness;
    std::size_t offset = 0;
    // Bit zero selects the deterministic valid prefix.  The corpus includes
    // both modes so state-machine transitions and raw parser failures are
    // exercised by the same target.
    if (size != 0 && (data[0] & 1u) != 0) {
        harness.run_bootstrap();
        offset = 1;
    }
    run_stream(data + offset, size - offset, &harness);
    return 0;
}
