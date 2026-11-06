#pragma once

#include "../src/storage/RdbFormat.h"
#include "../src/base/Clock.h"
#include "../src/base/Int128.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace minidb_test {

inline std::string make_unique_temp_dir(const std::string& prefix) {
    const std::filesystem::path base =
        std::filesystem::temp_directory_path() / prefix;
    std::string tmpl = base.string() + "XXXXXX";
    std::vector<char> buffer(tmpl.begin(), tmpl.end());
    buffer.push_back('\0');
    char* created = ::mkdtemp(buffer.data());
    if (created == nullptr) {
        throw std::runtime_error("mkdtemp() failed for " + tmpl);
    }
    return created;
}

inline void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    const uint32_t network = minidb::RdbFormat::hton32(value);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&network);
    out.insert(out.end(), bytes, bytes + sizeof(network));
}

inline void append_u64(std::vector<uint8_t>& out, uint64_t value) {
    const uint64_t network = minidb::RdbFormat::hton64(value);
    const auto* bytes = reinterpret_cast<const uint8_t*>(&network);
    out.insert(out.end(), bytes, bytes + sizeof(network));
}

inline void append_bytes(std::vector<uint8_t>& out, const void* data,
                         size_t size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + size);
}

inline std::vector<uint8_t> make_header(uint32_t record_count) {
    std::vector<uint8_t> out;
    append_bytes(out, minidb::RdbFormat::MAGIC, 4);
    append_u32(out, minidb::RdbFormat::VERSION);
    append_u32(out, record_count);
    return out;
}

inline void append_length_prefixed(std::vector<uint8_t>& out,
                                   const std::string& value) {
    const auto encoded =
        minidb::RdbFormat::encode_varint(static_cast<uint32_t>(value.size()));
    out.insert(out.end(), encoded.begin(), encoded.end());
    append_bytes(out, value.data(), value.size());
}

inline void append_record(std::vector<uint8_t>& out, const std::string& key,
                          const std::string& value,
                          int64_t absolute_expiry_ms = 0) {
    append_length_prefixed(out, key);
    append_length_prefixed(out, value);
    append_u64(out, static_cast<uint64_t>(absolute_expiry_ms));
}

inline void append_checksum(std::vector<uint8_t>& out) {
    append_u32(out, minidb::RdbFormat::crc32c(out.data(), out.size()));
}

inline bool write_file(const std::string& path,
                       const std::vector<uint8_t>& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(content.data()),
                 static_cast<std::streamsize>(content.size()));
    output.close();
    return output.good();
}

class FrozenClock final : public minidb::ClockInterface {
  public:
    FrozenClock(int64_t monotonic_ms, int64_t wall_ms)
        : monotonic_ms_(monotonic_ms), wall_ms_(wall_ms) {}

    std::chrono::steady_clock::time_point now() const override {
        return minidb::Clock::from_ms(
            monotonic_ms_.load(std::memory_order_relaxed));
    }

    int64_t system_now_ms() const override {
        return wall_ms_.load(std::memory_order_relaxed);
    }

    // Convenience for tests where both clocks should move together.
    void advance(int64_t milliseconds) {
        advance_monotonic(milliseconds);
        advance_wall(milliseconds);
    }

    // Independent setters make wall-clock jump behavior testable without
    // perturbing the monotonic deadline clock (and vice versa).
    void set_monotonic_ms(int64_t milliseconds) {
        monotonic_ms_.store(milliseconds, std::memory_order_relaxed);
    }

    void set_wall_ms(int64_t milliseconds) {
        wall_ms_.store(milliseconds, std::memory_order_relaxed);
    }

    void advance_monotonic(int64_t milliseconds) {
        int64_t current = monotonic_ms_.load(std::memory_order_relaxed);
        while (!monotonic_ms_.compare_exchange_weak(
            current, saturating_add(current, milliseconds),
            std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }

    void advance_wall(int64_t milliseconds) {
        int64_t current = wall_ms_.load(std::memory_order_relaxed);
        while (!wall_ms_.compare_exchange_weak(
            current, saturating_add(current, milliseconds),
            std::memory_order_relaxed, std::memory_order_relaxed)) {
        }
    }

  private:
    static int64_t saturating_add(int64_t value, int64_t delta) {
        const minidb::int128_t result = static_cast<minidb::int128_t>(value) +
                                        static_cast<minidb::int128_t>(delta);
        if (result > std::numeric_limits<int64_t>::max()) {
            return std::numeric_limits<int64_t>::max();
        }
        if (result < std::numeric_limits<int64_t>::min()) {
            return std::numeric_limits<int64_t>::min();
        }
        return static_cast<int64_t>(result);
    }

    std::atomic<int64_t> monotonic_ms_;
    std::atomic<int64_t> wall_ms_;
};

} // namespace minidb_test
