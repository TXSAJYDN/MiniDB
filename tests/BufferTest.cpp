#include "base/Buffer.h"
#include <gtest/gtest.h>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace minidb {

TEST(BufferTest, InitialState) {
    Buffer buf;
    EXPECT_EQ(buf.readable_bytes(), 0);
    EXPECT_EQ(buf.writable_bytes(), Buffer::kInitialSize);
    EXPECT_EQ(buf.prepend_bytes(), Buffer::kPrependSize);
}

TEST(BufferTest, AppendAndRetrieve) {
    Buffer buf;
    const char* data = "hello";
    buf.append(data, 5);
    EXPECT_EQ(buf.readable_bytes(), 5);
    EXPECT_EQ(std::memcmp(buf.peek(), data, 5), 0);
    buf.retrieve(2);
    EXPECT_EQ(buf.readable_bytes(), 3);
    EXPECT_EQ(std::memcmp(buf.peek(), "llo", 3), 0);
}

TEST(BufferTest, RetrieveAsString) {
    Buffer buf;
    buf.append("world", 5);
    std::string result = buf.retrieve_as_string(3);
    EXPECT_EQ(result, "wor");
    EXPECT_EQ(buf.readable_bytes(), 2);
}

TEST(BufferTest, RetrieveAllAsString) {
    Buffer buf;
    buf.append("test data", 9);
    std::string result = buf.retrieve_all_as_string();
    EXPECT_EQ(result, "test data");
    EXPECT_EQ(buf.readable_bytes(), 0);
}

TEST(BufferTest, AppendGrowsBuffer) {
    Buffer buf(16);
    std::string large_data(100, 'x');
    buf.append(large_data.data(), large_data.size());
    EXPECT_EQ(buf.readable_bytes(), 100);
}

TEST(BufferTest, EnsureWritable) {
    Buffer buf(16);
    buf.ensure_writable(100);
    EXPECT_GE(buf.writable_bytes(), 100);
}

TEST(BufferTest, ReadFdSuccess) {
    Buffer buf;

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    const char* test_data = "hello world";
    ssize_t written = write(pipefd[1], test_data, 11);
    ASSERT_EQ(written, 11);
    close(pipefd[1]);

    int saved_errno = 0;
    ssize_t n = buf.read_fd(pipefd[0], &saved_errno);

    EXPECT_EQ(n, 11);
    EXPECT_EQ(buf.readable_bytes(), 11);
    EXPECT_EQ(std::memcmp(buf.peek(), test_data, 11), 0);

    close(pipefd[0]);
}

TEST(BufferTest, ReadFdEOF) {
    Buffer buf;

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    close(pipefd[1]);

    int saved_errno = 0;
    ssize_t n = buf.read_fd(pipefd[0], &saved_errno);

    EXPECT_EQ(n, 0);
    EXPECT_EQ(buf.readable_bytes(), 0);

    close(pipefd[0]);
}

TEST(BufferTest, ReadFdError) {
    Buffer buf;

    int invalid_fd = -1;
    int saved_errno = 0;
    ssize_t n = buf.read_fd(invalid_fd, &saved_errno);

    EXPECT_EQ(n, -1);
    EXPECT_EQ(saved_errno, EBADF);
}

TEST(BufferTest, ReadFdWouldBlock) {
    Buffer buf;

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    // Set non-blocking
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    int saved_errno = 0;
    ssize_t n = buf.read_fd(pipefd[0], &saved_errno);

    EXPECT_EQ(n, -1);
    EXPECT_TRUE(saved_errno == EAGAIN || saved_errno == EWOULDBLOCK);

    close(pipefd[0]);
    close(pipefd[1]);
}

TEST(BufferTest, ReadFdPartialRead) {
    Buffer buf(16);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    // Write data in two chunks
    ssize_t w1 = write(pipefd[1], "first", 5);
    ASSERT_EQ(w1, 5);

    int saved_errno = 0;
    ssize_t n1 = buf.read_fd(pipefd[0], &saved_errno);
    EXPECT_EQ(n1, 5);
    EXPECT_EQ(buf.readable_bytes(), 5);

    ssize_t w2 = write(pipefd[1], "second", 6);
    ASSERT_EQ(w2, 6);
    ssize_t n2 = buf.read_fd(pipefd[0], &saved_errno);
    EXPECT_EQ(n2, 6);
    EXPECT_EQ(buf.readable_bytes(), 11);

    std::string result = buf.retrieve_all_as_string();
    EXPECT_EQ(result, "firstsecond");

    close(pipefd[0]);
    close(pipefd[1]);
}

TEST(BufferTest, ReadFdEINTR) {
    Buffer buf;

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    // Set up signal handler without SA_RESTART
    struct sigaction sa;
    sa.sa_handler = [](int) {}; // Empty handler
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // No SA_RESTART - syscall will be interrupted
    ASSERT_EQ(sigaction(SIGUSR1, &sa, nullptr), 0);

    // Make read end blocking
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags & ~O_NONBLOCK);

    // Fork to send signal during read
    pid_t pid = fork();
    if (pid == 0) {
        // Child: wait a bit then send signal
        usleep(10000); // 10ms
        kill(getppid(), SIGUSR1);
        exit(0);
    }

    // Parent: try to read (will be interrupted)
    int saved_errno = 0;
    ssize_t n = buf.read_fd(pipefd[0], &saved_errno);

    // Should be interrupted by signal
    bool got_eintr = (n == -1 && saved_errno == EINTR);

    // Clean up
    close(pipefd[0]);
    close(pipefd[1]);
    int status;
    waitpid(pid, &status, 0);

    // Restore default signal handler
    signal(SIGUSR1, SIG_DFL);

    EXPECT_TRUE(got_eintr);
}

TEST(BufferTest, MakeSpaceByMoving) {
    Buffer buf(32);
    buf.append("1234567890", 10);
    buf.retrieve(5);

    size_t old_capacity =
        buf.writable_bytes() + buf.readable_bytes() + buf.prepend_bytes();
    buf.append("abcdefghij", 10);
    size_t new_capacity =
        buf.writable_bytes() + buf.readable_bytes() + buf.prepend_bytes();

    EXPECT_EQ(buf.readable_bytes(), 15);
    EXPECT_EQ(old_capacity, new_capacity);
}

} // namespace minidb
