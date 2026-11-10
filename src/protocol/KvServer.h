#pragma once
#include "../net/TcpServer.h"
#include "KvSession.h"
#include "../storage/KvStore.h"
#include "../storage/PersistenceManager.h"
#include "../storage/RdbManager.h"
#include <memory>
#include <map>
#include <string>

namespace minidb {

class KvServer {
  public:
    KvServer(EventLoop* loop, const InetAddress& listen_addr,
             const std::string& rdb_path = "minidb.rdb",
             DurabilityMode durability_mode = DurabilityMode::kSync);
    ~KvServer();

    void start();
    // Stops this server's TTL scanner lease.  Idempotent; call after the
    // EventLoop stops and before the final synchronous RDB save.
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

    EventLoop* loop_;
    std::shared_ptr<KvStore> kv_store_;
    std::unique_ptr<RdbManager> rdb_manager_;
    std::string rdb_path_;
    DurabilityMode durability_mode_;
    std::unique_ptr<PersistenceManager> persistence_manager_;
    std::uint64_t interval_timer_id_ = 0;
    KvStore::ScannerLease scanner_lease_;
    bool started_ = false;
    std::map<std::string, std::shared_ptr<KvSession>> sessions_;
    // Destroy the server (and invoke close callbacks) before the session map
    // and storage dependencies it refers to.
    TcpServer server_;
};

} // namespace minidb
