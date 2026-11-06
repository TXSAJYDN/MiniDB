// InetAddress.cpp
#include "InetAddress.h"
#include <arpa/inet.h>
#include <cstring>

namespace minidb {

InetAddress::InetAddress(uint16_t port, bool loopback) {
    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
    addr_.sin_port = htons(port);
}

InetAddress::InetAddress(const std::string& ip, uint16_t port) {
    std::memset(&addr_, 0, sizeof(addr_));
    addr_.sin_family = AF_INET;
    addr_.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr_.sin_addr) <= 0) {
        addr_.sin_addr.s_addr = htonl(INADDR_ANY);
    }
}

InetAddress::InetAddress(const struct sockaddr_in& addr) : addr_(addr) {}

std::string InetAddress::ip() const {
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr_.sin_addr, buf, sizeof(buf));
    return buf;
}

uint16_t InetAddress::port() const {
    return ntohs(addr_.sin_port);
}

std::string InetAddress::to_string() const {
    return ip() + ":" + std::to_string(port());
}

} // namespace minidb
