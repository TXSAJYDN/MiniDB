#ifndef MINIDB_BASE_FILE_DESCRIPTOR_H
#define MINIDB_BASE_FILE_DESCRIPTOR_H

namespace minidb {

class FileDescriptor {
  public:
    explicit FileDescriptor(int fd = -1) noexcept;
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&& other) noexcept;
    FileDescriptor& operator=(FileDescriptor&& other) noexcept;
    ~FileDescriptor();

    int get() const noexcept {
        return fd_;
    }
    bool valid() const noexcept {
        return fd_ >= 0;
    }
    explicit operator bool() const noexcept {
        return valid();
    }
    int release() noexcept;
    void close() noexcept;
    void reset(int fd = -1) noexcept;

  private:
    int fd_;
};

} // namespace minidb

#endif
