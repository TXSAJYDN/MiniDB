#include <gtest/gtest.h>

#include "../src/net/Acceptor.h"
#include "../src/net/EventLoop.h"
#include "../src/net/InetAddress.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <utility>
#include <vector>

namespace minidb {

struct AcceptorTestPeer {
    static void handle_read(Acceptor& acceptor) {
        acceptor.handle_read();
    }
    static bool paused(const Acceptor& acceptor) {
        return acceptor.accept_paused_;
    }
    static void resume(Acceptor& acceptor) {
        acceptor.resume_accepting();
    }
    static int idle_fd(const Acceptor& acceptor) {
        return acceptor.idle_fd_;
    }
};

namespace {

class FakeAcceptorIo final : public AcceptorIo {
  public:
    struct AcceptStep {
        int fd;
        int error;
    };

    int accept_fd(int /*listen_fd*/, InetAddress* /*peer_addr*/) override {
        ++accept_calls;
        if (accept_steps.empty()) {
            errno = EAGAIN;
            return -1;
        }
        const AcceptStep step = accept_steps.front();
        accept_steps.pop_front();
        if (step.fd < 0) {
            errno = step.error;
        }
        return step.fd;
    }

    int open_idle_fd() override {
        ++idle_open_calls;
        if (idle_fds.empty()) {
            return -1;
        }
        const int fd = idle_fds.front();
        idle_fds.pop_front();
        return fd;
    }

    int close_fd(int fd) override {
        closed_fds.push_back(fd);
        return 0;
    }

    std::deque<AcceptStep> accept_steps;
    std::deque<int> idle_fds;
    std::vector<int> closed_fds;
    int accept_calls = 0;
    int idle_open_calls = 0;
};

class AcceptorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        fake_ = std::make_shared<FakeAcceptorIo>();
    }

    std::shared_ptr<FakeAcceptorIo> fake_;
    EventLoop loop_;
};

TEST_F(AcceptorTest, EintrIsRetriedAndEagainDoesNotPause) {
    fake_->idle_fds.push_back(70);
    fake_->accept_steps.push_back({-1, EINTR});
    fake_->accept_steps.push_back({41, 0});
    fake_->accept_steps.push_back({-1, EAGAIN});

    Acceptor acceptor(&loop_, InetAddress(0, true), fake_, 1000);
    std::vector<int> accepted;
    acceptor.set_new_connection_callback(
        [&](int fd, const InetAddress&) { accepted.push_back(fd); });
    acceptor.listen();
    AcceptorTestPeer::handle_read(acceptor);

    EXPECT_EQ(accepted, std::vector<int>({41}));
    EXPECT_EQ(fake_->accept_calls, 3);
    EXPECT_FALSE(AcceptorTestPeer::paused(acceptor));
}

void ExpectResourceExhaustionPausesAndRecoversIdleFd(int exhaustion_error) {
    EventLoop loop;
    auto fake = std::make_shared<FakeAcceptorIo>();
    fake->idle_fds = {70, 72};
    fake->accept_steps.push_back({-1, exhaustion_error});
    fake->accept_steps.push_back({71, 0});
    fake->accept_steps.push_back({-1, EAGAIN});

    Acceptor acceptor(&loop, InetAddress(0, true), fake, 60000);
    int accepted = 0;
    acceptor.set_new_connection_callback(
        [&](int, const InetAddress&) { ++accepted; });
    acceptor.listen();
    AcceptorTestPeer::handle_read(acceptor);

    EXPECT_EQ(accepted, 0);
    EXPECT_EQ(fake->accept_calls, 2);
    EXPECT_EQ(fake->closed_fds, std::vector<int>({70, 71}));
    EXPECT_EQ(fake->idle_open_calls, 2);
    EXPECT_EQ(AcceptorTestPeer::idle_fd(acceptor), 72);
    EXPECT_TRUE(AcceptorTestPeer::paused(acceptor));

    const int calls_while_paused = fake->accept_calls;
    AcceptorTestPeer::handle_read(acceptor);
    EXPECT_EQ(fake->accept_calls, calls_while_paused);
}

TEST_F(AcceptorTest, EmfileClosesIdleAndDummyFdThenPauses) {
    ExpectResourceExhaustionPausesAndRecoversIdleFd(EMFILE);
}

TEST_F(AcceptorTest, EnfileClosesIdleAndDummyFdThenPauses) {
    ExpectResourceExhaustionPausesAndRecoversIdleFd(ENFILE);
}

TEST_F(AcceptorTest, ResourceExhaustionWithoutReservedIdleFdStillPauses) {
    fake_->accept_steps.push_back({-1, EMFILE});
    Acceptor acceptor(&loop_, InetAddress(0, true), fake_, 60000);
    int accepted = 0;
    acceptor.set_new_connection_callback(
        [&](int, const InetAddress&) { ++accepted; });
    acceptor.listen();
    AcceptorTestPeer::handle_read(acceptor);

    EXPECT_EQ(accepted, 0);
    EXPECT_EQ(fake_->accept_calls, 1);
    EXPECT_TRUE(fake_->closed_fds.empty());
    EXPECT_EQ(AcceptorTestPeer::idle_fd(acceptor), -1);
    EXPECT_TRUE(AcceptorTestPeer::paused(acceptor));
}

TEST_F(AcceptorTest, RetryTimerResumesAccepting) {
    fake_->idle_fds = {70, 72};
    fake_->accept_steps.push_back({-1, EMFILE});
    fake_->accept_steps.push_back({71, 0});

    Acceptor acceptor(&loop_, InetAddress(0, true), fake_, 20);
    int accepted = 0;
    acceptor.set_new_connection_callback(
        [&](int, const InetAddress&) { ++accepted; });
    acceptor.listen();
    AcceptorTestPeer::handle_read(acceptor);
    ASSERT_TRUE(AcceptorTestPeer::paused(acceptor));

    loop_.run_after(80, [&]() { loop_.quit(); });
    loop_.loop();
    ASSERT_FALSE(AcceptorTestPeer::paused(acceptor));

    fake_->accept_steps.push_back({41, 0});
    fake_->accept_steps.push_back({-1, EAGAIN});
    AcceptorTestPeer::handle_read(acceptor);
    EXPECT_EQ(accepted, 1);
}

TEST_F(AcceptorTest, PausesAfterExactly1024AcceptsThenResumes) {
    fake_->idle_fds.push_back(70);
    for (int i = 0; i < 1024; ++i) {
        fake_->accept_steps.push_back({1000 + i, 0});
    }

    Acceptor acceptor(&loop_, InetAddress(0, true), fake_, 60000);
    int accepted = 0;
    acceptor.set_new_connection_callback(
        [&](int, const InetAddress&) { ++accepted; });
    acceptor.listen();
    AcceptorTestPeer::handle_read(acceptor);

    EXPECT_EQ(accepted, 1024);
    EXPECT_EQ(fake_->accept_calls, 1024);
    EXPECT_TRUE(AcceptorTestPeer::paused(acceptor));

    AcceptorTestPeer::resume(acceptor);
    ASSERT_FALSE(AcceptorTestPeer::paused(acceptor));
    fake_->accept_steps.push_back({2000, 0});
    fake_->accept_steps.push_back({2001, 0});
    fake_->accept_steps.push_back({-1, EAGAIN});
    AcceptorTestPeer::handle_read(acceptor);
    EXPECT_EQ(accepted, 1026);
}

TEST_F(AcceptorTest, AcceptedFdIsClosedWhenNoCallbackIsInstalled) {
    fake_->idle_fds.push_back(70);
    fake_->accept_steps.push_back({41, 0});
    fake_->accept_steps.push_back({-1, EAGAIN});

    Acceptor acceptor(&loop_, InetAddress(0, true), fake_, 60000);
    acceptor.listen();
    AcceptorTestPeer::handle_read(acceptor);

    EXPECT_EQ(
        std::count(fake_->closed_fds.begin(), fake_->closed_fds.end(), 41), 1);
}

TEST_F(AcceptorTest, StopClosesListenerAndRestartIsRejected) {
    fake_->idle_fds.push_back(70);
    Acceptor acceptor(&loop_, InetAddress(0, true), fake_, 60000);
    acceptor.listen();
    ASSERT_TRUE(acceptor.listening());

    acceptor.stop();
    EXPECT_FALSE(acceptor.listening());
    acceptor.stop();
    EXPECT_FALSE(acceptor.listening());
    EXPECT_THROW(acceptor.listen(), std::logic_error);
}

TEST_F(AcceptorTest, UnexpectedAcceptErrorStopsOnlyCurrentDrain) {
    fake_->idle_fds.push_back(70);
    fake_->accept_steps.push_back({-1, ECONNABORTED});

    Acceptor acceptor(&loop_, InetAddress(0, true), fake_, 60000);
    int accepted = 0;
    acceptor.set_new_connection_callback(
        [&](int, const InetAddress&) { ++accepted; });
    acceptor.listen();
    AcceptorTestPeer::handle_read(acceptor);

    EXPECT_EQ(accepted, 0);
    EXPECT_FALSE(AcceptorTestPeer::paused(acceptor));

    fake_->accept_steps.push_back({41, 0});
    fake_->accept_steps.push_back({-1, EAGAIN});
    AcceptorTestPeer::handle_read(acceptor);
    EXPECT_EQ(accepted, 1);
}

} // namespace
} // namespace minidb
