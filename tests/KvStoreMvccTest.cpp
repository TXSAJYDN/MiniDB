#include <gtest/gtest.h>

#include "../src/storage/KvStore.h"
#include "RdbTestUtil.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace minidb {
namespace {

using Kind = KvStore::AppliedMutation::Kind;

KvStore::AppliedMutation make_set(const std::string& key,
                                  const std::string& value,
                                  std::int64_t expiry_ms = 0) {
    KvStore::AppliedMutation mutation;
    mutation.kind = Kind::kSet;
    mutation.key = key;
    mutation.value = value;
    mutation.absolute_expiry_ms = expiry_ms;
    return mutation;
}

KvStore::AppliedMutation make_delete(const std::string& key) {
    KvStore::AppliedMutation mutation;
    mutation.kind = Kind::kDelete;
    mutation.key = key;
    return mutation;
}

KvStore::AppliedMutation make_expire(const std::string& key,
                                     std::int64_t expiry_ms) {
    KvStore::AppliedMutation mutation;
    mutation.kind = Kind::kExpire;
    mutation.key = key;
    mutation.absolute_expiry_ms = expiry_ms;
    return mutation;
}

TEST(KvStoreMvccTest, SnapshotReadsSeeOnlyCommitsAtOrBeforeSnapshot) {
    KvStore store;
    ASSERT_TRUE(store.apply_committed(1, {make_set("key", "one")}));
    const std::uint64_t snapshot = store.current_lsn();
    ASSERT_TRUE(store.apply_committed(2, {make_set("key", "two")}));

    EXPECT_EQ(store.get_at("key", snapshot), std::optional<std::string>("one"));
    EXPECT_EQ(store.get_at("key", 2), std::optional<std::string>("two"));
    EXPECT_EQ(store.get("key"), std::optional<std::string>("two"));
    EXPECT_EQ(store.get_at("key", 0), std::nullopt);
    EXPECT_EQ(store.current_lsn(), 2u);
    EXPECT_EQ(store.version_count(), 2u);
}

TEST(KvStoreMvccTest, SnapshotIsRepeatableAcrossLaterCommits) {
    KvStore store;
    ASSERT_TRUE(store.apply_committed(4, {make_set("key", "before")}));
    const std::uint64_t snapshot = store.current_lsn();

    ASSERT_TRUE(store.apply_committed(5, {make_set("key", "after")}));
    ASSERT_TRUE(store.apply_committed(6, {make_delete("key")}));
    ASSERT_TRUE(store.apply_committed(7, {make_set("key", "again")}));

    EXPECT_EQ(store.get_at("key", snapshot),
              std::optional<std::string>("before"));
    EXPECT_EQ(store.get_at("key", snapshot),
              std::optional<std::string>("before"));
    EXPECT_EQ(store.get_at("key", 7), std::optional<std::string>("again"));
}

TEST(KvStoreMvccTest, TombstonesHideKeysFromNewSnapshotsButNotOldOnes) {
    KvStore store;
    ASSERT_TRUE(store.apply_committed(1, {make_set("key", "value")}));
    ASSERT_TRUE(store.apply_committed(2, {make_delete("key")}));

    EXPECT_EQ(store.get_at("key", 1), std::optional<std::string>("value"));
    EXPECT_EQ(store.get_at("key", 2), std::nullopt);
    EXPECT_EQ(store.get("key"), std::nullopt);
    EXPECT_EQ(store.latest_committed_lsn("key"), 2u);
}

TEST(KvStoreMvccTest, TtlSelectionPrecedesExpiryAndNeverFallsBack) {
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    KvStore store(clock);
    ASSERT_TRUE(store.apply_committed(1, {make_set("key", "old")}));
    ASSERT_TRUE(store.apply_committed(
        2, {make_set("key", "temporary", clock->system_now_ms() + 10)}));

    const auto old_snapshot =
        std::optional<std::string>(store.get_at("key", 1));
    const auto new_snapshot =
        std::optional<std::string>(store.get_at("key", 2));
    ASSERT_EQ(old_snapshot, std::optional<std::string>("old"));
    ASSERT_EQ(new_snapshot, std::optional<std::string>("temporary"));

    clock->advance(100);
    EXPECT_EQ(store.get_at("key", 1), std::optional<std::string>("old"));
    EXPECT_EQ(store.get_at("key", 2), std::nullopt);
    EXPECT_EQ(store.get("key"), std::nullopt);
}

TEST(KvStoreMvccTest, TtlReadsAtSnapshotReportTheSelectedVersion) {
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    KvStore store(clock);
    ASSERT_TRUE(store.apply_committed(1, {make_set("key", "forever")}));
    ASSERT_TRUE(store.apply_committed(
        2, {make_set("key", "short", clock->system_now_ms() + 100)}));

    EXPECT_EQ(store.ttl_info_at("key", 1).state, KvStore::TtlState::kNoExpiry);
    EXPECT_EQ(store.ttl_info_at("key", 1).remaining_ms, -1);
    EXPECT_EQ(store.ttl_info_at("key", 2).state, KvStore::TtlState::kExpiring);
    EXPECT_EQ(store.ttl_info_at("key", 2).remaining_ms, 100);
    EXPECT_EQ(store.ttl_info_at("missing", 2).state,
              KvStore::TtlState::kMissing);
}

TEST(KvStoreMvccTest, ApplyCommittedPublishesMultipleKeysAtomically) {
    KvStore store;
    ASSERT_TRUE(
        store.apply_committed(1, {make_set("a", "a1"), make_set("b", "b1")}));
    ASSERT_TRUE(
        store.apply_committed(2, {make_set("a", "a2"), make_set("b", "b2")}));

    EXPECT_EQ(store.get("a"), std::optional<std::string>("a2"));
    EXPECT_EQ(store.get("b"), std::optional<std::string>("b2"));
    EXPECT_EQ(store.latest_committed_lsn("a"), 2u);
    EXPECT_EQ(store.latest_committed_lsn("b"), 2u);
    EXPECT_EQ(store.version_count(), 4u);
}

TEST(KvStoreMvccTest, FailedApplyPublishesNoPartialVersionsOrLsn) {
    KvStore store;
    ASSERT_TRUE(store.apply_committed(5, {make_set("a", "a1")}));

    std::vector<KvStore::AppliedMutation> mutations{
        make_set("b", "b1"),
        KvStore::AppliedMutation{Kind::kDelete, "c", "unexpected", 0}};
    std::string error;
    EXPECT_FALSE(store.apply_committed(6, mutations, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(store.current_lsn(), 5u);
    EXPECT_EQ(store.version_count(), 1u);
    EXPECT_EQ(store.get("b"), std::nullopt);
    EXPECT_EQ(store.latest_committed_lsn("c"), 0u);

    EXPECT_TRUE(store.apply_committed(6, {make_set("d", "d1")}, &error));
    EXPECT_EQ(store.current_lsn(), 6u);
    EXPECT_EQ(store.get("d"), std::optional<std::string>("d1"));
}

TEST(KvStoreMvccTest, DuplicateKeysFoldToFinalStateInOneVersion) {
    KvStore store;
    ASSERT_TRUE(store.apply_committed(
        1, {make_set("key", "one"), make_set("key", "two"), make_delete("key"),
            make_set("key", "three")}));

    EXPECT_EQ(store.get("key"), std::optional<std::string>("three"));
    EXPECT_EQ(store.version_count(), 1u);
    EXPECT_EQ(store.latest_committed_lsn("key"), 1u);

    ASSERT_TRUE(store.apply_committed(
        2, {make_set("deleted", "x"), make_delete("deleted"),
            make_expire("deleted", 1700000010000)}));
    EXPECT_EQ(store.get("deleted"), std::nullopt);
    EXPECT_EQ(store.version_count(), 2u);
}

TEST(KvStoreMvccTest, ExpireAfterSetPreservesValueAndAppliesDeadline) {
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    KvStore store(clock);
    const std::int64_t deadline = clock->system_now_ms() + 50;
    ASSERT_TRUE(store.apply_committed(
        1, {make_set("key", "value"), make_expire("key", deadline)}));

    ASSERT_EQ(store.get("key"), std::optional<std::string>("value"));
    EXPECT_EQ(store.version_count(), 1u);
    EXPECT_EQ(store.ttl_info("key").remaining_ms, 50);

    clock->advance(50);
    EXPECT_EQ(store.get("key"), std::nullopt);
}

TEST(KvStoreMvccTest, ApplyRejectsInvalidLsnAndMutationFields) {
    KvStore store;
    std::string error;
    EXPECT_FALSE(store.apply_committed(0, {make_set("key", "value")}, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(store.apply_committed(1, {}, &error));

    EXPECT_FALSE(store.apply_committed(1, {make_set("", "value")}, &error));
    EXPECT_FALSE(store.apply_committed(
        1,
        {make_set(std::string(KvStore::MAX_KEY_SIZE + 1, char{107}), "value")},
        &error));
    EXPECT_FALSE(store.apply_committed(
        1,
        {make_set("key", std::string(KvStore::MAX_VALUE_SIZE + 1, char{118}))},
        &error));
    EXPECT_FALSE(
        store.apply_committed(1, {make_set("key", "value", -1)}, &error));
    EXPECT_FALSE(store.apply_committed(
        1, {KvStore::AppliedMutation{Kind::kDelete, "key", "", 1}}, &error));
    EXPECT_FALSE(store.apply_committed(
        1, {KvStore::AppliedMutation{Kind::kExpire, "key", "", 0}}, &error));
    EXPECT_EQ(store.current_lsn(), 0u);
    EXPECT_EQ(store.version_count(), 0u);

    ASSERT_TRUE(store.apply_committed(1, {make_set("key", "value")}));
    EXPECT_FALSE(
        store.apply_committed(1, {make_set("other", "value")}, &error));
    EXPECT_FALSE(
        store.apply_committed(0, {make_set("other", "value")}, &error));
    EXPECT_EQ(store.current_lsn(), 1u);
    EXPECT_EQ(store.get("other"), std::nullopt);
}

TEST(KvStoreMvccTest, VersionLimitRejectsBeforePublication) {
    KvStore store(nullptr, KvStore::Limits{2});
    std::string error;
    EXPECT_EQ(store.apply_committed_status(
                  1,
                  {make_set("a", "1"), make_set("b", "2"), make_set("c", "3")},
                  &error),
              KvStore::ApplyStatus::kResourceLimit);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(store.current_lsn(), 0u);
    EXPECT_EQ(store.version_count(), 0u);

    ASSERT_TRUE(store.apply_committed(1, {make_set("a", "1")}));
    ASSERT_TRUE(store.apply_committed(2, {make_set("b", "2")}));
    EXPECT_EQ(store.apply_committed_status(3, {make_set("c", "3")}, &error),
              KvStore::ApplyStatus::kResourceLimit);
    EXPECT_EQ(store.current_lsn(), 2u);
    EXPECT_EQ(store.version_count(), 2u);
    EXPECT_EQ(store.get("c"), std::nullopt);
}

TEST(KvStoreMvccTest, GarbageCollectPreservesEveryActiveSnapshot) {
    KvStore store;
    ASSERT_TRUE(store.apply_committed(1, {make_set("key", "v1")}));
    ASSERT_TRUE(store.apply_committed(2, {make_set("key", "v2")}));
    ASSERT_TRUE(store.apply_committed(3, {make_set("key", "v3")}));

    KvStore::GcResult all_snapshots;
    ASSERT_TRUE(store.garbage_collect({1, 2, 3}, &all_snapshots));
    EXPECT_EQ(all_snapshots.versions_reclaimed, 0u);

    KvStore::GcResult result;
    ASSERT_TRUE(store.garbage_collect({1, 3}, &result));
    EXPECT_EQ(result.versions_before, 3u);
    EXPECT_EQ(result.versions_after, 2u);
    EXPECT_EQ(result.versions_reclaimed, 1u);
    EXPECT_EQ(store.version_count(), 2u);
    EXPECT_EQ(store.get_at("key", 1), std::optional<std::string>("v1"));
    EXPECT_EQ(store.get_at("key", 2), std::optional<std::string>("v1"));
    EXPECT_EQ(store.get_at("key", 3), std::optional<std::string>("v3"));
}

TEST(KvStoreMvccTest, GarbageCollectRemovesUnobservedIntermediateVersions) {
    KvStore store;
    ASSERT_TRUE(store.apply_committed(1, {make_set("key", "v1")}));
    ASSERT_TRUE(store.apply_committed(2, {make_set("key", "v2")}));
    ASSERT_TRUE(store.apply_committed(3, {make_set("key", "v3")}));

    ASSERT_TRUE(store.garbage_collect({1, 3}));
    KvStore::GcResult second;
    ASSERT_TRUE(store.garbage_collect({3}, &second));
    EXPECT_EQ(second.versions_reclaimed, 1u);
    EXPECT_EQ(store.version_count(), 1u);
    EXPECT_EQ(store.get_at("key", 1), std::nullopt);
    EXPECT_EQ(store.get_at("key", 3), std::optional<std::string>("v3"));
}

TEST(KvStoreMvccTest, GarbageCollectDropsFullyObsoleteTombstoneChain) {
    KvStore store;
    ASSERT_TRUE(store.apply_committed(1, {make_set("key", "value")}));
    ASSERT_TRUE(store.apply_committed(2, {make_delete("key")}));

    KvStore::GcResult result;
    ASSERT_TRUE(store.garbage_collect({2}, &result));
    EXPECT_EQ(result.versions_reclaimed, 2u);
    EXPECT_EQ(store.version_count(), 0u);
    EXPECT_EQ(store.latest_committed_lsn("key"), 0u);
    EXPECT_EQ(store.get_at("key", 1), std::nullopt);
    EXPECT_EQ(store.get_at("key", 2), std::nullopt);
}

TEST(KvStoreMvccTest, GarbageCollectDropsExpiredLatestWithoutOlderSnapshot) {
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    KvStore store(clock);
    ASSERT_TRUE(store.apply_committed(
        1, {make_set("key", "value", clock->system_now_ms() + 10)}));
    clock->advance(20);

    KvStore::GcResult result;
    ASSERT_TRUE(store.garbage_collect({}, &result));
    EXPECT_EQ(result.versions_reclaimed, 1u);
    EXPECT_EQ(store.version_count(), 0u);
    EXPECT_EQ(store.get("key"), std::nullopt);
}

TEST(KvStoreMvccTest, SwapExchangesDataLsnAndVersionMetadata) {
    KvStore left;
    KvStore right;
    ASSERT_TRUE(left.apply_committed(2, {make_set("left", "l")}));
    ASSERT_TRUE(right.apply_committed(5, {make_set("right", "r1")}));
    ASSERT_TRUE(right.apply_committed(6, {make_set("right", "r2")}));

    left.swap(right);
    EXPECT_EQ(left.current_lsn(), 6u);
    EXPECT_EQ(left.version_count(), 2u);
    EXPECT_EQ(left.get("right"), std::optional<std::string>("r2"));
    EXPECT_EQ(right.current_lsn(), 2u);
    EXPECT_EQ(right.version_count(), 1u);
    EXPECT_EQ(right.get("left"), std::optional<std::string>("l"));
}

TEST(KvStoreMvccTest, LegacyMutatorsAdvanceTheSameVersionSequence) {
    auto clock =
        std::make_shared<minidb_test::FrozenClock>(1000, 1700000000000);
    KvStore store(clock);
    store.set("key", "one");
    EXPECT_EQ(store.current_lsn(), 1u);
    store.set("key", "two");
    EXPECT_EQ(store.current_lsn(), 2u);
    EXPECT_TRUE(store.expire("key", 100));
    EXPECT_EQ(store.current_lsn(), 3u);
    EXPECT_EQ(store.get_at("key", 2), std::optional<std::string>("two"));
    EXPECT_EQ(store.ttl_info_at("key", 3).state, KvStore::TtlState::kExpiring);

    clock->advance(50);
    EXPECT_EQ(store.get_at("key", 2), std::optional<std::string>("two"));
    EXPECT_EQ(store.get_at("key", 3), std::optional<std::string>("two"));
    clock->advance(50);
    EXPECT_EQ(store.get_at("key", 3), std::nullopt);

    ASSERT_TRUE(store.apply_committed(10, {make_delete("key")}));
    EXPECT_EQ(store.current_lsn(), 10u);
    EXPECT_EQ(store.get("key"), std::nullopt);
    EXPECT_EQ(store.latest_committed_lsn("key"), 10u);
}

} // namespace
} // namespace minidb
