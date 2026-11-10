#include "HttpServer.h"
#include "PersistenceStatsJson.h"

#include "../base/Log.h"
#include "../net/EventLoop.h"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace minidb {
namespace {

constexpr std::int64_t kIntervalFlushMs = 100;

bool has_kv_prefix(const std::string& path) {
    return path.rfind("/kv/", 0) == 0;
}

void set_plain_text(HttpResponse* resp) {
    resp->set_content_type("text/plain");
}

void set_not_found(HttpResponse* resp) {
    resp->set_status_code(HttpResponse::k404NotFound);
    resp->set_status_message("Not Found");
    set_plain_text(resp);
    resp->set_body("404 Not Found\n");
}

void set_bad_request(HttpResponse* resp, const std::string& message) {
    resp->set_status_code(HttpResponse::k400BadRequest);
    resp->set_status_message("Bad Request");
    set_plain_text(resp);
    resp->set_body(message + "\n");
}

void set_internal_error(HttpResponse* resp, const std::string& message) {
    resp->set_status_code(HttpResponse::k500InternalServerError);
    resp->set_status_message("Internal Server Error");
    resp->set_content_type("application/json");
    resp->set_body("{\"status\":\"error\",\"message\":\"" + message + "\"}\n");
}

} // namespace

HttpServer::HttpServer(EventLoop* loop, const InetAddress& listen_addr,
                       const std::string& rdb_path,
                       DurabilityMode durability_mode)
    : loop_(loop), kv_store_(std::make_shared<KvStore>()),
      rdb_manager_(std::make_unique<RdbManager>(rdb_path.empty() ? "minidb.rdb"
                                                                 : rdb_path)),
      rdb_path_(rdb_path.empty() ? "minidb.rdb" : rdb_path),
      durability_mode_(durability_mode),
      server_(loop, listen_addr, "HttpServer") {
    if (loop_ == nullptr) {
        throw std::invalid_argument("HttpServer requires a non-null EventLoop");
    }
    server_.set_connection_callback(
        [this](const TcpConnectionPtr& conn) { on_connection(conn); });
    server_.set_message_callback(
        [this](const TcpConnectionPtr& conn, Buffer* buf) {
            on_message(conn, buf);
        });
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start() {
    if (started_) {
        return;
    }
    scanner_lease_ = kv_store_->acquire_scanner();
    try {
        server_.start();
        if (persistence_manager_ && persistence_manager_->opened() &&
            durability_mode_ == DurabilityMode::kInterval) {
            interval_timer_id_ = loop_->run_every(
                kIntervalFlushMs, [this]() { flush_interval(); });
        }
    } catch (...) {
        if (interval_timer_id_ != 0) {
            loop_->cancel_timer(interval_timer_id_);
            interval_timer_id_ = 0;
        }
        scanner_lease_.reset();
        throw;
    }
    started_ = true;
}

void HttpServer::stop() {
    if (interval_timer_id_ != 0) {
        loop_->cancel_timer(interval_timer_id_);
        interval_timer_id_ = 0;
    }
    scanner_lease_.reset();
    started_ = false;
}

void HttpServer::set_kv_store(std::shared_ptr<KvStore> store) {
    if (!store) {
        throw std::invalid_argument("HttpServer requires a non-null KvStore");
    }
    if (started_ || scanner_lease_) {
        throw std::logic_error(
            "HttpServer::set_kv_store must precede HttpServer::start");
    }
    if (persistence_manager_) {
        throw std::logic_error(
            "HttpServer::set_kv_store must precede load_rdb");
    }
    kv_store_ = std::move(store);
}

bool HttpServer::load_rdb() {
    if (!rdb_manager_) {
        Log::Warning("RdbManager not initialized");
        return false;
    }
    if (persistence_manager_) {
        return persistence_manager_->opened();
    }
    if (started_) {
        Log::Error("Cannot load persistence state after HttpServer::start");
        return false;
    }

    auto manager = std::make_unique<PersistenceManager>(kv_store_, rdb_path_,
                                                        durability_mode_);
    std::string error;
    if (!manager->open(&error)) {
        Log::Error("Failed to open persistence image: " + error);
        return false;
    }
    const RecoveryStats& stats = manager->recovery_stats();
    persistence_manager_ = std::move(manager);

    std::ostringstream message;
    message << "Recovered " << stats.rdb_records << " RDB records and replayed "
            << stats.wal_records_replayed << " WAL records"
            << " (checkpoint_lsn=" << stats.checkpoint_lsn
            << ", last_lsn=" << stats.last_lsn
            << ", truncated_bytes=" << stats.truncated_bytes << ")";
    if (stats.legacy_import) {
        message << "; legacy RDB will be imported on first persistence write";
    }
    Log::Info(message.str());
    return true;
}

RdbSaveResult HttpServer::save_rdb() {
    RdbSaveResult result = RdbSaveResult::Failed;
    if (persistence_manager_) {
        std::string error;
        result = persistence_manager_->checkpoint(&error);
        if (result != RdbSaveResult::Ok && !error.empty()) {
            Log::Error("Checkpoint failed: " + error);
        }
    } else if (rdb_manager_) {
        result = rdb_manager_->save_checked(*kv_store_);
    } else {
        Log::Error("RdbManager not initialized");
        return result;
    }

    if (result == RdbSaveResult::Ok) {
        std::ostringstream message;
        message << "Saved " << kv_store_->size()
                << " keys to the durable checkpoint";
        Log::Info(message.str());
    } else if (result == RdbSaveResult::DurabilityUnconfirmed) {
        Log::Error("Checkpoint was replaced but directory fsync failed; "
                   "durability is unconfirmed");
    } else {
        Log::Error("Failed to save the durable checkpoint");
    }
    return result;
}

void HttpServer::flush_interval() {
    if (!persistence_manager_) {
        return;
    }
    std::string error;
    if (!persistence_manager_->flush_interval(&error)) {
        Log::Error("INTERVAL WAL flush failed; stopping fail-closed: " + error);
        loop_->quit();
    }
}

void HttpServer::on_connection(const TcpConnectionPtr& conn) {
    if (!conn) {
        return;
    }
    if (conn->connected()) {
        auto session = std::make_shared<HttpSession>(conn);
        session->set_idle_timeout(idle_timeout_);
        session->set_request_callback(
            [this](const HttpRequest& req, HttpResponse* resp) {
                on_request(req, resp);
            });
        session->start();
        sessions_[conn->name()] = std::move(session);
    } else {
        sessions_.erase(conn->name());
    }
}

void HttpServer::on_message(const TcpConnectionPtr& conn, Buffer* buf) {
    auto it = sessions_.find(conn->name());
    if (it != sessions_.end()) {
        // on_message() can synchronously close the connection, which erases
        // this map entry. Keep the session alive for the full call.
        auto session = it->second;
        session->on_message(buf);
    }
}

void HttpServer::on_request(const HttpRequest& req, HttpResponse* resp) {
    if (!req.has_valid_method()) {
        set_bad_request(resp, "Invalid method token");
        return;
    }

    // A syntactically valid extension method cannot match any known route.
    // This check must precede route matching so, for example, TRACE /health
    // is 501 rather than a route-specific 405.
    if (req.method() == HttpRequest::kInvalid) {
        resp->set_status_code(HttpResponse::k501NotImplemented);
        resp->set_status_message("Not Implemented");
        set_plain_text(resp);
        resp->set_body("501 Not Implemented\n");
        return;
    }

    const std::string& path = req.path();
    if (path == "/health") {
        if (req.method() != HttpRequest::kGet) {
            method_not_allowed("GET", resp);
            return;
        }
        resp->set_status_code(HttpResponse::k200Ok);
        resp->set_status_message("OK");
        set_plain_text(resp);
        resp->set_body("OK\n");
        return;
    }

    if (path == "/echo") {
        if (req.method() != HttpRequest::kPost) {
            method_not_allowed("POST", resp);
            return;
        }
        resp->set_status_code(HttpResponse::k200Ok);
        resp->set_status_message("OK");
        resp->set_content_type("application/octet-stream");
        resp->set_body(req.body());
        return;
    }

    if (path == "/stats") {
        if (req.method() != HttpRequest::kGet) {
            method_not_allowed("GET", resp);
            return;
        }
        if (!persistence_manager_) {
            resp->set_status_code(HttpResponse::k503ServiceUnavailable);
            resp->set_status_message("Service Unavailable");
            set_plain_text(resp);
            resp->set_body("Persistence is not open\n");
            return;
        }
        resp->set_status_code(HttpResponse::k200Ok);
        resp->set_status_message("OK");
        resp->set_content_type("application/json");
        resp->set_body(
            persistence_stats_json(persistence_manager_->snapshot()) + "\n");
        return;
    }

    if (path == "/save") {
        if (req.method() != HttpRequest::kPost) {
            method_not_allowed("POST", resp);
            return;
        }
        if (save_rdb() == RdbSaveResult::Ok) {
            resp->set_status_code(HttpResponse::k200Ok);
            resp->set_status_message("OK");
            resp->set_content_type("application/json");
            std::ostringstream body;
            body << "{\"status\":\"ok\",\"keys\":" << kv_store_->size()
                 << "}\n";
            resp->set_body(body.str());
        } else {
            set_internal_error(resp, "Failed to save RDB checkpoint");
        }
        return;
    }

    if (has_kv_prefix(path)) {
        handle_kv(req, resp);
        return;
    }

    // Known methods on unknown paths are ordinary 404 responses.
    set_not_found(resp);
}

void HttpServer::handle_kv(const HttpRequest& req, HttpResponse* resp) {
    const std::string& path = req.path();
    const std::string key = path.substr(4);
    if (key.empty()) {
        set_bad_request(resp, "KV key must not be empty");
        return;
    }
    if (key.size() > RdbFormat::MAX_KEY_SIZE) {
        set_bad_request(resp, "KV key exceeds 1 KiB limit");
        return;
    }

    if (req.method() == HttpRequest::kGet) {
        const auto value = kv_store_->get(key);
        if (!value.has_value()) {
            set_not_found(resp);
            return;
        }
        resp->set_status_code(HttpResponse::k200Ok);
        resp->set_status_message("OK");
        resp->set_content_type("application/octet-stream");
        resp->set_body(value.value());
        return;
    }

    if (req.method() == HttpRequest::kPost) {
        if (persistence_manager_) {
            const MutationOutcome outcome =
                persistence_manager_->set(key, req.body());
            if (!outcome.ok()) {
                if (outcome.status == MutationStatus::kInvalidArgument) {
                    set_bad_request(resp, outcome.error);
                } else {
                    set_internal_error(resp, "Persistence write failed");
                }
                return;
            }
        } else {
            kv_store_->set(key, req.body());
        }
        resp->set_status_code(HttpResponse::k200Ok);
        resp->set_status_message("OK");
        set_plain_text(resp);
        resp->set_body("OK\n");
        return;
    }

    if (req.method() == HttpRequest::kDelete) {
        bool deleted = false;
        if (persistence_manager_) {
            const MutationOutcome outcome = persistence_manager_->del(key);
            if (!outcome.ok()) {
                if (outcome.status == MutationStatus::kInvalidArgument) {
                    set_bad_request(resp, outcome.error);
                } else {
                    set_internal_error(resp, "Persistence delete failed");
                }
                return;
            }
            deleted = outcome.changed;
        } else {
            deleted = kv_store_->del(key);
        }
        if (!deleted) {
            set_not_found(resp);
            return;
        }
        resp->set_status_code(HttpResponse::k200Ok);
        resp->set_status_message("OK");
        set_plain_text(resp);
        resp->set_body("OK\n");
        return;
    }

    method_not_allowed("GET, POST, DELETE", resp);
}

void HttpServer::method_not_allowed(const char* allow, HttpResponse* resp) {
    resp->set_status_code(HttpResponse::k405MethodNotAllowed);
    resp->set_status_message("Method Not Allowed");
    resp->add_header("Allow", allow);
    set_plain_text(resp);
    resp->set_body("405 Method Not Allowed\n");
}

} // namespace minidb
