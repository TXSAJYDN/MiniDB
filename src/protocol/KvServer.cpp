#include "KvServer.h"
#include "../base/Log.h"
#include "../net/EventLoop.h"

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

namespace minidb {
namespace {

constexpr std::int64_t kAgentGcIntervalMs = 1000;

} // namespace

KvServer::KvServer(EventLoop* loop, const InetAddress& listen_addr,
                   const std::string& rdb_path, DurabilityMode durability_mode)
    : loop_(loop), runtime_(loop, rdb_path, durability_mode),
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
    runtime_.start();
    try {
        server_.start();
        runtime_.start_interval_timer(
            [this](const std::string&) { begin_shutdown(); });
        if (persistence_manager() != nullptr &&
            persistence_manager()->opened()) {
            agent_gc_timer_id_ = loop_->run_every(kAgentGcIntervalMs,
                                                  [this]() { run_agent_gc(); });
        }
    } catch (...) {
        if (agent_gc_timer_id_ != 0) {
            loop_->cancel_timer(agent_gc_timer_id_);
            agent_gc_timer_id_ = 0;
        }
        runtime_.stop();
        throw;
    }
    started_ = true;
}

void KvServer::stop() {
    if (agent_gc_timer_id_ != 0) {
        loop_->cancel_timer(agent_gc_timer_id_);
        agent_gc_timer_id_ = 0;
    }
    runtime_.stop();
    started_ = false;
}

void KvServer::begin_shutdown(ShutdownCompletion completion) {
    loop_->assert_in_loop_thread();
    if (runtime_.shutdown_complete()) {
        if (completion) {
            completion(runtime_.shutdown_result(), runtime_.shutdown_error());
        }
        return;
    }
    if (completion) {
        shutdown_completions_.push_back(std::move(completion));
    }
    if (shutdown_started_) {
        return;
    }
    shutdown_started_ = true;

    if (agent_gc_timer_id_ != 0) {
        loop_->cancel_timer(agent_gc_timer_id_);
        agent_gc_timer_id_ = 0;
    }
    server_.stop();
    started_ = false;

    runtime_.begin_shutdown(
        [this](RdbSaveResult result, const std::string& error) {
            finish_shutdown(result, error);
        });
}

void KvServer::finish_shutdown(RdbSaveResult result, const std::string& error) {
    std::vector<ShutdownCompletion> completions =
        std::move(shutdown_completions_);
    shutdown_completions_.clear();
    for (ShutdownCompletion& callback : completions) {
        if (!callback) {
            continue;
        }
        try {
            callback(result, error);
        } catch (const std::exception& ex) {
            Log::Error(std::string("shutdown completion threw: ") + ex.what());
        } catch (...) {
            Log::Error("shutdown completion threw an unknown exception");
        }
    }
    loop_->quit();
}

void KvServer::set_kv_store(std::shared_ptr<KvStore> store) {
    runtime_.set_store(std::move(store));
}

bool KvServer::load_rdb() {
    return runtime_.load();
}

void KvServer::run_agent_gc() {
    PersistenceManager* manager = persistence_manager();
    if (shutdown_started_ || runtime_.shutdown_complete() ||
        manager == nullptr) {
        return;
    }
    const PersistenceControlResult result = manager->agent_gc_async_result();
    if (result.accepted() || result.status == PersistenceControlStatus::kBusy ||
        result.status == PersistenceControlStatus::kNotReady ||
        result.status == PersistenceControlStatus::kShuttingDown) {
        return;
    }
    Log::Error(
        "Agent GC failed; stopping fail-closed: " +
        (result.error.empty() ? std::string("unknown error") : result.error));
    begin_shutdown();
}

void KvServer::on_connection(const TcpConnectionPtr& conn) {
    if (!conn) {
        return;
    }
    if (conn->connected()) {
        auto session = std::make_shared<KvSession>(conn, runtime_.store());
        if (PersistenceManager* manager = persistence_manager()) {
            session->set_persistence_manager(manager);
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
