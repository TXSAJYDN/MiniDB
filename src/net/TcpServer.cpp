#include "TcpServer.h"
#include "Acceptor.h"
#include "EventLoop.h"
#include "TcpConnection.h"
#include <utility>
#include <unistd.h>

namespace minidb {

TcpServer::TcpServer(EventLoop* loop, const InetAddress& listen_addr,
                     const std::string& name)
    : loop_(loop), name_(name),
      acceptor_(std::make_unique<Acceptor>(loop, listen_addr)), started_(false),
      next_conn_id_(1) {
    acceptor_->set_new_connection_callback(
        [this](int sockfd, const InetAddress& peer_addr) {
            new_connection(sockfd, peer_addr);
        });
}

TcpServer::~TcpServer() {
    loop_->assert_in_loop_thread();
    // Deliberately detach all connections before invoking user callbacks.  A
    // close callback may synchronously try to erase from the server; iterating
    // the live map here would invalidate the iterator.
    ConnectionMap connections;
    connections.swap(connections_);
    for (auto& item : connections) {
        TcpConnectionPtr conn = std::move(item.second);
        if (conn) {
            conn->loop()->assert_in_loop_thread();
            conn->connect_destroyed();
        }
    }
}

InetAddress TcpServer::listen_address() const {
    return acceptor_->listen_address();
}

void TcpServer::start() {
    if (!started_) {
        started_ = true;
    }
    if (!acceptor_->listening()) {
        loop_->assert_in_loop_thread();
        acceptor_->listen();
    }
}

void TcpServer::new_connection(int sockfd, const InetAddress& peer_addr) {
    loop_->assert_in_loop_thread();
    if (connections_.size() >= kMaxConnections) {
        ::close(sockfd);
        return;
    }
    char buf[64];
    snprintf(buf, sizeof buf, "-%s#%d", name_.c_str(), next_conn_id_);
    ++next_conn_id_;
    std::string conn_name = name_ + buf;

    InetAddress local_addr(0);
    TcpConnectionPtr conn = std::make_shared<TcpConnection>(
        loop_, conn_name, sockfd, local_addr, peer_addr);

    connections_[conn_name] = conn;
    conn->set_connection_callback(connection_callback_);
    conn->set_message_callback(message_callback_);
    conn->set_close_callback(
        [this](const TcpConnectionPtr& closed) { remove_connection(closed); });

    loop_->assert_in_loop_thread();
    conn->connect_established();
}

void TcpServer::remove_connection(const TcpConnectionPtr& conn) {
    loop_->assert_in_loop_thread();
    if (!conn) {
        return;
    }

    auto it = connections_.find(conn->name());
    if (it == connections_.end() || it->second != conn) {
        // Removal is intentionally idempotent: connection callbacks and
        // shutdown paths may race within the same loop.
        return;
    }
    connections_.erase(it);
    loop_->queue_in_loop([conn]() { conn->connect_destroyed(); });
}

} // namespace minidb
