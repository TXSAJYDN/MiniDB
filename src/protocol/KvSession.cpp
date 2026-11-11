#include "KvSession.h"
#include "PersistenceStatsJson.h"
#include "../base/Log.h"
#include "../net/EventLoop.h"

#include <charconv>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

namespace minidb {

KvSession::KvSession(const TcpConnectionPtr& conn,
                     std::shared_ptr<KvStore> store)
    : conn_(conn), store_(std::move(store)), started_(false),
      processing_(false), closing_(false), closed_(false),
      drain_scheduled_(false) {}

KvSession::~KvSession() {
    rollback_active_transaction();
}

void KvSession::start() {
    if (started_ || !conn_) {
        return;
    }
    started_ = true;
    std::weak_ptr<KvSession> weak = weak_from_this();
    conn_->set_write_complete_callback([weak](const TcpConnectionPtr&) {
        if (auto self = weak.lock()) {
            self->conn_->loop()->queue_in_loop([weak]() {
                if (auto session = weak.lock()) {
                    session->on_write_complete();
                }
            });
        }
    });
}

void KvSession::on_message(Buffer* buf) {
    if (buf == nullptr || buf->readable_bytes() == 0) {
        return;
    }
    if (closed_ || closing_) {
        buf->retrieve_all();
        return;
    }

    const size_t incoming = buf->readable_bytes();
    const size_t pending = pending_buffer_.readable_bytes();
    if (incoming > kMaxPendingBufferSize ||
        pending > kMaxPendingBufferSize - incoming) {
        buf->retrieve_all();
        pending_buffer_.retrieve_all();
        fail_and_close("Pending KV input exceeds limit");
        return;
    }

    pending_buffer_.append(buf->peek(), incoming);
    buf->retrieve_all();
    if (!processing_) {
        schedule_drain();
    }
}

void KvSession::schedule_drain() {
    if (closed_ || closing_ || processing_ || drain_scheduled_ || !conn_) {
        return;
    }
    drain_scheduled_ = true;
    std::weak_ptr<KvSession> weak = weak_from_this();
    conn_->loop()->queue_in_loop([weak]() {
        if (auto self = weak.lock()) {
            self->drain();
        }
    });
}

void KvSession::drain() {
    drain_scheduled_ = false;
    if (closed_ || closing_ || processing_ || !conn_ || !conn_->connected()) {
        return;
    }
    if (pending_buffer_.readable_bytes() == 0) {
        conn_->start_reading();
        return;
    }

    KvProtocol::Command cmd;
    const int parsed = KvProtocol::parse_command(
        reinterpret_cast<const uint8_t*>(pending_buffer_.peek()),
        pending_buffer_.readable_bytes(), &cmd);
    if (parsed < 0) {
        pending_buffer_.retrieve_all();
        fail_and_close("Invalid command frame");
        return;
    }
    if (parsed == 0) {
        if (pending_buffer_.readable_bytes() >
            static_cast<size_t>(KvProtocol::kMaxFrameSize) + 4u) {
            pending_buffer_.retrieve_all();
            fail_and_close("Oversized command frame");
            return;
        }
        conn_->start_reading();
        return;
    }

    pending_buffer_.retrieve(static_cast<size_t>(parsed));
    processing_ = true;
    conn_->stop_reading();
    if (!handle_command(cmd) || closed_) {
        return;
    }
}

bool KvSession::validate_key(const std::string& key, const char* command) {
    if (key.empty() || key.size() > KvProtocol::kMaxKeySize) {
        send_response(KvProtocol::kError,
                      {std::string(command) + " key must be 1..1024 bytes"});
        return false;
    }
    if (AgentRecordCodec::is_reserved_key(key)) {
        send_response(
            KvProtocol::kError,
            {std::string(command) + " key is in the reserved agent namespace"});
        return false;
    }
    return true;
}

bool KvSession::handle_command(const KvProtocol::Command& cmd) {
    if (!store_) {
        fail_and_close("KV store unavailable");
        return false;
    }

    const auto require_args = [&](size_t count, const char* usage) {
        if (cmd.args.size() == count) {
            return true;
        }
        send_response(KvProtocol::kError, {std::string(usage)});
        return false;
    };

    switch (cmd.type) {
    case KvProtocol::kPing:
        if (!require_args(0, "PING takes no arguments")) {
            return true;
        }
        return send_response(KvProtocol::kOk, {"PONG"});

    case KvProtocol::kSet:
        if (!require_args(2, "SET requires 2 arguments") ||
            !validate_key(cmd.args[0], "SET")) {
            return true;
        }
        if (active_txn_id_ != 0) {
            const PersistenceManager::TransactionResult result =
                persistence_manager_->transaction_set(active_txn_id_,
                                                      cmd.args[0], cmd.args[1]);
            if (!result.ok()) {
                return send_transaction_failure(result.status, result.error);
            }
            return send_response(KvProtocol::kOk, {});
        }
        if (persistence_manager_) {
            std::weak_ptr<KvSession> weak = weak_from_this();
            const MutationAdmission admission = persistence_manager_->set_async(
                cmd.args[0], cmd.args[1], [weak](MutationOutcome outcome) {
                    if (auto self = weak.lock()) {
                        self->on_mutation_completed(std::move(outcome),
                                                    KvProtocol::kOk);
                    }
                });
            return begin_async_operation(admission);
        }
        store_->set(cmd.args[0], cmd.args[1]);
        return send_response(KvProtocol::kOk, {});

    case KvProtocol::kGet:
        if (!require_args(1, "GET requires 1 argument") ||
            !validate_key(cmd.args[0], "GET")) {
            return true;
        }
        if (active_txn_id_ != 0) {
            const PersistenceManager::TransactionResult result =
                persistence_manager_->transaction_get(active_txn_id_,
                                                      cmd.args[0]);
            if (!result.ok()) {
                return send_transaction_failure(result.status, result.error);
            }
            return result.found
                       ? send_response(KvProtocol::kValue, {result.value})
                       : send_response(KvProtocol::kNotFound, {});
        }
        {
            const auto value = store_->get(cmd.args[0]);
            return value.has_value()
                       ? send_response(KvProtocol::kValue, {value.value()})
                       : send_response(KvProtocol::kNotFound, {});
        }

    case KvProtocol::kDel:
        if (!require_args(1, "DEL requires 1 argument") ||
            !validate_key(cmd.args[0], "DEL")) {
            return true;
        }
        if (active_txn_id_ != 0) {
            const PersistenceManager::TransactionResult result =
                persistence_manager_->transaction_del(active_txn_id_,
                                                      cmd.args[0]);
            if (!result.ok()) {
                return send_transaction_failure(result.status, result.error);
            }
            return send_response(KvProtocol::kInteger,
                                 {result.changed ? "1" : "0"});
        }
        {
            if (persistence_manager_) {
                std::weak_ptr<KvSession> weak = weak_from_this();
                const MutationAdmission admission =
                    persistence_manager_->del_async(
                        cmd.args[0], [weak](MutationOutcome outcome) {
                            if (auto self = weak.lock()) {
                                self->on_mutation_completed(
                                    std::move(outcome), KvProtocol::kInteger);
                            }
                        });
                return begin_async_operation(admission);
            }
            const bool deleted = store_->del(cmd.args[0]);
            return send_response(KvProtocol::kInteger, {deleted ? "1" : "0"});
        }

    case KvProtocol::kExpire: {
        if (!require_args(2, "EXPIRE requires 2 arguments") ||
            !validate_key(cmd.args[0], "EXPIRE")) {
            return true;
        }
        int64_t ttl_ms = 0;
        const std::string& text = cmd.args[1];
        const auto result =
            std::from_chars(text.data(), text.data() + text.size(), ttl_ms);
        if (text.empty() || result.ec != std::errc{} ||
            result.ptr != text.data() + text.size()) {
            send_response(KvProtocol::kError, {"Invalid TTL"});
            return true;
        }
        if (active_txn_id_ != 0) {
            const PersistenceManager::TransactionResult result =
                persistence_manager_->transaction_expire(active_txn_id_,
                                                         cmd.args[0], ttl_ms);
            if (!result.ok()) {
                return send_transaction_failure(result.status, result.error);
            }
            return send_response(KvProtocol::kOk, {});
        }
        if (persistence_manager_) {
            std::weak_ptr<KvSession> weak = weak_from_this();
            const MutationAdmission admission =
                persistence_manager_->expire_async(
                    cmd.args[0], ttl_ms, [weak](MutationOutcome outcome) {
                        if (auto self = weak.lock()) {
                            self->on_mutation_completed(std::move(outcome),
                                                        KvProtocol::kOk);
                        }
                    });
            return begin_async_operation(admission);
        }
        store_->expire(cmd.args[0], ttl_ms);
        return send_response(KvProtocol::kOk, {});
    }

    case KvProtocol::kTtl: {
        if (!require_args(1, "TTL requires 1 argument") ||
            !validate_key(cmd.args[0], "TTL")) {
            return true;
        }
        KvStore::TtlResult info;
        if (active_txn_id_ != 0) {
            const PersistenceManager::TransactionResult result =
                persistence_manager_->transaction_ttl(active_txn_id_,
                                                      cmd.args[0]);
            if (!result.ok()) {
                return send_transaction_failure(result.status, result.error);
            }
            info = result.ttl;
        } else {
            info = store_->ttl_info(cmd.args[0]);
        }
        switch (info.state) {
        case KvStore::TtlState::kMissing:
            return send_response(KvProtocol::kInteger, {"-2"});
        case KvStore::TtlState::kNoExpiry:
            return send_response(KvProtocol::kInteger, {"-1"});
        case KvStore::TtlState::kExpiring:
            return send_response(KvProtocol::kInteger,
                                 {std::to_string(info.remaining_ms)});
        }
        return true;
    }

    case KvProtocol::kBegin: {
        if (!require_args(0, "BEGIN takes no arguments")) {
            return true;
        }
        if (!persistence_manager_) {
            send_response(KvProtocol::kError, {"Transactions are unavailable"});
            return true;
        }
        if (active_txn_id_ != 0) {
            send_response(KvProtocol::kError, {"Transaction already active"});
            return true;
        }
        const PersistenceManager::TransactionBeginResult result =
            persistence_manager_->begin_transaction();
        if (!result.ok()) {
            return send_transaction_failure(result.status, result.error);
        }
        active_txn_id_ = result.txn_id;
        return send_response(KvProtocol::kOk, {std::to_string(result.txn_id)});
    }

    case KvProtocol::kCommit: {
        if (!require_args(0, "COMMIT takes no arguments")) {
            return true;
        }
        if (!persistence_manager_) {
            send_response(KvProtocol::kError, {"Transactions are unavailable"});
            return true;
        }
        if (active_txn_id_ == 0) {
            send_response(KvProtocol::kError, {"No active transaction"});
            return true;
        }
        const std::uint64_t txn_id = active_txn_id_;
        // Once COMMIT is admitted the manager owns its completion.  In
        // particular, clearing this id first ensures a disconnect cannot
        // roll back a commit that already owns a WAL slot.
        active_txn_id_ = 0;
        std::weak_ptr<KvSession> weak = weak_from_this();
        const MutationAdmission admission =
            persistence_manager_->commit_transaction_async(
                txn_id, [weak](MutationOutcome outcome) {
                    if (auto self = weak.lock()) {
                        self->on_commit_completed(std::move(outcome));
                    }
                });
        return begin_async_operation(admission);
    }

    case KvProtocol::kRollback: {
        if (!require_args(0, "ROLLBACK takes no arguments")) {
            return true;
        }
        if (!persistence_manager_) {
            send_response(KvProtocol::kError, {"Transactions are unavailable"});
            return true;
        }
        if (active_txn_id_ == 0) {
            send_response(KvProtocol::kError, {"No active transaction"});
            return true;
        }
        const std::uint64_t txn_id = active_txn_id_;
        active_txn_id_ = 0;
        const PersistenceManager::TransactionResult result =
            persistence_manager_->rollback_transaction(txn_id);
        if (!result.ok()) {
            return send_transaction_failure(result.status, result.error);
        }
        return send_response(KvProtocol::kOk, {});
    }

    case KvProtocol::kAgent: {
        if (cmd.args.empty()) {
            send_response(KvProtocol::kError,
                          {"AGENT requires at least one argument"});
            return true;
        }
        if (active_txn_id_ != 0) {
            send_response(
                KvProtocol::kError,
                {"AGENT is unavailable inside an explicit transaction"});
            return true;
        }
        if (!persistence_manager_) {
            send_response(KvProtocol::kError, {"AGENT is unavailable"});
            return true;
        }
        const std::string& subcommand = cmd.args.front();
        const std::vector<std::string> args(cmd.args.begin() + 1,
                                            cmd.args.end());
        AgentRequest request;
        std::string parse_error;
        if (!AgentStateStore::parse_request(subcommand, args, &request,
                                            &parse_error)) {
            send_response(
                KvProtocol::kError,
                {parse_error.empty() ? "Invalid AGENT request" : parse_error});
            return true;
        }
        if (!AgentStateStore::is_mutation(request.command)) {
            const AgentResult result =
                persistence_manager_->agent_query(request);
            if (result.status != AgentRequestStatus::kCompleted ||
                result.encoded_response.empty()) {
                send_agent_failure(result);
                return true;
            }
            return send_response(KvProtocol::kValue, {result.encoded_response});
        }
        std::weak_ptr<KvSession> weak = weak_from_this();
        const AgentResult admission = persistence_manager_->agent_mutate_async(
            request, [weak](AgentResult result) {
                if (auto self = weak.lock()) {
                    self->on_agent_completed(std::move(result));
                }
            });
        if (admission.status == AgentRequestStatus::kCompleted &&
            !admission.encoded_response.empty()) {
            return send_response(KvProtocol::kValue,
                                 {admission.encoded_response});
        }
        if (admission.status != AgentRequestStatus::kAccepted) {
            send_agent_failure(admission);
            return true;
        }
        return true;
    }

    case KvProtocol::kSave:
        if (!require_args(0, "SAVE takes no arguments")) {
            return true;
        }
        return handle_save();

    case KvProtocol::kStats:
        if (!require_args(0, "STATS takes no arguments")) {
            return true;
        }
        if (!persistence_manager_) {
            send_response(KvProtocol::kError, {"STATS is unavailable"});
            return true;
        }
        return send_response(
            KvProtocol::kValue,
            {persistence_stats_json(persistence_manager_->snapshot())});

    default:
        pending_buffer_.retrieve_all();
        fail_and_close("Unknown command");
        return false;
    }
}

bool KvSession::handle_save() {
    if (persistence_manager_) {
        std::weak_ptr<KvSession> weak = weak_from_this();
        std::string error;
        if (!persistence_manager_->checkpoint_async(
                [weak](RdbSaveResult result,
                       const std::string& completion_error) {
                    if (auto self = weak.lock()) {
                        self->on_save_completed(result, completion_error);
                    }
                },
                &error)) {
            Log::Error("SAVE could not start: " + error);
            send_response(KvProtocol::kError, {"Failed to save RDB"});
        }
        return true;
    }

    if (!save_callback_) {
        send_response(KvProtocol::kError, {"SAVE is unavailable"});
        return true;
    }
    const RdbSaveResult result = save_callback_();
    on_save_completed(result, {});
    return true;
}

bool KvSession::begin_async_operation(const MutationAdmission& admission) {
    if (admission.accepted()) {
        return true;
    }
    send_admission_failure(admission);
    return true;
}

void KvSession::on_mutation_completed(MutationOutcome outcome,
                                      KvProtocol::CommandType success_type) {
    if (closed_) {
        return;
    }
    if (!outcome.ok()) {
        send_mutation_failure(outcome);
        return;
    }
    if (success_type == KvProtocol::kInteger) {
        send_response(KvProtocol::kInteger, {outcome.changed ? "1" : "0"});
        return;
    }
    send_response(success_type, {});
}

void KvSession::on_commit_completed(MutationOutcome outcome) {
    if (closed_) {
        return;
    }
    if (!outcome.ok()) {
        send_mutation_failure(outcome);
        return;
    }
    send_response(KvProtocol::kOk, {std::to_string(outcome.lsn)});
}

void KvSession::on_agent_completed(AgentResult result) {
    if (closed_) {
        return;
    }
    if (result.status != AgentRequestStatus::kCompleted ||
        result.encoded_response.empty()) {
        send_agent_failure(result);
        return;
    }
    send_response(KvProtocol::kValue, {result.encoded_response});
}

bool KvSession::send_transaction_failure(MutationStatus status,
                                         const std::string& error) {
    // Resource and persistence failures abort the manager-side transaction.
    // Invalid arguments are rejected before mutating its state.
    if (active_txn_id_ != 0 && status != MutationStatus::kInvalidArgument) {
        active_txn_id_ = 0;
    }
    if (status == MutationStatus::kConflict) {
        return send_response(KvProtocol::kConflict, {});
    }

    std::string message = "Transaction failed";
    switch (status) {
    case MutationStatus::kInvalidArgument:
        message = error.empty() ? "Invalid transaction argument" : error;
        break;
    case MutationStatus::kResourceLimit:
        message = error.empty() ? "Transaction resource limit exceeded" : error;
        break;
    case MutationStatus::kNotReady:
        message = "Transactions are not accepting writes";
        break;
    case MutationStatus::kIoError:
        message = "Transaction persistence write failed";
        break;
    case MutationStatus::kNoTransaction:
        message = "Transaction is not active";
        break;
    case MutationStatus::kConflict:
    case MutationStatus::kOk:
        break;
    }
    return send_response(KvProtocol::kError, {message});
}

void KvSession::rollback_active_transaction() {
    if (active_txn_id_ == 0) {
        return;
    }
    const std::uint64_t txn_id = active_txn_id_;
    active_txn_id_ = 0;
    if (persistence_manager_ != nullptr) {
        (void)persistence_manager_->rollback_transaction(txn_id);
    }
}

void KvSession::on_save_completed(RdbSaveResult result,
                                  const std::string& error) {
    if (closed_) {
        return;
    }
    if (result == RdbSaveResult::Ok) {
        send_response(KvProtocol::kOk, {});
        return;
    }
    if (result == RdbSaveResult::DurabilityUnconfirmed) {
        send_response(KvProtocol::kError, {"RDB durability unconfirmed"});
        return;
    }
    if (!error.empty()) {
        Log::Error("SAVE failed: " + error);
    }
    send_response(KvProtocol::kError, {"Failed to save RDB"});
}

void KvSession::send_mutation_failure(const MutationOutcome& outcome) {
    std::string message = "Persistence write failed";
    switch (outcome.status) {
    case MutationStatus::kInvalidArgument:
        message = outcome.error.empty() ? "Invalid argument" : outcome.error;
        break;
    case MutationStatus::kResourceLimit:
        message = "Persistence resource limit exceeded";
        break;
    case MutationStatus::kNotReady:
        message = "Persistence is not accepting writes";
        break;
    case MutationStatus::kNoTransaction:
        message = "Transaction is not active";
        break;
    case MutationStatus::kConflict:
        send_response(KvProtocol::kConflict, {});
        return;
    case MutationStatus::kIoError:
    case MutationStatus::kOk:
        break;
    }
    send_response(KvProtocol::kError, {message});
}

void KvSession::send_agent_failure(const AgentResult& result) {
    if (result.status == AgentRequestStatus::kConflict) {
        send_response(KvProtocol::kConflict, {});
        return;
    }
    std::string message = "Agent persistence request failed";
    switch (result.status) {
    case AgentRequestStatus::kInvalidArgument:
        message = result.error.empty() ? "Invalid Agent request" : result.error;
        break;
    case AgentRequestStatus::kNotReady:
        message = "Persistence is not accepting writes";
        break;
    case AgentRequestStatus::kShuttingDown:
        message = "Persistence is shutting down";
        break;
    case AgentRequestStatus::kIoError:
        message = "Agent persistence I/O failure";
        break;
    case AgentRequestStatus::kResourceLimit:
        message = "Agent persistence resource limit exceeded";
        break;
    case AgentRequestStatus::kCompleted:
    case AgentRequestStatus::kAccepted:
    case AgentRequestStatus::kConflict:
        break;
    }
    send_response(KvProtocol::kError, {message});
}

void KvSession::send_admission_failure(const MutationAdmission& admission) {
    std::string message = "Persistence is not accepting writes";
    switch (admission.status) {
    case MutationAdmissionStatus::kInvalidArgument:
        message =
            admission.error.empty() ? "Invalid argument" : admission.error;
        break;
    case MutationAdmissionStatus::kResourceLimit:
        message = "Persistence resource limit exceeded";
        break;
    case MutationAdmissionStatus::kIoError:
        message = "Persistence write failed";
        break;
    case MutationAdmissionStatus::kNoTransaction:
        message = "Transaction is not active";
        break;
    case MutationAdmissionStatus::kConflict:
        send_response(KvProtocol::kConflict, {});
        return;
    case MutationAdmissionStatus::kNotReady:
    case MutationAdmissionStatus::kShuttingDown:
    case MutationAdmissionStatus::kAccepted:
        break;
    }
    send_response(KvProtocol::kError, {message});
}

bool KvSession::send_response(KvProtocol::CommandType type,
                              const std::vector<std::string>& args) {
    if (!conn_ || closed_ || !conn_->connected()) {
        rollback_active_transaction();
        closed_ = true;
        return false;
    }

    std::vector<uint8_t> data;
    try {
        data = KvProtocol::encode_command(type, args);
    } catch (const std::exception& ex) {
        Log::Error(std::string("KV response encoding failed: ") + ex.what());
        fail_and_close("Response exceeds wire limit");
        return false;
    }

    if (data.size() > KvProtocol::kMaxFrameSize + 4u ||
        !conn_->send(data.data(), data.size())) {
        fail_and_close("Failed to queue KV response");
        return false;
    }
    return true;
}

void KvSession::fail_and_close(const std::string& message) {
    rollback_active_transaction();
    if (closed_ || closing_) {
        return;
    }
    closing_ = true;
    processing_ = true;
    drain_scheduled_ = false;
    pending_buffer_.retrieve_all();
    if (conn_) {
        conn_->stop_reading();
    }
    if (!send_response(KvProtocol::kError, {message})) {
        closed_ = true;
        return;
    }
    if (conn_ && conn_->connected()) {
        conn_->shutdown();
    } else {
        closed_ = true;
    }
}

void KvSession::on_write_complete() {
    if (closed_) {
        return;
    }
    if (closing_) {
        closed_ = true;
        if (conn_ && conn_->connected()) {
            conn_->shutdown();
        }
        return;
    }
    if (!processing_) {
        return;
    }
    processing_ = false;
    if (!conn_ || !conn_->connected()) {
        rollback_active_transaction();
        closed_ = true;
        return;
    }
    if (pending_buffer_.readable_bytes() > 0) {
        schedule_drain();
    } else {
        conn_->start_reading();
    }
}

} // namespace minidb
