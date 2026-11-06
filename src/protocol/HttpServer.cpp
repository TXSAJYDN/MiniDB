#include "HttpServer.h"

#include "../base/Log.h"
#include "../net/EventLoop.h"

#include <sstream>
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

} // namespace

HttpServer::HttpServer(EventLoop* loop, const InetAddress& listen_addr,
                       const std::string& rdb_path)
    : kv_store_(std::make_shared<KvStore>()),
      rdb_path_(rdb_path.empty() ? "minidb.rdb" : rdb_path),
      server_(loop, listen_addr, "HttpServer") {
    server_.set_connection_callback(
        [this](const TcpConnectionPtr& conn) { on_connection(conn); });
    server_.set_message_callback(
        [this](const TcpConnectionPtr& conn, Buffer* buf) {
            on_message(conn, buf);
        });

    if (!rdb_path_.empty()) {
        rdb_manager_ = std::make_unique<RdbManager>(rdb_path_);
    }
}

HttpServer::~HttpServer() = default;

void HttpServer::start() {
    server_.start();
}

bool HttpServer::load_rdb() {
    if (!rdb_manager_) {
        Log::Warning("RdbManager not initialized");
        return false;
    }
    const int count = rdb_manager_->load(*kv_store_);
    if (count < 0) {
        Log::Error("Failed to load RDB file");
        return false;
    }

    std::ostringstream oss;
    oss << "Loaded " << count << " keys from RDB file";
    Log::Info(oss.str());
    return true;
}

RdbSaveResult HttpServer::save_rdb() {
    if (!rdb_manager_) {
        Log::Error("RdbManager not initialized");
        return RdbSaveResult::Failed;
    }
    const RdbSaveResult result = rdb_manager_->save_checked(*kv_store_);
    if (result == RdbSaveResult::Ok) {
        std::ostringstream oss;
        oss << "Saved " << kv_store_->size() << " keys to RDB file";
        Log::Info(oss.str());
    } else if (result == RdbSaveResult::DurabilityUnconfirmed) {
        Log::Error("RDB snapshot was replaced but directory fsync failed; "
                   "durability is unconfirmed");
    } else {
        Log::Error("Failed to save RDB file");
    }
    return result;
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
            resp->set_status_code(HttpResponse::k500InternalServerError);
            resp->set_status_message("Internal Server Error");
            resp->set_content_type("application/json");
            resp->set_body(
                "{\"status\":\"error\",\"message\":\"Failed to save RDB\"}\n");
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
        kv_store_->set(key, req.body());
        resp->set_status_code(HttpResponse::k200Ok);
        resp->set_status_message("OK");
        set_plain_text(resp);
        resp->set_body("OK\n");
        return;
    }

    if (req.method() == HttpRequest::kDelete) {
        if (!kv_store_->del(key)) {
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
