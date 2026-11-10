#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace minidb {

// WAL record wire format.  All integers are big-endian:
//
//   "MWAL" | version:u16 | flags:u16 | lsn:u64 | op:u8 | reserved:u8
//          | key_len:u32 | value_len:u32 | expiry_ms:i64 | key | value
//          | crc32c:u32
//
// The CRC covers every byte before the trailing checksum.  The format is
// intentionally fixed-width so a torn record can be detected before the key
// or value is materialised.
enum class WalOp : std::uint8_t {
    kSet = 1,
    kDel = 2,
    kExpire = 3,
};

struct WalRecord {
    std::uint64_t lsn = 0;
    WalOp op = WalOp::kSet;
    std::string key;
    std::string value;
    // Absolute Unix epoch milliseconds.  0 means no expiry.  DEL carries 0;
    // EXPIRE carries a positive absolute deadline.
    std::int64_t expiry_ms = 0;
};

// One explicit resource policy shared by append, rotation, directory
// selection, replay and checkpoint retry.  Tests override these values; the
// defaults are the production bounds documented in docs/wal-format.md.
struct WalLimits {
    std::size_t max_segment_bytes = std::size_t{16} * 1024 * 1024;
    std::size_t max_total_bytes = std::size_t{256} * 1024 * 1024;
    std::size_t max_segments = 128;
    std::uint64_t max_recovery_records = 1'000'000;
    std::size_t max_recovery_bytes = std::size_t{512} * 1024 * 1024;
};

bool validate_wal_limits(const WalLimits& limits, std::string* error = nullptr);
const WalLimits& default_wal_limits() noexcept;

class WalFormat {
  public:
    static constexpr char MAGIC[5] = "MWAL";
    static constexpr std::uint16_t VERSION = 1;
    static constexpr std::uint16_t FLAGS = 0;
    static constexpr std::size_t HEADER_SIZE = 34;
    static constexpr std::size_t CHECKSUM_SIZE = 4;
    static constexpr std::size_t MAX_KEY_SIZE = 1024;
    static constexpr std::size_t MAX_VALUE_SIZE = std::size_t{1024} * 1024;
    static constexpr std::size_t MAX_RECORD_SIZE =
        HEADER_SIZE + MAX_KEY_SIZE + MAX_VALUE_SIZE + CHECKSUM_SIZE;
    // Production defaults.  Persistence code carries an injected WalLimits
    // value so tests can exercise resource boundaries without creating
    // multi-hundred-megabyte fixtures.
    static constexpr std::size_t MAX_SEGMENT_BYTES =
        std::size_t{16} * 1024 * 1024;
    static constexpr std::size_t MAX_TOTAL_BYTES =
        std::size_t{256} * 1024 * 1024;
    static constexpr std::size_t MAX_SEGMENTS = 128;
    static constexpr std::uint64_t MAX_RECOVERY_RECORDS = 1000000;
    static constexpr std::size_t MAX_RECOVERY_BYTES =
        std::size_t{512} * 1024 * 1024;

    enum class DecodeStatus : std::uint8_t {
        kOk,
        kNeedMore,
        kCorrupt,
    };

    static std::size_t encoded_size(const WalRecord& record);
    static bool validate(const WalRecord& record, std::string* error = nullptr);

    // Returns an empty vector when the record is invalid.  The caller must
    // keep the record size bounded before calling this function.
    static std::vector<std::uint8_t> encode(const WalRecord& record,
                                            std::string* error = nullptr);

    // Decodes one record from data.  NeedMore means the supplied bytes are a
    // truncated prefix and can be treated as a tail only by the recovery
    // layer.  Corrupt means the header or a complete record is invalid.
    static DecodeStatus decode(const std::uint8_t* data, std::size_t len,
                               WalRecord* record,
                               std::size_t* consumed = nullptr,
                               std::string* error = nullptr);

    static std::uint32_t crc32c(const std::uint8_t* data, std::size_t len);
};

} // namespace minidb
