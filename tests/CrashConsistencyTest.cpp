#include <gtest/gtest.h>

#include "../src/storage/PersistenceManager.h"
#include "FaultWalFileOperations.h"
#include "RdbTestUtil.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <poll.h>
#include <random>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace minidb {
namespace {

using FaultOps = minidb_test::FaultWalFileOperations;

struct DatabasePaths {
    std::string directory;
    std::string rdb;
    std::string manifest;
    std::string wal_dir;

    explicit DatabasePaths(const std::string& prefix) {
        directory = minidb_test::make_unique_temp_dir(prefix);
        rdb = directory + "/db.rdb";
        manifest = checkpoint_manifest_path(rdb);
        wal_dir = wal_directory_path(rdb);
    }

    ~DatabasePaths() {
        std::error_code ec;
        fs::remove_all(directory, ec);
    }

    DatabasePaths(const DatabasePaths&) = delete;
    DatabasePaths& operator=(const DatabasePaths&) = delete;
};

bool seed_values(const DatabasePaths& paths,
                 const std::vector<std::pair<std::string, std::string>>& values,
                 bool checkpoint_first, std::string* error) {
    auto store = std::make_shared<KvStore>();
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    if (!manager.open(error)) {
        return false;
    }
    for (const auto& [key, value] : values) {
        const MutationOutcome outcome = manager.set(key, value);
        if (!outcome.ok()) {
            *error = outcome.error;
            return false;
        }
    }
    if (checkpoint_first && manager.checkpoint(error) != RdbSaveResult::Ok) {
        return false;
    }
    return true;
}

bool write_all_fd(int fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        const ssize_t written = ::write(fd, bytes, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        bytes += written;
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
}

bool read_exact_timeout(int fd, void* data, std::size_t size, int timeout_ms) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        const int ready = ::poll(&descriptor, 1, timeout_ms);
        if (ready <= 0) {
            return false;
        }
        const ssize_t count = ::read(fd, bytes, remaining);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (count == 0) {
            return false;
        }
        bytes += count;
        remaining -= static_cast<std::size_t>(count);
    }
    return true;
}

[[noreturn]] void child_block_forever(int notify_fd) {
    const char ready = 'R';
    if (!write_all_fd(notify_fd, &ready, 1)) {
        ::_exit(91);
    }
    for (;;) {
        ::pause();
    }
}

enum class ChildReadyStatus { kReady, kError, kExited };

struct ChildReadyResult {
    ChildReadyStatus status = ChildReadyStatus::kExited;
    std::string message;
};

class CrashChild {
  public:
    template <typename Function>
    bool spawn(Function&& function, std::string* error) {
        int descriptors[2] = {-1, -1};
        if (::pipe(descriptors) != 0) {
            *error = std::string("pipe failed: ") + std::strerror(errno);
            return false;
        }
        pid_ = ::fork();
        if (pid_ < 0) {
            *error = std::string("fork failed: ") + std::strerror(errno);
            ::close(descriptors[0]);
            ::close(descriptors[1]);
            return false;
        }
        if (pid_ == 0) {
            ::close(descriptors[0]);
            try {
                std::forward<Function>(function)(descriptors[1]);
                ::_exit(0);
            } catch (const std::exception& exception) {
                std::string message = "E";
                message += exception.what();
                (void)write_all_fd(descriptors[1], message.data(),
                                   message.size());
                ::_exit(90);
            } catch (...) {
                const std::string message = "Eunknown child exception";
                (void)write_all_fd(descriptors[1], message.data(),
                                   message.size());
                ::_exit(90);
            }
        }
        ::close(descriptors[1]);
        read_fd_ = descriptors[0];
        return true;
    }

    ChildReadyResult wait_ready(int timeout_ms = 10000) {
        ChildReadyResult result;
        char marker = '\0';
        if (!read_exact_timeout(read_fd_, &marker, 1, timeout_ms)) {
            result.status = ChildReadyStatus::kExited;
            result.message = "child exited before reaching the crash point";
            return result;
        }
        if (marker == 'R') {
            result.status = ChildReadyStatus::kReady;
            return result;
        }
        if (marker != 'E') {
            result.status = ChildReadyStatus::kError;
            result.message = "unexpected child readiness marker";
            return result;
        }
        result.status = ChildReadyStatus::kError;
        char buffer[512];
        for (;;) {
            const ssize_t count = ::read(read_fd_, buffer, sizeof(buffer));
            if (count > 0) {
                result.message.append(buffer, static_cast<std::size_t>(count));
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        return result;
    }

    bool kill_and_wait(std::string* error) {
        if (pid_ <= 0) {
            *error = "child pid is not valid";
            return false;
        }
        if (::kill(pid_, SIGKILL) != 0 && errno != ESRCH) {
            *error = std::string("kill failed: ") + std::strerror(errno);
            return false;
        }
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0) {
            if (errno != EINTR) {
                *error = std::string("waitpid failed: ") + std::strerror(errno);
                return false;
            }
        }
        pid_ = -1;
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
            *error = "child did not terminate by SIGKILL";
            return false;
        }
        return true;
    }

    int read_fd() const {
        return read_fd_;
    }

    std::string drain() {
        std::string data;
        char buffer[4096];
        for (;;) {
            const ssize_t count = ::read(read_fd_, buffer, sizeof(buffer));
            if (count > 0) {
                data.append(buffer, static_cast<std::size_t>(count));
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        return data;
    }

    ~CrashChild() {
        if (pid_ > 0) {
            (void)::kill(pid_, SIGKILL);
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
        if (read_fd_ >= 0) {
            ::close(read_fd_);
        }
    }

    CrashChild() = default;
    CrashChild(const CrashChild&) = delete;
    CrashChild& operator=(const CrashChild&) = delete;

  private:
    pid_t pid_ = -1;
    int read_fd_ = -1;
};

bool await_and_kill(CrashChild* child, std::string* error) {
    const ChildReadyResult ready = child->wait_ready();
    if (ready.status != ChildReadyStatus::kReady) {
        *error = ready.message;
        return false;
    }
    return child->kill_and_wait(error);
}

template <typename Configure>
bool crash_during_mutation(
    const DatabasePaths& paths, Configure&& configure,
    const std::function<void(PersistenceManager&)>& mutation,
    std::string* error) {
    CrashChild child;
    if (!child.spawn(
            [&configure, &mutation, &paths](int notify_fd) {
                auto operations = std::make_shared<FaultOps>();
                operations->set_notify_fd(notify_fd);
                configure(*operations);
                auto store = std::make_shared<KvStore>();
                PersistenceManager manager(store, paths.rdb,
                                           DurabilityMode::kSync, operations,
                                           operations);
                std::string manager_error;
                if (!manager.open(&manager_error)) {
                    throw std::runtime_error(manager_error);
                }
                mutation(manager);
            },
            error)) {
        return false;
    }
    return await_and_kill(&child, error);
}

bool verify_manager_recovery(
    const DatabasePaths& paths,
    const std::vector<std::pair<std::string, std::string>>& expected,
    const std::vector<std::string>& absent, std::string* error) {
    auto store = std::make_shared<KvStore>();
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    if (!manager.open(error)) {
        return false;
    }
    for (const auto& [key, value] : expected) {
        const auto actual = store->get(key);
        if (!actual.has_value() || *actual != value) {
            *error = "missing or incorrect recovered value for key " + key;
            return false;
        }
    }
    for (const auto& key : absent) {
        if (store->get(key).has_value()) {
            *error = "unexpected recovered value for key " + key;
            return false;
        }
    }
    return true;
}

TEST(CrashConsistencyTest, C01CrashBeforeWalAppendLeavesOldState) {
    DatabasePaths paths("minidb_crash_c01_");
    std::string error;
    ASSERT_TRUE(seed_values(paths, {{"acked", "stable"}}, false, &error))
        << error;
    ASSERT_TRUE(crash_during_mutation(
        paths,
        [](FaultOps& operations) {
            operations.block_before_write(FaultOps::FileKind::kWalSegment);
        },
        [](PersistenceManager& manager) {
            (void)manager.set("unacknowledged", "value");
        },
        &error))
        << error;

    ASSERT_TRUE(verify_manager_recovery(paths, {{"acked", "stable"}},
                                        {"unacknowledged"}, &error))
        << error;
}

TEST(CrashConsistencyTest, C02HalfWalHeaderIsTruncatedOnRecovery) {
    DatabasePaths paths("minidb_crash_c02_");
    std::string error;
    ASSERT_TRUE(seed_values(paths, {{"acked", "stable"}}, false, &error))
        << error;
    ASSERT_TRUE(crash_during_mutation(
        paths,
        [](FaultOps& operations) {
            operations.block_after_partial_write(
                FaultOps::FileKind::kWalSegment, 5);
        },
        [](PersistenceManager& manager) {
            (void)manager.set("torn-header", "value");
        },
        &error))
        << error;

    ASSERT_TRUE(verify_manager_recovery(paths, {{"acked", "stable"}},
                                        {"torn-header"}, &error))
        << error;
}

TEST(CrashConsistencyTest, C03HalfWalPayloadIsTruncatedOnRecovery) {
    DatabasePaths paths("minidb_crash_c03_");
    std::string error;
    ASSERT_TRUE(seed_values(paths, {{"acked", "stable"}}, false, &error))
        << error;
    ASSERT_TRUE(crash_during_mutation(
        paths,
        [](FaultOps& operations) {
            operations.block_after_partial_write(
                FaultOps::FileKind::kWalSegment,
                static_cast<int>(WalFormat::HEADER_SIZE + 9));
        },
        [](PersistenceManager& manager) {
            (void)manager.set("torn-payload", "value-with-a-long-tail");
        },
        &error))
        << error;

    ASSERT_TRUE(verify_manager_recovery(paths, {{"acked", "stable"}},
                                        {"torn-payload"}, &error))
        << error;
}

TEST(CrashConsistencyTest, C04CompleteRecordBeforeFsyncMayReappearIntact) {
    DatabasePaths paths("minidb_crash_c04_");
    std::string error;
    ASSERT_TRUE(seed_values(paths, {{"acked", "stable"}}, false, &error))
        << error;
    ASSERT_TRUE(crash_during_mutation(
        paths,
        [](FaultOps& operations) {
            operations.block_before_fsync(FaultOps::FileKind::kWalSegment);
        },
        [](PersistenceManager& manager) {
            (void)manager.set("maybe", "complete-value");
        },
        &error))
        << error;

    auto store = std::make_shared<KvStore>();
    PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
    ASSERT_TRUE(manager.open(&error)) << error;
    EXPECT_EQ(store->get("acked"), std::optional<std::string>("stable"));
    const std::optional<std::string> maybe = store->get("maybe");
    EXPECT_FALSE(maybe.has_value() && *maybe != "complete-value")
        << "recovery exposed a partial value";
}

TEST(CrashConsistencyTest, C05FsyncBeforeMemoryApplyIsReplayed) {
    DatabasePaths paths("minidb_crash_c05_");
    std::string error;
    ASSERT_TRUE(seed_values(paths, {{"acked", "stable"}}, false, &error))
        << error;
    ASSERT_TRUE(crash_during_mutation(
        paths,
        [](FaultOps& operations) {
            operations.block_after_fsync(FaultOps::FileKind::kWalSegment);
        },
        [](PersistenceManager& manager) {
            (void)manager.set("durable", "value");
        },
        &error))
        << error;

    ASSERT_TRUE(verify_manager_recovery(
        paths, {{"acked", "stable"}, {"durable", "value"}}, {}, &error))
        << error;
}

TEST(CrashConsistencyTest, C06CrashAfterApplyBeforeAckRecoversAppliedValue) {
    DatabasePaths paths("minidb_crash_c06_");
    std::string error;
    ASSERT_TRUE(seed_values(paths, {{"acked", "stable"}}, false, &error))
        << error;

    CrashChild child;
    ASSERT_TRUE(child.spawn(
        [&paths](int notify_fd) {
            auto store = std::make_shared<KvStore>();
            PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync);
            std::string manager_error;
            if (!manager.open(&manager_error)) {
                throw std::runtime_error(manager_error);
            }
            manager.set_after_apply_callback([notify_fd](const WalRecord&) {
                child_block_forever(notify_fd);
            });
            (void)manager.set("applied-before-ack", "value");
        },
        &error))
        << error;
    ASSERT_TRUE(await_and_kill(&child, &error)) << error;

    ASSERT_TRUE(verify_manager_recovery(
        paths, {{"acked", "stable"}, {"applied-before-ack", "value"}}, {},
        &error))
        << error;
}

TEST(CrashConsistencyTest, C07CrashDuringSnapshotTempWriteKeepsWalRecoverable) {
    DatabasePaths paths("minidb_crash_c07_");
    std::string error;
    ASSERT_TRUE(seed_values(paths, {{"acked", "stable"}}, false, &error))
        << error;
    ASSERT_TRUE(crash_during_mutation(
        paths,
        [](FaultOps& operations) {
            operations.block_after_partial_write(
                FaultOps::FileKind::kRdbSnapshot, 12);
        },
        [](PersistenceManager& manager) {
            const MutationOutcome outcome =
                manager.set("before-snapshot", "value");
            if (!outcome.ok()) {
                throw std::runtime_error(outcome.error);
            }
            std::string checkpoint_error;
            const RdbSaveResult checkpoint_result =
                manager.checkpoint(&checkpoint_error);
            if (checkpoint_result != RdbSaveResult::Ok) {
                throw std::runtime_error(
                    "checkpoint failed before crash point: " +
                    checkpoint_error);
            }
        },
        &error))
        << error;

    ASSERT_TRUE(verify_manager_recovery(
        paths, {{"acked", "stable"}, {"before-snapshot", "value"}}, {}, &error))
        << error;
}

TEST(CrashConsistencyTest,
     C08CrashAfterSnapshotFsyncBeforeRenameKeepsOldState) {
    DatabasePaths paths("minidb_crash_c08_");
    std::string error;
    ASSERT_TRUE(seed_values(paths, {{"acked", "stable"}}, false, &error))
        << error;
    ASSERT_TRUE(crash_during_mutation(
        paths,
        [](FaultOps& operations) {
            operations.block_after_fsync(FaultOps::FileKind::kRdbSnapshot);
        },
        [](PersistenceManager& manager) {
            const MutationOutcome outcome =
                manager.set("before-snapshot", "value");
            if (!outcome.ok()) {
                throw std::runtime_error(outcome.error);
            }
            std::string checkpoint_error;
            const RdbSaveResult checkpoint_result =
                manager.checkpoint(&checkpoint_error);
            if (checkpoint_result != RdbSaveResult::Ok) {
                throw std::runtime_error(
                    "checkpoint failed before crash point: " +
                    checkpoint_error);
            }
        },
        &error))
        << error;

    ASSERT_TRUE(verify_manager_recovery(
        paths, {{"acked", "stable"}, {"before-snapshot", "value"}}, {}, &error))
        << error;
}

TEST(CrashConsistencyTest, C09OldManifestReplaysWalOverNewSnapshot) {
    DatabasePaths paths("minidb_crash_c09_");
    std::string error;
    ASSERT_TRUE(seed_values(paths, {{"seed", "base"}}, true, &error)) << error;
    ASSERT_TRUE(crash_during_mutation(
        paths,
        [](FaultOps& operations) {
            operations.block_after_rename(FaultOps::FileKind::kRdbSnapshot);
        },
        [](PersistenceManager& manager) {
            const MutationOutcome set = manager.set("new-snapshot", "value");
            if (!set.ok()) {
                throw std::runtime_error(set.error);
            }
            const MutationOutcome remove = manager.del("seed");
            if (!remove.ok()) {
                throw std::runtime_error(remove.error);
            }
            std::string checkpoint_error;
            const RdbSaveResult checkpoint_result =
                manager.checkpoint(&checkpoint_error);
            if (checkpoint_result != RdbSaveResult::Ok) {
                throw std::runtime_error(
                    "checkpoint failed before crash point: " +
                    checkpoint_error);
            }
        },
        &error))
        << error;

    ASSERT_TRUE(verify_manager_recovery(paths, {{"new-snapshot", "value"}},
                                        {"seed"}, &error))
        << error;
}

TEST(CrashConsistencyTest, C10HalfManifestWriteLeavesOldManifestAuthoritative) {
    DatabasePaths paths("minidb_crash_c10_");
    std::string error;
    ASSERT_TRUE(seed_values(paths, {{"seed", "base"}}, true, &error)) << error;
    ASSERT_TRUE(crash_during_mutation(
        paths,
        [](FaultOps& operations) {
            operations.block_after_partial_write(FaultOps::FileKind::kManifest,
                                                 11);
        },
        [](PersistenceManager& manager) {
            const MutationOutcome outcome =
                manager.set("checkpointed", "value");
            if (!outcome.ok()) {
                throw std::runtime_error(outcome.error);
            }
            std::string checkpoint_error;
            const RdbSaveResult checkpoint_result =
                manager.checkpoint(&checkpoint_error);
            if (checkpoint_result != RdbSaveResult::Ok) {
                throw std::runtime_error(
                    "checkpoint failed before crash point: " +
                    checkpoint_error);
            }
        },
        &error))
        << error;

    ASSERT_TRUE(verify_manager_recovery(
        paths, {{"seed", "base"}, {"checkpointed", "value"}}, {}, &error))
        << error;
}

TEST(CrashConsistencyTest,
     C11ManifestRenameBeforeDirectoryFsyncRemainsRecoverable) {
    DatabasePaths paths("minidb_crash_c11_");
    std::string error;
    ASSERT_TRUE(seed_values(paths, {{"seed", "base"}}, true, &error)) << error;
    ASSERT_TRUE(crash_during_mutation(
        paths,
        [](FaultOps& operations) {
            operations.block_after_rename(FaultOps::FileKind::kManifest);
        },
        [](PersistenceManager& manager) {
            const MutationOutcome outcome =
                manager.set("checkpointed", "value");
            if (!outcome.ok()) {
                throw std::runtime_error(outcome.error);
            }
            std::string checkpoint_error;
            const RdbSaveResult checkpoint_result =
                manager.checkpoint(&checkpoint_error);
            if (checkpoint_result != RdbSaveResult::Ok) {
                throw std::runtime_error(
                    "checkpoint failed before crash point: " +
                    checkpoint_error);
            }
        },
        &error))
        << error;

    ASSERT_TRUE(verify_manager_recovery(
        paths, {{"seed", "base"}, {"checkpointed", "value"}}, {}, &error))
        << error;
}

TEST(CrashConsistencyTest,
     C12CrashDuringObsoleteWalUnlinkUsesPublishedManifest) {
    DatabasePaths paths("minidb_crash_c12_");
    std::string error;
    ASSERT_TRUE(seed_values(paths, {{"first", "one"}}, true, &error)) << error;
    ASSERT_TRUE(crash_during_mutation(
        paths,
        [](FaultOps& operations) {
            operations.block_after_unlink(FaultOps::FileKind::kWalSegment);
        },
        [](PersistenceManager& manager) {
            const MutationOutcome outcome = manager.set("second", "two");
            if (!outcome.ok()) {
                throw std::runtime_error(outcome.error);
            }
            std::string checkpoint_error;
            const RdbSaveResult checkpoint_result =
                manager.checkpoint(&checkpoint_error);
            if (checkpoint_result != RdbSaveResult::Ok) {
                throw std::runtime_error(
                    "checkpoint failed before crash point: " +
                    checkpoint_error);
            }
        },
        &error))
        << error;

    ASSERT_TRUE(verify_manager_recovery(
        paths, {{"first", "one"}, {"second", "two"}}, {}, &error))
        << error;
}

TEST(CrashConsistencyTest,
     C13FixedSeedRandomSigkillPreservesEveryAcknowledgedWrite) {
    constexpr std::uint64_t kOperationCount = 20000;
    constexpr std::uint64_t kRequiredInitialAcks = 3;
    constexpr std::uint64_t kSeeds[] = {0x5eed1234u, 0x0badf00du, 0x13572468u};
    constexpr int kDelaysUs[] = {500, 3500, 12000};

    for (std::size_t round = 0; round < std::size(kSeeds); ++round) {
        DatabasePaths paths("minidb_crash_c13_");
        std::string error;
        ASSERT_TRUE(
            seed_values(paths, {{"acked-base", "stable"}}, false, &error))
            << error;

        CrashChild child;
        ASSERT_TRUE(child.spawn(
            [&paths](int notify_fd) {
                auto store = std::make_shared<KvStore>();
                PersistenceManager manager(store, paths.rdb,
                                           DurabilityMode::kSync);
                std::string manager_error;
                if (!manager.open(&manager_error)) {
                    throw std::runtime_error(manager_error);
                }
                const char ready = 'R';
                if (!write_all_fd(notify_fd, &ready, 1)) {
                    throw std::runtime_error("failed to signal readiness");
                }
                for (std::uint64_t index = 0; index < kOperationCount;
                     ++index) {
                    const std::string key =
                        "crash-key-" + std::to_string(index);
                    const std::string value =
                        "crash-value-" + std::to_string(index);
                    const MutationOutcome outcome = manager.set(key, value);
                    if (!outcome.ok()) {
                        throw std::runtime_error(outcome.error);
                    }
                    if (!write_all_fd(notify_fd, &index, sizeof(index))) {
                        throw std::runtime_error(
                            "failed to record acknowledged operation");
                    }
                }
            },
            &error))
            << error;

        const ChildReadyResult ready = child.wait_ready();
        ASSERT_EQ(ready.status, ChildReadyStatus::kReady) << ready.message;
        std::vector<std::uint64_t> acked;
        for (std::uint64_t index = 0; index < kRequiredInitialAcks; ++index) {
            std::uint64_t acknowledged = 0;
            ASSERT_TRUE(read_exact_timeout(child.read_fd(), &acknowledged,
                                           sizeof(acknowledged), 10000))
                << "child did not acknowledge initial writes";
            ASSERT_EQ(acknowledged, index);
            acked.push_back(acknowledged);
        }

        std::mt19937 generator(static_cast<std::uint32_t>(kSeeds[round]));
        std::uniform_int_distribution<int> jitter(0, 500);
        std::this_thread::sleep_for(
            std::chrono::microseconds(kDelaysUs[round] + jitter(generator)));
        ASSERT_TRUE(child.kill_and_wait(&error)) << error;

        const std::string acknowledgements = child.drain();
        ASSERT_EQ(acknowledgements.size() % sizeof(std::uint64_t), 0u);
        for (std::size_t offset = 0; offset < acknowledgements.size();
             offset += sizeof(std::uint64_t)) {
            std::uint64_t value = 0;
            std::memcpy(&value, acknowledgements.data() + offset,
                        sizeof(value));
            acked.push_back(value);
        }
        ASSERT_GE(acked.size(), kRequiredInitialAcks);
        for (std::size_t index = 0; index < acked.size(); ++index) {
            ASSERT_EQ(acked[index], index)
                << "acknowledgement stream is not contiguous";
        }

        auto recovered = std::make_shared<KvStore>();
        PersistenceManager manager(recovered, paths.rdb, DurabilityMode::kSync);
        ASSERT_TRUE(manager.open(&error)) << error;
        EXPECT_EQ(recovered->get("acked-base"),
                  std::optional<std::string>("stable"));
        for (std::uint64_t index = 0; index < acked.size(); ++index) {
            const std::string key = "crash-key-" + std::to_string(index);
            const std::string value = "crash-value-" + std::to_string(index);
            ASSERT_EQ(recovered->get(key), std::optional<std::string>(value))
                << "acknowledged write was lost at round " << round << " index "
                << index;
        }
        for (const KvStore::Entry& entry : recovered->export_all()) {
            if (entry.key.rfind("crash-key-", 0) != 0) {
                continue;
            }
            const std::uint64_t index =
                std::stoull(entry.key.substr(std::strlen("crash-key-")));
            ASSERT_EQ(entry.value, "crash-value-" + std::to_string(index))
                << "recovery exposed a partial or mixed value";
        }
    }
}

TEST(CrashConsistencyTest, C14IoErrorsAreReportedAndFailClosed) {
    {
        DatabasePaths paths("minidb_crash_c14_short_");
        std::string error;
        auto operations = std::make_shared<FaultOps>();
        operations->set_short_writes(3);
        operations->set_eintr_writes(2);
        auto store = std::make_shared<KvStore>();
        {
            PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync,
                                       operations);
            ASSERT_TRUE(manager.open(&error)) << error;
            const MutationOutcome outcome = manager.set("short-eintr", "value");
            ASSERT_TRUE(outcome.ok()) << outcome.error;
        }

        auto recovered = std::make_shared<KvStore>();
        PersistenceManager restarted(recovered, paths.rdb,
                                     DurabilityMode::kSync);
        ASSERT_TRUE(restarted.open(&error)) << error;
        EXPECT_EQ(recovered->get("short-eintr"),
                  std::optional<std::string>("value"));
    }

    {
        DatabasePaths paths("minidb_crash_c14_enospc_");
        std::string error;
        ASSERT_TRUE(seed_values(paths, {{"acked", "stable"}}, false, &error))
            << error;
        auto operations = std::make_shared<FaultOps>();
        operations->set_partial_write_then_fail(
            1, static_cast<int>(WalFormat::HEADER_SIZE + 5), ENOSPC);
        auto store = std::make_shared<KvStore>();
        {
            PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync,
                                       operations);
            ASSERT_TRUE(manager.open(&error)) << error;
            const MutationOutcome outcome = manager.set("enospc", "value");
            EXPECT_EQ(outcome.status, MutationStatus::kIoError);
            EXPECT_FALSE(outcome.error.empty());
            EXPECT_TRUE(manager.failed());
        }

        auto recovered = std::make_shared<KvStore>();
        PersistenceManager restarted(recovered, paths.rdb,
                                     DurabilityMode::kSync);
        ASSERT_TRUE(restarted.open(&error)) << error;
        EXPECT_EQ(recovered->get("acked"),
                  std::optional<std::string>("stable"));
        EXPECT_FALSE(recovered->get("enospc").has_value());
    }

    {
        DatabasePaths paths("minidb_crash_c14_eio_");
        std::string error;
        ASSERT_TRUE(seed_values(paths, {{"acked", "stable"}}, false, &error))
            << error;
        auto operations = std::make_shared<FaultOps>();
        operations->set_fail_fsync(EIO);
        auto store = std::make_shared<KvStore>();
        PersistenceManager manager(store, paths.rdb, DurabilityMode::kSync,
                                   operations);
        ASSERT_TRUE(manager.open(&error)) << error;
        const MutationOutcome outcome = manager.set("eio", "possible");
        EXPECT_EQ(outcome.status, MutationStatus::kIoError);
        EXPECT_FALSE(outcome.error.empty());
        EXPECT_TRUE(manager.failed());
        EXPECT_EQ(manager.set("after-failure", "rejected").status,
                  MutationStatus::kNotReady);
    }
}

} // namespace
} // namespace minidb
