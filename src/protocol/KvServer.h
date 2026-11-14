#pragma once

#include "../net/TcpServer.h"
#include "KvSession.h"
#include "ServerPersistenceRuntime.h"
#include "../storage/KvStore.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace minidb {

class KvServer {
  public:
    using ShutdownCompletion =
        std::function<void(RdbSaveResult, const std::string&)>;

    KvServer(EventLoop* loop, const InetAddress& listen_addr,
             const std::string& rdb_path = "minidb.rdb",
             DurabilityMode durability_mode = DurabilityMode::kSync);
    ~KvServer();

    void start();
    // Owner-loop only and idempotent.  Stops admission of new connections,
    // closes live sessions, drains the group-commit writer, publishes the
    // final checkpoint, stops the writer, then invokes completion and quits
    // the owner loop.  All callbacks run on the owner loop.
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
    // Idempotent.  begin_shutdown() already performs this step.
    void stop();
    // Must be called before start().
    void set_kv_store(std::shared_ptr<KvStore> store);
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
    void run_agent_gc();
    void finish_shutdown(RdbSaveResult result, const std::string& error);

    EventLoop* loop_;
    ServerPersistenceRuntime runtime_;
    bool started_ = false;
    std::uint64_t agent_gc_timer_id_ = 0;
    bool shutdown_started_ = false;
    std::vector<ShutdownCompletion> shutdown_completions_;
    std::map<std::string, std::shared_ptr<KvSession>> sessions_;
    // Destroy the server (and invoke close callbacks) before the session map
    // and storage dependencies it refers to.
    TcpServer server_;
};

} // namespace minidb
