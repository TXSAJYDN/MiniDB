#include "ServerPersistenceRuntime.h"

#include "../base/Log.h"
#include "../net/EventLoop.h"
#include "../storage/RdbManager.h"

#include <sstream>
#include <stdexcept>
#include <utility>

namespace minidb {
namespace {

constexpr std::int64_t kIntervalFlushMs = 100;
constexpr std::int64_t kShutdownRetryMs = 10;

} // namespace

ServerPersistenceRuntime::ServerPersistenceRuntime(
    EventLoop* loop, std::string rdb_path, DurabilityMode durability_mode)
    : loop_(loop), kv_store_(std::make_shared<KvStore>()),
      rdb_path_(rdb_path.empty() ? "minidb.rdb" : std::move(rdb_path)),
      rdb_manager_(std::make_unique<RdbManager>(rdb_path_)),
      durability_mode_(durability_mode) {
    if (loop_ == nullptr) {
        throw std::invalid_argument(
            "ServerPersistenceRuntime requires a non-null EventLoop");
    }
}

ServerPersistenceRuntime::~ServerPersistenceRuntime() {
    stop();
}

void ServerPersistenceRuntime::set_store(std::shared_ptr<KvStore> store) {
    if (!store) {
        throw std::invalid_argument(
            "ServerPersistenceRuntime requires a non-null KvStore");
    }
    if (started_ || scanner_lease_) {
        throw std::logic_error(
            "set_store must precede ServerPersistenceRuntime::start");
    }
    if (persistence_manager_) {
        throw std::logic_error("set_store must precede load");
    }
    kv_store_ = std::move(store);
}

bool ServerPersistenceRuntime::load() {
    if (persistence_manager_) {
        return persistence_manager_->opened();
    }
    if (started_) {
        Log::Error(
            "Cannot load persistence state after persistence runtime start");
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

RdbSaveResult ServerPersistenceRuntime::save_snapshot() {
    RdbSaveResult result = RdbSaveResult::Failed;
    std::string error;
    if (persistence_manager_) {
        result = persistence_manager_->checkpoint(&error);
    } else if (rdb_manager_) {
        result = rdb_manager_->save_checked(*kv_store_);
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
        Log::Error("Failed to save the durable checkpoint" +
                   (error.empty() ? std::string() : ": " + error));
    }
    return result;
}

void ServerPersistenceRuntime::start() {
    if (started_) {
        return;
    }
    if (persistence_manager_ && !persistence_manager_->group_commit_started()) {
        std::string error;
        if (!persistence_manager_->start_group_commit(
                [this](std::function<void()> work) {
                    loop_->queue_in_loop(std::move(work));
                },
                &error)) {
            throw std::runtime_error(
                "failed to start the group commit writer: " + error);
        }
    }
    scanner_lease_ = kv_store_->acquire_scanner();
    started_ = true;
}

void ServerPersistenceRuntime::start_interval_timer(
    FailClosedCallback fail_closed) {
    if (!started_ || !persistence_manager_ || !persistence_manager_->opened()) {
        return;
    }
    if (durability_mode_ != DurabilityMode::kInterval) {
        return;
    }
    if (interval_timer_id_ != 0) {
        return;
    }
    interval_fail_closed_ = std::move(fail_closed);
    interval_timer_id_ =
        loop_->run_every(kIntervalFlushMs, [this]() { flush_interval(); });
}

void ServerPersistenceRuntime::stop() {
    cancel_interval_timer();
    cancel_shutdown_retry_timer();
    scanner_lease_.reset();
    started_ = false;
}

void ServerPersistenceRuntime::begin_shutdown(ShutdownCompletion completion) {
    loop_->assert_in_loop_thread();
    if (shutdown_complete_) {
        if (completion) {
            completion(shutdown_result_, shutdown_error_);
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

    cancel_interval_timer();
    scanner_lease_.reset();
    started_ = false;

    if (!persistence_manager_ ||
        !persistence_manager_->group_commit_started()) {
        finish_shutdown(RdbSaveResult::Ok, {});
        return;
    }
    try_start_persistence_shutdown();
}

void ServerPersistenceRuntime::try_start_persistence_shutdown() {
    shutdown_retry_timer_id_ = 0;
    if (!persistence_manager_ || persistence_shutdown_pending_ ||
        shutdown_complete_) {
        return;
    }
    const PersistenceControlResult result =
        persistence_manager_->shutdown_async_result(
            [this](RdbSaveResult shutdown_result,
                   const std::string& completion_error) {
                persistence_shutdown_pending_ = false;
                finish_shutdown(shutdown_result, completion_error);
            });
    if (result.accepted()) {
        persistence_shutdown_pending_ = true;
        return;
    }
    if (result.status == PersistenceControlStatus::kBusy) {
        shutdown_retry_timer_id_ = loop_->run_after(
            kShutdownRetryMs, [this]() { try_start_persistence_shutdown(); });
        return;
    }
    finish_shutdown(RdbSaveResult::Failed, result.error);
}

void ServerPersistenceRuntime::finish_shutdown(RdbSaveResult result,
                                               const std::string& error) {
    if (shutdown_complete_) {
        return;
    }
    shutdown_complete_ = true;
    shutdown_result_ = result;
    shutdown_error_ = error;
    if (result == RdbSaveResult::Ok) {
        std::ostringstream message;
        message << "Saved " << kv_store_->size()
                << " keys to the durable checkpoint";
        Log::Info(message.str());
    } else if (result == RdbSaveResult::DurabilityUnconfirmed) {
        Log::Error("Checkpoint was replaced but directory fsync failed; "
                   "durability is unconfirmed");
    } else {
        Log::Error(error.empty() ? "Failed to save the durable checkpoint"
                                 : "Checkpoint failed: " + error);
    }

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
}

void ServerPersistenceRuntime::flush_interval() {
    if (!persistence_manager_) {
        return;
    }
    const PersistenceControlResult result =
        persistence_manager_->flush_interval_async_result(
            [this](bool success, const std::string& flush_error) {
                if (success) {
                    return;
                }
                const std::string message =
                    flush_error.empty()
                        ? std::string("INTERVAL WAL flush failed")
                        : flush_error;
                Log::Error("INTERVAL WAL flush failed; stopping fail-closed: " +
                           message);
                if (interval_fail_closed_) {
                    interval_fail_closed_(message);
                }
            });
    // A checkpoint or a prior barrier owns the writer; the next timer tick
    // retries.  Anything else is a terminal interval-flush failure.
    if (result.accepted() || result.status == PersistenceControlStatus::kBusy ||
        result.status == PersistenceControlStatus::kShuttingDown) {
        return;
    }
    const std::string message =
        result.error.empty() ? std::string("INTERVAL WAL flush could not be "
                                           "scheduled")
                             : result.error;
    Log::Error(message);
    if (interval_fail_closed_) {
        interval_fail_closed_(message);
    }
}

void ServerPersistenceRuntime::cancel_interval_timer() {
    if (interval_timer_id_ != 0) {
        loop_->cancel_timer(interval_timer_id_);
        interval_timer_id_ = 0;
    }
}

void ServerPersistenceRuntime::cancel_shutdown_retry_timer() {
    if (shutdown_retry_timer_id_ != 0) {
        loop_->cancel_timer(shutdown_retry_timer_id_);
        shutdown_retry_timer_id_ = 0;
    }
}

} // namespace minidb
