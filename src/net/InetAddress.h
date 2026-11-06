// InetAddress.h - Socket address wrapper
#pragma once

#include <netinet/in.h>
#include <string>

namespace minidb {

class InetAddress {
  public:
    explicit InetAddress(uint16_t port = 0, bool loopback = false);
    InetAddress(const std::string& ip, uint16_t port);
    explicit InetAddress(const struct sockaddr_in& addr);

    const struct sockaddr* sockaddr() const {
        return reinterpret_cast<const struct sockaddr*>(&addr_);
    }

    socklen_t length() const {
        return sizeof(addr_);
    }

    std::string ip() const;
    uint16_t port() const;
    std::string to_string() const;

  private:
    struct sockaddr_in addr_;
};

} // namespace minidb
