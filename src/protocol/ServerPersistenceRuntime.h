#pragma once

#include "../storage/KvStore.h"
#include "../storage/PersistenceManager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace minidb {
class EventLoop;
class RdbManager;

// Owns the persistence lifetime shared by the HTTP and binary KV frontends:
// the store, PersistenceManager, TTL scanner lease, INTERVAL flush timer, and
// the graceful-shutdown state machine.
//
// The runtime deliberately knows nothing about listeners or EventLoop exit:
// each frontend starts its own TcpServer after start() and calls quit() only
// from the completion it passes to begin_shutdown().  This keeps the
// persistence shutdown sequence in one place without coupling it to either
// wire protocol.
class ServerPersistenceRuntime {
  public:
    using ShutdownCompletion =
        std::function<void(RdbSaveResult, const std::string&)>;
    using FailClosedCallback = std::function<void(const std::string&)>;

    ServerPersistenceRuntime(EventLoop* loop, std::string rdb_path,
                             DurabilityMode durability_mode);
    ~ServerPersistenceRuntime();

    ServerPersistenceRuntime(const ServerPersistenceRuntime&) = delete;
    ServerPersistenceRuntime&
    operator=(const ServerPersistenceRuntime&) = delete;

    // Must precede start().  Replacing the store while the scanner is running
    // would invalidate the runtime's scanner lease.
    void set_store(std::shared_ptr<KvStore> store);
    const std::shared_ptr<KvStore>& store() const {
        return kv_store_;
    }

    bool load();

    // Synchronous snapshot save for frontends that run without an open
    // persistence manager.  Keeps the legacy /save contract available.
    RdbSaveResult save_snapshot();

    // Starts the group-commit writer, then acquires the store scanner lease.
    // The frontend must call this before starting its listener.
    void start();
    bool started() const {
        return started_;
    }

    // Installs the INTERVAL durability flush timer.  Call after the listener
    // starts so a listener failure cannot leave a timer behind.  The callback
    // runs on the owner loop when an interval flush fails closed.
    void start_interval_timer(FailClosedCallback fail_closed);

    // Cancels runtime-owned timers and releases the scanner lease.  Idempotent.
    // Does not stop the group-commit writer; persistence shutdown owns that.
    void stop();

    // Starts the persistence shutdown state machine.  Completion runs on the
    // owner loop after the final checkpoint, or immediately when persistence
    // was never opened.  Additional completions registered while shutdown is
    // in flight are appended and invoked once the terminal result is known.
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

    PersistenceManager* persistence_manager() const {
        return persistence_manager_.get();
    }

  private:
    void flush_interval();
    void try_start_persistence_shutdown();
    void finish_shutdown(RdbSaveResult result, const std::string& error);
    void cancel_interval_timer();
    void cancel_shutdown_retry_timer();

    EventLoop* loop_;
    std::shared_ptr<KvStore> kv_store_;
    std::unique_ptr<PersistenceManager> persistence_manager_;
    std::string rdb_path_;
    std::unique_ptr<RdbManager> rdb_manager_;
    DurabilityMode durability_mode_;
    bool started_ = false;

    std::uint64_t interval_timer_id_ = 0;
    FailClosedCallback interval_fail_closed_;
    KvStore::ScannerLease scanner_lease_;

    bool shutdown_started_ = false;
    bool shutdown_complete_ = false;
    bool persistence_shutdown_pending_ = false;
    RdbSaveResult shutdown_result_ = RdbSaveResult::Failed;
    std::string shutdown_error_;
    std::vector<ShutdownCompletion> shutdown_completions_;
    std::uint64_t shutdown_retry_timer_id_ = 0;
};

} // namespace minidb
