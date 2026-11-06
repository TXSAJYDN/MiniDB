#include <gtest/gtest.h>

#include "../src/storage/KvStore.h"
#include "RdbTestUtil.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace minidb {
namespace {

struct ModelEntry {
    std::string value;
    std::optional<int64_t> deadline_ms;
};

class KvStoreTtlStressTest : public ::testing::Test {
  protected:
    static std::string key_for(uint64_t index) {
        return "key-" + std::to_string(index % 8);
    }

    static bool model_expired(const ModelEntry& entry, int64_t now_ms) {
        return entry.deadline_ms.has_value() && now_ms >= *entry.deadline_ms;
    }
};

TEST_F(KvStoreTtlStressTest, FixedSeedModelMatchesCommandAndSweepSemantics) {
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    KvStore store(clock);
    std::unordered_map<std::string, ModelEntry> model;
    std::mt19937_64 rng(0x5eed20261106ULL);
    int64_t now_ms = 1000;

    for (int step = 0; step < 6000; ++step) {
        const uint64_t key_index = rng() % 8;
        const std::string key = key_for(key_index);
        const uint64_t operation = rng() % 8;
        SCOPED_TRACE(::testing::Message()
                     << "step=" << step << " key=" << key
                     << " operation=" << operation << " now=" << now_ms);

        switch (operation) {
        case 0: {
            const std::string value = "value-" + std::to_string(step);
            store.set(key, value);
            model[key] = ModelEntry{value, std::nullopt};
            break;
        }
        case 1: {
            auto model_it = model.find(key);
            std::optional<std::string> expected;
            if (model_it != model.end()) {
                if (model_expired(model_it->second, now_ms)) {
                    model.erase(model_it);
                } else {
                    expected = model_it->second.value;
                }
            }
            EXPECT_EQ(store.get(key), expected);
            break;
        }
        case 2: {
            auto model_it = model.find(key);
            bool expected = false;
            if (model_it != model.end()) {
                if (model_expired(model_it->second, now_ms)) {
                    model.erase(model_it);
                } else {
                    model.erase(model_it);
                    expected = true;
                }
            }
            EXPECT_EQ(store.del(key), expected);
            break;
        }
        case 3:
        case 4: {
            const int64_t ttl_ms = operation == 3
                                       ? static_cast<int64_t>(rng() % 4) - 1
                                       : static_cast<int64_t>(rng() % 12) + 1;
            auto model_it = model.find(key);
            bool expected = false;
            if (model_it != model.end()) {
                if (model_expired(model_it->second, now_ms)) {
                    model.erase(model_it);
                } else if (ttl_ms <= 0) {
                    model.erase(model_it);
                    expected = true;
                } else {
                    model_it->second.deadline_ms = now_ms + ttl_ms;
                    expected = true;
                }
            }
            EXPECT_EQ(store.expire(key, ttl_ms), expected);
            break;
        }
        case 5: {
            auto model_it = model.find(key);
            std::optional<int64_t> expected;
            if (model_it != model.end()) {
                if (model_expired(model_it->second, now_ms)) {
                    model.erase(model_it);
                } else if (!model_it->second.deadline_ms.has_value()) {
                    expected = int64_t{-1};
                } else {
                    expected = std::max<int64_t>(
                        0, *model_it->second.deadline_ms - now_ms);
                }
            }
            EXPECT_EQ(store.ttl(key), expected);
            break;
        }
        case 6: {
            size_t expected_removed = 0;
            for (auto model_it = model.begin(); model_it != model.end();) {
                if (model_expired(model_it->second, now_ms)) {
                    model_it = model.erase(model_it);
                    ++expected_removed;
                } else {
                    ++model_it;
                }
            }
            EXPECT_EQ(store.sweep_expired(), expected_removed);
            break;
        }
        case 7: {
            const int64_t advance = static_cast<int64_t>(rng() % 3);
            now_ms += advance;
            clock->set_monotonic_ms(now_ms);
            clock->advance_wall(advance);
            break;
        }
        default:
            FAIL() << "unreachable operation";
        }
    }

    size_t expected_live = 0;
    for (const auto& [key, entry] : model) {
        if (!model_expired(entry, now_ms)) {
            ++expected_live;
            EXPECT_EQ(store.get(key), std::optional<std::string>(entry.value));
        }
    }
    EXPECT_EQ(store.size(), expected_live);
}

TEST_F(KvStoreTtlStressTest, ScannerAndSameKeyMutationsRemainLiveAndConverge) {
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(5000, 1700000000000);
    KvStore store(clock);
    store.start_scanner(std::chrono::milliseconds(1));
    store.start_scanner(std::chrono::milliseconds(1));
    ASSERT_TRUE(store.scanner_running());

    constexpr int kWorkers = 4;
    constexpr int kOperationsPerWorker = 2500;
    std::atomic<bool> start{false};
    std::atomic<bool> stop_clock{false};
    std::atomic<bool> invariant_failed{false};
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);

    for (int worker = 0; worker < kWorkers; ++worker) {
        workers.emplace_back([&, worker]() {
            std::mt19937_64 rng(0xabc000ULL + static_cast<uint64_t>(worker));
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int step = 0; step < kOperationsPerWorker; ++step) {
                const std::string key = key_for(rng() % 16);
                switch (rng() % 5) {
                case 0:
                    store.set(key, "v-" + std::to_string(worker) + "-" +
                                       std::to_string(step));
                    break;
                case 1: {
                    const auto value = store.get(key);
                    if (value.has_value() && value->empty()) {
                        invariant_failed.store(true, std::memory_order_relaxed);
                    }
                    break;
                }
                case 2:
                    (void)store.del(key);
                    break;
                case 3:
                    (void)store.expire(key, static_cast<int64_t>(rng() % 5));
                    break;
                case 4: {
                    const auto info = store.ttl_info(key);
                    if (info.state == KvStore::TtlState::kExpiring &&
                        info.remaining_ms < 0) {
                        invariant_failed.store(true, std::memory_order_relaxed);
                    }
                    if (info.state == KvStore::TtlState::kNoExpiry &&
                        info.remaining_ms != -1) {
                        invariant_failed.store(true, std::memory_order_relaxed);
                    }
                    break;
                }
                default:
                    invariant_failed.store(true, std::memory_order_relaxed);
                }
            }
        });
    }

    std::thread clock_thread([&]() {
        while (!stop_clock.load(std::memory_order_acquire)) {
            clock->advance_monotonic(1);
            clock->advance_wall(1);
            std::this_thread::yield();
        }
    });

    start.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }
    stop_clock.store(true, std::memory_order_release);
    clock_thread.join();

    EXPECT_FALSE(invariant_failed.load(std::memory_order_relaxed));
    EXPECT_TRUE(store.scanner_running());
    store.stop_scanner();
    EXPECT_FALSE(store.scanner_running());
    store.stop_scanner();
    EXPECT_FALSE(store.scanner_running());

    store.clear();
    for (int i = 0; i < 128; ++i) {
        const std::string key = "expire-" + std::to_string(i);
        store.set(key, "value");
        ASSERT_TRUE(store.expire(key, 5));
    }
    clock->advance_monotonic(100);
    clock->advance_wall(100);
    EXPECT_EQ(store.sweep_expired(), 128u);
    EXPECT_EQ(store.size(), 0u);
}

TEST_F(KvStoreTtlStressTest, ScannerLeasesShareOneThreadUntilLastRelease) {
    auto clock = std::make_shared<minidb_test::FrozenClock>(0, 1700000000000);
    KvStore store(clock);
    EXPECT_FALSE(store.scanner_running());

    auto first = store.acquire_scanner(std::chrono::milliseconds(1));
    EXPECT_TRUE(first);
    EXPECT_TRUE(store.scanner_running());
    auto second = store.acquire_scanner(std::chrono::milliseconds(1));
    EXPECT_TRUE(second);
    EXPECT_TRUE(store.scanner_running());

    first.reset();
    EXPECT_FALSE(first);
    EXPECT_TRUE(store.scanner_running());

    auto moved = std::move(second);
    EXPECT_FALSE(second);
    EXPECT_TRUE(moved);
    EXPECT_TRUE(store.scanner_running());

    moved.reset();
    EXPECT_FALSE(moved);
    EXPECT_FALSE(store.scanner_running());
}

TEST_F(KvStoreTtlStressTest, ManualOwnerAndLeaseDoNotStopEachOther) {
    auto clock = std::make_shared<minidb_test::FrozenClock>(0, 1700000000000);
    KvStore store(clock);
    store.start_scanner(std::chrono::milliseconds(1));
    EXPECT_TRUE(store.scanner_running());

    {
        auto lease = store.acquire_scanner(std::chrono::milliseconds(1));
        EXPECT_TRUE(lease);
    }
    // The manual owner still keeps the scanner alive.
    EXPECT_TRUE(store.scanner_running());

    {
        auto lease = store.acquire_scanner(std::chrono::milliseconds(1));
        store.stop_scanner();
        // Removing the manual owner must not stop an active lease.
        EXPECT_TRUE(store.scanner_running());
        EXPECT_TRUE(lease);
    }
    EXPECT_FALSE(store.scanner_running());
}

TEST_F(KvStoreTtlStressTest,
       ScannerCanBeStoppedAndRestartedWithoutLeakingAThread) {
    auto clock = std::make_shared<minidb_test::FrozenClock>(0, 1700000000000);
    KvStore store(clock);
    EXPECT_FALSE(store.scanner_running());

    store.start_scanner(std::chrono::milliseconds(1));
    EXPECT_TRUE(store.scanner_running());
    store.stop_scanner();
    EXPECT_FALSE(store.scanner_running());

    store.start_scanner(std::chrono::milliseconds(1));
    EXPECT_TRUE(store.scanner_running());
    store.stop_scanner();
    EXPECT_FALSE(store.scanner_running());
    store.stop_scanner();
    EXPECT_FALSE(store.scanner_running());
}

} // namespace
} // namespace minidb
