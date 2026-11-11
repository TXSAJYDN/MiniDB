#pragma once
#include "../net/TcpServer.h"
#include "KvSession.h"
#include "../storage/KvStore.h"
#include "../storage/PersistenceManager.h"
#include "../storage/RdbManager.h"
#include <memory>
#include <map>
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
        return shutdown_complete_;
    }
    RdbSaveResult shutdown_result() const {
        return shutdown_result_;
    }
    const std::string& shutdown_error() const {
        return shutdown_error_;
    }
    // Stops this server's TTL scanner lease.  Idempotent.  begin_shutdown()
    // already performs this step.
    void stop();
    // Must be called before start().
    void set_kv_store(std::shared_ptr<KvStore> store);
    InetAddress listen_address() const {
        return server_.listen_address();
    }
    bool load_rdb();
    // Returns Ok only after the snapshot and its parent directory were made
    // durable.  DurabilityUnconfirmed is intentionally not treated as success
    // by the process exit path.
    RdbSaveResult save_rdb();

    PersistenceManager* persistence_manager() const {
        return persistence_manager_.get();
    }

  private:
    void on_connection(const TcpConnectionPtr& conn);
    void on_message(const TcpConnectionPtr& conn, Buffer* buf);
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
    std::map<std::string, std::shared_ptr<KvSession>> sessions_;
    // Destroy the server (and invoke close callbacks) before the session map
    // and storage dependencies it refers to.
    TcpServer server_;
};

} // namespace minidb
