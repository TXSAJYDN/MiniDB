#include "WalFormat.h"

#include "RdbFormat.h"

#include <cstring>
#include <limits>

namespace minidb {

bool validate_wal_limits(const WalLimits& limits, std::string* error) {
    auto fail = [error](const char* message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };
    if (limits.max_segment_bytes == 0) {
        return fail("WAL segment limit must be positive");
    }
    if (limits.max_total_bytes == 0) {
        return fail("WAL total-byte limit must be positive");
    }
    if (limits.max_segments < 2) {
        return fail("WAL segment count limit must be at least 2");
    }
    if (limits.max_recovery_records == 0) {
        return fail("WAL recovery-record limit must be positive");
    }
    if (limits.max_recovery_bytes == 0) {
        return fail("WAL recovery-byte limit must be positive");
    }
    return true;
}

const WalLimits& default_wal_limits() noexcept {
    static const WalLimits limits;
    return limits;
}
namespace {

std::uint16_t load_u16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8) |
        static_cast<std::uint16_t>(data[1]));
}

std::uint32_t load_u32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

std::uint64_t load_u64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | data[i];
    }
    return value;
}

void store_u16(std::uint8_t* data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>(value >> 8);
    data[1] = static_cast<std::uint8_t>(value);
}

void store_u32(std::uint8_t* data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>(value >> 24);
    data[1] = static_cast<std::uint8_t>(value >> 16);
    data[2] = static_cast<std::uint8_t>(value >> 8);
    data[3] = static_cast<std::uint8_t>(value);
}

void store_u64(std::uint8_t* data, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        data[i] = static_cast<std::uint8_t>(value);
        value >>= 8;
    }
}

void set_error(std::string* error, const char* message) {
    if (error != nullptr) {
        *error = message;
    }
}

} // namespace

std::uint32_t WalFormat::crc32c(const std::uint8_t* data, std::size_t len) {
    return RdbFormat::crc32c(data, len);
}

std::size_t WalFormat::encoded_size(const WalRecord& record) {
    return HEADER_SIZE + record.key.size() + record.value.size() +
           CHECKSUM_SIZE;
}

bool WalFormat::validate(const WalRecord& record, std::string* error) {
    if (record.lsn == 0) {
        set_error(error, "WAL LSN must be positive");
        return false;
    }
    if (record.key.empty() || record.key.size() > MAX_KEY_SIZE) {
        set_error(error, "WAL key length is out of range");
        return false;
    }
    if (record.value.size() > MAX_VALUE_SIZE) {
        set_error(error, "WAL value length is out of range");
        return false;
    }
    if (record.expiry_ms < 0) {
        set_error(error, "WAL expiry is negative");
        return false;
    }
    switch (record.op) {
    case WalOp::kSet:
        break;
    case WalOp::kDel:
        if (!record.value.empty() || record.expiry_ms != 0) {
            set_error(error, "WAL DEL has value or expiry");
            return false;
        }
        break;
    case WalOp::kExpire:
        if (!record.value.empty() || record.expiry_ms <= 0) {
            set_error(error, "WAL EXPIRE has invalid payload");
            return false;
        }
        break;
    default:
        set_error(error, "WAL operation is unknown");
        return false;
    }
    if (record.key.size() > std::numeric_limits<std::uint32_t>::max() ||
        record.value.size() > std::numeric_limits<std::uint32_t>::max()) {
        set_error(error, "WAL payload is too large");
        return false;
    }
    const std::size_t size = encoded_size(record);
    if (size > MAX_RECORD_SIZE) {
        set_error(error, "WAL record exceeds maximum size");
        return false;
    }
    return true;
}

std::vector<std::uint8_t> WalFormat::encode(const WalRecord& record,
                                            std::string* error) {
    if (!validate(record, error)) {
        return {};
    }
    const std::size_t size = encoded_size(record);
    std::vector<std::uint8_t> result(size);
    std::memcpy(result.data(), MAGIC, 4);
    store_u16(result.data() + 4, VERSION);
    store_u16(result.data() + 6, FLAGS);
    store_u64(result.data() + 8, record.lsn);
    result[16] = static_cast<std::uint8_t>(record.op);
    result[17] = 0;
    store_u32(result.data() + 18,
              static_cast<std::uint32_t>(record.key.size()));
    store_u32(result.data() + 22,
              static_cast<std::uint32_t>(record.value.size()));
    store_u64(result.data() + 26, static_cast<std::uint64_t>(record.expiry_ms));
    std::memcpy(result.data() + HEADER_SIZE, record.key.data(),
                record.key.size());
    std::memcpy(result.data() + HEADER_SIZE + record.key.size(),
                record.value.data(), record.value.size());
    const std::size_t crc_offset = size - CHECKSUM_SIZE;
    const std::uint32_t checksum = crc32c(result.data(), crc_offset);
    store_u32(result.data() + crc_offset, checksum);
    return result;
}

WalFormat::DecodeStatus WalFormat::decode(const std::uint8_t* data,
                                          std::size_t len, WalRecord* record,
                                          std::size_t* consumed,
                                          std::string* error) {
    if (consumed != nullptr) {
        *consumed = 0;
    }
    if (data == nullptr || record == nullptr) {
        set_error(error, "WAL decode received null input");
        return DecodeStatus::kCorrupt;
    }
    if (len < 4) {
        return DecodeStatus::kNeedMore;
    }
    if (std::memcmp(data, MAGIC, 4) != 0) {
        set_error(error, "WAL magic mismatch");
        return DecodeStatus::kCorrupt;
    }
    if (len < HEADER_SIZE) {
        return DecodeStatus::kNeedMore;
    }
    if (load_u16(data + 4) != VERSION) {
        set_error(error, "WAL version mismatch");
        return DecodeStatus::kCorrupt;
    }
    if (load_u16(data + 6) != FLAGS) {
        set_error(error, "WAL flags are not supported");
        return DecodeStatus::kCorrupt;
    }
    const std::uint8_t raw_op = data[16];
    if (raw_op < static_cast<std::uint8_t>(WalOp::kSet) ||
        raw_op > static_cast<std::uint8_t>(WalOp::kExpire)) {
        set_error(error, "WAL operation is unknown");
        return DecodeStatus::kCorrupt;
    }
    if (data[17] != 0) {
        set_error(error, "WAL reserved byte is non-zero");
        return DecodeStatus::kCorrupt;
    }
    const std::uint64_t lsn = load_u64(data + 8);
    if (lsn == 0) {
        set_error(error, "WAL LSN is zero");
        return DecodeStatus::kCorrupt;
    }
    const std::uint32_t key_len = load_u32(data + 18);
    const std::uint32_t value_len = load_u32(data + 22);
    if (key_len == 0 || key_len > MAX_KEY_SIZE) {
        set_error(error, "WAL key length is out of range");
        return DecodeStatus::kCorrupt;
    }
    if (value_len > MAX_VALUE_SIZE) {
        set_error(error, "WAL value length is out of range");
        return DecodeStatus::kCorrupt;
    }
    const WalOp op = static_cast<WalOp>(raw_op);
    if ((op == WalOp::kDel || op == WalOp::kExpire) && value_len != 0) {
        set_error(error, "WAL operation has a value payload");
        return DecodeStatus::kCorrupt;
    }
    const std::uint64_t expiry_bits = load_u64(data + 26);
    const std::int64_t expiry_ms = static_cast<std::int64_t>(expiry_bits);
    if (expiry_ms < 0) {
        set_error(error, "WAL expiry is negative");
        return DecodeStatus::kCorrupt;
    }
    if (op == WalOp::kDel && expiry_ms != 0) {
        set_error(error, "WAL DEL has an expiry");
        return DecodeStatus::kCorrupt;
    }
    if (op == WalOp::kExpire && expiry_ms <= 0) {
        set_error(error, "WAL EXPIRE has no deadline");
        return DecodeStatus::kCorrupt;
    }

    const std::uint64_t total_u64 = static_cast<std::uint64_t>(HEADER_SIZE) +
                                    key_len + value_len + CHECKSUM_SIZE;
    if (total_u64 > MAX_RECORD_SIZE ||
        total_u64 > std::numeric_limits<std::size_t>::max()) {
        set_error(error, "WAL record size overflows");
        return DecodeStatus::kCorrupt;
    }
    const std::size_t total = static_cast<std::size_t>(total_u64);
    if (len < total) {
        return DecodeStatus::kNeedMore;
    }
    const std::uint32_t stored_crc = load_u32(data + total - CHECKSUM_SIZE);
    const std::uint32_t computed_crc = crc32c(data, total - CHECKSUM_SIZE);
    if (stored_crc != computed_crc) {
        set_error(error, "WAL checksum mismatch");
        return DecodeStatus::kCorrupt;
    }

    WalRecord decoded;
    decoded.lsn = lsn;
    decoded.op = op;
    decoded.key.assign(reinterpret_cast<const char*>(data + HEADER_SIZE),
                       key_len);
    decoded.value.assign(
        reinterpret_cast<const char*>(data + HEADER_SIZE + key_len), value_len);
    decoded.expiry_ms = expiry_ms;
    if (!validate(decoded, error)) {
        return DecodeStatus::kCorrupt;
    }
    *record = std::move(decoded);
    if (consumed != nullptr) {
        *consumed = total;
    }
    return DecodeStatus::kOk;
}

} // namespace minidb
