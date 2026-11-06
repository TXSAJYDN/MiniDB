#include "base/FileDescriptor.h"
#include <gtest/gtest.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <set>

namespace minidb {

std::set<int> get_open_fds() {
    std::set<int> fds;
    DIR* dir = opendir("/proc/self/fd");
    if (!dir)
        return fds;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] >= '0' && entry->d_name[0] <= '9') {
            fds.insert(std::atoi(entry->d_name));
        }
    }
    closedir(dir);
    return fds;
}

TEST(FileDescriptorTest, DefaultConstructor) {
    FileDescriptor fd;
    EXPECT_FALSE(fd.valid());
    EXPECT_EQ(fd.get(), -1);
}

TEST(FileDescriptorTest, ConstructWithValidFd) {
    int raw_fd = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw_fd, 0);
    FileDescriptor fd(raw_fd);
    EXPECT_TRUE(fd.valid());
    EXPECT_EQ(fd.get(), raw_fd);
}

TEST(FileDescriptorTest, MoveConstructor) {
    int raw_fd = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw_fd, 0);
    FileDescriptor fd1(raw_fd);
    int fd1_value = fd1.get();
    FileDescriptor fd2(std::move(fd1));
    EXPECT_FALSE(fd1.valid());
    EXPECT_TRUE(fd2.valid());
    EXPECT_EQ(fd2.get(), fd1_value);
}

TEST(FileDescriptorTest, MoveAssignment) {
    int raw_fd1 = ::open("/dev/null", O_RDONLY);
    int raw_fd2 = ::open("/dev/zero", O_RDONLY);
    ASSERT_GE(raw_fd1, 0);
    ASSERT_GE(raw_fd2, 0);

    FileDescriptor fd1(raw_fd1);
    FileDescriptor fd2(raw_fd2);

    int fd1_value = fd1.get();
    fd2 = std::move(fd1);

    EXPECT_FALSE(fd1.valid());
    EXPECT_TRUE(fd2.valid());
    EXPECT_EQ(fd2.get(), fd1_value);
}

TEST(FileDescriptorTest, Release) {
    int raw_fd = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw_fd, 0);
    FileDescriptor fd(raw_fd);
    int released = fd.release();
    EXPECT_EQ(released, raw_fd);
    EXPECT_FALSE(fd.valid());
    ::close(released);
}

TEST(FileDescriptorTest, Close) {
    int raw_fd = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw_fd, 0);
    FileDescriptor fd(raw_fd);
    fd.close();
    EXPECT_FALSE(fd.valid());
}

TEST(FileDescriptorTest, Reset) {
    int raw_fd1 = ::open("/dev/null", O_RDONLY);
    int raw_fd2 = ::open("/dev/zero", O_RDONLY);
    ASSERT_GE(raw_fd1, 0);
    ASSERT_GE(raw_fd2, 0);

    FileDescriptor fd(raw_fd1);
    fd.reset(raw_fd2);

    EXPECT_TRUE(fd.valid());
    EXPECT_EQ(fd.get(), raw_fd2);
}

TEST(FileDescriptorTest, ResetSameFd) {
    int raw_fd = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw_fd, 0);

    FileDescriptor fd(raw_fd);
    int original_fd = fd.get();

    fd.reset(original_fd);

    EXPECT_TRUE(fd.valid());
    EXPECT_EQ(fd.get(), original_fd);

    char buf[1];
    ssize_t result = ::read(original_fd, buf, 1);
    EXPECT_GE(result, 0);
}

TEST(FileDescriptorTest, DestructorClosesFd) {
    int raw_fd = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(raw_fd, 0);

    { FileDescriptor fd(raw_fd); }

    char buf[1];
    ssize_t result = ::read(raw_fd, buf, 1);
    EXPECT_EQ(result, -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(FileDescriptorTest, FdReuseAfterClose) {
    auto fds_before = get_open_fds();

    int fd1 = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(fd1, 0);

    { FileDescriptor fd_obj(fd1); }

    int fd2 = ::open("/dev/null", O_RDONLY);
    ASSERT_GE(fd2, 0);

    EXPECT_EQ(fd1, fd2);

    ::close(fd2);

    auto fds_after = get_open_fds();
    EXPECT_EQ(fds_before.size(), fds_after.size());
}

TEST(FileDescriptorTest, NoFdLeakInProcSelfFd) {
    auto fds_before = get_open_fds();

    {
        FileDescriptor fd1(::open("/dev/null", O_RDONLY));
        FileDescriptor fd2(::open("/dev/zero", O_RDONLY));
        FileDescriptor fd3(::open("/dev/random", O_RDONLY));
    }

    auto fds_after = get_open_fds();

    EXPECT_EQ(fds_before.size(), fds_after.size());
}

} // namespace minidb
