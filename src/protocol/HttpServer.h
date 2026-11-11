#pragma once

#include "../net/TcpServer.h"
#include "HttpSession.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "../storage/KvStore.h"
#include "../storage/PersistenceManager.h"
#include "../storage/RdbManager.h"

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
    // Owner-loop only and idempotent. Stops accepting new connections,
    // drains accepted persistence mutations, publishes the final checkpoint,
    // stops the writer, invokes completion, then quits the owner loop.
    // All callbacks run on the owner loop.
    void begin_shutdown(ShutdownCompletion completion = {});
    bool shutdown_started() const {
        return shutdown_started_;
    }
    bool shutdown_complete() const {
        return shutdown_complete_;
    }
    RdbSaveResult shutdown_result() const {
        return shutdown_result_;
    }
    const std::string& shutdown_error() const {
        return shutdown_error_;
    }
    // Stops this server's TTL scanner lease.  Idempotent, and safe to call
    // from the destructor or after EventLoop::loop() returns before a final
    // synchronous RDB save.
    void stop();
    // Must be called before start(); replacing the store while the scanner is
    // running would invalidate this server's lease.
    void set_kv_store(std::shared_ptr<KvStore> store);
    const std::shared_ptr<KvStore>& kv_store() const {
        return kv_store_;
    }
    void set_idle_timeout(std::chrono::milliseconds timeout) {
        idle_timeout_ = timeout;
    }

    InetAddress listen_address() const {
        return server_.listen_address();
    }

    bool load_rdb();
    RdbSaveResult save_rdb();

    PersistenceManager* persistence_manager() const {
        return persistence_manager_.get();
    }

  private:
    void on_connection(const TcpConnectionPtr& conn);
    void on_message(const TcpConnectionPtr& conn, Buffer* buf);
    bool on_request(const HttpRequest& req, HttpResponse* resp,
                    const HttpSession::ResponseSender& sender);
    bool handle_kv(const HttpRequest& req, HttpResponse* resp,
                   const HttpSession::ResponseSender& sender);
    void method_not_allowed(const char* allow, HttpResponse* resp);
    void flush_interval();
    void try_start_persistence_shutdown();
    void finish_shutdown(RdbSaveResult result, const std::string& error);

    EventLoop* loop_;
    std::shared_ptr<KvStore> kv_store_;
    std::unique_ptr<RdbManager> rdb_manager_;
    std::string rdb_path_;
    DurabilityMode durability_mode_;
    std::unique_ptr<PersistenceManager> persistence_manager_;
    std::uint64_t interval_timer_id_ = 0;
    KvStore::ScannerLease scanner_lease_;
    bool started_ = false;
    bool shutdown_started_ = false;
    bool shutdown_complete_ = false;
    bool persistence_shutdown_pending_ = false;
    RdbSaveResult shutdown_result_ = RdbSaveResult::Failed;
    std::string shutdown_error_;
    std::vector<ShutdownCompletion> shutdown_completions_;
    std::uint64_t shutdown_retry_timer_id_ = 0;
    std::chrono::milliseconds idle_timeout_{30000};
    std::map<std::string, std::shared_ptr<HttpSession>> sessions_;
    // Destroy the server (and invoke close callbacks) before callback
    // dependencies such as sessions_.
    TcpServer server_;
};

} // namespace minidb
