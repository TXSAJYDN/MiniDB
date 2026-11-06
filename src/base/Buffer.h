#pragma once
#include <algorithm>
#include <cstddef>
#include <string>
#include <sys/types.h>
#include <vector>

namespace minidb {

class Buffer {
  public:
    static constexpr size_t kPrependSize = 8;
    static constexpr size_t kInitialSize = 1024;
    static constexpr size_t kDefaultReadSize = size_t{64} * 1024;

    explicit Buffer(size_t initial_size = kInitialSize);
    ~Buffer() = default;

    size_t readable_bytes() const {
        return write_index_ - read_index_;
    }
    size_t writable_bytes() const {
        return buffer_.size() - write_index_;
    }
    size_t prepend_bytes() const {
        return read_index_;
    }

    const char* peek() const {
        return begin() + read_index_;
    }
    void retrieve(size_t len);
    void retrieve_all() {
        read_index_ = kPrependSize;
        write_index_ = kPrependSize;
    }
    std::string retrieve_as_string(size_t len);
    std::string retrieve_all_as_string();

    const char* find_crlf() const {
        const char* crlf = std::search(peek(), begin_write(), kCRLF, kCRLF + 2);
        return crlf == begin_write() ? nullptr : crlf;
    }

    void append(const char* data, size_t len);
    void append(const std::string& str) {
        append(str.data(), str.size());
    }

    void ensure_writable(size_t len);
    char* begin_write() {
        return begin() + write_index_;
    }
    const char* begin_write() const {
        return begin() + write_index_;
    }
    void has_written(size_t len) {
        write_index_ += len;
    }

    // Reads at most max_bytes from fd.  Callers such as TcpConnection use the
    // bound before the syscall so an attacker cannot force an unbounded read
    // followed by an after-the-fact size check.
    ssize_t read_fd(int fd, int* saved_errno,
                    size_t max_bytes = kDefaultReadSize);

  private:
    char* begin() {
        return &*buffer_.begin();
    }
    const char* begin() const {
        return &*buffer_.begin();
    }
    void make_space(size_t len);

    std::vector<char> buffer_;
    size_t read_index_;
    size_t write_index_;

    static const char kCRLF[];
};

} // namespace minidb