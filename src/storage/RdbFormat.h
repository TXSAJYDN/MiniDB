#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace minidb {

// RDB v2 wire format (all integers are big-endian):
//   "MRDB" | version:u32 | record_count:u32 | records | crc32c:u32
// record := key_len:varint | key | value_len:varint | value | expiry_ms:i64
class RdbFormat {
  public:
    static constexpr char MAGIC[5] = "MRDB";
    static constexpr uint32_t VERSION = 2;
    static constexpr size_t HEADER_SIZE = 12;
    static constexpr size_t CHECKSUM_SIZE = 4;
    static constexpr size_t MIN_FILE_SIZE = HEADER_SIZE + CHECKSUM_SIZE;

    static constexpr size_t MAX_FILE_SIZE = size_t{64} * 1024 * 1024;
    static constexpr uint32_t MAX_RECORDS = 100000;
    static constexpr size_t MAX_KEY_SIZE = 1024;
    static constexpr size_t MAX_VALUE_SIZE = size_t{1024} * 1024;
    static constexpr size_t MAX_RECOVERY_BYTES = size_t{256} * 1024 * 1024;

    static std::vector<uint8_t> encode_varint(uint32_t value);
    static std::pair<uint32_t, size_t> decode_varint(const uint8_t* data,
                                                     size_t max_len);
    static uint32_t crc32c(const uint8_t* data, size_t len);

    static uint32_t hton32(uint32_t value);
    static uint32_t ntoh32(uint32_t value);
    static uint64_t hton64(uint64_t value);
    static uint64_t ntoh64(uint64_t value);
};

} // namespace minidb
