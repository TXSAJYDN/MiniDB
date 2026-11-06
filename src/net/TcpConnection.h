#pragma once
#include "../base/Buffer.h"
#include "InetAddress.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace minidb {
class EventLoop;
class Socket;
class Channel;
class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
  public:
    using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
    using MessageCallback =
        std::function<void(const TcpConnectionPtr&, Buffer*)>;
    using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
    using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;

    TcpConnection(EventLoop* loop, const std::string& name, int sockfd,
                  const InetAddress& local_addr, const InetAddress& peer_addr);
    ~TcpConnection();
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    EventLoop* loop() const {
        return loop_;
    }
    const std::string& name() const {
        return name_;
    }
    const InetAddress& local_address() const {
        return local_addr_;
    }
    const InetAddress& peer_address() const {
        return peer_addr_;
    }
    bool connected() const {
        return state_ == kConnected;
    }

    // Returns false when the data cannot be accepted (fatal socket error,
    // output budget exceeded, connection closing).  Calls from another thread
    // return true once the copy has been queued; the owner-loop result cannot
    // be observed synchronously.
    bool send(const void* data, size_t len);
    bool send(const std::string& message) {
        return send(message.data(), message.size());
    }

    // Graceful write-side shutdown.  Once all user-space output has drained,
    // SHUT_WR is issued exactly once and the local descriptor is retired.  The
    // connection does not wait indefinitely for the peer's EOF.
    void shutdown();

    void stop_reading();
    void start_reading();

    void set_connection_callback(const ConnectionCallback& cb) {
        connection_callback_ = cb;
    }
    void set_message_callback(const MessageCallback& cb) {
        message_callback_ = cb;
    }
    void set_close_callback(const CloseCallback& cb) {
        close_callback_ = cb;
    }
    void set_write_complete_callback(const WriteCompleteCallback& cb) {
        write_complete_callback_ = cb;
    }

    void connect_established();
    void connect_destroyed();

    // The largest legal KV frame is 1 MiB + 64 KiB and includes a 4-byte
    // frame-length prefix.  A 1.25 MiB connection budget leaves bounded
    // headroom for HTTP headers and pipelined tails without allowing an
    // attacker to grow per-connection memory without limit.
    static constexpr size_t kMaxInputBufferSize = (5u * 1024u * 1024u) / 4u;
    static constexpr size_t kMaxOutputBufferSize = (5u * 1024u * 1024u) / 4u;
    static constexpr size_t kMaxReadChunkSize = size_t{64} * 1024;

  private:
    enum StateE : std::uint8_t {
        kDisconnected,
        kConnecting,
        kConnected,
        kDisconnecting,
    };

    bool send_in_loop(const char* data, size_t len);
    void handle_read();
    void handle_write();
    void handle_peer_close();
    void complete_peer_close();
    void schedule_peer_close();
    void handle_close();
    void handle_error();
    void note_peer_eof();
    void finish_after_write();
    void force_close(const char* reason);
    void set_state(StateE s) {
        state_ = s;
    }

    EventLoop* loop_;
    const std::string name_;
    StateE state_;
    std::unique_ptr<Socket> socket_;
    std::unique_ptr<Channel> channel_;
    InetAddress local_addr_;
    InetAddress peer_addr_;
    ConnectionCallback connection_callback_;
    MessageCallback message_callback_;
    CloseCallback close_callback_;
    WriteCompleteCallback write_complete_callback_;
    Buffer input_buffer_;
    Buffer output_buffer_;
    bool reading_;
    bool channel_removed_;
    bool peer_eof_;
    bool peer_close_scheduled_;
    bool close_after_write_;
    bool shutdown_write_done_;
};
} // namespace minidb