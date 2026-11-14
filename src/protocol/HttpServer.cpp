#include "HttpServer.h"
#include "Json.h"
#include "PersistenceStatsJson.h"

#include "../base/Log.h"
#include "../net/EventLoop.h"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace minidb {
namespace {

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
    resp->set_body("{\"status\":\"error\",\"message\":\"" +
                   json_escape(message) + "\"}\n");
}

void set_service_unavailable(HttpResponse* resp, const std::string& message) {
    resp->set_status_code(HttpResponse::k503ServiceUnavailable);
    resp->set_status_message("Service Unavailable");
    resp->set_content_type("application/json");
    resp->set_body("{\"status\":\"error\",\"message\":\"" +
                   json_escape(message) + "\"}\n");
}

void set_conflict(HttpResponse* resp) {
    resp->set_status_code(HttpResponse::k409Conflict);
    resp->set_status_message("Conflict");
    resp->set_content_type("application/json");
    resp->set_body(
        "{\"status\":\"error\",\"message\":\"Transaction conflict\"}\n");
}

void set_mutation_failure(HttpResponse* resp, MutationStatus status,
                          const std::string& error) {
    switch (status) {
    case MutationStatus::kInvalidArgument:
        set_bad_request(resp, error.empty() ? "Invalid argument" : error);
        return;
    case MutationStatus::kResourceLimit:
        resp->set_status_code(HttpResponse::k413RequestEntityTooLarge);
        resp->set_status_message("Request Entity Too Large");
        resp->set_content_type("application/json");
        resp->set_body("{\"status\":\"error\",\"message\":\"Persistence "
                       "resource limit exceeded\"}\n");
        return;
    case MutationStatus::kNotReady:
        set_service_unavailable(resp, "Persistence is not accepting writes");
        return;
    case MutationStatus::kNoTransaction:
        set_internal_error(resp, "Transaction is not active");
        return;
    case MutationStatus::kConflict:
        set_conflict(resp);
        return;
    case MutationStatus::kIoError:
    case MutationStatus::kOk:
        break;
    }
    set_internal_error(resp, "Persistence write failed");
}

void set_admission_failure(HttpResponse* resp,
                           const MutationAdmission& admission) {
    switch (admission.status) {
    case MutationAdmissionStatus::kInvalidArgument:
        set_bad_request(resp, admission.error.empty() ? "Invalid argument"
                                                      : admission.error);
        return;
    case MutationAdmissionStatus::kResourceLimit:
        resp->set_status_code(HttpResponse::k413RequestEntityTooLarge);
        resp->set_status_message("Request Entity Too Large");
        resp->set_content_type("application/json");
        resp->set_body("{\"status\":\"error\",\"message\":\"Persistence "
                       "resource limit exceeded\"}\n");
        return;
    case MutationAdmissionStatus::kIoError:
        set_internal_error(resp, "Persistence write failed");
        return;
    case MutationAdmissionStatus::kNoTransaction:
        set_internal_error(resp, "Transaction is not active");
        return;
    case MutationAdmissionStatus::kConflict:
        set_conflict(resp);
        return;
    case MutationAdmissionStatus::kNotReady:
    case MutationAdmissionStatus::kShuttingDown:
    case MutationAdmissionStatus::kAccepted:
        break;
    }
    set_service_unavailable(resp, "Persistence is not accepting writes");
}

} // namespace

HttpServer::HttpServer(EventLoop* loop, const InetAddress& listen_addr,
                       const std::string& rdb_path,
                       DurabilityMode durability_mode)
    : loop_(loop), runtime_(loop, rdb_path, durability_mode),
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
    runtime_.start();
    try {
        server_.start();
        runtime_.start_interval_timer(
            [this](const std::string&) { begin_shutdown(); });
    } catch (...) {
        runtime_.stop();
        throw;
    }
    started_ = true;
}

void HttpServer::stop() {
    runtime_.stop();
    started_ = false;
}

void HttpServer::begin_shutdown(ShutdownCompletion completion) {
    loop_->assert_in_loop_thread();
    if (runtime_.shutdown_complete()) {
        if (completion) {
            completion(runtime_.shutdown_result(), runtime_.shutdown_error());
        }
        return;
    }
    if (completion) {
        shutdown_completions_.push_back(std::move(completion));
    }
    if (shutdown_started_) {
        return;
    }
    shutdown_started_ = true;

    server_.stop();
    started_ = false;

    runtime_.begin_shutdown(
        [this](RdbSaveResult result, const std::string& error) {
            finish_shutdown(result, error);
        });
}

void HttpServer::finish_shutdown(RdbSaveResult result,
                                 const std::string& error) {
    std::vector<ShutdownCompletion> completions =
        std::move(shutdown_completions_);
    shutdown_completions_.clear();
    for (ShutdownCompletion& callback : completions) {
        if (!callback) {
            continue;
        }
        try {
            callback(result, error);
        } catch (const std::exception& ex) {
            Log::Error(std::string("shutdown completion threw: ") + ex.what());
        } catch (...) {
            Log::Error("shutdown completion threw an unknown exception");
        }
    }
    loop_->quit();
}

void HttpServer::set_kv_store(std::shared_ptr<KvStore> store) {
    runtime_.set_store(std::move(store));
}

bool HttpServer::load_rdb() {
    return runtime_.load();
}
void HttpServer::on_connection(const TcpConnectionPtr& conn) {
    if (!conn) {
        return;
    }
    if (conn->connected()) {
        auto session = std::make_shared<HttpSession>(conn);
        session->set_idle_timeout(idle_timeout_);
        session->set_async_request_callback(
            [this](const HttpRequest& req, HttpResponse* resp,
                   HttpSession::ResponseSender sender) {
                return on_request(req, resp, std::move(sender));
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

bool HttpServer::on_request(const HttpRequest& req, HttpResponse* resp,
                            const HttpSession::ResponseSender& sender) {
    if (!req.has_valid_method()) {
        set_bad_request(resp, "Invalid method token");
        return false;
    }

    // A syntactically valid extension method cannot match any known route.
    // This check must precede route matching so, for example, TRACE /health
    // is 501 rather than a route-specific 405.
    if (req.method() == HttpRequest::kInvalid) {
        resp->set_status_code(HttpResponse::k501NotImplemented);
        resp->set_status_message("Not Implemented");
        set_plain_text(resp);
        resp->set_body("501 Not Implemented\n");
        return false;
    }

    const std::string& path = req.path();
    const bool kv_ttl_query =
        has_kv_prefix(path) && (req.method() == HttpRequest::kGet ||
                                req.method() == HttpRequest::kHead);
    if (req.has_query() && !kv_ttl_query) {
        set_bad_request(resp, "Query parameters are not supported for this "
                              "request");
        return false;
    }
    if (path == "/health") {
        if (req.method() != HttpRequest::kGet &&
            req.method() != HttpRequest::kHead) {
            method_not_allowed("GET, HEAD", resp);
            return false;
        }
        resp->set_status_code(HttpResponse::k200Ok);
        resp->set_status_message("OK");
        set_plain_text(resp);
        resp->set_body("OK\n");
        return false;
    }

    if (path == "/echo") {
        if (req.method() != HttpRequest::kPost) {
            method_not_allowed("POST", resp);
            return false;
        }
        resp->set_status_code(HttpResponse::k200Ok);
        resp->set_status_message("OK");
        resp->set_content_type("application/octet-stream");
        resp->set_body(req.body());
        return false;
    }

    if (path == "/stats") {
        if (req.method() != HttpRequest::kGet &&
            req.method() != HttpRequest::kHead) {
            method_not_allowed("GET, HEAD", resp);
            return false;
        }
        if (!runtime_.persistence_manager()) {
            resp->set_status_code(HttpResponse::k503ServiceUnavailable);
            resp->set_status_message("Service Unavailable");
            set_plain_text(resp);
            resp->set_body("Persistence is not open\n");
            return false;
        }
        resp->set_status_code(HttpResponse::k200Ok);
        resp->set_status_message("OK");
        resp->set_content_type("application/json");
        resp->set_body(
            persistence_stats_json(runtime_.persistence_manager()->snapshot()) +
            "\n");
        return false;
    }

    if (path == "/save") {
        if (req.method() != HttpRequest::kPost) {
            method_not_allowed("POST", resp);
            return false;
        }
        if (!runtime_.persistence_manager()) {
            if (runtime_.save_snapshot() == RdbSaveResult::Ok) {
                resp->set_status_code(HttpResponse::k200Ok);
                resp->set_status_message("OK");
                resp->set_content_type("application/json");
                std::ostringstream body;
                body << "{\"status\":\"ok\",\"keys\":"
                     << runtime_.store()->size() << "}\n";
                resp->set_body(body.str());
            } else {
                set_internal_error(resp, "Failed to save RDB checkpoint");
            }
            return false;
        }
        if (!sender) {
            set_internal_error(resp, "Failed to save RDB checkpoint");
            return false;
        }

        // A timer flush or a concurrent SAVE can briefly own the writer
        // control path.  Retry only that transient conflict; never report a
        // successful checkpoint before its completion arrives.
        auto attempt = std::make_shared<std::function<void()>>();
        // The retry timer owns the callback.  The callback itself must only
        // hold a weak reference, otherwise the shared_ptr forms an
        // unreachable self-cycle that also pins HttpSession weak-control
        // state until process exit.
        std::weak_ptr<std::function<void()>> weak_attempt = attempt;
        *attempt = [this, weak_attempt, sender]() mutable {
            if (!runtime_.persistence_manager()) {
                HttpResponse failed(false);
                set_internal_error(&failed, "Failed to save RDB checkpoint");
                sender(std::move(failed));
                return;
            }
            const PersistenceControlResult result =
                runtime_.persistence_manager()->checkpoint_async_result(
                    [this,
                     sender](RdbSaveResult checkpoint_result,
                             const std::string& completion_error) mutable {
                        HttpResponse response(false);
                        if (checkpoint_result == RdbSaveResult::Ok) {
                            response.set_status_code(HttpResponse::k200Ok);
                            response.set_status_message("OK");
                            response.set_content_type("application/json");
                            std::ostringstream body;
                            body << "{\"status\":\"ok\",\"keys\":"
                                 << runtime_.store()->size() << "}\n";
                            response.set_body(body.str());
                        } else {
                            if (!completion_error.empty()) {
                                Log::Error("SAVE failed: " + completion_error);
                            }
                            set_internal_error(&response,
                                               "Failed to save RDB checkpoint");
                        }
                        sender(std::move(response));
                    });
            if (result.accepted()) {
                return;
            }
            if (result.status == PersistenceControlStatus::kBusy) {
                if (auto retry = weak_attempt.lock()) {
                    loop_->run_after(10, [retry]() { (*retry)(); });
                }
                return;
            }
            Log::Error("SAVE could not start: " + result.error);
            HttpResponse failed(false);
            set_internal_error(&failed, "Failed to save RDB checkpoint");
            sender(std::move(failed));
        };
        (*attempt)();
        return true;
    }

    if (has_kv_prefix(path)) {
        return handle_kv(req, resp, sender);
    }

    // Known methods on unknown paths are ordinary 404 responses.
    set_not_found(resp);
    return false;
}

bool HttpServer::handle_kv(const HttpRequest& req, HttpResponse* resp,
                           const HttpSession::ResponseSender& sender) {
    const std::string& path = req.path();
    const std::string key = path.substr(4);
    if (key.empty()) {
        set_bad_request(resp, "KV key must not be empty");
        return false;
    }
    if (key.size() > RdbFormat::MAX_KEY_SIZE) {
        set_bad_request(resp, "KV key exceeds 1 KiB limit");
        return false;
    }
    if (AgentRecordCodec::is_reserved_key(key)) {
        set_bad_request(resp, "KV key is in the reserved agent namespace");
        return false;
    }

    if (req.method() == HttpRequest::kGet ||
        req.method() == HttpRequest::kHead) {
        if (req.has_query()) {
            if (req.query() != "ttl=1") {
                set_bad_request(resp, "Only the ttl=1 query is supported");
                return false;
            }
            const KvStore::TtlResult ttl = runtime_.store()->ttl_info(key);
            if (ttl.state == KvStore::TtlState::kMissing) {
                set_not_found(resp);
                return false;
            }
            resp->set_status_code(HttpResponse::k200Ok);
            resp->set_status_message("OK");
            resp->set_content_type("application/json");
            std::ostringstream body;
            body << "{\"ttl_ms\":";
            if (ttl.state == KvStore::TtlState::kNoExpiry) {
                body << "null";
            } else {
                body << ttl.remaining_ms;
            }
            body << "}\n";
            resp->set_body(body.str());
            return false;
        }
        const auto value = runtime_.store()->get(key);
        if (!value.has_value()) {
            set_not_found(resp);
            return false;
        }
        resp->set_status_code(HttpResponse::k200Ok);
        resp->set_status_message("OK");
        resp->set_content_type("application/octet-stream");
        resp->set_body(value.value());
        return false;
    }

    if (req.method() == HttpRequest::kPost) {
        if (runtime_.persistence_manager()) {
            MutationAdmission admission =
                runtime_.persistence_manager()->set_async(
                    key, req.body(), [sender](MutationOutcome outcome) mutable {
                        HttpResponse response(false);
                        if (!outcome.ok()) {
                            set_mutation_failure(&response, outcome.status,
                                                 outcome.error);
                        } else {
                            response.set_status_code(HttpResponse::k200Ok);
                            response.set_status_message("OK");
                            set_plain_text(&response);
                            response.set_body("OK\n");
                        }
                        sender(std::move(response));
                    });
            if (!admission.accepted()) {
                set_admission_failure(resp, admission);
                return false;
            }
            return true;
        }
        runtime_.store()->set(key, req.body());
        resp->set_status_code(HttpResponse::k200Ok);
        resp->set_status_message("OK");
        set_plain_text(resp);
        resp->set_body("OK\n");
        return false;
    }

    if (req.method() == HttpRequest::kDelete) {
        if (runtime_.persistence_manager()) {
            MutationAdmission admission =
                runtime_.persistence_manager()->del_async(
                    key, [sender](MutationOutcome outcome) mutable {
                        HttpResponse response(false);
                        if (!outcome.ok()) {
                            set_mutation_failure(&response, outcome.status,
                                                 outcome.error);
                        } else if (!outcome.changed) {
                            set_not_found(&response);
                        } else {
                            response.set_status_code(HttpResponse::k200Ok);
                            response.set_status_message("OK");
                            set_plain_text(&response);
                            response.set_body("OK\n");
                        }
                        sender(std::move(response));
                    });
            if (!admission.accepted()) {
                set_admission_failure(resp, admission);
                return false;
            }
            return true;
        }
        const bool deleted = runtime_.store()->del(key);
        if (!deleted) {
            set_not_found(resp);
            return false;
        }
        resp->set_status_code(HttpResponse::k200Ok);
        resp->set_status_message("OK");
        set_plain_text(resp);
        resp->set_body("OK\n");
        return false;
    }

    method_not_allowed("GET, HEAD, POST, DELETE", resp);
    return false;
}

void HttpServer::method_not_allowed(const char* allow, HttpResponse* resp) {
    resp->set_status_code(HttpResponse::k405MethodNotAllowed);
    resp->set_status_message("Method Not Allowed");
    resp->add_header("Allow", allow);
    set_plain_text(resp);
    resp->set_body("405 Method Not Allowed\n");
}

} // namespace minidb
