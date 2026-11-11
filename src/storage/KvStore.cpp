#include "KvStore.h"
#include "../base/Int128.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace minidb {
namespace {

template <typename TimePoint>
TimePoint saturating_add_milliseconds(TimePoint base, int64_t milliseconds) {
    if (milliseconds <= 0) {
        return base;
    }
    using Duration = typename TimePoint::duration;
    using Rep = typename Duration::rep;
    using Period = typename Duration::period;

    const int128_t delta = (static_cast<int128_t>(milliseconds) *
                            static_cast<int128_t>(Period::den)) /
                           (static_cast<int128_t>(Period::num) * 1000);
    const int128_t current =
        static_cast<int128_t>(base.time_since_epoch().count());
    const int128_t maximum =
        static_cast<int128_t>(TimePoint::max().time_since_epoch().count());
    const int128_t minimum =
        static_cast<int128_t>(TimePoint::min().time_since_epoch().count());
    const int128_t sum = current + delta;
    if (sum >= maximum) {
        return TimePoint::max();
    }
    if (sum <= minimum) {
        return TimePoint::min();
    }
    return TimePoint(static_cast<Duration>(static_cast<Rep>(sum)));
}

int64_t
saturating_milliseconds_between(std::chrono::steady_clock::time_point later,
                                std::chrono::steady_clock::time_point earlier) {
    using Duration = std::chrono::steady_clock::duration;
    using Period = Duration::period;
    const int128_t tick_delta =
        static_cast<int128_t>(later.time_since_epoch().count()) -
        static_cast<int128_t>(earlier.time_since_epoch().count());
    const int128_t milliseconds =
        (tick_delta * static_cast<int128_t>(Period::num) * 1000) /
        static_cast<int128_t>(Period::den);
    const int128_t maximum = std::numeric_limits<int64_t>::max();
    const int128_t minimum = std::numeric_limits<int64_t>::min();
    if (milliseconds >= maximum) {
        return std::numeric_limits<int64_t>::max();
    }
    if (milliseconds <= minimum) {
        return std::numeric_limits<int64_t>::min();
    }
    return static_cast<int64_t>(milliseconds);
}

int64_t saturating_add_wall_clock(int64_t wall_now, int64_t elapsed_ms) {
    const int128_t sum = static_cast<int128_t>(wall_now) + elapsed_ms;
    if (sum >= std::numeric_limits<int64_t>::max()) {
        return std::numeric_limits<int64_t>::max();
    }
    if (sum <= std::numeric_limits<int64_t>::min()) {
        return std::numeric_limits<int64_t>::min();
    }
    return static_cast<int64_t>(sum);
}

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

KvStore::KvStore(std::shared_ptr<ClockInterface> clock)
    : KvStore(std::move(clock), Limits{}) {}

KvStore::KvStore(std::shared_ptr<ClockInterface> clock, Limits limits)
    : clock_(std::move(clock)), max_versions_(limits.max_versions) {
    if (max_versions_ == 0) {
        throw std::invalid_argument("KvStore max_versions must be positive");
    }
}

KvStore::~KvStore() {
    // Lease holders normally release before the store, but destruction must
    // still join a live scanner rather than leaving a joinable std::thread.
    std::thread thread;
    {
        std::lock_guard<std::mutex> lifecycle_lock(scanner_lifecycle_mutex_);
        std::lock_guard<std::mutex> state_lock(scanner_mutex_);
        manual_scanner_owner_ = false;
        scanner_lease_count_ = 0;
        stop_scanner_locked(&thread);
    }
    if (thread.joinable()) {
        thread.join();
    }
}

std::chrono::steady_clock::time_point KvStore::now() const {
    return clock_ ? clock_->now() : Clock::now();
}

int64_t KvStore::system_now_ms() const {
    return clock_ ? clock_->system_now_ms() : Clock::system_now_ms();
}

bool KvStore::is_expired(const Version& version,
                         std::chrono::steady_clock::time_point current) const {
    return version.has_ttl && current >= version.deadline;
}

bool KvStore::is_live(const Version& version,
                      std::chrono::steady_clock::time_point current) const {
    return !version.deleted && !is_expired(version, current);
}

const KvStore::Version*
KvStore::visible_version_locked(const std::string& key,
                                std::uint64_t snapshot_lsn) const {
    const auto chain_it = versions_.find(key);
    if (chain_it == versions_.end() || chain_it->second.empty()) {
        return nullptr;
    }
    const VersionChain& chain = chain_it->second;
    auto it =
        std::upper_bound(chain.begin(), chain.end(), snapshot_lsn,
                         [](std::uint64_t snapshot, const Version& version) {
                             return snapshot < version.commit_lsn;
                         });
    if (it == chain.begin()) {
        return nullptr;
    }
    --it;
    return &*it;
}

const KvStore::Version*
KvStore::latest_version_locked(const std::string& key) const {
    const auto it = versions_.find(key);
    if (it == versions_.end() || it->second.empty()) {
        return nullptr;
    }
    return &it->second.back();
}

std::uint64_t KvStore::next_synthetic_lsn_locked() {
    if (max_applied_lsn_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("KvStore synthetic LSN exhausted");
    }
    return max_applied_lsn_ + 1;
}

bool KvStore::append_version_locked(const std::string& key, Version version,
                                    std::string* error) {
    if (version_count_ >= max_versions_) {
        set_error(error, "KvStore version limit exceeded");
        return false;
    }
    try {
        auto [it, inserted] = versions_.try_emplace(key);
        (void)inserted;
        it->second.push_back(std::move(version));
        ++version_count_;
        return true;
    } catch (const std::exception& exception) {
        // If this call created an empty chain, do not leave it behind.  A
        // caller applying multiple versions rolls back earlier appends.
        const auto it = versions_.find(key);
        if (it != versions_.end() && it->second.empty()) {
            versions_.erase(it);
        }
        set_error(error, exception.what());
        return false;
    }
}

bool KvStore::append_synthetic_version(const std::string& key,
                                       Version version) {
    const std::uint64_t new_lsn = next_synthetic_lsn_locked();
    version.commit_lsn = new_lsn;
    std::string error;
    if (!append_version_locked(key, std::move(version), &error)) {
        throw std::runtime_error(error);
    }
    max_applied_lsn_ = new_lsn;
    return true;
}

void KvStore::set(const std::string& key, const std::string& value) {
    Version version;
    version.value = value;
    std::lock_guard<std::mutex> lock(mutex_);
    (void)append_synthetic_version(key, std::move(version));
}

std::optional<std::string> KvStore::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = versions_.find(key);
    if (it == versions_.end() || it->second.empty()) {
        return std::nullopt;
    }
    const auto current = now();
    (void)retire_expired_chain_locked(it->second, current);
    const Version& version = it->second.back();
    if (!is_live(version, current)) {
        return std::nullopt;
    }
    return version.value;
}

std::optional<std::string> KvStore::get_at(const std::string& key,
                                           std::uint64_t snapshot_lsn) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Version* version = visible_version_locked(key, snapshot_lsn);
    if (version == nullptr || version->deleted || is_expired(*version, now())) {
        return std::nullopt;
    }
    return version->value;
}

bool KvStore::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = versions_.find(key);
    if (it == versions_.end() || it->second.empty()) {
        return false;
    }
    const auto current = now();
    (void)retire_expired_chain_locked(it->second, current);
    if (!is_live(it->second.back(), current)) {
        return false;
    }
    Version tombstone;
    tombstone.deleted = true;
    (void)append_synthetic_version(key, std::move(tombstone));
    return true;
}

bool KvStore::expire(const std::string& key, int64_t ttl_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = versions_.find(key);
    if (it == versions_.end() || it->second.empty()) {
        return false;
    }
    const auto current = now();
    (void)retire_expired_chain_locked(it->second, current);
    if (!is_live(it->second.back(), current)) {
        return false;
    }
    if (ttl_ms <= 0) {
        Version tombstone;
        tombstone.deleted = true;
        (void)append_synthetic_version(key, std::move(tombstone));
        return true;
    }
    Version version = it->second.back();
    version.has_ttl = true;
    version.deadline = saturating_add_milliseconds(current, ttl_ms);
    (void)append_synthetic_version(key, std::move(version));
    return true;
}

bool KvStore::expire_absolute(const std::string& key,
                              int64_t absolute_deadline_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = versions_.find(key);
    if (it == versions_.end() || it->second.empty()) {
        return false;
    }
    const auto current = now();
    (void)retire_expired_chain_locked(it->second, current);
    if (!is_live(it->second.back(), current)) {
        return false;
    }
    const int64_t wall_now = system_now_ms();
    if (absolute_deadline_ms <= wall_now) {
        Version tombstone;
        tombstone.deleted = true;
        (void)append_synthetic_version(key, std::move(tombstone));
        return true;
    }
    const int128_t difference =
        static_cast<int128_t>(absolute_deadline_ms) - wall_now;
    const int64_t remaining = difference >= std::numeric_limits<int64_t>::max()
                                  ? std::numeric_limits<int64_t>::max()
                                  : static_cast<int64_t>(difference);
    Version version = it->second.back();
    version.has_ttl = true;
    version.deadline = saturating_add_milliseconds(current, remaining);
    (void)append_synthetic_version(key, std::move(version));
    return true;
}

bool KvStore::set_with_expiry(const std::string& key, const std::string& value,
                              int64_t absolute_expiry_ms) {
    if (key.empty()) {
        return false;
    }
    Version version;
    version.value = value;
    if (absolute_expiry_ms > 0) {
        const auto current = now();
        const int64_t wall_now = system_now_ms();
        version.has_ttl = true;
        if (absolute_expiry_ms <= wall_now) {
            // Keep an expired version as the newest committed state.  It must
            // not reveal an older value if it is physically retained.
            version.deadline = current;
        } else {
            const int128_t difference =
                static_cast<int128_t>(absolute_expiry_ms) - wall_now;
            const int64_t remaining =
                difference >= std::numeric_limits<int64_t>::max()
                    ? std::numeric_limits<int64_t>::max()
                    : static_cast<int64_t>(difference);
            version.deadline = saturating_add_milliseconds(current, remaining);
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    (void)append_synthetic_version(key, std::move(version));
    return true;
}

KvStore::TtlResult KvStore::ttl_info(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = versions_.find(key);
    if (it == versions_.end() || it->second.empty()) {
        return {};
    }
    const auto current = now();
    (void)retire_expired_chain_locked(it->second, current);
    const Version& version = it->second.back();
    if (!is_live(version, current)) {
        return {};
    }
    if (!version.has_ttl) {
        return {TtlState::kNoExpiry, -1};
    }
    // Deadline comparison above is authoritative.  The conversion to whole
    // milliseconds rounds down, so a live key less than 1 ms before its
    // deadline legitimately reports zero remaining milliseconds.
    const int64_t remaining = std::max<int64_t>(
        0, saturating_milliseconds_between(version.deadline, current));
    return {TtlState::kExpiring, remaining};
}

KvStore::TtlResult KvStore::ttl_info_at(const std::string& key,
                                        std::uint64_t snapshot_lsn) {
    std::lock_guard<std::mutex> lock(mutex_);
    const Version* version = visible_version_locked(key, snapshot_lsn);
    if (version == nullptr) {
        return {};
    }
    const auto current = now();
    if (!is_live(*version, current)) {
        return {};
    }
    if (!version->has_ttl) {
        return {TtlState::kNoExpiry, -1};
    }
    const int64_t remaining = std::max<int64_t>(
        0, saturating_milliseconds_between(version->deadline, current));
    return {TtlState::kExpiring, remaining};
}

std::optional<int64_t> KvStore::ttl(const std::string& key) {
    const TtlResult result = ttl_info(key);
    switch (result.state) {
    case TtlState::kMissing:
        return std::nullopt;
    case TtlState::kNoExpiry:
        return int64_t{-1};
    case TtlState::kExpiring:
        return result.remaining_ms;
    }
    return std::nullopt;
}

size_t KvStore::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto current = now();
    (void)purge_expired_locked(current);
    size_t live = 0;
    for (const auto& [key, chain] : versions_) {
        (void)key;
        if (!chain.empty() && is_live(chain.back(), current)) {
            ++live;
        }
    }
    return live;
}

void KvStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    versions_.clear();
    version_count_ = 0;
    max_applied_lsn_ = 0;
}

// NOLINTNEXTLINE(performance-noexcept-swap): locking may throw.
void KvStore::swap(KvStore& other) {
    if (this == &other) {
        return;
    }
    std::scoped_lock lock(mutex_, other.mutex_);
    versions_.swap(other.versions_);
    std::swap(max_applied_lsn_, other.max_applied_lsn_);
    std::swap(version_count_, other.version_count_);
}

bool KvStore::retire_expired_chain_locked(
    VersionChain& chain, std::chrono::steady_clock::time_point current) const {
    if (chain.empty()) {
        return false;
    }
    // The sweep count reports logical keys that transition from expiring to
    // absent.  Reads deliberately do not mutate TTL metadata, so the first
    // sweep after expiry still observes has_ttl on the latest version.
    const bool latest_newly_expired =
        chain.back().has_ttl && is_expired(chain.back(), current);
    for (Version& version : chain) {
        if (is_expired(version, current)) {
            // Mark every expired historical version as a tombstone too.  A
            // retained old version must never become visible merely because
            // a later version was reclaimed.
            version.deleted = true;
            version.has_ttl = false;
            version.value.clear();
        }
    }
    return latest_newly_expired;
}

size_t KvStore::purge_expired_locked(
    std::chrono::steady_clock::time_point current) const {
    size_t removed = 0;
    for (auto& [key, chain] : versions_) {
        (void)key;
        if (retire_expired_chain_locked(chain, current)) {
            ++removed;
        }
    }
    return removed;
}

std::vector<KvStore::Entry> KvStore::export_all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto current = now();
    const int64_t wall_now = system_now_ms();
    std::vector<Entry> result;
    result.reserve(versions_.size());

    for (const auto& [key, chain] : versions_) {
        if (chain.empty()) {
            continue;
        }
        const Version& version = chain.back();
        if (!is_live(version, current)) {
            continue;
        }
        Entry exported;
        exported.key = key;
        exported.value = version.value;
        if (version.has_ttl) {
            // Millisecond conversion rounds down.  A live sub-millisecond
            // TTL must persist with at least 1 ms rather than being omitted
            // or encoded as 0 (the no-expiry sentinel).
            const int64_t remaining = std::max<int64_t>(
                0, saturating_milliseconds_between(version.deadline, current));
            // Preserve at least one serialized millisecond of liveness for a
            // sub-millisecond remainder.  Using the rounded-down zero here
            // would encode "expires at the previous wall-clock millisecond"
            // and discard the key on an immediate restart.
            const int64_t persisted_remaining = std::max<int64_t>(1, remaining);
            const int64_t absolute =
                saturating_add_wall_clock(wall_now, persisted_remaining);
            exported.expiry_ms = absolute > 0 ? absolute : 1;
        }
        result.push_back(std::move(exported));
    }
    return result;
}

bool KvStore::restore_entry(const std::string& key, const std::string& value,
                            int64_t absolute_expiry_ms) {
    if (key.empty()) {
        return false;
    }
    Version version;
    version.commit_lsn = 0;
    version.value = value;
    if (absolute_expiry_ms > 0) {
        const int64_t wall_now = system_now_ms();
        if (absolute_expiry_ms <= wall_now) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = versions_.find(key);
            if (it != versions_.end()) {
                version_count_ -= it->second.size();
                versions_.erase(it);
            }
            return true; // An already-expired record is valid but not live.
        }
        const int128_t difference =
            static_cast<int128_t>(absolute_expiry_ms) - wall_now;
        const int64_t remaining =
            difference >= std::numeric_limits<int64_t>::max()
                ? std::numeric_limits<int64_t>::max()
                : static_cast<int64_t>(difference);
        version.has_ttl = true;
        version.deadline = saturating_add_milliseconds(now(), remaining);
    }

    VersionChain replacement;
    replacement.push_back(std::move(version));
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = versions_.find(key);
    if (existing == versions_.end() && version_count_ >= max_versions_) {
        return false;
    }
    const std::size_t removed =
        existing == versions_.end() ? 0 : existing->second.size();
    try {
        versions_[key].swap(replacement);
    } catch (const std::exception&) {
        return false;
    }
    version_count_ = version_count_ - removed + 1;
    return true;
}

std::uint64_t KvStore::current_lsn() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_applied_lsn_;
}

std::uint64_t KvStore::latest_committed_lsn(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Version* version = latest_version_locked(key);
    return version == nullptr ? 0 : version->commit_lsn;
}

std::size_t KvStore::version_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_count_;
}

std::size_t KvStore::max_versions() const {
    return max_versions_;
}

bool KvStore::garbage_collect(const std::vector<std::uint64_t>& snapshot_lsns,
                              GcResult* result, std::string* error) {
    GcResult gc;
    if (result != nullptr) {
        *result = gc;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    gc.versions_before = version_count_;
    const auto current = now();

    try {
        for (auto it = versions_.begin(); it != versions_.end();) {
            VersionChain& chain = it->second;
            if (chain.empty()) {
                it = versions_.erase(it);
                continue;
            }

            const Version& latest = chain.back();
            bool has_older_snapshot = false;
            for (const std::uint64_t snapshot : snapshot_lsns) {
                if (snapshot < latest.commit_lsn) {
                    has_older_snapshot = true;
                    break;
                }
            }
            if (!has_older_snapshot && !is_live(latest, current)) {
                gc.versions_reclaimed += chain.size();
                version_count_ -= chain.size();
                it = versions_.erase(it);
                continue;
            }

            std::vector<bool> keep(chain.size(), false);
            keep.back() = true;
            for (const std::uint64_t snapshot : snapshot_lsns) {
                auto visible = std::upper_bound(
                    chain.begin(), chain.end(), snapshot,
                    [](std::uint64_t value, const Version& version) {
                        return value < version.commit_lsn;
                    });
                if (visible != chain.begin()) {
                    --visible;
                    keep[static_cast<std::size_t>(visible - chain.begin())] =
                        true;
                }
            }

            VersionChain replacement;
            replacement.reserve(chain.size());
            for (std::size_t index = 0; index < chain.size(); ++index) {
                if (keep[index]) {
                    replacement.push_back(chain[index]);
                }
            }
            if (replacement.size() == chain.size()) {
                ++it;
                continue;
            }
            gc.versions_reclaimed += chain.size() - replacement.size();
            version_count_ -= chain.size() - replacement.size();
            chain.swap(replacement);
            ++it;
        }
    } catch (const std::exception& exception) {
        set_error(error, exception.what());
        gc.versions_after = version_count_;
        if (result != nullptr) {
            *result = gc;
        }
        return false;
    }

    gc.versions_after = version_count_;
    if (result != nullptr) {
        *result = gc;
    }
    return true;
}

KvStore::ApplyStatus
KvStore::apply_committed_status(std::uint64_t commit_lsn,
                                const std::vector<AppliedMutation>& mutations,
                                std::string* error) {
    if (commit_lsn == 0) {
        set_error(error, "transaction commit LSN is zero");
        return ApplyStatus::kInvalidArgument;
    }
    if (mutations.empty()) {
        set_error(error, "transaction mutation set is empty");
        return ApplyStatus::kInvalidArgument;
    }
    for (const AppliedMutation& mutation : mutations) {
        if (mutation.key.empty() || mutation.key.size() > MAX_KEY_SIZE) {
            set_error(error, "transaction key length is out of range");
            return ApplyStatus::kInvalidArgument;
        }
        if (mutation.value.size() > MAX_VALUE_SIZE) {
            set_error(error, "transaction value length is out of range");
            return ApplyStatus::kInvalidArgument;
        }
        switch (mutation.kind) {
        case AppliedMutation::Kind::kSet:
            if (mutation.absolute_expiry_ms < 0) {
                set_error(error, "transaction SET has a negative expiry");
                return ApplyStatus::kInvalidArgument;
            }
            break;
        case AppliedMutation::Kind::kDelete:
            if (!mutation.value.empty() || mutation.absolute_expiry_ms != 0) {
                set_error(error, "transaction DEL has a payload");
                return ApplyStatus::kInvalidArgument;
            }
            break;
        case AppliedMutation::Kind::kExpire:
            if (!mutation.value.empty() || mutation.absolute_expiry_ms <= 0) {
                set_error(error, "transaction EXPIRE has invalid fields");
                return ApplyStatus::kInvalidArgument;
            }
            break;
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (commit_lsn <= max_applied_lsn_) {
        set_error(error, "transaction commit LSN is not increasing");
        return ApplyStatus::kInvalidArgument;
    }

    const auto current = now();
    const int64_t wall_now = system_now_ms();

    // Fold duplicate keys before reserving any metadata.  EXPIRE is converted
    // to a new SET of the then-current value so each final mutation contains
    // the complete state to publish.
    std::vector<AppliedMutation> folded;
    folded.reserve(mutations.size());
    std::unordered_map<std::string, std::size_t> positions;
    positions.reserve(mutations.size());
    for (const AppliedMutation& mutation : mutations) {
        const auto position = positions.find(mutation.key);
        if (position == positions.end()) {
            if (mutation.kind == AppliedMutation::Kind::kExpire) {
                const Version* latest = latest_version_locked(mutation.key);
                if (latest == nullptr || !is_live(*latest, current)) {
                    // EXPIRE on a missing/expired/tombstoned key is a no-op,
                    // matching the legacy command semantics.
                    continue;
                }
                AppliedMutation converted;
                converted.kind = AppliedMutation::Kind::kSet;
                converted.key = mutation.key;
                converted.value = latest->value;
                converted.absolute_expiry_ms = mutation.absolute_expiry_ms;
                positions.emplace(converted.key, folded.size());
                folded.push_back(std::move(converted));
                continue;
            }
            positions.emplace(mutation.key, folded.size());
            folded.push_back(mutation);
            continue;
        }

        AppliedMutation& prior = folded[position->second];
        switch (mutation.kind) {
        case AppliedMutation::Kind::kSet:
            prior.kind = AppliedMutation::Kind::kSet;
            prior.value = mutation.value;
            prior.absolute_expiry_ms = mutation.absolute_expiry_ms;
            break;
        case AppliedMutation::Kind::kDelete:
            prior.kind = AppliedMutation::Kind::kDelete;
            prior.value.clear();
            prior.absolute_expiry_ms = 0;
            break;
        case AppliedMutation::Kind::kExpire:
            if (prior.kind == AppliedMutation::Kind::kDelete) {
                break;
            }
            prior.kind = AppliedMutation::Kind::kSet;
            prior.absolute_expiry_ms = mutation.absolute_expiry_ms;
            break;
        }
    }

    struct PreparedVersion {
        const std::string* key = nullptr;
        Version version;
    };
    std::vector<PreparedVersion> prepared;
    prepared.reserve(folded.size());
    for (const AppliedMutation& mutation : folded) {
        Version version;
        if (mutation.kind == AppliedMutation::Kind::kDelete) {
            version.deleted = true;
        } else {
            version.value = mutation.value;
            if (mutation.absolute_expiry_ms > 0) {
                version.has_ttl = true;
                if (mutation.absolute_expiry_ms <= wall_now) {
                    version.deadline = current;
                } else {
                    const int128_t difference =
                        static_cast<int128_t>(mutation.absolute_expiry_ms) -
                        wall_now;
                    const int64_t remaining =
                        difference >= std::numeric_limits<int64_t>::max()
                            ? std::numeric_limits<int64_t>::max()
                            : static_cast<int64_t>(difference);
                    version.deadline =
                        saturating_add_milliseconds(current, remaining);
                }
            }
        }
        prepared.push_back({&mutation.key, std::move(version)});
    }

    if (version_count_ > max_versions_ ||
        prepared.size() > max_versions_ - version_count_) {
        set_error(error, "KvStore version limit exceeded");
        return ApplyStatus::kResourceLimit;
    }

    struct Touch {
        VersionChain* chain = nullptr;
        std::size_t old_size = 0;
    };
    std::vector<Touch> touches;
    touches.reserve(prepared.size());
    std::vector<const std::string*> inserted_keys;
    inserted_keys.reserve(prepared.size());
    std::size_t appended = 0;

    try {
        for (PreparedVersion& item : prepared) {
            Version version = std::move(item.version);
            version.commit_lsn = commit_lsn;
            auto [it, inserted] = versions_.try_emplace(*item.key);
            if (inserted) {
                inserted_keys.push_back(item.key);
            }
            touches.push_back({&it->second, it->second.size()});
            std::string append_error;
            if (!append_version_locked(*item.key, std::move(version),
                                       &append_error)) {
                for (auto touch = touches.rbegin(); touch != touches.rend();
                     ++touch) {
                    touch->chain->resize(touch->old_size);
                }
                for (const std::string* key : inserted_keys) {
                    versions_.erase(*key);
                }
                version_count_ -= appended;
                set_error(error, append_error.c_str());
                return ApplyStatus::kInvalidArgument;
            }
            ++appended;
        }
    } catch (const std::exception& exception) {
        for (auto touch = touches.rbegin(); touch != touches.rend(); ++touch) {
            touch->chain->resize(touch->old_size);
        }
        for (const std::string* key : inserted_keys) {
            versions_.erase(*key);
        }
        version_count_ -= appended;
        set_error(error, exception.what());
        return ApplyStatus::kInvalidArgument;
    }

    max_applied_lsn_ = commit_lsn;
    return ApplyStatus::kOk;
}

size_t KvStore::sweep_expired() {
    ScannerPause pause = pause_scanner();
    std::lock_guard<std::mutex> lock(mutex_);
    return purge_expired_locked(now());
}

KvStore::ScannerPause KvStore::pause_scanner() const {
    return ScannerPause(scanner_pause_mutex_);
}

KvStore::ScannerLease::~ScannerLease() {
    reset();
}

KvStore::ScannerLease::ScannerLease(ScannerLease&& other) noexcept
    : store_(std::exchange(other.store_, nullptr)) {}

KvStore::ScannerLease&
KvStore::ScannerLease::operator=(ScannerLease&& other) noexcept {
    if (this != &other) {
        reset();
        store_ = std::exchange(other.store_, nullptr);
    }
    return *this;
}

void KvStore::ScannerLease::reset() noexcept {
    KvStore* store = std::exchange(store_, nullptr);
    if (store != nullptr) {
        store->release_scanner();
    }
}

KvStore::ScannerLease
KvStore::acquire_scanner(std::chrono::milliseconds interval) {
    std::lock_guard<std::mutex> lifecycle_lock(scanner_lifecycle_mutex_);
    {
        std::lock_guard<std::mutex> state_lock(scanner_mutex_);
        ++scanner_lease_count_;
        try {
            ensure_scanner_locked(interval);
        } catch (...) {
            --scanner_lease_count_;
            throw;
        }
    }
    return ScannerLease(this);
}

void KvStore::release_scanner() noexcept {
    std::thread thread;
    {
        std::lock_guard<std::mutex> lifecycle_lock(scanner_lifecycle_mutex_);
        std::lock_guard<std::mutex> state_lock(scanner_mutex_);
        if (scanner_lease_count_ > 0) {
            --scanner_lease_count_;
        }
        if (scanner_lease_count_ == 0 && !manual_scanner_owner_) {
            stop_scanner_locked(&thread);
        }
    }
    if (thread.joinable()) {
        thread.join();
    }
}

void KvStore::ensure_scanner_locked(std::chrono::milliseconds interval) {
    if (interval.count() <= 0) {
        interval = std::chrono::milliseconds(1);
    }
    if (scanner_thread_.joinable()) {
        return;
    }
    scanner_stop_ = false;
    scanner_interval_ = interval;
    scanner_running_ = true;
    try {
        scanner_thread_ = std::thread([this]() { scanner_loop(); });
    } catch (...) {
        scanner_running_ = false;
        throw;
    }
}

void KvStore::stop_scanner_locked(std::thread* thread) {
    if (!scanner_thread_.joinable()) {
        scanner_running_ = false;
        scanner_stop_ = false;
        return;
    }
    scanner_stop_ = true;
    scanner_running_ = false;
    scanner_cv_.notify_all();
    *thread = std::move(scanner_thread_);
}

void KvStore::start_scanner(std::chrono::milliseconds interval) {
    std::lock_guard<std::mutex> lifecycle_lock(scanner_lifecycle_mutex_);
    std::lock_guard<std::mutex> state_lock(scanner_mutex_);
    const bool was_manual_owner = manual_scanner_owner_;
    manual_scanner_owner_ = true;
    try {
        ensure_scanner_locked(interval);
    } catch (...) {
        manual_scanner_owner_ = was_manual_owner;
        throw;
    }
}

void KvStore::stop_scanner() {
    std::thread thread;
    {
        std::lock_guard<std::mutex> lifecycle_lock(scanner_lifecycle_mutex_);
        std::lock_guard<std::mutex> state_lock(scanner_mutex_);
        manual_scanner_owner_ = false;
        if (scanner_lease_count_ == 0) {
            stop_scanner_locked(&thread);
        }
    }
    if (thread.joinable()) {
        thread.join();
    }
}

bool KvStore::scanner_running() const {
    std::lock_guard<std::mutex> lifecycle_lock(scanner_lifecycle_mutex_);
    std::lock_guard<std::mutex> state_lock(scanner_mutex_);
    return scanner_running_;
}

void KvStore::scanner_loop() {
    std::unique_lock<std::mutex> lock(scanner_mutex_);
    while (!scanner_stop_) {
        scanner_cv_.wait_for(lock, scanner_interval_,
                             [this]() { return scanner_stop_; });
        if (scanner_stop_) {
            break;
        }
        // Drop the scanner lock while taking the store lock.  The fixed lock
        // order is scanner_mutex_ -> mutex_ only when rechecking, and no store
        // operation ever takes scanner_mutex_ while holding mutex_.
        lock.unlock();
        sweep_expired();
        lock.lock();
    }
}

} // namespace minidb
