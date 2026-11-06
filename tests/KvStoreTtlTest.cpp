#include <gtest/gtest.h>

#include "../src/storage/KvStore.h"
#include "RdbTestUtil.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace minidb {
namespace {

class NanosecondClock final : public ClockInterface {
  public:
    NanosecondClock(int64_t now_ns, int64_t wall_ms)
        : now_ns_(now_ns), wall_ms_(wall_ms) {}

    std::chrono::steady_clock::time_point now() const override {
        return std::chrono::steady_clock::time_point(
            std::chrono::nanoseconds(now_ns_.load()));
    }

    int64_t system_now_ms() const override {
        return wall_ms_.load();
    }

    void set_now_ns(int64_t now_ns) {
        now_ns_.store(now_ns);
    }

  private:
    std::atomic<int64_t> now_ns_;
    std::atomic<int64_t> wall_ms_;
};

TEST(KvStoreTtlTest, LiveSubMillisecondDeadlineReportsZeroInsteadOfMissing) {
    auto clock = std::make_shared<NanosecondClock>(1000000001, 1700000000000);
    KvStore store(clock);
    store.set("key", "value");
    ASSERT_TRUE(store.expire("key", 1));

    // The deadline is 1 ms after construction: 1,001,000,001 ns.  At
    // 1,001,000,000 ns the key is still live, but the rounded-down remaining
    // duration is exactly zero.  Liveness must come from the deadline
    // comparison, not from the rounded display value.
    clock->set_now_ns(1001000000);
    auto info = store.ttl_info("key");
    EXPECT_EQ(info.state, KvStore::TtlState::kExpiring);
    EXPECT_EQ(info.remaining_ms, 0);
    EXPECT_EQ(store.get("key"), std::optional<std::string>("value"));

    const auto exported = store.export_all();
    ASSERT_EQ(exported.size(), 1u);
    EXPECT_EQ(exported.front().expiry_ms, 1700000000001);
    EXPECT_EQ(store.get("key"), std::optional<std::string>("value"));
}

TEST(KvStoreTtlTest, DeadlineBoundaryIsStrictlyNowLessThanDeadline) {
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(5000, 1700000000000);
    KvStore store(clock);
    store.set("key", "value");
    ASSERT_TRUE(store.expire("key", 10));

    clock->set_monotonic_ms(5009);
    auto info = store.ttl_info("key");
    EXPECT_EQ(info.state, KvStore::TtlState::kExpiring);
    EXPECT_EQ(info.remaining_ms, 1);

    clock->set_monotonic_ms(5010);
    info = store.ttl_info("key");
    EXPECT_EQ(info.state, KvStore::TtlState::kMissing);
    EXPECT_FALSE(store.get("key").has_value());
}

TEST(KvStoreTtlTest, SetClearsExistingTtl) {
    auto clock = std::make_shared<minidb_test::FrozenClock>(0, 1700000000000);
    KvStore store(clock);
    store.set("key", "old");
    ASSERT_TRUE(store.expire("key", 100));
    store.set("key", "new");
    EXPECT_EQ(store.ttl_info("key").state, KvStore::TtlState::kNoExpiry);
    EXPECT_EQ(store.get("key"), std::optional<std::string>("new"));
}

TEST(KvStoreTtlTest, ExpireMissingAndExpiredKeysReturnFalse) {
    auto clock = std::make_shared<minidb_test::FrozenClock>(0, 1700000000000);
    KvStore store(clock);
    EXPECT_FALSE(store.expire("missing", 10));
    store.set("key", "value");
    ASSERT_TRUE(store.expire("key", 10));
    clock->set_monotonic_ms(10);
    EXPECT_FALSE(store.expire("key", 10));
    EXPECT_FALSE(store.get("key").has_value());
}

TEST(KvStoreTtlTest, NonPositiveExpireRemovesLiveKeyAndIsIdempotentWithTtl) {
    auto clock = std::make_shared<minidb_test::FrozenClock>(0, 1700000000000);
    KvStore store(clock);
    store.set("key", "value");
    EXPECT_TRUE(store.expire("key", 0));
    EXPECT_EQ(store.ttl("key"), std::nullopt);
    EXPECT_FALSE(store.expire("key", -1));
}

TEST(KvStoreTtlTest, TtlCommandsReturnProtocolValuesAndRoundDown) {
    auto clock = std::make_shared<minidb_test::FrozenClock>(0, 1700000000000);
    KvStore store(clock);
    store.set("persistent", "p");
    store.set("key", "v");
    ASSERT_TRUE(store.expire("key", 1500));
    clock->set_monotonic_ms(249);
    EXPECT_EQ(store.ttl("persistent"), std::optional<int64_t>(-1));
    EXPECT_EQ(store.ttl("key"), std::optional<int64_t>(1251));
    EXPECT_EQ(store.ttl("missing"), std::nullopt);

    clock->set_monotonic_ms(1499);
    EXPECT_EQ(store.ttl("key"), std::optional<int64_t>(1));
    clock->set_monotonic_ms(1500);
    EXPECT_EQ(store.ttl("key"), std::nullopt);
}

TEST(KvStoreTtlTest, ExportOmitsOnlyActuallyExpiredEntries) {
    auto clock = std::make_shared<minidb_test::FrozenClock>(100, 1700000000000);
    KvStore store(clock);
    store.set("live", "value");
    store.set("expired", "value");
    ASSERT_TRUE(store.expire("live", 2));
    ASSERT_TRUE(store.expire("expired", 1));
    clock->set_monotonic_ms(100);
    auto entries = store.export_all();
    ASSERT_EQ(entries.size(), 2u);
    std::sort(
        entries.begin(), entries.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.key < rhs.key; });
    EXPECT_EQ(entries[0].key, "expired");
    EXPECT_EQ(entries[1].key, "live");
    EXPECT_GT(entries[1].expiry_ms, 0);

    clock->set_monotonic_ms(101);
    entries = store.export_all();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries.front().key, "live");
}

TEST(KvStoreTtlTest, WallClockJumpsDoNotChangeMonotonicExpiry) {
    auto clock = std::make_shared<minidb_test::FrozenClock>(0, 1700000000000);
    KvStore store(clock);
    store.set("key", "value");
    ASSERT_TRUE(store.expire("key", 1000));

    clock->advance_wall(1000000000);
    EXPECT_EQ(store.ttl("key"), std::optional<int64_t>(1000));
    EXPECT_EQ(store.get("key"), std::optional<std::string>("value"));

    clock->set_wall_ms(1);
    EXPECT_EQ(store.ttl("key"), std::optional<int64_t>(1000));

    const auto exported = store.export_all();
    ASSERT_EQ(exported.size(), 1u);
    EXPECT_GT(exported.front().expiry_ms, 0);
}

TEST(KvStoreTtlTest, RestoreUsesAbsoluteWallDeadlineAndRejectsEmptyKey) {
    auto clock = std::make_shared<minidb_test::FrozenClock>(0, 1000);
    KvStore store(clock);
    EXPECT_FALSE(store.restore_entry("", "value", 2000));
    ASSERT_TRUE(store.restore_entry("future", "value", 1500));
    EXPECT_EQ(store.ttl("future"), std::optional<int64_t>(500));
    ASSERT_TRUE(store.restore_entry("past", "value", 999));
    EXPECT_FALSE(store.get("past").has_value());
}

} // namespace
} // namespace minidb
