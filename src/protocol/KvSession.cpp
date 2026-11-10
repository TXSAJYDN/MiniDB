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

KvSession::~KvSession() = default;

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
    const auto fail_mutation = [&](const MutationOutcome& outcome) {
        const std::string message =
            outcome.status == MutationStatus::kInvalidArgument
                ? outcome.error
                : std::string("Persistence write failed");
        send_response(KvProtocol::kError, {message});
        return true;
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
        if (persistence_manager_) {
            const MutationOutcome outcome =
                persistence_manager_->set(cmd.args[0], cmd.args[1]);
            if (!outcome.ok()) {
                return fail_mutation(outcome);
            }
        } else {
            store_->set(cmd.args[0], cmd.args[1]);
        }
        return send_response(KvProtocol::kOk, {});

    case KvProtocol::kGet:
        if (!require_args(1, "GET requires 1 argument") ||
            !validate_key(cmd.args[0], "GET")) {
            return true;
        } else {
            const auto value = store_->get(cmd.args[0]);
            return value.has_value()
                       ? send_response(KvProtocol::kValue, {value.value()})
                       : send_response(KvProtocol::kNotFound, {});
        }

    case KvProtocol::kDel:
        if (!require_args(1, "DEL requires 1 argument") ||
            !validate_key(cmd.args[0], "DEL")) {
            return true;
        } else {
            bool deleted = false;
            if (persistence_manager_) {
                const MutationOutcome outcome =
                    persistence_manager_->del(cmd.args[0]);
                if (!outcome.ok()) {
                    return fail_mutation(outcome);
                }
                deleted = outcome.changed;
            } else {
                deleted = store_->del(cmd.args[0]);
            }
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
        if (persistence_manager_) {
            const MutationOutcome outcome =
                persistence_manager_->expire(cmd.args[0], ttl_ms);
            if (!outcome.ok()) {
                return fail_mutation(outcome);
            }
        } else {
            store_->expire(cmd.args[0], ttl_ms);
        }
        return send_response(KvProtocol::kOk, {});
    }

    case KvProtocol::kTtl:
        if (!require_args(1, "TTL requires 1 argument") ||
            !validate_key(cmd.args[0], "TTL")) {
            return true;
        } else {
            const auto info = store_->ttl_info(cmd.args[0]);
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

    case KvProtocol::kSave: {
        if (!require_args(0, "SAVE takes no arguments")) {
            return true;
        }
        if (!save_callback_) {
            send_response(KvProtocol::kError, {"SAVE is unavailable"});
            return true;
        }
        const RdbSaveResult result = save_callback_();
        if (result == RdbSaveResult::Ok) {
            return send_response(KvProtocol::kOk, {});
        }
        send_response(KvProtocol::kError,
                      {result == RdbSaveResult::DurabilityUnconfirmed
                           ? "RDB durability unconfirmed"
                           : "Failed to save RDB"});
        return true;
    }

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

bool KvSession::send_response(KvProtocol::CommandType type,
                              const std::vector<std::string>& args) {
    if (!conn_ || closed_ || !conn_->connected()) {
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
