#pragma once

#include "../net/TcpServer.h"
#include "HttpSession.h"
#include "HttpRequest.h"
#include "HttpResponse.h"
#include "../storage/KvStore.h"
#include "../storage/RdbManager.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace minidb {
class EventLoop;

class HttpServer {
  public:
    HttpServer(EventLoop* loop, const InetAddress& listen_addr,
               const std::string& rdb_path = "");
    ~HttpServer();
    void start();
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

  private:
    void on_connection(const TcpConnectionPtr& conn);
    void on_message(const TcpConnectionPtr& conn, Buffer* buf);
    void on_request(const HttpRequest& req, HttpResponse* resp);
    void handle_kv(const HttpRequest& req, HttpResponse* resp);
    void method_not_allowed(const char* allow, HttpResponse* resp);

    std::shared_ptr<KvStore> kv_store_;
    std::unique_ptr<RdbManager> rdb_manager_;
    std::string rdb_path_;
    KvStore::ScannerLease scanner_lease_;
    bool started_ = false;
    std::chrono::milliseconds idle_timeout_{30000};
    std::map<std::string, std::shared_ptr<HttpSession>> sessions_;
    // Destroy the server (and invoke close callbacks) before callback
    // dependencies such as sessions_.
    TcpServer server_;
};

} // namespace minidb
