#include <gtest/gtest.h>

#include "../src/storage/KvStore.h"
#include "../src/storage/RdbManager.h"
#include "RdbTestUtil.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace minidb {
namespace {

enum class FaultStage {
    None,
    Open,
    WriteFailure,
    FsyncFailure,
    CloseFailure,
    RenameFailure,
    OpenParent,
    FsyncDirectory,
    KillDuringWrite,
    KillDuringFsync,
    KillAfterRename,
};

class FaultRdbFileOperations final : public RdbFileOperations {
  public:
    explicit FaultRdbFileOperations(FaultStage stage = FaultStage::None)
        : stage_(stage) {}

    int open_exclusive(const std::string& path, mode_t mode) override {
        if (stage_ == FaultStage::Open) {
            errno = EACCES;
            return -1;
        }
        const int remaining = eexist_remaining_.load(std::memory_order_relaxed);
        if (remaining > 0) {
            eexist_remaining_.fetch_sub(1, std::memory_order_relaxed);
            errno = EEXIST;
            return -1;
        }

        const int active = active_opens_.fetch_add(1) + 1;
        int observed = max_active_opens_.load();
        while (active > observed &&
               !max_active_opens_.compare_exchange_weak(observed, active)) {
        }
        if (open_delay_ms_ > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(open_delay_ms_));
        }
        const int fd =
            ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
        active_opens_.fetch_sub(1);
        return fd;
    }

    ssize_t write(int fd, const void* data, size_t len) override {
        const int call = write_calls_.fetch_add(1) + 1;
        if (eintr_once_ && call == 1) {
            errno = EINTR;
            return -1;
        }
        if (stage_ == FaultStage::WriteFailure) {
            if (call == 1) {
                const size_t prefix = std::min<size_t>(len, 7);
                return ::write(fd, data, prefix);
            }
            errno = ENOSPC;
            return -1;
        }
        if (stage_ == FaultStage::KillDuringWrite) {
            const size_t prefix = std::min<size_t>(len, 7);
            const ssize_t written = ::write(fd, data, prefix);
            if (written >= 0) {
                BlockUntilKilled();
            }
            return written;
        }
        if (short_writes_) {
            return ::write(fd, data, std::min<size_t>(len, 5));
        }
        return ::write(fd, data, len);
    }

    int fsync_fd(int fd) override {
        if (stage_ == FaultStage::FsyncFailure) {
            errno = EIO;
            return -1;
        }
        if (stage_ == FaultStage::KillDuringFsync) {
            BlockUntilKilled();
        }
        return ::fsync(fd);
    }

    int close_fd(int fd) override {
        if (stage_ == FaultStage::CloseFailure) {
            (void)::close(fd);
            errno = EIO;
            return -1;
        }
        return ::close(fd);
    }

    int rename_file(const std::string& from, const std::string& to) override {
        if (stage_ == FaultStage::RenameFailure) {
            errno = EXDEV;
            return -1;
        }
        const int result = ::rename(from.c_str(), to.c_str());
        if (stage_ == FaultStage::KillAfterRename && result == 0) {
            BlockUntilKilled();
        }
        return result;
    }

    int unlink_file(const std::string& path) override {
        unlink_calls_.fetch_add(1);
        return ::unlink(path.c_str());
    }

    int open_parent_dir(const std::string& path) override {
        if (stage_ == FaultStage::OpenParent) {
            errno = EACCES;
            return -1;
        }
        fs::path parent = fs::path(path).parent_path();
        if (parent.empty()) {
            parent = ".";
        }
        return ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }

    int fsync_dir(int fd) override {
        if (stage_ == FaultStage::FsyncDirectory) {
            errno = EIO;
            return -1;
        }
        return ::fsync(fd);
    }

    void set_eexist_remaining(int count) {
        eexist_remaining_.store(count);
    }
    void set_open_delay_ms(int delay_ms) {
        open_delay_ms_ = delay_ms;
    }
    void set_short_writes(bool enabled) {
        short_writes_ = enabled;
    }
    void set_eintr_once(bool enabled) {
        eintr_once_ = enabled;
    }
    void set_notify_fd(int fd) {
        notify_fd_ = fd;
    }

    int write_calls() const {
        return write_calls_.load();
    }
    int max_active_opens() const {
        return max_active_opens_.load();
    }
    int unlink_calls() const {
        return unlink_calls_.load();
    }

  private:
    [[noreturn]] void BlockUntilKilled() const {
        if (notify_fd_ >= 0) {
            const char ready = 'R';
            ssize_t sent = -1;
            do {
                sent = ::write(notify_fd_, &ready, 1);
            } while (sent < 0 && errno == EINTR);
            if (sent != 1) {
                ::_exit(92);
            }
        }
        for (;;) {
            ::pause();
        }
    }

    FaultStage stage_;
    std::atomic<int> eexist_remaining_{0};
    std::atomic<int> active_opens_{0};
    std::atomic<int> max_active_opens_{0};
    std::atomic<int> write_calls_{0};
    std::atomic<int> unlink_calls_{0};
    int open_delay_ms_ = 0;
    bool short_writes_ = false;
    bool eintr_once_ = false;
    int notify_fd_ = -1;
};

class RdbManagerFaultTest : public ::testing::Test {
  protected:
    void SetUp() override {
        test_dir_ = minidb_test::make_unique_temp_dir("minidb_rdb_fault_");
        rdb_path_ = test_dir_ + "/fault.rdb";
    }

    void TearDown() override {
        for (const auto& entry : fs::directory_iterator(test_dir_)) {
            const std::string name = entry.path().filename().string();
            if (name.find(".tmp.") != std::string::npos) {
                std::error_code ec;
                fs::remove(entry.path(), ec);
            }
        }
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    void SeedOld(KvStore& store) {
        store.set("base", "old");
        store.set("old-a", "1");
        store.set("old-b", "2");
    }

    void SeedNew(KvStore& store) {
        store.set("base", "new");
        for (int i = 0; i < 64; ++i) {
            const std::string suffix =
                i < 10 ? "0" + std::to_string(i) : std::to_string(i);
            store.set("new-" + suffix, "value-" + suffix);
        }
    }

    void SaveOldSnapshot() {
        RdbManager manager(rdb_path_);
        KvStore old_store;
        SeedOld(old_store);
        ASSERT_EQ(manager.save_checked(old_store), RdbSaveResult::Ok);
    }

    void ExpectOldSnapshot() {
        RdbManager loader(rdb_path_);
        KvStore recovered;
        EXPECT_EQ(loader.load(recovered), 3);
        EXPECT_EQ(recovered.get("base"), std::optional<std::string>("old"));
        EXPECT_EQ(recovered.get("old-a"), std::optional<std::string>("1"));
        EXPECT_EQ(recovered.get("old-b"), std::optional<std::string>("2"));
        EXPECT_FALSE(recovered.get("new-00").has_value());
    }

    void ExpectCompleteNewSnapshot() {
        RdbManager loader(rdb_path_);
        KvStore recovered;
        EXPECT_EQ(loader.load(recovered), 65);
        EXPECT_EQ(recovered.get("base"), std::optional<std::string>("new"));
        for (int i = 0; i < 64; ++i) {
            const std::string suffix =
                i < 10 ? "0" + std::to_string(i) : std::to_string(i);
            EXPECT_EQ(recovered.get("new-" + suffix),
                      std::optional<std::string>("value-" + suffix));
        }
    }

    void RunWithKillAt(FaultStage stage) {
        int notify_pipe[2] = {-1, -1};
        ASSERT_EQ(::pipe(notify_pipe), 0);
        auto operations = std::make_shared<FaultRdbFileOperations>(stage);
        operations->set_notify_fd(notify_pipe[1]);

        const pid_t child = ::fork();
        if (child < 0) {
            ::close(notify_pipe[0]);
            ::close(notify_pipe[1]);
            FAIL() << "fork failed: " << std::strerror(errno);
        }
        if (child == 0) {
            ::close(notify_pipe[0]);
            RdbManager manager(rdb_path_, operations);
            KvStore new_store;
            SeedNew(new_store);
            (void)manager.save_checked(new_store);
            ::_exit(90);
        }

        ::close(notify_pipe[1]);
        bool notified = false;
        int poll_result = 0;
        do {
            pollfd descriptor{notify_pipe[0], POLLIN, 0};
            poll_result = ::poll(&descriptor, 1, 10000);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result > 0) {
            char ready = 0;
            ssize_t received = -1;
            do {
                received = ::read(notify_pipe[0], &ready, 1);
            } while (received < 0 && errno == EINTR);
            notified = received == 1 && ready == 'R';
        }
        ::close(notify_pipe[0]);

        EXPECT_EQ(::kill(child, SIGKILL), 0);
        int status = 0;
        EXPECT_EQ(::waitpid(child, &status, 0), child);
        EXPECT_TRUE(notified)
            << "child did not reach fault stage, poll=" << poll_result
            << " errno=" << errno;
        EXPECT_TRUE(WIFSIGNALED(status))
            << "child status=" << status << " notified=" << notified;
        if (WIFSIGNALED(status)) {
            EXPECT_EQ(WTERMSIG(status), SIGKILL);
        }
    }

    int temporary_file_count() const {
        int count = 0;
        for (const auto& entry : fs::directory_iterator(test_dir_)) {
            if (entry.path().filename().string().find(".tmp.") !=
                std::string::npos) {
                ++count;
            }
        }
        return count;
    }

    std::string test_dir_;
    std::string rdb_path_;
};

void ExpectPreRenameFailure(FaultStage stage) {
    const std::string dir =
        minidb_test::make_unique_temp_dir("minidb_rdb_pre_rename_");
    const std::string path = dir + "/snapshot.rdb";

    RdbManager baseline(path);
    KvStore old_store;
    old_store.set("old", "value");
    ASSERT_EQ(baseline.save_checked(old_store), RdbSaveResult::Ok);

    auto operations = std::make_shared<FaultRdbFileOperations>(stage);
    RdbManager failing(path, operations);
    KvStore new_store;
    new_store.set("new", "value");
    EXPECT_EQ(failing.save_checked(new_store), RdbSaveResult::Failed);
    EXPECT_EQ(failing.last_save_result(), RdbSaveResult::Failed);

    RdbManager loader(path);
    KvStore loaded;
    EXPECT_EQ(loader.load(loaded), 1);
    EXPECT_EQ(loaded.get("old"), std::optional<std::string>("value"));
    EXPECT_FALSE(loaded.get("new").has_value());
    if (stage == FaultStage::Open) {
        EXPECT_EQ(operations->unlink_calls(), 0);
    } else {
        EXPECT_GT(operations->unlink_calls(), 0);
    }

    for (const auto& entry : fs::directory_iterator(dir)) {
        EXPECT_EQ(entry.path().filename().string().find(".tmp."),
                  std::string::npos);
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_F(RdbManagerFaultTest, OpenFailurePreservesOldSnapshot) {
    ExpectPreRenameFailure(FaultStage::Open);
}

TEST_F(RdbManagerFaultTest, PartialWriteThenEnospcPreservesOldSnapshot) {
    ExpectPreRenameFailure(FaultStage::WriteFailure);
}

TEST_F(RdbManagerFaultTest, FileFsyncFailurePreservesOldSnapshot) {
    ExpectPreRenameFailure(FaultStage::FsyncFailure);
}

TEST_F(RdbManagerFaultTest, CloseFailurePreservesOldSnapshot) {
    ExpectPreRenameFailure(FaultStage::CloseFailure);
}

TEST_F(RdbManagerFaultTest, RenameFailurePreservesOldSnapshot) {
    ExpectPreRenameFailure(FaultStage::RenameFailure);
}

TEST_F(RdbManagerFaultTest, EintrAndShortWritesAreRetriedUntilComplete) {
    SaveOldSnapshot();
    auto operations = std::make_shared<FaultRdbFileOperations>();
    operations->set_eintr_once(true);
    operations->set_short_writes(true);
    RdbManager manager(rdb_path_, operations);
    KvStore store;
    store.set("key", "value-with-short-writes");
    EXPECT_EQ(manager.save_checked(store), RdbSaveResult::Ok);
    EXPECT_GT(operations->write_calls(), 2);

    RdbManager loader(rdb_path_);
    KvStore loaded;
    EXPECT_EQ(loader.load(loaded), 1);
    EXPECT_EQ(loaded.get("key"),
              std::optional<std::string>("value-with-short-writes"));
}

TEST_F(RdbManagerFaultTest, EexistIsRetriedWithANewTemporaryPath) {
    auto operations = std::make_shared<FaultRdbFileOperations>();
    operations->set_eexist_remaining(3);
    RdbManager manager(rdb_path_, operations);
    KvStore store;
    store.set("key", "value");
    EXPECT_EQ(manager.save_checked(store), RdbSaveResult::Ok);
    EXPECT_EQ(temporary_file_count(), 0);
}

TEST_F(RdbManagerFaultTest, EexistExhaustionFailsAfterBoundedRetries) {
    auto operations = std::make_shared<FaultRdbFileOperations>();
    operations->set_eexist_remaining(200);
    RdbManager manager(rdb_path_, operations);
    KvStore store;
    EXPECT_EQ(manager.save_checked(store), RdbSaveResult::Failed);
    EXPECT_EQ(operations->write_calls(), 0);
}

TEST_F(RdbManagerFaultTest,
       ParentOpenFailureAfterRenameIsDurabilityUnconfirmed) {
    SaveOldSnapshot();
    auto operations =
        std::make_shared<FaultRdbFileOperations>(FaultStage::OpenParent);
    RdbManager manager(rdb_path_, operations);
    KvStore store;
    store.set("new", "visible");
    EXPECT_EQ(manager.save_checked(store),
              RdbSaveResult::DurabilityUnconfirmed);
    EXPECT_EQ(manager.last_save_result(), RdbSaveResult::DurabilityUnconfirmed);

    RdbManager loader(rdb_path_);
    KvStore loaded;
    EXPECT_EQ(loader.load(loaded), 1);
    EXPECT_EQ(loaded.get("new"), std::optional<std::string>("visible"));
    EXPECT_EQ(temporary_file_count(), 0);
}

TEST_F(RdbManagerFaultTest,
       DirectoryFsyncFailureAfterRenameIsDurabilityUnconfirmed) {
    SaveOldSnapshot();
    auto operations =
        std::make_shared<FaultRdbFileOperations>(FaultStage::FsyncDirectory);
    RdbManager manager(rdb_path_, operations);
    KvStore store;
    store.set("new", "visible");
    EXPECT_EQ(manager.save_checked(store),
              RdbSaveResult::DurabilityUnconfirmed);

    RdbManager loader(rdb_path_);
    KvStore loaded;
    EXPECT_EQ(loader.load(loaded), 1);
    EXPECT_EQ(loaded.get("new"), std::optional<std::string>("visible"));
    EXPECT_EQ(temporary_file_count(), 0);
}

TEST_F(RdbManagerFaultTest, ConcurrentSavesSerializeAtMaximumConcurrencyOne) {
    auto operations = std::make_shared<FaultRdbFileOperations>();
    operations->set_open_delay_ms(30);
    RdbManager manager(rdb_path_, operations);
    KvStore store;
    store.set("key", "value");

    std::atomic<bool> start{false};
    std::atomic<int> ok_count{0};
    auto worker = [&]() {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (manager.save_checked(store) == RdbSaveResult::Ok) {
            ok_count.fetch_add(1);
        }
    };
    std::thread first(worker);
    std::thread second(worker);
    start.store(true, std::memory_order_release);
    first.join();
    second.join();

    EXPECT_EQ(ok_count.load(), 2);
    EXPECT_EQ(operations->max_active_opens(), 1);
}

TEST_F(RdbManagerFaultTest,
       KillNineAtWriteAndFsyncPhasesRecoversCompleteOldSnapshot) {
    for (FaultStage crash_stage :
         {FaultStage::KillDuringWrite, FaultStage::KillDuringFsync}) {
        SCOPED_TRACE(crash_stage == FaultStage::KillDuringWrite ? "write"
                                                                : "fsync");
        SaveOldSnapshot();
        RunWithKillAt(crash_stage);
        ExpectOldSnapshot();
    }
}

TEST_F(RdbManagerFaultTest, KillNineAfterRenameRecoversCompleteNewSnapshot) {
    SaveOldSnapshot();
    RunWithKillAt(FaultStage::KillAfterRename);
    ExpectCompleteNewSnapshot();
}

} // namespace
} // namespace minidb
