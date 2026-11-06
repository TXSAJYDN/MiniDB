#pragma once
#include "../base/Clock.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace minidb {

// A thread-safe key/value store with lazy and periodic TTL expiry.
//
// Time semantics:
//   * deadline_ is maintained on the monotonic clock, so wall-clock jumps
//     cannot make a key expire early or late while the process is running.
//   * Entry::expiry_ms is an absolute Unix epoch millisecond timestamp. It is
//     used only at the RDB boundary; never persist a relative TTL as a
//     deadline.
class KvStore {
  public:
    explicit KvStore(std::shared_ptr<ClockInterface> clock = nullptr);
    ~KvStore();

    KvStore(const KvStore&) = delete;
    KvStore& operator=(const KvStore&) = delete;

    // SET replaces the value and clears any existing TTL.
    void set(const std::string& key, const std::string& value);

    // Returns no value for a missing, expired, or already-reaped key.
    std::optional<std::string> get(const std::string& key);

    // Returns true only when a live key was removed.
    bool del(const std::string& key);

    // EXPIRE with a non-positive TTL removes a live key.  Returns true when
    // the key existed and the operation was applied.
    bool expire(const std::string& key, int64_t ttl_ms);

    enum class TtlState : std::uint8_t { kMissing, kNoExpiry, kExpiring };

    struct TtlResult {
        TtlState state = TtlState::kMissing;
        int64_t remaining_ms = 0;
    };

    // Distinguishes a missing/expired key from an existing key without TTL.
    TtlResult ttl_info(const std::string& key);

    // Compatibility helper: Missing/expired: nullopt.  Existing without TTL:
    // -1.  Otherwise the remaining monotonic duration rounded down to
    // milliseconds.
    std::optional<int64_t> ttl(const std::string& key);

    // Number of live (non-expired) entries.  Expired entries are reaped while
    // computing the result.
    size_t size() const;

    void clear();

    // Atomic publication of a fully validated recovery image.  The source
    // store must use the same clock policy as the target.
    // NOLINTNEXTLINE(performance-noexcept-swap): locking may throw.
    void swap(KvStore& other);

    struct Entry {
        std::string key;
        std::string value;
        // Absolute Unix epoch milliseconds. 0 means no expiry.
        int64_t expiry_ms = 0;
    };

    // Export only live entries.  TTLs are converted to absolute wall-clock
    // timestamps under the same lock used by all mutations.
    std::vector<Entry> export_all() const;

    // Restore a single entry using an absolute Unix epoch millisecond
    // timestamp.  Expired entries are discarded.  This is deliberately
    // separate from set()+expire() so recovery does not extend a TTL.
    bool restore_entry(const std::string& key, const std::string& value,
                       int64_t absolute_expiry_ms);

    std::shared_ptr<ClockInterface> clock() const {
        return clock_;
    }

    // Optional periodic reaper.  The scanner shares mutex_ with all commands.
    void
    start_scanner(std::chrono::milliseconds interval = std::chrono::seconds(1));
    void stop_scanner();
    bool scanner_running() const;
    size_t sweep_expired();

  private:
    struct InternalEntry {
        std::string value;
        std::chrono::steady_clock::time_point deadline;
        bool has_ttl = false;
    };

    using InternalMap = std::unordered_map<std::string, InternalEntry>;

    std::chrono::steady_clock::time_point now() const;
    int64_t system_now_ms() const;
    bool is_expired(const InternalEntry& entry,
                    std::chrono::steady_clock::time_point current) const;
    size_t
    purge_expired_locked(std::chrono::steady_clock::time_point current) const;
    void scanner_loop();

    std::shared_ptr<ClockInterface> clock_;
    mutable std::mutex mutex_;
    mutable InternalMap store_;

    mutable std::mutex scanner_mutex_;
    std::condition_variable scanner_cv_;
    std::thread scanner_thread_;
    bool scanner_stop_ = false;
    bool scanner_running_ = false;
    std::chrono::milliseconds scanner_interval_{1000};
};

} // namespace minidb
