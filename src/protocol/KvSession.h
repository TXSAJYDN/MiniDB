#pragma once
#include "KvProtocol.h"
#include "../net/TcpConnection.h"
#include "../base/Buffer.h"
#include "../storage/KvStore.h"
#include "../storage/RdbManager.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace minidb {

// Per-connection KV protocol state machine.  Like HttpSession it allows only
// one command in flight, bounds buffered input, and resumes parsing from the
// write-complete callback.  All methods run on the TcpConnection owner loop.
class KvSession : public std::enable_shared_from_this<KvSession> {
  public:
    explicit KvSession(const TcpConnectionPtr& conn,
                       std::shared_ptr<KvStore> store);
    ~KvSession();

    KvSession(const KvSession&) = delete;
    KvSession& operator=(const KvSession&) = delete;

    void start();
    void on_message(Buffer* buf);
    void set_save_callback(std::function<RdbSaveResult()> callback) {
        save_callback_ = std::move(callback);
    }

    bool is_processing() const {
        return processing_;
    }
    bool is_closing() const {
        return closing_;
    }

  private:
    void schedule_drain();
    void drain();
    bool handle_command(const KvProtocol::Command& cmd);
    bool validate_key(const std::string& key, const char* command);
    bool send_response(KvProtocol::CommandType type,
                       const std::vector<std::string>& args);
    void fail_and_close(const std::string& message);
    void on_write_complete();

    TcpConnectionPtr conn_;
    std::shared_ptr<KvStore> store_;
    std::function<RdbSaveResult()> save_callback_;
    Buffer pending_buffer_;
    bool started_;
    bool processing_;
    bool closing_;
    bool closed_;
    bool drain_scheduled_;

    static constexpr size_t kMaxPendingBufferSize =
        TcpConnection::kMaxInputBufferSize;
};

} // namespace minidb
