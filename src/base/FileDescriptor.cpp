#include "FileDescriptor.h"
#include <unistd.h>

namespace minidb {

FileDescriptor::FileDescriptor(int fd) noexcept : fd_(fd) {}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept
    : fd_(other.fd_) {
    other.fd_ = -1;
}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

FileDescriptor::~FileDescriptor() {
    close();
}

int FileDescriptor::release() noexcept {
    int fd = fd_;
    fd_ = -1;
    return fd;
}

void FileDescriptor::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void FileDescriptor::reset(int fd) noexcept {
    if (fd_ == fd)
        return;
    close();
    fd_ = fd;
}

} // namespace minidb
