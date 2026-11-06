#include "KvServer.h"
#include "../base/Log.h"

#include <sstream>
#include <utility>

namespace minidb {

KvServer::KvServer(EventLoop* loop, const InetAddress& listen_addr,
                   const std::string& rdb_path)
    : kv_store_(std::make_shared<KvStore>()),
      rdb_manager_(std::make_unique<RdbManager>(rdb_path)), rdb_path_(rdb_path),
      server_(loop, listen_addr, "KvServer") {
    server_.set_connection_callback(
        [this](const TcpConnectionPtr& conn) { on_connection(conn); });
    server_.set_message_callback(
        [this](const TcpConnectionPtr& conn, Buffer* buf) {
            on_message(conn, buf);
        });
}

KvServer::~KvServer() = default;

void KvServer::start() {
    server_.start();
}

bool KvServer::load_rdb() {
    if (!rdb_manager_) {
        Log::Warning("RdbManager not initialized");
        return false;
    }
    // load() itself maps only ENOENT to an empty database.  Calling exists()
    // first would reintroduce a TOCTOU window and silently hide other lookup
    // failures.
    const int count = rdb_manager_->load(*kv_store_);
    if (count < 0) {
        Log::Error("Failed to load RDB file");
        return false;
    }

    std::ostringstream message;
    message << "Loaded " << count << " keys from RDB file";
    Log::Info(message.str());
    return true;
}

RdbSaveResult KvServer::save_rdb() {
    if (!rdb_manager_) {
        Log::Error("RdbManager not initialized");
        return RdbSaveResult::Failed;
    }
    const RdbSaveResult result = rdb_manager_->save_checked(*kv_store_);
    if (result == RdbSaveResult::Ok) {
        std::ostringstream message;
        message << "Saved " << kv_store_->size() << " keys to RDB file";
        Log::Info(message.str());
    } else if (result == RdbSaveResult::DurabilityUnconfirmed) {
        Log::Error("RDB snapshot was replaced but directory fsync failed; "
                   "durability is unconfirmed");
    } else {
        Log::Error("Failed to save RDB file");
    }
    return result;
}

void KvServer::on_connection(const TcpConnectionPtr& conn) {
    if (!conn) {
        return;
    }
    if (conn->connected()) {
        auto session = std::make_shared<KvSession>(conn, kv_store_);
        session->set_save_callback([this]() { return save_rdb(); });
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
