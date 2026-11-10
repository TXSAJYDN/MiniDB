#include "KvStore.h"
#include "../base/Int128.h"

#include <algorithm>
#include <limits>
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

} // namespace

KvStore::KvStore(std::shared_ptr<ClockInterface> clock)
    : clock_(std::move(clock)) {}

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

void KvStore::set(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    InternalEntry entry;
    entry.value = value;
    entry.has_ttl = false;
    store_[key] = std::move(entry);
}

std::optional<std::string> KvStore::get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_.find(key);
    if (it == store_.end()) {
        return std::nullopt;
    }
    if (is_expired(it->second, now())) {
        store_.erase(it);
        return std::nullopt;
    }
    return it->second.value;
}

bool KvStore::del(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_.find(key);
    if (it == store_.end()) {
        return false;
    }
    if (is_expired(it->second, now())) {
        store_.erase(it);
        return false;
    }
    store_.erase(it);
    return true;
}

bool KvStore::expire(const std::string& key, int64_t ttl_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_.find(key);
    if (it == store_.end()) {
        return false;
    }
    const auto current = now();
    if (is_expired(it->second, current)) {
        store_.erase(it);
        return false;
    }
    if (ttl_ms <= 0) {
        store_.erase(it);
        return true;
    }
    it->second.has_ttl = true;
    it->second.deadline = saturating_add_milliseconds(current, ttl_ms);
    return true;
}

bool KvStore::expire_absolute(const std::string& key,
                              int64_t absolute_deadline_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_.find(key);
    if (it == store_.end()) {
        return false;
    }
    const auto current = now();
    if (is_expired(it->second, current)) {
        store_.erase(it);
        return false;
    }
    const int64_t wall_now = system_now_ms();
    if (absolute_deadline_ms <= wall_now) {
        store_.erase(it);
        return true;
    }
    const int128_t difference =
        static_cast<int128_t>(absolute_deadline_ms) - wall_now;
    const int64_t remaining = difference >= std::numeric_limits<int64_t>::max()
                                  ? std::numeric_limits<int64_t>::max()
                                  : static_cast<int64_t>(difference);
    it->second.has_ttl = true;
    it->second.deadline = saturating_add_milliseconds(current, remaining);
    return true;
}

bool KvStore::set_with_expiry(const std::string& key, const std::string& value,
                              int64_t absolute_expiry_ms) {
    if (key.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (absolute_expiry_ms > 0) {
        const int64_t wall_now = system_now_ms();
        if (absolute_expiry_ms <= wall_now) {
            store_.erase(key);
            return true;
        }
    }
    InternalEntry entry;
    entry.value = value;
    if (absolute_expiry_ms > 0) {
        const int64_t wall_now = system_now_ms();
        const int128_t difference =
            static_cast<int128_t>(absolute_expiry_ms) - wall_now;
        const int64_t remaining =
            difference >= std::numeric_limits<int64_t>::max()
                ? std::numeric_limits<int64_t>::max()
                : static_cast<int64_t>(difference);
        entry.has_ttl = true;
        entry.deadline = saturating_add_milliseconds(now(), remaining);
    }
    store_[key] = std::move(entry);
    return true;
}

KvStore::TtlResult KvStore::ttl_info(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = store_.find(key);
    if (it == store_.end()) {
        return {};
    }
    const auto current = now();
    if (is_expired(it->second, current)) {
        store_.erase(it);
        return {};
    }
    if (!it->second.has_ttl) {
        return {TtlState::kNoExpiry, -1};
    }
    // Deadline comparison above is authoritative.  The conversion to
    // whole milliseconds rounds down, so a live key less than 1 ms before
    // its deadline legitimately reports zero remaining milliseconds.
    const int64_t remaining = std::max<int64_t>(
        0, saturating_milliseconds_between(it->second.deadline, current));
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
    purge_expired_locked(now());
    return store_.size();
}

void KvStore::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    store_.clear();
}

// NOLINTNEXTLINE(performance-noexcept-swap): locking may throw.
void KvStore::swap(KvStore& other) {
    if (this == &other) {
        return;
    }
    std::scoped_lock lock(mutex_, other.mutex_);
    store_.swap(other.store_);
}

bool KvStore::is_expired(const InternalEntry& entry,
                         std::chrono::steady_clock::time_point current) const {
    return entry.has_ttl && current >= entry.deadline;
}

size_t KvStore::purge_expired_locked(
    std::chrono::steady_clock::time_point current) const {
    size_t removed = 0;
    for (auto it = store_.begin(); it != store_.end();) {
        if (is_expired(it->second, current)) {
            it = store_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::vector<KvStore::Entry> KvStore::export_all() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto current = now();
    const int64_t wall_now = system_now_ms();
    std::vector<Entry> result;
    result.reserve(store_.size());

    for (const auto& [key, entry] : store_) {
        if (is_expired(entry, current)) {
            continue;
        }
        Entry exported;
        exported.key = key;
        exported.value = entry.value;
        if (entry.has_ttl) {
            // Millisecond conversion rounds down.  A live sub-millisecond
            // TTL must persist with at least 1 ms rather than being omitted
            // or encoded as 0 (the no-expiry sentinel).
            const int64_t remaining = std::max<int64_t>(
                0, saturating_milliseconds_between(entry.deadline, current));
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
    std::lock_guard<std::mutex> lock(mutex_);
    InternalEntry entry;
    entry.value = value;
    if (absolute_expiry_ms > 0) {
        const int64_t wall_now = system_now_ms();
        if (absolute_expiry_ms <= wall_now) {
            store_.erase(key);
            return true; // An already-expired record is valid but not live.
        }
        const int128_t difference =
            static_cast<int128_t>(absolute_expiry_ms) - wall_now;
        const int64_t remaining =
            difference >= std::numeric_limits<int64_t>::max()
                ? std::numeric_limits<int64_t>::max()
                : static_cast<int64_t>(difference);
        entry.has_ttl = true;
        entry.deadline = saturating_add_milliseconds(now(), remaining);
    }
    store_[key] = std::move(entry);
    return true;
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
