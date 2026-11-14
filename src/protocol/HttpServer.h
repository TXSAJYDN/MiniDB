#pragma once

#include "../net/TcpServer.h"
#include "HttpSession.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "ServerPersistenceRuntime.h"
#include "../storage/KvStore.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace minidb {
class EventLoop;

class HttpServer {
  public:
    using ShutdownCompletion =
        std::function<void(RdbSaveResult, const std::string&)>;

    HttpServer(EventLoop* loop, const InetAddress& listen_addr,
               const std::string& rdb_path = "",
               DurabilityMode durability_mode = DurabilityMode::kSync);
    ~HttpServer();
    void start();
    // Owner-loop only and idempotent.  Stops accepting new connections,
    // drains accepted persistence mutations, publishes the final checkpoint,
    // stops the writer, invokes completion, then quits the owner loop.
    // All callbacks run on the owner loop.
    void begin_shutdown(ShutdownCompletion completion = {});
    bool shutdown_started() const {
        return shutdown_started_;
    }
    bool shutdown_complete() const {
        return runtime_.shutdown_complete();
    }
    RdbSaveResult shutdown_result() const {
        return runtime_.shutdown_result();
    }
    const std::string& shutdown_error() const {
        return runtime_.shutdown_error();
    }
    // Cancels runtime-owned timers and releases the TTL scanner lease.
    // Idempotent, and safe to call from the destructor.
    void stop();
    // Must be called before start(); replacing the store while the scanner is
    // running would invalidate this server's lease.
    void set_kv_store(std::shared_ptr<KvStore> store);
    const std::shared_ptr<KvStore>& kv_store() const {
        return runtime_.store();
    }
    void set_idle_timeout(std::chrono::milliseconds timeout) {
        idle_timeout_ = timeout;
    }

    InetAddress listen_address() const {
        return server_.listen_address();
    }

    bool load_rdb();

    PersistenceManager* persistence_manager() const {
        return runtime_.persistence_manager();
    }

  private:
    void on_connection(const TcpConnectionPtr& conn);
    void on_message(const TcpConnectionPtr& conn, Buffer* buf);
    bool on_request(const HttpRequest& req, HttpResponse* resp,
                    const HttpSession::ResponseSender& sender);
    bool handle_kv(const HttpRequest& req, HttpResponse* resp,
                   const HttpSession::ResponseSender& sender);
    void method_not_allowed(const char* allow, HttpResponse* resp);
    void finish_shutdown(RdbSaveResult result, const std::string& error);

    EventLoop* loop_;
    ServerPersistenceRuntime runtime_;
    bool started_ = false;
    std::chrono::milliseconds idle_timeout_{30000};
    bool shutdown_started_ = false;
    std::vector<ShutdownCompletion> shutdown_completions_;
    std::map<std::string, std::shared_ptr<HttpSession>> sessions_;
    // Destroy the server (and invoke close callbacks) before callback
    // dependencies such as sessions_.
    TcpServer server_;
};

} // namespace minidb
