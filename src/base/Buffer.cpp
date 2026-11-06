#include "Buffer.h"
#include <sys/uio.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
namespace minidb {
const char Buffer::kCRLF[] = "\r\n";
Buffer::Buffer(size_t initial_size)
    : buffer_(kPrependSize + initial_size), read_index_(kPrependSize),
      write_index_(kPrependSize) {}
void Buffer::retrieve(size_t len) {
    if (len < readable_bytes())
        read_index_ += len;
    else
        retrieve_all();
}
std::string Buffer::retrieve_as_string(size_t len) {
    std::string result(peek(), len);
    retrieve(len);
    return result;
}
std::string Buffer::retrieve_all_as_string() {
    return retrieve_as_string(readable_bytes());
}
void Buffer::append(const char* data, size_t len) {
    ensure_writable(len);
    std::copy(data, data + len, begin_write());
    has_written(len);
}
void Buffer::ensure_writable(size_t len) {
    if (writable_bytes() < len)
        make_space(len);
}
void Buffer::make_space(size_t len) {
    if (writable_bytes() + prepend_bytes() < len + kPrependSize) {
        buffer_.resize(write_index_ + len);
    } else {
        size_t readable = readable_bytes();
        std::copy(begin() + read_index_, begin() + write_index_,
                  begin() + kPrependSize);
        read_index_ = kPrependSize;
        write_index_ = read_index_ + readable;
    }
}
ssize_t Buffer::read_fd(int fd, int* saved_errno, size_t max_bytes) {
    if (saved_errno == nullptr) {
        errno = EINVAL;
        return -1;
    }
    if (max_bytes == 0) {
        errno = EINVAL;
        *saved_errno = EINVAL;
        return -1;
    }
    char extra_buf[kDefaultReadSize];
    struct iovec vec[2];
    const size_t writable = writable_bytes();
    const size_t first_len = std::min(writable, max_bytes);
    const size_t second_len =
        std::min(sizeof(extra_buf), max_bytes - first_len);
    vec[0].iov_base = first_len == 0 ? nullptr : begin() + write_index_;
    vec[0].iov_len = first_len;
    vec[1].iov_base = second_len == 0 ? nullptr : extra_buf;
    vec[1].iov_len = second_len;
    const ssize_t n = ::readv(fd, vec, 2);
    if (n < 0) {
        *saved_errno = errno;
    } else if (static_cast<size_t>(n) <= first_len) {
        write_index_ += static_cast<size_t>(n);
    } else {
        write_index_ += first_len;
        append(extra_buf, static_cast<size_t>(n) - first_len);
    }
    return n;
}
} // namespace minidb
