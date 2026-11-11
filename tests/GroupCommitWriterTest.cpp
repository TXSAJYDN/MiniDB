#include <gtest/gtest.h>

#include "../src/storage/GroupCommitWriter.h"
#include "../src/storage/WalManager.h"
#include "FaultWalFileOperations.h"
#include "RdbTestUtil.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace minidb {
namespace {

std::uint64_t SteadyNowUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

WalRecord MakeRecord(std::uint64_t lsn, std::string key = "key",
                     std::string value = "value") {
    WalRecord record;
    record.lsn = lsn;
    record.op = WalOp::kSet;
    record.key = std::move(key);
    record.value = std::move(value);
    return record;
}

class OwnerPump {
  public:
    GroupCommitWriter::OwnerDispatch dispatcher() {
        return [this](std::function<void()> callback) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                callbacks_.push_back(std::move(callback));
            }
            condition_.notify_one();
        };
    }

    bool RunUntil(
        const std::function<bool()>& predicate,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::deque<std::function<void()>> callbacks;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    callbacks.swap(callbacks_);
                }
                for (auto& callback : callbacks) {
                    callback();
                }
            }
            if (predicate()) {
                return true;
            }
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait_for(lock, std::chrono::milliseconds(10));
        }
        return predicate();
    }

  private:
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<std::function<void()>> callbacks_;
};

struct CompletionLog {
    void Record(const std::string& name, bool success,
                const std::string& error) {
        std::lock_guard<std::mutex> lock(mutex);
        names.push_back(name);
        successes.push_back(success);
        errors.push_back(error);
        condition.notify_all();
    }

    std::size_t Count() const {
        std::lock_guard<std::mutex> lock(mutex);
        return names.size();
    }

    std::vector<std::string> Names() const {
        std::lock_guard<std::mutex> lock(mutex);
        return names;
    }

    std::vector<bool> Successes() const {
        std::lock_guard<std::mutex> lock(mutex);
        return successes;
    }

    std::vector<std::string> Errors() const {
        std::lock_guard<std::mutex> lock(mutex);
        return errors;
    }

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::string> names;
    std::vector<bool> successes;
    std::vector<std::string> errors;
};

class GatedWalFileOperations : public minidb_test::FaultWalFileOperations {
  public:
    void ArmOneShotFsyncGate() {
        std::lock_guard<std::mutex> lock(gate_mutex_);
        armed_ = true;
        blocked_ = false;
        released_ = false;
    }

    bool WaitUntilGateReached(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        std::unique_lock<std::mutex> lock(gate_mutex_);
        return gate_condition_.wait_for(lock, timeout,
                                        [this]() { return blocked_; });
    }

    void ReleaseGate() {
        std::lock_guard<std::mutex> lock(gate_mutex_);
        released_ = true;
        gate_condition_.notify_all();
    }

    int fsync_fd(int fd) override {
        {
            std::unique_lock<std::mutex> lock(gate_mutex_);
            if (armed_) {
                armed_ = false;
                blocked_ = true;
                gate_condition_.notify_all();
                gate_condition_.wait_for(lock, std::chrono::seconds(10),
                                         [this]() { return released_; });
            }
        }
        return FaultWalFileOperations::fsync_fd(fd);
    }

  private:
    std::mutex gate_mutex_;
    std::condition_variable gate_condition_;
    bool armed_ = false;
    bool blocked_ = false;
    bool released_ = false;
};

class GroupCommitWriterTest : public ::testing::Test {
  protected:
    void SetUp() override {
        test_dir_ = minidb_test::make_unique_temp_dir("minidb_group_commit_");
        wal_dir_ = test_dir_ + "/db.rdb.wal";
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(test_dir_, ec);
    }

    std::string test_dir_;
    std::string wal_dir_;
};

TEST_F(GroupCommitWriterTest, DrainsRecordsInLsnOrderAndStopsCleanly) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager wal(wal_dir_, operations);
    ASSERT_TRUE(wal.open({}, 1));

    OwnerPump owner;
    CompletionLog log;
    std::shared_ptr<GroupCommitWriter> writer;
    auto drain = [&](GroupCommitCompletion completion) {
        if (completion.kind == GroupCommitCompletion::Kind::kBarrier) {
            log.Record("barrier", completion.success, completion.error);
            if (completion.barrier_completion) {
                completion.barrier_completion(completion.success,
                                              completion.error);
            }
            return;
        }
        for (GroupCommitRecordResult& result : completion.records) {
            if (writer) {
                writer->observe_applied(result, SteadyNowUs());
            }
            log.Record(std::to_string(result.record.lsn), result.ok(),
                       result.error);
            if (result.completion) {
                result.completion(result.ok(), result.error);
            }
        }
    };
    writer = std::make_shared<GroupCommitWriter>(&wal, DurabilityMode::kSync,
                                                 owner.dispatcher(), drain);
    std::string error;
    ASSERT_TRUE(writer->start(1, &error)) << error;

    for (std::uint64_t lsn = 1; lsn <= 8; ++lsn) {
        GroupCommitEntry entry;
        entry.record = MakeRecord(lsn, "key-" + std::to_string(lsn),
                                  "value-" + std::to_string(lsn));
        entry.completion = [&log, lsn](bool success, const std::string& err) {
            if (!success) {
                log.Record("callback-" + std::to_string(lsn), success, err);
            }
        };
        ASSERT_TRUE(writer->submit(std::move(entry), &error)) << error;
    }

    ASSERT_TRUE(owner.RunUntil([&]() { return log.Count() >= 8; }));
    EXPECT_EQ(log.Names(), (std::vector<std::string>{"1", "2", "3", "4", "5",
                                                     "6", "7", "8"}));
    EXPECT_EQ(wal.next_lsn(), 9u);
    EXPECT_EQ(writer->next_lsn(), 9u);
    const GroupCommitStats stats = writer->stats();
    EXPECT_GE(stats.batches, 1u);
    EXPECT_EQ(stats.batch_records, 8u);
    EXPECT_EQ(stats.batch_bytes, [&]() {
        std::uint64_t total = 0;
        for (std::uint64_t lsn = 1; lsn <= 8; ++lsn) {
            total += WalFormat::encoded_size(
                MakeRecord(lsn, "key-" + std::to_string(lsn),
                           "value-" + std::to_string(lsn)));
        }
        return total;
    }());
    EXPECT_GE(stats.submit_to_fsync_total_us, stats.submit_to_fsync_max_us);
    EXPECT_GE(stats.fsync_to_apply_total_us, stats.fsync_to_apply_max_us);

    bool barrier_called = false;
    ASSERT_TRUE(writer->request_flush(
        [&barrier_called](bool success, const std::string& err) {
            barrier_called = success;
            EXPECT_TRUE(err.empty());
        },
        &error))
        << error;
    ASSERT_TRUE(owner.RunUntil([&]() { return barrier_called; }));
    EXPECT_TRUE(barrier_called);

    writer->stop();
    EXPECT_EQ(writer->state(), GroupCommitWriterState::kStopped);
    EXPECT_FALSE(writer->failed());
}

TEST_F(GroupCommitWriterTest, IntervalAppendAcksBeforeFlushBarrier) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager wal(wal_dir_, operations);
    ASSERT_TRUE(wal.open({}, 1));
    const std::uint64_t fsync_before = wal.stats().data_fsyncs;

    OwnerPump owner;
    CompletionLog log;
    std::shared_ptr<GroupCommitWriter> writer;
    auto drain = [&](GroupCommitCompletion completion) {
        if (completion.kind == GroupCommitCompletion::Kind::kBarrier) {
            log.Record("barrier", completion.success, completion.error);
            if (completion.barrier_completion) {
                completion.barrier_completion(completion.success,
                                              completion.error);
            }
            return;
        }
        for (GroupCommitRecordResult& result : completion.records) {
            log.Record(std::to_string(result.record.lsn), result.ok(),
                       result.error);
            if (result.completion) {
                result.completion(result.ok(), result.error);
            }
        }
    };
    writer = std::make_shared<GroupCommitWriter>(
        &wal, DurabilityMode::kInterval, owner.dispatcher(), drain);
    ASSERT_TRUE(writer->start(1));

    GroupCommitEntry entry;
    entry.record = MakeRecord(1, "alpha", "beta");
    std::string error;
    ASSERT_TRUE(writer->submit(std::move(entry), &error)) << error;
    ASSERT_TRUE(owner.RunUntil([&]() { return log.Count() == 1; }));
    EXPECT_EQ(wal.stats().data_fsyncs, fsync_before);

    bool flushed = false;
    ASSERT_TRUE(writer->request_flush(
        [&flushed](bool success, const std::string& err) {
            flushed = success;
            EXPECT_TRUE(err.empty());
        },
        &error))
        << error;
    ASSERT_TRUE(owner.RunUntil([&]() { return flushed; }));
    EXPECT_TRUE(flushed);
    EXPECT_GT(wal.stats().data_fsyncs, fsync_before);
    EXPECT_GE(writer->stats().data_fsyncs, 1u);

    writer->stop();
    EXPECT_EQ(writer->state(), GroupCommitWriterState::kStopped);
}

TEST_F(GroupCommitWriterTest,
       PauseBarrierFollowsAllPriorRecordsAndResumeWorks) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    operations->set_short_writes(1);
    WalManager wal(wal_dir_, operations);
    ASSERT_TRUE(wal.open({}, 1));

    OwnerPump owner;
    CompletionLog log;
    std::shared_ptr<GroupCommitWriter> writer;
    auto drain = [&](GroupCommitCompletion completion) {
        if (completion.kind == GroupCommitCompletion::Kind::kBarrier) {
            log.Record("pause", completion.success, completion.error);
            if (completion.barrier_completion) {
                completion.barrier_completion(completion.success,
                                              completion.error);
            }
            return;
        }
        for (GroupCommitRecordResult& result : completion.records) {
            log.Record(std::to_string(result.record.lsn), result.ok(),
                       result.error);
            if (result.completion) {
                result.completion(result.ok(), result.error);
            }
        }
    };
    writer = std::make_shared<GroupCommitWriter>(&wal, DurabilityMode::kSync,
                                                 owner.dispatcher(), drain);
    ASSERT_TRUE(writer->start(1));

    std::string error;
    for (std::uint64_t lsn = 1; lsn <= 16; ++lsn) {
        GroupCommitEntry entry;
        entry.record = MakeRecord(lsn);
        ASSERT_TRUE(writer->submit(std::move(entry), &error)) << error;
    }
    bool paused = false;
    ASSERT_TRUE(writer->request_pause(
        [&paused](bool success, const std::string& err) {
            paused = success;
            EXPECT_TRUE(err.empty());
        },
        &error))
        << error;

    GroupCommitEntry late;
    late.record = MakeRecord(17);
    EXPECT_FALSE(writer->submit(std::move(late), &error));
    EXPECT_NE(error.find("not accepting"), std::string::npos);

    ASSERT_TRUE(owner.RunUntil([&]() { return paused; }));
    EXPECT_TRUE(paused);
    ASSERT_EQ(log.Names().size(), 17u);
    EXPECT_EQ(log.Names().back(), "pause");
    for (std::size_t index = 0; index < 16; ++index) {
        EXPECT_EQ(log.Names()[index], std::to_string(index + 1));
    }
    EXPECT_EQ(writer->state(), GroupCommitWriterState::kPaused);

    ASSERT_TRUE(writer->resume(&error)) << error;
    GroupCommitEntry resumed;
    resumed.record = MakeRecord(17);
    ASSERT_TRUE(writer->submit(std::move(resumed), &error)) << error;
    ASSERT_TRUE(owner.RunUntil([&]() { return log.Count() == 18; }));
    EXPECT_EQ(log.Names().back(), "17");
    writer->stop();
}

TEST_F(GroupCommitWriterTest, AppendFailureCompletesEveryAcceptedRequestOnce) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager wal(wal_dir_, operations);
    ASSERT_TRUE(wal.open({}, 1));
    operations->set_fail_write_after(0);

    OwnerPump owner;
    CompletionLog log;
    std::shared_ptr<GroupCommitWriter> writer;
    auto drain = [&](GroupCommitCompletion completion) {
        if (completion.kind == GroupCommitCompletion::Kind::kBarrier) {
            log.Record("barrier", completion.success, completion.error);
            return;
        }
        for (GroupCommitRecordResult& result : completion.records) {
            log.Record(std::to_string(result.record.lsn), result.ok(),
                       result.error);
            if (result.completion) {
                result.completion(result.ok(), result.error);
            }
        }
    };
    writer = std::make_shared<GroupCommitWriter>(&wal, DurabilityMode::kSync,
                                                 owner.dispatcher(), drain);
    ASSERT_TRUE(writer->start(1));

    std::atomic<int> completion_calls{0};
    std::string error;
    for (std::uint64_t lsn = 1; lsn <= 3; ++lsn) {
        GroupCommitEntry entry;
        entry.record = MakeRecord(lsn);
        entry.completion = [&completion_calls](bool, const std::string&) {
            ++completion_calls;
        };
        ASSERT_TRUE(writer->submit(std::move(entry), &error)) << error;
    }
    ASSERT_TRUE(owner.RunUntil([&]() { return log.Count() >= 3; }));
    for (const bool success : log.Successes()) {
        EXPECT_FALSE(success);
    }
    EXPECT_EQ(log.Count(), 3u);
    EXPECT_EQ(completion_calls.load(), 3);
    EXPECT_TRUE(writer->failed());
    EXPECT_GE(writer->stats().append_failures, 1u);

    GroupCommitEntry rejected;
    rejected.record = MakeRecord(4);
    EXPECT_FALSE(writer->submit(std::move(rejected), &error));
    writer->stop();
    EXPECT_EQ(writer->state(), GroupCommitWriterState::kFailed);
}

TEST_F(GroupCommitWriterTest, FsyncFailureIsReportedAndFailsClosed) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager wal(wal_dir_, operations);
    ASSERT_TRUE(wal.open({}, 1));
    operations->set_fail_fsync(EIO);

    OwnerPump owner;
    CompletionLog log;
    std::shared_ptr<GroupCommitWriter> writer;
    auto drain = [&](GroupCommitCompletion completion) {
        for (GroupCommitRecordResult& result : completion.records) {
            log.Record(std::to_string(result.record.lsn), result.ok(),
                       result.error);
            if (result.completion) {
                result.completion(result.ok(), result.error);
            }
        }
    };
    writer = std::make_shared<GroupCommitWriter>(&wal, DurabilityMode::kSync,
                                                 owner.dispatcher(), drain);
    ASSERT_TRUE(writer->start(1));

    GroupCommitEntry entry;
    entry.record = MakeRecord(1);
    std::string error;
    ASSERT_TRUE(writer->submit(std::move(entry), &error)) << error;
    ASSERT_TRUE(owner.RunUntil([&]() { return log.Count() == 1; }));
    EXPECT_FALSE(log.Successes().front());
    EXPECT_TRUE(writer->failed());
    EXPECT_EQ(writer->stats().fsync_failures, 1u);
    writer->stop();
}

TEST_F(GroupCommitWriterTest, QueueByteLimitRejectsWithoutInvokingCallback) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager wal(wal_dir_, operations);
    ASSERT_TRUE(wal.open({}, 1));

    OwnerPump owner;
    CompletionLog log;
    std::shared_ptr<GroupCommitWriter> writer;
    auto drain = [&](GroupCommitCompletion completion) {
        for (GroupCommitRecordResult& result : completion.records) {
            log.Record(std::to_string(result.record.lsn), result.ok(),
                       result.error);
            if (result.completion) {
                result.completion(result.ok(), result.error);
            }
        }
    };
    GroupCommitLimits limits;
    limits.max_queue_bytes = 1;
    writer = std::make_shared<GroupCommitWriter>(
        &wal, DurabilityMode::kSync, owner.dispatcher(), drain, limits);
    ASSERT_TRUE(writer->start(1));

    std::string error;
    GroupCommitEntry first;
    first.record = MakeRecord(1, "key", std::string(4096, 'v'));
    bool callback_called = false;
    first.completion = [&callback_called](bool, const std::string&) {
        callback_called = true;
    };
    EXPECT_FALSE(writer->submit(std::move(first), &error));
    EXPECT_NE(error.find("queue is full"), std::string::npos);
    EXPECT_GE(writer->stats().queue_rejections, 1u);
    EXPECT_FALSE(callback_called);
    EXPECT_EQ(log.Count(), 0u);
    writer->stop();
}

TEST_F(GroupCommitWriterTest,
       QueueEntryLimitRejectsWithoutInvokingTheRejectedCallback) {
    auto operations = std::make_shared<GatedWalFileOperations>();
    WalManager wal(wal_dir_, operations);
    ASSERT_TRUE(wal.open({}, 1));

    OwnerPump owner;
    std::atomic<int> completed{0};
    std::atomic<int> callbacks{0};
    std::shared_ptr<GroupCommitWriter> writer;
    auto drain = [&](GroupCommitCompletion completion) {
        for (GroupCommitRecordResult& result : completion.records) {
            if (result.completion) {
                result.completion(result.ok(), result.error);
            }
            ++completed;
            ++callbacks;
        }
    };
    GroupCommitLimits limits;
    limits.max_queue_entries = 1;
    writer = std::make_shared<GroupCommitWriter>(
        &wal, DurabilityMode::kSync, owner.dispatcher(), drain, limits);
    ASSERT_TRUE(writer->start(1));

    operations->ArmOneShotFsyncGate();
    std::string error;
    GroupCommitEntry first;
    first.record = MakeRecord(1);
    first.completion = [](bool, const std::string&) {};
    ASSERT_TRUE(writer->submit(std::move(first), &error)) << error;
    if (!operations->WaitUntilGateReached()) {
        operations->ReleaseGate();
        FAIL() << "writer did not reach the fsync gate";
    }

    // The first request is already in the writer's physical batch.  Fill the
    // single admitted queue slot, then verify the next request is rejected
    // without ever receiving a completion callback.
    GroupCommitEntry second;
    second.record = MakeRecord(2);
    ASSERT_TRUE(writer->submit(std::move(second), &error)) << error;
    bool rejected_callback_called = false;
    GroupCommitEntry third;
    third.record = MakeRecord(3);
    third.completion = [&rejected_callback_called](bool, const std::string&) {
        rejected_callback_called = true;
    };
    EXPECT_FALSE(writer->submit(std::move(third), &error));
    EXPECT_NE(error.find("queue is full"), std::string::npos);
    EXPECT_GE(writer->stats().queue_rejections, 1u);
    EXPECT_FALSE(rejected_callback_called);

    operations->ReleaseGate();
    ASSERT_TRUE(owner.RunUntil([&]() { return completed.load() == 2; }));
    EXPECT_EQ(callbacks.load(), 2);
    EXPECT_EQ(writer->stats().queue_max_entries, 1u);

    writer->stop();
    owner.RunUntil([]() { return true; });
    EXPECT_EQ(callbacks.load(), 2);
}

TEST_F(GroupCommitWriterTest,
       CompletionEntryLimitFailStopsAndCompletesEveryAdmittedCallbackOnce) {
    auto operations = std::make_shared<GatedWalFileOperations>();
    WalManager wal(wal_dir_, operations);
    ASSERT_TRUE(wal.open({}, 1));

    OwnerPump owner;
    std::atomic<int> record_callbacks{0};
    std::atomic<int> barrier_callbacks{0};
    std::shared_ptr<GroupCommitWriter> writer;
    auto drain = [&](GroupCommitCompletion completion) {
        if (completion.kind == GroupCommitCompletion::Kind::kBarrier) {
            ++barrier_callbacks;
            if (completion.barrier_completion) {
                completion.barrier_completion(completion.success,
                                              completion.error);
            }
            return;
        }
        for (GroupCommitRecordResult& result : completion.records) {
            ++record_callbacks;
            if (result.completion) {
                result.completion(result.ok(), result.error);
            }
        }
    };
    GroupCommitLimits limits;
    limits.max_completion_entries = 1;
    writer = std::make_shared<GroupCommitWriter>(
        &wal, DurabilityMode::kSync, owner.dispatcher(), drain, limits);
    ASSERT_TRUE(writer->start(1));

    operations->ArmOneShotFsyncGate();
    std::string error;
    GroupCommitEntry first;
    first.record = MakeRecord(1);
    first.completion = [](bool, const std::string&) {};
    ASSERT_TRUE(writer->submit(std::move(first), &error)) << error;
    if (!operations->WaitUntilGateReached()) {
        operations->ReleaseGate();
        FAIL() << "writer did not reach the fsync gate";
    }

    constexpr std::uint64_t kQueuedRecords = 8;
    for (std::uint64_t lsn = 2; lsn <= kQueuedRecords; ++lsn) {
        GroupCommitEntry entry;
        entry.record = MakeRecord(lsn);
        entry.completion = [](bool, const std::string&) {};
        ASSERT_TRUE(writer->submit(std::move(entry), &error)) << error;
    }
    ASSERT_TRUE(writer->request_flush([](bool, const std::string&) {}, &error))
        << error;

    operations->ReleaseGate();
    ASSERT_TRUE(owner.RunUntil([&]() {
        return record_callbacks.load() == static_cast<int>(kQueuedRecords) &&
               barrier_callbacks.load() == 1;
    }));
    EXPECT_TRUE(writer->failed());
    EXPECT_EQ(writer->state(), GroupCommitWriterState::kFailed);
    EXPECT_EQ(writer->stats().writer_failures, 1u);
    EXPECT_LE(writer->stats().completion_backlog_max, 1u);
    EXPECT_EQ(record_callbacks.load(), static_cast<int>(kQueuedRecords));
    EXPECT_EQ(barrier_callbacks.load(), 1);

    writer->stop();
    owner.RunUntil([]() { return true; });
    EXPECT_EQ(record_callbacks.load(), static_cast<int>(kQueuedRecords));
    EXPECT_EQ(barrier_callbacks.load(), 1);
}

TEST_F(GroupCommitWriterTest,
       CompletionByteLimitFailStopsAndInvokesCallbackExactlyOnce) {
    auto operations = std::make_shared<GatedWalFileOperations>();
    WalManager wal(wal_dir_, operations);
    ASSERT_TRUE(wal.open({}, 1));

    OwnerPump owner;
    std::atomic<int> callbacks{0};
    std::shared_ptr<GroupCommitWriter> writer;
    auto drain = [&](GroupCommitCompletion completion) {
        for (GroupCommitRecordResult& result : completion.records) {
            ++callbacks;
            if (result.completion) {
                result.completion(result.ok(), result.error);
            }
        }
    };
    GroupCommitLimits limits;
    limits.max_completion_bytes = 1;
    writer = std::make_shared<GroupCommitWriter>(
        &wal, DurabilityMode::kSync, owner.dispatcher(), drain, limits);
    ASSERT_TRUE(writer->start(1));

    operations->ArmOneShotFsyncGate();
    GroupCommitEntry entry;
    entry.record = MakeRecord(1);
    entry.completion = [](bool, const std::string&) {};
    std::string error;
    ASSERT_TRUE(writer->submit(std::move(entry), &error)) << error;
    if (!operations->WaitUntilGateReached()) {
        operations->ReleaseGate();
        FAIL() << "writer did not reach the fsync gate";
    }
    operations->ReleaseGate();

    ASSERT_TRUE(owner.RunUntil([&]() { return callbacks.load() == 1; }));
    EXPECT_TRUE(writer->failed());
    EXPECT_EQ(writer->stats().writer_failures, 1u);
    EXPECT_EQ(writer->stats().completion_backlog_max, 0u);
    writer->stop();
    owner.RunUntil([]() { return true; });
    EXPECT_EQ(callbacks.load(), 1);
}

TEST_F(GroupCommitWriterTest, ManySmallSubmissionsUseBoundedGroupedBatches) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    operations->set_short_writes(1);
    WalManager wal(wal_dir_, operations);
    ASSERT_TRUE(wal.open({}, 1));

    OwnerPump owner;
    std::atomic<std::size_t> completed{0};
    std::shared_ptr<GroupCommitWriter> writer;
    auto drain = [&](GroupCommitCompletion completion) {
        for (GroupCommitRecordResult& result : completion.records) {
            if (result.completion) {
                result.completion(result.ok(), result.error);
            }
            ++completed;
        }
    };
    EXPECT_EQ(GroupCommitLimits{}.max_batch_records, 256u);
    EXPECT_EQ(GroupCommitLimits{}.max_batch_bytes, 16u * 1024u * 1024u);
    GroupCommitLimits limits;
    limits.max_batch_records = 16;
    writer = std::make_shared<GroupCommitWriter>(
        &wal, DurabilityMode::kSync, owner.dispatcher(), drain, limits);
    ASSERT_TRUE(writer->start(1));

    constexpr std::uint64_t kRecords = 128;
    std::string error;
    for (std::uint64_t lsn = 1; lsn <= kRecords; ++lsn) {
        GroupCommitEntry entry;
        entry.record = MakeRecord(lsn);
        entry.completion = [](bool, const std::string&) {};
        ASSERT_TRUE(writer->submit(std::move(entry), &error)) << error;
    }
    ASSERT_TRUE(owner.RunUntil([&]() { return completed.load() == kRecords; }));
    const GroupCommitStats stats = writer->stats();
    EXPECT_GE(stats.batches, 1u);
    EXPECT_LE(stats.max_batch_records, 16u);
    EXPECT_LE(stats.max_batch_bytes, limits.max_batch_bytes);
    EXPECT_EQ(stats.batch_records, kRecords);
    EXPECT_EQ(stats.queue_current_entries, 0u);
    writer->stop();
}

TEST_F(GroupCommitWriterTest, WalControlsRequirePauseAndRotatePreservesLsn) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager wal(wal_dir_, operations);
    ASSERT_TRUE(wal.open({}, 1));

    OwnerPump owner;
    std::atomic<std::size_t> records{0};
    std::atomic<bool> paused{false};
    std::atomic<bool> rotated{false};
    std::atomic<bool> cleaned{false};
    GroupCommitWalBarrierInfo rotate_info;
    GroupCommitWalBarrierInfo cleanup_info;
    std::shared_ptr<GroupCommitWriter> writer;
    auto drain = [&](GroupCommitCompletion completion) {
        if (completion.kind == GroupCommitCompletion::Kind::kRecords) {
            for (GroupCommitRecordResult& result : completion.records) {
                if (result.completion) {
                    result.completion(result.ok(), result.error);
                }
                ++records;
            }
            return;
        }
        if (completion.kind == GroupCommitCompletion::Kind::kWalBarrier) {
            if (completion.wal_barrier_completion) {
                completion.wal_barrier_completion(completion.wal_barrier);
            }
            return;
        }
        if (completion.barrier_completion) {
            completion.barrier_completion(completion.success, completion.error);
        }
    };
    writer = std::make_shared<GroupCommitWriter>(&wal, DurabilityMode::kSync,
                                                 owner.dispatcher(), drain);
    std::string error;
    ASSERT_TRUE(writer->start(1, &error)) << error;

    GroupCommitEntry entry;
    entry.record = MakeRecord(1, "alpha", "beta");
    entry.completion = [](bool, const std::string&) {};
    ASSERT_TRUE(writer->submit(std::move(entry), &error)) << error;
    ASSERT_TRUE(owner.RunUntil([&]() { return records.load() == 1; }));

    EXPECT_FALSE(writer->request_rotate([](const GroupCommitWalBarrierInfo&) {},
                                        &error));
    EXPECT_NE(error.find("paused"), std::string::npos);
    EXPECT_FALSE(
        writer->request_cleanup([](bool, const std::string&) {}, &error));
    EXPECT_NE(error.find("paused"), std::string::npos);

    ASSERT_TRUE(writer->request_pause(
        [&paused](bool success, const std::string& err) {
            EXPECT_TRUE(err.empty());
            paused = success;
        },
        &error))
        << error;
    ASSERT_TRUE(owner.RunUntil([&]() { return paused.load(); }));
    const GroupCommitWalSnapshot before = writer->wal_snapshot();
    EXPECT_EQ(before.active_segment_start_lsn, 1u);
    EXPECT_GT(before.active_segment_bytes, 0u);
    EXPECT_EQ(before.group_commit.queue_current_entries, 0u);

    ASSERT_TRUE(writer->request_rotate(
        [&rotated, &rotate_info](const GroupCommitWalBarrierInfo& info) {
            rotate_info = info;
            rotated = info.success;
        },
        &error))
        << error;
    ASSERT_TRUE(owner.RunUntil([&]() { return rotated.load(); }));
    EXPECT_TRUE(rotate_info.success) << rotate_info.error;
    EXPECT_EQ(rotate_info.next_lsn, 2u);
    EXPECT_EQ(rotate_info.active_segment_start_lsn, 2u);
    EXPECT_EQ(rotate_info.active_segment_bytes, 0u);
    EXPECT_EQ(writer->next_lsn(), 2u);
    EXPECT_EQ(writer->state(), GroupCommitWriterState::kPaused);

    ASSERT_TRUE(writer->request_cleanup(
        [&cleaned, &cleanup_info](bool success, const std::string& err) {
            cleanup_info.success = success;
            cleanup_info.error = err;
            cleaned = success;
        },
        &error))
        << error;
    ASSERT_TRUE(owner.RunUntil([&]() { return cleaned.load(); }));
    EXPECT_TRUE(cleanup_info.success) << cleanup_info.error;
    const GroupCommitWalSnapshot after = writer->wal_snapshot();
    EXPECT_EQ(after.wal_segment_count, 1u);
    EXPECT_EQ(after.active_segment_start_lsn, 2u);
    EXPECT_EQ(after.wal_total_bytes, after.active_segment_bytes);
    EXPECT_TRUE(writer->resume(&error)) << error;
    writer->stop();
}

TEST_F(GroupCommitWriterTest, QueuedRotateIsReportedWhenFailStopDrainsQueue) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager wal(wal_dir_, operations);
    ASSERT_TRUE(wal.open({}, 1));

    OwnerPump owner;
    std::atomic<std::size_t> records{0};
    std::atomic<bool> paused{false};
    std::atomic<int> rotate_callbacks{0};
    std::atomic<int> failed_rotates{0};
    std::shared_ptr<GroupCommitWriter> writer;
    auto drain = [&](GroupCommitCompletion completion) {
        if (completion.kind == GroupCommitCompletion::Kind::kRecords) {
            ++records;
            return;
        }
        if (completion.kind == GroupCommitCompletion::Kind::kWalBarrier) {
            ++rotate_callbacks;
            if (!completion.wal_barrier.success) {
                ++failed_rotates;
            }
            if (completion.wal_barrier_completion) {
                completion.wal_barrier_completion(completion.wal_barrier);
            }
            return;
        }
        if (completion.barrier_completion) {
            completion.barrier_completion(completion.success, completion.error);
        }
    };
    writer = std::make_shared<GroupCommitWriter>(&wal, DurabilityMode::kSync,
                                                 owner.dispatcher(), drain);
    std::string error;
    ASSERT_TRUE(writer->start(1, &error)) << error;

    GroupCommitEntry entry;
    entry.record = MakeRecord(1);
    ASSERT_TRUE(writer->submit(std::move(entry), &error)) << error;
    ASSERT_TRUE(owner.RunUntil([&]() { return records.load() == 1; }));
    ASSERT_TRUE(writer->request_pause(
        [&paused](bool success, const std::string&) { paused = success; },
        &error))
        << error;
    ASSERT_TRUE(owner.RunUntil([&]() { return paused.load(); }));

    operations->set_fail_open_exclusive(EIO);
    ASSERT_TRUE(
        writer->request_rotate([](const GroupCommitWalBarrierInfo&) {}, &error))
        << error;
    ASSERT_TRUE(
        writer->request_rotate([](const GroupCommitWalBarrierInfo&) {}, &error))
        << error;
    ASSERT_TRUE(owner.RunUntil([&]() { return rotate_callbacks.load() == 2; }));
    EXPECT_EQ(failed_rotates.load(), 2);
    EXPECT_TRUE(writer->failed());
    EXPECT_EQ(writer->state(), GroupCommitWriterState::kFailed);
    EXPECT_EQ(writer->stats().append_failures, 1u);
    writer->stop();
}

TEST_F(GroupCommitWriterTest, CleanupFailureKeepsPausedAndReportsBarrier) {
    auto operations = std::make_shared<minidb_test::FaultWalFileOperations>();
    WalManager wal(wal_dir_, operations);
    ASSERT_TRUE(wal.open({}, 1));

    OwnerPump owner;
    std::atomic<std::size_t> records{0};
    std::atomic<bool> paused{false};
    std::atomic<bool> rotated{false};
    std::atomic<bool> cleanup_done{false};
    bool cleanup_success = true;
    std::string cleanup_error;
    std::shared_ptr<GroupCommitWriter> writer;
    auto drain = [&](GroupCommitCompletion completion) {
        if (completion.kind == GroupCommitCompletion::Kind::kRecords) {
            ++records;
            return;
        }
        if (completion.kind == GroupCommitCompletion::Kind::kWalBarrier) {
            if (completion.wal_barrier_completion) {
                completion.wal_barrier_completion(completion.wal_barrier);
            }
            rotated = completion.success;
            return;
        }
        if (completion.barrier_completion) {
            completion.barrier_completion(completion.success, completion.error);
        }
    };
    writer = std::make_shared<GroupCommitWriter>(&wal, DurabilityMode::kSync,
                                                 owner.dispatcher(), drain);
    std::string error;
    ASSERT_TRUE(writer->start(1, &error)) << error;

    GroupCommitEntry entry;
    entry.record = MakeRecord(1);
    ASSERT_TRUE(writer->submit(std::move(entry), &error)) << error;
    ASSERT_TRUE(owner.RunUntil([&]() { return records.load() == 1; }));
    ASSERT_TRUE(writer->request_pause(
        [&paused](bool success, const std::string&) { paused = success; },
        &error))
        << error;
    ASSERT_TRUE(owner.RunUntil([&]() { return paused.load(); }));
    ASSERT_TRUE(
        writer->request_rotate([](const GroupCommitWalBarrierInfo&) {}, &error))
        << error;
    ASSERT_TRUE(owner.RunUntil([&]() { return rotated.load(); }));

    operations->set_fail_unlink(EIO);
    ASSERT_TRUE(writer->request_cleanup(
        [&cleanup_done, &cleanup_success,
         &cleanup_error](bool success, const std::string& err) {
            cleanup_success = success;
            cleanup_error = err;
            cleanup_done = true;
        },
        &error))
        << error;
    ASSERT_TRUE(owner.RunUntil([&]() { return cleanup_done.load(); }));
    EXPECT_FALSE(cleanup_success);
    EXPECT_FALSE(cleanup_error.empty());
    EXPECT_EQ(writer->state(), GroupCommitWriterState::kPaused);
    EXPECT_FALSE(writer->failed());
    writer->stop();
    EXPECT_EQ(writer->state(), GroupCommitWriterState::kStopped);
}

} // namespace
} // namespace minidb
