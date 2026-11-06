#pragma once
#include "TcpConnection.h"
#include "InetAddress.h"
#include <map>
#include <memory>
#include <string>

namespace minidb {
class EventLoop;
class Acceptor;

class TcpServer {
  public:
    using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
    using MessageCallback =
        std::function<void(const TcpConnectionPtr&, Buffer*)>;

    TcpServer(EventLoop* loop, const InetAddress& listen_addr,
              const std::string& name);
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    void start();
    void set_connection_callback(const ConnectionCallback& cb) {
        connection_callback_ = cb;
    }
    void set_message_callback(const MessageCallback& cb) {
        message_callback_ = cb;
    }
    const std::string& name() const {
        return name_;
    }
    InetAddress listen_address() const;
    size_t connection_count() const {
        return connections_.size();
    }

    static constexpr size_t kMaxConnections = 32;

  private:
    void new_connection(int sockfd, const InetAddress& peer_addr);
    void remove_connection(const TcpConnectionPtr& conn);

    using ConnectionMap = std::map<std::string, TcpConnectionPtr>;

    EventLoop* loop_;
    const std::string name_;
    std::unique_ptr<Acceptor> acceptor_;
    ConnectionCallback connection_callback_;
    MessageCallback message_callback_;
    bool started_;
    int next_conn_id_;
    ConnectionMap connections_;
};
} // namespace minidb
