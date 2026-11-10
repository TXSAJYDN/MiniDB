#include "KvServer.h"
#include "../base/Log.h"
#include "../net/EventLoop.h"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace minidb {
namespace {

constexpr std::int64_t kIntervalFlushMs = 100;

} // namespace

KvServer::KvServer(EventLoop* loop, const InetAddress& listen_addr,
                   const std::string& rdb_path, DurabilityMode durability_mode)
    : loop_(loop), kv_store_(std::make_shared<KvStore>()),
      rdb_manager_(std::make_unique<RdbManager>(rdb_path.empty() ? "minidb.rdb"
                                                                 : rdb_path)),
      rdb_path_(rdb_path.empty() ? "minidb.rdb" : rdb_path),
      durability_mode_(durability_mode),
      server_(loop, listen_addr, "KvServer") {
    if (loop_ == nullptr) {
        throw std::invalid_argument("KvServer requires a non-null EventLoop");
    }
    server_.set_connection_callback(
        [this](const TcpConnectionPtr& conn) { on_connection(conn); });
    server_.set_message_callback(
        [this](const TcpConnectionPtr& conn, Buffer* buf) {
            on_message(conn, buf);
        });
}

KvServer::~KvServer() {
    stop();
}

void KvServer::start() {
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

void KvServer::stop() {
    if (interval_timer_id_ != 0) {
        loop_->cancel_timer(interval_timer_id_);
        interval_timer_id_ = 0;
    }
    scanner_lease_.reset();
    started_ = false;
}

void KvServer::set_kv_store(std::shared_ptr<KvStore> store) {
    if (!store) {
        throw std::invalid_argument("KvServer requires a non-null KvStore");
    }
    if (started_ || scanner_lease_) {
        throw std::logic_error(
            "KvServer::set_kv_store must precede KvServer::start");
    }
    if (persistence_manager_) {
        throw std::logic_error("KvServer::set_kv_store must precede load_rdb");
    }
    kv_store_ = std::move(store);
}

bool KvServer::load_rdb() {
    if (!rdb_manager_) {
        Log::Warning("RdbManager not initialized");
        return false;
    }
    if (persistence_manager_) {
        return persistence_manager_->opened();
    }
    if (started_) {
        Log::Error("Cannot load persistence state after KvServer::start");
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

RdbSaveResult KvServer::save_rdb() {
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

void KvServer::flush_interval() {
    if (!persistence_manager_) {
        return;
    }
    std::string error;
    if (!persistence_manager_->flush_interval(&error)) {
        Log::Error("INTERVAL WAL flush failed; stopping fail-closed: " + error);
        loop_->quit();
    }
}

void KvServer::on_connection(const TcpConnectionPtr& conn) {
    if (!conn) {
        return;
    }
    if (conn->connected()) {
        auto session = std::make_shared<KvSession>(conn, kv_store_);
        session->set_save_callback([this]() { return save_rdb(); });
        if (persistence_manager_) {
            session->set_persistence_manager(persistence_manager_.get());
        }
        session->start();
        sessions_[conn->name()] = std::move(session);
    } else {
        sessions_.erase(conn->name());
    }
}

void KvServer::on_message(const TcpConnectionPtr& conn, Buffer* buf) {
    auto it = sessions_.find(conn->name());
    if (it != sessions_.end()) {
        // Synchronous error handling may erase the map entry.
        auto session = it->second;
        session->on_message(buf);
    }
}

} // namespace minidb
